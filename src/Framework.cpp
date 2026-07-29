#include "Framework.hpp"

#include <array>
#include <unordered_map>
#include <atomic>
#include <cmath>
#include <limits>
#include <cinttypes>
#include <cstdio>
#include <iterator>
#include <thread>
#include <vector>

#include <windows.h>
#include <tlhelp32.h>

#include <utility/Module.hpp>

#include "Hooks.hpp"
#include "Log.hpp"
#include "Mods.hpp"
#include "sdk/CClientMgr.hpp"
#include "sdk/CClientShell.hpp"
#include "sdk/DatabaseMgr.hpp"
#include "sdk/Modules.hpp"
#include "sdk/Model.hpp"
#include "sdk/Object.hpp"
#include "sdk/Engine.hpp"
#include "sdk/EngineVars.hpp"
#include "sdk/Input.hpp"
#include "sdk/Render.hpp"
#include "sdk/SceneCamera.hpp"
#include "sdk/ShaderParams.hpp"
#include "sdk/VisTree.hpp"
#include "sdk/interfaces/All.hpp"
#include "sdk/interfaces/ILTModel.hpp"

std::unique_ptr<Framework> g_framework;

namespace {

// --- frame hook (the framework's first real hook) ---------------------------
// Detour on CClientShell::Update (sdk-mapped anchor). Counts ticks and fans
// out Mods::on_frame. x86 __thiscall (ecx=this) -> __fastcall shim (edx dummy).

// One-shot engine-thread object walk.
//
// sdk::CClientMgr::for_each_object hands the callback a live LTObject*, which
// is only sound where nothing is mutating those lists -- the engine thread,
// inside the engine's own update. Mods driven from on_frame() below are the
// normal consumers. An off-thread caller (IPC/diagnostics) cannot satisfy
// that precondition and must use snapshot_objects() instead.
//
// So when something off-thread wants an authoritative in-place count, it does
// not walk anything itself: it raises this request and reads the result the
// engine thread publishes. Cost on an idle frame is a single relaxed load --
// walking every object every frame to keep a stat warm would be absurd in a
// per-frame hot path.
//
// Only a count crosses the boundary. Nothing reached through the callback is
// allowed to outlive it.
std::atomic<bool> g_object_walk_requested{false};
std::atomic<uint32_t> g_object_walk_type{0};
std::atomic<int64_t> g_object_walk_count{-1};
std::atomic<uint64_t> g_object_walk_generation{0};

void service_object_walk_request() {
    auto* mgr = sdk::CClientMgr::get();
    if (mgr == nullptr) {
        return; // leave the request pending; nothing consumed
    }
    // CLAIM the request atomically before walking. A plain load-then-store
    // would erase a request that arrives while we are mid-walk, silently
    // losing it; exchange means a late request stays pending for next frame.
    if (!g_object_walk_requested.exchange(false, std::memory_order_acquire)) {
        return;
    }
    const auto type = static_cast<sdk::ObjectType>(g_object_walk_type.load(std::memory_order_relaxed));
    const auto walked = mgr->for_each_object(type, [](const regenny::LTObject*) {
        // Counting body: for_each_object already returns the visited count, so
        // there is nothing to accumulate here. A real consumer reads the
        // object's transform at this point instead.
    });
    // Publish the count BEFORE bumping the generation, and bump with release,
    // so a reader that acquire-loads the generation first is guaranteed to see
    // the count belonging to it rather than the previous one.
    g_object_walk_count.store(walked.has_value() ? static_cast<int64_t>(*walked) : -1,
                              std::memory_order_relaxed);
    g_object_walk_generation.fetch_add(1, std::memory_order_release);
}

int __fastcall frame_tick_detour(void* _this, void* edx) {
    Framework* fw = Framework::get();
    if (fw != nullptr) {
        fw->note_frame_tick();
    }

    service_object_walk_request();
    Mods::get().on_frame();

    auto* hook = Hooks::get().find("CClientShell::Update");
    if (hook == nullptr || !*hook) {
        return 0; // retired while in-flight: skip the original call
    }
    return hook->original<int(__fastcall*)(void*, void*)>()(_this, edx);
}

// --- quiescence verification (32-bit FEAR2.exe process) --------------------
// Fail-closed by construction: ANY inspection failure means "not quiescent",
// never "probably fine".
//
// x86 port note: our target is a native 32-bit process, so CONTEXT here is the
// x86 context and the instruction pointer is ctx.Eip (a 64-bit port would need
// ctx.Rip -- do not carry this verbatim to x64).

// Open handles for every OTHER thread. Any snapshot/iteration failure sets
// enumeration_ok=false so the caller never reads a partial list as "no threads".
std::vector<HANDLE> open_other_thread_handles(bool& enumeration_ok) {
    enumeration_ok = false;
    std::vector<HANDLE> handles;
    const uint32_t pid = GetCurrentProcessId();
    const uint32_t self_tid = GetCurrentThreadId();

    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return handles;
    }

    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    if (!Thread32First(snap, &te)) {
        CloseHandle(snap);
        return handles;
    }

    do {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self_tid) {
            continue;
        }
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID);
        // Fail-closed: record even a null handle so the verifier refuses to
        // unmap when a thread could not be inspected.
        handles.push_back(h);
    } while (Thread32Next(snap, &te));

    enumeration_ok = (GetLastError() == ERROR_NO_MORE_FILES);

    CloseHandle(snap);
    return handles;
}

bool suspend_and_verify_clear(std::vector<HANDLE>& threads, uintptr_t base, size_t size) {
    if (base == 0 || size == 0) {
        return false;
    }

    std::vector<HANDLE> suspended;
    suspended.reserve(threads.size());
    bool ok = true;

    for (HANDLE h : threads) {
        if (h == nullptr) {
            ok = false;
            break;
        }
        if (SuspendThread(h) == (DWORD)-1) {
            ok = false;
            break;
        }
        suspended.push_back(h);
    }

    if (ok) {
        for (HANDLE h : suspended) {
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_CONTROL;
            if (!GetThreadContext(h, &ctx)) {
                ok = false;
                break;
            }
            const uintptr_t ip = static_cast<uintptr_t>(ctx.Eip); // x86: Eip, not Rip
            if (ip >= base && ip < base + size) {
                ok = false;
                break;
            }
        }
    }

    for (HANDLE h : suspended) {
        ResumeThread(h);
    }
    return ok;
}

bool prove_quiescent(uintptr_t base, size_t size, int32_t attempts) {
    for (int32_t attempt = 0; attempt < attempts; ++attempt) {
        bool enumeration_ok = false;
        auto handles = open_other_thread_handles(enumeration_ok);
        const bool clear = enumeration_ok && suspend_and_verify_clear(handles, base, size);
        for (HANDLE h : handles) {
            if (h != nullptr) {
                CloseHandle(h);
            }
        }
        if (clear) {
            return true;
        }
        Sleep(5);
    }
    return false;
}

// --- diagnostics payload builders (free functions; SEH stays out of lambdas) -

// Minimal JSON string escaper (backslashes/quotes/control chars) -- the
// DatabaseMgr path strings genuinely contain backslashes ("Database\Loki.
// gamedb"), so this is a correctness requirement, not decoration.
void json_escape_append(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char esc[8];
                    snprintf(esc, sizeof(esc), "\\u%04x", c);
                    out += esc;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    out += '"';
}

// A JSON object built FIELD BY FIELD, so a key and its value cannot drift apart.
//
// WHY THIS EXISTS, from three failures in one sitting: the object report below was a single
// snprintf with 78 format specifiers. Twice an edit added arguments without specifiers or
// specifiers without arguments, and both times the shifts PARTIALLY CANCELLED -- so most
// fields still printed plausible numbers and one read 669, a real count from a different
// counter in the same struct. The third time the new fields were simply absent, because
// extra arguments are benign while missing specifiers are silent.
//
// A format string has no type checking across it and no way to notice a shift. Naming each
// field AT its value removes the whole class of bug, and drops the fixed buffer and its
// truncation guard with it -- the report can no longer outgrow itself either.
class JsonFields {
public:
    explicit JsonFields(std::string& out) : m_out{out} { m_out += '{'; }
    ~JsonFields() { m_out += '}'; }

    JsonFields(const JsonFields&) = delete;
    JsonFields& operator=(const JsonFields&) = delete;

    JsonFields& u(const char* k, size_t v) {
        key(k);
        m_out += std::to_string(v);
        return *this;
    }
    JsonFields& i(const char* k, long long v) {
        key(k);
        m_out += std::to_string(v);
        return *this;
    }
    // Fixed precision so a float field reads the same as it did under %.4f.
    JsonFields& f(const char* k, double v, int precision = 2) {
        key(k);
        char tmp[64];
        snprintf(tmp, sizeof(tmp), "%.*f", precision, v);
        m_out += tmp;
        return *this;
    }
    JsonFields& b(const char* k, bool v) {
        key(k);
        m_out += v ? "true" : "false";
        return *this;
    }
    // Escaped, so a Windows path in a field can no longer break the payload -- which it did
    // twice before this existed.
    JsonFields& s(const char* k, const std::string& v) {
        key(k);
        json_escape_append(m_out, v);
        return *this;
    }
    JsonFields& hex(const char* k, uintptr_t v) {
        key(k);
        char tmp[32];
        snprintf(tmp, sizeof(tmp), "\"0x%08" PRIXPTR "\"", v);
        m_out += tmp;
        return *this;
    }

private:
    void key(const char* k) {
        if (!m_first) {
            m_out += ',';
        }
        m_first = false;
        m_out += '"';
        m_out += k;
        m_out += "\":";
    }

    std::string& m_out;
    bool m_first{true};
};

std::string build_health_fragment() {
    Framework* fw = Framework::get();
    char buf[256];
    snprintf(buf, sizeof(buf),
             "\"pid\":%lu,\"state\":\"%s\",\"sdk_ready\":%s,\"hooks\":%zu,"
             "\"hooks_retired\":%zu,\"frame_ticks\":%llu",
             GetCurrentProcessId(),
             fw->is_shutting_down() ? "shutting_down" : "running",
             sdk::Modules::get().is_initialized() ? "true" : "false",
             Hooks::get().count(), Hooks::get().retired_count(),
             static_cast<unsigned long long>(fw->frame_ticks()));
    return buf;
}

std::string build_targets_json() {
    // Grown three times now, each time by one more field. The GUARD below is the part
    // that matters: an overflow used to ship invalid JSON and break the caller's parser
    // with a mystery offset, which cost more than the missing field would have.
    // Grown as the report gained fields; the guard below is what makes this safe to grow
    // reactively rather than having to guess right. It reported `needed: 3118` when the
    // reachability block pushed past 3072, which is exactly the failure mode a half-written
    // JSON object would have hidden.
    char buf[5120];
    const auto* exe = sdk::Modules::get().exe();
    auto* client_mgr = sdk::CClientMgr::get();
    const auto engine_time = sdk::Engine::client_time();
    const auto shell_game = sdk::CClientShell::game_time_seconds();
    const auto shell_real = sdk::CClientShell::real_time_seconds();
    const auto force = sdk::Engine::global_force();
    // Enumerated ONCE: the walk touches 128 buckets, and a mod asking twice would get
    // two different instants of a table the engine mutates.
    const auto convars = sdk::Engine::console_vars();
    size_t convars_named = 0;
    for (const auto& cv : convars) {
        if (!cv.name.empty()) {
            ++convars_named;
        }
    }
    // THE LOCAL PLAYER, and a cross-check worth having: the shell keeps the object as a
    // HANDLE and as a POINTER, so resolving the handle through the engine's own table
    // must land on the same object the pointer names. Two independently-stored routes
    // agreeing is what makes this a mapping rather than a lucky offset.
    const auto player = sdk::CClientShell::local_player(0);
    const auto player_n = sdk::CClientShell::local_player_count();
    bool player_routes_agree = false;
    std::string player_mdl;
    if (player.has_value()) {
        if (auto* mgr2 = sdk::CClientMgr::get(); mgr2 != nullptr) {
            player_routes_agree = mgr2->object_from_handle(player->handle) == player->object;
        }
        // What the object IS, in plain text -- the cheapest proof it is the player.
        // A .mdl path is a Windows path, so its backslashes must be doubled to keep the
        // JSON valid -- the same requirement the models endpoint already handles.
        for (const char ch : sdk::model_filename(player->object).value_or(std::string{})) {
            if (ch == '\\') {
                player_mdl += "\\\\";
            } else {
                player_mdl += ch;
            }
        }
    }

    // THE WHOLE VR CHAIN, end to end, on the object that matters: local player -> its
    // skeleton -> a socket by NAME -> that socket's world transform. Four subsystems in
    // one expression, which is why it is worth exercising as a unit rather than
    // separately. The hands are the interesting pair: their bones are clean at rest, so
    // a mod can read them from any thread, unlike the view bones.
    int player_sockets = 0;
    bool hands_ok = false, hands_clean = false, hands_distinct = false;
    float hands_reach = -1.0f;
    float lhand[3] = {0, 0, 0}, rhand[3] = {0, 0, 0};
    if (player.has_value()) {
        if (const auto sk = sdk::ModelSkeleton::from_object(player->object); sk.has_value()) {
            player_sockets = static_cast<int>(sk->socket_count());
            const auto l = sk->socket_world_transform("LeftHand");
            const auto r = sk->socket_world_transform("RightHand");
            if (l.has_value() && r.has_value()) {
                hands_ok = true;
                hands_clean = !l->stale && !r->stale;
                lhand[0] = l->position.x; lhand[1] = l->position.y; lhand[2] = l->position.z;
                rhand[0] = r->position.x; rhand[1] = r->position.y; rhand[2] = r->position.z;
                // Two DIFFERENT bones must give two different points. If the socket
                // lookup or the node indexing collapsed, both hands would land together
                const float dx = lhand[0] - rhand[0];
                const float dy = lhand[1] - rhand[1];
                const float dz = lhand[2] - rhand[2];
                hands_distinct = (dx * dx + dy * dy + dz * dz) > 1.0f;
                // AND the hands must belong to the BODY. This is the regression guard for
                // the double-apply bug: when a world-space bone cache was composed with
                // the object's transform anyway, sockets landed 5449 units from their
                // owner. A hand is attached to a person, so its distance from the
                // object's own origin is bounded by the model, not by the level.
                if (const auto info = sdk::object_info(player->object); info.has_value()) {
                    const auto& pos = info->position;
                    const auto d = [&](const float* h) {
                        const float ax = h[0] - pos.x, ay = h[1] - pos.y, az = h[2] - pos.z;
                        return std::sqrt(ax * ax + ay * ay + az * az);
                    };
                    hands_reach = std::max(d(lhand), d(rhand));
                }
            }
        }
    }

    // Direct probe of the sector array itself: how many of the first N sectors read back
    // with planes at all. If this is zero the array access is wrong and any containment
    // result built on it is vacuous rather than negative.
    int sec_read_ok = 0, sec_with_planes = 0, sec_plane_total = 0;
    // AND THE TEST THAT ACTUALLY EXERCISES THE PLANE SIGN, which the player-location query
    // does not: only 19 of 263 sectors carry planes and the player's is not among them. A
    // convex cell's own box centre must satisfy its own planes, so an inverted sign collapses
    // this to zero. That is how the sign was wrong for several passes without any test
    // noticing -- the brute-force oracle called the same predicate.
    int sec_planed = 0, sec_centre_in = 0;
    int sec_plane_probed = 0, sec_plane_pos = 0, sec_plane_neg = 0;
    {
        const int n = static_cast<int>(sdk::VisTree::sector_count().value_or(0));
        for (int i = 0; i < n; ++i) {
            if (const auto s = sdk::VisTree::sector(static_cast<size_t>(i)); s.has_value()) {
                ++sec_read_ok;
                if (s->plane_count != 0) {
                    ++sec_with_planes;
                }
                sec_plane_total += static_cast<int>(sdk::VisTree::sector_planes(
                    static_cast<size_t>(i)).size());
            }
            // The centre test, on the sectors that actually have planes.
            if (const auto sc = sdk::VisTree::sector(static_cast<size_t>(i));
                sc.has_value() && sc->plane_count != 0) {
                ++sec_planed;
                const regenny::LTVector mid{(sc->min.x + sc->max.x) * 0.5f,
                                            (sc->min.y + sc->max.y) * 0.5f,
                                            (sc->min.z + sc->max.z) * 0.5f};
                if (sdk::VisTree::sector_contains(static_cast<size_t>(i), mid)
                        .value_or(false)) {
                    ++sec_centre_in;
                }
                // The signs themselves, so the counterfactual is DATA rather than an
                // inference: under the old `reject when d > 0` rule a sector would have been
                // rejected iff any plane gave d > 0 at its centre.
                for (const auto& pl : sdk::VisTree::sector_planes(static_cast<size_t>(i))) {
                    const float dd = pl.normal.x * mid.x + pl.normal.y * mid.y +
                                     pl.normal.z * mid.z - pl.distance;
                    ++sec_plane_probed;
                    if (dd > 0.0f) {
                        ++sec_plane_pos;
                    } else if (dd < 0.0f) {
                        ++sec_plane_neg;
                    }
                }
            }
        }
    }
    // WHERE IS THE PLAYER, IN THE WORLD'S OWN TERMS -- the locomotion question, and the
    // KD shortcut checked against brute force so the descent is proven, not trusted.
    //
    // sectors_at() descends the tree to a leaf and hands back a handful of candidates;
    // scanning all sectors and testing every one's planes is the ORACLE. If the KD result
    // ever omits a sector the brute-force scan finds, the descent's side convention is
    // wrong -- which is the one thing about that structure the layout does not state.
    int sector_total = 0, sector_candidates = 0, sector_brute = 0;
    int player_sector = -1, brute_sector = -1;
    if (player.has_value()) {
        if (const auto info = sdk::object_info(player->object); info.has_value()) {
            sector_total = static_cast<int>(sdk::VisTree::sector_count().value_or(0));
            const auto cands = sdk::VisTree::sectors_at(info->position);
            sector_candidates = static_cast<int>(cands.size());
            if (const auto s = sdk::VisTree::sector_containing(info->position);
                s.has_value()) {
                player_sector = static_cast<int>(s->index);
            }
            // The oracle: every sector in the world, tested directly.
            for (int i = 0; i < sector_total; ++i) {
                if (sdk::VisTree::sector_contains(static_cast<size_t>(i), info->position)
                        .value_or(false)) {
                    ++sector_brute;
                    if (brute_sector < 0) {
                        brute_sector = i;
                    }
                }
            }
        }
    }

    // REGION QUERY vs a FULL SCAN. This validates the TRAVERSAL: both sides use the same
    // per-sector test, which is grounded in the engine's code and in the centre-containment
    // invariant, so what is being checked here is whether the descent VISITS everything it
    // should. Three radii, because a descent bug can show up only at one scale: 0 collapses to
    // point location, 250 is a play-space, and 4000 spans most of the level.
    int region_probes = 0, region_agree = 0, region_hits = 0;
    if (player.has_value()) {
        if (const auto info = sdk::object_info(player->object); info.has_value()) {
            const int nsec = static_cast<int>(sdk::VisTree::sector_count().value_or(0));
            for (const float r : {0.0f, 250.0f, 4000.0f}) {
                ++region_probes;
                const auto found = sdk::VisTree::sectors_in_sphere(info->position, r, 512);
                int brute = 0;
                bool all_found = true;
                for (int i = 0; i < nsec; ++i) {
                    if (!sdk::VisTree::sector_overlaps_sphere(static_cast<size_t>(i),
                                                              info->position, r)
                             .value_or(false)) {
                        continue;
                    }
                    ++brute;
                    bool seen = false;
                    for (const auto& f : found) {
                        if (f.index == static_cast<size_t>(i)) {
                            seen = true;
                            break;
                        }
                    }
                    if (!seen) {
                        all_found = false;
                    }
                }
                region_hits += brute;
                if (all_found && static_cast<int>(found.size()) == brute) {
                    ++region_agree;
                }
            }
        }
    }
    // THE BOX VARIANT, same oracle shape as the sphere: descent versus a full scan, at three
    // extents. And the corner_code CURRENCY check -- every plane's cached selector against
    // what its own normal implies. A stale one breaks the engine's box queries while leaving
    // the sphere path working, so it is worth knowing whether the live world has any.
    int box_probes = 0, box_agree = 0, box_hits = 0;
    int code_probed = 0, code_current = 0;
    {
        const int nsec = static_cast<int>(sdk::VisTree::sector_count().value_or(0));
        for (int i = 0; i < nsec; ++i) {
            const auto planes = sdk::VisTree::sector_planes(static_cast<size_t>(i));
            for (size_t k = 0; k < planes.size(); ++k) {
                ++code_probed;
                if (sdk::VisTree::plane_corner_code_is_current(static_cast<size_t>(i), k)
                        .value_or(false)) {
                    ++code_current;
                }
            }
        }
        if (player.has_value()) {
            if (const auto info = sdk::object_info(player->object); info.has_value()) {
                for (const float e : {0.0f, 250.0f, 4000.0f}) {
                    ++box_probes;
                    const regenny::LTVector lo{info->position.x - e, info->position.y - e,
                                               info->position.z - e};
                    const regenny::LTVector hi{info->position.x + e, info->position.y + e,
                                               info->position.z + e};
                    const auto found = sdk::VisTree::sectors_in_box(lo, hi, 512);
                    int brute = 0;
                    bool all_found = true;
                    for (int i = 0; i < nsec; ++i) {
                        if (!sdk::VisTree::sector_overlaps_box(static_cast<size_t>(i), lo, hi)
                                 .value_or(false)) {
                            continue;
                        }
                        ++brute;
                        bool seen = false;
                        for (const auto& f : found) {
                            if (f.index == static_cast<size_t>(i)) {
                                seen = true;
                                break;
                            }
                        }
                        if (!seen) {
                            all_found = false;
                        }
                    }
                    box_hits += brute;
                    if (all_found && static_cast<int>(found.size()) == brute) {
                        ++box_agree;
                    }
                }
            }
        }
    }
    // SPHERE INSIDE BOX: a real cross-check between two INDEPENDENT implementations. The
    // sphere of radius e is contained in the box of half-extent e, so every sector the sphere
    // touches must also be touched by the box. The two share nothing -- different engine
    // functions, different traversal arithmetic (`split > c+r` versus `split <= max`), and
    // different plane rejects (a `-radius` slack term versus a selected corner against zero).
    // That independence is the whole point: unlike a scan built on the code under test, this
    // one cannot be fooled by an error the two happen to share, because they share no code.
    int contain_probes = 0, contain_ok = 0, contain_sphere = 0;
    if (player.has_value()) {
        if (const auto info = sdk::object_info(player->object); info.has_value()) {
            for (const float e : {1.0f, 250.0f, 4000.0f}) {
                ++contain_probes;
                const regenny::LTVector lo{info->position.x - e, info->position.y - e,
                                           info->position.z - e};
                const regenny::LTVector hi{info->position.x + e, info->position.y + e,
                                           info->position.z + e};
                const auto sph = sdk::VisTree::sectors_in_sphere(info->position, e, 512);
                const auto box = sdk::VisTree::sectors_in_box(lo, hi, 512);
                contain_sphere += static_cast<int>(sph.size());
                bool subset = true;
                for (const auto& a : sph) {
                    bool seen = false;
                    for (const auto& b : box) {
                        if (a.index == b.index) {
                            seen = true;
                            break;
                        }
                    }
                    if (!seen) {
                        subset = false;
                    }
                }
                if (subset) {
                    ++contain_ok;
                }
            }
        }
    }
    // THE ENGINE'S OWN ANSWER versus my reimplementation of the query that produced it.
    // LTSpatialRecord_CollectSphere runs LTVisTree_QuerySphere with AddEntry as the callback,
    // so a record's entry list IS this query's result as the ENGINE computed it, at relink
    // time, in its own code. Feeding my walk the volume the record itself stores holds the
    // input fixed, so a disagreement is either my traversal or a record collected against a
    // volume it no longer holds.
    int rec_objects = 0, rec_with_entries = 0, rec_match = 0, rec_entries = 0, rec_count_ok = 0;
    int rev_probed = 0, rev_ok = 0, rev_pairs = 0;
    int shape_probed = 0, shape_agree = 0;
    int gate_rend = 0, gate_rend_match = 0, gate_norend = 0, gate_norend_match = 0,
        gate_norend_empty = 0;
    int rec_missing = 0, rec_extra = 0, rec_only_missing = 0, rec_only_extra = 0,
        rec_both = 0, rec_consistent = 0;
    {
        auto* rec_mgr = sdk::CClientMgr::get();
        const size_t types = sdk::CClientMgr::object_list_count();
        std::vector<sdk::CClientMgr::ObjectSnapshot> snaps(512);
        for (size_t t = 0; t < types; ++t) {
            const auto taken = rec_mgr == nullptr
                                   ? std::nullopt
                                   : rec_mgr->snapshot_objects(static_cast<sdk::ObjectType>(t),
                                                     snaps.data(), snaps.size());
            if (!taken.has_value()) {
                continue;
            }
            for (size_t si2 = 0; si2 < *taken; ++si2) {
                const auto* obj =
                    reinterpret_cast<const regenny::LTObject*>(snaps[si2].address);
                ++rec_objects;
                const auto secs = sdk::VisTree::sectors_for_object(obj);
                rec_entries += static_cast<int>(secs.size());
                if (!secs.empty()) {
                    ++rec_with_entries;
                }
                // the maintained counter against the walked length
                if (sdk::VisTree::spatial_entry_count(obj).value_or(SIZE_MAX) == secs.size()) {
                    ++rec_count_ok;
                }
                const bool rmatch =
                    sdk::VisTree::spatial_record_matches_volume(obj).value_or(false);
                if (rmatch) {
                    ++rec_match;
                }
                // THE GATE. LTObjectOwner_UpdateSpatialRecord stores the volume
                // UNCONDITIONALLY and collects only `if ((flags & 1) && !(flags2 & 0x700))`,
                // which is is_renderable(). So a non-renderable object keeps a current volume
                // with a RELEASED entry list, and "the entries do not match the volume" is the
                // engine working as designed rather than a fault. Splitting the mismatches by
                // that gate is what tells the two apart.
                // WHICH DIRECTION the residual mismatches go. `missing` blames a stale
                // engine record, `extra` blames this SDK's traversal -- and they must be
                // counted apart, because a bare "differs" cannot assign blame.
                if (const auto d = sdk::VisTree::spatial_record_diff(obj); d.has_value()) {
                    rec_missing += static_cast<int>(d->missing);
                    rec_extra += static_cast<int>(d->extra);
                    if (d->missing != 0 && d->extra == 0) {
                        ++rec_only_missing;
                    } else if (d->extra != 0 && d->missing == 0) {
                        ++rec_only_extra;
                    } else if (d->extra != 0) {
                        ++rec_both;
                    }
                }
                if (sdk::VisTree::spatial_record_is_consistent(obj).value_or(false)) {
                    ++rec_consistent;
                }
                const bool rend = sdk::is_renderable(obj).value_or(false);
                if (rend) {
                    ++gate_rend;
                    if (rmatch) {
                        ++gate_rend_match;
                    }
                } else {
                    ++gate_norend;
                    if (rmatch) {
                        ++gate_norend_match;
                    }
                    if (secs.empty()) {
                        ++gate_norend_empty;
                    }
                }
                // THE TAG AGAINST THE TYPE RULE. cull_volume() now takes the layout from the
                // record's own bit; computed_cull_volume() still derives it from the object's
                // type. Where they differ, the old code was reading a sphere's four floats as
                // a box's six or the reverse.
                if (const auto sv = sdk::cull_volume(obj); sv.has_value()) {
                    if (const auto cv = sdk::computed_cull_volume(obj); cv.has_value()) {
                        ++shape_probed;
                        if (sv->shape == cv->shape) {
                            ++shape_agree;
                        }
                    }
                }
                // THE REVERSE INDEX must agree with the forward one: if this object lists a
                // sector, that sector must list this object. One doubly-linked structure read
                // from both ends -- a wrong hit_next or hit_head breaks the pairing while each
                // direction still looks like a plausible list.
                for (const size_t si : secs) {
                    ++rev_probed;
                    const auto objs = sdk::VisTree::objects_in_sector(si);
                    for (const auto* o : objs) {
                        if (o == obj) {
                            ++rev_ok;
                            break;
                        }
                    }
                }
            }
        }
        const int nsec = static_cast<int>(sdk::VisTree::sector_count().value_or(0));
        for (int i = 0; i < nsec; ++i) {
            rev_pairs += static_cast<int>(sdk::VisTree::objects_in_sector(static_cast<size_t>(i)).size());
        }
    }
    // THE SECTOR'S OWN PORTAL ARRAY, against the portal table's back-references. Two
    // representations of one graph, and LTVisSector_LoadFromStream derives the second from the
    // first via LTVisPortal_AttachSector -- so this checks a real engine invariant, not my
    // arithmetic against itself.
    //
    // Also the STORED SECTOR INDEX against the index used to reach it. Every accessor converts
    // pointer to index by arithmetic on the table base; the engine writes the index into the
    // sector too, so a wrong stride or base shows up here immediately instead of silently
    // yielding plausible sectors.
    int sec_idx_probed = 0, sec_idx_ok = 0, sec_links_probed = 0, sec_links_ok = 0;
    int sec_portal_sum = 0, sec_portal_listed = 0;
    {
        const int nsec = static_cast<int>(sdk::VisTree::sector_count().value_or(0));
        for (int i = 0; i < nsec; ++i) {
            ++sec_idx_probed;
            if (sdk::VisTree::sector_index_is_stored_index(static_cast<size_t>(i))
                    .value_or(false)) {
                ++sec_idx_ok;
            }
            sec_portal_sum +=
                static_cast<int>(sdk::VisTree::sector_portal_count(static_cast<size_t>(i))
                                     .value_or(0));
            sec_portal_listed +=
                static_cast<int>(sdk::VisTree::sector_portals(static_cast<size_t>(i)).size());
            ++sec_links_probed;
            if (sdk::VisTree::sector_portal_links_agree(static_cast<size_t>(i))
                    .value_or(false)) {
                ++sec_links_ok;
            }
        }
    }
    // THE VARIABLE-LENGTH RECORD, read at its own declared length. portal_polygon() reads
    // vertex_count vertices; Portal.vertices holds at most four and flags the shortfall. Both
    // must agree with the stored count, and every vertex must still satisfy the plane equation
    // the struct-based path already asserts -- reading further into the record and STILL landing
    // on the plane is what shows the extra vertices are really there.
    int poly_probed = 0, poly_len_ok = 0, poly_on_plane = 0, poly_trunc = 0, poly_verts = 0;
    {
        const int npor = static_cast<int>(sdk::VisTree::portal_count().value_or(0));
        for (int i = 0; i < npor; ++i) {
            const auto pp = sdk::VisTree::portal(static_cast<size_t>(i));
            if (!pp.has_value()) {
                continue;
            }
            ++poly_probed;
            if (pp->vertices_truncated) {
                ++poly_trunc;
            }
            const auto poly = sdk::VisTree::portal_polygon(static_cast<size_t>(i));
            if (poly.size() == pp->vertex_count) {
                ++poly_len_ok;
            }
            poly_verts += static_cast<int>(poly.size());
            bool on = !poly.empty();
            for (const auto& v : poly) {
                const float d = pp->plane.normal.x * v.x + pp->plane.normal.y * v.y +
                                pp->plane.normal.z * v.z - pp->plane.distance;
                if (d > 0.5f || d < -0.5f) {
                    on = false;
                }
            }
            if (on) {
                ++poly_on_plane;
            }
        }
    }
    // REACHABILITY, checked against properties the BFS cannot fake.
    //
    //   1-hop agreement -- sectors_within(s,1) must be exactly {s} + sector_neighbours(s),
    //                      tying the walk to the primitive already validated against the
    //                      portal table from both directions.
    //   monotonicity    -- within(s,n) must be a subset of within(s,n+1).
    //   SYMMETRY        -- b reachable from a in <=n hops iff a is from b. This is the strong
    //                      one: it holds because portal adjacency is symmetric (688/688,
    //                      established independently of any traversal), so a BFS that lost or
    //                      invented an edge breaks it.
    //   components      -- every member of a component must report the SAME component, which is
    //                      transitivity and cannot hold by accident across 263 sectors.
    int rch_probed = 0, rch_1hop_ok = 0, rch_mono_ok = 0, rch_sym_probed = 0, rch_sym_ok = 0;
    int rch_comp_ok = 0, rch_comp_size = 0, rch_hops_ok = 0;
    {
        const int nsec = static_cast<int>(sdk::VisTree::sector_count().value_or(0));
        for (int i = 0; i < nsec; ++i) {
            const size_t si = static_cast<size_t>(i);
            ++rch_probed;
            const auto w0 = sdk::VisTree::sectors_within(si, 0);
            const auto w1 = sdk::VisTree::sectors_within(si, 1);
            const auto w2 = sdk::VisTree::sectors_within(si, 2);
            const auto nb = sdk::VisTree::sector_neighbours(si);
            if (w0.size() == 1 && w0[0] == si && w1.size() == nb.size() + 1) {
                bool all = true;
                for (const size_t n : nb) {
                    if (std::find(w1.begin(), w1.end(), n) == w1.end()) {
                        all = false;
                    }
                }
                if (all && std::find(w1.begin(), w1.end(), si) != w1.end()) {
                    ++rch_1hop_ok;
                }
            }
            bool mono = w1.size() <= w2.size();
            for (const size_t a : w1) {
                if (std::find(w2.begin(), w2.end(), a) == w2.end()) {
                    mono = false;
                }
            }
            if (mono) {
                ++rch_mono_ok;
            }
            // symmetry at a fixed radius, over the whole 2-hop frontier
            for (const size_t b : w2) {
                ++rch_sym_probed;
                const auto back = sdk::VisTree::sectors_within(b, 2);
                if (std::find(back.begin(), back.end(), si) != back.end()) {
                    ++rch_sym_ok;
                }
            }
            // hop distance must agree with the frontier that contains it
            if (const auto h = sdk::VisTree::sector_hops(si, si); h.value_or(999) == 0) {
                ++rch_hops_ok;
            }
        }
        // component transitivity: every member agrees on the component
        if (nsec > 0) {
            const auto comp = sdk::VisTree::sector_component(0);
            rch_comp_size = static_cast<int>(comp.size());
            for (const size_t m : comp) {
                const auto other = sdk::VisTree::sector_component(m);
                if (other.size() == comp.size()) {
                    ++rch_comp_ok;
                }
            }
        }
    }
    // MY REIMPLEMENTATION vs THE ENGINE'S OWN FUNCTION, called through vtable slot 16. This is
    // the strongest kind of check available: not a second walk of my own, but the shipped code
    // answering the same question.
    //
    // The probe points are chosen where a mistake would actually show. Points exactly ON each
    // bound test the strict-versus-inclusive boundary the engine uses (min > p, max < p, so the
    // surface is INSIDE); points one unit outside each of the six faces test each comparison
    // separately, which a single corner probe would conflate.
    int wb_probed = 0, wb_agree = 0, wb_outside = 0, wb_inside = 0;
    int wb_bounds_ok = 0, wb_bounds_probed = 0;
    float wb_inst[6]{}, wb_glob[6]{};
    int wb_loaded = -1, wb_srv_probed = 0, wb_srv_expanded = -1;
    // WHICH DIRECTION IS THE +0x08 PAIR? ILTModel_GetBindPoseNodeTransform copies it and then INVERTS it
    // (LTTransform_InvertInPlace), so either the record stores the bind pose and the engine hands out its
    // inverse, or the record stores the INVERSE bind and the engine hands out the pose. A model-space bind
    // pose has one signature its inverse does not: adjacent bones sit a BONE LENGTH apart. So measure the
    // mean parent-child position distance both ways -- raw, and after inverting each pose.
    double raw_edge = 0.0, inv_edge = 0.0;
    int edge_n = 0;
    int inv_roundtrip_ok = 0, inv_roundtrip_n = 0;
    double rt_worst = 0.0, rt_worst_mag = 0.0;
    // THE DECISIVE CROSS-CHECK, through the SDK's own guarded accessor rather than a raw call here:
    // ModelSkeleton::engine_bind_pose() resolves ILTModelClient vt[22] with the mapped bound, verifies its
    // RVA, and SEH-guards the invocation.
    int eng_calls = 0, eng_match = 0, eng_rc_ok = 0;
    double eng_worst = 0.0;
    const bool eng_avail = sdk::ModelSkeleton::engine_bind_pose_available();
    int bp_reject_oor = 0;
    if (auto* emgr = sdk::CClientMgr::get(); emgr != nullptr) {
        std::vector<sdk::CClientMgr::ObjectSnapshot> esnaps(2048);
        for (size_t t = 0; t < sdk::CClientMgr::object_list_count(); ++t) {
            const auto taken = emgr->snapshot_objects(static_cast<sdk::ObjectType>(t), esnaps.data(),
                                                      esnaps.size());
            if (!taken.has_value()) {
                continue;
            }
            for (size_t si = 0; si < *taken; ++si) {
                const auto* obj = reinterpret_cast<const regenny::LTObject*>(esnaps[si].address);
                const auto sk = sdk::ModelSkeleton::from_object(obj);
                if (!sk.has_value()) {
                    continue;
                }
                if (bp_reject_oor == 0) {
                    bp_reject_oor = (!sk->inverse_bind_pose(sk->node_count() + 1000).has_value() &&
                                     !sk->bind_pose(sk->node_count() + 1000).has_value()) ? 1 : -1;
                }
                for (size_t i = 0; i < sk->node_count(); ++i) {
                    const auto par = sk->parent_of(i);
                    if (!par.has_value() || *par == i) {
                        continue;
                    }
                    const auto a = sk->inverse_bind_pose(i), b = sk->inverse_bind_pose(*par);
                    if (!a.has_value() || !b.has_value()) {
                        continue;
                    }
                    const auto dist = [](const regenny::LTVector& u, const regenny::LTVector& v) {
                        const double dx = u.x - v.x, dy = u.y - v.y, dz = u.z - v.z;
                        return std::sqrt(dx * dx + dy * dy + dz * dz);
                    };
                    // The inverted side now goes through the SDK accessor, so the probe measures the
                    // shipped helper rather than a copy of its arithmetic.
                    const auto ea = sk->bind_pose(i);
                    const auto eb = sk->bind_pose(*par);
                    if (!ea.has_value() || !eb.has_value()) {
                        continue;
                    }
                    raw_edge += dist(a->position, b->position);
                    inv_edge += dist(ea->position, eb->position);
                    ++edge_n;
                    // invert_rigid is its own inverse: applying it twice must return the input. Checks the
                    // shipped helper's arithmetic against a property rather than against a copy of itself.
                    if (eng_avail && eng_calls < 4000) {
                        ++eng_calls;
                        if (const auto got = sk->engine_bind_pose(i); got.has_value()) {
                            ++eng_rc_ok;
                            const double d = dist(got->position, ea->position);
                            const double dr = std::fabs(got->rotation.x - ea->rotation.x) +
                                              std::fabs(got->rotation.y - ea->rotation.y) +
                                              std::fabs(got->rotation.z - ea->rotation.z) +
                                              std::fabs(got->rotation.w - ea->rotation.w);
                            if (d + dr > eng_worst) {
                                eng_worst = d + dr;
                            }
                            if (d < 0.01 && dr < 1e-3) {
                                ++eng_match;
                            }
                        }
                    }
                    const auto back = sdk::ModelSkeleton::invert_rigid(*ea);
                    ++inv_roundtrip_n;
                    // ALL SEVEN components, and finite. Comparing only x and w would pass a bug in y or
                    // z, which is exactly where the inversion's cross terms live.
                    const bool finite = std::isfinite(back.position.x) && std::isfinite(back.position.y) &&
                                        std::isfinite(back.position.z) && std::isfinite(back.rotation.x) &&
                                        std::isfinite(back.rotation.y) && std::isfinite(back.rotation.z) &&
                                        std::isfinite(back.rotation.w);
                    {
                        const double e = dist(back.position, a->position);
                        if (e > rt_worst) {
                            rt_worst = e;
                            rt_worst_mag = std::sqrt(static_cast<double>(a->position.x) * a->position.x +
                                                     static_cast<double>(a->position.y) * a->position.y +
                                                     static_cast<double>(a->position.z) * a->position.z);
                        }
                    }
                    if (finite && dist(back.position, a->position) < 0.01 &&
                        std::fabs(back.rotation.x - a->rotation.x) < 1e-4f &&
                        std::fabs(back.rotation.y - a->rotation.y) < 1e-4f &&
                        std::fabs(back.rotation.z - a->rotation.z) < 1e-4f &&
                        std::fabs(back.rotation.w - a->rotation.w) < 1e-4f) {
                        ++inv_roundtrip_ok;
                    }
                }
            }
        }
        if (edge_n > 0) {
            raw_edge /= edge_n;
            inv_edge /= edge_n;
        }
    }
    // THE DEVICE'S VTABLE: where it lives and whether it is writable. A stereo path patches this table, so
    // the reportable facts are its address, that it is NOT inside d3d9.dll, and its page protection.
    const uintptr_t dev_vt = sdk::Render::device_vtable();
    // -1 = could not find out, 0 = read-only, 1 = writable. Folding the first two together would hide
    // "no device yet" behind "needs VirtualProtect".
    const auto dev_vt_w = sdk::Render::device_vtable_writable();
    const int dev_vt_writable = dev_vt_w.has_value() ? (*dev_vt_w ? 1 : 0) : -1;
    int dev_vt_outside_d3d9 = 0;
    {
        const auto d3d9 = GetModuleHandleW(L"d3d9.dll");
        if (d3d9 != nullptr && dev_vt != 0) {
            const auto base = reinterpret_cast<uintptr_t>(d3d9);
            const auto size = utility::get_module_size(d3d9);
            if (size.has_value()) {
                dev_vt_outside_d3d9 = (dev_vt < base || dev_vt >= base + *size) ? 1 : 0;
            }
        }
    }
    // THE GAME DLL'S PER-FRAME HOOK ANCHORS. Reported so the slot map is checked against the live
    // build every run: the identity string must match, and all three addresses must land inside
    // gameclient.dll -- an implementation slot pointing anywhere else would mean the layout
    // assumption is wrong.
    int gcs_ok = sdk::GameClientShell::available() ? 1 : 0;
    int gcs_pre_empty = sdk::GameClientShell::pre_update_entry_returns_immediately() ? 1 : 0;
    int gcs_shapes = sdk::GameClientShell::slots_match_mapped_shapes() ? 1 : 0;
    int gcs_entry_agrees = 0;
    int gcs_in_module = 0;
    char gcs_name[64]{};
    if (const auto nm = sdk::GameClientShell::implementation_name(); nm.has_value()) {
        const size_t n = nm->size() < sizeof(gcs_name) - 1 ? nm->size() : sizeof(gcs_name) - 1;
        memcpy(gcs_name, nm->c_str(), n);
    }
    {
        // The vtable ENTRY for slot 2 must sit inside the vtable and hold exactly the function the
        // anchor accessor reports -- the two routes to the same slot have to agree.
        const uintptr_t entry = sdk::GameClientShell::pre_update_vtable_entry();
        gcs_entry_agrees = (entry != 0 && *reinterpret_cast<uintptr_t*>(entry) ==
                                             sdk::GameClientShell::pre_update_fn()) ? 1 : 0;
        const uintptr_t fns[3] = {sdk::GameClientShell::pre_update_fn(),
                                  sdk::GameClientShell::update_fn(),
                                  sdk::GameClientShell::post_update_fn()};
        for (const auto fn : fns) {
            if (fn != 0) {
                ++gcs_in_module;  // the accessor already refuses anything outside gameclient.dll
            }
        }
    }
    // THE LOCAL PLAYER'S TWO FORMS, read RAW. local_player() now fails closed on a disagreeing
    // pair, so measuring through it could never show one; this goes at the slot directly. The handle
    // and pointer are independent routes to one object and the shell re-resolves the pointer once
    // per frame inside Update, so agreement is a real check on that refresh.
    // Also confirm the safe accessor still ACCEPTS the slots -- failing closed must not over-reject.
    int lp_slots = 0, lp_consistent = 0, lp_accepted = 0;
    for (unsigned li = 0; li < 4; ++li) {
        const auto ok = sdk::CClientShell::local_player_raw_pair_agrees(li);
        if (!ok.has_value()) {
            continue;  // empty slot
        }
        ++lp_slots;
        if (*ok) {
            ++lp_consistent;
        }
        if (sdk::CClientShell::local_player(li).has_value()) {
            ++lp_accepted;
        }
    }
    int wb_obj_gap = -1, wb_class_size = -1;
    {
        // DIAGNOSTIC: the actual numbers each path sees, because "they disagree" is not
        // actionable without them.
        if (const auto ib = sdk::WorldBSP::bounds(); ib.has_value()) {
            wb_inst[0] = ib->min.x; wb_inst[1] = ib->min.y; wb_inst[2] = ib->min.z;
            wb_inst[3] = ib->max.x; wb_inst[4] = ib->max.y; wb_inst[5] = ib->max.z;
        }
        if (const auto gb = sdk::WorldBSP::engine_bounds(); gb.has_value()) {
            wb_glob[0] = gb->min.x; wb_glob[1] = gb->min.y; wb_glob[2] = gb->min.z;
            wb_glob[3] = gb->max.x; wb_glob[4] = gb->max.y; wb_glob[5] = gb->max.z;
        }
        // THE SCHEMA'S CLASS SIZE, GUARDED BY THE NEXT SINGLETON. This is the check whose
        // absence let LTWorldClientBSP be recorded as 0x244 for several passes: the server BSP
        // object sits a fixed distance after the client one, so that distance is a hard ceiling
        // on the client class, and a schema that overruns it is provably wrong. Cheap, and it
        // needs no knowledge of what the fields mean.
        {
            const auto cli = reinterpret_cast<uintptr_t>(sdk::WorldBSP::get());
            if (const auto* exe = sdk::Modules::get().exe(); exe != nullptr && cli != 0) {
                const auto srv = *reinterpret_cast<const uintptr_t*>(
                    exe->base + (0x6F6BBC - 0x400000));
                if (srv > cli) {
                    wb_obj_gap = static_cast<int>(srv - cli);
                    wb_class_size = static_cast<int>(sizeof(regenny::LTWorldClientBSP));
                }
            }
        }
        // THE LOAD GATE, cross-checked against two independent signs of the same state: a
        // non-empty world path and a non-zero sector count. Three indicators, one fact.
        if (const auto ld = sdk::WorldBSP::is_world_loaded(); ld.has_value()) {
            wb_loaded = *ld ? 1 : 0;
        }
        // THE SERVER'S DERIVED EXTENT. Its world load writes the globals expanded by 100 units,
        // so this checks a rule read out of that function against what the server actually holds
        // -- reading the producer, then verifying it live.
        if (const auto sb = sdk::WorldBSP::server_bounds(); sb.has_value()) {
            if (const auto gb = sdk::WorldBSP::engine_bounds(); gb.has_value()) {
                wb_srv_probed = 1;
                const float e = 100.0f;
                wb_srv_expanded =
                    (sb->min.x == gb->min.x - e && sb->min.y == gb->min.y - e &&
                     sb->min.z == gb->min.z - e && sb->max.x == gb->max.x + e &&
                     sb->max.y == gb->max.y + e && sb->max.z == gb->max.z + e)
                        ? 1
                        : 0;
            }
        }
        if (const auto ag = sdk::WorldBSP::bounds_agree(); ag.has_value()) {
            ++wb_bounds_probed;
            if (*ag) {
                ++wb_bounds_ok;
            }
        }
        // PROBE POINTS COME FROM THE INSTANCE BOUNDS, deliberately NOT from engine_bounds():
        // deriving the inputs from the code under test makes a failure cascade. The first run of
        // this had a broken engine_bounds(), so every probe point was nonsense and the ENGINE
        // correctly called all fifteen outside -- which read as the engine disagreeing with me.
        if (const auto b = sdk::WorldBSP::bounds(); b.has_value()) {
            const float cx = (b->min.x + b->max.x) * 0.5f;
            const float cy = (b->min.y + b->max.y) * 0.5f;
            const float cz = (b->min.z + b->max.z) * 0.5f;
            const regenny::LTVector pts[] = {
                {cx, cy, cz},                                  // centre: inside
                {b->min.x, cy, cz},   {b->max.x, cy, cz},      // exactly on x faces
                {cx, b->min.y, cz},   {cx, b->max.y, cz},      // exactly on y faces
                {cx, cy, b->min.z},   {cx, cy, b->max.z},      // exactly on z faces
                {b->min.x - 1.0f, cy, cz}, {b->max.x + 1.0f, cy, cz},
                {cx, b->min.y - 1.0f, cz}, {cx, b->max.y + 1.0f, cz},
                {cx, cy, b->min.z - 1.0f}, {cx, cy, b->max.z + 1.0f},
                {b->min.x, b->min.y, b->min.z},                // the min corner itself
                {b->max.x, b->max.y, b->max.z},                // the max corner itself
            };
            for (const auto& pt : pts) {
                const auto mine = sdk::WorldBSP::is_point_outside_world(pt);
                const auto theirs = sdk::WorldBSP::is_point_outside_world_engine(pt);
                if (!mine.has_value() || !theirs.has_value()) {
                    continue;
                }
                ++wb_probed;
                if (*mine == *theirs) {
                    ++wb_agree;
                }
                if (*theirs) {
                    ++wb_outside;
                } else {
                    ++wb_inside;
                }
            }
        }
    }
    // WHICH WORLD IS LOADED. A wrong offset here does not fault -- it yields whatever bytes
    // follow, which is why the checks are on the string's SHAPE: printable, non-empty, carrying
    // the .wld extension the "WLDC" format implies, and with world_name() a genuine suffix of
    // the path. Garbage passes none of those.
    char wp_path[300]{};
    char wp_name[300]{};
    int wp_printable = 0, wp_len = 0;
    {
        if (const auto path = sdk::WorldBSP::world_path(); path.has_value()) {
            wp_len = static_cast<int>(path->size());
            const size_t n = path->size() < sizeof(wp_path) - 1 ? path->size() : sizeof(wp_path) - 1;
            memcpy(wp_path, path->c_str(), n);
            wp_printable = 1;
            for (char c : *path) {
                if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7E) {
                    wp_printable = 0;
                }
            }
        }
        if (const auto name = sdk::WorldBSP::world_name(); name.has_value()) {
            const size_t n = name->size() < sizeof(wp_name) - 1 ? name->size() : sizeof(wp_name) - 1;
            memcpy(wp_name, name->c_str(), n);
        }
    }
    // THE PATH CONTAINS BACKSLASHES, so it goes through the escaper instead of the format
    // string. Emitting it raw produced invalid JSON at column 2678 -- the same trap the
    // DatabaseMgr paths hit, which is exactly why json_escape_append already exists.
    std::string world_json = "\"world_path\":";
    json_escape_append(world_json, wp_path);
    world_json += ",\"world_name\":";
    json_escape_append(world_json, wp_name);
    world_json += ',';
    // PORTALS AND CONNECTIVITY. The load-bearing property is SYMMETRY: if B is reachable
    // from A then A must be reachable from B, because both come from the same portal read
    // from opposite ends. A wrong sector_a/sector_b offset, or a broken pointer-to-index
    // conversion, breaks the pairing while each side still looks like a valid index.
    int portal_total = 0, portal_both_sectors = 0, portal_on_plane = 0;
    int sectors_with_neighbours = 0, neighbour_edges = 0, symmetric_edges = 0;
    int player_neighbours = -1;
    {
        portal_total = static_cast<int>(sdk::VisTree::portal_count().value_or(0));
        for (int i = 0; i < portal_total; ++i) {
            const auto pr = sdk::VisTree::portal(static_cast<size_t>(i));
            if (!pr.has_value()) {
                continue;
            }
            if (pr->sector_a.has_value() && pr->sector_b.has_value() &&
                *pr->sector_a != *pr->sector_b) {
                ++portal_both_sectors;
            }
            // The centre must lie ON the portal's own plane -- the geometric check that
            // pins the record layout, asked through the public struct this time.
            const float cd = pr->plane.normal.x * pr->center.x +
                             pr->plane.normal.y * pr->center.y +
                             pr->plane.normal.z * pr->center.z - pr->plane.distance;
            if (cd > -0.5f && cd < 0.5f) {
                ++portal_on_plane;
            }
        }
        const int nsec = static_cast<int>(sdk::VisTree::sector_count().value_or(0));
        for (int i = 0; i < nsec; ++i) {
            const auto ns = sdk::VisTree::sector_neighbours(static_cast<size_t>(i));
            if (!ns.empty()) {
                ++sectors_with_neighbours;
            }
            for (const size_t n : ns) {
                ++neighbour_edges;
                const auto back = sdk::VisTree::sector_neighbours(n);
                for (const size_t b : back) {
                    if (b == static_cast<size_t>(i)) {
                        ++symmetric_edges;
                        break;
                    }
                }
            }
        }
        if (player_sector >= 0) {
            player_neighbours = static_cast<int>(
                sdk::VisTree::sector_neighbours(static_cast<size_t>(player_sector)).size());
        }
    }

    // THE MUZZLE, asked mechanically: not "which attachment is the weapon" but "what
    // mounted on me carries a socket called flash". Live that resolves the shotgun and
    // skips the engine\default.mdl placeholder beside it.
    //
    // AND THE CROSS-CHECK WORTH HAVING: the engine moves an attached object to its mount
    // point itself, so the weapon OBJECT's own position should already equal the hand
    // socket position this SDK composes independently. Two unrelated producers -- the
    // engine's attachment updater and our own composition -- landing on the same point is
    // real evidence the composition is right.
    bool muzzle_ok = false, muzzle_clean = false;
    float muzzle[3] = {0, 0, 0};
    float weapon_vs_hand = -1.0f, muzzle_from_hand = -1.0f;
    std::string muzzle_mdl;
    if (player.has_value()) {
        if (const auto m = sdk::attached_socket(player->object, "flash"); m.has_value()) {
            muzzle_ok = true;
            muzzle_clean = !m->transform.stale;
            muzzle[0] = m->transform.position.x;
            muzzle[1] = m->transform.position.y;
            muzzle[2] = m->transform.position.z;
            for (const char ch : sdk::model_filename(m->object).value_or(std::string{})) {
                muzzle_mdl += (ch == '\\') ? "\\\\" : std::string(1, ch);
            }
            // And how far the muzzle sits from the hand holding it -- a barrel length,
            // which is the sanity bound on the socket offset having been applied in the
            // bone's frame rather than raw.
            if (hands_ok) {
                const float mx = muzzle[0] - rhand[0];
                const float my = muzzle[1] - rhand[1];
                const float mz = muzzle[2] - rhand[2];
                muzzle_from_hand = std::sqrt(mx * mx + my * my + mz * mz);
            }
            // The weapon's own origin against the hand socket we composed above.
            if (hands_ok) {
                if (const auto wi = sdk::object_info(m->object); wi.has_value()) {
                    const float ax = wi->position.x - rhand[0];
                    const float ay = wi->position.y - rhand[1];
                    const float az = wi->position.z - rhand[2];
                    weapon_vs_hand = std::sqrt(ax * ax + ay * ay + az * az);
                }
            }
        }
    }
    // ROUND TRIP, and deliberately not against a hardcoded name: take variables the
    // table itself reported, upper-case them, and require the lookup to find each one
    // with the same value. That tests the case-insensitive path without assuming which
    // variables a given build registers. Capped at 8 because each lookup re-walks all
    // 128 buckets.
    size_t convar_roundtrip = 0, convar_probed = 0;
    for (const auto& cv : convars) {
        if (convar_probed >= 8) {
            break;
        }
        if (cv.name.empty()) {
            continue;
        }
        ++convar_probed;
        std::string up = cv.name;
        for (auto& c : up) {
            c = static_cast<char>(::toupper(static_cast<unsigned char>(c)));
        }
        const auto hit = sdk::Engine::console_var(up.c_str());
        if (hit.has_value() && hit->name == cv.name && hit->value == cv.value) {
            ++convar_roundtrip;
        }
    }

    // start_shell_list_count is std::optional: nullopt means the SDK's own
    // walk did NOT terminate (corrupt list / wrong mapping) or faulted.
    // Reported as -1 so a consumer can distinguish "didn't terminate" from
    // "terminated with 0 entries" WITHOUT restating the SDK's internal cap
    // (see that method's comment -- the cap must not leak outside the SDK).
    const std::optional<size_t> shell_count =
        client_mgr != nullptr ? client_mgr->start_shell_list_count() : std::optional<size_t>{};

    // Zero when D3D is not up, which is a real state rather than a fault -- both records
    // are static storage and read as zeros before creation.
    const auto dmode = sdk::Render::display_mode().value_or(D3DDISPLAYMODE{});
    const auto pparams =
        sdk::Render::present_params().value_or(D3DPRESENT_PARAMETERS{});
    const auto caps = sdk::Render::device_caps();

    const int written = snprintf(buf, sizeof(buf),
             "{\"ok\":true,\"exe_base\":\"0x%08" PRIXPTR "\",\"exe_size\":\"0x%08" PRIXPTR "\","
             "\"client_mgr_update\":\"0x%08" PRIXPTR "\",\"client_shell_update\":\"0x%08" PRIXPTR "\","
             "\"get_engine_hook\":\"0x%08" PRIXPTR "\",\"g_pClientMgr_slot\":\"0x%08" PRIXPTR "\","
             "\"hWnd_slot\":\"0x%08" PRIXPTR "\",\"client_mgr\":\"0x%08" PRIXPTR "\","
             "\"client_shell\":\"0x%08" PRIXPTR "\",\"main_hwnd\":\"0x%08" PRIXPTR "\","
             "\"database_mgr\":\"0x%08" PRIXPTR "\","
             "\"client_mgr_updating\":%s,\"counter_elapsed_ms\":%u,"
             "\"counter_elapsed_time\":%f,\"start_shell_list_count\":%lld,"
             "\"counter_node_registered\":%s,"
             "\"last_sample_time_ms\":%u,\"pending_shell_release\":%s,"
             // The SAME clock by a different road: these come from calling the
             // engine's own accessors, while counter_elapsed_* above are field reads
             // through CClientMgr. Two independent paths to one value, so a
             // disagreement means one of them is wrong.
             "\"engine_time_seconds\":%f,\"engine_time_ms\":%u,\"engine_time_ok\":%s,"
             // The shell's two clocks and the global force. shell_real is the one that
             // KEEPS RUNNING while the game is paused -- the distinction a VR mod
             "\"shell_game_time\":%f,\"shell_real_time\":%f,\"shell_clocks_ok\":%s,"
             "\"local_client_count\":%d,\"local_client_0\":%d,\"frame_interval\":%f,"
             "\"global_force\":[%f,%f,%f],\"global_force_ok\":%s,"
             // The console-variable table, walked the way a mod would. A NAMED probe is
             // included because "the table has entries" and "I can find the one I want"
             // are different claims.
             "\"convar_count\":%zu,\"convar_named\":%zu,\"convar_probed\":%zu,"
             "\"convar_roundtrip\":%zu,"
             // The local player: how many slots are filled, whether the two routes to
             // slot 0 agree, and what the object says it is.
             "\"player_count\":%d,\"player_ok\":%s,\"player_handle\":%d,"
             "\"player_routes_agree\":%s,\"player_mdl\":\"%s\","
             // The VR chain's answer: the player's two hands, in world space.
             "\"player_sockets\":%d,\"hands_ok\":%s,\"hands_clean\":%s,"
             "\"hands_distinct\":%s,\"hands_reach\":%.2f,"
             "\"lhand\":[%.2f,%.2f,%.2f],\"rhand\":[%.2f,%.2f,%.2f],"
             // The muzzle, and the engine-vs-us agreement on where the weapon sits.
             "\"muzzle_ok\":%s,\"muzzle_clean\":%s,\"muzzle\":[%.2f,%.2f,%.2f],"
             "\"muzzle_mdl\":\"%s\",\"weapon_vs_hand\":%.3f,\"muzzle_from_hand\":%.2f,"
             // Where the player is in the world's own spatial terms.
             "\"sector_total\":%d,\"sector_candidates\":%d,\"sector_brute\":%d,\"sec_read_ok\":%d,\"sec_with_planes\":%d,\"sec_plane_total\":%d,"
             "\"sec_planed\":%d,\"sec_centre_in\":%d,"
             "\"sec_plane_probed\":%d,\"sec_plane_pos\":%d,\"sec_plane_neg\":%d,"
             "\"region_probes\":%d,\"region_agree\":%d,\"region_hits\":%d,"
             "\"box_probes\":%d,\"box_agree\":%d,\"box_hits\":%d,"
             "\"code_probed\":%d,\"code_current\":%d,"
             "\"contain_probes\":%d,\"contain_ok\":%d,\"contain_sphere\":%d,"
             "\"rec_objects\":%d,\"rec_with_entries\":%d,\"rec_match\":%d,"
             "\"rec_entries\":%d,\"rec_count_ok\":%d,"
             "\"rev_probed\":%d,\"rev_ok\":%d,\"rev_pairs\":%d,"
             "\"shape_probed\":%d,\"shape_agree\":%d,"
             "\"gate_rend\":%d,\"gate_rend_match\":%d,\"gate_norend\":%d,"
             "\"gate_norend_match\":%d,\"gate_norend_empty\":%d,"
             "\"rec_missing\":%d,\"rec_extra\":%d,\"rec_only_missing\":%d,"
             "\"rec_only_extra\":%d,\"rec_both\":%d,\"rec_consistent\":%d,"
             "\"sec_idx_probed\":%d,\"sec_idx_ok\":%d,\"sec_links_probed\":%d,"
             "\"sec_links_ok\":%d,\"sec_portal_sum\":%d,\"sec_portal_listed\":%d,"
             "\"poly_probed\":%d,\"poly_len_ok\":%d,\"poly_on_plane\":%d,"
             "\"poly_trunc\":%d,\"poly_verts\":%d,"
             "\"rch_probed\":%d,\"rch_1hop_ok\":%d,\"rch_mono_ok\":%d,"
             "\"rch_sym_probed\":%d,\"rch_sym_ok\":%d,\"rch_comp_ok\":%d,"
             "\"rch_comp_size\":%d,\"rch_hops_ok\":%d,"
             "\"wb_probed\":%d,\"wb_agree\":%d,\"wb_outside\":%d,\"wb_inside\":%d,"
             "\"wb_bounds_probed\":%d,\"wb_bounds_ok\":%d,"
             "\"world_printable\":%d,\"world_len\":%d,"
             "\"wb_loaded\":%d,\"wb_srv_probed\":%d,\"wb_srv_expanded\":%d,"
             "\"lp_slots\":%d,\"lp_consistent\":%d,\"lp_accepted\":%d,"
             "\"dev_vt\":\"0x%08zX\",\"dev_vt_writable\":%d,\"dev_vt_outside_d3d9\":%d,\"gcs_ok\":%d,\"gcs_anchors\":%d,\"gcs_pre_empty\":%d,\"gcs_shapes\":%d,\"gcs_entry_agrees\":%d,\"bp_raw_edge\":%.4f,\"bp_inv_edge\":%.4f,\"bp_edges\":%d,\"bp_rt_ok\":%d,\"bp_rt_n\":%d,\"bp_rt_worst\":%.5f,\"bp_rt_worst_mag\":%.2f,\"bp_eng_calls\":%d,\"bp_eng_rc_ok\":%d,\"bp_eng_match\":%d,\"bp_eng_worst\":%.5f,\"bp_reject_oor\":%d,"
             "\"wb_obj_gap\":%d,\"wb_class_size\":%d,"
             "\"wb_inst\":[%.3f,%.3f,%.3f,%.3f,%.3f,%.3f],"
             "\"wb_glob\":[%.3f,%.3f,%.3f,%.3f,%.3f,%.3f],"
             "\"player_sector\":%d,\"brute_sector\":%d,\"portal_total\":%d,\"portal_both_sectors\":%d,"
             "\"portal_on_plane\":%d,\"sectors_with_neighbours\":%d,"
             "\"neighbour_edges\":%d,\"symmetric_edges\":%d,\"player_neighbours\":%d,"
             // THE D3D9 SIDE. Both interfaces, and for each the module that IMPLEMENTS
             // its methods -- which differs: the factory is a Steam-overlay proxy while
             // the device is the real runtime.
             "\"d3d9\":\"0x%08" PRIXPTR "\",\"d3d9_impl\":\"%s\","
             "\"device\":\"0x%08" PRIXPTR "\",\"device_impl\":\"%s\","
             // device_type is D3DCAPS9's FIRST field. The three after it are chosen to be
             // FAR from the base -- MaxTextureWidth in the middle, the two shader versions
             // near the very end -- so that a wrong base or a wrong struct size shows up.
             // The leading fields alone would still look right.
             "\"device_type\":%u,\"caps_max_tex_w\":%u,"
             "\"caps_vs\":\"0x%08X\",\"caps_ps\":\"0x%08X\","
             // The desktop mode the engine recorded, and what it is actually presenting.
             "\"display_w\":%u,\"display_h\":%u,\"display_hz\":%u,\"display_fmt\":%u,"
             "\"bb_w\":%u,\"bb_h\":%u,\"bb_fmt\":%u,\"bb_count\":%u,"
             "\"windowed\":%s,\"swap_effect\":%u,\"depth_fmt\":%u}",
             static_cast<uintptr_t>(exe->base), static_cast<uintptr_t>(exe->size),
             sdk::CClientMgr::update_fn(),
             sdk::CClientShell::update_fn(),
             sdk::Engine::get_engine_hook_fn(),
             sdk::CClientMgr::instance_slot(),
             sdk::Engine::main_hwnd_slot(),
             reinterpret_cast<uintptr_t>(client_mgr),
             reinterpret_cast<uintptr_t>(sdk::CClientShell::get()),
             reinterpret_cast<uintptr_t>(sdk::Engine::main_hwnd()),
             reinterpret_cast<uintptr_t>(sdk::DatabaseMgr::get()),
             (client_mgr != nullptr && client_mgr->is_updating()) ? "true" : "false",
             client_mgr != nullptr ? client_mgr->counter_elapsed_ms() : 0u,
             client_mgr != nullptr ? client_mgr->counter_elapsed_time() : 0.0,
             shell_count.has_value() ? static_cast<long long>(*shell_count) : -1LL,
             (client_mgr != nullptr && client_mgr->counter_node_registered()) ? "true" : "false",
             client_mgr != nullptr ? client_mgr->last_sample_time_ms() : 0u,
             (client_mgr != nullptr && client_mgr->has_pending_shell_release()) ? "true" : "false",
             // ONE call, not three: sampling the clock per format argument would let
             // it advance mid-line and print a seconds/ms pair that never coexisted.
             engine_time.value_or(sdk::Engine::ClientTime{}).seconds,
             engine_time.value_or(sdk::Engine::ClientTime{}).milliseconds,
             engine_time.has_value() ? "true" : "false",
             shell_game.value_or(0.0), shell_real.value_or(0.0),
             (shell_game.has_value() && shell_real.has_value()) ? "true" : "false",
             static_cast<int>(sdk::CClientShell::local_client_count().value_or(0)),
             // -1 for "no local client in slot 0", which is a real state (no shell,
             // or an empty slot) and must not read as id 0.
             sdk::CClientShell::local_client_id(0).has_value()
                 ? static_cast<int>(*sdk::CClientShell::local_client_id(0))
                 : -1,
             static_cast<double>(sdk::CClientShell::frame_interval_seconds().value_or(0.0f)),
             force.value_or(sdk::Engine::ForceVector{}).x,
             force.value_or(sdk::Engine::ForceVector{}).y,
             force.value_or(sdk::Engine::ForceVector{}).z,
             force.has_value() ? "true" : "false",
             convars.size(), convars_named, convar_probed, convar_roundtrip,
             static_cast<int>(player_n.value_or(0)),
             player.has_value() ? "true" : "false",
             player.has_value() ? static_cast<int>(player->handle) : -1,
             player_routes_agree ? "true" : "false",
             player_mdl.c_str(),
             player_sockets,
             hands_ok ? "true" : "false",
             hands_clean ? "true" : "false",
             hands_distinct ? "true" : "false", hands_reach,
             lhand[0], lhand[1], lhand[2],
             rhand[0], rhand[1], rhand[2],
             muzzle_ok ? "true" : "false",
             muzzle_clean ? "true" : "false",
             muzzle[0], muzzle[1], muzzle[2],
             muzzle_mdl.c_str(), weapon_vs_hand, muzzle_from_hand,
             sector_total, sector_candidates, sector_brute,
             sec_read_ok, sec_with_planes, sec_plane_total, sec_planed, sec_centre_in,
             sec_plane_probed, sec_plane_pos, sec_plane_neg,
             region_probes, region_agree, region_hits,
             box_probes, box_agree, box_hits, code_probed, code_current,
             contain_probes, contain_ok, contain_sphere,
             rec_objects, rec_with_entries, rec_match, rec_entries, rec_count_ok,
             rev_probed, rev_ok, rev_pairs, shape_probed, shape_agree,
             gate_rend, gate_rend_match, gate_norend, gate_norend_match, gate_norend_empty,
             rec_missing, rec_extra, rec_only_missing, rec_only_extra, rec_both,
             rec_consistent, sec_idx_probed, sec_idx_ok, sec_links_probed,
             sec_links_ok, sec_portal_sum, sec_portal_listed,
             poly_probed, poly_len_ok, poly_on_plane, poly_trunc, poly_verts,
             rch_probed, rch_1hop_ok, rch_mono_ok, rch_sym_probed, rch_sym_ok,
             rch_comp_ok, rch_comp_size, rch_hops_ok,
             wb_probed, wb_agree, wb_outside, wb_inside, wb_bounds_probed, wb_bounds_ok,
             wp_printable, wp_len, wb_loaded, wb_srv_probed, wb_srv_expanded,
             lp_slots, lp_consistent, lp_accepted, static_cast<size_t>(dev_vt), dev_vt_writable, dev_vt_outside_d3d9, gcs_ok, gcs_in_module, gcs_pre_empty, gcs_shapes, gcs_entry_agrees, raw_edge, inv_edge, edge_n, inv_roundtrip_ok, inv_roundtrip_n, rt_worst, rt_worst_mag, eng_calls, eng_rc_ok, eng_match, eng_worst, bp_reject_oor,
             wb_obj_gap, wb_class_size,
             wb_inst[0], wb_inst[1], wb_inst[2], wb_inst[3], wb_inst[4], wb_inst[5],
             wb_glob[0], wb_glob[1], wb_glob[2], wb_glob[3], wb_glob[4], wb_glob[5],
             player_sector,
             brute_sector, portal_total, portal_both_sectors, portal_on_plane,
             sectors_with_neighbours, neighbour_edges, symmetric_edges,
             player_neighbours,
             reinterpret_cast<uintptr_t>(sdk::Render::d3d9()),
             sdk::Render::interface_impl_owner(sdk::Render::d3d9())
                 .value_or(std::string{"(none)"}).c_str(),
             reinterpret_cast<uintptr_t>(sdk::Render::device()),
             sdk::Render::interface_impl_owner(sdk::Render::device())
                 .value_or(std::string{"(none)"}).c_str(),
             caps.has_value() ? static_cast<unsigned>(caps->DeviceType) : 0u,
             caps.has_value() ? static_cast<unsigned>(caps->MaxTextureWidth) : 0u,
             caps.has_value() ? static_cast<unsigned>(caps->VertexShaderVersion) : 0u,
             caps.has_value() ? static_cast<unsigned>(caps->PixelShaderVersion) : 0u,
             dmode.Width, dmode.Height, dmode.RefreshRate,
             static_cast<unsigned>(dmode.Format),
             pparams.BackBufferWidth, pparams.BackBufferHeight,
             static_cast<unsigned>(pparams.BackBufferFormat), pparams.BackBufferCount,
             pparams.Windowed ? "true" : "false",
             static_cast<unsigned>(pparams.SwapEffect),
             static_cast<unsigned>(pparams.AutoDepthStencilFormat));
    // A truncated payload is not valid JSON, so say so in JSON the caller CAN parse
    // rather than handing back a half-written object.
    if (written < 0 || static_cast<size_t>(written) >= sizeof(buf)) {
        return "{\"ok\":false,\"error\":\"targets truncated\",\"needed\":" +
               std::to_string(written) + "}";
    }
    // Splice the escaped string fields in after the '{' rather than through the format string.
    return "{" + world_json + std::string(buf + 1);
}

// /sdk/models -- written the way a MOD would use the SDK, not the way the test
// suite does. Everything here goes through the public sdk::Model API: no offsets,
// no schema types, no engine pointers. It is the smallest thing that answers the
// questions a VR mod starts with -- what is this object, and where is its head?
//
// It also serves as the API's own smoke test: if sdk::ModelSkeleton stops
// resolving, or find_node stops matching, this endpoint says so in plain text
// rather than a count going from 34 to 33.
std::string build_models_json() {
    auto* mgr = sdk::CClientMgr::get();
    if (mgr == nullptr) {
        return "{\"ok\":false,\"error\":\"CClientMgr::get() returned null\"}";
    }

    // Names a mod would plausibly look for. "Head" is the camera attachment point;
    // the hand nodes are where a motion controller would go. Which of these a given
    // model has is scene-dependent, so they are reported, not required.
    static constexpr const char* kWanted[] = {"Head", "L_Hand", "R_Hand"};

    std::string out = "{\"ok\":true,\"models\":[";
    size_t emitted = 0, with_skeleton = 0, resolved_wanted = 0;

    // Snapshot first, then work from the copies -- the same discipline the object
    // report uses. Note what this does NOT buy: the addresses are still live
    // pointers, so an object destroyed between the snapshot and the read would be
    // dereferenced here. That is precisely why every sdk::Model read is SEH-guarded
    // and returns nullopt rather than trusting the caller to be lucky.
    std::vector<sdk::CClientMgr::ObjectSnapshot> snaps(2048);
    const auto taken = mgr->snapshot_objects(static_cast<sdk::ObjectType>(1), snaps.data(),
                                             snaps.size());
    if (!taken.has_value()) {
        return "{\"ok\":false,\"error\":\"snapshot_objects failed for OT_MODEL\"}";
    }

    for (size_t si = 0; si < *taken; ++si) {
        const auto* obj = reinterpret_cast<const regenny::LTObject*>(snaps[si].address);
        const auto skel = sdk::ModelSkeleton::from_object(obj);
        if (skel.has_value()) {
            ++with_skeleton;
        }
        // Cap the emitted list: a mod does not need 215 entries to identify itself,
        // and the interesting ones are those carrying the nodes we asked about.
        bool interesting = false;
        std::string nodes_json = "[";
        if (skel.has_value()) {
            for (const char* want : kWanted) {
                const auto idx = skel->find_node(want);
                if (!idx.has_value()) {
                    continue;
                }
                interesting = true;
                ++resolved_wanted;
                // Round-trip the lookup: the name we searched for must come back out
                // of the index we were handed. That is what proves find_node agrees
                // with node_name rather than both being independently plausible.
                const auto back = skel->node_name(*idx);
                const auto parent = skel->parent_of(*idx);
                const auto chain = skel->path_to_root(*idx);
                const auto pose = skel->inverse_bind_pose(*idx);
                char nb[512];
                snprintf(nb, sizeof(nb),
                         "%s{\"asked\":\"%s\",\"index\":%zu,\"name\":\"%s\",\"round_trip\":%s,"
                         "\"parent\":%d,\"depth\":%d,\"pos_a\":[%.3f,%.3f,%.3f]}",
                         nodes_json.size() > 1 ? "," : "", want, *idx,
                         back.has_value() ? back->c_str() : "",
                         (back.has_value() && *back == want) ? "true" : "false",
                         parent.has_value() ? static_cast<int>(*parent) : -1,
                         chain.has_value() ? static_cast<int>(chain->size()) : -1,
                         pose.has_value() ? pose->position.x : 0.0f,
                         pose.has_value() ? pose->position.y : 0.0f,
                         pose.has_value() ? pose->position.z : 0.0f);
                nodes_json += nb;
            }
        }
        nodes_json += "]";
        if (!interesting || emitted >= 12) {
            continue;
        }

        const auto file = sdk::model_filename(obj);
        const auto mats = sdk::model_materials(obj);
        std::string entry = emitted == 0 ? "" : ",";
        entry += "{\"file\":\"";
        // Backslashes in a .mdl path have to be escaped to keep the JSON valid.
        if (file.has_value()) {
            for (const char ch : *file) {
                if (ch == '\\') {
                    entry += "\\\\";
                } else {
                    entry += ch;
                }
            }
        }
        char tail[128];
        snprintf(tail, sizeof(tail), "\",\"nodes\":%zu,\"materials\":%d,\"found\":",
                 skel->node_count(), mats.has_value() ? static_cast<int>(mats->size()) : -1);
        entry += tail;
        entry += nodes_json;
        entry += "}";
        out += entry;
        ++emitted;
    }

    // The BONE MATRIX PALETTE and ANIMATION STATE, both through the public API. The
    // palette is reported as a POPULATED count, never asserted full: it is per-frame
    // render state filled during the draw, so on an idle frame most slots are
    // legitimately zero. The animation numbers have real invariants though -- the
    // index is bounded by the asset's table and the fraction is normalised -- so
    // those are counted for assertion.
    size_t bone_slots = 0, bone_slots_live = 0, models_with_palette = 0;
    size_t anim_ok = 0, anim_index_in_range = 0, anim_frac_in_range = 0, anim_blending = 0;
    size_t anim_nodes_in_range = 0, anim_nodes_named = 0, anim_nodes_ordered = 0;
    size_t anim_named = 0, piece_answers = 0, piece_hidden = 0;
    size_t piece_counts = 0, piece_named = 0, piece_roundtrip = 0;
    size_t sock_xform_ok = 0, sock_xform_stale = 0, sock_xform_unit = 0;
    size_t sock_xform_finite = 0, sock_xform_clean = 0;
    size_t sock_xform_nonfinite_stale = 0, sock_xform_nonfinite_clean = 0;
    size_t sock_usable = 0, sock_usable_probed = 0;
    int ilt_slot_ok = -1;
    size_t engine_xf_probed = 0, engine_xf_ok = 0, engine_xf_agree = 0;
    float engine_xf_worst = 0.0f, engine_xf_worst_local = 0.0f;
    size_t engine_xf_agree_local = 0;
    size_t engine_f1_ok = 0, engine_f1_agree_world = 0, engine_f1_agree_local = 0;
    size_t sock_camera_measured = 0, sock_camera_above = 0;
    float sock_xform_max_dist = 0.0f, sock_camera_max_height = 0.0f;
    for (size_t si = 0; si < *taken; ++si) {
        const auto* obj = reinterpret_cast<const regenny::LTObject*>(snaps[si].address);
        const auto skel = sdk::ModelSkeleton::from_object(obj);
        if (!skel.has_value()) {
            continue;
        }
        bool any = false;
        for (size_t i = 0; i < skel->node_count(); ++i) {
            const auto mat = skel->bone_matrix(i);
            if (!mat.has_value()) {
                continue;
            }
            any = true;
            ++bone_slots;
            if (sdk::ModelSkeleton::is_populated(*mat)) {
                ++bone_slots_live;
            }
        }
        if (any) {
            ++models_with_palette;
        }

        // Animation state, same object, through the public accessors. The two
        // invariants a consumer can rely on are counted here: the index stays inside
        // the asset's animation table, and the fraction stays normalised.
        const auto anim = sdk::model_anim_state(obj);
        const auto anim_n = sdk::model_anim_count(obj);
        if (anim.has_value() && anim_n.has_value()) {
            ++anim_ok;
            if (*anim_n > 0 && anim->index < *anim_n && anim->current < *anim_n) {
                ++anim_index_in_range;
            }
            if (anim->fraction >= 0.0f && anim->fraction <= 1.0f) {
                ++anim_frac_in_range;
            }
            if (anim->index != anim->current) {
                ++anim_blending;
            }
            // THE NAME, which is what a mod actually reacts to. Resolved through the
            // engine's own chain, so if this stops answering the mapping moved.
            if (const auto nm = sdk::model_current_anim_name(obj);
                nm.has_value() && !nm->empty()) {
                ++anim_named;
            }
            // Piece visibility and NAMES, asked the way a mod does: enumerate the
            // pieces the model reports, name each one, and require the name to find
            // its own index again. The sweep to 64 also proves the accessor enforces
            // the engine's piece bound rather than the mask's width.
            const auto pcount = sdk::model_piece_count(obj);
            if (pcount.has_value()) {
                piece_counts += *pcount;
            }
            for (size_t pi = 0; pi < 64; ++pi) {
                const auto h = sdk::model_piece_hidden(obj, pi);
                if (!h.has_value()) {
                    continue;
                }
                ++piece_answers;
                if (*h) {
                    ++piece_hidden;
                }
                if (const auto nm = sdk::model_piece_name(obj, pi);
                    nm.has_value() && !nm->empty()) {
                    ++piece_named;
                    // Case-insensitive round trip through the engine's own comparison,
                    // using the piece's OWN name upper-cased so the check does not
                    // depend on which assets a level loaded.
                    std::string up = *nm;
                    for (auto& c : up) {
                        c = static_cast<char>(::toupper(static_cast<unsigned char>(c)));
                    }
                    if (sdk::model_find_piece(obj, up.c_str()) == pi) {
                        ++piece_roundtrip;
                    }
                }
            }
            // THE SLOT GUARD, asked through the SDK rather than reimplemented here -- the whole point
    // of putting it in the header is that a consumer checks it once instead of open-coding a
    // vtable read at each site.
    if (ilt_slot_ok < 0) {
        ilt_slot_ok = sdk::ModelSkeleton::engine_socket_transform_available() ? 1 : 0;
    }
    // SOCKET WORLD TRANSFORMS. The interesting validation is not that the
            // numbers are finite -- it is that they point the right WAY. A wrong
            // quaternion sign convention preserves length (any Hamilton-shaped product
            // does), so a norm check cannot catch it; putting the head below the feet
            // can. Gravity reads (0, -980, 0), so +Y is up, and a character's `camera`
            // socket sits at local Y = +13.7 -- it must come out ABOVE the object.
            if (const auto sk2 = sdk::ModelSkeleton::from_object(obj); sk2.has_value()) {
                for (size_t si = 0; si < sk2->socket_count(); ++si) {
                    const auto wt = sk2->socket_world_transform(si);
                    if (!wt.has_value()) {
                        continue;
                    }
                    ++sock_xform_ok;
                    if (wt->stale) {
                        ++sock_xform_stale;
                    }
                    // Unit rotation: composing two unit quaternions must stay unit.
                    const float qn = wt->rotation.x * wt->rotation.x +
                                     wt->rotation.y * wt->rotation.y +
                                     wt->rotation.z * wt->rotation.z +
                                     wt->rotation.w * wt->rotation.w;
                    if (qn > 0.98f && qn < 1.02f) {
                        ++sock_xform_unit;
                    }
                    // THE ENGINE'S OWN ANSWER vs the composition, on CLEAN sockets only. That
                    // restriction is not caution for its own sake: on a stale node the engine
                    // EVALUATES the skeleton and clears the flag, which is a mutation and belongs
                    // on the game thread. On a clean one the dirty check short-circuits and the
                    // call is a pure read, which is exactly the population worth comparing.
                    if (!wt->stale) {
                        ++engine_xf_probed;
                        if (const auto ex = sk2->engine_socket_transform(si, 0); ex.has_value()) {
                            ++engine_xf_ok;
                            // WHICH SPACE did the engine hand back? Compare against BOTH of this
                            // SDK's answers rather than assuming: the local (bone-cache) pose and
                            // the world pose. Whichever matches names the convention, and a
                            // mismatch against both would mean something else is wrong.
                            const auto lc = sk2->socket_transform(si);
                            // AND THE OTHER FLAG VALUE, to see whether it selects world space.
                            if (const auto ex1 = sk2->engine_socket_transform(si, 1);
                                ex1.has_value()) {
                                ++engine_f1_ok;
                                const float ax = ex1->position.x - wt->position.x;
                                const float ay = ex1->position.y - wt->position.y;
                                const float az = ex1->position.z - wt->position.z;
                                if (std::sqrt(ax * ax + ay * ay + az * az) < 0.05f) {
                                    ++engine_f1_agree_world;
                                }
                                if (lc.has_value()) {
                                    const float bx = ex1->position.x - lc->position.x;
                                    const float by = ex1->position.y - lc->position.y;
                                    const float bz = ex1->position.z - lc->position.z;
                                    if (std::sqrt(bx * bx + by * by + bz * bz) < 0.05f) {
                                        ++engine_f1_agree_local;
                                    }
                                }
                            }
                            const float dxw = ex->position.x - wt->position.x;
                            const float dyw = ex->position.y - wt->position.y;
                            const float dzw = ex->position.z - wt->position.z;
                            const float dw = std::sqrt(dxw * dxw + dyw * dyw + dzw * dzw);
                            if (dw > engine_xf_worst) {
                                engine_xf_worst = dw;
                            }
                            if (dw < 0.05f) {
                                ++engine_xf_agree;
                            }
                            if (lc.has_value()) {
                                const float dxl = ex->position.x - lc->position.x;
                                const float dyl = ex->position.y - lc->position.y;
                                const float dzl = ex->position.z - lc->position.z;
                                const float dl = std::sqrt(dxl * dxl + dyl * dyl + dzl * dzl);
                                if (dl > engine_xf_worst_local) {
                                    engine_xf_worst_local = dl;
                                }
                                if (dl < 0.05f) {
                                    ++engine_xf_agree_local;
                                }
                            }
                        }
                    }
                    // THE CONSUMER PATH: one call that answers "can I apply this pose?".
                    if (sdk::ModelSkeleton::from_object(obj).has_value()) {
                        const auto usable = sk2->socket_world_transform_is_usable(si);
                        if (usable.has_value()) {
                            ++sock_usable_probed;
                            if (*usable) {
                                ++sock_usable;
                            }
                        }
                    }
                    const bool finite_pos =
                        std::isfinite(wt->position.x) && std::isfinite(wt->position.y) &&
                        std::isfinite(wt->position.z);
                    if (finite_pos) {
                        ++sock_xform_finite;
                    } else if (wt->stale) {
                        // Non-finite AND stale: the bone cache was never evaluated, so the
                        // numbers are whatever the allocation held.
                        ++sock_xform_nonfinite_stale;
                    } else {
                        // Non-finite while CLEAN would be a real problem -- a transform the SDK
                        // says is usable but is not.
                        ++sock_xform_nonfinite_clean;
                    }
                    // A CLEAN transform is the only one worth measuring geometrically.
                    // Two numbers come out: the distance from the object (bounded by
                    // how far a bone can be from the origin) and, for the `camera`
                    // socket specifically, its SIGNED height -- which must be positive
                    // if the composition is oriented correctly.
                    if (!wt->stale) {
                        const auto info = sdk::object_info(obj);
                        if (info.has_value()) {
                            const float dx = wt->position.x - info->position.x;
                            const float dy = wt->position.y - info->position.y;
                            const float dz = wt->position.z - info->position.z;
                            const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
                            if (d > sock_xform_max_dist) {
                                sock_xform_max_dist = d;
                            }
                            ++sock_xform_clean;
                            const auto nm2 = sk2->socket(si);
                            if (nm2.has_value() && nm2->name == "camera") {
                                ++sock_camera_measured;
                                if (dy > 0.0f) {
                                    ++sock_camera_above;
                                }
                                if (dy > sock_camera_max_height) {
                                    sock_camera_max_height = dy;
                                }
                            }
                        }
                    }
                }
            }
            // The consumer flow that makes these fields worth exposing: a track's
            // node index handed to the skeleton comes back as a BONE NAME. If that
            // resolves for every model, the index is usable as an index and not just
            // in-range as a number.
            if (skel.has_value()) {
                if (anim->node_a < skel->node_count() && anim->node_b < skel->node_count()) {
                    ++anim_nodes_in_range;
                }
                if (skel->node_name(anim->node_a).has_value() &&
                    skel->node_name(anim->node_b).has_value()) {
                    ++anim_nodes_named;
                }
                if (anim->node_b >= anim->node_a) {
                    ++anim_nodes_ordered;
                }
            }
        }
    }

    // A HANDLE ROUND-TRIP over every model, done the way a mod must: take the
    // handle an object carries, hand it back to the engine's own table, and require
    // the same object out. This is the conversion every ILT* call depends on, so it
    // is worth proving rather than assuming -- and it is checked here, at the API,
    // rather than by re-deriving the table layout host-side.
    size_t handles_seen = 0, handles_round_trip = 0, handles_absent = 0;
    for (size_t si = 0; si < *taken; ++si) {
        const auto* obj = reinterpret_cast<const regenny::LTObject*>(snaps[si].address);
        const auto h = mgr->handle_of(obj);
        if (!h.has_value()) {
            ++handles_absent;  // legal: 335 of 3583 live objects carry no handle
            continue;
        }
        ++handles_seen;
        if (mgr->object_from_handle(*h) == obj) {
            ++handles_round_trip;
        }
    }

    // And a search the way a mod identifies what it cares about: by path substring.
    // "weapons" is a stable directory in this game's asset tree, so it is a fair
    // demonstration without pinning an exact filename that a level change would move.
    const auto weapons = sdk::find_models("weapons", 32);
    const auto everything = sdk::find_models("", 4096);

    // snprintf TRUNCATES rather than overflowing, which is safe for memory and
    // terrible for a JSON consumer: the reply parses as an unterminated string and
    // the failure looks like a transport bug. It returns the length it WANTED, so a
    // buffer that grew past its literal is caught here instead of downstream.
    char sum[1536];
    const int want = snprintf(sum, sizeof(sum),
             "],\"model_objects\":%zu,\"with_skeleton\":%zu,\"wanted_resolved\":%zu,\"listed\":%zu,"
             "\"handles_seen\":%zu,\"handles_round_trip\":%zu,\"handles_absent\":%zu,"
             "\"handle_table_slots\":%zu,\"found_weapons\":%zu,\"found_all\":%zu,"
             "\"bone_slots\":%zu,\"bone_slots_live\":%zu,\"models_with_palette\":%zu,"
             "\"anim_ok\":%zu,\"anim_index_in_range\":%zu,\"anim_frac_in_range\":%zu,"
             "\"anim_blending\":%zu,\"anim_nodes_in_range\":%zu,"
             "\"anim_nodes_named\":%zu,\"anim_nodes_ordered\":%zu,"
             "\"anim_named\":%zu,\"piece_answers\":%zu,\"piece_hidden\":%zu,"
             "\"piece_counts\":%zu,\"piece_named\":%zu,\"piece_roundtrip\":%zu,"
             "\"sock_xform_ok\":%zu,\"sock_xform_stale\":%zu,\"sock_xform_unit\":%zu,"
             "\"sock_xform_finite\":%zu,\"sock_xform_clean\":%zu,"
             "\"sock_nf_stale\":%zu,\"sock_nf_clean\":%zu,"
             "\"sock_usable\":%zu,\"sock_usable_probed\":%zu,"
             "\"ilt_slot_ok\":%d,\"engine_gate\":%lld,\"engine_rc\":%lld,"
             "\"engine_xf_probed\":%zu,\"engine_xf_ok\":%zu,"
             "\"engine_xf_agree\":%zu,\"engine_xf_worst\":%.4f,"
             "\"engine_xf_agree_local\":%zu,\"engine_xf_worst_local\":%.4f,"
             "\"engine_f1_ok\":%zu,\"engine_f1_world\":%zu,\"engine_f1_local\":%zu,"
             "\"sock_xform_max_dist\":%.2f,\"sock_camera_measured\":%zu,"
             "\"sock_camera_above\":%zu,\"sock_camera_max_height\":%.2f}",
             *taken, with_skeleton, resolved_wanted, emitted, handles_seen, handles_round_trip,
             handles_absent, mgr->handle_table_size().value_or(0), weapons.size(),
             everything.size(), bone_slots, bone_slots_live, models_with_palette, anim_ok,
             anim_index_in_range, anim_frac_in_range, anim_blending, anim_nodes_in_range,
             anim_nodes_named, anim_nodes_ordered, anim_named, piece_answers, piece_hidden,
             piece_counts, piece_named, piece_roundtrip, sock_xform_ok, sock_xform_stale,
             sock_xform_unit, sock_xform_finite, sock_xform_clean,
             sock_xform_nonfinite_stale, sock_xform_nonfinite_clean, sock_usable,
             sock_usable_probed, ilt_slot_ok,
             static_cast<long long>(sdk::ModelSkeleton::engine_iface_gate_byte()),
             static_cast<long long>(sdk::ModelSkeleton::last_engine_rc()),
             engine_xf_probed, engine_xf_ok,
             engine_xf_agree, static_cast<double>(engine_xf_worst), engine_xf_agree_local,
             static_cast<double>(engine_xf_worst_local), engine_f1_ok,
             engine_f1_agree_world, engine_f1_agree_local,
             static_cast<double>(sock_xform_max_dist), sock_camera_measured,
             sock_camera_above, static_cast<double>(sock_camera_max_height));
    if (want < 0 || static_cast<size_t>(want) >= sizeof(sum)) {
        // Say so in the payload rather than shipping half a field. A reader that
        // sees this knows the numbers are missing, not that the socket broke.
        out += "],\"error\":\"summary truncated\"}";
        return out;
    }
    out += sum;
    return out;
}

// Diagnostics only -- goes entirely through sdk::CClientMgr's own
// object_list_count()/object_count()/snapshot_objects(). Reports every
// type bucket's live count plus a bounded sample of copied-out object
// transforms.
//
// Uses the SNAPSHOT api deliberately: these lists mutate continuously while
// we read them, so the SDK copies each object's fields in the same guarded
// pass that walks the list, and we only ever format already-copied PODs
// here (see sdk::CClientMgr's object-enumeration comment).
std::string build_objects_json() {
    auto* mgr = sdk::CClientMgr::get();
    if (mgr == nullptr) {
        return "{\"ok\":false,\"error\":\"CClientMgr::get() returned null\"}";
    }

    // Bucket count comes from the schema via the SDK -- never a literal here.
    const size_t buckets = sdk::CClientMgr::object_list_count();

    std::string out = "{\"ok\":true,\"bucket_count\":";
    out += std::to_string(buckets);
    out += ",\"buckets\":[";

    size_t total = 0;
    bool all_terminated = true;
    for (size_t t = 0; t < buckets; ++t) {
        if (t != 0) {
            out += ",";
        }
        const auto n = mgr->object_count(static_cast<sdk::ObjectType>(t));
        if (!n.has_value()) {
            all_terminated = false;
            out += "-1"; // walk faulted or did not terminate
        } else {
            total += *n;
            out += std::to_string(*n);
        }
    }
    out += "],\"total\":";
    out += std::to_string(total);
    out += ",\"all_terminated\":";
    out += all_terminated ? "true" : "false";

    // Per-bucket type names, so the numbers above are readable without
    // cross-referencing the enum by hand.
    out += ",\"bucket_names\":[";
    for (size_t t = 0; t < buckets; ++t) {
        if (t != 0) {
            out += ",";
        }
        out += "\"";
        out += sdk::object_type_name(static_cast<sdk::ObjectType>(t));
        out += "\"";
    }
    out += "]";

    // Per-object-type allocator banks. The index is NOT the type (OT_LIGHT has
    // no bank), so this goes through bank_at() and reports the type each bank
    // actually serves rather than assuming array order.
    out += ",\"banks\":[";
    for (size_t i = 0; i < sdk::CClientMgr::object_bank_count(); ++i) {
        if (i != 0) {
            out += ",";
        }
        const auto b = mgr->bank_at(i);
        if (!b.has_value()) {
            out += "null";
            continue;
        }
        char bb[256];
        snprintf(bb, sizeof(bb),
                 "{\"index\":%zu,\"type\":%u,\"type_name\":\"%s\",\"pool\":\"0x%08" PRIXPTR "\","
                 "\"element_size\":%u,\"block_size\":%u,\"block_matches\":%s}",
                 i, static_cast<unsigned>(b->type), sdk::object_type_name(b->type), b->pool,
                 b->element_size, b->block_size,
                 (b->block_size == ((b->element_size + 8u) & ~7u)) ? "true" : "false");
        out += bb;
    }
    out += "]";

    // Cached transforms. The pair is WORLDMODEL state inherited by Camera, so
    // both types are reported: an earlier version walked only type 5, which is
    // precisely why the mis-attribution to LTCameraObject went unnoticed for
    // several passes. det_ok is the hard invariant on both; rotation_match and
    // inverse_ok are exact on cameras but lag on moving worldmodels, so the test
    // asserts them only for type 5.
    for (const size_t type : {size_t{2}, size_t{5}}) {
        out += type == 2 ? ",\"worldmodel_transforms\":" : ",\"camera_transforms\":";
        if (const auto tc = mgr->check_transforms(type, 8192); tc.has_value()) {
            char tb[192];
            snprintf(tb, sizeof(tb),
                     "{\"sampled\":%zu,\"rotation_match\":%zu,\"inverse_ok\":%zu,\"det_ok\":%zu}",
                     tc->sampled, tc->rotation_match, tc->inverse_ok, tc->det_ok);
            out += tb;
        } else {
            out += "null";
        }
    }

    // Schema size agreement. The engine's per-type element_size against our
    // sizeof for the class mapped onto that type -- both derived, neither a
    // baseline. Also that OT_LIGHT still has no bank, since it is uncreatable.
    out += ",\"schema_sizes\":";
    if (const auto ss = mgr->check_schema_sizes(); ss.has_value()) {
        char sb[160];
        snprintf(sb, sizeof(sb),
                 "{\"types_checked\":%zu,\"size_matches\":%zu,\"light_has_no_bank\":%s}",
                 ss->types_checked, ss->size_matches, ss->light_has_no_bank ? "true" : "false");
        out += sb;
    } else {
        out += "null";
    }

    // OT_MODEL's embedded list: a stored count against a walk, plus the two
    // routes to the asset pointer agreeing.
    out += ",\"model_lists\":";
    if (const auto ml = mgr->check_model_lists(8192); ml.has_value()) {
        char mb[384];
        snprintf(mb, sizeof(mb),
                 "{\"sampled\":%zu,\"count_matches_walk\":%zu,\"embedded_linked\":%zu,"
                 "\"asset_dup_agrees\":%zu,\"asset_present\":%zu,\"rotation_unit\":%zu,"
                 "\"max_members\":%zu,\"members_total\":%zu,\"member_asset_ok\":%zu}",
                 ml->sampled, ml->count_matches_walk, ml->embedded_linked, ml->asset_dup_agrees,
                 ml->asset_present, ml->rotation_unit, ml->max_members, ml->members_total,
                 ml->member_asset_ok);
        out += mb;
    } else {
        out += "null";
    }

    // Material names: an owned std::string array whose base, length and stride
    // all come from the engine's own teardown loop. Every count here is a string
    // checked against ITSELF -- terminator at [size], size within capacity.
    out += ",\"model_materials\":";
    if (const auto mc = mgr->check_model_materials(8192); mc.has_value()) {
        char cb[320];
        snprintf(cb, sizeof(cb),
                 "{\"models\":%zu,\"strings_total\":%zu,\"terminated\":%zu,"
                 "\"size_le_capacity\":%zu,\"capacity_sane\":%zu,"
                 "\"nonempty_printable\":%zu,\"max_count\":%zu}",
                 mc->models, mc->strings_total, mc->terminated, mc->size_le_capacity,
                 mc->capacity_sane, mc->nonempty_printable, mc->max_count);
        out += cb;
    } else {
        out += "null";
    }

    // The shared per-.mdl asset: self-pointer, its two duplicated fields, the
    // filename decode, and the refcount floor against live model users.
    out += ",\"model_assets\":";
    if (const auto ac = mgr->check_model_assets(8192); ac.has_value()) {
        char ab[512];
        snprintf(ab, sizeof(ab),
                 "{\"assets\":%zu,\"self_ref_ok\":%zu,\"radius_dup_ok\":%zu,\"name_at_blob\":%zu,"
                 "\"name_readable\":%zu,\"refcount_ge\":%zu,\"refcount_exact\":%zu,"
                 "\"blob_size_sane\":%zu,\"arrays_in_blob\":%zu,\"write_order_ok\":%zu,"
                 "\"count_matches\":%zu,\"count_dup_ok\":%zu}",
                 ac->assets, ac->self_ref_ok, ac->radius_dup_ok, ac->name_at_blob,
                 ac->name_readable, ac->refcount_ge, ac->refcount_exact, ac->blob_size_sane,
                 ac->arrays_in_blob, ac->write_order_ok, ac->count_matches, ac->count_dup_ok);
        out += ab;
    } else {
        out += "null";
    }

    // Skeleton nodes. The hash is NOT recomputed host-side -- the check is that
    // the same name always carries the same hash, which needs no engine data.
    out += ",\"model_nodes\":";
    if (const auto nc = mgr->check_model_nodes(8192); nc.has_value()) {
        char nb[704];
        snprintf(nb, sizeof(nb),
                 "{\"assets\":%zu,\"nodes_total\":%zu,\"names_in_blob\":%zu,"
                 "\"names_printable\":%zu,\"distinct_names\":%zu,\"repeated_names\":%zu,"
                 "\"hash_consistent\":%zu,\"hash_collisions\":%zu,\"count_dup_ok\":%zu,"
                 "\"records_in_blob\":%zu,\"root_is_255\":%zu,\"index_self_ok\":%zu,"
                 "\"topological_ok\":%zu,\"child_sum_ok\":%zu,\"rot_a_unit\":%zu,"
                 "\"rot_b_unit\":%zu,\"pos_finite\":%zu,\"child_block_in_range\":%zu,"
                 "\"child_parents_ok\":%zu,\"child_links_seen\":%zu}",
                 nc->assets, nc->nodes_total, nc->names_in_blob, nc->names_printable,
                 nc->distinct_names, nc->repeated_names, nc->hash_consistent,
                 nc->hash_collisions, nc->count_dup_ok, nc->records_in_blob, nc->root_is_255,
                 nc->index_self_ok, nc->topological_ok, nc->child_sum_ok, nc->rot_a_unit,
                 nc->rot_b_unit, nc->pos_finite, nc->child_block_in_range, nc->child_parents_ok,
                 nc->child_links_seen);
        out += nb;
    } else {
        out += "null";
    }

    // The animation-name lookup table. Ascending order is what the engine's own
    // binary search requires, so it is an invariant with a quiet failure mode.
    out += ",\"anim_tables\":";
    if (const auto at = mgr->check_anim_tables(8192); at.has_value()) {
        char tb[256];
        snprintf(tb, sizeof(tb),
                 "{\"assets\":%zu,\"table_sane\":%zu,\"hashes_ascending\":%zu,"
                 "\"entries_total\":%zu,\"max_entries\":%zu}",
                 at->assets, at->table_sane, at->hashes_ascending, at->entries_total,
                 at->max_entries);
        out += tb;
    } else {
        out += "null";
    }

    // Bounding geometry across every type. Same self-check shape: these are
    // identities SetDims establishes, so a divergence means a moved offset in
    // the culling inputs (dims / radius / AABB).
    out += ",\"geometry\":";
    if (const auto gc = mgr->check_object_geometry(512); gc.has_value()) {
        char gb[256];
        snprintf(gb, sizeof(gb),
                 "{\"sampled\":%zu,\"aabb_min_ok\":%zu,\"aabb_max_ok\":%zu,\"radius_sized\":%zu,"
                 "\"radius_pristine\":%zu,\"dims_nonneg\":%zu}",
                 gc->sampled, gc->aabb_min_ok, gc->aabb_max_ok, gc->radius_sized,
                 gc->radius_pristine, gc->dims_nonneg);
        out += gb;
    } else {
        out += "null";
    }

    // World tree (the X/Z quadtree objects are culled through). Reaches the
    // structure the way the engine does -- object -> link -> owning node -> up
    // the parent chain -- so one report exercises world_tree_link,
    // parent_offset and occupied_count together.
    out += ",\"world_tree\":";
    if (const auto wt = mgr->check_world_tree(8192); wt.has_value()) {
        char wb[448];
        snprintf(wb, sizeof(wb),
                 "{\"objects_seen\":%zu,\"linked\":%zu,\"unlinked\":%zu,\"node_found\":%zu,"
                 "\"root_reached\":%zu,\"counts_monotonic\":%zu,\"root_mismatches\":%zu,"
                 "\"root\":\"0x%08" PRIXPTR "\",\"max_depth\":%zu,"
                 "\"bsp_root\":\"0x%08" PRIXPTR "\",\"root_matches_bsp\":%s}",
                 wt->objects_seen, wt->linked, wt->unlinked, wt->node_found, wt->root_reached,
                 wt->counts_monotonic, wt->root_mismatches, wt->root, wt->max_depth,
                 wt->bsp_root, wt->root_matches_bsp ? "true" : "false");
        out += wb;
    } else {
        out += "null";
    }

    // The world container itself, reached from IWorldClientBSP by name. Holds
    // the world bounds (two copies), the world-tree root and node count, and the
    // embedded vis tree -- so this walk validates the world tree from the HEADER
    // side, independently of the object-side walk above.
    out += ",\"world_bsp\":";
    if (const auto wbsp = sdk::WorldBSP::check(); wbsp.has_value()) {
        char bb[352];
        snprintf(bb, sizeof(bb),
                 "{\"stored_node_count\":%zu,\"nodes_walked\":%zu,\"occupied\":%zu,"
                 "\"max_depth\":%zu,\"bounds_ordered\":%s,\"bounds_copies_agree\":%s,"
                 "\"sectors_in_bounds\":%zu,\"sector_count\":%zu}",
                 wbsp->stored_node_count, wbsp->nodes_walked, wbsp->occupied, wbsp->max_depth,
                 wbsp->bounds_ordered ? "true" : "false",
                 wbsp->bounds_copies_agree ? "true" : "false", wbsp->sectors_in_bounds,
                 wbsp->sector_count);
        out += bb;
    } else {
        out += "null";
    }

    // Per-type cull volumes. Guards the offsets behind the sphere/AABB the
    // engine culls with -- notably OT_MODEL's vis_radius * scale, which is the
    // expression that identified `scale` in the first place.
    out += ",\"cull_volumes\":";
    if (const auto cv = mgr->check_cull_volumes(512); cv.has_value()) {
        char cb[512];
        snprintf(cb, sizeof(cb),
                 "{\"models\":%zu,\"model_vis_radius_pos\":%zu,\"model_radius_ok\":%zu,"
                 "\"model_asset_nonnull\":%zu,\"model_asset_radius_eq\":%zu,"
                 "\"particles\":%zu,\"particle_type_ok\":%zu,\"particle_sphere\":%zu,"
                 "\"particle_aabb\":%zu,\"sprites\":%zu,\"sprite_aabb\":%zu,"
                 "\"sprite_sphere\":%zu,\"sprite_aabb_ordered\":%zu,\"sprite_radius_ok\":%zu}",
                 cv->models, cv->model_vis_radius_pos, cv->model_radius_ok,
                 cv->model_asset_nonnull, cv->model_asset_radius_eq, cv->particles,
                 cv->particle_type_ok, cv->particle_sphere, cv->particle_aabb, cv->sprites,
                 cv->sprite_aabb, cv->sprite_sphere, cv->sprite_aabb_ordered,
                 cv->sprite_radius_ok);
        out += cb;
    } else {
        out += "null";
    }

    // Attachment graph, owned game-side objects, and the per-object slot index.
    // The strong guards here are the totals: `self` on every object and the
    // parent/link biconditional. The parented population is scene-dependent, so
    // children_reached == parented is a cross-count identity rather than a
    // population assertion.
    out += ",\"attachments\":";
    // Cap chosen to COVER a normal scene rather than to sample it: the
    // children_reached == parented identity is only valid over a complete walk,
    // and `listed` lets the test detect truncation instead of silently comparing
    // mismatched populations.
    if (const auto ac = mgr->check_attachments(8192); ac.has_value()) {
        char ab[512];
        snprintf(ab, sizeof(ab),
                 "{\"objects\":%zu,\"listed\":%zu,\"self_ptr_ok\":%zu,\"parentless\":%zu,"
                 "\"parented\":%zu,\"link_consistent\":%zu,\"children_reached\":%zu,"
                 "\"child_parent_ok\":%zu,\"owned_nonempty\":%zu,\"owned_entries\":%zu,"
                 "\"index_none\":%zu,\"index_set\":%zu,\"shared_refs\":%zu,"
                 "\"shared_ref_count_ok\":%zu,\"shared_ref_self_ok\":%zu}",
                 ac->objects, ac->listed, ac->self_ptr_ok, ac->parentless, ac->parented,
                 ac->link_consistent, ac->children_reached, ac->child_parent_ok,
                 ac->owned_nonempty, ac->owned_entries, ac->index_none, ac->index_set,
                 ac->shared_refs, ac->shared_ref_count_ok, ac->shared_ref_self_ok);
        out += ab;
    } else {
        out += "null";
    }

    // Spatial records: the cull volume the engine itself stored, compared against
    // the volume recomputed from typed fields. The comparison spans LTObject,
    // LTModelObject, LTSpriteObject and LTParticleSystemObject, so one number
    // here guards the whole geometry mapping. Complete coverage matters for the
    // same reason as the attachment walk, so the cap covers rather than samples.
    out += ",\"spatial_records\":";
    if (const auto sr = mgr->check_spatial_records(8192); sr.has_value()) {
        char sb[576];
        snprintf(sb, sizeof(sb),
                 "{\"objects\":%zu,\"backpointer_ok\":%zu,\"volume_matched\":%zu,"
                 "\"volume_gated\":%zu,\"unexplained\":%zu,\"entries\":%zu,"
                 "\"count_matches_walk\":%zu,\"entry_record_ok\":%zu,\"hit_links_ok\":%zu,"
                 "\"entry_sector_aabb_ok\":%zu,\"entry_sector_planes_ok\":%zu,"
                 "\"gate_open\":%zu,\"records_with_entries\":%zu,\"gated_violations\":%zu}",
                 sr->objects, sr->backpointer_ok, sr->volume_matched, sr->volume_gated,
                 sr->unexplained, sr->entries, sr->count_matches_walk, sr->entry_record_ok,
                 sr->hit_links_ok, sr->entry_sector_aabb_ok, sr->entry_sector_planes_ok,
                 sr->gate_open, sr->records_with_entries, sr->gated_violations);
        out += sb;
    } else {
        out += "null";
    }

    // THE PUBLIC OBJECT API, cross-checked against the internal walk above.
    //
    // sdk::is_renderable reproduces the engine's collection gate for consumers.
    // check_spatial_records already counts that same gate internally, and that count
    // is proven against engine behaviour (gated_violations == 0 across every bucket).
    // So counting it a SECOND time through the public path and requiring the two to
    // agree is what stops the consumer-facing function from quietly lying -- a
    // wrong mask or a wrong flags2 offset would show up here as a divergence, not as
    // a subtly wrong answer inside somebody's mod.
    //
    // Deliberately computed the long way: snapshot each bucket, then call the same
    // public functions a mod would call, on the same object addresses.
    {
        size_t api_objects = 0, api_renderable = 0, api_info_ok = 0, api_camera_bit = 0,
               api_cameras = 0, api_with_handle = 0, api_with_slot = 0,
               api_identities_agree = 0, api_addressable = 0, api_with_attachments = 0,
               api_attachments = 0, api_att_child_ok = 0, api_att_socketed = 0,
               api_att_resolved = 0, api_att_is_socket = 0, api_att_measured = 0,
               api_att_placed = 0, api_socket_total = 0, api_socket_ok = 0,
               api_socket_named_node = 0, api_socket_roundtrip = 0,
               api_socket_camera = 0, api_socket_eyes = 0, api_node_xform_ok = 0,
               api_eye_geom = 0, api_eye_level = 0, api_eye_left_neg = 0, api_eye_vs_camera = 0,
               api_bind_nodes = 0, api_bind_unit = 0, api_bind_finite = 0, api_bind_shared = 0,
               api_bind_shared_ok = 0, api_bind_same_array = 0, api_bind_n_shallow = 0,
               api_bind_n_deep = 0, api_bind_max_depth = 0, api_bind_n_edge = 0,
               api_hier_probed = 0, api_hier_depth_ok = 0, api_hier_max_depth = 0,
               api_hier_roots = 0, api_hier_root_zero = 0, api_hier_step_ok = 0,
               api_hier_anc_ok = 0, api_hier_self_ok = 0,
               api_node_xform_stale = 0, api_node_xform_clean = 0,
               api_node_xform_clean_sane = 0, api_camera_node_clean = 0,
               api_dims_ok = 0, api_dims_nonneg = 0, api_dims_zero = 0,
               api_standing = 0, api_standing_sane = 0, api_standing_node = 0,
               api_color_ok = 0, api_color_packed_ok = 0, api_color_default = 0,
               api_color_translucent = 0, api_brush = 0, api_brush_roundtrip = 0,
               api_brush_rt_exact = 0, api_brush_origin_ok = 0, api_brush_quality = 0,
               api_brush_trusted = 0, api_brush_matrix = 0, api_brush_origin_agrees = 0,
               api_cull_ok = 0, api_cull_sphere = 0, api_cull_box = 0, api_cull_none = 0,
               api_cull_sane = 0, api_cull_compared = 0, api_cull_current = 0,
               api_tree_asked = 0, api_tree_linked = 0, api_tree_nonempty = 0,
               api_tree_self_found = 0,
               api_tree_nonwm = 0, api_tree_nonwm_found = 0, api_tree_wm_missed = 0,
               api_tree_miss_slot_found = 0, api_tree_miss_max_depth = 0,
               api_tree_miss_at_leaf = 0, api_tree_hit_slot_probed = 0, api_tree_miss_stale = 0,
               api_tree_cur_asked = 0, api_tree_cur_ok = 0,
               api_aabb_ok = 0, api_aabb_ordered = 0, api_aabb_asked = 0,
               api_aabb_current = 0, api_rad_ok = 0, api_rad_sized = 0,
               api_rad_unsized = 0, api_rad_sane = 0;
        // Per-asset bind poses, keyed by the shared asset pointer: the first object of an
        // asset records them, every later one must match.
        std::unordered_map<uintptr_t,
                           std::pair<std::vector<std::array<float, 7>>, sdk::ModelSkeleton>>
            bind_seen;
        // The worst disagreement between the engine's placement of an attached child and
        // our own composition for its socket handle. A float, not a count: the interesting
        // result is the magnitude.
        double api_bind_mag_shallow = 0.0, api_bind_mag_deep = 0.0, api_bind_edge = 0.0;
        float api_eye_sep_min = -1.0f, api_eye_sep_max = 0.0f, api_eye_asym_max = 0.0f;
        float api_att_worst_err = 0.0f, api_brush_worst_rt = 0.0f,
              api_brush_worst_origin = 0.0f, api_brush_worst_rot = 0.0f;
        std::vector<sdk::CClientMgr::ObjectSnapshot> snaps(4096);
        for (size_t t = 0; t < sdk::CClientMgr::object_list_count(); ++t) {
            const auto taken = mgr->snapshot_objects(static_cast<sdk::ObjectType>(t),
                                                     snaps.data(), snaps.size());
            if (!taken.has_value()) {
                continue;
            }
            for (size_t i = 0; i < *taken; ++i) {
                const auto* obj = reinterpret_cast<const regenny::LTObject*>(snaps[i].address);
                ++api_objects;
                if (const auto info = sdk::object_info(obj); info.has_value()) {
                    ++api_info_ok;
                    if (info->kind == sdk::ObjectKind::Camera) {
                        ++api_cameras;
                        if ((info->flags & sdk::object_flags::kCameraOnly) != 0) {
                            ++api_camera_bit;
                        }
                    }
                    // THE TWO IDENTITIES, both through the public struct. An object
                    // either carries a handle and a slot index or neither -- so
                    // counting them separately and comparing is a real check: a
                    // schema drift that moved one field would break the equality
                    // without either count going to zero.
                    const bool has_handle = info->handle != 0xFFFF;
                    const bool has_slot = info->slot_index != 0xFFFFFFFFu;
                    if (has_handle) {
                        ++api_with_handle;
                    }
                    if (has_slot) {
                        ++api_with_slot;
                    }
                    if (has_handle == has_slot) {
                        ++api_identities_agree;
                    }
                    // And the predicate a mod actually calls -- which is the engine's
                    // own IsServerObject test, so it must match handle presence.
                    if (const auto a = sdk::is_server_object(obj); a.value_or(false)) {
                        ++api_addressable;
                    }
                    // DIMS and GROUND CONTACT, both through the public API. A dim is a
                    // half-extent, so no component may be negative -- that one is an
                    // invariant. Ground contact is per-scene, so it is counted.
                    if (const auto d = sdk::object_dims(obj); d.has_value()) {
                        ++api_dims_ok;
                        if (d->x >= 0.0f && d->y >= 0.0f && d->z >= 0.0f) {
                            ++api_dims_nonneg;
                        }
                        if (d->x == 0.0f && d->y == 0.0f && d->z == 0.0f) {
                            ++api_dims_zero;
                        }
                    }
                    if (const auto s = sdk::standing_on(obj); s.has_value()) {
                        ++api_standing;
                        if (s->object != nullptr && std::isfinite(s->surface_height)) {
                            ++api_standing_sane;
                        }
                        if (s->has_node) {
                            ++api_standing_node;
                        }
                    }
                    // BRUSH SPACE, both directions. A world model carries its transform
                    // AND the engine's own inverse, so a round trip through them is a real
                    // check on two independently-stored matrices: transform a point out
                    // and back, and it must return. A transposed read or a wrong offset
                    // breaks the return trip even though each direction alone would still
                    // produce plausible coordinates.
                    //
                    // The ORIGIN check pins the translation column specifically: the
                    // object's own world position must map to the brush's local origin.
                    if (info->kind == sdk::ObjectKind::WorldModel ||
                        info->kind == sdk::ObjectKind::Camera) {
                        ++api_brush;
                        // Probe in BRUSH space so the point starts near the brush's own
                        // origin. A first version offset from the object's WORLD position
                        // and I predicted that was inflating the error via the lever arm
                        // from a brush origin 17741 units out. MEASURED: it changed the
                        // worst error from 49.84466 to 49.84435, i.e. not at all. The
                        // prediction was wrong and the brush-space probe is kept only
                        // because it is the more direct question, not because it fixed
                        // anything.
                        const regenny::LTVector probe{37.0f, -11.0f, 5.0f};
                        const auto world = sdk::brush_to_world(obj, probe);
                        const auto back = world.has_value()
                                              ? sdk::world_to_brush(obj, *world)
                                              : std::optional<regenny::LTVector>{};
                        if (back.has_value()) {
                            ++api_brush_roundtrip;
                            const float rx = back->x - probe.x;
                            const float ry = back->y - probe.y;
                            const float rz = back->z - probe.z;
                            const float err = std::sqrt(rx * rx + ry * ry + rz * rz);
                            if (err > api_brush_worst_rt) {
                                api_brush_worst_rt = err;
                            }
                            // 0.5, NOT 0.05, and the number is measured rather than
                            // chosen: at these coordinates a float32 point through two
                            // 3x4 matrices loses ~0.1 units, so 23 of the 27 "failures"
                            // at the tighter bound were just precision. A wrong offset or
                            // a transposed read misses by thousands, so this still catches
                            // real breakage.
                            if (err < 0.5f) {
                                ++api_brush_rt_exact;
                            }
                        }
                        // AND A SEPARATE, WEAKER QUESTION, deliberately only counted:
                        // does the brush's local origin coincide with the object's world
                        // position? For a transform built straight from the object's
                        // pos/rot it would, and for 908 of 1947 it does -- but for the
                        // rest the brush's modelling origin sits elsewhere, so this is a
                        // property of the level's art and NOT an invariant to assert.
                        if (const auto o = sdk::world_to_brush(obj, info->position);
                            o.has_value()) {
                            const float d = std::sqrt(o->x * o->x + o->y * o->y +
                                                      o->z * o->z);
                            if (d > api_brush_worst_origin) {
                                api_brush_worst_origin = d;
                            }
                            if (d < 0.05f) {
                                ++api_brush_origin_ok;
                            }
                        }
                        // THE TRANSFORM PRIMITIVES, used the way a mod would: ask for the
                        // matrix, and ask whether it can be trusted. Both were trapped
                        // inside CClientMgr::check_transforms until this pass -- a mod
                        // could learn the population-wide count and nothing about the
                        // object in front of it.
                        if (const auto q = sdk::brush_transform_quality(obj); q.has_value()) {
                            ++api_brush_quality;
                            if (q->trustworthy()) {
                                ++api_brush_trusted;
                            }
                            if (q->rotation_error > api_brush_worst_rot) {
                                api_brush_worst_rot = q->rotation_error;
                            }
                        }
                        // And the matrix itself: its translation column must be the point
                        // brush_to_world maps the local origin to. Two routes through the
                        // same data -- one reading the column, one composing -- so a
                        // row/column mix-up in either breaks the agreement.
                        if (const auto m = sdk::brush_transform(obj); m.has_value()) {
                            ++api_brush_matrix;
                            const auto o = sdk::brush_to_world(obj, regenny::LTVector{});
                            if (o.has_value()) {
                                const float ex = o->x - m->m[3];
                                const float ey = o->y - m->m[7];
                                const float ez = o->z - m->m[11];
                                if (std::sqrt(ex * ex + ey * ey + ez * ez) < 0.01f) {
                                    ++api_brush_origin_agrees;
                                }
                            }
                        }
                    }
                    // THE SPATIAL INDEX. The oracle is SELF-LOCATION: the engine linked
                    // each object at the deepest node fully containing its AABB, and the
                    // object's own position lies inside that AABB, so descending toward that
                    // position must pass through that node. Every LINKED object therefore
                    // has to find ITSELF. A wrong quadrant mapping, or harvesting only the
                    // leaf, breaks this while still returning plausible neighbours.
                    if (const auto linked = sdk::WorldBSP::is_linked(obj); linked.has_value()) {
                        ++api_tree_asked;
                        if (*linked) {
                            ++api_tree_linked;
                            if (const auto cur = sdk::WorldBSP::index_is_current(obj);
                                cur.has_value()) {
                                ++api_tree_cur_asked;
                                if (*cur) {
                                    ++api_tree_cur_ok;
                                }
                            }
                            const auto nearby = sdk::WorldBSP::objects_near(info->position, 4096);
                            if (!nearby.empty()) {
                                ++api_tree_nonempty;
                                bool self = false;
                                for (const auto* o : nearby) {
                                    if (o == obj) {
                                        self = true;
                                        break;
                                    }
                                }
                                const bool is_wm = info->kind == sdk::ObjectKind::WorldModel;
                                if (!is_wm) {
                                    ++api_tree_nonwm;
                                }
                                if (self) {
                                    ++api_tree_self_found;
                                    if (!is_wm) {
                                        ++api_tree_nonwm_found;
                                    }
                                } else if (info->kind == sdk::ObjectKind::WorldModel) {
                                    // AN OPEN QUESTION, recorded rather than smoothed over.
                                    // 235 of 1473 linked worldmodels are not found by a
                                    // descent toward their own position, while all 669
                                    // non-worldmodels are. Two candidate explanations were
                                    // MEASURED AND REFUTED: buffer truncation (raising the
                                    // cap 256 -> 4096 recovered only 3) and a position
                                    // outside the object's own AABB (zero of the 235). The
                                    // quadrant convention is the engine's own, transcribed
                                    // from LTWorldTree_FindNodeForObject, and it is exactly
                                    // what makes the other 669 work.
                                    ++api_tree_wm_missed;
                                    // WHERE is it actually parked? Compare the node the
                                    // engine chose against what the descent visits.
                                    // THE SURVIVING CANDIDATE, tested: is the entry
                                    // STALE? Descend the engine's own box rule with the
                                    // object's CURRENT bounds and compare against the node
                                    // it is actually parked in.
                                    if (const auto cur = sdk::WorldBSP::index_is_current(obj);
                                        cur.has_value() && !*cur) {
                                        ++api_tree_miss_stale;
                                    }
                                    if (const auto sl = sdk::WorldBSP::tree_slot(obj);
                                        sl.has_value()) {
                                        ++api_tree_miss_slot_found;
                                        if (sl->depth > api_tree_miss_max_depth) {
                                            api_tree_miss_max_depth = sl->depth;
                                        }
                                        if (sl->leaf) {
                                            ++api_tree_miss_at_leaf;
                                        }
                                    }
                                }
                            }
                        }
                    }
                    // WORLD BOUNDS, extracted from check_object_geometry this pass. The
                    // interesting cross-measurement: the AABB is written by the same
                    // SetPos path that relinks the spatial index, so if 370 index entries
                    // are stale, are any AABBs stale too? Measured rather than assumed.
                    if (const auto box = sdk::world_aabb(obj); box.has_value()) {
                        ++api_aabb_ok;
                        if (box->min.x <= box->max.x && box->min.y <= box->max.y &&
                            box->min.z <= box->max.z) {
                            ++api_aabb_ordered;
                        }
                    }
                    if (const auto fresh = sdk::world_aabb_is_current(obj);
                        fresh.has_value()) {
                        ++api_aabb_asked;
                        if (*fresh) {
                            ++api_aabb_current;
                        }
                    }
                    if (const auto br = sdk::bounding_radius(obj); br.has_value()) {
                        ++api_rad_ok;
                        if (br->from_dims) {
                            ++api_rad_sized;
                        } else if (br->unsized) {
                            ++api_rad_unsized;
                        }
                        if (br->radius >= 0.0f && std::isfinite(br->radius)) {
                            ++api_rad_sane;
                        }
                    }
                    // CULL VOLUMES, the way a mod asks for bounds. Extracted from
                    // check_spatial_records this pass -- previously a mod could learn the
                    // population-wide match count and nothing about the object it holds.
                    if (const auto cv = sdk::computed_cull_volume(obj); cv.has_value()) {
                        ++api_cull_ok;
                        switch (cv->shape) {
                            case sdk::CullShape::Sphere:
                                ++api_cull_sphere;
                                // A radius is a size: it cannot be negative, and a
                                // non-finite one would poison any range test built on it.
                                if (cv->radius >= 0.0f && std::isfinite(cv->radius)) {
                                    ++api_cull_sane;
                                }
                                break;
                            case sdk::CullShape::Box:
                                ++api_cull_box;
                                // A box's min must not exceed its max on any axis -- the
                                // one invariant every box satisfies regardless of which
                                // type produced it.
                                if (cv->min.x <= cv->max.x && cv->min.y <= cv->max.y &&
                                    cv->min.z <= cv->max.z) {
                                    ++api_cull_sane;
                                }
                                break;
                            case sdk::CullShape::None:
                                ++api_cull_none;
                                ++api_cull_sane;
                                break;
                        }
                    }
                    if (const auto cur = sdk::cull_volume_is_current(obj); cur.has_value()) {
                        ++api_cull_compared;
                        if (*cur) {
                            ++api_cull_current;
                        }
                    }
                    // COLOUR AND ALPHA. The assertable part is the PACKING: alpha must
                    // be the high byte of the value the engine hands out, which is what
                    // SetObjectAlpha's single-byte write at +0x07 claims. Counting
                    // translucent objects is scene-dependent and only reported.
                    if (const auto c = sdk::object_color(obj); c.has_value()) {
                        ++api_color_ok;
                        if ((c->packed >> 24) == c->a && ((c->packed >> 16) & 0xFF) == c->r &&
                            ((c->packed >> 8) & 0xFF) == c->g && (c->packed & 0xFF) == c->b) {
                            ++api_color_packed_ok;
                        }
                        if (c->packed == 0xFFFFFFFFu) {
                            ++api_color_default;
                        }
                        if (c->a != 255) {
                            ++api_color_translucent;
                        }
                    }
                    // ATTACHMENTS, walked the way a mod would: for every object, ask what
                    // rides on it, and resolve each handle THROUGH THE ENGINE'S UNIFIED
                    // SPACE. An earlier version of this loop resolved the handle as a node
                    // index and counted how many produced a name -- it counted 27/27 and
                    // was wrong, because a socket handle also lands inside node_count.
                    //
                    // THE REAL CHECK is placement: the engine moves each child to its
                    // handle's transform, so the child's own position must equal what we
                    // compose for that handle. That compares two independent producers
                    // instead of asking one table for a plausible-looking string.
                    const auto atts = sdk::attachments(obj);
                    if (!atts.empty()) {
                        ++api_with_attachments;
                        api_attachments += atts.size();
                        const auto skel = sdk::ModelSkeleton::from_object(obj);
                        for (const auto& at : atts) {
                            if (at.child != nullptr) {
                                ++api_att_child_ok;
                            }
                            if (!at.socket_handle.has_value() || !skel.has_value()) {
                                continue;
                            }
                            ++api_att_socketed;
                            const auto rh = skel->resolve_socket_handle(*at.socket_handle);
                            if (!rh.has_value()) {
                                continue;
                            }
                            ++api_att_resolved;
                            if (rh->kind == sdk::ModelSkeleton::HandleKind::Socket) {
                                ++api_att_is_socket;
                            }
                            const auto xf = skel->socket_handle_transform(*at.socket_handle);
                            if (!xf.has_value() || at.child == nullptr) {
                                continue;
                            }
                            const auto ci = sdk::object_info(at.child);
                            if (!ci.has_value()) {
                                continue;
                            }
                            ++api_att_measured;
                            const float ex = ci->position.x - xf->position.x;
                            const float ey = ci->position.y - xf->position.y;
                            const float ez = ci->position.z - xf->position.z;
                            const float err = std::sqrt(ex * ex + ey * ey + ez * ez);
                            if (err > api_att_worst_err) {
                                api_att_worst_err = err;
                            }
                            if (err < 0.05f) {
                                ++api_att_placed;
                            }
                        }
                    }
                    // SOCKETS, asked for the way a VR mod would: by NAME. These are
                    // the art's own attach points, so the interesting question is not
                    // "how many" but "does the model define the ones I need".
                    if (const auto sk = sdk::ModelSkeleton::from_object(obj); sk.has_value()) {
                        api_socket_total += sk->socket_count();
                        for (size_t si = 0; si < sk->socket_count(); ++si) {
                            const auto s = sk->socket(si);
                            if (!s.has_value()) {
                                continue;
                            }
                            ++api_socket_ok;
                            // Every socket names a node in THIS skeleton, and that
                            // node must resolve to a name -- the same two-subsystem
                            // crossing the attachment sockets get.
                            if (sk->node_name(s->node_index).has_value()) {
                                ++api_socket_named_node;
                            }
                            // CASE-INSENSITIVITY, tested without depending on which
                            // assets happen to be loaded: take the socket's OWN name,
                            // upper-case it, and require the lookup to return the same
                            // index. Asking for a hardcoded "LEFTHAND" would only
                            // prove anything in a level containing characters.
                            std::string upper = s->name;
                            for (auto& c : upper) {
                                c = static_cast<char>(::toupper(static_cast<unsigned char>(c)));
                            }
                            if (sk->find_socket(upper.c_str()) == si) {
                                ++api_socket_roundtrip;
                            }
                        }
                        // THE WHOLE CHAIN A VR MOD WALKS, end to end: find a socket by
                        // name, take the node it rides, ask where that node currently
                        // is. Counted separately for clean and stale, because the
                        // difference is the entire contract.
                        for (size_t ni = 0; ni < sk->node_count(); ++ni) {
                            if (const auto nt = sk->node_transform(ni); nt.has_value()) {
                                ++api_node_xform_ok;
                                if (nt->stale) {
                                    ++api_node_xform_stale;
                                } else {
                                    ++api_node_xform_clean;
                                    // A clean slot must hold a usable position. This is
                                    // the measured invariant (343/343 live), so it is
                                    // counted for assertion rather than reported.
                                    const float m = std::abs(nt->position.x) +
                                                    std::abs(nt->position.y) +
                                                    std::abs(nt->position.z);
                                    if (std::isfinite(m) && m < 1.0e5f) {
                                        ++api_node_xform_clean_sane;
                                    }
                                }
                            }
                        }
                        // And the same question a mod really asks: where is the head
                        // socket's bone right now?
                        if (const auto ci = sk->find_socket("camera"); ci.has_value()) {
                            if (const auto s = sk->socket(*ci); s.has_value()) {
                                if (const auto nt = sk->node_transform(s->node_index);
                                    nt.has_value() && !nt->stale) {
                                    ++api_camera_node_clean;
                                }
                            }
                        }
                        // The VR-relevant ones, REPORTED rather than required: which
                        // sockets exist depends entirely on what the level loaded.
                        if (sk->find_socket("camera").has_value()) {
                            ++api_socket_camera;
                        }
                        // DERIVED HIERARCHY QUERIES, exercised as a consumer would: the walking and
                        // the malformed-chain guard live in ModelSkeleton, so this only aggregates.
                        for (size_t ni = 0; ni < sk->node_count(); ++ni) {
                            ++api_hier_probed;
                            const auto d = sk->node_depth(ni);
                            if (!d.has_value()) {
                                continue;  // malformed chain -- counted by the shortfall
                            }
                            ++api_hier_depth_ok;
                            if (*d > api_hier_max_depth) {
                                api_hier_max_depth = *d;
                            }
                            const auto par = sk->parent_of(ni);
                            if (!par.has_value()) {
                                ++api_hier_roots;
                                if (*d == 0) {
                                    ++api_hier_root_zero;
                                }
                                continue;
                            }
                            // depth(child) == depth(parent) + 1, and the parent must be an ancestor.
                            // Two independent consequences of one walk.
                            if (const auto pd = sk->node_depth(*par);
                                pd.has_value() && *d == *pd + 1) {
                                ++api_hier_step_ok;
                            }
                            if (sk->node_has_ancestor(ni, *par)) {
                                ++api_hier_anc_ok;
                            }
                            if (!sk->node_has_ancestor(ni, ni)) {
                                ++api_hier_self_ok;
                            }
                        }
                        // THE BIND POSE IS ASSET DATA, so every object sharing an asset must
                        // report the SAME bind pose for the same node. That is the check which
                        // distinguishes asset data from a per-object cache -- had the offset been a
                        // per-instance field, instances would diverge -- and it needs no reference
                        // values of any kind, only two objects and the engine's own sharing.
                        {
                            // asset_id() is exactly the intended use: "two models with the
                            // same value share node indices", so it keys the per-asset cache.
                            const auto asset = sk->asset_id();
                            if (asset != 0) {
                                auto it = bind_seen.find(asset);
                                if (it == bind_seen.end()) {
                                    // First sighting: record this asset's bind poses.
                                    std::vector<std::array<float, 7>> poses;
                                    for (size_t ni = 0; ni < sk->node_count(); ++ni) {
                                        if (const auto bp = sk->inverse_bind_pose(ni); bp.has_value()) {
                                            poses.push_back(std::array<float, 7>{
                                                bp->position.x, bp->position.y, bp->position.z,
                                                bp->rotation.x, bp->rotation.y, bp->rotation.z,
                                                bp->rotation.w});
                                            ++api_bind_nodes;
                                            // DEPTH vs MAGNITUDE, to discriminate the coordinate
                                            // space without a reader: parent-relative offsets are
                                            // bone lengths and should not grow with depth, while
                                            // model-space positions must. Accumulated separately
                                            // for shallow and deep nodes.
                                            size_t depth = 0;
                                            for (size_t up = ni; depth < 32;) {
                                                const auto par = sk->parent_of(up);
                                                if (!par.has_value()) {
                                                    break;
                                                }
                                                up = *par;
                                                ++depth;
                                            }
                                            const float mag = std::sqrt(
                                                bp->position.x * bp->position.x +
                                                bp->position.y * bp->position.y +
                                                bp->position.z * bp->position.z);
                                            if (depth > api_bind_max_depth) {
                                                api_bind_max_depth = depth;
                                            }
                                            // THE DECISIVE ONE: if these are model-space, the
                                            // DIFFERENCE from the parent is the bone length -- small
                                            // and depth-independent -- while the position itself
                                            // grows. If they are parent-relative, the position IS
                                            // the bone length and the difference is meaningless.
                                            if (const auto par = sk->parent_of(ni); par.has_value()) {
                                                if (const auto pp = sk->inverse_bind_pose(*par);
                                                    pp.has_value()) {
                                                    const float ex = bp->position.x - pp->position.x;
                                                    const float ey = bp->position.y - pp->position.y;
                                                    const float ez = bp->position.z - pp->position.z;
                                                    api_bind_edge += std::sqrt(ex * ex + ey * ey +
                                                                               ez * ez);
                                                    ++api_bind_n_edge;
                                                }
                                            }
                                            if (depth <= 1) {
                                                api_bind_mag_shallow += mag;
                                                ++api_bind_n_shallow;
                                            } else if (depth >= 4) {
                                                api_bind_mag_deep += mag;
                                                ++api_bind_n_deep;
                                            }
                                            const float qn =
                                                bp->rotation.x * bp->rotation.x +
                                                bp->rotation.y * bp->rotation.y +
                                                bp->rotation.z * bp->rotation.z +
                                                bp->rotation.w * bp->rotation.w;
                                            if (qn > 0.98f && qn < 1.02f) {
                                                ++api_bind_unit;
                                            }
                                            if (std::isfinite(bp->position.x) &&
                                                std::isfinite(bp->position.y) &&
                                                std::isfinite(bp->position.z)) {
                                                ++api_bind_finite;
                                            }
                                        }
                                    }
                                    bind_seen.emplace(asset,
                                                      std::make_pair(std::move(poses), *sk));
                                } else {
                                    // Seen this asset before. TWO SEPARATE CHECKS, because they
                                    // establish different things: the pointer comparison shows the
                                    // two views read the SAME storage, while the value comparison
                                    // only shows the bytes agree -- which separate copies would too.
                                    ++api_bind_shared;
                                    if (sk->shares_node_data(it->second.second)) {
                                        ++api_bind_same_array;
                                    }
                                    bool same = true;
                                    for (size_t ni = 0; ni < sk->node_count(); ++ni) {
                                        const auto bp = sk->inverse_bind_pose(ni);
                                        if (!bp.has_value() || ni >= it->second.first.size()) {
                                            same = false;
                                            break;
                                        }
                                        const auto& r = it->second.first[ni];
                                        // EXACT equality: two reads of one immutable array, not two
                                        // computations, so any difference at all is meaningful.
                                        if (bp->position.x != r[0] || bp->position.y != r[1] ||
                                            bp->position.z != r[2] || bp->rotation.x != r[3] ||
                                            bp->rotation.y != r[4] || bp->rotation.z != r[5] ||
                                            bp->rotation.w != r[6]) {
                                            same = false;
                                            break;
                                        }
                                    }
                                    if (same) {
                                        ++api_bind_shared_ok;
                                    }
                                }
                            }
                        }
                        if (sk->find_socket("socket_left_eye").has_value() &&
                            sk->find_socket("socket_right_eye").has_value()) {
                            ++api_socket_eyes;
                            // EYE GEOMETRY from asset data: no cache, no staleness, no engine
                            // call. Reported so the numbers can be checked for plausibility as
                            // anatomy rather than merely for being finite.
                            if (const auto eg = sk->eye_geometry(); eg.has_value()) {
                                ++api_eye_geom;
                                if (eg->separation > api_eye_sep_max) {
                                    api_eye_sep_max = eg->separation;
                                }
                                if (api_eye_sep_min < 0.0f || eg->separation < api_eye_sep_min) {
                                    api_eye_sep_min = eg->separation;
                                }
                                // MIRROR SYMMETRY about the node's own sagittal plane: a rig puts
                                // the eyes at opposite x with matching height and depth. This is
                                // the check that catches a left/right swap, which no distance
                                // measurement can see.
                                const float sx = eg->left.x + eg->right.x;
                                const float sy = eg->left.y - eg->right.y;
                                const float sz = eg->left.z - eg->right.z;
                                if (std::fabs(sy) < 0.01f && std::fabs(sz) < 0.01f) {
                                    ++api_eye_level;
                                }
                                if (std::fabs(sx) > api_eye_asym_max) {
                                    api_eye_asym_max = std::fabs(sx);
                                }
                                if (eg->left.x < eg->right.x) {
                                    ++api_eye_left_neg;
                                }
                            }
                            if (sk->camera_to_eye_center().has_value()) {
                                ++api_eye_vs_camera;
                            }
                        }
                    }
                }
                if (const auto r = sdk::is_renderable(obj); r.value_or(false)) {
                    ++api_renderable;
                }
            }
        }
        // BUILT FIELD BY FIELD rather than with one 78-specifier snprintf -- see
        // JsonFields for the three misalignments that motivated it. No format string, no
        // fixed buffer, and therefore no truncation guard to grow.
        out += ",\"object_api\":";
        {
            JsonFields j{out};
            j
        .u("objects", api_objects)
        .u("info_ok", api_info_ok)
        .u("renderable", api_renderable)
        .u("cameras", api_cameras)
        .u("cameras_with_bit11", api_camera_bit)
        .u("with_handle", api_with_handle)
        .u("with_slot", api_with_slot)
        .u("identities_agree", api_identities_agree)
        .u("addressable", api_addressable)
        .u("with_attachments", api_with_attachments)
        .u("attachments", api_attachments)
        .u("att_child_ok", api_att_child_ok)
        .u("att_socketed", api_att_socketed)
        .u("att_resolved", api_att_resolved)
        .u("att_is_socket", api_att_is_socket)
        .u("att_measured", api_att_measured)
        .u("att_placed", api_att_placed)
        .f("att_worst_err", api_att_worst_err, 4)
        .u("socket_total", api_socket_total)
        .u("socket_ok", api_socket_ok)
        .u("socket_named_node", api_socket_named_node)
        .u("socket_roundtrip", api_socket_roundtrip)
        .u("socket_camera", api_socket_camera)
        .u("bind_nodes", api_bind_nodes)
        .u("bind_unit", api_bind_unit)
        .u("bind_finite", api_bind_finite)
        .u("bind_shared", api_bind_shared)
        .u("bind_shared_ok", api_bind_shared_ok)
        .u("hier_probed", api_hier_probed)
        .u("hier_depth_ok", api_hier_depth_ok)
        .u("hier_max_depth", api_hier_max_depth)
        .u("hier_roots", api_hier_roots)
        .u("hier_root_zero", api_hier_root_zero)
        .u("hier_step_ok", api_hier_step_ok)
        .u("hier_anc_ok", api_hier_anc_ok)
        .u("hier_self_ok", api_hier_self_ok)
        .u("bind_same_array", api_bind_same_array)
        .u("bind_max_depth", api_bind_max_depth)
        .u("bind_n_shallow", api_bind_n_shallow)
        .u("bind_n_deep", api_bind_n_deep)
        .f("bind_mag_shallow", api_bind_n_shallow ? api_bind_mag_shallow / api_bind_n_shallow : 0.0, 3)
        .f("bind_mag_deep", api_bind_n_deep ? api_bind_mag_deep / api_bind_n_deep : 0.0, 3)
        .u("bind_n_edge", api_bind_n_edge)
        .f("bind_edge_mean", api_bind_n_edge ? api_bind_edge / api_bind_n_edge : 0.0, 3)
        .u("socket_eyes", api_socket_eyes)
        .u("eye_geom", api_eye_geom)
        .u("eye_level", api_eye_level)
        .u("eye_left_neg", api_eye_left_neg)
        .u("eye_vs_camera", api_eye_vs_camera)
        .f("eye_sep_min", api_eye_sep_min, 4)
        .f("eye_sep_max", api_eye_sep_max, 4)
        .f("eye_asym_max", api_eye_asym_max, 4)
        .u("node_xform_ok", api_node_xform_ok)
        .u("node_xform_stale", api_node_xform_stale)
        .u("node_xform_clean", api_node_xform_clean)
        .u("node_xform_clean_sane", api_node_xform_clean_sane)
        .u("camera_node_clean", api_camera_node_clean)
        .u("dims_ok", api_dims_ok)
        .u("dims_nonneg", api_dims_nonneg)
        .u("dims_zero", api_dims_zero)
        .u("standing", api_standing)
        .u("standing_sane", api_standing_sane)
        .u("standing_node", api_standing_node)
        .u("color_ok", api_color_ok)
        .u("color_packed_ok", api_color_packed_ok)
        .u("color_default", api_color_default)
        .u("color_translucent", api_color_translucent)
        .u("brush", api_brush)
        .u("brush_roundtrip", api_brush_roundtrip)
        .u("brush_rt_exact", api_brush_rt_exact)
        .u("brush_origin_ok", api_brush_origin_ok)
        .f("brush_worst_rt", api_brush_worst_rt, 5)
        .f("brush_worst_origin", api_brush_worst_origin, 5)
        .u("brush_quality", api_brush_quality)
        .u("brush_trusted", api_brush_trusted)
        .u("brush_matrix", api_brush_matrix)
        .u("brush_origin_agrees", api_brush_origin_agrees)
        .f("brush_worst_rot", api_brush_worst_rot, 5)
        .u("cull_ok", api_cull_ok)
        .u("cull_sphere", api_cull_sphere)
        .u("cull_box", api_cull_box)
        .u("cull_none", api_cull_none)
        .u("cull_sane", api_cull_sane)
        .u("cull_compared", api_cull_compared)
        .u("cull_current", api_cull_current)
        .u("tree_asked", api_tree_asked)
        .u("tree_linked", api_tree_linked)
        .u("tree_nonempty", api_tree_nonempty)
        .u("tree_self_found", api_tree_self_found)
        .u("tree_nonwm", api_tree_nonwm)
        .u("tree_nonwm_found", api_tree_nonwm_found)
        .u("tree_wm_missed", api_tree_wm_missed)
        .u("miss_slot_found", api_tree_miss_slot_found)
        .u("miss_max_depth", api_tree_miss_max_depth)
        .u("miss_at_leaf", api_tree_miss_at_leaf)
        .u("miss_stale", api_tree_miss_stale)
        .u("cur_asked", api_tree_cur_asked)
        .u("cur_ok", api_tree_cur_ok)
        .u("aabb_ok", api_aabb_ok)
        .u("aabb_ordered", api_aabb_ordered)
        .u("aabb_asked", api_aabb_asked)
        .u("aabb_current", api_aabb_current)
        .u("rad_ok", api_rad_ok)
        .u("rad_sized", api_rad_sized)
        .u("rad_unsized", api_rad_unsized)
        .u("rad_sane", api_rad_sane)
                ;
        }
    }

    // Renderability vs world-tree membership. Asserts the mechanism-backed
    // direction only (renderable => linked); the reverse count is reported
    // because the engine genuinely does not maintain it.
    out += ",\"render_flags\":";
    if (const auto rf = mgr->check_render_flags(8192); rf.has_value()) {
        char fb[288];
        snprintf(fb, sizeof(fb),
                 "{\"objects\":%zu,\"renderable\":%zu,\"linked\":%zu,"
                 "\"renderable_not_linked\":%zu,\"linked_not_renderable\":%zu,"
                 "\"suppressed\":%zu,\"suppressed_linked\":%zu}",
                 rf->objects, rf->renderable, rf->linked, rf->renderable_not_linked,
                 rf->linked_not_renderable, rf->suppressed, rf->suppressed_linked);
        out += fb;
    } else {
        out += "null";
    }

    // The visibility tree, reached from IWorldClientBSP (resolved by engine-side
    // NAME) plus the schema-confirmed +0x24. Unlike every other check here this
    // one does not go through the object lists at all, so it validates the vis
    // subsystem independently -- and the engine stores its own node and sector
    // counts, so the walk is checked against the engine's numbers.
    out += ",\"vis_tree\":";
    if (const auto vt = sdk::VisTree::check(); vt.has_value()) {
        char tb[512];
        snprintf(tb, sizeof(tb),
                 "{\"sector_count\":%zu,\"node_count\":%zu,\"nodes_walked\":%zu,"
                 "\"elements_seen\":%zu,\"elements_in_arr\":%zu,\"sectors_reached\":%zu,"
                 "\"leaves\":%zu,\"max_depth\":%zu,\"portal_count\":%zu,"
                 "\"portal_unit_normal\":%zu,\"portal_center_on_plane\":%zu,"
                 "\"portal_sectors_ok\":%zu,\"portal_verts_on_plane\":%zu}",
                 vt->sector_count, vt->node_count, vt->nodes_walked, vt->elements_seen,
                 vt->elements_in_arr, vt->sectors_reached, vt->leaves, vt->max_depth,
                 vt->portal_count, vt->portal_unit_normal, vt->portal_center_on_plane,
                 vt->portal_sectors_ok, vt->portal_verts_on_plane);
        out += tb;
    } else {
        out += "null";
    }

    // Ask the ENGINE THREAD for an in-place for_each_object count and report
    // whatever it last published. Deliberately non-blocking: if the engine is
    // not running frames (paused, suspended, pre-init) no result will ever
    // arrive, and blocking the IPC thread on it would hang this endpoint.
    // -1 means "no walk has completed yet". Compare `generation` across two
    // polls to know a fresh result landed rather than a stale one.
    // Read the LAST published result before raising a new request, and read
    // the generation FIRST with acquire so the count we then read is the one
    // that generation published (the reverse order would let us pair a fresh
    // generation with a stale count).
    const uint64_t walk_generation = g_object_walk_generation.load(std::memory_order_acquire);
    const int64_t walk_count = g_object_walk_count.load(std::memory_order_relaxed);

    g_object_walk_type.store(static_cast<uint32_t>(regenny::OT_MODEL), std::memory_order_relaxed);
    g_object_walk_requested.store(true, std::memory_order_release);

    out += ",\"engine_walk_type\":";
    out += std::to_string(static_cast<unsigned>(regenny::OT_MODEL));
    out += ",\"engine_walk_type_name\":\"";
    out += sdk::object_type_name(regenny::OT_MODEL);
    out += "\",\"engine_walk_count\":";
    out += std::to_string(walk_count);
    out += ",\"engine_walk_generation\":";
    out += std::to_string(walk_generation);

    // Bounded sample from the first non-empty bucket, proving the transforms
    // are really reachable (not just that the list walks).
    sdk::CClientMgr::ObjectSnapshot snaps[4]{};
    long long sample_bucket = -1;
    size_t got = 0;
    for (size_t t = 0; t < buckets; ++t) {
        const auto n = mgr->snapshot_objects(static_cast<sdk::ObjectType>(t), snaps, std::size(snaps));
        if (n.has_value() && *n > 0) {
            sample_bucket = static_cast<long long>(t);
            got = *n;
            break;
        }
    }
    out += ",\"sample_bucket\":";
    out += std::to_string(sample_bucket);
    out += ",\"sample\":[";
    for (size_t i = 0; i < got; ++i) {
        if (i != 0) {
            out += ",";
        }
        char b[384];
        const auto& s = snaps[i];
        // rotation magnitude: an independent correctness signal -- a wrong
        // offset would not yield a unit quaternion.
        const double mag = std::sqrt(static_cast<double>(s.rotation[0]) * s.rotation[0] +
                                     static_cast<double>(s.rotation[1]) * s.rotation[1] +
                                     static_cast<double>(s.rotation[2]) * s.rotation[2] +
                                     static_cast<double>(s.rotation[3]) * s.rotation[3]);
        snprintf(b, sizeof(b),
                 "{\"address\":\"0x%08" PRIXPTR "\",\"vtable\":\"0x%08" PRIXPTR "\","
                 "\"type\":%u,\"type_name\":\"%s\",\"handle\":%u,"
                 "\"pos\":[%f,%f,%f],\"rot\":[%f,%f,%f,%f],\"rot_magnitude\":%f}",
                 s.address, s.vtable,
                 static_cast<unsigned>(s.type), sdk::object_type_name(s.type), s.handle,
                 s.position[0], s.position[1], s.position[2],
                 s.rotation[0], s.rotation[1], s.rotation[2], s.rotation[3], mag);
        out += b;
    }
    out += "]}";
    return out;
}

// Diagnostics only -- goes entirely through sdk::interfaces' own registry and
// the generated per-interface classes. Reports, for every interface name
// recovered from FEAR2.exe, how many holders requested it, how many of their
// slots currently hold a pointer, whether those agree, and the value.
//
// A null value is NOT a failure: the engine's interface database fills slots
// via APIFound() and clears them via APIRemoved(), so server-side interfaces
// read null in a client-only session and everything reads null before module
// resolution. `all_agree` false WOULD be an anomaly -- two holders for the
// same interface disagreeing means our decode picked up something wrong.
std::string build_interfaces_json() {
    auto& reg = sdk::interfaces::Registry::get();
    const bool ok = reg.initialize();

    char head[256];
    snprintf(head, sizeof(head),
             "{\"ok\":%s,\"ctor\":\"0x%08" PRIXPTR "\",\"call_sites\":%zu,"
             "\"holders\":%zu,\"names\":%zu,\"expected_names\":%zu,\"interfaces\":[",
             ok ? "true" : "false", reg.ctor_addr(), reg.call_sites_seen(),
             reg.holders().size(), reg.names().size(),
             sdk::interfaces::all_interface_count());
    std::string out = head;

    // Drive the GENERATED descriptor table, whose entries call each interface
    // class's own typed getter. That exercises all 36 public API paths, so a
    // miswired kName or a getter bound to the wrong instance shows up here
    // instead of being masked by a registry lookup with the right string.
    size_t resolved_count = 0, slot1_shaped_count = 0;
    std::string iltclient_slot1, iclientshell_slot1, iltmodel_slot1;
    const auto* entries = sdk::interfaces::all_interfaces();
    const size_t count = sdk::interfaces::all_interface_count();
    for (size_t i = 0; i < count; ++i) {
        if (i != 0) {
            out += ",";
        }
        const auto& e = entries[i];
        const auto a = e.agreement();
        void* via_getter = e.get();
        // What slot 1's body actually is, per object, measured rather than assumed. The helper reads a
        // constant-return shape without calling anything, so a mismatch is information ("this object's
        // slot 1 is not that shape") and never a wrong invocation.
        const auto slot1 = sdk::interfaces::slot1_constant_string(via_getter);
        // Counted HERE, where the loop already visits every entry. A consumer of this endpoint should
        // not have to re-derive these by splitting the array back apart.
        if (via_getter != nullptr) {
            ++resolved_count;
            if (slot1.has_value()) {
                ++slot1_shaped_count;
            }
        }
        if (strcmp(e.name, "ILTClient.Default") == 0 && slot1.has_value()) {
            iltclient_slot1 = *slot1;
        }
        if (strcmp(e.name, "IClientShell.Default") == 0 && slot1.has_value()) {
            iclientshell_slot1 = *slot1;
        }
        // The model interface this project's skeleton work goes through. Reported so the suite can pin the
        // implementing class, not just that something resolved.
        if (strcmp(e.name, "ILTModel.Client") == 0 && slot1.has_value()) {
            iltmodel_slot1 = *slot1;
        }
        char b[512];
        snprintf(b, sizeof(b),
                 "{\"name\":\"%s\",\"holders\":%zu,\"non_null\":%zu,\"all_agree\":%s,"
                 "\"value\":\"0x%08" PRIXPTR "\",\"getter\":\"0x%08" PRIXPTR "\","
                 "\"getter_matches\":%s,\"slot1\":\"%s\"}",
                 e.name, a.total, a.non_null, a.all_agree ? "true" : "false",
                 reinterpret_cast<uintptr_t>(a.value),
                 reinterpret_cast<uintptr_t>(via_getter),
                 (via_getter == a.value) ? "true" : "false",
                 slot1.has_value() ? slot1->c_str() : "");
        out += b;
    }
    out += "],";
    char tail[256];
    snprintf(tail, sizeof(tail),
             "\"resolved\":%zu,\"slot1_constant_strings\":%zu,"
             "\"iltclient_slot1\":\"%s\",\"iclientshell_slot1\":\"%s\","
             "\"iltmodel_slot1\":\"%s\"}",
             resolved_count, slot1_shaped_count, iltclient_slot1.c_str(), iclientshell_slot1.c_str(),
             iltmodel_slot1.c_str());
    out += tail;
    return out;
}

// The engine's named shader parameters, entirely through sdk::ShaderParams -- no raw
// pointer arithmetic here, which is the point of the class holding the fault-guarded
// reads rather than this reporter.
//
// Available at the MAIN MENU, unlike most of what this server reports: the parameter list
// is static exe data, so it does not wait on a level. What DOES wait is binding -- every
// record reads kUnboundBinding until the engine assigns handles.
// Append "key":value pairs to a growing JSON object body. Exists because two fixed snprintf
// fragments in this reporter overflowed as recon fields were added, and a truncated fragment
// invalidates the WHOLE response -- a diagnostic that grows every session should not have a
// hand-maintained size.
void json_append_bool(std::string& out, const char* key, bool value) {
    out += '"';
    out += key;
    out += "\":";
    out += value ? "true" : "false";
    out += ',';
}

void json_append_double(std::string& out, const char* key, double value, int decimals = 4) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%.*f", decimals, value);
    out += '"';
    out += key;
    out += "\":";
    out += buf;
    out += ',';
}

std::string build_shader_params_json() {
    const auto head = sdk::ShaderParams::list_head_address();
    if (head == 0) {
        return "{\"ok\":false,\"error\":\"exe not mapped\"}";
    }
    const auto params = sdk::ShaderParams::all();

    std::string out = "{\"ok\":true,\"list_head\":";
    char num[64];
    snprintf(num, sizeof(num), "\"0x%08zX\",\"count\":%zu,", static_cast<size_t>(head),
             params.size());
    out += num;

    size_t bound = 0, pending = 0, size_disagrees = 0;
    out += "\"params\":[";
    for (size_t i = 0; i < params.size(); ++i) {
        const auto& p = params[i];
        if (p.bound()) {
            ++bound;
        }
        if (p.has_pending_upload()) {
            ++pending;
        }
        if (!p.size_agrees_with_type()) {
            ++size_disagrees;
        }
        char b[320];
        snprintf(b, sizeof(b),
                 "%s{\"name\":\"%s\",\"type\":\"%s\",\"type_id\":%u,\"size\":%u,"
                 "\"elements\":%zu,\"binding\":%u,\"bound\":%s,\"pending\":%u,"
                 "\"address\":\"0x%08zX\"}",
                 i == 0 ? "" : ",", p.name.c_str(),
                 sdk::ShaderParams::type_name(p.type), static_cast<unsigned>(p.type),
                 static_cast<unsigned>(p.size), p.element_count(),
                 static_cast<unsigned>(p.binding_index), p.bound() ? "true" : "false",
                 static_cast<unsigned>(p.pending), static_cast<size_t>(p.address));
        out += b;
    }
    out += "],";

    // A few reads a VR mod actually wants, exercised through the typed accessors so the
    // report proves the accessors work and not merely that the list walks.
    const auto res = sdk::ShaderParams::screen_resolution();
    const auto obj_to_clip = sdk::ShaderParams::matrix4x4("k_mObjectToClip");
    const auto nodes = sdk::ShaderParams::matrix4x3_array("k_mModelObjectNodes");
    // Must be refused: same type as k_mObjectToWorld but an array, so the fixed-size
    // accessor has to decline rather than hand back the first of many.
    const auto array_via_fixed = sdk::ShaderParams::matrix4x3("k_mModelObjectNodes");

    // The scene camera record, taken as ONE snapshot through sdk::SceneCamera. Every predicate
    // reported here is the snapshot's own method, so a consumer validating what it read runs
    // exactly this code rather than a copy of it.
    const auto scam = sdk::SceneCamera::snapshot();

    // Recomposition through the snapshot's own view_projection_is_coherent(): the engine's
    // projection (+0x78) and view (+0x48), run through our transcription of
    // LTMatrix_Mul4x4ByAffine, must reproduce the view-projection it stored at +0xB8.
    //
    // Reported, NOT reimplemented here. The 16-coefficient comparison used to live in this
    // reporter, which meant a consumer wanting the same coherence check could not reach it -- and
    // it is the most useful check on this record, spanning three regions the render thread writes
    // at different moments.
    const bool compose_matches_record = scam.has_value() && scam->view_projection_is_coherent();

    // The pass setup anchors, resolved from the live vtable. Reported as exe-relative offsets so the
    // fixture can compare them against what static reversing recorded without knowing the base.
    const auto* exe_mod = sdk::Modules::get().exe();
    const uintptr_t exe_base = exe_mod != nullptr ? exe_mod->base : 0;
    const auto anchor_offset = [exe_base](uintptr_t address) -> long long {
        if (address == 0 || exe_base == 0 || address < exe_base) {
            return -1;
        }
        return static_cast<long long>(address - exe_base);
    };
    using Slot = sdk::SceneCamera::RendererSlot;
    const auto persp_fn = sdk::SceneCamera::renderer_fn(Slot::SetupPassPerspective);
    const auto affine_fn = sdk::SceneCamera::renderer_fn(Slot::SetupPassAffine);
    const auto stored_fn = sdk::SceneCamera::renderer_fn(Slot::SetupPassStored);
    const auto begin_frame_fn = sdk::SceneCamera::renderer_fn(Slot::BeginFrame);
    const auto begin_target_fn = sdk::SceneCamera::renderer_fn(Slot::BeginRenderTarget);
    const auto end_target_fn = sdk::SceneCamera::renderer_fn(Slot::EndRenderTarget);
    const auto renderer_state = sdk::SceneCamera::state();
    const auto frame_clock = sdk::ShaderParams::frame_time();
    const auto clock_agrees = sdk::ShaderParams::frame_time_matches_engine_clock();
    const auto engine_clock = sdk::Engine::client_time();

    // The engine's built-in settings table. Reported structurally -- how many entries walked, whether
    // the two typed accessors refuse the wrong type -- rather than by value, since these are user
    // settings and a machine's numbers are not this suite's business.
    const auto engine_vars = sdk::EngineVars::all();
    const auto pause_physics = sdk::EngineVars::find("PausePhysics");
    const auto finalize_ms = sdk::EngineVars::read_float("MaxFinalizeTimeMS");
    const auto rate = sdk::EngineVars::read_int("PhysicsClientUpdateRate");
    // Typed refusal: MaxFinalizeTimeMS is a float, so an int read must fail, and vice versa.
    const bool refuses_wrong_type =
        !sdk::EngineVars::read_int("MaxFinalizeTimeMS").has_value() &&
        !sdk::EngineVars::read_float("PhysicsClientUpdateRate").has_value();
    size_t vars_in_exe = 0;
    size_t vars_string = 0, vars_float = 0, vars_int = 0, vars_spaced = 0;
    for (const auto& v : engine_vars) {
        if (v.address != 0 && v.type <= 2) {
            ++vars_in_exe;
        }
        if (v.type == 0) {
            ++vars_string;
        } else if (v.type == 1) {
            ++vars_float;
        } else if (v.type == 2) {
            ++vars_int;
        }
    }
    // THE MEASURED BASIS FOR "type 0 is a POINTER, not a buffer": every string entry has another
    // variable's storage exactly 4 bytes above it, so its slot is 4 bytes wide. Recomputed here rather
    // than asserted from a note, so the claim is checked against the live table on every run.
    for (const auto& v : engine_vars) {
        if (v.type != 0) {
            continue;
        }
        for (const auto& other : engine_vars) {
            if (other.address == v.address + 4) {
                ++vars_spaced;
                break;
            }
        }
    }
    const auto end_fn = sdk::SceneCamera::renderer_fn(Slot::EndPass);
    const auto draw_fn = sdk::SceneCamera::renderer_fn(Slot::DrawScene);
    const auto draw_list_fn = sdk::SceneCamera::renderer_fn(Slot::DrawObjectList);

    // screen_to_clip against the viewport transform the rect implies, and a pixel round trip: the
    // viewport centre must map to clip (0,0) whenever the two really are inverses.
    const bool s2c_inverts_viewport =
        scam.has_value() && scam->screen_to_clip_inverts_viewport();
    bool centre_maps_to_origin = false;
    if (scam.has_value() && scam->viewport_valid()) {
        const float cx = static_cast<float>(scam->viewport_left) +
                         static_cast<float>(scam->viewport_width()) * 0.5f;
        const float cy = static_cast<float>(scam->viewport_top) +
                         static_cast<float>(scam->viewport_height()) * 0.5f;
        const auto clip = scam->pixel_to_clip(cx, cy);
        centre_maps_to_origin =
            clip.has_value() && fabsf(clip->x) < 1e-3f && fabsf(clip->y) < 1e-3f;
    }

    // The world-to-screen matrix against the viewport transform the record's own rect implies. Ties
    // four separately-written regions, and holds in whatever pass is live.
    const bool world_to_screen_coherent =
        scam.has_value() && scam->world_to_screen_is_coherent();

    // Projection through that matrix. In the screen pass it is the identity, so a point projects to
    // ITSELF -- which is a real round-trip check rather than a tautology, because the identity is the
    // product of the ortho and the viewport transform and any error in either breaks it.
    bool projects_identity = false;
    bool rejects_behind_camera = false;
    if (scam.has_value() && scam->pose_is_identity()) {
        const auto p = scam->project_point(123.0f, -45.0f, 1.0f);
        if (p.has_value()) {
            projects_identity = fabsf(p->x - 123.0f) < 0.01f && fabsf(p->y + 45.0f) < 0.01f;
        }
        // In the screen pass w is the constant m[3][3], so nothing is "behind" the camera and the
        // refusal cannot be exercised here at all -- see the synthetic perspective probe below, which
        // is where that contract is actually tested.
        rejects_behind_camera = !scam->w_is_view_space_depth() ||
                                !scam->project_point(0.0f, 0.0f, -1.0f).has_value();
    }
    // Sized with headroom and CHECKED: an earlier 448 silently truncated once the projection
    // classifiers were added, and a half-written object is invalid JSON that fails downstream as
    // "missing field" rather than as "the report is broken".
    char sc[1024];
    int sc_len = 0;
    if (scam.has_value()) {
        sc_len = snprintf(sc, sizeof(sc),
                 "\"scene_camera\":true,\"sc_mode\":%u,\"sc_vp_w\":%lld,\"sc_vp_h\":%lld,"
                 "\"sc_viewport_valid\":%s,\"sc_view_identity\":%s,\"sc_perspective\":%s,"
                 "\"sc_normalized_ortho\":%s,\"sc_ortho_matches_viewport\":%s,"
                 "\"sc_proj_off_x\":%.4f,"
                 "\"sc_proj_off_y\":%.4f,\"sc_hvp_x\":%.4f,\"sc_hvp_y\":%.4f,"
                 "\"sc_depth_min\":%.4f,\"sc_depth_max\":%.4f,"
                 "\"sc_pose_rot_unit\":%s,\"sc_pose_pos_finite\":%s,"
                 "\"sc_pose_x\":%.3f,\"sc_pose_y\":%.3f,\"sc_pose_z\":%.3f,"
                 "\"sc_pose_qw\":%.4f,\"sc_pose_identity\":%s,"
                 "\"sc_compose_matches_record\":%s,\"sc_view_matches_pose\":%s,"
                 "\"sc_w2s_coherent\":%s,\"sc_projects_identity\":%s,"
                 "\"sc_s2c_inverts_viewport\":%s,\"sc_centre_to_origin\":%s,"
                 "\"sc_rejects_behind\":%s,"
                 "\"sc_affine\":%s,\"sc_w_row_scale\":%.6f,\"sc_fov_present\":%s,"
                 "\"sc_fov_y_deg\":%.3f,\"sc_proj_agrees_hvp\":%s,",
                 scam->mode, static_cast<long long>(scam->viewport_width()),
                 static_cast<long long>(scam->viewport_height()),
                 scam->viewport_valid() ? "true" : "false",
                 scam->view_is_identity() ? "true" : "false",
                 scam->is_perspective_projection() ? "true" : "false",
                 scam->is_normalized_orthographic_projection() ? "true" : "false",
                 scam->projection_matches_viewport_ortho() ? "true" : "false",
                 scam->proj_center_offset_x, scam->proj_center_offset_y,
                 scam->half_view_plane_x, scam->half_view_plane_y,
                 scam->depth_min, scam->depth_max,
                 scam->pose_rotation_is_unit() ? "true" : "false",
                 scam->pose_position_is_finite() ? "true" : "false",
                 scam->pose.position.x, scam->pose.position.y, scam->pose.position.z,
                 scam->pose.rotation.w, scam->pose_is_identity() ? "true" : "false",
                 compose_matches_record ? "true" : "false",
                 scam->view_matches_pose() ? "true" : "false",
                 world_to_screen_coherent ? "true" : "false",
                 projects_identity ? "true" : "false",
                 s2c_inverts_viewport ? "true" : "false",
                 centre_maps_to_origin ? "true" : "false",
                 rejects_behind_camera ? "true" : "false",
                 scam->is_affine_projection() ? "true" : "false",
                 scam->projection_w_row_scale(),
                 scam->fov_y_radians().has_value() ? "true" : "false",
                 scam->fov_y_radians().has_value()
                     ? (*scam->fov_y_radians() * 57.2957795f) : 0.0f,
                 scam->projection_agrees_with_half_view_plane() ? "true" : "false");
    } else {
        sc_len = snprintf(sc, sizeof(sc), "\"scene_camera\":false,");
    }
    if (sc_len < 0 || static_cast<size_t>(sc_len) >= sizeof(sc)) {
        return "{\"ok\":false,\"error\":\"scene camera fragment truncated\"}";
    }
    out += sc;

    // EXERCISING THE PERSPECTIVE PATH, which sampling cannot reach: the engine leaves the record
    // in its affine screen pass between frames, so fov_*_radians() and the projection/half-plane
    // identity never run on live data. Building a matrix with SceneCamera's own builder -- itself a
    // transcription of LTMatrix_BuildPerspectiveProjection -- runs the real predicates on a matrix
    // of known shape. This is a check of the CLASS, and not runtime corroboration of the engine.
    //
    // The scaled repeat is the point of the exercise: homogeneous matrices are scale-equivalent, so
    // the classifier and the FOV must survive multiplying every coefficient by a constant.
    constexpr float kProbeHalfX = 2.2651f;
    constexpr float kProbeHalfY = 0.6371f;
    sdk::SceneCameraSnapshot probe{};
    probe.half_view_plane_x = kProbeHalfX;
    probe.half_view_plane_y = kProbeHalfY;
    const auto probe_matrix =
        sdk::SceneCamera::make_perspective_projection(kProbeHalfX, kProbeHalfY, 4.3f);
    if (probe_matrix.has_value()) {
        probe.projection = *probe_matrix;
    }
    const auto probe_fov_y = probe.fov_y_radians();

    sdk::SceneCameraSnapshot scaled = probe;
    for (auto& coefficient : scaled.projection) {
        coefficient *= 137.5f;
    }
    const auto scaled_fov_y = scaled.fov_y_radians();

    sdk::SceneCameraSnapshot affine_probe{};
    affine_probe.half_view_plane_x = kProbeHalfX;
    affine_probe.half_view_plane_y = kProbeHalfY;
    const auto affine_matrix =
        sdk::SceneCamera::make_affine_projection(kProbeHalfX, kProbeHalfY, 4.3f, 100000.0f);
    if (affine_matrix.has_value()) {
        affine_probe.projection = *affine_matrix;
    }

    // REJECTION COVERAGE. A builder that quietly accepts a degenerate frustum is the failure this
    // guards, so the probe asserts the refusals as well as the successes.
    const bool rejects_zero_extent =
        !sdk::SceneCamera::make_perspective_projection(0.0f, kProbeHalfY, 4.3f).has_value();
    const bool rejects_negative_extent =
        !sdk::SceneCamera::make_perspective_projection(kProbeHalfX, -1.0f, 4.3f).has_value();
    // A finite POSITIVE extent whose reciprocal is not finite: input validation alone passes this.
    const bool rejects_tiny_extent =
        !sdk::SceneCamera::make_perspective_projection(1e-40f, kProbeHalfY, 4.3f).has_value();
    const bool rejects_zero_span =
        !sdk::SceneCamera::make_affine_projection(kProbeHalfX, kProbeHalfY, 4.3f, 4.3f).has_value();

    // THE COMPOSE, exercised on cases whose answers are known independently of the code under
    // test. Identity must leave a matrix alone; a pure translation in the affine operand must land
    // in column 3 scaled by the projection's own row -- out[0][3] == m00*tx, which follows from the
    // convention rather than from running this function.
    bool compose_identity_ok = false;
    bool compose_translation_ok = false;
    bool compose_keeps_perspective = false;
    if (probe_matrix.has_value()) {
        const auto ident = sdk::SceneCamera::multiply_by_affine(*probe_matrix,
                                                               sdk::SceneCamera::affine_identity());
        compose_identity_ok = true;
        for (size_t i = 0; i < 16; ++i) {
            if (fabsf(ident[i] - (*probe_matrix)[i]) > 1e-5f) {
                compose_identity_ok = false;
            }
        }
        std::array<float, 12> translate = sdk::SceneCamera::affine_identity();
        constexpr float kTx = 11.0f, kTy = -3.0f, kTz = 7.0f;
        translate[3] = kTx;
        translate[7] = kTy;
        translate[11] = kTz;
        const auto moved = sdk::SceneCamera::multiply_by_affine(*probe_matrix, translate);
        const float m00 = (*probe_matrix)[0];
        const float m11 = (*probe_matrix)[5];
        constexpr float kNear = 4.3f;

        // ALL SIXTEEN COEFFICIENTS, from the closed form of P * T rather than from running the
        // function. Checking only columns 0 and 1 of column 3 would miss the HOMOGENEOUS ROW, which
        // is the part of the convention actually being mapped: a bug in row 3 passes both the
        // identity case and a translation checked only at [3] and [7].
        //
        //   row 2 of P is (0, 0, 1, -near)  ->  out[2][3] = tz - near
        //   row 3 of P is (0, 0, 1,  0)     ->  out[3][3] = tz, out[3][2] = 1
        const std::array<float, 16> want = {
            m00,  0.0f, 0.0f, m00 * kTx,
            0.0f, m11,  0.0f, m11 * kTy,
            0.0f, 0.0f, 1.0f, kTz - kNear,
            0.0f, 0.0f, 1.0f, kTz,
        };
        compose_translation_ok = true;
        for (size_t i = 0; i < 16; ++i) {
            const float allow = fabsf(want[i]) * 1e-4f + 1e-5f;
            if (!(fabsf(moved[i] - want[i]) <= allow)) {
                compose_translation_ok = false;
            }
        }

        sdk::SceneCameraSnapshot composed{};
        composed.projection = moved;
        composed.half_view_plane_x = kProbeHalfX;
        composed.half_view_plane_y = kProbeHalfY;
        compose_keeps_perspective = composed.is_perspective_projection();
    }

    // THE INVERSE, CHECKED BY ROUND TRIP on a deliberately non-trivial pose. Composing a transform
    // with its own inverse must give the identity, which catches a wrong conjugate sign, a
    // transposed rotation or a mis-signed translation -- none of which the live comparison can see
    // while the engine's pose is identity.
    //
    // The quaternion below is normalised so the conjugate really is the inverse, since that is the
    // assumption the engine's own inversion makes.
    regenny::LTNodeTransform probe_pose{};
    probe_pose.position.x = 137.5f;
    probe_pose.position.y = -42.25f;
    probe_pose.position.z = 8.0f;
    {
        // An arbitrary rotation, then normalised.
        float qx = 0.3f, qy = -0.5f, qz = 0.2f, qw = 0.78f;
        const float n = sqrtf(qx * qx + qy * qy + qz * qz + qw * qw);
        probe_pose.rotation.x = qx / n;
        probe_pose.rotation.y = qy / n;
        probe_pose.rotation.z = qz / n;
        probe_pose.rotation.w = qw / n;
    }
    // Round trip through the snapshot class's own predicate -- the promotion, multiply and identity
    // comparison used to live here, which meant a consumer wanting to validate a pose it built
    // could not reach them.
    const bool inverse_round_trips = sdk::SceneCamera::view_inverts_pose(probe_pose);
    // The same pose pushed far from the origin, which is what exposes a translation tolerance that
    // does not scale: the residual of column 3's cancellation grows with |position|.
    regenny::LTNodeTransform distant_pose = probe_pose;
    distant_pose.position.x = 98000.0f;
    distant_pose.position.y = -75500.0f;
    distant_pose.position.z = 43000.0f;
    const bool distant_round_trips = sdk::SceneCamera::view_inverts_pose(distant_pose);
    // A NaN tolerance must be REFUSED, not silently accepted: every comparison against NaN is
    // false, so an unvalidated predicate would call any pair of matrices inverses.
    bool rejects_nan_tolerance = true;
    {
        const auto fwd = sdk::SceneCamera::transform_to_matrix(probe_pose);
        const auto inv = sdk::SceneCamera::view_matrix_from_pose(probe_pose);
        if (fwd.has_value() && inv.has_value()) {
            const float nan_value = std::numeric_limits<float>::quiet_NaN();
            rejects_nan_tolerance =
                !sdk::SceneCamera::affines_are_inverse(*fwd, *inv, nan_value) &&
                !sdk::SceneCamera::affines_are_inverse(*fwd, *inv, 1e-4f, nan_value);
        }
    }
    // THE REGRESSION TEST FOR THE HARDCODED-2 BUG. rotation_matrix used a fixed factor of 2 where the
    // engine divides by |q|^2. Every rotation the game exposes is unit, so that bug passed the entire
    // suite -- the only thing that catches it is a NON-UNIT quaternion, where the two formulas differ:
    // scaling q must not change R(q), because a scaled quaternion is the same rotation.
    bool rotation_scale_invariant = false;
    bool rotation_rejects_zero = false;
    bool rotation_rejects_nonfinite = false;
    {
        regenny::LTRotation q{};
        q.x = 0.3f; q.y = -0.5f; q.z = 0.2f; q.w = 0.78f;  // deliberately NOT normalised
        regenny::LTRotation scaled{};
        constexpr float kScale = 3.7f;
        scaled.x = q.x * kScale; scaled.y = q.y * kScale;
        scaled.z = q.z * kScale; scaled.w = q.w * kScale;
        const auto a = sdk::rotation_matrix(q);
        const auto b = sdk::rotation_matrix(scaled);
        if (a.has_value() && b.has_value()) {
            rotation_scale_invariant = true;
            for (size_t i = 0; i < 12; ++i) {
                if (fabsf(a->m[i] - b->m[i]) > 1e-5f) {
                    rotation_scale_invariant = false;
                }
            }
        }
        regenny::LTRotation zero{};
        rotation_rejects_zero = !sdk::rotation_matrix(zero).has_value();
        regenny::LTRotation bad{};
        bad.x = std::numeric_limits<float>::quiet_NaN();
        bad.w = 1.0f;
        rotation_rejects_nonfinite = !sdk::rotation_matrix(bad).has_value();
    }

    // QUATERNION <-> MATRIX ROUND TRIP. rotation_matrix and rotation_from_matrix are independent
    // transcriptions of two different engine functions (LTRotation_ToMatrix3x4 and
    // LTRotation_FromMatrix3x4), so requiring them to invert each other catches a sign or index error in
    // either -- which no single-direction test would.
    //
    // Both branches of the conversion are exercised: a small rotation takes the trace path, and a
    // 180-degree one about an axis drives the trace negative and takes the largest-diagonal path.
    int quat_roundtrip_failures = 0;
    int quat_branches_covered = 0;
    {
        // (angle, axis) pairs: the second and third have negative trace, so they exercise the fallback.
        const float axes[3][4] = {
            {0.7f, 0.3f, -0.5f, 0.2f},   // arbitrary small-ish rotation
            {3.1415f, 1.0f, 0.0f, 0.0f}, // 180 about x  -> trace = -1
            {3.1415f, 0.0f, 0.0f, 1.0f}, // 180 about z  -> trace = -1
        };
        for (const auto& spec : axes) {
            const float half = spec[0] * 0.5f;
            const float sn = sinf(half);
            float ax = spec[1], ay = spec[2], az = spec[3];
            const float len = sqrtf(ax * ax + ay * ay + az * az);
            if (len <= 0.0f) {
                continue;
            }
            ax /= len; ay /= len; az /= len;
            regenny::LTRotation q{};
            q.x = ax * sn; q.y = ay * sn; q.z = az * sn; q.w = cosf(half);

            const auto m = sdk::rotation_matrix(q);
            if (!m.has_value()) {
                ++quat_roundtrip_failures;
                continue;
            }
            regenny::LTMatrix3x4 mm{};
            for (size_t i = 0; i < 12; ++i) {
                mm.m[i] = m->m[i];
            }
            if (mm.m[0] + mm.m[5] + mm.m[10] < -0.999f) {
                ++quat_branches_covered;  // the fallback branch really was taken
            }
            const auto back = sdk::SceneCamera::rotation_from_matrix(mm);
            if (!back.has_value()) {
                ++quat_roundtrip_failures;
                continue;
            }
            // q and -q are the same rotation, so compare the MATRICES rather than the components.
            const auto m2 = sdk::rotation_matrix(*back);
            if (!m2.has_value()) {
                ++quat_roundtrip_failures;
                continue;
            }
            for (size_t i = 0; i < 12; ++i) {
                if (fabsf(m2->m[i] - mm.m[i]) > 2e-3f) {
                    ++quat_roundtrip_failures;
                    break;
                }
            }
        }
    }

    // THE PASS ARGUMENT MODEL. Both helpers reproduce engine behaviour a consumer has to predict before
    // calling slot 15, so both the normal case and the CLAMPING are checked -- the engine clamps rather
    // than rejects, and a helper that refused instead would mispredict every out-of-range request.
    bool fov_tan_ok = false;
    bool fov_clamps_high = false;
    bool fov_clamps_negative = false;
    bool rect_halves_ok = false;
    bool rect_clamps_ok = false;
    {
        // 90 degrees -> tan(45) = 1.
        const auto ninety = sdk::SceneCamera::predicted_half_view_plane(1.57079633f, 1.57079633f);
        fov_tan_ok = ninety.has_value() && fabsf((*ninety)[0] - 1.0f) < 1e-3f &&
                     fabsf((*ninety)[1] - 1.0f) < 1e-3f;

        // Above the ceiling the engine clamps to 179 degrees, so the result must equal the ceiling's.
        const auto ceiling = sdk::SceneCamera::predicted_half_view_plane(
            sdk::SceneCamera::kMaxFovRadians, sdk::SceneCamera::kMaxFovRadians);
        const auto over = sdk::SceneCamera::predicted_half_view_plane(3.2f, 3.2f);
        fov_clamps_high = ceiling.has_value() && over.has_value() &&
                          fabsf((*over)[0] - (*ceiling)[0]) < 1e-2f;

        const auto negative = sdk::SceneCamera::predicted_half_view_plane(-1.0f, -1.0f);
        fov_clamps_negative = negative.has_value() && fabsf((*negative)[0]) < 1e-4f;

        // A side-by-side pair: the two halves of a 5120x1440 target.
        const auto left = sdk::SceneCamera::predicted_viewport_pixels({0.0f, 0.0f, 0.5f, 1.0f},
                                                                     5120, 1440);
        const auto right = sdk::SceneCamera::predicted_viewport_pixels({0.5f, 0.0f, 1.0f, 1.0f},
                                                                      5120, 1440);
        rect_halves_ok = left.has_value() && right.has_value() &&
                         (*left)[0] == 0 && (*left)[2] == 2560 &&
                         (*right)[0] == 2560 && (*right)[2] == 5120 &&
                         (*left)[1] == 0 && (*left)[3] == 1440;

        // Out-of-range components clamp to the full target rather than overflowing it.
        const auto over_rect = sdk::SceneCamera::predicted_viewport_pixels({-1.0f, -1.0f, 2.0f, 2.0f},
                                                                          5120, 1440);
        rect_clamps_ok = over_rect.has_value() && (*over_rect)[0] == 0 && (*over_rect)[1] == 0 &&
                         (*over_rect)[2] == 5120 && (*over_rect)[3] == 1440;
    }

    // THE LOOK-AT, VERIFIED BY WHAT IT MUST DO rather than by matching the transcription to itself:
    // rotating +Z by the result must reproduce the requested forward direction. That is the property a
    // consumer depends on, and it fails if either cross is flipped, the basis columns are swapped, or
    // the quaternion conversion is wrong -- the whole chain at once.
    int lookat_failures = 0;
    bool lookat_identity = false;
    bool lookat_handles_parallel = false;
    {
        // Canonical case first: forward +Z with up +Y is the identity basis, so the identity rotation.
        const auto ident = sdk::SceneCamera::rotation_from_forward_up(0, 0, 1, 0, 1, 0);
        lookat_identity = ident.has_value() && fabsf(ident->w - 1.0f) < 1e-3f &&
                          fabsf(ident->x) < 1e-3f && fabsf(ident->y) < 1e-3f &&
                          fabsf(ident->z) < 1e-3f;

        // A spread of directions: rotating +Z by the result must land on `forward`.
        const float dirs[5][3] = {
            {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f},
            {0.577f, 0.577f, 0.577f}, {-0.3f, 0.8f, 0.5f},
        };
        for (const auto& d : dirs) {
            const float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            const float fx = d[0] / len, fy = d[1] / len, fz = d[2] / len;
            const auto q = sdk::SceneCamera::rotation_from_forward_up(fx, fy, fz, 0, 1, 0);
            if (!q.has_value()) {
                ++lookat_failures;
                continue;
            }
            const auto m = sdk::rotation_matrix(*q);
            if (!m.has_value()) {
                ++lookat_failures;
                continue;
            }
            // Column 2 of the basis IS forward, and R * (0,0,1) reads column 2 out again.
            const float rx = m->m[2], ry = m->m[6], rz = m->m[10];
            if (fabsf(rx - fx) > 2e-3f || fabsf(ry - fy) > 2e-3f || fabsf(rz - fz) > 2e-3f) {
                ++lookat_failures;
            }
        }

        // An up hint PARALLEL to forward: the engine swizzles rather than failing, so this must still
        // produce a usable rotation whose forward is correct.
        const auto par = sdk::SceneCamera::rotation_from_forward_up(0, 1, 0, 0, 1, 0);
        if (par.has_value()) {
            const auto m = sdk::rotation_matrix(*par);
            lookat_handles_parallel = m.has_value() && fabsf(m->m[2]) < 2e-3f &&
                                      fabsf(m->m[6] - 1.0f) < 2e-3f && fabsf(m->m[10]) < 2e-3f;
        }
    }

    // Finite input, overflowing output: FLT_MAX positions make the dot products infinite.
    regenny::LTNodeTransform huge_pose = probe_pose;
    huge_pose.position.x = std::numeric_limits<float>::max();
    huge_pose.position.y = std::numeric_limits<float>::max();
    huge_pose.position.z = std::numeric_limits<float>::max();
    const bool rejects_overflow_pose =
        !sdk::SceneCamera::invert_transform(huge_pose).has_value();

    // EVERY TOLERANCE-TAKING PREDICATE, SWEPT AT ONCE. Two things are proven per predicate: that a
    // deliberately WRONG snapshot is rejected at a sane tolerance, and that a NaN and a +inf
    // tolerance are both refused rather than used.
    //
    // Both directions matter because the failure mode depends on how each comparison happens to be
    // spelled. `deviation > tolerance` accepts everything on NaN; a relative form built on near_equal
    // rejects on NaN but accepts everything on +inf. Nothing in the signatures distinguishes them, so
    // the sweep is the only way to know all eight behave the same.
    int tolerance_guard_failures = 0;
    int mismatch_detect_failures = 0;
    {
        const float nan_value = std::numeric_limits<float>::quiet_NaN();
        const float inf_value = std::numeric_limits<float>::infinity();

        // A snapshot whose parts deliberately disagree: a non-identity view, a projection that is not
        // built from its half-extents, a view-projection that is not their product, and a pose that
        // does not imply that view.
        sdk::SceneCameraSnapshot bad{};
        bad.viewport_left = 0;
        bad.viewport_top = 0;
        bad.viewport_right = 1920;
        bad.viewport_bottom = 1080;
        bad.half_view_plane_x = 3.0f;
        bad.half_view_plane_y = 1.0f;
        bad.view = regenny::LTMatrix3x4{};
        bad.view.m[0] = 0.5f;   // not identity
        bad.view.m[5] = 1.0f;
        bad.view.m[10] = 1.0f;
        bad.view.m[3] = 25.0f;  // and translated
        bad.projection = {7.0f, 0, 0, 0, 0, 9.0f, 0, 0, 0, 0, 1.0f, -2.0f, 0, 0, 1.0f, 0};
        bad.view_projection = {};  // certainly not projection * view
        bad.pose.rotation.w = 1.0f;
        bad.pose.position.x = 100.0f;  // a pose that does not imply `view`

        // Each must SEE the mismatch at a sane tolerance.
        if (bad.view_is_identity(1e-4f)) ++mismatch_detect_failures;
        if (bad.view_projection_is_coherent(1e-4f)) ++mismatch_detect_failures;
        if (bad.view_matches_pose(1e-3f)) ++mismatch_detect_failures;
        if (bad.pose_is_identity(1e-4f)) ++mismatch_detect_failures;
        if (bad.projection_agrees_with_half_view_plane(0.02f)) ++mismatch_detect_failures;
        if (bad.projection_matches_viewport_ortho(0.02f)) ++mismatch_detect_failures;

        // And each must REFUSE an unusable tolerance rather than act on it. Checked against the bad
        // snapshot, so "accepted the tolerance" and "accepted the data" cannot cancel out.
        for (const float bogus : {nan_value, inf_value, -1.0f}) {
            if (bad.view_is_identity(bogus)) ++tolerance_guard_failures;
            if (bad.view_projection_is_coherent(bogus)) ++tolerance_guard_failures;
            if (bad.view_matches_pose(bogus)) ++tolerance_guard_failures;
            if (bad.pose_is_identity(bogus)) ++tolerance_guard_failures;
            if (bad.pose_rotation_is_unit(bogus)) ++tolerance_guard_failures;
            if (bad.projection_agrees_with_half_view_plane(bogus)) ++tolerance_guard_failures;
            if (bad.projection_matches_viewport_ortho(bogus)) ++tolerance_guard_failures;
        }
        // The same sweep on a GOOD snapshot: a bogus tolerance must still be refused, so the guard is
        // not being satisfied merely because the data was wrong.
        const auto good_view = sdk::SceneCamera::view_matrix_from_pose(probe_pose);
        if (good_view.has_value()) {
            sdk::SceneCameraSnapshot good{};
            good.pose = probe_pose;
            good.view = *good_view;
            for (const float bogus : {nan_value, inf_value}) {
                if (good.view_matches_pose(bogus)) ++tolerance_guard_failures;
                if (good.pose_rotation_is_unit(bogus)) ++tolerance_guard_failures;
            }
            // ...and at a sane tolerance it must PASS, so the predicate is not simply always false.
            if (!good.view_matches_pose(1e-3f)) ++mismatch_detect_failures;
        }
    }

    // THE BEHIND-CAMERA CONTRACT, on a synthetic PERSPECTIVE snapshot -- the only place it means
    // anything. A point in front must project; one behind must be refused. The live record cannot test
    // this: in its affine screen pass w is a positive constant, so every point "passes".
    bool perspective_projects_front = false;
    bool perspective_rejects_behind = false;
    bool perspective_w_is_depth = false;
    bool affine_w_is_not_depth = false;
    {
        sdk::SceneCameraSnapshot ps{};
        const auto pm = sdk::SceneCamera::make_perspective_projection(1.0f, 1.0f, 1.0f);
        if (pm.has_value()) {
            ps.world_to_screen = *pm;  // project directly through the perspective matrix
            perspective_w_is_depth = ps.w_is_view_space_depth();
            const auto front = ps.project_point(0.0f, 0.0f, 10.0f);
            const auto behind = ps.project_point(0.0f, 0.0f, -10.0f);
            perspective_projects_front = front.has_value() && front->w > 9.9f && front->w < 10.1f;
            perspective_rejects_behind = !behind.has_value();
        }
        sdk::SceneCameraSnapshot as{};
        const auto am = sdk::SceneCamera::make_affine_projection(1.0f, 1.0f, 1.0f, 100.0f);
        if (am.has_value()) {
            as.world_to_screen = *am;
            affine_w_is_not_depth = !as.w_is_view_space_depth();
        }
    }

    // A NaN tolerance must REJECT here too. Asserted rather than argued, since "it fails closed" is a
    // claim about the comparison's shape and a later rewrite could invert it silently.
    const bool identity_rejects_nan_tolerance =
        !scam.has_value() ||
        !scam->projection_agrees_with_half_view_plane(std::numeric_limits<float>::quiet_NaN());

    const auto near_err = sdk::SceneCamera::view_inverse_round_trip_error(probe_pose);
    const auto far_err = sdk::SceneCamera::view_inverse_round_trip_error(distant_pose);
    const bool view_from_pose_built =
        sdk::SceneCamera::view_matrix_from_pose(probe_pose).has_value();

    // The value a correct implementation must recover, computed here from the input rather than
    // hard-coded, so the expectation cannot drift away from the probe.
    const float want_fov_y = 2.0f * atanf(kProbeHalfY);

    // Built by append rather than into a fixed buffer: this fragment gains fields every session.
    json_append_bool(out, "probe_perspective", probe.is_perspective_projection());
    json_append_bool(out, "probe_agrees", probe.projection_agrees_with_half_view_plane());
    json_append_double(out, "probe_fov_y", probe_fov_y.value_or(-1.0f), 6);
    json_append_double(out, "probe_fov_y_want", want_fov_y, 6);
    json_append_bool(out, "probe_scaled_perspective", scaled.is_perspective_projection());
    json_append_double(out, "probe_scaled_fov_y", scaled_fov_y.value_or(-1.0f), 6);
    json_append_bool(out, "probe_affine_is_affine", affine_probe.is_affine_projection());
    json_append_bool(out, "probe_affine_fov_present", affine_probe.fov_y_radians().has_value());
    json_append_bool(out, "probe_built", probe_matrix.has_value() && affine_matrix.has_value());
    json_append_bool(out, "probe_rejects_zero_extent", rejects_zero_extent);
    json_append_bool(out, "probe_rejects_negative_extent", rejects_negative_extent);
    json_append_bool(out, "probe_rejects_zero_span", rejects_zero_span);
    json_append_bool(out, "probe_rejects_tiny_extent", rejects_tiny_extent);
    json_append_bool(out, "compose_identity", compose_identity_ok);
    json_append_bool(out, "compose_translation", compose_translation_ok);
    json_append_bool(out, "compose_keeps_perspective", compose_keeps_perspective);
    json_append_bool(out, "inverse_round_trips", inverse_round_trips);
    json_append_bool(out, "view_from_pose_built", view_from_pose_built);
    json_append_bool(out, "distant_round_trips", distant_round_trips);
    json_append_double(out, "pass_persp_off", static_cast<double>(anchor_offset(persp_fn)), 0);
    json_append_double(out, "pass_affine_off", static_cast<double>(anchor_offset(affine_fn)), 0);
    json_append_double(out, "pass_stored_off", static_cast<double>(anchor_offset(stored_fn)), 0);
    json_append_double(out, "pass_end_off", static_cast<double>(anchor_offset(end_fn)), 0);
    json_append_double(out, "pass_draw_off", static_cast<double>(anchor_offset(draw_fn)), 0);
    json_append_double(out, "pass_drawlist_off", static_cast<double>(anchor_offset(draw_list_fn)), 0);
    json_append_double(out, "pass_beginframe_off", static_cast<double>(anchor_offset(begin_frame_fn)), 0);
    json_append_double(out, "pass_begintarget_off", static_cast<double>(anchor_offset(begin_target_fn)), 0);
    json_append_double(out, "pass_endtarget_off", static_cast<double>(anchor_offset(end_target_fn)), 0);
    json_append_double(out, "engine_var_count", static_cast<double>(engine_vars.size()), 0);
    json_append_double(out, "engine_var_wellformed", static_cast<double>(vars_in_exe), 0);
    json_append_double(out, "engine_var_strings", static_cast<double>(vars_string), 0);
    json_append_double(out, "engine_var_floats", static_cast<double>(vars_float), 0);
    json_append_double(out, "engine_var_ints", static_cast<double>(vars_int), 0);
    json_append_double(out, "engine_var_strings_4byte", static_cast<double>(vars_spaced), 0);

    // ---- INPUT SUBSYSTEM ----------------------------------------------------------------------
    //
    // The focus flags first, because one of them decides whether the engine simulates at all. Reported
    // rather than asserted for the ones that track window state: whether this machine's window is
    // active when the suite runs is not the suite's business, but the SHAPE of the device array and the
    // arithmetic of the key queue are.
    const auto focus = sdk::Input::focus();
    json_append_bool(out, "input_focus_readable", focus.has_value());
    if (focus.has_value()) {
        json_append_bool(out, "input_client_active", focus->client_active);
        json_append_bool(out, "input_lost_focus", focus->lost_focus);
        json_append_bool(out, "input_minimized", focus->minimized);
        json_append_bool(out, "input_renderer_shutdown", focus->renderer_shutdown);
        json_append_bool(out, "input_render_initted", focus->render_initted);
    }
    const auto gated = sdk::Input::simulation_is_gated();
    json_append_bool(out, "input_gate_readable", gated.has_value());
    json_append_bool(out, "input_simulation_gated", gated.value_or(false));
    // The gate agrees with the flag by construction; asserted anyway because "gated" is the question a
    // consumer asks and a sign error here would invert every answer this SDK gives about liveness.
    json_append_bool(out, "input_gate_matches_flag",
                     focus.has_value() && gated.has_value() && (*gated == !focus->client_active));
    json_append_double(out, "input_client_active_offset",
                       static_cast<double>(sdk::Input::client_active_address() != 0
                                               ? sdk::Input::client_active_address() -
                                                     sdk::Modules::get().exe()->base
                                               : 0),
                       0);

    const auto input_devices = sdk::Input::devices();
    size_t n_keyboard = 0, n_mouse = 0, n_unknown = 0, n_vt_in_exe = 0;
    const uintptr_t input_exe_base = sdk::Modules::get().exe()->base;
    const size_t input_exe_size = sdk::Modules::get().exe()->size;
    for (const auto& d : input_devices) {
        if (d.kind == sdk::Input::DeviceKind::Keyboard) {
            ++n_keyboard;
        } else if (d.kind == sdk::Input::DeviceKind::Mouse) {
            ++n_mouse;
        } else {
            ++n_unknown;
        }
        // Every device's vtable must live inside the exe: these are engine-side classes, and a vtable
        // outside the image would mean the slot is not what this mapping says it is.
        if (input_exe_base != 0 && d.vtable >= input_exe_base &&
            d.vtable < input_exe_base + input_exe_size) {
            ++n_vt_in_exe;
        }
    }
    json_append_double(out, "input_devices_populated", static_cast<double>(input_devices.size()), 0);
    json_append_double(out, "input_devices_keyboard", static_cast<double>(n_keyboard), 0);
    json_append_double(out, "input_devices_mouse", static_cast<double>(n_mouse), 0);
    json_append_double(out, "input_devices_unknown", static_cast<double>(n_unknown), 0);
    json_append_double(out, "input_devices_vtable_in_exe", static_cast<double>(n_vt_in_exe), 0);

    // Keyboard reads through the SDK's own accessors, so the suite exercises what a consumer calls.
    // A key's state must be one of exactly two values the engine tests for, and the edge helpers must
    // agree with the two banks they derive from -- checked across the whole 256-entry space rather
    // than on a key someone happens to be holding.
    const auto keys = sdk::Input::keys_down();
    json_append_bool(out, "input_keys_readable", keys.has_value());
    json_append_double(out, "input_keys_down_count",
                       static_cast<double>(keys.has_value() ? keys->size() : 0), 0);
    size_t edge_consistent = 0, edge_checked = 0;
    for (uint32_t vk = 0; vk < sdk::Input::kKeyStateCount; ++vk) {
        const auto now = sdk::Input::key_is_down(static_cast<uint8_t>(vk));
        const auto before = sdk::Input::key_was_down(static_cast<uint8_t>(vk));
        const auto pressed = sdk::Input::key_just_pressed(static_cast<uint8_t>(vk));
        const auto released = sdk::Input::key_just_released(static_cast<uint8_t>(vk));
        if (!now.has_value() || !before.has_value() || !pressed.has_value() || !released.has_value()) {
            continue;
        }
        ++edge_checked;
        const bool want_pressed = *now && !*before;
        const bool want_released = !*now && *before;
        // A key cannot be both a press edge and a release edge, which is the invariant that catches a
        // swapped bank far more reliably than comparing values that are usually all zero.
        if (*pressed == want_pressed && *released == want_released && !(*pressed && *released)) {
            ++edge_consistent;
        }
    }
    json_append_double(out, "input_key_edges_checked", static_cast<double>(edge_checked), 0);
    json_append_double(out, "input_key_edges_consistent", static_cast<double>(edge_consistent), 0);

    const auto mouse = sdk::Input::mouse();
    json_append_bool(out, "input_mouse_readable", mouse.has_value());
    if (mouse.has_value()) {
        json_append_bool(out, "input_mouse_look_delta_valid", mouse->look_delta_valid);
        json_append_double(out, "input_mouse_screen_x", static_cast<double>(mouse->screen_x), 0);
        json_append_double(out, "input_mouse_screen_y", static_cast<double>(mouse->screen_y), 0);
        json_append_double(out, "input_mouse_look_dx", mouse->look_delta[0], 2);
        json_append_double(out, "input_mouse_look_dy", mouse->look_delta[1], 2);
    }

    // The queue's two counters and the path selector. The counters are reported, not pinned: they are
    // whatever the last frame left behind. What IS pinned is that they never exceed the capacity the
    // handlers enforce, since that bound is what makes the parallel arrays' extents facts.
    const auto geom = sdk::Input::window_geometry();
    json_append_bool(out, "input_window_geometry_readable", geom.has_value());
    if (geom.has_value()) {
        json_append_double(out, "input_window_width", static_cast<double>(geom->client_width), 0);
        json_append_double(out, "input_window_height", static_cast<double>(geom->client_height), 0);
        json_append_double(out, "input_window_origin_x", static_cast<double>(geom->screen_x), 0);
        json_append_double(out, "input_window_origin_y", static_cast<double>(geom->screen_y), 0);
        json_append_bool(out, "input_window_geometry_iconic", geom->iconic);
    }
    const auto iconic = sdk::Input::window_is_iconic();
    json_append_bool(out, "input_window_readable", sdk::Input::main_window() != 0);
    json_append_bool(out, "input_window_iconic_readable", iconic.has_value());
    json_append_bool(out, "input_window_iconic", iconic.value_or(false));

    // ---- THE DEVICE VTABLES' EXTENT -------------------------------------------------------------
    //
    // 11 slots each, and every entry must be engine code. This is the check that would have caught the
    // ten-slot reading: a table recorded one slot short still resolves fine, so nothing fails -- but a
    // table recorded one slot LONG reads past its end into the neighbouring object, and since .rdata
    // packs these contiguously the overrun still looks like a function pointer. Requiring exactly 11
    // in-exe entries pins the extent from the side that can actually fail.
    const auto kb_slots = sdk::Input::device_vtable_entries(sdk::Input::DeviceKind::Keyboard);
    const auto ms_slots = sdk::Input::device_vtable_entries(sdk::Input::DeviceKind::Mouse);
    json_append_double(out, "input_keyboard_vtable_slots", static_cast<double>(kb_slots.size()), 0);
    json_append_double(out, "input_mouse_vtable_slots", static_cast<double>(ms_slots.size()), 0);
    // The two tables are distinct objects: identical entries would mean one device is being read twice.
    json_append_bool(out, "input_device_vtables_distinct",
                     !kb_slots.empty() && !ms_slots.empty() && kb_slots != ms_slots);
    // Slot 10 is Reset, and it must differ between the two -- the mouse's is its constructor's helper,
    // the keyboard's zeroes its banks.
    json_append_bool(out, "input_device_reset_differs",
                     kb_slots.size() > sdk::Input::kDeviceSlotReset &&
                         ms_slots.size() > sdk::Input::kDeviceSlotReset &&
                         kb_slots[sdk::Input::kDeviceSlotReset] !=
                             ms_slots[sdk::Input::kDeviceSlotReset]);

    // ---- THE ENGINE'S PUBLIC CLASSIFIER, AGAINST THE LOCAL MIRROR -------------------------------
    //
    // classify_object() reimplements four lines of engine code. Slot 23 IS those four lines. Comparing
    // them across the whole interesting id space is what makes the mirror trustworthy rather than merely
    // plausible -- and it covers the fallthrough, where an id past the joystick range must resolve to the
    // keyboard rather than being rejected.
    size_t cls_checked = 0, cls_agrees = 0;
    for (int id : {0, 1, 27, 65, 255, 256, 999, 1000, 1001, 1005, 1006, 1007, 1500, 1999,
                   2000, 2010, 2021, 2022, 3000, 5000}) {
        const auto engine_idx = sdk::Input::engine_object_device_index(id);
        if (!engine_idx.has_value()) {
            continue;
        }
        ++cls_checked;
        // The engine returns a device INDEX; the mirror returns a class. Keyboard is index 0, mouse 1,
        // and joystick ids resolve to kind+2 which for the inert kind -1 lands at 1 -- so those are
        // compared only on the two the mirror can claim without knowing a set's kind.
        const auto cls = sdk::Input::classify_object(id);
        bool agree = false;
        if (cls == sdk::Input::ObjectClass::Keyboard) {
            agree = (*engine_idx == 0);
        } else if (cls == sdk::Input::ObjectClass::Mouse) {
            agree = (*engine_idx == 1);
        } else {
            agree = true;  // joystick: the index depends on a binding set's kind, not on the id alone
        }
        if (agree) {
            ++cls_agrees;
        }
    }
    json_append_double(out, "input_classify_checked", static_cast<double>(cls_checked), 0);
    json_append_double(out, "input_classify_agrees", static_cast<double>(cls_agrees), 0);

    // Key names come from the engine (GetKeyNameTextW underneath). Reported as a count of vks that name
    // themselves rather than as specific strings, since the names are locale- and layout-dependent.
    size_t named_keys = 0;
    for (uint32_t vk = 1; vk < 256; ++vk) {
        const auto nm = sdk::Input::key_name(static_cast<uint8_t>(vk));
        if (nm.has_value() && !nm->empty()) {
            ++named_keys;
        }
    }
    json_append_double(out, "input_named_keys", static_cast<double>(named_keys), 0);
    json_append_bool(out, "input_key_name_rejects_zero", !sdk::Input::key_name(0).has_value());

    // ---- THE ENGINE'S OWN VIEW OF ITS DEVICES ---------------------------------------------------
    //
    // Both answers come through the ILTInput vtable, so they are independent of the array walk above.
    // GetDeviceCount returning something other than kDeviceSlots would mean this SDK is iterating the
    // wrong number of slots; IsDevicePresent disagreeing with the walk would mean it is iterating the
    // wrong ADDRESS. Neither error is visible from inside either method alone.
    const auto engine_dev_count = sdk::Input::engine_device_count();
    json_append_bool(out, "input_engine_device_count_readable", engine_dev_count.has_value());
    json_append_double(out, "input_engine_device_count",
                       static_cast<double>(engine_dev_count.value_or(0)), 0);

    size_t presence_checked = 0, presence_agrees = 0;
    for (size_t i = 0; i < sdk::Input::kDeviceSlots; ++i) {
        const auto present = sdk::Input::device_is_present(i);
        if (!present.has_value()) {
            continue;
        }
        ++presence_checked;
        bool walked = false;
        for (const auto& d : input_devices) {
            if (d.slot == i) {
                walked = true;
                break;
            }
        }
        if (*present == walked) {
            ++presence_agrees;
        }
    }
    json_append_double(out, "input_presence_checked", static_cast<double>(presence_checked), 0);
    json_append_double(out, "input_presence_agrees", static_cast<double>(presence_agrees), 0);

    // ---- THE BINDING SETS -----------------------------------------------------------------------
    //
    // THE OWNER BACK-POINTER IS THE LOAD-BEARING CHECK: every record carries the address of its own set
    // header, so if the stride or the records base were wrong, the walk would produce records whose owner
    // does not match. That is an invariant the data itself supplies, unlike a count someone wrote down.
    const auto sets = sdk::Input::binding_sets();
    size_t set_records = 0, owner_ok = 0, n_bound = 0, with_handler = 0, inert_sets = 0;
    for (const auto& set : sets) {
        if (set.is_inert()) {
            ++inert_sets;
        }
        for (const auto& rec : set.entries) {
            ++set_records;
            if (rec.owner == set.address) {
                ++owner_ok;
            }
            if (rec.is_bound()) {
                ++n_bound;
            }
            if (rec.has_handler()) {
                ++with_handler;
            }
        }
    }
    json_append_double(out, "input_binding_sets", static_cast<double>(sets.size()), 0);
    json_append_double(out, "input_binding_records", static_cast<double>(set_records), 0);
    json_append_double(out, "input_binding_owner_ok", static_cast<double>(owner_ok), 0);
    json_append_double(out, "input_binding_bound", static_cast<double>(n_bound), 0);
    json_append_double(out, "input_binding_with_handler", static_cast<double>(with_handler), 0);
    json_append_double(out, "input_binding_inert_sets", static_cast<double>(inert_sets), 0);
    json_append_double(out, "input_binding_first_count",
                       static_cast<double>(sets.empty() ? 0 : sets.front().record_count), 0);
    json_append_double(out, "input_binding_first_kind",
                       static_cast<double>(sets.empty() ? -99 : sets.front().kind), 0);
    json_append_double(out, "input_bound_actions",
                       static_cast<double>(sdk::Input::bound_actions().size()), 0);

    // WHAT the bound records are bound TO, by the engine's own classifier. Worth counting rather than
    // assuming: if any bind a 2000-range object, then the game registers joystick bindings that
    // LTInput_ObjectChanged can never fire, which is a real asymmetry rather than a mapping error.
    size_t bind_kb = 0, bind_mouse = 0, bind_joy = 0, bind_mods = 0, bind_alts = 0;
    uint32_t first_action = 0xFFFFFFFFu;
    int32_t first_primary = -1;
    for (const auto& rec : sdk::Input::bound_actions()) {
        for (const int32_t id : {rec.primary, rec.alternate}) {
            if (id == -1) {
                continue;
            }
            switch (sdk::Input::classify_object(id)) {
            case sdk::Input::ObjectClass::Keyboard: ++bind_kb; break;
            case sdk::Input::ObjectClass::Mouse: ++bind_mouse; break;
            default: ++bind_joy; break;
            }
        }
        if (rec.alternate != -1) {
            ++bind_alts;
        }
        if (rec.primary_modifier != -1 || rec.alternate_modifier != -1) {
            ++bind_mods;
        }
        if (first_action == 0xFFFFFFFFu) {
            first_action = rec.action_code;
            first_primary = rec.primary;
        }
    }
    json_append_double(out, "input_bind_keyboard", static_cast<double>(bind_kb), 0);
    json_append_double(out, "input_bind_mouse", static_cast<double>(bind_mouse), 0);
    json_append_double(out, "input_bind_joystick", static_cast<double>(bind_joy), 0);
    json_append_double(out, "input_bind_with_alternate", static_cast<double>(bind_alts), 0);
    json_append_double(out, "input_bind_with_modifier", static_cast<double>(bind_mods), 0);
    json_append_double(out, "input_bind_first_action", static_cast<double>(first_action), 0);
    json_append_double(out, "input_bind_first_primary", static_cast<double>(first_primary), 0);

    // THE ENGINE'S OWN ACTIVITY JUDGEMENT versus one computed here from the same inputs. For an
    // unmodified binding the engine asks whether the object's value is non-zero; this reproduces that
    // through object_value(). Records with a modifier are skipped: those read a latch inside the set's
    // modifier-state vector, and none are in use on this build, so including them would compare two
    // paths that both answer "no" for the same trivial reason.
    size_t active_checked = 0, active_agrees = 0, modifier_records = 0;
    for (const auto& set : sets) {
        for (const auto& rec : set.entries) {
            if (rec.primary_modifier != -1 || rec.alternate_modifier != -1) {
                ++modifier_records;
                continue;
            }
            const auto engine_active = sdk::Input::engine_binding_is_active(rec.address);
            if (!engine_active.has_value()) {
                continue;
            }
            bool mine = false;
            for (const int32_t id : {rec.primary, rec.alternate}) {
                if (id == -1) {
                    continue;
                }
                const auto v = sdk::Input::object_value(id);
                if (v.has_value() && *v != 0.0f) {
                    mine = true;
                }
            }
            ++active_checked;
            if (mine == *engine_active) {
                ++active_agrees;
            }
        }
    }
    json_append_double(out, "input_active_checked", static_cast<double>(active_checked), 0);
    json_append_double(out, "input_active_agrees", static_cast<double>(active_agrees), 0);
    json_append_double(out, "input_modifier_records", static_cast<double>(modifier_records), 0);

    // ---- THE OBJECT NAMESPACE, CROSS-CHECKED AGAINST THE RAW BANKS ----------------------------
    //
    // Two genuinely independent paths to the same state: object_value() calls the device's own vtable
    // getter, while key_is_down() reads the state byte directly. They must agree on every key, and a
    // disagreement would mean one of the two mappings is wrong -- which is exactly the error neither
    // path can detect alone.
    size_t obj_checked = 0, obj_value_agrees = 0, obj_prev_agrees = 0, obj_changed_agrees = 0;
    for (uint32_t vk = 0; vk < sdk::Input::kKeyStateCount; ++vk) {
        const auto now = sdk::Input::key_is_down(static_cast<uint8_t>(vk));
        const auto before = sdk::Input::key_was_down(static_cast<uint8_t>(vk));
        const auto value = sdk::Input::object_value(static_cast<int>(vk));
        const auto prev = sdk::Input::object_previous_value(static_cast<int>(vk));
        const auto changed = sdk::Input::object_changed(static_cast<int>(vk));
        if (!now.has_value() || !before.has_value() || !value.has_value() || !prev.has_value() ||
            !changed.has_value()) {
            continue;
        }
        ++obj_checked;
        if ((*value == 1.0f) == *now) {
            ++obj_value_agrees;
        }
        if ((*prev == 1.0f) == *before) {
            ++obj_prev_agrees;
        }
        // The engine's change test is `previous != current`, so it must equal the edge computed from the
        // two banks this SDK reads separately.
        if (*changed == (*now != *before)) {
            ++obj_changed_agrees;
        }
    }
    json_append_double(out, "input_object_keys_checked", static_cast<double>(obj_checked), 0);
    json_append_double(out, "input_object_value_agrees", static_cast<double>(obj_value_agrees), 0);
    json_append_double(out, "input_object_prev_agrees", static_cast<double>(obj_prev_agrees), 0);
    json_append_double(out, "input_object_changed_agrees", static_cast<double>(obj_changed_agrees), 0);

    // The mouse side of the same comparison, plus the two facts that are structural rather than
    // stateful: position axes never report a change, and joystick ids are refused.
    size_t mouse_btn_checked = 0, mouse_btn_agrees = 0;
    const auto mouse_for_obj = sdk::Input::mouse();
    if (mouse_for_obj.has_value()) {
        for (int i = 0; i < 3; ++i) {
            const auto value = sdk::Input::object_value(1000 + i);
            if (!value.has_value()) {
                continue;
            }
            ++mouse_btn_checked;
            if ((*value == 1.0f) == mouse_for_obj->buttons[static_cast<size_t>(i)]) {
                ++mouse_btn_agrees;
            }
        }
    }
    json_append_double(out, "input_object_mouse_btn_checked", static_cast<double>(mouse_btn_checked), 0);
    json_append_double(out, "input_object_mouse_btn_agrees", static_cast<double>(mouse_btn_agrees), 0);

    // 1005/1006 read the very same floats MouseState::axis exposes, so these must be bit-identical --
    // a weaker result would mean the axis offsets are wrong in one of the two readings.
    const auto axis0 = sdk::Input::object_value(1005);
    const auto axis1 = sdk::Input::object_value(1006);
    json_append_bool(out, "input_object_axis_matches",
                     mouse_for_obj.has_value() && axis0.has_value() && axis1.has_value() &&
                         *axis0 == mouse_for_obj->axis[0] && *axis1 == mouse_for_obj->axis[1]);

    // The position axes: the engine computes exactly what look_delta reproduces, so when the SDK is
    // willing to report a delta at all the two must match. While the window is iconic the SDK refuses
    // and the engine's getter still returns its ~34480 garbage, so there is nothing to compare.
    const auto pos_x = sdk::Input::object_value(1003);
    const auto pos_y = sdk::Input::object_value(1004);
    json_append_bool(out, "input_object_position_readable", pos_x.has_value() && pos_y.has_value());
    json_append_bool(out, "input_object_position_matches_delta",
                     mouse_for_obj.has_value() && mouse_for_obj->look_delta_valid &&
                         pos_x.has_value() && pos_y.has_value() &&
                         *pos_x == mouse_for_obj->look_delta[0] &&
                         *pos_y == mouse_for_obj->look_delta[1]);

    const auto ch1003 = sdk::Input::object_changed(1003);
    const auto ch1004 = sdk::Input::object_changed(1004);
    json_append_bool(out, "input_object_position_never_changes",
                     ch1003.has_value() && ch1004.has_value() && !*ch1003 && !*ch1004);

    // Joystick ids are refused by this SDK because the dispatch rejects them too.
    json_append_bool(out, "input_object_joystick_refused",
                     !sdk::Input::object_value(2000).has_value() &&
                         !sdk::Input::object_value(2021).has_value());

    // The namespace's boundaries, including the keyboard FALLTHROUGH: an id past the joystick range is
    // not rejected, it reads as a keyboard object. Encoded as a single bool so a boundary slip anywhere
    // in the classifier fails one check.
    using OC = sdk::Input::ObjectClass;
    json_append_bool(out, "input_object_classify_boundaries",
                     sdk::Input::classify_object(0) == OC::Keyboard &&
                         sdk::Input::classify_object(999) == OC::Keyboard &&
                         sdk::Input::classify_object(1000) == OC::Mouse &&
                         sdk::Input::classify_object(1006) == OC::Mouse &&
                         sdk::Input::classify_object(1007) == OC::Keyboard &&
                         sdk::Input::classify_object(1999) == OC::Keyboard &&
                         sdk::Input::classify_object(2000) == OC::Joystick &&
                         sdk::Input::classify_object(2021) == OC::Joystick &&
                         sdk::Input::classify_object(2022) == OC::Keyboard &&
                         sdk::Input::classify_object(5000) == OC::Keyboard);

    const auto chain = sdk::Input::wndproc_chain();
    json_append_bool(out, "input_wndproc_readable", chain.has_value());
    if (chain.has_value()) {
        json_append_bool(out, "input_wndproc_saved_is_engine", chain->saved_is_engine);
        json_append_bool(out, "input_wndproc_engine_owns_window", chain->engine_owns_window);
        json_append_double(out, "input_wndproc_current_offset",
                           static_cast<double>(chain->current >= input_exe_base &&
                                                       chain->current < input_exe_base + input_exe_size
                                                   ? chain->current - input_exe_base
                                                   : 0),
                           0);
    }
    // Whether the exe owns GWL_WNDPROC. The owner's NAME stays on the SDK (WndProcChain::current_owner)
    // for a consumer to print: this endpoint emits bools and doubles only, and adding a string emitter
    // for one diagnostic is not worth a new convention.
    json_append_bool(out, "input_wndproc_owner_is_exe",
                     chain.has_value() && chain->current >= input_exe_base &&
                         chain->current < input_exe_base + input_exe_size);
    const auto in_enabled = sdk::Input::input_is_enabled();
    json_append_bool(out, "input_enabled_readable", in_enabled.has_value());
    json_append_bool(out, "input_enabled", in_enabled.value_or(false));
    // The published pointer must BE the array's address: the engine stores the array's own base there.
    json_append_bool(out, "input_device_array_published_matches",
                     sdk::Input::device_array_address() != 0 &&
                         sdk::Input::device_array_address() == sdk::Input::published_device_array());
    const auto eps = sdk::Input::entry_points();
    json_append_bool(out, "input_entry_points_resolved", eps.all_resolved());

    const auto downs = sdk::Input::pending_key_downs();
    const auto ups = sdk::Input::pending_key_ups();
    const auto drained = sdk::Input::key_queue_is_drained();
    json_append_double(out, "input_queue_downs", static_cast<double>(downs.size()), 0);
    json_append_double(out, "input_queue_ups", static_cast<double>(ups.size()), 0);
    json_append_bool(out, "input_queue_drain_readable", drained.has_value());
    json_append_bool(out, "input_queue_is_drained", drained.value_or(false));
    json_append_bool(out, "engine_var_pause_physics_found", pause_physics.has_value());
    // Reported as an exe-relative OFFSET, like the vtable anchors: an absolute address would encode
    // this machine's load base into the suite.
    json_append_double(out, "engine_var_pause_physics_off",
                       static_cast<double>(anchor_offset(
                           pause_physics.has_value() ? pause_physics->address : 0)), 0);
    json_append_bool(out, "engine_var_float_read", finalize_ms.has_value());
    json_append_double(out, "engine_var_float_value", finalize_ms.value_or(-1.0f), 4);
    json_append_bool(out, "engine_var_int_read", rate.has_value());
    json_append_bool(out, "engine_var_refuses_wrong_type", refuses_wrong_type);
    json_append_double(out, "frame_time", frame_clock.value_or(-1.0f), 5);
    json_append_double(out, "engine_seconds",
                       engine_clock.has_value() ? engine_clock->seconds : -1.0, 5);
    json_append_bool(out, "clock_cross_check_available", clock_agrees.has_value());
    json_append_bool(out, "clock_cross_check", clock_agrees.value_or(false));
    json_append_double(out, "renderer_state",
                       static_cast<double>(renderer_state.has_value()
                                               ? static_cast<long long>(*renderer_state) : -1),
                       0);
    json_append_double(out, "near_rot_err", near_err.has_value() ? near_err->rotation : -1.0, 8);
    json_append_double(out, "near_trans_err", near_err.has_value() ? near_err->translation : -1.0, 8);
    json_append_double(out, "far_rot_err", far_err.has_value() ? far_err->rotation : -1.0, 8);
    json_append_double(out, "far_trans_err", far_err.has_value() ? far_err->translation : -1.0, 8);
    json_append_bool(out, "rejects_nan_tolerance", rejects_nan_tolerance);
    json_append_bool(out, "rejects_overflow_pose", rejects_overflow_pose);
    json_append_bool(out, "rotation_scale_invariant", rotation_scale_invariant);
    json_append_bool(out, "rotation_rejects_zero", rotation_rejects_zero);
    json_append_bool(out, "rotation_rejects_nonfinite", rotation_rejects_nonfinite);
    json_append_bool(out, "quat_roundtrips", quat_roundtrip_failures == 0);
    json_append_double(out, "quat_branches", static_cast<double>(quat_branches_covered), 0);
    json_append_bool(out, "lookat_forward_ok", lookat_failures == 0);
    json_append_bool(out, "lookat_identity", lookat_identity);
    json_append_bool(out, "lookat_parallel_ok", lookat_handles_parallel);
    json_append_bool(out, "fov_tan_ok", fov_tan_ok);
    json_append_bool(out, "fov_clamps_high", fov_clamps_high);
    json_append_bool(out, "fov_clamps_negative", fov_clamps_negative);
    json_append_bool(out, "rect_halves_ok", rect_halves_ok);
    json_append_bool(out, "rect_clamps_ok", rect_clamps_ok);
    json_append_bool(out, "identity_rejects_nan_tolerance", identity_rejects_nan_tolerance);
    json_append_bool(out, "tolerance_guards_hold", tolerance_guard_failures == 0);
    json_append_bool(out, "mismatch_detected", mismatch_detect_failures == 0);
    json_append_bool(out, "persp_projects_front", perspective_projects_front);
    json_append_bool(out, "persp_rejects_behind", perspective_rejects_behind);
    json_append_bool(out, "persp_w_is_depth", perspective_w_is_depth);
    json_append_bool(out, "affine_w_is_not_depth", affine_w_is_not_depth);

    // The camera parameters, through the same accessors a stereo path would use. The
    // reciprocal check is the class's own helper, not re-implemented here -- a consumer
    // validating what it read calls exactly this.
    const auto hvp = sdk::ShaderParams::half_view_plane();
    const auto zr = sdk::ShaderParams::z_range();
    const auto cam_dir = sdk::ShaderParams::world_space_camera_dir();
    char cam[384];
    snprintf(cam, sizeof(cam),
             "\"half_view_plane\":%s,\"hvp_half_w\":%.4f,\"hvp_half_h\":%.4f,"
             "\"hvp_reciprocals_consistent\":%s,\"hvp_aspect\":%.4f,"
             "\"z_range\":%s,\"z_near\":%.4f,\"z_far\":%.1f,\"camera_dir\":%s,",
             hvp.has_value() ? "true" : "false", hvp.has_value() ? hvp->half_width : 0.0f,
             hvp.has_value() ? hvp->half_height : 0.0f,
             (hvp.has_value() && hvp->reciprocals_consistent()) ? "true" : "false",
             hvp.has_value() ? hvp->aspect() : 0.0f,
             zr.has_value() ? "true" : "false", zr.has_value() ? (*zr)[0] : 0.0f,
             zr.has_value() ? (*zr)[1] : 0.0f, cam_dir.has_value() ? "true" : "false");
    out += cam;

    char tail[512];
    snprintf(tail, sizeof(tail),
             "\"screen_res\":%s,\"screen_res_w\":%.1f,\"screen_res_h\":%.1f,"
             "\"object_to_clip_readable\":%s,\"model_nodes_elements\":%zu,"
             "\"array_refused_by_fixed_accessor\":%s,"
             // Distinct names ON PURPOSE: every entry in "params" already carries "bound"
             // and "pending", and a naive JSON reader that finds the first match would
             // parse a per-param boolean as this summary count. The fixture did exactly
             // that and reported -1.
             "\"bound_count\":%zu,\"pending_count\":%zu,\"size_disagrees\":%zu}",
             res.has_value() ? "true" : "false", res.has_value() ? (*res)[0] : 0.0f,
             res.has_value() ? (*res)[1] : 0.0f,
             obj_to_clip.has_value() ? "true" : "false", nodes.size(),
             array_via_fixed.has_value() ? "false" : "true", bound, pending, size_disagrees);
    out += tail;
    return out;
}

// Diagnostics only -- goes entirely through sdk::DatabaseMgr's own methods
// (category_count/category/record_count/record/*_name), never raw
// pointer/offset arithmetic here. Builds a small JSON array of up to
// `limit` {"name","record_count"} category summaries starting at
// `start_index`, walking the REAL mapped-struct traversal in-process.
std::string build_category_list_json(const regenny::DatabaseMgrSubRecord* database, size_t start_index, size_t limit) {
    std::string out = "[";
    const size_t total = sdk::DatabaseMgr::category_count(database);
    bool first = true;
    for (size_t i = start_index; i < total && i < start_index + limit; ++i) {
        auto* cat = sdk::DatabaseMgr::category(database, i);
        if (cat == nullptr) {
            continue;
        }
        if (!first) {
            out += ",";
        }
        first = false;
        out += "{\"name\":";
        json_escape_append(out, sdk::DatabaseMgr::category_name(cat));
        out += ",\"record_count\":" + std::to_string(sdk::DatabaseMgr::record_count(cat)) + "}";
    }
    out += "]";
    return out;
}

// Same shape for a single category's records.
std::string build_record_list_json(const regenny::DatabaseMgrCategory* category, size_t limit) {
    std::string out = "[";
    const size_t total = sdk::DatabaseMgr::record_count(category);
    bool first = true;
    for (size_t i = 0; i < total && i < limit; ++i) {
        auto* rec = sdk::DatabaseMgr::record(category, i);
        if (rec == nullptr) {
            continue;
        }
        if (!first) {
            out += ",";
        }
        first = false;
        json_escape_append(out, sdk::DatabaseMgr::record_name(rec));
    }
    out += "]";
    return out;
}

// Diagnostics only -- goes entirely through sdk::DatabaseMgr's own methods
// (regenny() fields, entry_count(), entry(i), read_path()); NEVER raw
// pointer/offset arithmetic here. This deliberately EXERCISES the real
// mapped-struct traversal in-process (the actual code path any future mod
// feature would use), not just reports addresses -- that's the point: prove
// the mapping works and doesn't crash the game, not just that it parses.
//
// Also exercises the category/record enumeration layer added alongside
// DatabaseMgrCategory/DatabaseMgrRecord: up to 5 category summaries
// (name + record_count) for record_a, and up to 5 record names from the
// FIRST category that actually has records -- proves both traversal levels
// against live data, not just that entry0's top-level fields parse.
std::string build_database_json() {
    auto* db = sdk::DatabaseMgr::get();
    if (db == nullptr) {
        return "{\"ok\":false,\"error\":\"DatabaseMgr::get() returned null\"}";
    }
    auto* r = db->regenny();
    const size_t count = db->entry_count();

    std::string entry0_json = "null";
    if (count >= 1) {
        if (auto* e = db->entry(0); e != nullptr) {
            const std::string path_a = sdk::DatabaseMgr::read_path(e->record_a);
            const std::string path_b = sdk::DatabaseMgr::read_path(e->record_b);
            entry0_json = "{\"record_a\":\"0x";
            char hexbuf[24];
            snprintf(hexbuf, sizeof(hexbuf), "%08" PRIXPTR, reinterpret_cast<uintptr_t>(e->record_a));
            entry0_json += hexbuf;
            entry0_json += "\",\"record_b\":\"0x";
            snprintf(hexbuf, sizeof(hexbuf), "%08" PRIXPTR, reinterpret_cast<uintptr_t>(e->record_b));
            entry0_json += hexbuf;
            entry0_json += "\",\"record_a_path\":";
            json_escape_append(entry0_json, path_a);
            entry0_json += ",\"record_b_path\":";
            json_escape_append(entry0_json, path_b);
            entry0_json += ",\"record_a_category_count\":" + std::to_string(sdk::DatabaseMgr::category_count(e->record_a));
            entry0_json += ",\"categories\":";
            entry0_json += build_category_list_json(e->record_a, 0, 5);

            // First category with any records at all, walked linearly (no
            // hardcoded index assumption -- which category has records can
            // differ per loaded .gamedb).
            const size_t cat_total = sdk::DatabaseMgr::category_count(e->record_a);
            std::string sample_records_json = "null";
            std::string sample_category_name;
            for (size_t i = 0; i < cat_total; ++i) {
                auto* cat = sdk::DatabaseMgr::category(e->record_a, i);
                if (cat != nullptr && sdk::DatabaseMgr::record_count(cat) > 0) {
                    sample_category_name = sdk::DatabaseMgr::category_name(cat);
                    sample_records_json = build_record_list_json(cat, 5);
                    break;
                }
            }
            entry0_json += ",\"sample_records_category\":";
            json_escape_append(entry0_json, sample_category_name);
            entry0_json += ",\"sample_records\":";
            entry0_json += sample_records_json;

            entry0_json += "}";
        }
    }

    char buf[512];
    snprintf(buf, sizeof(buf),
             "{\"ok\":true,\"instance\":\"0x%08" PRIXPTR "\",\"vtable\":\"0x%08" PRIXPTR "\","
             "\"unk_04\":%u,"
             "\"array_begin\":\"0x%08" PRIXPTR "\",\"array_end\":\"0x%08" PRIXPTR "\","
             "\"array_cap_end\":\"0x%08" PRIXPTR "\","
             "\"unk_14\":%u,\"entry_count\":%zu,",
             reinterpret_cast<uintptr_t>(db),
             reinterpret_cast<uintptr_t>(r->vtable),
             r->unk_04,
             reinterpret_cast<uintptr_t>(r->array_begin),
             reinterpret_cast<uintptr_t>(r->array_end),
             reinterpret_cast<uintptr_t>(r->array_cap_end),
             r->unk_14, count);
    std::string out = buf;
    out += "\"entry0\":";
    out += entry0_json;
    out += "}";
    return out;
}

std::string build_engine_hook_json(const std::string& name) {
    void* out = nullptr;
    const int rc = sdk::Engine::get_engine_hook(name.c_str(), &out);
    char buf[256];
    snprintf(buf, sizeof(buf), "{\"ok\":true,\"rc\":%d,\"value\":\"0x%08" PRIXPTR "\"}", rc,
             reinterpret_cast<uintptr_t>(out));
    return buf;
}

} // namespace

Framework::Framework(void* self_module, int32_t ipc_port) : m_self(self_module), m_ipc_port(ipc_port) {}

bool Framework::initialize() {
    bool expected = false;
    if (!m_initialized.compare_exchange_strong(expected, true)) {
        LOGX("[framework] initialize() called twice -- ignored");
        return true;
    }

    LOGX("[framework] initializing (self=%p, ipc port %d)", m_self, m_ipc_port);

    m_sdk_ready = sdk::Modules::get().initialize();
    if (!m_sdk_ready) {
        LOGX("[framework] sdk module init FAILED (missing modules?); continuing degraded");
    }

    // Warm the SDK anchors now so a broken pattern is a loud log line at init,
    // not a silent 0 on first use.
    (void)sdk::CClientMgr::update_fn();
    (void)sdk::CClientShell::update_fn();
    (void)sdk::Engine::get_engine_hook_fn();

    // The frame hook: our first real engine hook, and what makes /health's
    // frame_ticks rise -- the host-side proof the hook pipeline is live.
    const uintptr_t update_target = sdk::CClientShell::update_fn();
    if (update_target != 0) {
        Hooks::get().install("CClientShell::Update", reinterpret_cast<void*>(update_target),
                             reinterpret_cast<void*>(&frame_tick_detour));
    } else {
        LOGX("[framework] CClientShell::Update anchor missing -- frame hook NOT installed");
    }

    Mods::get().on_initialize();

    cmdsrv::Handlers handlers;
    handlers.health = build_health_fragment;
    handlers.targets = build_targets_json;
    handlers.database = build_database_json;
    handlers.objects = build_objects_json;
    handlers.models = build_models_json;
    handlers.interfaces = build_interfaces_json;
    handlers.shader_params = build_shader_params_json;
    handlers.engine_hook = build_engine_hook_json;
    if (!cmdsrv::start(m_ipc_port, std::move(handlers))) {
        LOGX("[framework] IPC server failed to start on port %d (in use?)", m_ipc_port);
        return false;
    }
    LOGX("[framework] IPC command server on http://127.0.0.1:%d", m_ipc_port);
    LOGX("[framework] initialized");
    return true;
}

bool Framework::shutdown() {
    if (m_shutting_down.exchange(true)) {
        LOGX("[framework] shutdown() re-entry -- refusing");
        return false;
    }

    // 2. Stop IPC: joins the socket thread; afterwards no handler is executing.
    cmdsrv::stop();
    LOGX("[framework] IPC stopped");

    // 3. Mods yield the frame path.
    Mods::get().on_shutdown();

    // 4. Retire ALL hooks so no new detour fires during the quiescence scan.
    //    Fail-closed: a failed disable means the DLL must stay mapped.
    const bool hooks_retired = Hooks::get().retire();

    // 5. Prove quiescence: with every other thread suspended, no EIP sits in
    //    our module. Hook InlineHook objects leak on success (straggler-in-
    //    trampoline safety; see Hooks.hpp) -- nothing else to free here.
    const uintptr_t base = reinterpret_cast<uintptr_t>(m_self);
    const size_t size = utility::get_module_size(static_cast<HMODULE>(m_self)).value_or(0);

    if (!hooks_retired) {
        LOGX("[framework] hook retire FAILED -> staying dormant (DLL remains mapped)");
        return false;
    }

    if (base == 0 || size == 0) {
        LOGX("[framework] self-module geometry unknown (base=%p size=%zu) -> staying dormant",
             reinterpret_cast<void*>(base), size);
        return false;
    }

    if (prove_quiescent(base, size, 400)) { // ~2s worst case
        LOGX("[framework] quiescent: no EIP in module -> safe to unmap");
        return true;
    }

    LOGX("[framework] could not confirm quiescence -> staying dormant");
    return false;
}
