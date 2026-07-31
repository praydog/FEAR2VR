#include "Framework.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
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
#include "sdk/ObjectWatch.hpp"
#include "sdk/CClientShell.hpp"
#include "sdk/DatabaseMgr.hpp"
#include "sdk/Delegates.hpp"
#include "sdk/Modules.hpp"
#include "mods/CameraPassHook.hpp"
#include "mods/ConsoleRunner.hpp"
#include "mods/FocusKeeper.hpp"
#include "mods/HeadTracking.hpp"
#include "mods/WeaponAgreement.hpp"
#include "mods/HudPassHook.hpp"
#include "mods/BoneControl.hpp"
#include "mods/WeaponWheel.hpp"
#include "mods/AmmoKeeper.hpp"
#include "mods/FrameCapture.hpp"
#include "mods/ResourceWatch.hpp"
#include "mods/FireRedirect.hpp"
#include "mods/ViewmodelDecouple.hpp"
#include "mods/TurnController.hpp"
#include "ExceptionHandler.hpp"
#include "mods/VR.hpp"
#include "mods/vr/runtimes/SimulatedRuntime.hpp"
#include "mods/Comfort.hpp"
#include "mods/RenderHook.hpp"
#include "mods/SyntheticInput.hpp"
#include "mods/Watchpoints.hpp"
#include "mods/ViewHook.hpp"
#include "sdk/Model.hpp"
#include "sdk/Object.hpp"
#include "sdk/Engine.hpp"
#include "sdk/Events.hpp"
#include "sdk/EngineVars.hpp"
#include "sdk/Input.hpp"
#include "sdk/Common.hpp"
#include "sdk/Console.hpp"
#include "sdk/UiCommands.hpp"
#include "sdk/Memory.hpp"
#include "sdk/Physics.hpp"
#include "sdk/PlayerMgr.hpp"
#include "sdk/WeaponMgr.hpp"
#include "sdk/Resources.hpp"
#include "sdk/Vtables.hpp"
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
    // RECLAIM THE CRASH FILTER FIRST. The engine installs its own and there is only one slot, so
    // ours is periodically displaced -- which is why an access violation inside our own DLL was
    // reported by WER with nothing in our log. One call per frame, and it self-heals.
    exception_handler::reassert();

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
    // Explicit absence -- never an invented zero (see AGENT.MD's fail-closed convention and the
    // /api/* contract's "absent values are null" rule). Used throughout the /api/* builders below.
    JsonFields& n(const char* k) {
        key(k);
        m_out += "null";
        return *this;
    }
    // A value that is ALREADY valid JSON (a nested object or array built into its own std::string
    // first) -- spliced in verbatim rather than escaped as a string. This is what lets the /api/*
    // builders below compose nested shapes (camera.clamps, items arrays, ...) out of JsonFields
    // objects built bottom-up, with no trailing-comma bookkeeping anywhere.
    JsonFields& raw(const char* k, const std::string& json) {
        key(k);
        m_out += json;
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

// Defined further down, beside the other JSON helpers it uses. Declared here because /sdk/targets is built
// before them and the gate's signals must appear in EVERY document a gated check reads -- emitting them from
// one place is what stops a second document drifting out of sync, which is exactly what jammed the gate shut.
void append_world_state(std::string& out);

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
    // IN-PHASE AGREEMENT, measured on the frame callback rather than here.
    //
    // Two independent producers of one point -- the engine's placement of the attached weapon, and our own
    // composition of the hand socket -- can only be compared if they are read in the SAME frame. Read from
    // this thread they are not: a frame boundary lands between them whenever it likes, and the idle animation
    // sways the arm across it, so the disagreement measures WHEN the reads happened rather than whether the
    // arithmetic is right. Sampled live it wandered 0.000 / 0.159 / 2.139 for that reason alone.
    //
    // The frame callback has no such gap. It also records how far the weapon travels per frame, which is the
    // scale any residual has to be judged against: a broken composition is wrong by units no matter how
    // still the player stands, while frame skew shrinks to nothing when nothing moves.
    const auto wa = WeaponAgreement::get().observed();
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
             "\"global_force\":[%f,%f,%f],\"global_force_y\":%.3f,\"global_force_ok\":%s,"
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
             "\"shell_obj\":\"0x%08" PRIXPTR "\","
             "\"muzzle_ok\":%s,\"muzzle_clean\":%s,\"muzzle\":[%.2f,%.2f,%.2f],"
             // Components as well as the array. A consumer differencing the muzzle frame to frame
             // wants scalars rather than a list to parse -- the fixture is one such consumer.
             "\"muzzle_x\":%.3f,\"muzzle_y\":%.3f,\"muzzle_z\":%.3f,"
             "\"muzzle_mdl\":\"%s\",\"weapon_vs_hand\":%.3f,\"muzzle_from_hand\":%.2f,"
             "\"wa_valid\":%s,\"wa_frames\":%llu,\"wa_disagreement\":%.4f,\"wa_step\":%.4f,"
             "\"wa_worst_at_rest\":%.4f,\"wa_still_frames\":%llu,"
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
             // Y ALSO AS A SCALAR. It is the world's scale statement -- 980 units/s^2 is 9.8 m/s^2
             // in centimetres -- and a consumer checking that premise wants a number, not a list.
             force.value_or(sdk::Engine::ForceVector{}).y,
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
             player.has_value() ? reinterpret_cast<uintptr_t>(player->object) : 0,
             muzzle_ok ? "true" : "false",
             muzzle_clean ? "true" : "false",
             muzzle[0], muzzle[1], muzzle[2],
             muzzle[0], muzzle[1], muzzle[2],
             muzzle_mdl.c_str(), weapon_vs_hand, muzzle_from_hand,
             wa.valid ? "true" : "false", static_cast<unsigned long long>(wa.frames), wa.disagreement,
             wa.step, wa.worst, static_cast<unsigned long long>(wa.still_frames),
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
    std::string body = "{" + world_json + std::string(buf + 1);
    // AND THE GATE'S SIGNALS. The world-dependent checks read THIS document, so without them here the gate
    // reads false in a loaded level and silently skips 57 assertions.
    std::string ws;
    append_world_state(ws);
    while (!ws.empty() && ws.back() == ',') {
        ws.pop_back();
    }
    if (!ws.empty() && body.size() > 1 && body.back() == '}') {
        body.pop_back();
        body += ',';
        body += ws;
        body += '}';
    }
    return body;
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
                 "\"bsp_root\":\"0x%08" PRIXPTR "\",\"root_matches_bsp\":%s,"
                 "\"completed\":%s,\"faulted_list\":%zu,\"lists_walked\":%zu,"
                 "\"object_faults\":%zu,\"first_fault\":\"0x%08" PRIXPTR "\",\"hit_cap\":%s}",
                 wt->objects_seen, wt->linked, wt->unlinked, wt->node_found, wt->root_reached,
                 wt->counts_monotonic, wt->root_mismatches, wt->root, wt->max_depth,
                 wt->bsp_root, wt->root_matches_bsp ? "true" : "false",
                 wt->completed ? "true" : "false", wt->faulted_list, wt->lists_walked,
                 wt->object_faults, wt->first_fault, wt->hit_cap ? "true" : "false");
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
                 "\"gate_open\":%zu,\"records_with_entries\":%zu,\"gated_violations\":%zu,"
                 // THE POPULATION THIS WALK COVERED, under a name no other block uses. gate_open is compared
                 // host-side against the public API's renderable count, and the two are the SAME predicate over
                 // DIFFERENT walks -- so the comparison only means anything when both covered the same objects.
                 // Without this the identity silently degrades into a race whenever the scene churns.
                 "\"sr_objects\":%zu}",
                 sr->objects, sr->backpointer_ok, sr->volume_matched, sr->volume_gated,
                 sr->unexplained, sr->entries, sr->count_matches_walk, sr->entry_record_ok,
                 sr->hit_links_ok, sr->entry_sector_aabb_ok, sr->entry_sector_planes_ok,
                 sr->gate_open, sr->records_with_entries, sr->gated_violations, sr->objects);
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

// A value that is ALREADY valid JSON -- an array or object composed elsewhere -- spliced in verbatim rather
// than escaped as a string. The JsonFields class has raw() for the same reason; this is its counterpart for
// the older append-style builders.
void json_append_raw(std::string& out, const char* key, const char* json) {
    // TRAILING comma, matching json_append_bool/double/string above. A leading-comma version of this shipped
    // briefly and produced a payload that parsed fine for 39 kB and then did not -- the builders around it
    // all emit their own trailing separator, so one function disagreeing corrupts the document at whatever
    // offset it happens to sit.
    out += '"';
    out += key;
    out += "\":";
    out += json;
    out += ',';
}

void json_append_string(std::string& out, const char* key, const char* value) {
    // Trailing-comma convention, matching json_append_bool/double above.
    out += '"';
    out += key;
    out += "\":\"";
    for (const char* p = value; p != nullptr && *p != '\0'; ++p) {
        // Only the two characters that would break the document are escaped; the payload is identifiers,
        // offsets and separators.
        if (*p == '"' || *p == '\\') {
            out += '\\';
        }
        out += *p;
    }
    out += '"';
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

// THE GATE'S SIGNALS, emitted into EVERY document a gated check might read.
//
// Learned the hard way: these first went only into /sdk/shader-params while the world-dependent checks read
// /sdk/targets, so json_bool missed, the condition read false, and 57 checks reported "no world loaded" while
// standing in a level. A gate stuck SHUT converts failure into silence, which is worse than the reds it
// replaced -- and the only reason it was caught immediately is that the skip tally made 57 in-game skips
// obviously absurd. Emit from one function so a second document cannot drift again.
void append_world_state(std::string& out) {
    const auto loaded = sdk::WorldBSP::is_world_loaded();
    const auto lp = sdk::PlayerMgr::player(0);
    const bool have_world = loaded.has_value() && *loaded;
    // The player object POINTER survives leaving a level, so this proves little on its own; world_loaded is the
    // load-bearing half and this is reported for diagnosis.
    const bool have_player = lp.has_value() && lp->object != 0;
    json_append_bool(out, "ws_world_loaded", have_world);
    json_append_bool(out, "ws_player_present", have_player);
    json_append_bool(out, "ws_world_ready", have_world && have_player);
}

std::string build_shader_params_json(bool include_write_probes) {
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
        // BUTTON BANKS, current and previous. Published because "is a button down" is a question a consumer
        // has, and because it is the only way to observe whether a synthetic button reached the device --
        // position was visible here while button state was not, which hid exactly that.
        json_append_bool(out, "input_mouse_btn_l", mouse->buttons[0]);
        json_append_bool(out, "input_mouse_btn_r", mouse->buttons[1]);
        json_append_bool(out, "input_mouse_btn_m", mouse->buttons[2]);
        json_append_bool(out, "input_mouse_prev_btn_l", mouse->prev_buttons[0]);
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

    // ---- ONE RESOURCE RECORD, READ THROUGH THE VERIFIED OFFSETS ---------------------------------
    //
    // The registry's field offsets come from ListResourcesOfType's column accessors, every one a pure field
    // read. The CONTAINER layout is not established -- see Resources.hpp -- so there is no enumeration
    // here, and what is checked instead is that the reader REFUSES what it cannot validate: a null address,
    // and an address that is real memory but not a record.
    json_append_bool(out, "resources_manager_resolved", sdk::Resources::manager_address() != 0);
    json_append_bool(out, "resources_table_offset_ok",
                     sdk::Resources::table_address() == sdk::Resources::manager_address() + 0x2C);
    const auto res_stats = sdk::Resources::stats();
    json_append_bool(out, "resources_stats_readable", res_stats.has_value());
    if (res_stats.has_value()) {
        json_append_double(out, "resources_total", static_cast<double>(res_stats->total), 0);
        json_append_double(out, "resources_named", static_cast<double>(res_stats->named), 0);
        json_append_double(out, "resources_loaded", static_cast<double>(res_stats->loaded), 0);
        json_append_double(out, "resources_prefetched",
                           static_cast<double>(res_stats->auto_prefetched), 0);
        json_append_double(out, "resources_buckets_used",
                           static_cast<double>(res_stats->buckets_used), 0);
        json_append_double(out, "resources_longest_chain",
                           static_cast<double>(res_stats->longest_chain), 0);
        // THE FIGURES ARE ONLY COUNTS IF NO CAP WAS REACHED. This flag is how the wrong table base was
        // caught, so it is reported rather than swallowed.
        json_append_bool(out, "resources_hit_cap", res_stats->hit_cap);
        // THE SHARPEST AVAILABLE CHECK ON THE TRAVERSAL: an address is unique by construction, so fewer
        // distinct addresses than records means the walk visited a node twice -- which is what a wrong
        // table base produces. Deliberately NOT keyed on a record field: the first version of this used a
        // supposed id at +0x1C, and that field holds only 131 distinct values across 3458 records.
        json_append_double(out, "resources_distinct_addresses",
                           static_cast<double>(res_stats->distinct_addresses), 0);
    }
    // A query a mod would run, plus a ROUND TRIP: whatever search() returns must be findable by its exact
    // name, which tests the two query paths against each other rather than against a constant.
    const auto res_worlds = sdk::Resources::search("worlds", 6);
    json_append_double(out, "resources_world_hits", static_cast<double>(res_worlds.size()), 0);
    size_t res_roundtrip = 0;
    for (const auto& r : res_worlds) {
        const auto back = sdk::Resources::find(r.name);
        if (back.has_value() && back->address == r.address) {
            ++res_roundtrip;
        }
    }
    json_append_double(out, "resources_roundtrip", static_cast<double>(res_roundtrip), 0);
    // Settings whose type tag disagrees with their bytes. Two exist in this build, both unread by the
    // executable, so the disagreement is invisible to the engine and visible only to a consumer.
    const auto ev_suspicious = sdk::EngineVars::suspicious_int_entries();
    json_append_double(out, "enginevars_suspicious_count", static_cast<double>(ev_suspicious.size()), 0);
    bool ev_susp_named = !ev_suspicious.empty();
    bool ev_susp_extrapolate = false;
    for (const auto& e : ev_suspicious) {
        if (e.name.empty()) {
            ev_susp_named = false;
        }
        if (e.name.rfind("MaxExtrapolate", 0) == 0) {
            ev_susp_extrapolate = true;
        }
    }
    json_append_bool(out, "enginevars_suspicious_named", ev_susp_named);
    json_append_bool(out, "enginevars_suspicious_is_extrapolate", ev_susp_extrapolate);

    // ---- THE CONSOLE REGISTRY THE GAME ACTUALLY DISPATCHES AGAINST -------------------------------
    //
    // Two independently-derived sets meet here. The engine's static descriptor table publishes its own
    // count (34, written as a literal by the initialiser), and the live circular list is walked until it
    // returns to its head. If the walk is right, EVERY static command must also appear in the live list --
    // a cross-check that needs neither set to be trusted.
    const auto con_stats = sdk::Console::stats();
    const auto con_live = sdk::Console::all();
    const auto con_static = sdk::Console::static_commands();
    // ---- REGISTERED DEFAULTS, AND CMoveMgr'S TWO INSTANCE FIELDS ----
    {
        size_t ndef = 0;
        const auto* defs = sdk::Engine::registered_defaults(ndef);
        json_append_double(out, "vd_recorded", static_cast<double>(ndef), 0);
        size_t literal = 0, dbrec = 0, at_default = 0, answerable = 0;
        bool all_found = true;
        for (size_t i = 0; i < ndef; ++i) {
            if (defs[i].source == sdk::Engine::DefaultSource::CodeLiteral) {
                ++literal;
                const auto at = sdk::Engine::is_at_default(defs[i].name);
                if (at.has_value()) {
                    ++answerable;
                    if (*at) {
                        ++at_default;
                    }
                }
            } else {
                ++dbrec;
                // A DATABASE-SOURCED default must yield no answer -- there is no literal to compare against.
                if (sdk::Engine::is_at_default(defs[i].name).has_value()) {
                    all_found = false;
                }
            }
            // Every recorded name must exist as a discovered cache pair, EXCEPT SpectatorSpeedMul which has no
            // global at all -- the reason PlayerMgr exposes it off the instance.
            const bool is_spectator = std::string_view{defs[i].name} == "SpectatorSpeedMul";
            const bool found = sdk::Engine::find_cached_var(defs[i].name).has_value();
            if (found == is_spectator) {
                all_found = false;
            }
        }
        json_append_double(out, "vd_literal", static_cast<double>(literal), 0);
        json_append_double(out, "vd_database", static_cast<double>(dbrec), 0);
        json_append_double(out, "vd_answerable", static_cast<double>(answerable), 0);
        json_append_double(out, "vd_at_default", static_cast<double>(at_default), 0);
        json_append_bool(out, "vd_globals_as_expected", all_found);
        json_append_double(out, "vd_changed",
                           static_cast<double>(sdk::Engine::vars_changed_from_default().size()), 0);
        json_append_bool(out, "vd_lookup_refused",
                         sdk::Engine::registered_default("NoSuchVar") == nullptr &&
                             sdk::Engine::registered_default("") == nullptr &&
                             !sdk::Engine::is_at_default("NoSuchVar").has_value());
        const auto* grav = sdk::Engine::registered_default("PlayerGravity");
        json_append_bool(out, "vd_gravity_recorded", grav != nullptr && grav->value == -2000.0f);

        // CMoveMgr's own fields.
        const auto was = sdk::PlayerMgr::water_affects_speed(0);
        const auto ssm = sdk::PlayerMgr::spectator_speed_mul(0);
        const auto ssc = sdk::PlayerMgr::spectator_speed_mul_cache(0);
        json_append_bool(out, "mm_water_readable", was.has_value());
        json_append_bool(out, "mm_ssm_cache_populated", ssc.has_value() && ssc->populated());
        json_append_bool(out, "mm_ssm_readable", ssm.has_value());
        if (ssm.has_value()) {
            json_append_double(out, "mm_ssm_value", static_cast<double>(*ssm), 3);
        }
        // THE OWNER HALF must be the ILTClient every other cache pair shares, which ties the instance pair to
        // the 474 discovered ones.
        bool owner_shared = false;
        if (ssc.has_value()) {
            for (const auto& v : sdk::Engine::cached_console_vars(64)) {
                if (v.owner != 0 && v.owner == ssc->owner) {
                    owner_shared = true;
                    break;
                }
            }
        }
        json_append_bool(out, "mm_ssm_owner_shared", owner_shared);
        // THE HOLE Engine CANNOT COVER, closed where the instance is reachable.
        json_append_bool(out, "mm_ssm_default_answerable",
                         sdk::PlayerMgr::spectator_speed_mul_is_default(0).has_value());
        json_append_bool(out, "mm_ssm_is_default",
                         sdk::PlayerMgr::spectator_speed_mul_is_default(0).value_or(false));
        json_append_bool(out, "mm_ssm_engine_cannot",
                         !sdk::Engine::is_at_default("SpectatorSpeedMul").has_value());
        json_append_bool(out, "mm_range_refused", !sdk::PlayerMgr::water_affects_speed(9).has_value() &&
                                                      !sdk::PlayerMgr::spectator_speed_mul(9).has_value());
    }

    // ---- WHO REGISTERED EACH COMMAND, AND WHICH ONES DO NOTHING ----
    {
        size_t nreg = 0;
        const auto* regs = sdk::Console::registrars(nreg);
        json_append_double(out, "creg_registrars", static_cast<double>(nreg), 0);
        size_t total = 0;
        bool counts_agree = true;
        for (size_t i = 0; i < nreg; ++i) {
            const auto listed = sdk::Console::commands_registered_by(regs[i].offset);
            if (listed.size() != regs[i].count) {
                counts_agree = false;
            }
            total += listed.size();
        }
        json_append_double(out, "creg_total", static_cast<double>(total), 0);
        // EVERY registrar's declared count must equal the number of commands attributed to it -- the table is
        // hand-transcribed, so the two halves must be checked against each other.
        json_append_bool(out, "creg_counts_agree", counts_agree);
        // THE IDENTIFICATION THAT MATTERS: CMoveMgr registers exactly the five the reference does.
        const auto move = sdk::Console::commands_registered_by(0x108CD0);
        json_append_double(out, "creg_movemgr", static_cast<double>(move.size()), 0);
        const auto* leash = sdk::Console::registrar_of("PlayerLeash");
        const auto* health = sdk::Console::registrar_of("Health");
        json_append_bool(out, "creg_leash_is_movemgr",
                         leash != nullptr && leash->offset == 0x108CD0 && leash->name != nullptr);
        json_append_bool(out, "creg_health_is_stats",
                         health != nullptr && health->offset == 0x114F10);
        json_append_bool(out, "creg_unknown_refused",
                         sdk::Console::registrar_of("quit") == nullptr &&
                             sdk::Console::registrar_of("") == nullptr &&
                             sdk::Console::commands_registered_by(0).empty());
        {
            const auto unattr = sdk::Console::unattributed_commands();
            const auto unreg = sdk::Console::unregistered_table_commands();
            json_append_double(out, "creg_unattributed", static_cast<double>(unattr.size()), 0);
            json_append_double(out, "creg_unregistered", static_cast<double>(unreg.size()), 0);
            std::string names;
            for (const auto& n : unattr) {
                if (!names.empty()) {
                    names += ",";
                }
                names += n;
            }
            json_append_string(out, "creg_unattributed_names", names.c_str());
            std::string un;
            for (const auto& n : unreg) {
                if (!un.empty()) {
                    un += ",";
                }
                un += n;
            }
            json_append_string(out, "creg_unregistered_names", un.c_str());
            // THE ARITHMETIC MUST CLOSE: live-in-gameclient = (table entries that are live) + unattributed.
            size_t table_live = 0;
            size_t ntab = 0;
            const auto* rr = sdk::Console::registrars(ntab);
            for (size_t i = 0; i < ntab; ++i) {
                for (const auto& nm : sdk::Console::commands_registered_by(rr[i].offset)) {
                    if (sdk::Console::find(nm).has_value()) {
                        ++table_live;
                    }
                }
            }
            size_t live_gc = 0;
            const auto* gcm = sdk::Modules::get().game_client();
            for (const auto& c : sdk::Console::all()) {
                if (!c.from_exe && gcm != nullptr && c.handler >= gcm->base &&
                    c.handler < gcm->base + gcm->size) {
                    ++live_gc;
                }
            }
            json_append_double(out, "creg_table_live", static_cast<double>(table_live), 0);
            json_append_double(out, "creg_live_gc", static_cast<double>(live_gc), 0);
            json_append_bool(out, "creg_accounting_closes", live_gc == table_live + unattr.size());
        }

        // NO-OPS: five commands resolve to the ICF-folded empty stub.
        const auto noops = sdk::Console::noop_commands();
        json_append_double(out, "creg_noops", static_cast<double>(noops.size()), 0);
        json_append_bool(out, "creg_stub_found", sdk::Console::empty_stub() != 0);
        json_append_bool(out, "creg_rebindfx_noop", sdk::Console::is_noop("RebindFX").value_or(false));
        json_append_bool(out, "creg_aidebug_noop", sdk::Console::is_noop("AIDebug").value_or(false));
        // AND A REAL COMMAND MUST NOT BE ONE, or the test would pass on everything.
        json_append_bool(out, "creg_health_not_noop", sdk::Console::is_noop("Health") == false);
        json_append_bool(out, "creg_noop_absent_refused", !sdk::Console::is_noop("NoSuchCommand").has_value());
    }

    json_append_bool(out, "console_stats_readable", con_stats.has_value());
    json_append_double(out, "console_static_count", static_cast<double>(con_static.size()), 0);
    json_append_double(out, "console_live_total", static_cast<double>(con_live.size()), 0);
    if (con_stats.has_value()) {
        json_append_double(out, "console_from_exe", static_cast<double>(con_stats->from_exe), 0);
        json_append_double(out, "console_from_modules", static_cast<double>(con_stats->from_modules), 0);
        json_append_double(out, "console_distinct_names", static_cast<double>(con_stats->distinct_names), 0);
        json_append_double(out, "console_inconsistent_nodes",
                           static_cast<double>(con_stats->inconsistent_nodes), 0);
        json_append_double(out, "console_unreadable_names",
                           static_cast<double>(con_stats->unreadable_names), 0);
        json_append_bool(out, "console_hit_cap", con_stats->hit_cap);
    }

    // Every static command present in the live list: the cross-check described above.
    size_t con_static_in_live = 0;
    for (const auto& sc : con_static) {
        for (const auto& lc : con_live) {
            if (lc.name == sc.name) {
                ++con_static_in_live;
                break;
            }
        }
    }
    json_append_double(out, "console_static_in_live", static_cast<double>(con_static_in_live), 0);

    // PROVENANCE: flags 0 is a built-in descriptor, flags 1 arrived through RegisterConsoleProgram. The
    // built-in count must equal the static table's published count exactly -- two different routes to the
    // same 34 -- and the runtime group must be non-empty or nothing registered at all.
    if (con_stats.has_value()) {
        json_append_double(out, "console_builtin", static_cast<double>(con_stats->builtin), 0);
        json_append_double(out, "console_runtime", static_cast<double>(con_stats->runtime), 0);
    }

    // The engine's own console API, resolved through ILTClient rather than called. Each must land inside the
    // exe: CLTClient implements them, so a slot pointing elsewhere means the vtable is not the mapped one.
    json_append_bool(out, "console_api_find_var", sdk::Console::find_variable_fn().has_value());
    json_append_bool(out, "console_api_set_var", sdk::Console::set_variable_float_fn().has_value());
    json_append_bool(out, "console_api_register", sdk::Console::register_program_fn().has_value());
    // The three slots must be DISTINCT functions -- a vtable read that produced one address three times
    // would satisfy every check above and still be wrong.
    const auto con_s1 = sdk::Console::client_vtable_slot(sdk::Console::kSlotFindVariable);
    const auto con_s2 = sdk::Console::client_vtable_slot(sdk::Console::kSlotSetVariableFloat);
    const auto con_s3 = sdk::Console::client_vtable_slot(sdk::Console::kSlotRegisterProgram);
    json_append_bool(out, "console_api_slots_distinct",
                     con_s1 != 0 && con_s2 != 0 && con_s3 != 0 && con_s1 != con_s2 && con_s2 != con_s3 &&
                         con_s1 != con_s3);

    // ---- THE GAME'S OWN PLAYER, AND THE OBJECT IDENTITY THAT TIES IT TO THE ENGINE'S -------------
    //
    // gameclient's player manager reaches the same engine model object CClientShell::local_player names, by
    // a completely unrelated route. That agreement is the check worth making: two independent paths to one
    // pointer cannot both be a lucky offset.
    const auto pm_player = sdk::PlayerMgr::local_player();
    json_append_bool(out, "pmgr_manager_resolved", sdk::PlayerMgr::manager() != 0);
    json_append_double(out, "pmgr_occupied_slots",
                       static_cast<double>(sdk::PlayerMgr::occupied_slot_count()), 0);
    json_append_double(out, "pmgr_first_slot",
                       static_cast<double>(sdk::PlayerMgr::first_occupied_slot().value_or(99)), 0);
    json_append_bool(out, "pmgr_player_read", pm_player.has_value());
    if (pm_player.has_value()) {
        json_append_bool(out, "pmgr_rotation_unit", pm_player->pose.rotation_is_unit());
        json_append_bool(out, "pmgr_has_camera_object", pm_player->camera_object != 0);
        json_append_bool(out, "pmgr_has_model", pm_player->model_object != 0);
        // ADDRESSES, as hex. Diagnostics only, but the ones a reverser actually needs: every
        // "who writes this" question starts by pasting one of these into /watch/arm.
        {
            char ab[96];
            snprintf(ab, sizeof(ab), "\"0x%08" PRIXPTR "\"", pm_player->object);
            json_append_raw(out, "pmgr_object_addr", ab);
            snprintf(ab, sizeof(ab), "\"0x%08" PRIXPTR "\"", pm_player->holder);
            json_append_raw(out, "pmgr_holder_addr", ab);
            snprintf(ab, sizeof(ab), "\"0x%08" PRIXPTR "\"", pm_player->camera_object);
            json_append_raw(out, "pmgr_camera_addr", ab);
            snprintf(ab, sizeof(ab), "\"0x%08" PRIXPTR "\"", pm_player->model_object);
            json_append_raw(out, "pmgr_model_addr", ab);
        }
        // TWO CO-LOCATED PLAYER MODELS, not one. The holder's model and the shell's share asset, dims and
        // position -- which is what an earlier pass mistook for identity -- and differ by the engine's own
        // server/client discriminator. Both halves are asserted: same description, different objects.
        bool same_pointer = true;
        bool same_position = false;
        if (const auto shell_player = sdk::CClientShell::local_player(0)) {
            const auto shell_obj = reinterpret_cast<uintptr_t>(shell_player->object);
            same_pointer = shell_obj == pm_player->model_object;
            std::array<float, 3> a{};
            std::array<float, 3> b{};
            if (sdk::mem::copy(a.data(), shell_obj + 0x14, sizeof(a)) &&
                sdk::mem::copy(b.data(), pm_player->model_object + 0x14, sizeof(b))) {
                same_position = a == b;
            }
            json_append_bool(out, "pmgr_shell_model_server_backed",
                             sdk::PlayerMgr::is_server_backed(shell_obj).value_or(false));
        }
        json_append_bool(out, "pmgr_models_are_distinct", !same_pointer);
        json_append_bool(out, "pmgr_models_co_located", same_position);
        json_append_bool(out, "pmgr_holder_model_client_only",
                         sdk::PlayerMgr::is_server_backed(pm_player->model_object).value_or(true) == false);
        json_append_bool(out, "pmgr_camera_client_only",
                         sdk::PlayerMgr::is_server_backed(pm_player->camera_object).value_or(true) == false);
        // The anchor is transform-only: no flags, no dims. That is what makes it a view anchor rather than
        // a thing in the world, and it is asserted rather than described.
        const auto anchor_flags = sdk::mem::read_u32(pm_player->camera_object + 0x3C);
        std::array<float, 3> anchor_dims{};
        const bool dims_ok =
            sdk::mem::copy(anchor_dims.data(), pm_player->camera_object + 0x64, sizeof(anchor_dims));
        json_append_bool(out, "pmgr_camera_no_flags", anchor_flags.value_or(1u) == 0u);
        json_append_bool(out, "pmgr_camera_no_dims",
                         dims_ok && anchor_dims[0] == 0.0f && anchor_dims[1] == 0.0f &&
                             anchor_dims[2] == 0.0f);
        // THE CAMERA'S COMPOSITION, told apart by LINK STATE alone. Its constructor self-links eleven
        // embedded links; by runtime the eight at +16..+156 have been inserted into other subsystems' lists
        // (Linked) while the three at +420/+468/+516 are owned heads with nothing in them (Empty). Reading
        // the vtable at +16 as a base-class vtable -- which an earlier pass did -- gets the class wrong.
        size_t cam_nodes = 0, cam_heads = 0, cam_unreadable = 0;
        for (const uintptr_t off : {16u, 36u, 56u, 76u, 96u, 116u, 136u, 156u}) {
            switch (sdk::mem::classify_link(pm_player->holder + off + 4)) {
            case sdk::mem::LinkState::Linked: ++cam_nodes; break;
            case sdk::mem::LinkState::Empty: ++cam_heads; break;
            default: ++cam_unreadable; break;
            }
        }
        size_t cam_owned_empty = 0;
        for (const uintptr_t off : {420u, 468u, 516u}) {
            if (sdk::mem::link_is_empty(pm_player->holder + off + 8)) {
                ++cam_owned_empty;
            }
        }
        json_append_double(out, "cam_sink_nodes", static_cast<double>(cam_nodes), 0);
        json_append_double(out, "cam_sink_heads", static_cast<double>(cam_heads), 0);
        json_append_double(out, "cam_sink_unreadable", static_cast<double>(cam_unreadable), 0);
        json_append_double(out, "cam_owned_lists_empty", static_cast<double>(cam_owned_empty), 0);
        // Eight distinct vtables on the nodes against one shared by the owned heads -- the other half of
        // the composition claim, and it does not depend on link state at all.
        std::vector<uint32_t> node_vts;
        for (const uintptr_t off : {16u, 36u, 56u, 76u, 96u, 116u, 136u, 156u}) {
            node_vts.push_back(sdk::mem::read_u32(pm_player->holder + off).value_or(0));
        }
        std::sort(node_vts.begin(), node_vts.end());
        const size_t distinct_node_vts =
            static_cast<size_t>(std::unique(node_vts.begin(), node_vts.end()) - node_vts.begin());
        json_append_double(out, "cam_sink_distinct_vtables", static_cast<double>(distinct_node_vts), 0);
        const auto h1 = sdk::mem::read_u32(pm_player->holder + 420);
        const auto h2 = sdk::mem::read_u32(pm_player->holder + 468);
        const auto h3 = sdk::mem::read_u32(pm_player->holder + 516);
        json_append_bool(out, "cam_owned_share_vtable",
                         h1.has_value() && h1 == h2 && h2 == h3 && *h1 != 0);
        // THE DELEGATES, read through the accessor rather than by offset arithmetic here. Every one must
        // name the camera as its owner -- an internal-consistency check across eight independent nodes, which
        // is what establishes the owner field rather than any single read.
        const auto cam_dels = sdk::PlayerMgr::camera_delegates(0);
        size_t del_owned = 0, del_registered = 0, del_subject = 0;
        for (const auto& d : cam_dels) {
            if (d.owner == pm_player->holder) {
                ++del_owned;
            }
            if (d.registered) {
                ++del_registered;
            }
            if (d.subject != 0) {
                ++del_subject;
            }
        }
        json_append_double(out, "cam_delegates", static_cast<double>(cam_dels.size()), 0);
        json_append_double(out, "cam_delegates_owned", static_cast<double>(del_owned), 0);
        json_append_double(out, "cam_delegates_registered", static_cast<double>(del_registered), 0);
        json_append_double(out, "cam_delegates_subject", static_cast<double>(del_subject), 0);
        json_append_bool(out, "cam_delegates_consistent",
                         sdk::PlayerMgr::camera_delegates_consistent(0).value_or(false));
        // Six of the eight subjects point into the player object's own region, which is what says the camera
        // listens to the player rather than to the world at large. Reported as a count so a build that
        // rewires it is visible rather than silently different.
        size_t del_subject_in_player = 0;
        for (const auto& d : cam_dels) {
            if (d.subject >= pm_player->object && d.subject < pm_player->object + 0x400) {
                ++del_subject_in_player;
            }
        }
        json_append_double(out, "cam_delegates_on_player",
                           static_cast<double>(del_subject_in_player), 0);

        // WHO IS LISTENING TO THE PLAYER. Take the subject one of the camera's own delegates records, treat
        // it as a list head, and walk it: the camera must be among the listeners it finds. That closes the
        // loop -- the camera says which list it is in, and the list says the camera is in it.
        size_t lst_total = 0, lst_valid = 0;
        bool lst_camera_found = false;
        if (!cam_dels.empty()) {
            const auto subject = cam_dels.front().subject;
            const auto found = sdk::Delegates::listeners(subject);
            lst_total = found.size();
            for (const auto& l : found) {
                if (l.vtable_valid) {
                    ++lst_valid;
                }
                if (l.owner == pm_player->holder) {
                    lst_camera_found = true;
                }
            }
            json_append_bool(out, "dlg_is_listening",
                             sdk::Delegates::is_listening(subject, pm_player->holder));
        }
        json_append_double(out, "dlg_listeners", static_cast<double>(lst_total), 0);
        json_append_double(out, "dlg_listeners_valid", static_cast<double>(lst_valid), 0);
        json_append_bool(out, "dlg_camera_in_list", lst_camera_found);
        json_append_bool(out, "dlg_detach_resolved", sdk::Delegates::detach_fn() != 0);
        // The validator must REJECT something that is not a delegate vtable -- the camera's own primary
        // vtable is a real vtable and is not one of the 329, so it is the honest negative control.
        const auto cam_primary = sdk::mem::read_u32(pm_player->holder).value_or(0);
        json_append_bool(out, "dlg_validator_rejects",
                         cam_primary != 0 && !sdk::Delegates::is_delegate_vtable(cam_primary));
        json_append_bool(out, "dlg_null_refused",
                         sdk::Delegates::listeners(0).empty() &&
                             !sdk::Delegates::read_node(0).has_value() &&
                             !sdk::Delegates::is_delegate_vtable(0) &&
                             !sdk::Delegates::is_listening(0, pm_player->holder));

        // THE PLAYER'S EVENT CHANNELS, discovered by scan rather than by the offsets this session measured
        // by hand -- so a build that moves them is found rather than silently missed.
        const auto chans = sdk::Delegates::find_channels(pm_player->object, 0x100);
        size_t chan_listeners = 0;
        for (const auto& ch : chans) {
            chan_listeners += ch.listeners;
        }
        json_append_double(out, "dlg_channels", static_cast<double>(chans.size()), 0);
        json_append_double(out, "dlg_channel_listeners", static_cast<double>(chan_listeners), 0);
        // The camera must appear on several of them, and the set must be exactly what its own nodes claim:
        // eight delegates, of which the ones whose subject lies in the player are these channels.
        const auto cam_chans =
            sdk::Delegates::channels_listened_to(pm_player->object, pm_player->holder, 0x100);
        json_append_double(out, "dlg_camera_channels", static_cast<double>(cam_chans.size()), 0);
        size_t cam_subjects_in_player = 0;
        for (const auto& d : cam_dels) {
            if (d.subject >= pm_player->object && d.subject < pm_player->object + 0x100) {
                ++cam_subjects_in_player;
            }
        }
        json_append_bool(out, "dlg_camera_channels_match",
                         !cam_chans.empty() && cam_chans.size() == cam_subjects_in_player);
        // A scan of something that is NOT an object of this shape must find nothing -- the exe's image base
        // is real memory with no delegate lists in it.
        const auto* exe_mod = sdk::Modules::get().exe();
        json_append_bool(out, "dlg_scan_negative",
                         exe_mod != nullptr && exe_mod->base != 0 &&
                             sdk::Delegates::find_channels(exe_mod->base, 0x100).empty());
        json_append_bool(out, "dlg_list_predicate_rejects",
                         !sdk::Delegates::is_delegate_list(0) &&
                             !sdk::Delegates::is_delegate_list(pm_player->holder + 420 + 8));

        // A null link is refused rather than reported as an empty list.
        json_append_bool(out, "cam_link_null_refused",
                         sdk::mem::classify_link(0) == sdk::mem::LinkState::Unreadable &&
                             !sdk::mem::link_is_empty(0));

        // TWO POSE GENERATIONS: the applied pair matches the engine object bit-for-bit, and the other pair's
        // POSITION does not. Both halves reported, because "they match" alone would also be true if this SDK
        // were reading one pair twice.
        json_append_bool(out, "pmgr_applied_matches_object",
                         sdk::PlayerMgr::applied_pose_matches_camera_object(0).value_or(false));
        bool pm_gens_differ = false;
        if (const auto other = sdk::PlayerMgr::read_pose(pm_player->holder, false)) {
            for (size_t i = 0; i < 3; ++i) {
                uint32_t a = 0;
                uint32_t b = 0;
                std::memcpy(&a, &other->position[i], sizeof(a));
                std::memcpy(&b, &pm_player->applied_pose.position[i], sizeof(b));
                if (a != b) {
                    pm_gens_differ = true;
                }
            }
        }
        json_append_bool(out, "pmgr_pose_generations_differ", pm_gens_differ);
        json_append_bool(out, "pmgr_applied_rot_unit", pm_player->applied_pose.rotation_is_unit());
        json_append_bool(out, "pmgr_camera_rot_matches",
                         sdk::PlayerMgr::camera_rotation_matches_pose(0).value_or(false));
        // THE SAME TWO COMPARISONS AS VERDICTS, which is the only form that survives a running engine. The bool
        // above reads one side then the other, so a frame landing in the gap makes them disagree for reasons
        // that have nothing to do with the mapping -- four checks failed exactly that way on the first live run.
        {
            const auto verdict_name = [](sdk::PlayerMgr::PoseAgreement a) {
                switch (a) {
                case sdk::PlayerMgr::PoseAgreement::Equal: return "equal";
                case sdk::PlayerMgr::PoseAgreement::Differ: return "differ";
                case sdk::PlayerMgr::PoseAgreement::Torn: return "torn";
                default: return "unreadable";
                }
            };
            const auto rot_v = sdk::PlayerMgr::camera_rotation_agreement(0);
            const auto app_v = sdk::PlayerMgr::applied_pose_agreement(0);
            json_append_string(out, "pmgr_rot_agreement", verdict_name(rot_v));
            json_append_string(out, "pmgr_applied_agreement", verdict_name(app_v));
            // A consumer's actual question: is this reading usable right now? Torn is not a failure, it is
            // "ask again"; Differ is the one that means the mapping is wrong.
            json_append_bool(out, "pmgr_rot_never_differs",
                             rot_v != sdk::PlayerMgr::PoseAgreement::Differ);
            json_append_bool(out, "pmgr_applied_never_differs",
                             app_v != sdk::PlayerMgr::PoseAgreement::Differ);
            // THE CENSUS, so "never Differs" cannot pass vacuously on a permanent Torn. 16 samples is enough to
            // see both outcomes on a running engine without stalling the IPC thread.
            // BRACKETED BY THE VIEW-WRITE COUNTER. The pose and the camera object only agree in a SETTLED
            // state: while ApplyLookDelta is running the pose leads and the object follows within the frame, so
            // sampling during look input finds them stably different -- neither a race nor a wrong offset.
            //
            // The hook's own call count is the exact signal for "the view is being written right now", which is
            // far better than the clamp timer that was standing in for it.
            const uint64_t vw_before = ViewHook::get().observed().calls;
            const auto cen = sdk::PlayerMgr::agreement_census(0, 0, 16);
            const uint64_t vw_after = ViewHook::get().observed().calls;
            json_append_double(out, "pmgr_view_writes_during",
                               static_cast<double>(vw_after - vw_before), 0);
            json_append_double(out, "pmgr_agree_equal", static_cast<double>(cen.equal), 0);
            json_append_double(out, "pmgr_agree_differ", static_cast<double>(cen.differ), 0);
            json_append_double(out, "pmgr_agree_torn", static_cast<double>(cen.torn), 0);
            json_append_double(out, "pmgr_agree_unreadable", static_cast<double>(cen.unreadable), 0);
            json_append_double(out, "pmgr_agree_samples",
                               static_cast<double>(cen.equal + cen.differ + cen.torn + cen.unreadable), 0);
        }
        if (const auto eye = sdk::PlayerMgr::eye_offset(0)) {
            json_append_double(out, "pmgr_eye_offset_y", (*eye)[1], 3);
            json_append_double(out, "pmgr_eye_offset_len",
                               std::sqrt((*eye)[0] * (*eye)[0] + (*eye)[1] * (*eye)[1] +
                                         (*eye)[2] * (*eye)[2]),
                               3);
        }
    }
    // An out-of-range slot and an empty one are both refused.
    json_append_bool(out, "pmgr_bounds_refused",
                     !sdk::PlayerMgr::slot(4).has_value() && !sdk::PlayerMgr::player(4).has_value() &&
                         !sdk::PlayerMgr::read_pose(0).has_value());

    // ---- THE NAMED EVENT BUS, VERIFIED AGAINST THE BINARY ----------------------------------------
    //
    // Each catalogued dispatcher must still reference its own event-name string. That turns the table from a
    // transcription into something the suite maintains: a moved function or a renamed event fails here rather
    // than handing a consumer a stale hook address.
    const auto& evs = sdk::Events::all();
    json_append_double(out, "ev_total", static_cast<double>(evs.size()), 0);
    json_append_double(out, "ev_verified", static_cast<double>(sdk::Events::verified_count()), 0);
    size_t ev_resolved = 0, ev_wellformed = 0;
    for (const auto& e : evs) {
        if (sdk::Events::dispatcher(e.name) != 0) {
            ++ev_resolved;
        }
        if (sdk::Events::payload_is_well_formed(e.payload)) {
            ++ev_wellformed;
        }
    }
    json_append_double(out, "ev_resolved", static_cast<double>(ev_resolved), 0);
    json_append_double(out, "ev_wellformed", static_cast<double>(ev_wellformed), 0);
    // Payload arithmetic, on a known multi-argument event: "sdd" is three slots, twelve bytes.
    const auto ammo = sdk::Events::find("AmmoCountChanged");
    json_append_double(out, "ev_ammo_args",
                       ammo.has_value()
                           ? static_cast<double>(sdk::Events::payload_arg_count(ammo->payload))
                           : -1.0,
                       0);
    json_append_double(out, "ev_ammo_bytes",
                       ammo.has_value()
                           ? static_cast<double>(
                                 sdk::Events::payload_stack_bytes(ammo->payload).value_or(0))
                           : -1.0,
                       0);
    // THE BINDING TABLES, walked live. The rule under test is a naming/role correspondence across the WHOLE
    // population: every Game_* entry must be kind 7 (game->Flash), every _global.* entry a global setter, and
    // every OnConstruct/OnDestruct a lifecycle entry. A kind byte that meant something else would break it.
    size_t bt_panels = 0, bt_entries = 0, bt_game = 0, bt_game_ok = 0;
    size_t bt_global = 0, bt_global_ok = 0, bt_life = 0, bt_life_ok = 0, bt_unknown = 0;
    for (const auto& p : sdk::Events::ui_panels()) {
        if (!sdk::Events::panel_table_initialised(p.name)) {
            continue;
        }
        ++bt_panels;
        for (const auto& b : sdk::Events::panel_bindings(p.name)) {
            ++bt_entries;
            const bool is_game = b.name.rfind("Game_", 0) == 0;
            const bool is_global = b.name.rfind("_global.", 0) == 0;
            const bool is_life = b.name.find(".OnConstruct") != std::string::npos ||
                                 b.name.find(".OnDestruct") != std::string::npos;
            if (is_game) {
                ++bt_game;
                if (b.role == sdk::Events::BindingRole::GameToFlash) {
                    ++bt_game_ok;
                }
            } else if (is_global) {
                ++bt_global;
                if (b.role == sdk::Events::BindingRole::GlobalSetter) {
                    ++bt_global_ok;
                }
            } else if (is_life) {
                ++bt_life;
                if (b.role == sdk::Events::BindingRole::Lifecycle) {
                    ++bt_life_ok;
                }
            }
            if (b.role == sdk::Events::BindingRole::Unknown) {
                ++bt_unknown;
            }
        }
    }
    json_append_double(out, "bt_panels", static_cast<double>(bt_panels), 0);
    json_append_double(out, "bt_entries", static_cast<double>(bt_entries), 0);
    json_append_double(out, "bt_game", static_cast<double>(bt_game), 0);
    json_append_double(out, "bt_game_ok", static_cast<double>(bt_game_ok), 0);
    json_append_double(out, "bt_global", static_cast<double>(bt_global), 0);
    json_append_double(out, "bt_global_ok", static_cast<double>(bt_global_ok), 0);
    json_append_double(out, "bt_life", static_cast<double>(bt_life), 0);
    json_append_double(out, "bt_life_ok", static_cast<double>(bt_life_ok), 0);
    json_append_double(out, "bt_unknown_roles", static_cast<double>(bt_unknown), 0);
    // Role coverage as a partition: every entry must land in exactly one role, so these must sum to the total.
    size_t bt_role_life = 0, bt_role_f2g = 0, bt_role_g2f = 0, bt_role_glob = 0;
    for (const auto& p : sdk::Events::ui_panels()) {
        for (const auto& b : sdk::Events::panel_bindings(p.name)) {
            switch (b.role) {
            case sdk::Events::BindingRole::Lifecycle: ++bt_role_life; break;
            case sdk::Events::BindingRole::FlashToGame: ++bt_role_f2g; break;
            case sdk::Events::BindingRole::GameToFlash: ++bt_role_g2f; break;
            case sdk::Events::BindingRole::GlobalSetter: ++bt_role_glob; break;
            default: break;
            }
        }
    }
    json_append_double(out, "bt_role_lifecycle", static_cast<double>(bt_role_life), 0);
    json_append_double(out, "bt_role_flash_to_game", static_cast<double>(bt_role_f2g), 0);
    json_append_double(out, "bt_role_game_to_flash", static_cast<double>(bt_role_g2f), 0);
    json_append_double(out, "bt_role_global", static_cast<double>(bt_role_glob), 0);

    // THE BRIDGE FROM THE GAME'S PLAYER TO THE ENGINE'S OBJECT, and the physics state the game starves.
    //
    // Two routes that share nothing must name the same LTObject: the game's own two loads out of its player
    // class, and CClientShell's per-frame resolved array. Then the claim that gameclient ZEROES that object's
    // velocity and acceleration every frame is checked against the live fields.
    const auto pe_obj = sdk::PlayerMgr::engine_object(0);
    const auto pe_match = sdk::PlayerMgr::engine_object_is_shell_object(0);
    const auto pe_ctrl = sdk::PlayerMgr::movement_controller(0);
    const auto pe_back = sdk::PlayerMgr::movement_controller_owner_agrees(0);
    const auto pe_reg = sdk::PlayerMgr::engine_object_is_registered(0);
    json_append_bool(out, "pe_resolved", pe_obj.has_value() && *pe_obj != 0);
    json_append_bool(out, "pe_is_shell_object", pe_match.has_value() && *pe_match);
    json_append_bool(out, "pe_match_determinable", pe_match.has_value());
    // THE CLASS-IDENTITY INVARIANT that licenses the offsets: the controller must point back at its owner.
    json_append_bool(out, "pe_controller_resolved", pe_ctrl.has_value() && *pe_ctrl != 0);
    json_append_bool(out, "pe_controller_owner_agrees", pe_back.has_value() && *pe_back);
    json_append_bool(out, "pe_registered_determinable", pe_reg.has_value());
    json_append_bool(out, "pe_is_registered", pe_reg.has_value() && *pe_reg);
    if (pe_obj.has_value()) {
        json_append_bool(out, "pe_zeroed_predicate", sdk::Physics::velocity_zeroed_by_game(*pe_obj));
        // The engine's own getters, through the real vtable slots -- not a raw field read, so a wrong slot
        // would show up rather than agreeing by construction.
        const auto vel = sdk::Physics::velocity(*pe_obj);
        const auto acc = sdk::Physics::acceleration(*pe_obj);
        json_append_bool(out, "pe_velocity_readable", vel.has_value());
        json_append_bool(out, "pe_acceleration_readable", acc.has_value());
        const auto is_zero = [](const std::array<float, 3>& v) {
            return v[0] == 0.0f && v[1] == 0.0f && v[2] == 0.0f;
        };
        json_append_bool(out, "pe_velocity_zero", vel.has_value() && is_zero(*vel));
        json_append_bool(out, "pe_acceleration_zero", acc.has_value() && is_zero(*acc));
        // Reading the raw fields the setters write must agree with the interface getters -- two paths to the
        // same six floats, which is what establishes the +144/+156 offsets rather than assuming them.
        std::array<float, 3> raw_v{}, raw_a{};
        bool raw_ok = true;
        for (size_t i = 0; i < 3; ++i) {
            const auto rv = sdk::mem::read<float>(*pe_obj + 144 + i * 4);
            const auto ra = sdk::mem::read<float>(*pe_obj + 156 + i * 4);
            if (!rv.has_value() || !ra.has_value()) { raw_ok = false; break; }
            raw_v[i] = *rv;
            raw_a[i] = *ra;
        }
        json_append_bool(out, "pe_raw_matches_getters",
                         raw_ok && vel.has_value() && acc.has_value() && raw_v == *vel && raw_a == *acc);
    }
    // ---- THE CAMERA CLAMP THE ENGINE WILL APPLY ----
    {
        auto* clamp_rec = sdk::PlayerMgr::camera_clamp_record(0);
        const auto state = sdk::PlayerMgr::camera_clamp_state(0);
        const auto chase = sdk::PlayerMgr::camera_state_is_chase(0);
        json_append_bool(out, "cc_record_present", clamp_rec != nullptr);
        json_append_bool(out, "cc_state_readable", state.has_value());
        if (state.has_value()) {
            json_append_double(out, "cc_state", static_cast<double>(*state), 0);
        }
        json_append_bool(out, "cc_chase_determinable", chase.has_value());
        if (clamp_rec != nullptr) {
            json_append_string(out, "cc_record_name", sdk::DatabaseMgr::record_name(clamp_rec).c_str());
            const char* kStates[] = {"StandIdle", "StandMoving", "CrouchIdle",
                                     "CrouchMoving", "Chase", "SlideKick"};
            size_t found = 0, ordered = 0;
            std::string vals;
            for (const char* st : kStates) {
                const auto c = sdk::PlayerMgr::camera_clamp(0, st);
                if (!c.has_value()) {
                    continue;
                }
                ++found;
                if (c->first <= c->second) {
                    ++ordered;
                }
                char buf[48]{};
                snprintf(buf, sizeof(buf), "%s=%g/%g", st, static_cast<double>(c->first),
                         static_cast<double>(c->second));
                if (!vals.empty()) {
                    vals += ";";
                }
                vals += buf;
            }
            json_append_double(out, "cc_states_found", static_cast<double>(found), 0);
            json_append_double(out, "cc_states_ordered", static_cast<double>(ordered), 0);
            json_append_string(out, "cc_values", vals.c_str());
            // THE FALLBACK IS WIDER THAN THE REAL CLAMPS -- worth asserting, since it inverts the usual
            // expectation that a missing record is more restrictive.
            const auto ch = sdk::PlayerMgr::camera_clamp(0, "Chase");
            json_append_bool(out, "cc_fallback_wider",
                             ch.has_value() && ch->second < sdk::PlayerMgr::kCameraClampFallback);
        }
        // ---- THE PAIR IS ONE SIGNED AXIS, WHICH THE NEGATION PROVES ----
        {
            const auto deg = sdk::PlayerMgr::camera_clamp(0, "Chase");
            const auto rad = sdk::PlayerMgr::camera_clamp_radians(0, "Chase");
            json_append_bool(out, "cc_rad_available", rad.has_value());
            if (deg.has_value() && rad.has_value()) {
                json_append_double(out, "cc_rad_first", rad->first, 4);
                json_append_double(out, "cc_rad_second", rad->second, 4);
                // BOTH stored components are positive, and the applied pair straddles zero -- the signature of
                // a signed range on one axis rather than two axis limits.
                json_append_bool(out, "cc_stored_both_positive", deg->first > 0.0f && deg->second > 0.0f);
                json_append_bool(out, "cc_applied_straddles_zero", rad->first > 0.0f && rad->second < 0.0f);
                // And the magnitudes must survive the conversion.
                const float k = 0.01745329238474369f;
                json_append_bool(out, "cc_conversion_exact",
                                 std::fabs(rad->first - deg->first * k) < 1e-6f &&
                                     std::fabs(rad->second + deg->second * k) < 1e-6f);
                json_append_bool(out, "cc_asymmetric", deg->first != deg->second);
            }
        }

        // ---- WHAT THE DISPATCHER WOULD PICK RIGHT NOW ----
        {
            const auto flags = sdk::PlayerMgr::move_mgr_flags(0);
            const auto crouch = sdk::PlayerMgr::is_crouching(0);
            const auto moving = sdk::PlayerMgr::is_moving(0);
            const auto vel = sdk::PlayerMgr::physics_velocity(0);
            const auto pick = sdk::PlayerMgr::predicted_clamp_state(0);
            json_append_bool(out, "mv_flags_readable", flags.has_value());
            if (flags.has_value()) {
                json_append_double(out, "mv_flags", static_cast<double>(*flags), 0);
            }
            // THE DECODE ON THE ASSERTION SURFACE. /api/state carries the same thing for the browser; both
            // call PlayerMgr::movement_flags, so there is one bit map and two reporters.
            if (const auto mfd = sdk::PlayerMgr::movement_flags(0)) {
                json_append_string(out, "mv_decoded",
                                   sdk::PlayerMgr::movement_flag_names(mfd->raw).c_str());
                json_append_double(out, "mv_unmapped", static_cast<double>(mfd->unmapped()), 0);
                json_append_bool(out, "mv_dir_contradicts", mfd->direction_contradicts());
                json_append_bool(out, "mv_f_sprinting", mfd->sprinting());
                json_append_bool(out, "mv_f_melee", mfd->melee());
                json_append_bool(out, "mv_f_grenade", mfd->grenade_held());
                json_append_bool(out, "mv_f_normal_speed", mfd->normal_speed());
                json_append_bool(out, "mv_f_moving", mfd->counts_as_moving());
                json_append_bool(out, "mv_f_crouching", mfd->crouching());
                json_append_bool(out, "mv_f_forward", mfd->forward());
                json_append_bool(out, "mv_f_backward", mfd->backward());
                json_append_bool(out, "mv_f_left", mfd->left());
                json_append_bool(out, "mv_f_right", mfd->right());
            }
            json_append_bool(out, "mv_crouch_determinable", crouch.has_value());
            json_append_bool(out, "mv_crouching", crouch.value_or(false));
            json_append_bool(out, "mv_velocity_readable", vel.has_value());
            if (vel.has_value()) {
                const float sp = std::sqrt((*vel)[0] * (*vel)[0] + (*vel)[1] * (*vel)[1] +
                                           (*vel)[2] * (*vel)[2]);
                json_append_double(out, "mv_speed", static_cast<double>(sp), 3);
            }
            json_append_bool(out, "mv_moving_determinable", moving.has_value());
            json_append_bool(out, "mv_moving", moving.value_or(false));
            json_append_bool(out, "mv_pick_available", pick.has_value());
            if (pick.has_value()) {
                json_append_string(out, "mv_pick", pick->state.c_str());
                // The picked state must be one the record actually carries, or the prediction is meaningless.
                json_append_bool(out, "mv_pick_exists",
                                 sdk::PlayerMgr::camera_clamp(0, pick->state).has_value());
                json_append_bool(out, "mv_slide_kick_unchecked", pick->slide_kick_unchecked);
            }
            // IS THE STRONG FORM OF THE CACHE-COHERENCE CHECKS EVEN EXERCISABLE RIGHT NOW?
            //
            // Several suite checks are conditionals of the form "moving || equality": the cached position, the
            // camera pose and the physics velocity only agree with their live counterparts while the player is at
            // rest. If the player never comes to rest, those checks pass on the permissive branch forever and
            // verify nothing -- the same trap as the frozen-render-path probes, which is why this is reported
            // rather than left implicit.
            //
            // NOTE the game freezes simulation when unfocused, so a player who was moving when the window lost
            // focus reports a CONSTANT non-zero speed indefinitely. That is exactly the state in which these
            // checks are least meaningful and most likely to be trusted.
            json_append_bool(out, "mv_strong_form_exercisable", !moving.value_or(true));
            json_append_bool(out, "mv_range_refused",
                             !sdk::PlayerMgr::move_mgr_flags(9).has_value() &&
                                 !sdk::PlayerMgr::is_moving(9).has_value() &&
                                 !sdk::PlayerMgr::predicted_clamp_state(9).has_value());
        }

        // ---- WHAT THE CLAMP ACTUALLY DID ----
        //
        // Keys are prefixed pitch_ rather than pc_: an early pass's PLATFORM CARRY block already uses pc_, and
        // two unrelated blocks sharing a prefix means a json_has assertion can match the wrong one.
        {
            const auto rec2 = sdk::PlayerMgr::camera_pitch_clamp_record(0);
            const auto within = sdk::PlayerMgr::pitch_clamp_record_within_active(0);
            json_append_bool(out, "pitch_readable", rec2.has_value());
            if (rec2.has_value()) {
                json_append_double(out, "pitch_before", static_cast<double>(rec2->before), 4);
                json_append_double(out, "pitch_after", static_cast<double>(rec2->after), 4);
                json_append_bool(out, "pitch_corrected", rec2->corrected());
                // BOTH ZERO MEANS THE CLAMP HAS NEVER ENGAGED for this camera -- the fields are only written on
                // a violation. Reported explicitly, because it makes the range check below VACUOUS: zero is
                // inside every clamp, so a pass there would prove nothing.
                json_append_bool(out, "pitch_never_engaged",
                                 rec2->before == 0.0f && rec2->after == 0.0f);
                json_append_bool(out, "pitch_plausible_radians",
                                 std::fabs(rec2->before) <= 3.15f && std::fabs(rec2->after) <= 3.15f);
                // A CLAMP NEVER PUSHES A VALUE FURTHER OUT, so any recorded correction must move towards zero.
                json_append_bool(out, "pitch_correction_inward",
                                 !rec2->corrected() ||
                                     std::fabs(rec2->after) <= std::fabs(rec2->before) + 1e-6f);
            }
            json_append_bool(out, "pitch_within_determinable", within.has_value());
            json_append_bool(out, "pitch_within_active", within.value_or(false));
            // ---- THE RECOVERY TIMER AND THE SECOND PITCH LIMIT ----
            const auto tmr = sdk::PlayerMgr::pitch_recovery_timer(0);
            json_append_bool(out, "pitch_timer_readable", tmr.has_value());
            if (tmr.has_value()) {
                json_append_bool(out, "pitch_timer_active", tmr->active);
                json_append_double(out, "pitch_timer_duration", tmr->duration, 4);
                json_append_bool(out, "pitch_timer_use_cached", tmr->use_cached);
                // AN INACTIVE TIMER COUNTS AS ELAPSED, which is the accessor's own reading and the reason the
                // hard-clamp branch is the default rather than the exception.
                json_append_bool(out, "pitch_timer_inactive_is_elapsed", tmr->elapsed(0.0) || tmr->active);
                json_append_bool(out, "pitch_timer_duration_finite",
                                 tmr->duration >= 0.0 && tmr->duration < 3600.0);
                // IS THE CLAMP CORRECTING RIGHT NOW? `active` means ARMED, not running: measured live it stays
                // true after a correction finishes. Anything conditioning on "the clamp is mid-interpolation"
                // must use active AND NOT elapsed, or it takes the permissive branch forever -- the same
                // vacuity trap as a conditional that never sees its strong case.
                const auto tclk = sdk::Engine::client_time();
                const double tnow = tclk.has_value() ? tclk->seconds : 0.0;
                json_append_bool(out, "pitch_correcting", tmr->active && !tmr->elapsed(tnow));
                json_append_bool(out, "pitch_clock_available", tclk.has_value());
            }
            const auto aim = sdk::PlayerMgr::aim_tracking_limits();
            const auto astate = sdk::PlayerMgr::aim_state_raw(0);
            const auto azoom = sdk::PlayerMgr::uses_zoomed_aim_limit(0);
            const auto afov = sdk::PlayerMgr::ads_fov_active(0);
            json_append_bool(out, "aim_normal_present", aim.normal_degrees.has_value());
            json_append_bool(out, "aim_zoomed_present", aim.zoomed_degrees.has_value());
            if (aim.normal_degrees.has_value()) {
                json_append_double(out, "aim_normal", static_cast<double>(*aim.normal_degrees), 3);
            }
            if (aim.zoomed_degrees.has_value()) {
                json_append_double(out, "aim_zoomed", static_cast<double>(*aim.zoomed_degrees), 3);
            }
            // THE TWO LIMITS MUST DIFFER, or selecting between them would be pointless -- and if they do not,
            // the "zoomed" reading of the selector field loses its only supporting evidence.
            if (aim.normal_degrees.has_value() && aim.zoomed_degrees.has_value()) {
                json_append_bool(out, "aim_limits_differ", *aim.normal_degrees != *aim.zoomed_degrees);
            }
            // THE AIM STATE MACHINE, which is what actually selects the limit. Its four values were established
            // by freezing the field in a live game: 3 hip, 0 entering ADS, 1 full ADS, 2 leaving. ApplyLookDelta
            // compares against 3, so the zoomed limit covers the entire ADS lifecycle.
            json_append_bool(out, "aim_state_readable", astate.has_value());
            if (astate.has_value()) {
                json_append_double(out, "aim_state", static_cast<double>(*astate), 0);
            }
            json_append_bool(out, "aim_zoom_determinable", azoom.has_value());
            json_append_bool(out, "aim_uses_zoomed_limit", azoom.value_or(false));
            // The SEPARATE FOV flag. Freezing this one stops the FOV zoom while recoil stays ADS-light, so it is
            // not a duplicate of the state above -- it is the field a VR consumer wants to suppress.
            json_append_bool(out, "aim_fov_determinable", afov.has_value());
            json_append_bool(out, "aim_fov_active", afov.value_or(false));
            // THE ZOOM FRACTION, the game's own progress through the aim transition. Measured against the engine
            // clock the transition timer is expressed in, so it is comparable to what the renderer used.
            {
                const auto clk = sdk::Engine::client_time();
                const double now = clk.has_value() ? clk->seconds : 0.0;
                const auto zf = sdk::PlayerMgr::zoom_fraction(0, now);
                json_append_bool(out, "aim_zoom_fraction_available", zf.has_value());
                if (zf.has_value()) {
                    json_append_double(out, "aim_zoom_fraction", static_cast<double>(*zf), 4);
                }
                // THE PAIR MUST AGREE AT THE ENDPOINTS: hip is exactly 0 and full ADS exactly 1, since those two
                // states are constants in the game's own switch rather than anything interpolated.
                const bool endpoints_ok =
                    !zf.has_value() || !astate.has_value() ||
                    (*astate == 3 ? *zf == 0.0f : (*astate == 1 ? *zf == 1.0f : (*zf >= 0.0f && *zf <= 1.0f)));
                json_append_bool(out, "aim_zoom_fraction_endpoints", endpoints_ok);
            }
            // RAW WINDOWS FOR FINDING THE FLAGS, because both the crouch bit and this aim selector are offsets
            // taken from static reading, and a play session that performs the action without moving them proves
            // the offset wrong -- not that the player skipped the action. Dumping the neighbourhood lets a diff
            // across a deliberate crouch or aim locate the byte that actually moves.
            if (const auto subs = sdk::PlayerMgr::camera_sub_objects(0)) {
                if (subs->player_camera != 0) {
                    // 1005 is the mapped aim byte; the window starts 45 bytes earlier so it sits inside, not at
                    // the edge, and covers the 8-byte-aligned neighbourhood a compiler would pack flags into.
                    // THE FULL 512, because the first 96-byte window found NOTHING moving around the mapped
                    // aim byte while a crouch in the same session moved three bits in the other window. Either
                    // the offset is wrong or the state does not live in this object at all, and a wider window
                    // is what separates those.
                    const uintptr_t pcw = subs->player_camera + 768;
                    json_append_double(out, "pcam_window_at", static_cast<double>(pcw), 0);
                    json_append_string(out, "pcam_window_hex", sdk::mem::hex_window(pcw, 512).c_str());
                }
                if (subs->controller != 0) {
                    // 296 is the mapped flags dword; same reasoning.
                    const uintptr_t mmw = subs->controller + 272;
                    json_append_double(out, "mm_window_at", static_cast<double>(mmw), 0);
                    json_append_string(out, "mm_window_hex", sdk::mem::hex_window(mmw, 64).c_str());
                }
            }
            json_append_bool(out, "aim_range_refused",
                             !sdk::PlayerMgr::pitch_recovery_timer(9).has_value() &&
                                 !sdk::PlayerMgr::aim_state_raw(9).has_value());

            json_append_bool(out, "pitch_range_refused",
                             !sdk::PlayerMgr::camera_pitch_clamp_record(9).has_value() &&
                                 !sdk::PlayerMgr::pitch_clamp_record_within_active(9).has_value());
        }

        json_append_bool(out, "cc_unknown_state_refused",
                         !sdk::PlayerMgr::camera_clamp(0, "NoSuchState").has_value() &&
                             !sdk::PlayerMgr::camera_clamp(9, "Chase").has_value());
    }

    // THE PLAYER'S SUBSYSTEM TABLE. 24 slots at +228..+320; 22 are class instances built by one constructor.
    {
        const auto slots = sdk::PlayerMgr::subsystem_slots(0);
        const auto n = sdk::PlayerMgr::subsystem_count(0);
        const auto dist = sdk::PlayerMgr::subsystem_vtables_distinct(0);
        json_append_double(out, "ss_slots", static_cast<double>(slots.size()), 0);
        json_append_double(out, "ss_instances", static_cast<double>(n.value_or(0)), 0);
        json_append_bool(out, "ss_vtables_distinct", dist.value_or(false));
        // THE PARTITION, not "every slot is filled". A previous pass asserted every slot held a non-null
        // pointer; live it does not, and the suite already asserts that slot +288 is deliberately NOT a class
        // instance -- so the two claims were in tension from the start. A subsystem whose constructor has not
        // run in this state reads null, which is the same "its writer never ran" second state that the object
        // radius turned out to have. So the counts are reported and the partition is what gets asserted.
        bool all_nonnull = !slots.empty();
        size_t nonnull = 0;
        // THE TWO KNOWN EXCEPTIONS, asserted as exceptions rather than tolerated silently.
        bool k288_not_instance = false, k312_is_instance = false, k312_owner_differs = false;
        size_t owner_agrees = 0;
        for (const auto& e : slots) {
            if (e.object == 0) {
                all_nonnull = false;
            } else {
                ++nonnull;
            }
            if (e.is_class_instance && e.owner_is_player) {
                ++owner_agrees;
            }
            if (e.offset == 288) {
                k288_not_instance = !e.is_class_instance;
            }
            if (e.offset == 312) {
                k312_is_instance = e.is_class_instance;
                k312_owner_differs = !e.owner_is_player;
            }
        }
        json_append_bool(out, "ss_all_nonnull", all_nonnull);
        json_append_double(out, "ss_nonnull", static_cast<double>(nonnull), 0);
        json_append_double(out, "ss_owner_agrees", static_cast<double>(owner_agrees), 0);
        json_append_bool(out, "ss_288_not_instance", k288_not_instance);
        json_append_bool(out, "ss_312_is_instance", k312_is_instance);
        json_append_bool(out, "ss_312_owner_differs", k312_owner_differs);
        // THE THREE NAMED SLOTS MUST STILL BE WHAT EARLIER PASSES ESTABLISHED -- the table is a superset of
        // that work, so if it disagrees, one of the two is wrong.
        const auto s236 = sdk::PlayerMgr::subsystem_at(0, 236);
        const auto s252 = sdk::PlayerMgr::subsystem_at(0, 252);
        const auto s260 = sdk::PlayerMgr::subsystem_at(0, 260);
        const auto subs = sdk::PlayerMgr::camera_sub_objects(0);
        json_append_bool(out, "ss_agrees_with_earlier",
                         s236.has_value() && s252.has_value() && s260.has_value() && subs.has_value() &&
                             s236->object == subs->controller && s252->object == subs->player_camera &&
                             s260->object == subs->physics_holder);
        // AND THE RECORDED SIZES MUST BOUND THE DELEGATE ARRAYS FOUND INSIDE THEM: a node array ending past
        // the recorded size would mean the size is too small.
        bool sizes_bound_nodes = false;
        if (s236.has_value() && s252.has_value() && s260.has_value()) {
            const auto fits = [](const sdk::PlayerMgr::Subsystem& e) {
                const auto nodes = sdk::Delegates::owned_nodes(e.object, e.size_lower_bound);
                return !nodes.empty() &&
                       (nodes.back().node - e.object) + sdk::Delegates::kNodeSize <= e.size_lower_bound;
            };
            sizes_bound_nodes = fits(*s236) && fits(*s252) && fits(*s260);
        }
        json_append_bool(out, "ss_sizes_bound_nodes", sizes_bound_nodes);
        // ---- THE TEN IDENTIFIED ROLES ----
        json_append_double(out, "ss_named", static_cast<double>(sdk::PlayerMgr::named_subsystem_count()), 0);
        // EVERY recorded name must resolve to the slot it was recorded at. A name pointing at the wrong slot
        // is the failure mode of a hand-written table.
        const struct { const char* name; uintptr_t offset; } kExpect[] = {
            {"head bob", 228},    {"flashlight", 232},     {"weapon chooser", 244},
            {"target info", 248}, {"ladder", 264},         {"weapon perturb", 268},
            {"damage fx", 272},   {"special move", 276},   {"player stats", 280},
            {"input bindings", 300},
        };
        bool names_resolve = true;
        for (const auto& e : kExpect) {
            const auto got = sdk::PlayerMgr::subsystem_by_name(0, e.name);
            if (!got.has_value() || got->offset != e.offset || !got->is_class_instance) {
                names_resolve = false;
            }
        }
        json_append_bool(out, "ss_names_resolve", names_resolve);
        json_append_bool(out, "ss_unknown_name_refused",
                         !sdk::PlayerMgr::subsystem_by_name(0, "player ui").has_value() &&
                             !sdk::PlayerMgr::subsystem_by_name(0, "").has_value());
        // +312 MUST STAY UNNAMED: its identifying strings are a base class's. If a later pass names it from
        // those strings, this fails and asks for the locality evidence again.
        const auto s312 = sdk::PlayerMgr::subsystem_at(0, 312);
        json_append_bool(out, "ss_312_unnamed", s312.has_value() && s312->name == nullptr);

        // ---- CPlayerStats ----
        const auto st = sdk::PlayerMgr::player_stats(0);
        json_append_bool(out, "ps_resolved", st.has_value());
        if (st.has_value()) {
            json_append_double(out, "ps_health", static_cast<double>(st->health), 0);
            json_append_double(out, "ps_armor", static_cast<double>(st->armor), 0);
            json_append_double(out, "ps_max_health", static_cast<double>(st->max_health), 0);
            json_append_double(out, "ps_max_armor", static_cast<double>(st->max_armor), 0);
            json_append_double(out, "ps_air", st->air, 3);
            json_append_double(out, "ps_health_lost", static_cast<double>(st->health_lost), 0);
            json_append_bool(out, "ps_limits", st->limits_respected());
            json_append_bool(out, "ps_air_range", st->air_in_range());
            json_append_bool(out, "ps_alive", st->alive());
        }
        {
            // STANCE, and the eye height a room-scale consumer maps a headset onto. is_crouching()
            // already existed; what was missing is whether to TRUST it (two independently stored
            // fields, found by a differential scan) and how far the eye actually moves.
            const auto crouch = sdk::PlayerMgr::is_crouching(0);
            json_append_raw(out, "ps_crouching", crouch.has_value() ? (*crouch ? "1" : "0") : "-1");
            const auto eh = sdk::PlayerMgr::eye_height(0);
            json_append_double(out, "ps_eye_height",
                               eh.has_value() ? static_cast<double>(*eh) : -1.0, 3);
            const auto bh = sdk::PlayerMgr::body_origin_height(0);
            json_append_double(out, "ps_body_y",
                               bh.has_value() ? static_cast<double>(*bh) : -1.0, 3);
            // THE MISPAIRING THE PREVIOUS PASS USED also satisfies an ordering check, which is why the check
            // was not evidence. Reported so the suite can assert the guard is non-discriminating rather than
            // leave that as a claim in a comment.
            json_append_bool(out, "ps_mispairing_also_ordered",
                             st->health <= st->armor && st->max_health <= st->max_armor);
        }
        json_append_bool(out, "ps_consistent", sdk::PlayerMgr::player_stats_consistent(0).value_or(false));
        json_append_bool(out, "ps_range_refused", !sdk::PlayerMgr::player_stats(9).has_value() &&
                                                      !sdk::PlayerMgr::player_stats_consistent(9).has_value());
        // The subsystem is now named for its class, so the old name must no longer resolve.
        json_append_bool(out, "ps_renamed",
                         sdk::PlayerMgr::subsystem_by_name(0, "player stats").has_value() &&
                             !sdk::PlayerMgr::subsystem_by_name(0, "health armor").has_value());

        json_append_bool(out, "ss_lookup_refused",
                         !sdk::PlayerMgr::subsystem_at(0, 224).has_value() &&
                             !sdk::PlayerMgr::subsystem_at(0, 324).has_value() &&
                             sdk::PlayerMgr::subsystem_slots(9).empty() &&
                             !sdk::PlayerMgr::subsystem_count(9).has_value());
    }

    // THE SECTION TEST, and the false positives it removes. A module-range test on the first dword admits
    // function pointers, which is what over-reported the player's sub-objects by a factor of three.
    {
        const auto* gc = sdk::Modules::get().game_client();
        if (gc != nullptr && gc->base != 0) {
            // A KNOWN .text ADDRESS and a KNOWN .rdata ADDRESS, so the predicate is tested in both
            // directions rather than only where it should pass.
            const auto code_addr = gc->base + 0x2ABA0;    // the prologue bytes that fooled the old rule
            const auto data_addr = gc->base + 0x1D5B94;   // g_vtbl_CPlayerCamera
            const auto csec = sdk::Modules::section_of(code_addr);
            const auto dsec = sdk::Modules::section_of(data_addr);
            json_append_bool(out, "sec_code_resolved", csec.has_value());
            json_append_bool(out, "sec_data_resolved", dsec.has_value());
            json_append_bool(out, "sec_code_is_code",
                             csec.has_value() && csec->kind == sdk::Modules::SectionKind::Code);
            json_append_bool(out, "sec_data_is_data",
                             dsec.has_value() && dsec->kind == sdk::Modules::SectionKind::Data);
            json_append_bool(out, "sec_code_rejected", !sdk::Modules::looks_like_vtable_pointer(code_addr));
            json_append_bool(out, "sec_data_accepted", sdk::Modules::looks_like_vtable_pointer(data_addr));
            json_append_bool(out, "sec_disjoint",
                             csec.has_value() && dsec.has_value() && csec->end <= dsec->start);
            json_append_bool(out, "sec_outside_refused", !sdk::Modules::section_of(0x10).has_value() &&
                                                             !sdk::Modules::looks_like_vtable_pointer(0x10));
        }
        // THE PLAYER HAS NO VTABLE -- its first dword is a heap address. An early pass noted it; this makes
        // it a live assertion, and it is the case a consumer assuming "every object has a vtable" breaks on.
        if (const auto pp = sdk::PlayerMgr::slot(0); pp.has_value()) {
            const auto has = sdk::Modules::object_has_vtable(*pp);
            json_append_bool(out, "sec_player_determinable", has.has_value());
            json_append_bool(out, "sec_player_has_no_vtable", has.has_value() && !*has);
        }
        if (const auto subs = sdk::PlayerMgr::camera_sub_objects(0); subs.has_value()) {
            const auto a = sdk::Modules::object_has_vtable(subs->controller);
            const auto b = sdk::Modules::object_has_vtable(subs->player_camera);
            const auto c = sdk::Modules::object_has_vtable(subs->physics_holder);
            json_append_bool(out, "sec_subobjects_have_vtables",
                             a.value_or(false) && b.value_or(false) && c.value_or(false));

            // ---- WHAT EACH SUB-OBJECT SUBSCRIBES TO ----
            //
            // Using the established node layout (owner at +0x0C, validated by slot 2 == Delegate_Detach)
            // rather than the wrong-phase rule a scan suggested. See Delegates.hpp.
            const auto cs = sdk::Delegates::owned_nodes(subs->controller, 2220);
            const auto ps = sdk::Delegates::owned_nodes(subs->player_camera, 6342);
            const auto hs = sdk::Delegates::owned_nodes(subs->physics_holder, 1949);
            json_append_double(out, "dg_controller_nodes", static_cast<double>(cs.size()), 0);
            json_append_double(out, "dg_camera_nodes", static_cast<double>(ps.size()), 0);
            json_append_double(out, "dg_physics_nodes", static_cast<double>(hs.size()), 0);
            // EVERY node returned is a validated delegate -- owned_nodes filters on the detach method, so
            // this asserts the filter rather than hoping for it.
            bool all_valid = !cs.empty() && !ps.empty() && !hs.empty();
            for (const auto* v : {&cs, &ps, &hs}) {
                for (const auto& n : *v) {
                    if (!n.vtable_valid || n.owner == 0) {
                        all_valid = false;
                    }
                }
            }
            json_append_bool(out, "dg_all_validated", all_valid);
            json_append_bool(out, "dg_all_contiguous",
                             sdk::Delegates::nodes_are_contiguous(cs) &&
                                 sdk::Delegates::nodes_are_contiguous(ps) &&
                                 sdk::Delegates::nodes_are_contiguous(hs));
            // THE VTABLES MUST DIFFER PER NODE: each subscribes to a different event.
            bool distinct = true;
            for (size_t i = 0; i < ps.size(); ++i) {
                for (size_t j = i + 1; j < ps.size(); ++j) {
                    if (ps[i].vtable == ps[j].vtable) {
                        distinct = false;
                    }
                }
            }
            json_append_bool(out, "dg_camera_vtables_distinct", distinct);
            // AND EVERY NODE VTABLE MUST BE IN A DATA SECTION, which re-tests the new predicate on data
            // this pass did not pick, while the detach check tests the node independently.
            bool vt_data = !ps.empty();
            for (const auto& n : ps) {
                if (!sdk::Modules::looks_like_vtable_pointer(n.vtable)) {
                    vt_data = false;
                }
            }
            json_append_bool(out, "dg_node_vtables_in_data", vt_data);
            // SLOT 1 IS THE HANDLER, established from Delegate_Notify's dispatch. Every node's handler must
            // resolve and must be CODE -- and it must differ from the detach method in slot 2, or the two
            // slot constants would be describing the same thing.
            bool handlers_ok = !ps.empty();
            for (const auto& n : ps) {
                const auto hf = sdk::Delegates::handler_of(n.node);
                if (!hf.has_value()) {
                    handlers_ok = false;
                    break;
                }
                const auto sec = sdk::Modules::section_of(*hf);
                if (!sec.has_value() || sec->kind != sdk::Modules::SectionKind::Code ||
                    *hf == sdk::Delegates::detach_fn()) {
                    handlers_ok = false;
                    break;
                }
            }
            json_append_bool(out, "dg_handlers_resolve", handlers_ok);
            json_append_bool(out, "dg_notify_found", sdk::Delegates::notify_fn() != 0);
            json_append_bool(out, "dg_notify_is_code", [] {
                const auto sec = sdk::Modules::section_of(sdk::Delegates::notify_fn());
                return sec.has_value() && sec->kind == sdk::Modules::SectionKind::Code;
            }());
            json_append_bool(out, "dg_notify_differs_from_detach",
                             sdk::Delegates::notify_fn() != sdk::Delegates::detach_fn());
            json_append_bool(out, "dg_handler_refused", !sdk::Delegates::handler_of(0).has_value());

            // THE SUBJECTS: who publishes what these objects react to.
            const auto csub = sdk::Delegates::subscribed_subjects(subs->player_camera, 6342);
            json_append_double(out, "dg_camera_subjects", static_cast<double>(csub.size()), 0);
            json_append_bool(out, "dg_subjects_bounded", csub.size() <= ps.size());
            json_append_bool(out, "dg_empty_refused",
                             sdk::Delegates::owned_nodes(0, 2220).empty() &&
                                 sdk::Delegates::owned_nodes(subs->controller, 4).empty());
        }
    }

    // THE PLAYER'S THREE CAMERA SUB-OBJECTS. Generalising last pass's lesson: establish each pointer's class before
    // reading offsets off it, and prefer an exact structural identity where one exists.
    const auto subs = sdk::PlayerMgr::camera_sub_objects(0);
    const auto own = sdk::PlayerMgr::sub_objects_own_player(0);
    const auto cvm = sdk::PlayerMgr::controller_class_matches(0);
    const auto pvm = sdk::PlayerMgr::physics_holder_class_matches(0);
    json_append_bool(out, "so_resolved", subs.has_value());
    if (subs.has_value()) {
        json_append_bool(out, "so_all_present",
                         subs->controller != 0 && subs->player_camera != 0 && subs->physics_holder != 0);
        json_append_bool(out, "so_all_distinct",
                         subs->controller != subs->player_camera &&
                             subs->player_camera != subs->physics_holder &&
                             subs->controller != subs->physics_holder);
        // ALL THREE ARE EMBEDDED, each at its own offset, and the AIM object is the one that is not. The
        // previous version of this had it backwards -- it asserted the camera and physics holder sat "far
        // outside" the player using a 0x10000 window that both of them fall inside. Exact identities now, and
        // the negative case is what makes "embedded" a distinction rather than a label everything satisfies.

        // THE OBSERVED OFFSET, because the embedding is NOT invariant for this one. It measured player+0x3020
        // in one session and does not match in another, while the vtable check still identifies it as the
        // physics holder -- so the pointer is right and the LOCATION varies. Reported rather than asserted.
        if (const auto pp2 = sdk::PlayerMgr::slot(0); pp2.has_value() && subs->physics_holder != 0) {
            const auto delta = subs->physics_holder > *pp2 ? subs->physics_holder - *pp2 : 0;
            json_append_double(out, "so_physics_offset", static_cast<double>(delta), 0);
        }
        // ALL THREE OFFSETS, MEASURED. The constants they used to be compared against came from one player
        // instance; reporting the live numbers is what lets the next session see whether they moved.
        if (const auto offs = sdk::PlayerMgr::sub_object_offsets(0)) {
            json_append_double(out, "so_off_controller",
                               static_cast<double>(offs->controller.value_or(0)), 0);
            json_append_double(out, "so_off_camera",
                               static_cast<double>(offs->player_camera.value_or(0)), 0);
            json_append_double(out, "so_off_physics",
                               static_cast<double>(offs->physics_holder.value_or(0)), 0);
            json_append_double(out, "so_off_aim", static_cast<double>(offs->aim.value_or(0)), 0);
            json_append_bool(out, "so_off_controller_above", offs->controller.has_value());
            json_append_bool(out, "so_off_camera_above", offs->player_camera.has_value());
            json_append_bool(out, "so_off_physics_above", offs->physics_holder.has_value());
        }
        json_append_bool(out, "so_aim_determinable", sdk::PlayerMgr::aim_object(0).has_value());
        // AND IT STILL FOLLOWS THE +4 OWNER CONVENTION despite not being embedded -- which is what says it
        // belongs to this player rather than being some other object the slot happens to hold.
        const auto ao = sdk::PlayerMgr::aim_object_owns_player(0);
        json_append_bool(out, "so_aim_owner_determinable", ao.has_value());
        json_append_bool(out, "so_aim_owns_player", ao.value_or(false));
    }
    // THE SHARED CONVENTION: all three name the player as owner at +4.
    json_append_bool(out, "so_own_determinable", own.has_value());
    json_append_bool(out, "so_all_own_player", own.has_value() && *own);
    // THE EXACT IDENTITY, which no unrelated pointer satisfies.

    // AND THE CLASS GUARDS FOR THE OTHER TWO.
    json_append_bool(out, "so_controller_class", cvm.has_value() && *cvm);
    json_append_bool(out, "so_physics_class", pvm.has_value() && *pvm);
    // THE GUARDS MUST DISCRIMINATE: the controller's vtable is not the physics holder's, so cross-applying the
    // constants has to fail. Without this the two checks could both be passing on one shared vtable.
    json_append_bool(out, "so_vtables_differ",
                     sdk::PlayerMgr::kControllerVtable != sdk::PlayerMgr::kPhysicsHolderVtable &&
                         sdk::PlayerMgr::kControllerVtable != sdk::PlayerMgr::kPlayerCameraVtable &&
                         sdk::PlayerMgr::kPhysicsHolderVtable != sdk::PlayerMgr::kPlayerCameraVtable);
    json_append_bool(out, "so_range_refused",
                     !sdk::PlayerMgr::camera_sub_objects(9).has_value() &&
                         !sdk::PlayerMgr::sub_objects_own_player(9).has_value() &&
                         !sdk::PlayerMgr::sub_object_offsets(9).has_value() &&
                         !sdk::PlayerMgr::controller_class_matches(9).has_value());

    // THE HOLDER'S CLASS. Thirty-odd offsets are read off this pointer, so confirming its class first is the guard
    // that would have caught reading the physics holder with pose offsets.
    const auto hic = sdk::PlayerMgr::holder_is_player_camera(0);
    json_append_bool(out, "hc_determinable", hic.has_value());
    json_append_bool(out, "hc_is_player_camera", hic.has_value() && *hic);
    // AND IT MUST BE ABLE TO FAIL: the OTHER holder on the adjacent player field is a different class, so the same
    // check applied there must say no. That is what makes this a test rather than a tautology.
    if (const auto pp = sdk::PlayerMgr::slot(0); pp.has_value()) {
        const auto* gcm = sdk::Modules::get().game_client();
        const auto phys = sdk::mem::read_ptr(*pp + sdk::PlayerMgr::kEngineHolderField).value_or(0);
        const auto phys_vt = phys != 0 ? sdk::mem::read_ptr(phys).value_or(0) : 0;
        json_append_bool(out, "hc_physics_holder_differs",
                         gcm != nullptr && phys_vt != 0 &&
                             phys_vt != gcm->base + sdk::PlayerMgr::kPlayerCameraVtable);
    }
    json_append_bool(out, "hc_range_refused", !sdk::PlayerMgr::holder_is_player_camera(9).has_value());

    // CAMERA HEIGHT SMOOTHING. The interesting result is that it is inert twice over, so the obvious VR advice
    // ("disable smoothing") would be wasted effort -- which is only visible by checking the gate AND the speeds
    // against the clamp the producer applies.
    const auto hs = sdk::PlayerMgr::camera_height_smoothing(0);
    json_append_bool(out, "hs_readable", hs.has_value());
    if (hs.has_value()) {
        json_append_double(out, "hs_enabled", static_cast<double>(hs->enabled), 4);
        json_append_double(out, "hs_up", static_cast<double>(hs->up_speed), 4);
        json_append_double(out, "hs_down", static_cast<double>(hs->down_speed), 4);
        json_append_bool(out, "hs_has_previous", hs->has_previous);
        json_append_double(out, "hs_previous_height", static_cast<double>(hs->previous_height), 4);
        json_append_double(out, "hs_applied_delta", static_cast<double>(hs->applied_delta), 6);
        json_append_bool(out, "hs_effective", hs->is_effective());
        // THE STATE TRIO IS INTERNALLY CONSISTENT: with no previous height recorded, neither the remembered height
        // nor the applied delta can be meaningful, and the producer leaves both at their constructed zero.
        json_append_bool(out, "hs_trio_consistent",
                         hs->has_previous || (hs->previous_height == 0.0f && hs->applied_delta == 0.0f));
        // THE CLAMP IS WHAT MAKES THE SPEEDS INERT, so it is checked as arithmetic rather than asserted in prose:
        // a speed at or above 1.0 lerps straight to the target.
        const auto clamped_up = hs->up_speed < 1.0f ? hs->up_speed : 1.0f;
        const auto clamped_down = hs->down_speed < 1.0f ? hs->down_speed : 1.0f;
        json_append_bool(out, "hs_speeds_reach_clamp", clamped_up >= 1.0f && clamped_down >= 1.0f);
        // is_effective() must agree with the conjunction it documents.
        json_append_bool(out, "hs_effective_agrees",
                         hs->is_effective() == (hs->enabled == 1.0f && (clamped_up < 1.0f || clamped_down < 1.0f)));
    }
    json_append_bool(out, "hs_range_refused", !sdk::PlayerMgr::camera_height_smoothing(9).has_value());

    // THE GAME-SIDE FOV, cross-checked against the renderer's projection. Two mappings sharing no offsets: a
    // float field on the pose holder against a value derived from the projection matrix by walking the
    // shader-parameter list.
    const auto cfv = sdk::PlayerMgr::camera_fov(0);
    const auto fy = sdk::PlayerMgr::fov_y_matches_projection(0);
    const auto fx = sdk::PlayerMgr::fov_x_matches_projection(0);
    json_append_bool(out, "cf_readable", cfv.has_value());
    if (cfv.has_value()) {
        json_append_double(out, "cf_fov_y", static_cast<double>(cfv->fov_y), 6);
        json_append_double(out, "cf_fov_x", static_cast<double>(cfv->fov_x), 6);
        json_append_double(out, "cf_fov_x_degrees", static_cast<double>(cfv->fov_x * 57.29577951f), 4);
        // BOTH angles must sit strictly inside the engine's clamp; a value AT the clamp means the setting was out
        // of range, which is a different state from a wide view.
        json_append_bool(out, "cf_within_clamp",
                         cfv->fov_x > 0.0f && cfv->fov_x < sdk::PlayerMgr::kFovClampRadians &&
                             cfv->fov_y > 0.0f && cfv->fov_y < sdk::PlayerMgr::kFovClampRadians);
        // A horizontal FOV must exceed the vertical for any ratio above 1 -- the cheapest check that the pair is
        // not swapped, which is exactly the mistake the producer's argument order invites.
        json_append_bool(out, "cf_x_exceeds_y", cfv->fov_x > cfv->fov_y);
        json_append_double(out, "cf_fov_y_degrees", static_cast<double>(cfv->fov_y * 57.29577951f), 4);
        // A vertical FOV has to be a plausible angle; a wrong offset would not land in this band.
        json_append_bool(out, "cf_fov_y_plausible", cfv->fov_y > 0.3f && cfv->fov_y < 2.5f);
    }
    json_append_bool(out, "cf_fov_y_determinable", fy.has_value());
    json_append_bool(out, "cf_fov_y_matches", fy.has_value() && *fy);
    // THE HONEST PART: +292 is tested against the projection's horizontal FOV rather than assumed to be it.
    json_append_bool(out, "cf_fov_x_determinable", fx.has_value());
    json_append_bool(out, "cf_fov_x_matches", fx.has_value() && *fx);

    // THE CINEMATIC CAMERA. A VR consumer must not fight a scripted view, so the flag matters on its own.
    const auto cin = sdk::PlayerMgr::cinematic_active(0);
    const auto cin_n = sdk::PlayerMgr::cinematic_camera_count();
    const auto nz = sdk::PlayerMgr::saved_near_z(0);
    json_append_bool(out, "cf_cine_determinable", cin.has_value());
    json_append_bool(out, "cf_cine_active", cin.has_value() && *cin);
    json_append_bool(out, "cf_cine_count_available", cin_n.has_value());
    json_append_double(out, "cf_cine_count", static_cast<double>(cin_n.value_or(0)), 0);
    json_append_bool(out, "cf_saved_nearz_readable", nz.has_value());
    // WHILE NO CINEMATIC IS ACTIVE the saved NearZ is not a live value -- the path only fills it on ENTRY to a
    // cinematic and never clears it on exit. So "idle implies zero" is true only until the FIRST cinematic of
    // the session and is not an invariant: a checkpoint load plays an intro, and every run after one reads the
    // value that intro parked. Measured going red exactly that way on a cold-started fixture.
    //
    // What IS invariant is that the field holds a plausible NEAR PLANE rather than garbage: either the
    // constructed zero, or a small positive distance. A wrong offset lands on neither.
    json_append_bool(out, "cf_nearz_idle_consistent",
                     cin.has_value() && nz.has_value() &&
                         (*cin || *nz == 0.0f || (std::isfinite(*nz) && *nz > 0.0f && *nz < 100.0f)));
    json_append_double(out, "cf_saved_nearz", static_cast<double>(nz.value_or(-1.0f)), 4);
    // THE RECOVERED RATIO. Reported rather than asserted against 16:9, because the producer also scales the
    // half-angle and the live value is 3.56 -- which is the honest measurement, not a bug.
    const auto ar = sdk::PlayerMgr::aspect_ratio(0);
    json_append_bool(out, "cf_aspect_available", ar.has_value());
    json_append_double(out, "cf_aspect", static_cast<double>(ar.value_or(-1.0f)), 4);
    // It must be a positive finite number, and it must REPRODUCE fov_x from fov_y through the producer's formula --
    // that is the identity the decompile established, checked arithmetically rather than taken on trust.
    bool ar_round_trips = false;
    if (ar.has_value() && cfv.has_value()) {
        const auto rebuilt = 2.0f * std::atan(std::tan(cfv->fov_y * 0.5f) * *ar);
        ar_round_trips = std::fabs(rebuilt - cfv->fov_x) <= 1e-4f;
    }
    json_append_bool(out, "cf_aspect_round_trips", ar_round_trips);
    // THE FULL CHAIN. Every input identified: a console variable in degrees, a viewport rect, a scale setting.
    // Recomputing BOTH stored angles from them checks the whole derivation rather than its output against itself.
    const auto vr = sdk::PlayerMgr::viewport_rect(0);
    const auto fi = sdk::PlayerMgr::fov_inputs(0);
    const auto fd = sdk::PlayerMgr::fov_derivation_holds(0);
    json_append_bool(out, "cf_rect_available", vr.has_value());
    if (vr.has_value()) {
        json_append_double(out, "cf_rect_w", static_cast<double>(vr->width), 0);
        json_append_double(out, "cf_rect_h", static_cast<double>(vr->height), 0);
        // A viewport has to be a positive, sane pixel count -- a wrong offset gives zero or nonsense.
        json_append_bool(out, "cf_rect_plausible",
                         vr->width > 16 && vr->width <= 16384 && vr->height > 16 && vr->height <= 16384);
    }
    json_append_bool(out, "cf_inputs_available", fi.has_value());
    if (fi.has_value()) {
        json_append_double(out, "cf_in_fov_deg", static_cast<double>(fi->fov_y_degrees), 4);
        json_append_double(out, "cf_in_scale", static_cast<double>(fi->aspect_scale), 4);
        json_append_double(out, "cf_in_aspect", static_cast<double>(fi->aspect), 6);
        // The rect-derived aspect must equal the ratio recovered from the stored angles -- two routes to the same
        // number, one from pixels and one from trigonometry on the outputs.
        json_append_bool(out, "cf_aspect_from_rect_matches",
                         ar.has_value() && std::fabs(fi->aspect - *ar) <= 0.01f);
    }
    json_append_bool(out, "cf_derivation_determinable", fd.has_value());
    json_append_bool(out, "cf_derivation_holds", fd.has_value() && *fd);
    json_append_bool(out, "cf_range_refused",
                     !sdk::PlayerMgr::camera_fov(9).has_value() &&
                         !sdk::PlayerMgr::cinematic_active(9).has_value() &&
                         !sdk::PlayerMgr::saved_near_z(9).has_value() &&
                         !sdk::PlayerMgr::fov_y_matches_projection(9).has_value() &&
                         !sdk::PlayerMgr::aspect_ratio(9).has_value() &&
                         !sdk::PlayerMgr::viewport_rect(9).has_value() &&
                         !sdk::PlayerMgr::fov_inputs(9).has_value() &&
                         !sdk::PlayerMgr::fov_derivation_holds(9).has_value());

    // WHERE THE CAMERA POSE COMES FROM: a model socket named in gameclient's code, plus three tunable floats.
    // The socket name is the cross-check -- a string literal in the DLL against the model ASSET's own socket table,
    // two independent artefacts that must agree.
    const auto cs = sdk::PlayerMgr::camera_socket_index(0);
    const auto cs_dead = sdk::PlayerMgr::camera_socket_index(0, sdk::PlayerMgr::kCameraDeadSocketName);
    json_append_bool(out, "cs_camera_socket_found", cs.has_value());
    json_append_double(out, "cs_camera_socket_index", static_cast<double>(cs.value_or(9999)), 0);
    json_append_bool(out, "cs_dead_socket_found", cs_dead.has_value());
    json_append_double(out, "cs_dead_socket_index", static_cast<double>(cs_dead.value_or(9999)), 0);
    // The two names must resolve to DIFFERENT sockets; if they collided, the death-state branch would be a no-op.
    json_append_bool(out, "cs_sockets_distinct",
                     cs.has_value() && cs_dead.has_value() && *cs != *cs_dead);
    // A name the asset does not carry must be refused rather than resolving to something.
    json_append_bool(out, "cs_absent_socket_refused",
                     !sdk::PlayerMgr::camera_socket_index(0, "NoSuchSocketAtAll").has_value() &&
                         !sdk::PlayerMgr::camera_socket_index(0, nullptr).has_value() &&
                         !sdk::PlayerMgr::camera_socket_index(9).has_value());

    const auto cofs = sdk::PlayerMgr::camera_attached_offset();
    json_append_bool(out, "cs_offset_readable", cofs.has_value());
    if (cofs.has_value()) {
        json_append_double(out, "cs_offset_x", static_cast<double>((*cofs)[0]), 4);
        json_append_double(out, "cs_offset_y", static_cast<double>((*cofs)[1]), 4);
        json_append_double(out, "cs_offset_z", static_cast<double>((*cofs)[2]), 4);
        json_append_bool(out, "cs_offset_finite",
                         std::isfinite((*cofs)[0]) && std::isfinite((*cofs)[1]) && std::isfinite((*cofs)[2]));
        // Each component must resolve to a DISTINCT cache record -- one shared record would mean the three axes
        // are the same variable and a consumer could not offset independently.
        const auto vx = sdk::Engine::find_cached_var("CameraAttachedOffsetX");
        const auto vy = sdk::Engine::find_cached_var("CameraAttachedOffsetY");
        const auto vz = sdk::Engine::find_cached_var("CameraAttachedOffsetZ");
        json_append_bool(out, "cs_offset_records_distinct",
                         vx.has_value() && vy.has_value() && vz.has_value() && vx->record != vy->record &&
                             vy->record != vz->record && vx->record != vz->record);
    }

    // THE CAMERA'S COMPOSED ROTATION. The write path multiplies two stored quaternions and pushes the product to
    // the camera object, so recomputing the product here and comparing against what the object carries
    // establishes BOTH operands AND the multiplication order in one measurement.
    const auto cro = sdk::PlayerMgr::camera_rotation_operands(0);
    const auto aim_roll = sdk::PlayerMgr::aim_roll(0);
    const auto avd = sdk::PlayerMgr::aim_vs_view(0);
    const auto cro_ok = sdk::PlayerMgr::camera_rotation_is_composed(0);
    json_append_bool(out, "cro_resolved", cro.has_value());
    json_append_bool(out, "cro_determinable", cro_ok.has_value());
    json_append_bool(out, "avd_readable", avd.has_value());
    json_append_bool(out, "avd_composed", avd.has_value() && avd->composed);
    json_append_double(out, "avd_angle_deg", avd.has_value() ? avd->angle * 57.2957795 : -1.0, 4);
    json_append_double(out, "avd_view_fz", avd.has_value() ? avd->view_forward[2] : 0.0, 4);
    json_append_double(out, "avd_aim_fz", avd.has_value() ? avd->aim_forward[2] : 0.0, 4);
    json_append_bool(out, "avd_body_readable", avd.has_value() && avd->body_readable);
    json_append_double(out, "avd_body_view_deg",
                       avd.has_value() ? avd->body_to_view_angle * 57.2957795 : -1.0, 4);
    json_append_double(out, "avd_body_aim_deg",
                       avd.has_value() ? avd->body_to_aim_angle * 57.2957795 : -1.0, 4);
    {
        const auto yaw = sdk::PlayerMgr::aim_yaw(0);
        const auto pitch = sdk::PlayerMgr::aim_pitch(0);
        // AMMUNITION. `ammo_total` is the cheap liveness probe (can this weapon system fire at
        // all); `ammo_held` is the consumer view, and it is what makes the array's indexing
        // checkable -- firing must move exactly the entry whose name matches the equipped weapon.
        const auto ammo_total = sdk::PlayerMgr::ammo_total(0);
        json_append_bool(out, "ammo_readable", ammo_total.has_value());
        json_append_double(out, "ammo_total", static_cast<double>(ammo_total.value_or(0)), 0);
        {
            const auto held = sdk::PlayerMgr::ammo_held(0);
            json_append_double(out, "ammo_kinds_held", static_cast<double>(held.size()), 0);
            // THE WHOLE HOLDINGS LIST, name and count. Reporting only the largest was a mistake
            // worth recording: it reads as "the equipped weapon's ammunition" and is not -- firing
            // a pistol while carrying 82 rifle rounds moves neither the largest entry nor its
            // name, and a check built on that proxy fails for a reason unrelated to the mapping.
            // A consumer diffing this list can see exactly which kind moved.
            std::string items = "[";
            bool first = true;
            for (const auto& h : held) {
                if (!first) {
                    items += ",";
                }
                first = false;
                items += "{\"name\":";
                json_escape_append(items, h.name);
                items += ",\"count\":" + std::to_string(h.count) + "}";
            }
            items += "]";
            json_append_raw(out, "ammo_held", items.c_str());
        }

        const auto plim = sdk::PlayerMgr::pitch_limits(0);
        json_append_bool(out, "pitch_limits_readable", plim.has_value());
        json_append_double(out, "pitch_up_deg", plim.has_value() ? plim->up * 57.2957795 : 0.0, 3);
        json_append_double(out, "pitch_down_deg", plim.has_value() ? plim->down * 57.2957795 : 0.0, 3);
        json_append_double(out, "aim_pitch_deg", pitch.value_or(0.0f) * 57.2957795, 4);
        json_append_bool(out, "aim_yaw_readable", yaw.has_value());
        json_append_double(out, "aim_yaw_deg", yaw.value_or(0.0f) * 57.2957795, 4);
    }
    json_append_bool(out, "avd_shell_readable", avd.has_value() && avd->shell_readable);
    json_append_bool(out, "avd_shell_is_body", avd.has_value() && avd->shell_is_body);
    json_append_double(out, "avd_shell_view_deg",
                       avd.has_value() ? avd->shell_to_view_angle * 57.2957795 : -1.0, 4);
    json_append_double(out, "avd_shell_aim_deg",
                       avd.has_value() ? avd->shell_to_aim_angle * 57.2957795 : -1.0, 4);
    json_append_bool(out, "aim_roll_readable", aim_roll.has_value());
    json_append_double(out, "aim_roll_deg", aim_roll.value_or(0.0f) * 57.2957795, 5);
    json_append_bool(out, "cro_composed_matches", cro_ok.has_value() && *cro_ok);
    if (cro.has_value()) {
        const auto unit = [](const std::array<float, 4>& q) {
            const auto n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
            return std::fabs(n - 1.0f) <= 0.01f;
        };
        // All four must be real orientations; a wrong offset does not produce norm 1.
        json_append_bool(out, "cro_outer_unit", unit(cro->outer));
        json_append_bool(out, "cro_inner_unit", unit(cro->inner));
        json_append_bool(out, "cro_actual_unit", unit(cro->actual));
        // THE OPERANDS MUST BE DIFFERENT QUATERNIONS. If one were identity the product would equal the other and
        // the test would pass without establishing the composition at all -- so identity is measured, not assumed
        // away.
        const auto is_identity = [](const std::array<float, 4>& q) {
            return std::fabs(q[0]) < 1e-4f && std::fabs(q[1]) < 1e-4f && std::fabs(q[2]) < 1e-4f &&
                   std::fabs(std::fabs(q[3]) - 1.0f) < 1e-4f;
        };
        json_append_bool(out, "cro_outer_identity", is_identity(cro->outer));
        json_append_bool(out, "cro_inner_identity", is_identity(cro->inner));
        // The WRONG ORDER must be distinguishable, otherwise the order claim is untestable on this data.
        regenny::LTRotation ra{}, rb{};
        ra.x = cro->outer[0]; ra.y = cro->outer[1]; ra.z = cro->outer[2]; ra.w = cro->outer[3];
        rb.x = cro->inner[0]; rb.y = cro->inner[1]; rb.z = cro->inner[2]; rb.w = cro->inner[3];
        const auto rev = sdk::multiply_rotations(rb, ra);
        const std::array<float, 4> reversed{rev.x, rev.y, rev.z, rev.w};
        float d_same = 0.0f, d_flip = 0.0f;
        for (size_t i = 0; i < 4; ++i) {
            d_same += std::fabs(reversed[i] - cro->actual[i]);
            d_flip += std::fabs(reversed[i] + cro->actual[i]);
        }
        json_append_double(out, "cro_reversed_error", static_cast<double>(std::min(d_same, d_flip)), 5);
    }
    json_append_bool(out, "cro_range_refused",
                     !sdk::PlayerMgr::camera_rotation_operands(9).has_value() &&
                         !sdk::PlayerMgr::camera_rotation_is_composed(9).has_value() &&
                         !sdk::PlayerMgr::camera_attachment_driving(9).has_value() &&
                         !sdk::PlayerMgr::probe_outer_operand(9).has_value());

    // PLATFORM CARRY -- the producer of the external-delta accumulator. Live nothing is being ridden, so the
    // assertions are about the IDLE state being internally consistent and about the carried case being reported
    // as unavailable rather than as agreement.
    const auto pc = sdk::PlayerMgr::platform_carry(0);
    const auto pc_cur = sdk::PlayerMgr::platform_carry_position_current(0);
    json_append_bool(out, "pc_resolved", pc.has_value());
    if (pc.has_value()) {
        json_append_bool(out, "pc_active", pc->active);
        // active must agree with the object being non-zero -- the flag is derived, not stored.
        json_append_bool(out, "pc_active_agrees", pc->active == (pc->object != 0));
        const auto zero3 = [](const std::array<float, 3>& v) {
            return v[0] == 0.0f && v[1] == 0.0f && v[2] == 0.0f;
        };
        // WHEN NOT CARRYING, the recorded platform position and the accumulator must both be zero: nothing to
        // ride means nothing recorded and nothing accumulated. That ties the two fields together.
        // WHILE IDLE the accumulator must be zero -- the commit clears it every frame -- and the platform
        // position must NOT be offered at all, because the underlying dwords are stale leftovers.
        bool idle_consistent = true;
        if (!pc->active) {
            const auto ms2 = sdk::PlayerMgr::movement_state(0);
            idle_consistent = !pc->last_position.has_value() && ms2.has_value() && zero3(ms2->external_delta);
        }
        json_append_bool(out, "pc_idle_consistent", idle_consistent);
        json_append_bool(out, "pc_position_offered_only_when_active",
                         pc->last_position.has_value() == pc->active);
        json_append_bool(out, "pc_position_finite",
                         !pc->last_position.has_value() ||
                             (std::isfinite((*pc->last_position)[0]) && std::isfinite((*pc->last_position)[1]) &&
                              std::isfinite((*pc->last_position)[2])));
    }
    // The comparison is UNAVAILABLE while idle, which is deliberately distinct from "they disagree".
    json_append_bool(out, "pc_compare_unavailable_when_idle",
                     pc.has_value() && (pc->active ? pc_cur.has_value() : !pc_cur.has_value()));
    json_append_bool(out, "pc_range_refused",
                     !sdk::PlayerMgr::platform_carry(9).has_value() &&
                         !sdk::PlayerMgr::platform_carry_position_current(9).has_value());

    // THE THREE PLAYER ENGINE OBJECTS. The point is that they are DIFFERENT and each has a role; confusing them
    // yields plausible results rather than errors, which is exactly what happened while mapping this.
    const auto eo = sdk::PlayerMgr::engine_objects(0);
    const auto eo_is_model = sdk::PlayerMgr::engine_object_is_model_object(0);
    json_append_bool(out, "eo_resolved", eo.has_value());
    if (eo.has_value()) {
        json_append_bool(out, "eo_camera_present", eo->camera != 0);
        json_append_bool(out, "eo_model_present", eo->model != 0);
        json_append_bool(out, "eo_shell_present", eo->shell != 0);
        json_append_bool(out, "eo_all_distinct",
                         eo->camera != 0 && eo->model != 0 && eo->shell != 0 && eo->camera != eo->model &&
                             eo->model != eo->shell && eo->camera != eo->shell);
    }
    // TWO ROUTES SHARING NO OFFSETS: *(*(player+260)+320) versus *(*(player+252)+600).
    json_append_bool(out, "eo_physics_is_model_determinable", eo_is_model.has_value());
    json_append_bool(out, "eo_physics_is_model", eo_is_model.has_value() && *eo_is_model);
    // The applied pose must still be what the camera object carries -- the invariant a VR override depends on.
    // THE TORN-AWARE FORM, because the single-read bool races and this was the last caller still using it.
    //
    // Both this and pmgr_applied_matches_object call the same accessor, and in ONE response they disagreed --
    // True at the earlier site, False here. The accessor reads the pose, then the camera object, so a frame
    // landing between them makes the two differ; the response takes long enough to build that two calls at
    // different points in it straddle frames. That is exactly what PoseAgreement's double read exists to
    // distinguish, and reporting "differs" for a torn read sends the next reader hunting a moved offset.
    const auto eo_agree = sdk::PlayerMgr::applied_pose_agreement(0);
    json_append_bool(out, "eo_pose_match_determinable",
                     eo_agree != sdk::PlayerMgr::PoseAgreement::Unreadable);
    json_append_bool(out, "eo_pose_never_differs",
                     eo_agree != sdk::PlayerMgr::PoseAgreement::Differ);
    json_append_bool(out, "eo_pose_matches_camera",
                     eo_agree == sdk::PlayerMgr::PoseAgreement::Equal);
    // A CENSUS, because a single verdict cannot describe a PHASE relationship.
    //
    // Differ here is neither a race nor a wrong offset: within a frame the applied pose is updated before the
    // camera object, so for part of every frame the two hold different -- and individually STABLE -- values.
    // The double read cannot see that, because nothing moves during the microseconds it samples.
    //
    // What is true and useful: they are the SAME quantity, coinciding once the frame settles. So the shape to
    // report is how often 16 samples find them equal. A VR override reading both must take them from the same
    // phase, or take one consistently.
    {
        const uint64_t eo_vw_before = ViewHook::get().observed().calls;
        const auto eo_cen = sdk::PlayerMgr::agreement_census(0, 1, 16);
        const uint64_t eo_vw_after = ViewHook::get().observed().calls;
        json_append_double(out, "eo_view_writes_during",
                           static_cast<double>(eo_vw_after - eo_vw_before), 0);
        json_append_double(out, "eo_pose_equal", static_cast<double>(eo_cen.equal), 0);
        json_append_double(out, "eo_pose_differ", static_cast<double>(eo_cen.differ), 0);
        json_append_double(out, "eo_pose_torn", static_cast<double>(eo_cen.torn), 0);
        json_append_double(out, "eo_pose_samples",
                           static_cast<double>(eo_cen.equal + eo_cen.differ + eo_cen.torn +
                                               eo_cen.unreadable), 0);
    }
    // The wrong-holder trap, reproduced as a check: reading the POSE offsets off the PHYSICS holder must not
    // yield a usable pose. If it ever did, the two holders would be interchangeable and the warning is wrong.
    if (const auto p = sdk::PlayerMgr::slot(0); p.has_value()) {
        bool wrong_holder_refused = true;
        if (const auto phys_holder = sdk::mem::read_ptr(*p + sdk::PlayerMgr::kEngineHolderField);
            phys_holder.has_value() && *phys_holder != 0) {
            // read_pose validates the quaternion, so a mis-offset holder is refused by construction.
            wrong_holder_refused = !sdk::PlayerMgr::read_pose(*phys_holder).has_value();
        }
        json_append_bool(out, "eo_wrong_holder_refused", wrong_holder_refused);
    }
    json_append_bool(out, "eo_range_refused",
                     !sdk::PlayerMgr::engine_objects(9).has_value() &&
                         !sdk::PlayerMgr::engine_object_is_model_object(9).has_value());

    // THE GAME-SIDE MOVEMENT STATE -- the velocity the engine's is not. The invariant that establishes these
    // offsets is that the controller's cached position is a VERBATIM COPY of the engine object's, refreshed every
    // frame, so it must compare bit-equal. A wrong offset lands on another triple and fails.
    const auto ms = sdk::PlayerMgr::movement_state(0);
    const auto ms_match = sdk::PlayerMgr::cached_position_matches_engine(0);
    const auto ms_speed = sdk::PlayerMgr::speed(0);
    json_append_bool(out, "ms_resolved", ms.has_value());
    json_append_bool(out, "ms_position_determinable", ms_match.has_value());
    json_append_bool(out, "ms_position_matches_engine", ms_match.has_value() && *ms_match);
    // AND AS A VERDICT. See the note on pmgr_rot_agreement: the bool above cannot distinguish "the offsets are
    // wrong" from "a frame landed between the two reads", and only the first is a defect.
    {
        const auto v = sdk::PlayerMgr::cached_position_agreement(0);
        const char* n = v == sdk::PlayerMgr::PoseAgreement::Equal      ? "equal"
                        : v == sdk::PlayerMgr::PoseAgreement::Differ   ? "differ"
                        : v == sdk::PlayerMgr::PoseAgreement::Torn     ? "torn"
                                                                       : "unreadable";
        json_append_string(out, "ms_position_agreement", n);
        json_append_bool(out, "ms_position_never_differs", v != sdk::PlayerMgr::PoseAgreement::Differ);
    }
    if (ms.has_value()) {
        const auto finite3 = [](const std::array<float, 3>& v) {
            return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
        };
        json_append_bool(out, "ms_velocity_finite", finite3(ms->velocity));
        json_append_double(out, "ms_vel_x", ms->velocity[0], 3);
        json_append_double(out, "ms_vel_y", ms->velocity[1], 3);
        json_append_double(out, "ms_vel_z", ms->velocity[2], 3);
        json_append_bool(out, "ms_position_finite", finite3(ms->cached_position));
        // The commit clears the accumulator unconditionally at the end, so after any completed frame it reads
        // zero. Non-zero here would mean the field is not what this claims, or the frame was caught mid-commit.
        json_append_bool(out, "ms_external_delta_zero",
                         ms->external_delta[0] == 0.0f && ms->external_delta[1] == 0.0f &&
                             ms->external_delta[2] == 0.0f);
        // speed() must be the magnitude of the very vector movement_state() reports -- one derived from the
        // other, so a mismatch means they read different memory.
        const auto mag = std::sqrt(ms->velocity[0] * ms->velocity[0] + ms->velocity[1] * ms->velocity[1] +
                                   ms->velocity[2] * ms->velocity[2]);
        json_append_bool(out, "ms_speed_is_magnitude",
                         ms_speed.has_value() && std::fabs(*ms_speed - mag) <= 1e-6f);
        json_append_double(out, "ms_speed", static_cast<double>(ms_speed.value_or(-1.0f)), 4);
        // The cached position must also be a plausible world coordinate rather than a denormal or a huge value,
        // which is what a wrong offset landing on packed data tends to look like.
        const auto plausible = [](float v) { return std::fabs(v) < 1.0e6f; };
        json_append_bool(out, "ms_position_plausible",
                         plausible(ms->cached_position[0]) && plausible(ms->cached_position[1]) &&
                             plausible(ms->cached_position[2]));
    }
    // THE INVARIANT HELD CONTINUOUSLY, not just at one instant. The commit refreshes the cached position every
    // frame, so sampling the predicate repeatedly across frames is a much stronger statement than one read: a
    // coincidentally-equal triple would drift apart, and a stale controller would stop tracking.
    size_t ms_track_ok = 0, ms_track_total = 0;
    for (size_t i = 0; i < 8; ++i) {
        if (const auto m = sdk::PlayerMgr::cached_position_matches_engine(0); m.has_value()) {
            ++ms_track_total;
            if (*m) { ++ms_track_ok; }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    json_append_double(out, "ms_track_ok", static_cast<double>(ms_track_ok), 0);
    json_append_double(out, "ms_track_total", static_cast<double>(ms_track_total), 0);

    json_append_bool(out, "ms_range_refused",
                     !sdk::PlayerMgr::movement_state(9).has_value() &&
                         !sdk::PlayerMgr::speed(9).has_value() &&
                         !sdk::PlayerMgr::cached_position_matches_engine(9).has_value());

    // THE TWO ROUTES DISAGREE, so characterise both rather than picking one. Each object's kind, handle and
    // dims say what it is; a consumer needs to know WHICH player object it is holding.
    if (const auto shell_lp = sdk::CClientShell::local_player(0); shell_lp.has_value()) {
        json_append_double(out, "pe_shell_object", static_cast<double>(
                                                      reinterpret_cast<uintptr_t>(shell_lp->object)), 0);
        json_append_double(out, "pe_shell_handle", static_cast<double>(shell_lp->handle), 0);
        if (const auto si = sdk::object_info(shell_lp->object); si.has_value()) {
            json_append_double(out, "pe_shell_kind", static_cast<double>(static_cast<int>(si->kind)), 0);
            json_append_double(out, "pe_shell_slot_index", static_cast<double>(si->slot_index), 0);
        }
        if (const auto sd = sdk::object_dims(shell_lp->object); sd.has_value()) {
            json_append_double(out, "pe_shell_dim_y", static_cast<double>(sd->y), 3);
        }
    }
    if (pe_obj.has_value()) {
        json_append_double(out, "pe_game_object", static_cast<double>(*pe_obj), 0);
        const auto* go = reinterpret_cast<const regenny::LTObject*>(*pe_obj);
        if (const auto gi = sdk::object_info(go); gi.has_value()) {
            json_append_double(out, "pe_game_kind", static_cast<double>(static_cast<int>(gi->kind)), 0);
            json_append_double(out, "pe_game_handle", static_cast<double>(gi->handle), 0);
            json_append_double(out, "pe_game_slot_index", static_cast<double>(gi->slot_index), 0);
        }
        if (const auto gd = sdk::object_dims(go); gd.has_value()) {
            json_append_double(out, "pe_game_dim_y", static_cast<double>(gd->y), 3);
        }
        // Is the game's object zeroed while the shell's is not? That would say the zeroing targets one of them
        // specifically, which is the fact a consumer needs.
        if (const auto shell_lp2 = sdk::CClientShell::local_player(0);
            shell_lp2.has_value() && shell_lp2->object != nullptr) {
            const auto sv = sdk::Physics::velocity(reinterpret_cast<uintptr_t>(shell_lp2->object));
            json_append_bool(out, "pe_shell_velocity_zero",
                             sv.has_value() && (*sv)[0] == 0.0f && (*sv)[1] == 0.0f && (*sv)[2] == 0.0f);
            json_append_bool(out, "pe_shell_velocity_readable", sv.has_value());
        }
    }

    // The predicate must not claim an arbitrary address is a zeroed player.
    json_append_bool(out, "pe_predicate_refuses_other",
                     !sdk::Physics::velocity_zeroed_by_game(0) &&
                         !sdk::Physics::velocity_zeroed_by_game(0xDEAD0000));
    // An out-of-range slot yields nothing rather than slot 0's answer.
    json_append_bool(out, "pe_range_refused",
                     !sdk::PlayerMgr::engine_object(9).has_value() &&
                         !sdk::PlayerMgr::engine_object_is_shell_object(9).has_value() &&
                         !sdk::PlayerMgr::movement_controller(9).has_value() &&
                         !sdk::PlayerMgr::movement_controller_owner_agrees(9).has_value());

    // GAMECLIENT'S OWN INTERFACE POINTER GLOBALS. Discovery is by VTABLE and accounting is by POINTER against
    // the registry, so the two never consult each other. The load-bearing part is the EXCLUSION: without it the
    // console-variable cache pairs flood the result, so the count itself is the evidence the filter works.
    const auto slots = sdk::interfaces::Registry::get().gameclient_interface_slots();
    size_t gs_accounted = 0, gs_distinct_ifaces = 0, gs_distinct_objs = 0;
    std::vector<std::string> gs_names;
    std::vector<uintptr_t> gs_objs;
    for (const auto& sl : slots) {
        if (!sl.interface_name.empty()) {
            ++gs_accounted;
            gs_names.push_back(sl.interface_name);
        }
        if (sl.value != 0) { gs_objs.push_back(sl.value); }
    }
    std::sort(gs_names.begin(), gs_names.end());
    gs_distinct_ifaces = static_cast<size_t>(std::unique(gs_names.begin(), gs_names.end()) - gs_names.begin());
    std::sort(gs_objs.begin(), gs_objs.end());
    gs_distinct_objs = static_cast<size_t>(std::unique(gs_objs.begin(), gs_objs.end()) - gs_objs.begin());
    json_append_double(out, "gs_total", static_cast<double>(slots.size()), 0);
    json_append_double(out, "gs_accounted", static_cast<double>(gs_accounted), 0);
    json_append_double(out, "gs_distinct_interfaces", static_cast<double>(gs_distinct_ifaces), 0);
    json_append_double(out, "gs_distinct_objects", static_cast<double>(gs_distinct_objs), 0);
    // Every slot must name a catalogued implementation class -- that is what discovery selected on.
    bool gs_all_classed = !slots.empty();
    for (const auto& sl : slots) {
        if (sl.class_name.empty()) { gs_all_classed = false; break; }
    }
    json_append_bool(out, "gs_all_classed", gs_all_classed);
    // A specific one a consumer would reach for, and it must hold the same object ILTPhysics resolves to.
    // NOTE THE NAME: the registry distinguishes ".Client" from ".Default" for the interfaces that have both a
    // client and a server implementation, so ILTPhysics is published as "ILTPhysics.Client". Looking for
    // ".Default" finds nothing -- which is how this was noticed.
    const auto phys_slot = sdk::interfaces::Registry::get().find_gameclient_slot("ILTPhysics.Client");
    json_append_bool(out, "gs_physics_found",
                     phys_slot.has_value() && phys_slot->value == sdk::Physics::instance() &&
                         phys_slot->class_name == "CLTPhysicsClient");
    // The unaccounted slots are a real state, not a defect: gameclient can hold a pointer the registry does not
    // currently publish. Report whether the registry even knows a name for that class's interface.
    {
        bool knows_gameutil = false;
        for (const auto& n : sdk::interfaces::Registry::get().names()) {
            if (n.rfind("ILTGameUtil", 0) == 0) { knows_gameutil = true; break; }
        }
        json_append_bool(out, "gs_registry_knows_gameutil", knows_gameutil);
    }
    json_append_bool(out, "gs_absent_refused",
                     !sdk::interfaces::Registry::get().find_gameclient_slot("INoSuchInterface").has_value() &&
                         !sdk::interfaces::Registry::get().find_gameclient_slot("").has_value());
    {
        std::string list;
        for (const auto& sl : slots) {
            if (!list.empty()) { list += ';'; }
            char buf[32]{};
            std::snprintf(buf, sizeof(buf), "%X|", static_cast<unsigned>(sl.offset));
            list += buf;
            list += sl.class_name;
            list += '|';
            list += sl.interface_name.empty() ? "-" : sl.interface_name;
        }
        json_append_string(out, "gs_list", list.c_str());
    }

    // EVERY CACHED CONSOLE VARIABLE, BY DISCOVERY. The pattern is scanned out of gameclient's .data rather than
    // listed, and the check is that a data scan and a hash-table walk agree: each discovered record must be
    // findable in the console tables BY ITS OWN NAME, at the very address the cache holds.
    const auto disc = sdk::Engine::cached_console_vars();
    size_t cv_named = 0, cv_agree = 0, cv_distinct_recs = 0, cv_same_owner = 0;
    std::vector<uintptr_t> cv_recs;
    const auto cv_owner = sdk::Engine::cached_var_owner();
    for (const auto& v : disc) {
        if (!v.name.empty()) { ++cv_named; }
        if (v.owner == cv_owner && cv_owner != 0) { ++cv_same_owner; }
        if (v.record != 0) { cv_recs.push_back(v.record); }
        // The name round-trips through the console tables to the same record.
        if (const auto found = sdk::Engine::console_var(v.name.c_str());
            found.has_value() && found->address == v.record) {
            ++cv_agree;
        }
    }
    std::sort(cv_recs.begin(), cv_recs.end());
    cv_distinct_recs = static_cast<size_t>(std::unique(cv_recs.begin(), cv_recs.end()) - cv_recs.begin());
    json_append_double(out, "cv_total", static_cast<double>(disc.size()), 0);
    // THE EXCLUSION IS LOAD-BEARING: without dropping cache-pair owner words the interface-slot scan would
    // return these hundreds instead of ~20, so the ratio is the evidence the filter works.
    json_append_bool(out, "gs_far_fewer_than_cache_pairs", slots.size() * 4 < disc.size());
    json_append_double(out, "cv_named", static_cast<double>(cv_named), 0);
    json_append_double(out, "cv_agree", static_cast<double>(cv_agree), 0);
    json_append_double(out, "cv_distinct_records", static_cast<double>(cv_distinct_recs), 0);
    json_append_double(out, "cv_same_owner", static_cast<double>(cv_same_owner), 0);
    json_append_bool(out, "cv_owner_resolved", cv_owner != 0);
    // THE CAMERA'S HARDCODED TABLE MUST BE A SUBSET of what discovery finds, at the same cache offsets. Two
    // routes to the same 67 pairs: one a recorded table, the other a scan that knows nothing about the camera.
    size_t cv_cam_found = 0, cv_cam_total = 0;
    for (const auto& t : sdk::Engine::camera_tunable_cache()) {
        if (t.record == 0) { continue; }
        ++cv_cam_total;
        for (const auto& d : disc) {
            if (d.cache_offset == t.cache_offset) {
                if (d.name == t.name && d.record == t.record) { ++cv_cam_found; }
                break;
            }
        }
    }
    json_append_double(out, "cv_camera_found", static_cast<double>(cv_cam_found), 0);
    json_append_double(out, "cv_camera_total", static_cast<double>(cv_cam_total), 0);
    // A VR-relevant name a consumer would actually reach for must be discoverable and readable.
    const auto shake = sdk::Engine::find_cached_var("DisableCameraShake");
    json_append_bool(out, "cv_shake_found", shake.has_value() && shake->record != 0);
    if (shake.has_value()) {
        json_append_bool(out, "cv_shake_readable", sdk::Engine::read_cached(*shake).has_value());
    }
    // The discovered set as a compact list, so the reversing side can name these globals in IDA from the same
    // data the SDK produced rather than from a re-implementation of the scan.
    {
        std::string list;
        for (const auto& v : disc) {
            if (!list.empty()) { list += ';'; }
            char buf[32]{};
            std::snprintf(buf, sizeof(buf), "%X|", static_cast<unsigned>(v.cache_offset));
            list += buf;
            list += v.name;
        }
        json_append_string(out, "cv_list", list.c_str());
    }
    json_append_bool(out, "cv_absent_refused",
                     !sdk::Engine::find_cached_var("NoSuchCachedVariable").has_value() &&
                         !sdk::Engine::find_cached_var("").has_value());

    // ---- THE VIEW HOOK ------------------------------------------------------------------------
    //
    // Data only, per AGENT.MD rule 2: the pass/fail judgement lives host-side. What matters here is that the
    // CALL COUNT is exposed, because "the hook is installed" is a static shape and never sufficient -- the
    // suite polls this twice and requires it to ADVANCE (TESTING.MD rule 3).
    {
        const auto vh = ViewHook::get().observed();
        const auto* gc = sdk::Modules::get().game_client();
        json_append_bool(out, "vh_installed", vh.installed);
        json_append_double(out, "vh_target", static_cast<double>(vh.target), 0);
        json_append_double(out, "vh_calls", static_cast<double>(vh.calls), 0);
        json_append_double(out, "vh_last_this", static_cast<double>(vh.last_this), 0);
        json_append_double(out, "vh_last_a3", static_cast<double>(vh.last_a3), 6);
        // THE TARGET MUST LIE INSIDE gameclient.dll. A pattern that matched in the wrong module, or a
        // resolution that drifted, shows up here rather than as a mystery crash -- and the host cross-checks it
        // against its OWN Toolhelp32 view of the module, which this cannot fake.
        json_append_bool(out, "vh_target_in_gameclient",
                         gc != nullptr && gc->base != 0 && vh.target >= gc->base &&
                             vh.target < gc->base + gc->size);
        json_append_bool(out, "vh_pose_installed", vh.pose_installed);
        json_append_double(out, "vh_pose_target", static_cast<double>(vh.pose_target), 0);
        json_append_double(out, "vh_pose_calls", static_cast<double>(vh.pose_calls), 0);
        // SAME-PHASE AGREEMENT, measured on the engine thread inside the detour. The out-of-band census in
        // the pmgr_/eo_ blocks cannot answer this: UpdateViewPose rewrites the pose every frame, so an IPC
        // reader always lands mid-update. This is the number that means something.
        // THE OVERRIDE EXPERIMENT'S RESULT. carried == applied means the engine propagated OUR pose into the
        // camera object -- the go/no-go for driving a head-tracked view from this hook.
        json_append_double(out, "vh_ov_frames_left", static_cast<double>(vh.override_frames_left), 0);
        json_append_double(out, "vh_ov_yaw_deg", static_cast<double>(vh.override_yaw_deg), 3);
        json_append_double(out, "vh_ov_applied", static_cast<double>(vh.override_applied), 0);
        json_append_double(out, "vh_ov_carried", static_cast<double>(vh.override_carried), 0);
        json_append_double(out, "vh_ov_rejected", static_cast<double>(vh.override_rejected), 0);
        json_append_double(out, "vh_ov_pose_held", static_cast<double>(vh.override_pose_held), 0);
        json_append_double(out, "vh_ov_max_drift_deg", static_cast<double>(vh.override_max_drift_deg), 4);
        json_append_double(out, "vh_ov_drift_frames", static_cast<double>(vh.override_drift_frames), 0);
        json_append_double(out, "vh_ov_inflight", static_cast<double>(vh.override_inflight), 0);
        json_append_double(out, "vh_ov_applied_writes", static_cast<double>(vh.override_applied_writes), 0);
        json_append_bool(out, "vh_setrot_installed", vh.setrot_installed);
        json_append_double(out, "vh_setrot_target", static_cast<double>(vh.setrot_target), 0);
        json_append_double(out, "vh_setrot_calls", static_cast<double>(vh.setrot_calls), 0);
        json_append_double(out, "vh_setrot_camera", static_cast<double>(vh.setrot_camera), 0);
        json_append_double(out, "vh_setrot_overridden", static_cast<double>(vh.setrot_overridden), 0);
        json_append_bool(out, "vh_spr_installed", vh.spr_installed);
        json_append_double(out, "vh_spr_calls", static_cast<double>(vh.spr_calls), 0);
        json_append_double(out, "vh_spr_camera", static_cast<double>(vh.spr_camera), 0);
        json_append_double(out, "vh_spr_overridden", static_cast<double>(vh.spr_overridden), 0);
        json_append_double(out, "ws_still_frames", static_cast<double>(vh.still_frames), 0);
        // VIEW BOB AS A MEASURED ENVIRONMENT FACT, not something to force off.
        //
        // The camera's two pose generations differ EXACTLY when bob is active -- established by a controlled
        // A/B: with bob on, 2 of 47 same-phase samples matched; with it off, 46 of 46. So this is a free and
        // exact bob detector, and checks affected by bob can be mode-aware instead of the suite dictating a
        // graphics setting. Forcing it off would mean only ever testing a mode no player uses, and bob-on is
        // what exposed four real defects.
        // THE CLOCK'S ADDRESS, published so a watchpoint can be pointed at it. This is the workflow AGENT.MD
        // prescribes for "what updates X": publish the address, trap the store, then read the writer in IDA.
        if (const auto ck = sdk::Engine::client_time_addresses()) {
            char tmp[32];
            snprintf(tmp, sizeof(tmp), "0x%08" PRIXPTR, ck->owner);
            json_append_string(out, "eng_clock_owner", tmp);
            snprintf(tmp, sizeof(tmp), "0x%08" PRIXPTR, ck->seconds);
            json_append_string(out, "eng_clock_seconds_at", tmp);
            snprintf(tmp, sizeof(tmp), "0x%08" PRIXPTR, ck->milliseconds);
            json_append_string(out, "eng_clock_ms_at", tmp);
        }
        // THE FRAME BOUNDARY, published so a consumer (and the fixture) can see where a stereo submit would
        // attach without recomputing slot arithmetic. Addresses only -- nothing is hooked here.
        {
            char tmp[32];
            const auto pres = sdk::Render::present_fn();
            const auto rst = sdk::Render::reset_fn();
            const auto bs = sdk::Render::begin_scene_fn();
            const auto es = sdk::Render::end_scene_fn();
            json_append_bool(out, "rnd_device_present", sdk::Render::device() != nullptr);
            if (pres && rst && bs && es) {
                snprintf(tmp, sizeof(tmp), "0x%08" PRIXPTR, *pres);
                json_append_string(out, "rnd_present_fn", tmp);
                snprintf(tmp, sizeof(tmp), "0x%08" PRIXPTR, *rst);
                json_append_string(out, "rnd_reset_fn", tmp);
                snprintf(tmp, sizeof(tmp), "0x%08" PRIXPTR, *bs);
                json_append_string(out, "rnd_begin_scene_fn", tmp);
                snprintf(tmp, sizeof(tmp), "0x%08" PRIXPTR, *es);
                json_append_string(out, "rnd_end_scene_fn", tmp);
                // Four DISTINCT entries. A vtable read landing on the wrong base, or a table of stubs, shows
                // up here as duplicates -- which no correct COM vtable produces.
                const bool distinct = *pres != *rst && *pres != *bs && *pres != *es &&
                                      *rst != *bs && *rst != *es && *bs != *es;
                json_append_bool(out, "rnd_frame_slots_distinct", distinct);
            }
            // THE ENGINE-SIDE frame boundary, which is the hook target a stereo path wants in preference to
            // the COM vtable. Static addresses so they can be pasted into the exe's IDB directly.
            const uintptr_t eng_present = sdk::Render::engine_present_fn();
            const uintptr_t swap = sdk::Render::renderer_swap_buffers_fn();
            const uintptr_t fence = sdk::Render::gpu_fence_wait_fn();
            json_append_bool(out, "rnd_engine_present_ok", eng_present != 0);
            json_append_bool(out, "rnd_swap_buffers_ok", swap != 0);
            json_append_bool(out, "rnd_gpu_fence_ok", fence != 0);
            if (eng_present != 0) {
                snprintf(tmp, sizeof(tmp), "0x%08" PRIXPTR, eng_present);
                json_append_string(out, "rnd_engine_present_fn", tmp);
            }
            if (swap != 0) {
                snprintf(tmp, sizeof(tmp), "0x%08" PRIXPTR, swap);
                json_append_string(out, "rnd_swap_buffers_fn", tmp);
            }
            if (fence != 0) {
                snprintf(tmp, sizeof(tmp), "0x%08" PRIXPTR, fence);
                json_append_string(out, "rnd_gpu_fence_fn", tmp);
            }
            // ALL THREE INSIDE THE EXE, and distinct. A pattern that matched the wrong place, or a decoded
            // jump landing outside the image, shows up here rather than as a mystery crash at hook time.
            if (const auto* exe = sdk::Modules::get().exe()) {
                const auto in_exe = [&](uintptr_t a) {
                    return a != 0 && a >= exe->base && a < exe->base + exe->size;
                };
                json_append_bool(out, "rnd_frame_fns_in_exe",
                                 in_exe(eng_present) && in_exe(swap) && in_exe(fence));
                json_append_bool(out, "rnd_frame_fns_distinct",
                                 eng_present != swap && swap != fence && eng_present != fence);
            }
            {
                const auto si = SyntheticInput::get().state();
                json_append_bool(out, "si_keyboard_resolved", si.keyboard_resolved);
                json_append_double(out, "si_writes", static_cast<double>(si.writes), 0);
                json_append_double(out, "si_taps_completed", static_cast<double>(si.taps_completed), 0);
                json_append_double(out, "si_active_taps", static_cast<double>(si.active_taps), 0);
                json_append_double(out, "si_held_keys", static_cast<double>(si.held_keys), 0);
                json_append_bool(out, "si_poll_hooked", sdk::Input::poll_fn() != 0);
            }
            {
                const auto cp = CameraPassHook::get().observed();
                json_append_bool(out, "cp_hooked", cp.hooked);
                json_append_double(out, "cp_passes", static_cast<double>(cp.passes), 0);
                json_append_double(out, "cp_overridden", static_cast<double>(cp.overridden), 0);
                json_append_double(out, "cp_rejected", static_cast<double>(cp.rejected), 0);
                json_append_bool(out, "cp_stereo", cp.stereo);
                json_append_bool(out, "cp_main_view_only", cp.main_view_only);
                json_append_double(out, "cp_skipped_aux", static_cast<double>(cp.skipped_aux), 0);
                json_append_double(out, "cp_centre_x", cp.frustum_centre[0], 5);
                json_append_double(out, "cp_centre_y", cp.frustum_centre[1], 5);
                json_append_double(out, "cp_centre_applied_x", cp.centre_applied[0], 5);
                json_append_double(out, "cp_centre_applied_y", cp.centre_applied[1], 5);
                json_append_double(out, "cp_rebuilds", static_cast<double>(cp.rebuilds), 0);
                // THE RECORD CHECKED AGAINST ITSELF: the projection's shear terms are determined by the
                // centre and the scale already in the same matrix, so this fails if a centre was written
                // without the rebuild reaching the projection.
                json_append_double(out, "cp_centre_checked", static_cast<double>(cp.centre_checked), 0);
                json_append_double(out, "cp_centre_inconsistent",
                                   static_cast<double>(cp.centre_inconsistent), 0);
                json_append_double(out, "cp_target_w", static_cast<double>(cp.target_size[0]), 0);
                json_append_double(out, "cp_target_h", static_cast<double>(cp.target_size[1]), 0);
                // THE PER-FRAME CENSUS. "One pass per frame" is not safe to assume, and this is the
                // evidence either way -- delimited by the engine's own frame boundary.
                json_append_double(out, "cp_passes_last_frame",
                                   static_cast<double>(CameraPassHook::get().passes_in_last_frame()), 0);
                json_append_double(out, "cp_max_passes_frame",
                                   static_cast<double>(CameraPassHook::get().max_passes_in_a_frame()), 0);
                {
                    std::string arr = "[";
                    size_t n = 0;
                    for (const auto& p : CameraPassHook::get().passes_last_frame()) {
                        if (n++ != 0) { arr += ','; }
                        std::string one;
                        {
                            JsonFields jf(one);
                            jf.f("fov_x", p.fov[0], 6).f("fov_y", p.fov[1], 6)
                              .f("rect_l", p.rect[0], 4).f("rect_r", p.rect[2], 4)
                              .i("vp_l", p.viewport[0]).i("vp_r", p.viewport[2])
                              .i("vp_t", p.viewport[1]).i("vp_b", p.viewport[3])
                              .f("depth_min", p.depth_min, 3).f("depth_max", p.depth_max, 1)
                              .f("cam_x", p.camera_position[0], 2)
                              .f("cam_y", p.camera_position[1], 2)
                              .f("cam_z", p.camera_position[2], 2);
                        }
                        arr += one;
                    }
                    arr += ']';
                    json_append_raw(out, "cp_frame_passes", arr.c_str());
                }
                json_append_double(out, "cp_vp_l", static_cast<double>(cp.viewport[0]), 0);
                json_append_double(out, "cp_vp_t", static_cast<double>(cp.viewport[1]), 0);
                json_append_double(out, "cp_vp_r", static_cast<double>(cp.viewport[2]), 0);
                json_append_double(out, "cp_vp_b", static_cast<double>(cp.viewport[3]), 0);
                json_append_double(out, "cp_draw_calls", static_cast<double>(cp.draw_calls), 0);
                json_append_double(out, "cp_second_eye_draws",
                                   static_cast<double>(cp.second_eye_draws), 0);
                json_append_double(out, "cp_eye", static_cast<double>(static_cast<int>(cp.eye)), 0);
                json_append_double(out, "cp_half_ipd", cp.half_ipd, 4);
                json_append_bool(out, "cp_split_viewport", cp.split_viewport);
                json_append_double(out, "cp_fov_x", cp.fov[0], 6);
                json_append_double(out, "cp_fov_y", cp.fov[1], 6);
                json_append_double(out, "cp_rect_l", cp.rect[0], 4);
                json_append_double(out, "cp_rect_t", cp.rect[1], 4);
                json_append_double(out, "cp_rect_r", cp.rect[2], 4);
                json_append_double(out, "cp_rect_b", cp.rect[3], 4);
                json_append_double(out, "cp_depth_min", cp.depth_min, 4);
                json_append_double(out, "cp_depth_max", cp.depth_max, 4);
                json_append_double(out, "cp_cam_x", cp.camera_position[0], 3);
                json_append_double(out, "cp_cam_y", cp.camera_position[1], 3);
                json_append_double(out, "cp_cam_z", cp.camera_position[2], 3);
                // THE ROTATION THE RENDERER IS ACTUALLY GIVEN. The camera OBJECT's rotation is a different
                // thing and this project has already been misled by conflating them once; this is the
                // transform the pass is set up with, captured as the argument.
                const auto hp = HudPassHook::get().observed();
                json_append_bool(out, "hud_hooked", hp.hooked);
                json_append_double(out, "hud_passes", static_cast<double>(hp.passes), 0);
                json_append_double(out, "hud_passes_last_frame", static_cast<double>(hp.passes_last_frame), 0);
                json_append_bool(out, "hud_ortho", hp.ortho);
                json_append_bool(out, "hud_record_read", hp.record_read);
                json_append_double(out, "hud_vp_w", static_cast<double>(hp.viewport[2]), 0);
                json_append_double(out, "hud_vp_h", static_cast<double>(hp.viewport[3]), 0);
                json_append_double(out, "hud_rect_r", hp.rect[2], 4);
                const auto po = sdk::SceneCamera::pass_offset();
                const auto pos = sdk::SceneCamera::pass_offset_stored();
                const auto poe = sdk::SceneCamera::pass_offset_enabled();
                json_append_bool(out, "hud_inphase_read", hp.offset_read);
                json_append_bool(out, "hud_inphase_gate", hp.offset_gate);
                json_append_double(out, "hud_inphase_eff_x", static_cast<double>(hp.offset_effective[0]), 0);
                json_append_double(out, "hud_inphase_stored_x", static_cast<double>(hp.offset_stored[0]), 0);
                json_append_bool(out, "hud_offset_read", po.has_value());
                json_append_bool(out, "hud_offset_gate", poe.value_or(false));
                json_append_bool(out, "hud_offset_gate_read", poe.has_value());
                if (po) {
                    json_append_double(out, "hud_offset_x", static_cast<double>((*po)[0]), 0);
                    json_append_double(out, "hud_offset_y", static_cast<double>((*po)[1]), 0);
                }
                if (pos) {
                    json_append_double(out, "hud_offset_stored_x", static_cast<double>((*pos)[0]), 0);
                    json_append_double(out, "hud_offset_stored_y", static_cast<double>((*pos)[1]), 0);
                }
                json_append_bool(out, "hud_stored_hooked", hp.stored_hooked);
                json_append_double(out, "hud_stored_passes", static_cast<double>(hp.stored_passes), 0);
                json_append_double(out, "hud_stored_last_frame", static_cast<double>(hp.stored_last_frame), 0);
                json_append_bool(out, "hud_stored_ortho", hp.stored_ortho);
                json_append_double(out, "hud_stored_vp_w", static_cast<double>(hp.stored_viewport[2]), 0);
                json_append_double(out, "cp_cam_qx", cp.camera_rotation[0], 5);
                json_append_double(out, "cp_cam_qy", cp.camera_rotation[1], 5);
                json_append_double(out, "cp_cam_qz", cp.camera_rotation[2], 5);
                json_append_double(out, "cp_cam_qw", cp.camera_rotation[3], 5);
                // THE CROSS-CHECK THAT MATTERS: the FOV we captured as an ARGUMENT, pushed through the
                // engine's own clamp-and-tan, must equal the half view-plane the record ends up holding.
                // Two independent routes -- an intercepted call and a mapped field -- to one pair.
                if (const auto pred = sdk::SceneCamera::predicted_half_view_plane(cp.fov[0], cp.fov[1])) {
                    json_append_double(out, "cp_pred_half_x", (*pred)[0], 6);
                    json_append_double(out, "cp_pred_half_y", (*pred)[1], 6);
                }
            }
            {
                const auto rh = RenderHook::get().stats();
                json_append_bool(out, "rh_hooked", rh.hooked);
                json_append_double(out, "rh_frames", static_cast<double>(rh.frames), 0);
                json_append_double(out, "rh_mean_interval_ms", rh.mean_interval_ms, 3);
                json_append_double(out, "rh_samples", static_cast<double>(rh.samples), 0);
                json_append_double(out, "rh_state_at_present",
                                   static_cast<double>(rh.state_at_present), 0);
                json_append_double(out, "rh_state_not_one", static_cast<double>(rh.state_not_one), 0);
                json_append_double(out, "rh_callbacks", static_cast<double>(rh.callbacks), 0);
            }
            if (const auto owner = sdk::Render::present_impl_owner()) {
                json_append_string(out, "rnd_present_owner", owner->c_str());
            }
        }
        if (const auto cs = sdk::Engine::clock_state()) {
            json_append_bool(out, "eng_clock_paused", cs->paused);
            json_append_bool(out, "eng_clock_advancing", cs->advancing());
            json_append_double(out, "eng_clock_scale", cs->scale(), 4);
            json_append_double(out, "eng_clock_min_step_ms", static_cast<double>(cs->min_step_ms), 0);
            json_append_double(out, "eng_clock_max_step_ms", static_cast<double>(cs->max_step_ms), 0);
            json_append_double(out, "eng_clock_last_step_ms", static_cast<double>(cs->last_step_ms), 0);
            json_append_double(out, "eng_clock_ms", static_cast<double>(cs->milliseconds), 0);
            json_append_double(out, "eng_clock_seconds", cs->seconds, 3);
        }
        json_append_bool(out, "ws_bob_active",
                         sdk::PlayerMgr::pose_generations_differ(0).value_or(false));
        {
            const auto fk = FocusKeeper::get().state();
            json_append_bool(out, "fk_enabled", fk.enabled);
            json_append_bool(out, "fk_hook_installed", fk.hook_installed);
            json_append_bool(out, "fk_window_active", fk.window_active);
            json_append_bool(out, "fk_lost_focus", fk.lost_focus);
            json_append_double(out, "fk_pause_requests", static_cast<double>(fk.pause_requests), 0);
            json_append_double(out, "fk_suppressed", static_cast<double>(fk.suppressed), 0);
            json_append_double(out, "fk_passed_through", static_cast<double>(fk.passed_through), 0);
            // THE REST OF THE FOCUS STATE, because holding the client-active flag proved NECESSARY BUT NOT
            // SUFFICIENT: 2473 re-asserts over 18 seconds with the flag never observed cleared, and the engine
            // clock did not advance by a single millisecond. Something upstream of that flag also stops, and our
            // own hook fell from ~300 to ~137 calls/second, which is the signature of a Sleep on the pump rather
            // than a branch being skipped.
            //
            // So the other latches get reported: whichever of these is set while unfocused is the next candidate.
            if (const auto fs = sdk::Input::focus()) {
                json_append_bool(out, "fk_lost_focus", fs->lost_focus);
                json_append_bool(out, "fk_minimized", fs->minimized);
                json_append_bool(out, "fk_renderer_shutdown", fs->renderer_shutdown);
                json_append_bool(out, "fk_render_initted", fs->render_initted);
            }
        }
        // ---- IS THERE A WORLD AND A PLAYER TO TEST AGAINST? -------------------------------------
        //
        // At the main menu 145 checks FAIL rather than reporting themselves unexercised: sectors, planes,
        // region and box queries, the client shell and the spatial index all assume a loaded world and a live
        // player. That is one missing gate counted 145 times, and it is the same defect class as the
        // stationary-world assumptions -- a legitimate engine state read as a failure.
        //
        // Reported as two independent signals plus the conjunction, so a run can say WHICH half is missing.
        // gameserver.dll being absent at a menu is already documented as expected for the same reason.
        append_world_state(out);
        json_append_double(out, "vh_ov_body_drift_deg",
                           static_cast<double>(vh.override_body_drift_deg), 4);
        // THE RENDER CHAIN'S OWN ADDRESSES, so a data breakpoint can find the writer that actually feeds the
        // renderer. Scanning for the offset is what failed here before: 67 functions share it.
        if (const auto pp = sdk::PlayerMgr::player(0); pp.has_value()) {
            json_append_double(out, "vh_addr_holder", static_cast<double>(pp->holder), 0);
            json_append_double(out, "vh_addr_camera_object", static_cast<double>(pp->camera_object), 0);
        }
        json_append_double(out, "vh_ov_applied_drift_deg",
                           static_cast<double>(vh.override_applied_drift_deg), 4);
        json_append_double(out, "vh_ov_object_drift_deg",
                           static_cast<double>(vh.override_object_drift_deg), 4);
        // THE REFUSAL PATH, checkable without touching the view. write_view_rotation must reject a non-unit
        // quaternion, because read_pose treats non-unit as proof of a wrong offset -- a writer that could
        // manufacture that state would be able to fake a mapping error into existence.
        json_append_bool(out, "vh_rejects_non_unit",
                         !sdk::PlayerMgr::write_view_rotation(0, {0.0f, 0.0f, 0.0f, 0.0f}) &&
                             !sdk::PlayerMgr::write_view_rotation(0, {2.0f, 0.0f, 0.0f, 0.0f}) &&
                             !sdk::PlayerMgr::write_applied_rotation(0, {0.0f, 0.0f, 0.0f, 0.0f}));
        json_append_bool(out, "vh_rejects_out_of_range",
                         !sdk::PlayerMgr::write_view_rotation(9, {0.0f, 0.0f, 0.0f, 1.0f}) &&
                             !sdk::PlayerMgr::view_rotation(9).has_value());
        // BOTH GENERATIONS, so an override can see whether the engine DERIVED the applied pose from the source
        // it was handed. carried==0 while writing +324 is expected -- the rebuild goes through euler and a pitch
        // clamp, so the result is transformed, not copied -- and this is how that gets checked rather than
        // assumed.
        if (const auto vr = sdk::PlayerMgr::view_rotation(0)) {
            json_append_double(out, "vh_src_y", static_cast<double>((*vr)[1]), 5);
            json_append_double(out, "vh_src_w", static_cast<double>((*vr)[3]), 5);
        }
        if (const auto ar = sdk::PlayerMgr::applied_rotation(0)) {
            json_append_double(out, "vh_applied_y", static_cast<double>((*ar)[1]), 5);
            json_append_double(out, "vh_applied_w", static_cast<double>((*ar)[3]), 5);
        }
        json_append_double(out, "vh_pose_agree_equal", static_cast<double>(vh.pose_agree_equal), 0);
        json_append_double(out, "vh_pose_agree_differ", static_cast<double>(vh.pose_agree_differ), 0);
        json_append_double(out, "vh_pose_agree_other", static_cast<double>(vh.pose_agree_other), 0);
        json_append_double(out, "vh_pose_target_offset",
                           static_cast<double>((gc != nullptr && gc->base != 0 && vh.pose_target >= gc->base)
                                                   ? vh.pose_target - gc->base : 0),
                           0);
        json_append_double(out, "vh_target_offset",
                           static_cast<double>((gc != nullptr && gc->base != 0 && vh.target >= gc->base)
                                                   ? vh.target - gc->base : 0),
                           0);
    }

    // ---- MUTATION PROBES, OPT-IN ONLY -----------------------------------------------------
    // These WRITE engine state and restore it, and a player reported seeing the camera snap away for a single
    // frame on every injection. That is this code: it writes a 90-degree yaw into the camera rotation, samples
    // for a few frames to see whether the engine reclaims it, then restores. The original comment called that
    // "harmless for the frames it lasts" -- it is VISIBLE, and it ran on every single read of this endpoint, so
    // a 171-sample coverage run perturbed the view 171 times.
    //
    // So they are gated. Nothing that merely observes state may mutate it; /sdk/write-probe opts in explicitly.
    // They are kept rather than deleted because they produced real findings: the outer operand's writer runs
    // every frame and reclaims anything written there, which is why a VR override cannot steer the view from it.
    //
    // Still ordered last within the probe set: an earlier version placed them first and the camera-object probe
    // perturbed the object a later read-only check compares the applied pose against, failing a correct
    // assertion. A verification probe must not mutate state other assertions read.
    // IS AN ATTACHMENT STEERING THE VIEW right now? Non-identity outer operand means yes. READ-ONLY, so it
    // stays on the observing path -- only the writing probes below are gated.
    const auto cro_att = sdk::PlayerMgr::camera_attachment_driving(0);
    json_append_bool(out, "cro_attachment_determinable", cro_att.has_value());
    json_append_bool(out, "cro_attachment_driving", cro_att.has_value() && *cro_att);
    // The render-path liveness signal, also read-only, and needed to interpret anything else about the pipeline.
    const auto fa = sdk::ShaderParams::frames_advanced();
    json_append_bool(out, "fa_available", fa.has_value());
    json_append_double(out, "fa_distinct_frames", static_cast<double>(fa.value_or(9999)), 0);
    if (include_write_probes) {
    // DOES A WRITTEN VALUE SURVIVE? The operand's only writer runs every frame and rewrites it unconditionally,
    // so a consumer cannot steer the view by writing here. Measured rather than asserted: write a 90-degree yaw,
    // sample until it disappears, restore. 0 samples survived means it was reclaimed before the first sample.
    const auto cro_pr = sdk::PlayerMgr::probe_outer_operand(0);
    json_append_bool(out, "cro_probe_ran", cro_pr.has_value());
    json_append_double(out, "cro_probe_survived", static_cast<double>(cro_pr.has_value() ? cro_pr->survived : 9999), 0);
    json_append_double(out, "cro_probe_samples", static_cast<double>(cro_pr.has_value() ? cro_pr->samples : 0), 0);
    json_append_bool(out, "cro_probe_view_followed", cro_pr.has_value() && cro_pr->followed);
    json_append_double(out, "cro_probe_frames",
                       static_cast<double>(cro_pr.has_value() ? cro_pr->frames_observed : 0), 0);
    json_append_double(out, "cro_probe_verdict",
                       static_cast<double>(cro_pr.has_value() ? static_cast<int>(cro_pr->verdict) : -1), 0);
    // THE INNER OPERAND, probed the same way. It is what the camera object's rotation currently equals, so if any
    // holder field steers the view in this state it should be this one. `false` for expect_composed means the
    // camera rotation is compared against the written value directly, since the outer operand is identity.
    const auto cro_pi = sdk::PlayerMgr::probe_holder_quaternion(0, sdk::PlayerMgr::kCameraRotationInner, false);
    json_append_bool(out, "cro_inner_probe_ran", cro_pi.has_value());
    json_append_double(out, "cro_inner_probe_survived",
                       static_cast<double>(cro_pi.has_value() ? cro_pi->survived : 9999), 0);
    json_append_bool(out, "cro_inner_probe_view_followed", cro_pi.has_value() && cro_pi->followed);
    // THE CAMERA OBJECT ITSELF. The engine renders from this, so survival here is what a VR override needs.
    const auto cro_pc = sdk::PlayerMgr::probe_camera_object_rotation(0);
    json_append_bool(out, "cro_object_probe_ran", cro_pc.has_value());
    json_append_double(out, "cro_object_probe_survived",
                       static_cast<double>(cro_pc.has_value() ? cro_pc->survived : 9999), 0);
    json_append_double(out, "cro_object_probe_samples",
                       static_cast<double>(cro_pc.has_value() ? cro_pc->samples : 0), 0);
    json_append_double(out, "cro_object_probe_frames",
                       static_cast<double>(cro_pc.has_value() ? cro_pc->frames_observed : 0), 0);
    json_append_double(out, "cro_object_probe_verdict",
                       static_cast<double>(cro_pc.has_value() ? static_cast<int>(cro_pc->verdict) : -1), 0);
    // After the probe the operand must be back to a unit quaternion -- the restore has to leave it usable.
    if (const auto after = sdk::PlayerMgr::camera_rotation_operands(0); after.has_value()) {
        const auto n = std::sqrt(after->outer[0] * after->outer[0] + after->outer[1] * after->outer[1] +
                                 after->outer[2] * after->outer[2] + after->outer[3] * after->outer[3]);
        json_append_bool(out, "cro_probe_left_unit", std::fabs(n - 1.0f) <= 0.01f);
    }
    }

    // THE CAMERA'S CACHED TUNABLES. The claim under test is the GRID ORDERING, and the check is that a name
    // COMPOSED from (channel, axis, parameter) resolves through the console tables to the very record cached at
    // the slot the grid formula computes. A wrong channel or axis order still composes 60 valid names, so name
    // validity alone proves nothing -- only the record identity does.
    const auto cam_cache = sdk::Engine::camera_tunable_cache();
    size_t tv_populated = 0, tv_distinct = 0, tv_same_owner = 0;
    std::vector<uintptr_t> recs;
    uintptr_t first_owner = 0;
    for (const auto& v : cam_cache) {
        if (v.record != 0) {
            ++tv_populated;
            recs.push_back(v.record);
        }
        if (first_owner == 0) { first_owner = v.owner; }
        if (v.owner != 0 && v.owner == first_owner) { ++tv_same_owner; }
    }
    std::sort(recs.begin(), recs.end());
    tv_distinct = static_cast<size_t>(std::unique(recs.begin(), recs.end()) - recs.begin());
    const auto [tv_agree, tv_pop2] = sdk::Engine::camera_tunable_agreement();
    json_append_double(out, "tv_total", static_cast<double>(cam_cache.size()), 0);
    json_append_double(out, "tv_populated", static_cast<double>(tv_populated), 0);
    json_append_double(out, "tv_distinct_records", static_cast<double>(tv_distinct), 0);
    json_append_double(out, "tv_same_owner", static_cast<double>(tv_same_owner), 0);
    json_append_double(out, "tv_agree", static_cast<double>(tv_agree), 0);
    json_append_double(out, "tv_agree_of", static_cast<double>(tv_pop2), 0);
    // The owner must be the engine's ILTClient, i.e. inside the executable, not gameclient.
    const auto* exe_t = sdk::Modules::get().exe();
    json_append_bool(out, "tv_owner_in_exe",
                     exe_t != nullptr && exe_t->base != 0 && first_owner >= exe_t->base &&
                         first_owner < exe_t->base + exe_t->size);
    // A named grid cell must equal the same cell reached by name lookup.
    const auto cell = sdk::Engine::head_bob_var(sdk::Engine::BobChannel::CameraRotation, 1,
                                                sdk::Engine::BobParam::Amp);
    const auto by_name = sdk::Engine::camera_tunable("HeadBobCameraRotationYAmp");
    json_append_bool(out, "tv_grid_matches_name",
                     cell.has_value() && by_name.has_value() && cell->record == by_name->record &&
                         cell->cache_offset == by_name->cache_offset && cell->record != 0);
    json_append_bool(out, "tv_name_composed",
                     sdk::Engine::head_bob_var_name(sdk::Engine::BobChannel::WeaponRotation, 2,
                                                    sdk::Engine::BobParam::AmpOffset) ==
                         "HeadBobWeaponRotationZAmpOffset");
    // Out-of-range coordinates are refused rather than clamped into a neighbouring cell.
    json_append_bool(out, "tv_range_refused",
                     !sdk::Engine::head_bob_var(sdk::Engine::BobChannel::CameraOffset, 3,
                                                sdk::Engine::BobParam::Amp).has_value() &&
                         sdk::Engine::head_bob_var_name(sdk::Engine::BobChannel::CameraOffset, 9,
                                                        sdk::Engine::BobParam::Amp).empty() &&
                         !sdk::Engine::camera_tunable("NoSuchTunableAtAll").has_value());
    // A round-trip through the cached record, restored afterwards -- this is the write path a VR comfort layer
    // would use, so it is exercised rather than merely described.
    bool tv_round = false;
    if (const auto amp = sdk::Engine::head_bob_var(sdk::Engine::BobChannel::CameraOffset, 2,
                                                   sdk::Engine::BobParam::Amp);
        amp.has_value() && amp->record != 0) {
        const auto before = sdk::Engine::read_cached(*amp);
        if (before.has_value() && sdk::Engine::write_cached(*amp, 0.25f)) {
            const auto mid = sdk::Engine::read_cached(*amp);
            sdk::Engine::write_cached(*amp, *before);
            const auto after = sdk::Engine::read_cached(*amp);
            tv_round = mid.has_value() && *mid == 0.25f && after.has_value() && *after == *before;
        }
    }
    json_append_bool(out, "tv_write_round_trip", tv_round);

    // THE LIVE GFx MOVIE. This is the object the whole UI catalogue was missing, so the checks are about whether
    // it is REACHABLE and USABLE, not about its contents: a movie whose SetVariable/SetVariableArray/Invoke slots
    // do not resolve into the executable is not something a consumer should call through.
    const auto mode = sdk::Events::ui_mode();
    json_append_double(out, "gfx_mode", static_cast<double>(static_cast<uint32_t>(mode)), 0);
    const auto movie = sdk::Events::active_movie();
    json_append_bool(out, "gfx_resolved", movie.has_value());
    if (movie.has_value()) {
        json_append_double(out, "gfx_slot", static_cast<double>(movie->slot), 0);
        json_append_bool(out, "gfx_usable", sdk::Events::movie_usable(*movie));
        const auto* gcm = sdk::Modules::get().game_client();
        const bool vt_outside_gc = gcm == nullptr || gcm->base == 0 ||
                                   movie->vtable < gcm->base || movie->vtable >= gcm->base + gcm->size;
        json_append_bool(out, "gfx_object_outside_gameclient",
                         movie->object != 0 && movie->vtable != 0 && vt_outside_gc);
        // The three slots must be distinct functions; a vtable read at the wrong stride tends to repeat one.
        const auto set_var = sdk::Events::movie_method(*movie, sdk::Events::kGFxSetVariable);
        const auto set_arr = sdk::Events::movie_method(*movie, sdk::Events::kGFxSetVariableArray);
        const auto invoke = sdk::Events::movie_method(*movie, sdk::Events::kGFxInvoke);
        json_append_bool(out, "gfx_slots_distinct",
                         set_var != 0 && set_arr != 0 && invoke != 0 && set_var != set_arr &&
                             set_arr != invoke && set_var != invoke);
        json_append_double(out, "gfx_mode_of_movie", static_cast<double>(static_cast<uint32_t>(movie->mode)), 0);
        // The holder is reachable a second way: reading it directly must produce the same movie.
        const auto again = sdk::Events::read_movie_holder(movie->holder, movie->mode);
        json_append_bool(out, "gfx_holder_reread_agrees",
                         again.has_value() && again->object == movie->object && again->slot == movie->slot);

        // THE TWO LAYERS. The wrapper holds the Scaleform movie at +4, and the inner interface is a different,
        // larger vtable -- so the inner methods must resolve and must NOT be the wrapper's own.
        const auto* exem = sdk::Modules::get().exe();
        const auto in_exe = [exem](uintptr_t p) {
            return exem != nullptr && exem->base != 0 && p >= exem->base && p < exem->base + exem->size;
        };
        const auto i_set = sdk::Events::inner_method(*movie, sdk::Events::kGFxInnerSetVariable);
        const auto i_arr = sdk::Events::inner_method(*movie, sdk::Events::kGFxInnerSetVariableArray);
        const auto i_inv = sdk::Events::inner_method(*movie, sdk::Events::kGFxInnerInvoke);
        json_append_bool(out, "gfx_inner_present",
                         movie->inner != 0 && movie->inner_vtable != 0 &&
                             movie->inner != movie->object && movie->inner_vtable != movie->vtable);
        json_append_bool(out, "gfx_inner_methods_resolve",
                         in_exe(i_set) && in_exe(i_arr) && in_exe(i_inv) && i_set != i_arr && i_arr != i_inv);
        json_append_bool(out, "gfx_inner_distinct_from_wrapper",
                         i_set != sdk::Events::movie_method(*movie, sdk::Events::kGFxSetVariable) &&
                             i_inv != sdk::Events::movie_method(*movie, sdk::Events::kGFxInvoke));

        // THE STATE FLAG EARNS ITS KEEP. Count slots whose object field is non-null versus slots that are
        // actually usable: live those numbers differ, which is the whole argument for checking state.
        const auto slots = sdk::Events::movie_slots(movie->holder);
        size_t sl_nonnull = 0, sl_live = 0, sl_usable = 0;
        for (const auto& sl : slots) {
            if (sl.object != 0) { ++sl_nonnull; }
            if (sl.state == 1) { ++sl_live; }
            if (sl.usable) { ++sl_usable; }
        }
        json_append_double(out, "gfx_slots_total", static_cast<double>(slots.size()), 0);
        json_append_double(out, "gfx_slots_nonnull", static_cast<double>(sl_nonnull), 0);
        json_append_double(out, "gfx_slots_live", static_cast<double>(sl_live), 0);
        json_append_double(out, "gfx_slots_usable", static_cast<double>(sl_usable), 0);
    }
    // A garbage holder must be refused rather than producing a movie-shaped result.
    json_append_bool(out, "gfx_bad_holder_refused",
                     !sdk::Events::read_movie_holder(0).has_value() &&
                         !sdk::Events::read_movie_holder(0xDEAD0000).has_value());
    sdk::Events::GFxMovie fake{};
    fake.object = 0x1000;
    fake.vtable = 0x1000;
    json_append_bool(out, "gfx_fake_movie_refused",
                     !sdk::Events::movie_usable(fake) && sdk::Events::movie_method(fake, 9) == 0);

    // THE PANEL OBJECTS. The structural invariant is that each object's +0x04 still points at the binding table
    // the panel is recorded with -- and that is a genuine cross-check, because the object addresses came from the
    // static initialisers while the table addresses came from the accessors. Two routes, one answer.
    const auto pobjs = sdk::Events::panel_objects();
    size_t po_consistent = 0, po_vtable = 0, po_path = 0, po_convention = 0, po_distinct_vt = 0;
    std::vector<uintptr_t> vts;
    for (const auto& o : pobjs) {
        if (sdk::Events::panel_object_consistent(o)) { ++po_consistent; }
        if (o.vtable != 0) { ++po_vtable; vts.push_back(o.vtable); }
        if (!o.as_path.empty()) {
            ++po_path;
            // Does the path follow "loki<Panel>Events"? Counted, NOT required: SystemLayer breaks it, which is
            // exactly why invoke_target reads the field instead of composing this string.
            if (o.as_path == std::string("loki") + o.panel + "Events") { ++po_convention; }
        }
    }
    std::sort(vts.begin(), vts.end());
    po_distinct_vt = static_cast<size_t>(std::unique(vts.begin(), vts.end()) - vts.begin());
    json_append_double(out, "po_total", static_cast<double>(pobjs.size()), 0);
    json_append_double(out, "po_consistent", static_cast<double>(po_consistent), 0);
    json_append_double(out, "po_vtable", static_cast<double>(po_vtable), 0);
    json_append_double(out, "po_path", static_cast<double>(po_path), 0);
    json_append_double(out, "po_convention", static_cast<double>(po_convention), 0);
    json_append_double(out, "po_distinct_vtables", static_cast<double>(po_distinct_vt), 0);
    // A panel WITH a path yields a dotted target; one WITHOUT is refused rather than yielding ".Method".
    const auto tgt = sdk::Events::invoke_target("Global", "OnConstruct");
    json_append_bool(out, "po_target_composed", tgt.has_value() && *tgt == "lokiGlobalEvents.OnConstruct");
    json_append_bool(out, "po_pathless_refused",
                     !sdk::Events::invoke_target("ControlPanel", "DoAction").has_value() &&
                         !sdk::Events::invoke_target("Global", "").has_value() &&
                         !sdk::Events::invoke_target("NoSuchPanel", "X").has_value());
    // SystemLayer is the counterexample that decides the API's shape, so it is asserted by name.
    const auto sysl = sdk::Events::find_panel_object("SystemLayer");
    json_append_bool(out, "po_systemlayer_breaks_convention",
                     sysl.has_value() && sysl->as_path == "lokiSystemEvents" &&
                         sysl->as_path != std::string("loki") + sysl->panel + "Events");
    // An object whose table field is wrong must be judged inconsistent -- the check has to be able to fail.
    sdk::Events::PanelObject bogus{};
    bogus.panel = "Global";
    bogus.table = 0x1234;
    json_append_bool(out, "po_inconsistent_detected", !sdk::Events::panel_object_consistent(bogus));

    // THE FLASH GLOBALS, resolved to their setters. The rule under test is that the kind byte predicts the
    // variable's C++ argument shape over the WHOLE population: every name's Hungarian prefix must equal the one
    // its kind denotes. A previous claim that the kind also predicts the GFx TYPE is NOT asserted -- it is false,
    // and the counters below record the mismatch that killed it rather than hiding it.
    const auto globals = sdk::Events::global_variables();
    size_t gv_prefix_ok = 0, gv_scalar = 0, gv_array = 0, gv_handler_ok = 0, gv_slot_ok = 0;
    for (const auto& v : globals) {
        const char* pre = sdk::Events::prefix_for_kind(v.kind);
        if (pre != nullptr) {
            // The name is "_global.<prefix><Rest>"; the prefix must match AND the next character must be upper
            // case, or "g_a" would satisfy "g_an" and the check would pass on a shorter prefix.
            const std::string want = std::string("_global.") + pre;
            if (v.name.rfind(want, 0) == 0 && v.name.size() > want.size() &&
                v.name[want.size()] >= 'A' && v.name[want.size()] <= 'Z') {
                ++gv_prefix_ok;
            }
        }
        if (v.is_array) { ++gv_array; } else { ++gv_scalar; }
        if (v.handler != 0) { ++gv_handler_ok; }
        if (v.gfx_slot == (v.is_array ? 11u : 9u)) { ++gv_slot_ok; }
    }
    json_append_double(out, "gv_total", static_cast<double>(globals.size()), 0);
    json_append_double(out, "gv_prefix_ok", static_cast<double>(gv_prefix_ok), 0);
    json_append_double(out, "gv_scalar", static_cast<double>(gv_scalar), 0);
    json_append_double(out, "gv_array", static_cast<double>(gv_array), 0);
    json_append_double(out, "gv_handler_ok", static_cast<double>(gv_handler_ok), 0);
    json_append_double(out, "gv_slot_ok", static_cast<double>(gv_slot_ok), 0);
    json_append_double(out, "gv_slot_scalar", static_cast<double>(sdk::Events::gfx_slot_for_kind(15)), 0);
    json_append_double(out, "gv_slot_array", static_cast<double>(sdk::Events::gfx_slot_for_kind(20)), 0);
    // Outside the observed kind range there is no slot and no prefix -- refused rather than guessed.
    json_append_bool(out, "gv_unknown_kind_refused",
                     sdk::Events::gfx_slot_for_kind(11) == 0 && sdk::Events::gfx_slot_for_kind(22) == 0 &&
                         sdk::Events::prefix_for_kind(11) == nullptr &&
                         sdk::Events::prefix_for_kind(22) == nullptr);
    // A named lookup must resolve to a callable setter, and a nonexistent one must not resolve at all.
    const auto host_id = sdk::Events::find_global("_global.g_nMonolithMultiplayerHostID");
    json_append_bool(out, "gv_lookup_resolves",
                     host_id.has_value() && host_id->handler != 0 && host_id->gfx_slot == 9 &&
                         !host_id->is_array && host_id->kind == 15);
    json_append_bool(out, "gv_absent_refused",
                     !sdk::Events::find_global("_global.g_nNoSuchVariableAtAll").has_value() &&
                         !sdk::Events::find_global("g_nMonolithMultiplayerHostID").has_value());
    // The two string kinds are the honest residue: both denote g_s, so the mapping is a function but not
    // injective. A consumer must not invert it.
    json_append_bool(out, "gv_prefix_not_injective",
                     sdk::Events::prefix_for_kind(13) != nullptr && sdk::Events::prefix_for_kind(14) != nullptr &&
                         std::string_view{sdk::Events::prefix_for_kind(13)} ==
                             std::string_view{sdk::Events::prefix_for_kind(14)} &&
                         std::string_view{sdk::Events::prefix_for_kind(18)} ==
                             std::string_view{sdk::Events::prefix_for_kind(19)});
    // ControlPanel's table is small enough to state exactly: 7 entries then a null name.
    json_append_double(out, "bt_controlpanel",
                       static_cast<double>(sdk::Events::panel_bindings("ControlPanel").size()), 0);
    json_append_bool(out, "bt_absent_refused",
                     sdk::Events::panel_bindings("NoSuchPanel").empty() &&
                         !sdk::Events::panel_table_initialised("NoSuchPanel") &&
                         sdk::Events::binding_role_for_kind(200) ==
                             sdk::Events::BindingRole::Unknown);

    // THE HUNGARIAN-PREFIX RULE for Flash globals: every scalar prefix takes the SetVariable slot and every
    // array prefix the SetVariableArray slot, with no exceptions across 162 classified names. Checked on one
    // representative of each observed prefix, using the real names from the binary.
    {
        struct Row { const char* name; size_t slot; char letter; bool is_array; };
        const Row rows[] = {
            {"_global.g_nMonolithGlobalPlatform", sdk::Events::kSetVariableSlot, 'd', false},
            {"_global.g_bMonolithGlobalIsCollectorsEdition", sdk::Events::kSetVariableSlot, 'b', false},
            {"_global.g_sMonolithMenuLBName", sdk::Events::kSetVariableSlot, 's', false},
            {"_global.g_asMonolithMenuCustomLevels", sdk::Events::kSetVariableArraySlot, 's', true},
            {"_global.g_anSomeNumbers", sdk::Events::kSetVariableArraySlot, 'd', true},
            {"_global.g_abSomeFlags", sdk::Events::kSetVariableArraySlot, 'b', true},
            {"_global.g_afSomeFloats", sdk::Events::kSetVariableArraySlot, 'f', true},
        };
        size_t ok = 0;
        for (const auto& r : rows) {
            if (sdk::Events::setter_slot_for_variable(r.name) == r.slot &&
                sdk::Events::type_letter_for_variable(r.name) == r.letter &&
                sdk::Events::variable_is_array(r.name) == r.is_array) {
                ++ok;
            }
        }
        json_append_double(out, "gfx_prefix_rows", static_cast<double>(std::size(rows)), 0);
        json_append_double(out, "gfx_prefix_ok", static_cast<double>(ok), 0);
        // A bare name works as well as a qualified one, and an unrecognised name yields nothing rather than a
        // default slot -- which would silently send a caller to SetVariable for an array.
        json_append_bool(out, "gfx_prefix_bare",
                         sdk::Events::setter_slot_for_variable("g_nThing") ==
                             sdk::Events::kSetVariableSlot);
        json_append_bool(out, "gfx_prefix_refused",
                         sdk::Events::setter_slot_for_variable("_global.notHungarian") == 0 &&
                             sdk::Events::type_letter_for_variable("") == 0 &&
                             sdk::Events::setter_slot_for_variable("g_n") == 0);
        json_append_bool(out, "gfx_slots_distinct",
                         sdk::Events::kSetVariableSlot != sdk::Events::kSetVariableArraySlot &&
                             sdk::Events::kInvokeSlot != sdk::Events::kSetVariableSlot &&
                             sdk::Events::kValueDataOffset == 8);
    }

    // THE UI PANELS: one dispatcher per panel, verified by prefix against the live binary.
    const auto& panels = sdk::Events::ui_panels();
    json_append_double(out, "ui_panels", static_cast<double>(panels.size()), 0);
    json_append_double(out, "ui_panels_verified",
                       static_cast<double>(sdk::Events::verified_panel_count()), 0);
    size_t ui_resolved = 0, ui_methods = 0;
    for (const auto& p : panels) {
        if (sdk::Events::panel_dispatch(p.name) != 0) {
            ++ui_resolved;
        }
        ui_methods += p.method_count;
    }
    json_append_double(out, "ui_panels_resolved", static_cast<double>(ui_resolved), 0);
    json_append_double(out, "ui_method_total", static_cast<double>(ui_methods), 0);
    // The Player panel's dispatcher is the same function that carries the Game_Player_* binding names, so it
    // is the one a consumer hooks to see every player-facing UI call at once.
    const auto player_panel = sdk::Events::find_panel("Player");
    json_append_bool(out, "ui_player_panel",
                     player_panel.has_value() && player_panel->method_count > 20 &&
                         sdk::Events::panel_dispatch("Player") != 0);
    json_append_bool(out, "ui_panel_absent_refused",
                     !sdk::Events::find_panel("NoSuchPanel").has_value() &&
                         sdk::Events::panel_dispatch("NoSuchPanel") == 0 &&
                         !sdk::Events::find_panel("").has_value());

    // THE ACTIONSCRIPT NAMES the sender composes. Its error string spells the interface as
    // "Monolith.I<category>Events", and the invoked method as "<path>.<EventName>" with a "Default" fallback.
    json_append_bool(out, "ev_as_interface",
                     sdk::Events::as_interface_name(sdk::Events::Category::Player) ==
                             "Monolith.IPlayerEvents" &&
                         sdk::Events::as_interface_name(sdk::Events::Category::Menu) ==
                             "Monolith.IMenuEvents");
    json_append_bool(out, "ev_as_method",
                     sdk::Events::as_method_name("HUD.Health", "HealthChanged") ==
                             "HUD.Health.HealthChanged" &&
                         sdk::Events::as_method_name("HUD.Health", "") == "HUD.Health.Default" &&
                         sdk::Events::as_method_name("", "HealthChanged").empty());

    // THE FRAME ARITHMETIC, against the two events whose `add esp, N` is visible in the disassembly. These
    // are the numbers a consumer checks its hook against, and they reconcile only with a float at 8 bytes.
    const auto health = sdk::Events::find("HealthChanged");
    const auto slowmo = sdk::Events::find("SlowMoMaxChanged");
    json_append_double(out, "ev_frame_d",
                       health.has_value()
                           ? static_cast<double>(sdk::Events::frame_bytes(health->payload).value_or(0))
                           : -1.0,
                       0);
    json_append_double(out, "ev_frame_f",
                       slowmo.has_value()
                           ? static_cast<double>(sdk::Events::frame_bytes(slowmo->payload).value_or(0))
                           : -1.0,
                       0);
    // FOUR observed cleanups, three format shapes. The one-tag-per-payload formula reproduced the first two
    // and was wrong by 8 and 12 on the others, which is why all four are here.
    json_append_double(out, "ev_frame_sdd",
                       static_cast<double>(sdk::Events::frame_bytes("sdd").value_or(0)), 0);
    json_append_double(out, "ev_frame_ddf",
                       static_cast<double>(sdk::Events::frame_bytes("ddf").value_or(0)), 0);
    // The alphabet is the marshaller's switch: 'w' is legitimate and was missing.
    json_append_bool(out, "ev_wide_accepted",
                     sdk::Events::payload_is_well_formed("w") &&
                         sdk::Events::tag_for('w') == sdk::Events::kTagWideString &&
                         sdk::Events::value_type_for('w') == 5);
    // Tags and value types, as the marshaller's switch assigns them -- int and bool SHARE a tag but not a type.
    json_append_bool(out, "ev_tags_map",
                     sdk::Events::tag_for('d') == sdk::Events::kTagInt &&
                         sdk::Events::tag_for('b') == sdk::Events::kTagInt &&
                         sdk::Events::tag_for('f') == sdk::Events::kTagFloat &&
                         sdk::Events::tag_for('s') == sdk::Events::kTagString &&
                         sdk::Events::tag_for('x') == 0 &&
                         sdk::Events::value_type_for('b') == 2 &&
                         sdk::Events::value_type_for('d') == 3 &&
                         sdk::Events::value_type_for('f') == 3 &&
                         sdk::Events::value_type_for('s') == 4);
    json_append_double(out, "ev_float_bytes",
                       static_cast<double>(sdk::Events::payload_stack_bytes("f").value_or(0)), 0);
    json_append_double(out, "ev_int_bytes",
                       static_cast<double>(sdk::Events::payload_stack_bytes("d").value_or(0)), 0);
    json_append_bool(out, "ev_malformed_refused",
                     !sdk::Events::payload_is_well_formed("dxf") &&
                         !sdk::Events::payload_stack_bytes("dxf").has_value() &&
                         !sdk::Events::find("NoSuchEventHere").has_value() &&
                         sdk::Events::dispatcher("NoSuchEventHere") == 0);
    // An empty payload is legitimate and must not be confused with a malformed one.
    json_append_bool(out, "ev_empty_payload_ok",
                     sdk::Events::payload_is_well_formed("") &&
                         sdk::Events::payload_arg_count("") == 0 &&
                         sdk::Events::payload_stack_bytes("").value_or(99) == 0);

    // ---- THE QUATERNION PRODUCT, AND WHICH ORDER IT MEANS ----------------------------------------
    //
    // Two non-commuting rotations, so R(a*b) can match R(a)*R(b) or R(b)*R(a) but not both. Whichever it is
    // IS the convention a consumer must use, and getting it backwards produces a plausible-looking result
    // that turns the wrong way.
    {
        regenny::LTRotation qa{};
        qa.x = 0.2f; qa.y = 0.3f; qa.z = -0.1f; qa.w = 0.927362f;  // unit to ~1e-6
        regenny::LTRotation qb{};
        qb.x = -0.4f; qb.y = 0.1f; qb.z = 0.5f; qb.w = 0.761577f;
        regenny::LTRotation qi{};
        qi.w = 1.0f;

        const auto prod = sdk::multiply_rotations(qa, qb);
        const auto ident_r = sdk::multiply_rotations(qa, qi);
        const auto ident_l = sdk::multiply_rotations(qi, qa);
        const auto same = [](const regenny::LTRotation& p, const regenny::LTRotation& q) {
            return std::fabs(p.x - q.x) < 1e-5f && std::fabs(p.y - q.y) < 1e-5f &&
                   std::fabs(p.z - q.z) < 1e-5f && std::fabs(p.w - q.w) < 1e-5f;
        };
        json_append_bool(out, "quat_identity_right", same(ident_r, qa));
        json_append_bool(out, "quat_identity_left", same(ident_l, qa));
        // A product of units stays unit -- the cheapest check that no term is transposed.
        const float n = prod.x * prod.x + prod.y * prod.y + prod.z * prod.z + prod.w * prod.w;
        json_append_bool(out, "quat_product_unit", std::fabs(n - 1.0f) < 1e-4f);

        // Now the order, through matrices. Compare R(a*b) against both 3x3 products.
        const auto ma = sdk::rotation_matrix(qa);
        const auto mb = sdk::rotation_matrix(qb);
        const auto mp = sdk::rotation_matrix(prod);
        if (ma.has_value() && mb.has_value() && mp.has_value()) {
            const auto mul3 = [](const sdk::Matrix34& x, const sdk::Matrix34& y) {
                sdk::Matrix34 r{};
                for (int i = 0; i < 3; ++i) {
                    for (int j = 0; j < 3; ++j) {
                        float acc = 0.0f;
                        for (int k = 0; k < 3; ++k) {
                            acc += x.m[i * 4 + k] * y.m[k * 4 + j];
                        }
                        r.m[i * 4 + j] = acc;
                    }
                }
                return r;
            };
            const auto close = [](const sdk::Matrix34& x, const sdk::Matrix34& y) {
                for (int i = 0; i < 3; ++i) {
                    for (int j = 0; j < 3; ++j) {
                        if (std::fabs(x.m[i * 4 + j] - y.m[i * 4 + j]) > 1e-4f) {
                            return false;
                        }
                    }
                }
                return true;
            };
            json_append_bool(out, "quat_order_ab", close(*mp, mul3(*ma, *mb)));
            json_append_bool(out, "quat_order_ba", close(*mp, mul3(*mb, *ma)));
        }
    }

    // ---- THE CAMERA TUNABLES, AND THE WRITE PATH -------------------------------------------------
    //
    // Every catalogued name must still resolve AND still hold the value the catalogue records. That second
    // half is the part worth having: it turns the catalogue from documentation into something the suite
    // maintains, so a retuned build or a mistyped transcription fails here rather than misleading a consumer.
    const auto& tunables = sdk::Engine::camera_tunables();
    size_t tun_found = 0, tun_default_ok = 0;
    for (const auto& t : tunables) {
        const auto v = sdk::Engine::console_var(t.name);
        if (!v.has_value()) {
            continue;
        }
        ++tun_found;
        if (std::fabs(v->value - t.live_default) < 0.0005f) {
            ++tun_default_ok;
        }
    }
    json_append_double(out, "tun_total", static_cast<double>(tunables.size()), 0);
    json_append_double(out, "tun_found", static_cast<double>(tun_found), 0);
    json_append_double(out, "tun_default_ok", static_cast<double>(tun_default_ok), 0);

    // FovY is the field of view, and its value must be a plausible one rather than merely present.
    const auto fovy = sdk::Engine::console_var("FovY");
    json_append_double(out, "tun_fovy", fovy.has_value() ? fovy->value : -1.0, 3);

    // THE WRITE PATH, round-tripped on a variable with no observable effect: DebugPlayerCamSize only matters
    // while DebugPlayerCam is on, and that is off. Written, read back through a SEPARATE lookup, then
    // restored -- so the check exercises the store rather than a cached copy of it.
    bool tun_write_ok = false, tun_restore_ok = false;
    if (const auto before = sdk::Engine::console_var("DebugPlayerCamSize")) {
        const float original = before->value;
        if (sdk::Engine::write_console_var("DebugPlayerCamSize", original + 0.25f)) {
            if (const auto after = sdk::Engine::console_var("DebugPlayerCamSize")) {
                tun_write_ok = std::fabs(after->value - (original + 0.25f)) < 0.0005f;
            }
        }
        sdk::Engine::write_console_var("DebugPlayerCamSize", original);
        if (const auto back = sdk::Engine::console_var("DebugPlayerCamSize")) {
            tun_restore_ok = std::fabs(back->value - original) < 0.0005f;
        }
    }
    json_append_bool(out, "tun_write_roundtrip", tun_write_ok);
    json_append_bool(out, "tun_write_restored", tun_restore_ok);
    json_append_bool(out, "tun_write_absent_refused",
                     !sdk::Engine::write_console_var("NoSuchTunableHere", 1.0f));

    // TWO REPRESENTATIONS OF ONE VARIABLE, compared by VALUE. sdk::EngineVars derives a variable's typed
    // storage from the engine's built-in DESCRIPTOR table; sdk::Engine::console_var reads the live RUNTIME
    // record out of the 128-bucket table on CClientMgr. Different objects holding one number, so the
    // addresses must differ and the numbers must agree.
    size_t cvar_checked = 0, cvar_value_agree = 0, cvar_string_agree = 0, cvar_addr_differs = 0;
    for (const char* nm : {"ScreenWidth", "ScreenHeight", "UpdateRate"}) {
        const auto viaTable = sdk::EngineVars::find(nm);
        const auto viaRecord = sdk::Engine::console_var(nm);
        if (!viaTable.has_value() || !viaRecord.has_value()) {
            continue;
        }
        ++cvar_checked;
        if (viaTable->address != viaRecord->address) {
            ++cvar_addr_differs;
        }
        if (const auto as_int = sdk::EngineVars::read_int(nm)) {
            if (static_cast<float>(*as_int) == viaRecord->value) {
                ++cvar_value_agree;
            }
        }
        if (!viaRecord->text.empty()) {
            const double parsed = std::strtod(viaRecord->text.c_str(), nullptr);
            if (std::fabs(parsed - static_cast<double>(viaRecord->value)) < 0.001) {
                ++cvar_string_agree;
            }
        }
    }
    json_append_double(out, "cvar_routes_checked", static_cast<double>(cvar_checked), 0);
    json_append_double(out, "cvar_value_agree", static_cast<double>(cvar_value_agree), 0);
    json_append_double(out, "cvar_string_agree", static_cast<double>(cvar_string_agree), 0);
    json_append_double(out, "cvar_addr_differs", static_cast<double>(cvar_addr_differs), 0);

    // The record now carries its own address, which is the write capability. Non-zero and inside the heap
    // rather than the exe's data -- the descriptor's storage is the one that lives in the image.
    bool cvar_addr_usable = false;
    if (const auto sw = sdk::Engine::console_var("ScreenWidth")) {
        cvar_addr_usable = sw->address != 0 && !sdk::Console::address_in_exe(sw->address);
    }
    json_append_bool(out, "cvar_record_address_usable", cvar_addr_usable);

    // THE RUNTIME TABLE IS A SUPERSET of the built-in descriptors: variables created by assignment exist
    // only there. The game registers ApplyWorldOffset that way.
    const bool cvar_rt_record = sdk::Engine::console_var("ApplyWorldOffset").has_value();
    const bool cvar_rt_table = sdk::EngineVars::find("ApplyWorldOffset").has_value();
    json_append_bool(out, "cvar_runtime_via_record", cvar_rt_record);
    json_append_bool(out, "cvar_runtime_in_table", cvar_rt_table);
    // BOTH TABLES, and neither contains the other. The source table is the larger and holds the engine's
    // display settings; CClientMgr's holds the render tunables. A name in one is not necessarily in the
    // other, which is why console_var searches both.
    const auto cvar_mgr = sdk::Engine::console_vars();
    const auto cvar_src = sdk::Engine::console_source_vars();
    json_append_double(out, "cvar_mgr_total", static_cast<double>(cvar_mgr.size()), 0);
    json_append_double(out, "cvar_src_total", static_cast<double>(cvar_src.size()), 0);
    const auto has = [](const std::vector<sdk::Engine::ConVar>& v, const char* nm) {
        for (const auto& e : v) {
            if (_stricmp(e.name.c_str(), nm) == 0) {
                return true;
            }
        }
        return false;
    };
    json_append_bool(out, "cvar_src_has_screenwidth", has(cvar_src, "ScreenWidth"));
    json_append_bool(out, "cvar_mgr_has_screenwidth", has(cvar_mgr, "ScreenWidth"));
    json_append_bool(out, "cvar_mgr_has_hdrblur", has(cvar_mgr, "HDR_Blur"));
    json_append_bool(out, "cvar_src_has_hdrblur", has(cvar_src, "HDR_Blur"));
    // An overlapping name proves the two are distinct objects rather than one table read twice.
    size_t cvar_overlap = 0;
    for (const auto& a : cvar_src) {
        if (has(cvar_mgr, a.name.c_str())) {
            ++cvar_overlap;
        }
    }
    json_append_double(out, "cvar_overlap", static_cast<double>(cvar_overlap), 0);
    const auto cvar_apply = sdk::Engine::console_var("ApplyWorldOffset");
    json_append_double(out, "cvar_apply_world_offset",
                       cvar_apply.has_value() ? cvar_apply->value : -1.0, 3);

    // The game-registered commands a VR mod actually reaches for. None of these are in the engine's table,
    // so finding them proves the walk sees past the static 34.
    size_t con_player_cmds = 0;
    for (const char* want : {"GetPlayerPos", "GetPlayerOrientation", "SetPlayerOrientation"}) {
        if (sdk::Console::find(want).has_value()) {
            ++con_player_cmds;
        }
    }
    json_append_double(out, "console_player_commands", static_cast<double>(con_player_cmds), 0);

    // A handler resolves to a real address in a real module, and the module is NOT the exe for a
    // game-registered command.
    bool con_handler_ok = false;
    bool con_handler_outside_exe = false;
    if (const auto h = sdk::Console::find("GetPlayerPos")) {
        con_handler_ok = h->handler != 0 && !h->module.empty();
        con_handler_outside_exe = !h->from_exe;
    }
    json_append_bool(out, "console_handler_resolved", con_handler_ok);
    json_append_bool(out, "console_handler_outside_exe", con_handler_outside_exe);

    // An engine command resolves INSIDE the exe -- the other side of the same question.
    bool con_engine_in_exe = false;
    if (const auto h = sdk::Console::find("RestartRender")) {
        con_engine_in_exe = h->from_exe && h->handler != 0;
    }
    json_append_bool(out, "console_engine_in_exe", con_engine_in_exe);

    // Case-insensitive lookup finds what exact lookup finds, and an absent name is refused by both.
    json_append_bool(out, "console_ci_finds_exact",
                     sdk::Console::find_insensitive("restartrender").has_value() &&
                         sdk::Console::find("restartrender") == std::nullopt);
    json_append_bool(out, "console_absent_refused",
                     !sdk::Console::find("NoSuchConsoleCommandHere").has_value() &&
                         !sdk::Console::handler_of("NoSuchConsoleCommandHere").has_value());
    json_append_bool(out, "console_null_object_refused",
                     !sdk::Console::read_object(0).has_value() && !sdk::Console::node_is_consistent(0));

    json_append_bool(out, "resources_absent_refused",
                     !sdk::Resources::find("no_such_resource_anywhere.xyz").has_value());
    json_append_bool(out, "resources_null_refused", !sdk::Resources::read(0).has_value());
    // The manager object itself is NOT a record. Reading it must either fail or come back unnamed -- it
    // must never present binary as a resource path, which is the guarantee the name validator exists for.
    const auto res_bogus = sdk::Resources::read(sdk::Resources::manager_address());
    json_append_bool(out, "resources_bogus_unnamed",
                     !res_bogus.has_value() || res_bogus->name.empty());

    // ---- THE THREE COUNTS THE ENGINE NAMED FOR ITSELF -------------------------------------------
    //
    // Physics Nodes, Weight Sets and Child Models come from the LogModels CSV header, which maps its
    // columns onto asset offsets -- five of which landed on fields this SDK had already mapped, which is
    // what makes the ordering trustworthy. The INVARIANT is the useful check: physics nodes are drawn from
    // the skeleton, so their count can never exceed node_count. A wrong column would very likely break it.
    size_t asset_counts_read = 0, physics_le_nodes = 0, weight_sane = 0, child_sane = 0;
    size_t physics_nonzero = 0, weight_nonzero = 0, child_nonzero = 0;
    uint32_t physics_max = 0, weight_max = 0, child_max = 0;
    {
        std::vector<sdk::CClientMgr::ObjectSnapshot> msnaps(2048);
        auto* mgr2 = sdk::CClientMgr::get();
        const auto mtaken = mgr2 == nullptr
                                ? std::optional<size_t>{}
                                : mgr2->snapshot_objects(static_cast<sdk::ObjectType>(1),
                                                         msnaps.data(), msnaps.size());
        if (mtaken.has_value()) {
            for (size_t si = 0; si < *mtaken; ++si) {
                const auto* obj = reinterpret_cast<const regenny::LTObject*>(msnaps[si].address);
                const auto skel = sdk::ModelSkeleton::from_object(obj);
                if (!skel.has_value()) {
                    continue;
                }
                const auto pn = skel->physics_node_count();
                const auto ws = skel->weight_set_count();
                const auto cm = skel->child_model_count();
                if (!pn.has_value() || !ws.has_value() || !cm.has_value()) {
                    continue;
                }
                ++asset_counts_read;
                if (*pn <= skel->node_count()) {
                    ++physics_le_nodes;
                }
                // Sanity bounds rather than values: these are per-asset content, so pinning numbers would
                // encode this scene's models into the suite.
                if (*ws <= 256) {
                    ++weight_sane;
                }
                if (*cm <= 64) {
                    ++child_sane;
                }
                // NON-VACUITY. An invariant like "physics nodes <= nodes" proves nothing if the field is
                // always zero, so the suite needs to see that these counts actually vary.
                if (*pn > 0) {
                    ++physics_nonzero;
                }
                if (*ws > 0) {
                    ++weight_nonzero;
                }
                if (*cm > 0) {
                    ++child_nonzero;
                }
                if (*pn > physics_max) {
                    physics_max = *pn;
                }
                if (*ws > weight_max) {
                    weight_max = *ws;
                }
                if (*cm > child_max) {
                    child_max = *cm;
                }
            }
        }
    }
    json_append_double(out, "asset_counts_read", static_cast<double>(asset_counts_read), 0);
    json_append_double(out, "asset_physics_le_nodes", static_cast<double>(physics_le_nodes), 0);
    json_append_double(out, "asset_weight_sane", static_cast<double>(weight_sane), 0);
    json_append_double(out, "asset_child_sane", static_cast<double>(child_sane), 0);
    json_append_double(out, "asset_physics_nonzero", static_cast<double>(physics_nonzero), 0);
    json_append_double(out, "asset_weight_nonzero", static_cast<double>(weight_nonzero), 0);
    json_append_double(out, "asset_child_nonzero", static_cast<double>(child_nonzero), 0);
    json_append_double(out, "asset_physics_max", static_cast<double>(physics_max), 0);
    json_append_double(out, "asset_weight_max", static_cast<double>(weight_max), 0);
    json_append_double(out, "asset_child_max", static_cast<double>(child_max), 0);

    // ---- THE MODEL TWIN -------------------------------------------------------------------------
    //
    // CLTModelClient has 83 slots, CLTModelServer 81, and they align at offset +0. The two extra client
    // slots are the TAIL (GetMaterial/SetMaterial) -- which is the shape that matters: extras appended
    // rather than inserted means every shared slot index is valid on both sides, and a consumer holding a
    // slot number does not need to know which side it is on.
    const uintptr_t mdl_c = sdk::Vtables::address("CLTModelClient");
    const uintptr_t mdl_s = sdk::Vtables::address("CLTModelServer");
    const auto* mdl_ce = sdk::Vtables::find("CLTModelClient");
    const auto* mdl_se = sdk::Vtables::find("CLTModelServer");
    size_t mdl_shared = 0, mdl_differ = 0;
    if (mdl_c != 0 && mdl_s != 0 && mdl_ce != nullptr && mdl_se != nullptr) {
        const size_t common = mdl_se->slot_count < mdl_ce->slot_count ? mdl_se->slot_count
                                                                     : mdl_ce->slot_count;
        for (size_t i = 0; i < common; ++i) {
            const auto a = sdk::Vtables::vtable_of(mdl_c + i * sizeof(uint32_t));
            const auto b = sdk::Vtables::vtable_of(mdl_s + i * sizeof(uint32_t));
            if (!a.has_value() || !b.has_value()) {
                continue;
            }
            if (*a == *b) {
                ++mdl_shared;
            } else {
                ++mdl_differ;
            }
        }
    }
    json_append_double(out, "model_client_slots",
                       static_cast<double>(mdl_ce != nullptr ? mdl_ce->slot_count : 0), 0);
    json_append_double(out, "model_server_slots",
                       static_cast<double>(mdl_se != nullptr ? mdl_se->slot_count : 0), 0);
    json_append_double(out, "model_shared_slots", static_cast<double>(mdl_shared), 0);
    json_append_double(out, "model_differing_slots", static_cast<double>(mdl_differ), 0);
    // The four node-control slots must be four DISTINCT functions: the Add/Remove and node/object split
    // rests on them being separate implementations, not one shared entry point.
    size_t nc_distinct = 0;
    if (mdl_c != 0) {
        uintptr_t seen[4]{};
        for (size_t k = 0; k < 4; ++k) {
            const auto v = sdk::Vtables::vtable_of(mdl_c + (23 + k) * sizeof(uint32_t));
            seen[k] = v.value_or(0);
        }
        for (size_t k = 0; k < 4; ++k) {
            bool uniq = seen[k] != 0;
            for (size_t j = 0; j < k; ++j) {
                if (seen[j] == seen[k]) {
                    uniq = false;
                }
            }
            if (uniq) {
                ++nc_distinct;
            }
        }
    }
    json_append_double(out, "model_node_control_distinct", static_cast<double>(nc_distinct), 0);

    // ---- PURE VIRTUALS, AND THE CLASS HIERARCHY THEY REVEAL -----------------------------------
    //
    // A _purecall slot terminates the process rather than returning an error, so "may I call this slot"
    // is a safety question. Counting them across the catalogue also identifies abstract bases for free.
    size_t pure_total = 0, pure_timer = 0, pure_timer_client = 0, pure_timer_server = 0;
    size_t pure_cat_count = 0;
    const auto* pure_cat = sdk::Vtables::all(pure_cat_count);
    for (size_t i = 0; i < pure_cat_count; ++i) {
        const auto& e = pure_cat[i];
        const uintptr_t vt = sdk::Vtables::address(e.name);
        if (vt == 0) {
            continue;
        }
        for (size_t sl = 0; sl < e.slot_count; ++sl) {
            const auto pure = sdk::Vtables::is_pure_virtual(vt, sl);
            if (!pure.has_value() || !*pure) {
                continue;
            }
            ++pure_total;
            if (std::strcmp(e.name, "CLTTimer") == 0) {
                ++pure_timer;
            } else if (std::strcmp(e.name, "CLTTimerClient") == 0) {
                ++pure_timer_client;
            } else if (std::strcmp(e.name, "CLTTimerServer") == 0) {
                ++pure_timer_server;
            }
        }
    }
    json_append_double(out, "vtable_pure_total", static_cast<double>(pure_total), 0);
    json_append_double(out, "vtable_pure_timer_base", static_cast<double>(pure_timer), 0);
    json_append_double(out, "vtable_pure_timer_client", static_cast<double>(pure_timer_client), 0);
    json_append_double(out, "vtable_pure_timer_server", static_cast<double>(pure_timer_server), 0);

    // THE HIERARCHY ACCOUNTING: across CLTTimer and its two subclasses, every one of the 22 slots is
    // either inherited unchanged by all three, pure in the base and overridden by both, or per-class.
    const uintptr_t tb = sdk::Vtables::address("CLTTimer");
    const uintptr_t tc = sdk::Vtables::address("CLTTimerClient");
    const uintptr_t ts = sdk::Vtables::address("CLTTimerServer");
    size_t t_all_same = 0, t_pure_overridden = 0, t_other = 0;
    if (tb != 0 && tc != 0 && ts != 0) {
        for (size_t sl = 0; sl < 22; ++sl) {
            const auto a = sdk::Vtables::vtable_of(tb + sl * sizeof(uint32_t));
            const auto b = sdk::Vtables::vtable_of(tc + sl * sizeof(uint32_t));
            const auto d = sdk::Vtables::vtable_of(ts + sl * sizeof(uint32_t));
            if (!a.has_value() || !b.has_value() || !d.has_value()) {
                continue;
            }
            const bool base_pure = *a == sdk::Vtables::purecall_address();
            if (*a == *b && *b == *d) {
                ++t_all_same;
            } else if (base_pure && *b != *d) {
                ++t_pure_overridden;
            } else {
                ++t_other;
            }
        }
    }
    json_append_double(out, "timer_slots_inherited", static_cast<double>(t_all_same), 0);
    json_append_double(out, "timer_slots_pure_overridden", static_cast<double>(t_pure_overridden), 0);
    json_append_double(out, "timer_slots_other", static_cast<double>(t_other), 0);

    // ---- ILTCommon: THE SLOT MAP, AND ITS SERVER TWIN -----------------------------------------
    //
    // The structural fact worth checking is the PAIRING: CLTCommonServer aligns with the client slot for
    // slot, and ten slots share the identical function address. That is what makes the layout credible,
    // since one table's ordering could be coincidence while two independently resolved tables agreeing
    // cannot be.
    const uintptr_t cmn = sdk::Common::instance();
    json_append_bool(out, "common_instance", cmn != 0);
    const auto cmn_class = sdk::Common::class_name();
    json_append_bool(out, "common_class_is_cltcommonclient",
                     cmn_class.has_value() && *cmn_class == "CLTCommonClient");
    size_t cmn_slots_ok = 0;
    for (size_t i = 0; i < sdk::Common::kSlotCount; ++i) {
        if (sdk::Common::slot_address(static_cast<sdk::Common::Slot>(i)).has_value()) {
            ++cmn_slots_ok;
        }
    }
    json_append_double(out, "common_slots_resolved", static_cast<double>(cmn_slots_ok), 0);
    json_append_bool(out, "common_slot_past_end_refused",
                     !sdk::Common::slot_address(
                          static_cast<sdk::Common::Slot>(sdk::Common::kSlotCount)).has_value());

    // The client and server tables must agree on the ten shared implementations. Compared through the
    // catalogue, so this needs no hardcoded addresses beyond the two vtables it already knows.
    const uintptr_t cmn_client_vt = sdk::Vtables::address("CLTCommonClient");
    const uintptr_t cmn_server_vt = sdk::Vtables::address("CLTCommonServer");
    size_t cmn_shared = 0, cmn_differ = 0;
    if (cmn_client_vt != 0 && cmn_server_vt != 0) {
        for (size_t i = 0; i < sdk::Common::kSlotCount; ++i) {
            const auto a = sdk::Vtables::vtable_of(cmn_client_vt + i * sizeof(uint32_t));
            const auto b = sdk::Vtables::vtable_of(cmn_server_vt + i * sizeof(uint32_t));
            if (!a.has_value() || !b.has_value()) {
                continue;
            }
            if (*a == *b) {
                ++cmn_shared;
            } else {
                ++cmn_differ;
            }
        }
    }
    json_append_double(out, "common_shared_slots", static_cast<double>(cmn_shared), 0);
    json_append_double(out, "common_differing_slots", static_cast<double>(cmn_differ), 0);

    const auto low_violence = sdk::Common::is_low_violence();
    json_append_bool(out, "common_low_violence_readable", low_violence.has_value());
    json_append_bool(out, "common_low_violence", low_violence.value_or(false));
    json_append_bool(out, "common_null_object_refused", !sdk::Common::object_type(0).has_value());

    // ---- ILTPhysics: THE SLOT MAP, EXERCISED --------------------------------------------------
    //
    // The two queries that need no object handle are real calls through real vtable slots, which is the
    // only way to test a slot map: a wrong index either faults or returns nonsense, whereas reading the
    // table's addresses only proves they are addresses.
    const uintptr_t phys = sdk::Physics::instance();
    json_append_bool(out, "physics_instance", phys != 0);
    const auto phys_class = sdk::Physics::class_name();
    json_append_bool(out, "physics_class_is_cltphysicsclient",
                     phys_class.has_value() && *phys_class == "CLTPhysicsClient");
    size_t phys_slots_ok = 0;
    for (size_t i = 0; i < sdk::Physics::kSlotCount; ++i) {
        if (sdk::Physics::slot_address(static_cast<sdk::Physics::Slot>(i)).has_value()) {
            ++phys_slots_ok;
        }
    }
    json_append_double(out, "physics_slots_resolved", static_cast<double>(phys_slots_ok), 0);
    json_append_bool(out, "physics_slot_past_end_refused",
                     !sdk::Physics::slot_address(
                          static_cast<sdk::Physics::Slot>(sdk::Physics::kSlotCount)).has_value());

    const auto stair = sdk::Physics::stair_height();
    json_append_bool(out, "physics_stair_height_readable", stair.has_value());
    json_append_double(out, "physics_stair_height", stair.value_or(-1.0f), 3);

    const auto gforce = sdk::Physics::global_force();
    json_append_bool(out, "physics_global_force_readable", gforce.has_value());
    if (gforce.has_value()) {
        json_append_double(out, "physics_global_force_x", (*gforce)[0], 3);
        json_append_double(out, "physics_global_force_y", (*gforce)[1], 3);
        json_append_double(out, "physics_global_force_z", (*gforce)[2], 3);
    }
    // A null handle must be refused rather than dereferenced by the engine on our behalf.
    json_append_bool(out, "physics_null_object_refused",
                     !sdk::Physics::velocity(0).has_value() &&
                         !sdk::Physics::is_world_object(0).has_value());

    // ---- THE REGISTRY MET THE CATALOGUE ---------------------------------------------------------
    //
    // Two subsystems built independently: the registry discovers interface holders by scanning for
    // CAPIHolder_ctor call sites, and the catalogue bounds vtables by their trailing name string. Neither
    // knows about the other, so every resolved interface whose vtable the catalogue recognises is a point
    // where two separate reversing routes agree on the same object.
    //
    // A resolved interface whose vtable is NOT catalogued is not an error: the catalogue covers classes
    // that publish a name, and some interfaces are served by classes that do not.
    auto& registry = sdk::interfaces::Registry::get();
    size_t iface_resolved = 0, iface_named = 0, iface_unnamed = 0, iface_unnamed_foreign = 0;
    uint32_t unnamed_vtables[4]{};
    const uintptr_t iface_exe_lo = sdk::Modules::get().exe()->base;
    const uintptr_t iface_exe_hi = iface_exe_lo + sdk::Modules::get().exe()->size;
    bool input_identified = false, renderer_identified = false;
    for (const auto& nm : registry.names()) {
        void* p = registry.resolve(nm.c_str());
        if (p == nullptr) {
            continue;
        }
        ++iface_resolved;
        const auto cls = sdk::Vtables::class_name_of(reinterpret_cast<uintptr_t>(p));
        if (cls.has_value()) {
            ++iface_named;
            if (nm == "ILTInput.Default" && *cls == "CLTInput") {
                input_identified = true;
            }
            if (nm == "ILTRenderer.Default" && *cls == "CLTRenderer") {
                renderer_identified = true;
            }
        } else {
            // AN UNIDENTIFIED INTERFACE SHOULD MEAN "IMPLEMENTED IN ANOTHER MODULE", not "the catalogue
            // is short". Counting the two apart is what turns this from a report into a completeness
            // claim about the exe -- and it is how the 11 wrong extents were caught: they showed up here
            // as in-exe vtables the catalogue could not name.
            const auto vt = sdk::Vtables::vtable_of(reinterpret_cast<uintptr_t>(p));
            if (iface_unnamed < 4 && vt.has_value()) {
                unnamed_vtables[iface_unnamed] = static_cast<uint32_t>(*vt);
            }
            if (vt.has_value() && (*vt < iface_exe_lo || *vt >= iface_exe_hi)) {
                ++iface_unnamed_foreign;
            }
            ++iface_unnamed;
        }
    }
    json_append_double(out, "iface_resolved", static_cast<double>(iface_resolved), 0);
    json_append_double(out, "iface_class_named", static_cast<double>(iface_named), 0);
    json_append_double(out, "iface_class_unnamed", static_cast<double>(iface_unnamed), 0);
    json_append_double(out, "iface_unnamed_foreign", static_cast<double>(iface_unnamed_foreign), 0);
    for (size_t i = 0; i < 4; ++i) {
        char key[40];
        snprintf(key, sizeof(key), "iface_unnamed_vtable_%zu", i);
        json_append_double(out, key,
                           static_cast<double>(unnamed_vtables[i] >= iface_exe_lo
                                                   ? unnamed_vtables[i] - iface_exe_lo
                                                   : 0),
                           0);
    }
    json_append_bool(out, "iface_input_identified", input_identified);
    json_append_bool(out, "iface_renderer_identified", renderer_identified);
    // A pointer that is not an object at all must be refused rather than matched by accident.
    json_append_bool(out, "vtable_class_of_nonobject_refused",
                     !sdk::Vtables::class_name_of(sdk::Modules::get().exe()->base).has_value());

    // ---- THE VTABLE CATALOGUE, VERIFIED AGAINST LIVE MEMORY -------------------------------------
    //
    // Every entry's extent is checked in BOTH directions, which is what makes this worth running rather
    // than trusting: one slot too long and the extra dword is the first four bytes of the class-name
    // string, not an in-image address; one slot too short and the trailing-string read lands on a
    // function pointer instead of text. A single-sided check would pass either way.
    size_t vt_total = 0;
    const auto* vt_entries = sdk::Vtables::all(vt_total);
    size_t vt_verified = 0, vt_slots_ok = 0, vt_names_ok = 0, vt_slots_sum = 0, vt_convention = 0;
    for (size_t i = 0; i < vt_total; ++i) {
        const auto& e = vt_entries[i];
        if (e.follows_convention()) {
            ++vt_convention;
        }
        const auto v = sdk::Vtables::verify(e);
        if (!v.has_value()) {
            continue;
        }
        ++vt_verified;
        vt_slots_sum += v->slots_checked;
        if (v->slots_in_image) {
            ++vt_slots_ok;
        }
        if (v->name_matches) {
            ++vt_names_ok;
        }
    }
    json_append_double(out, "vtable_catalogue_total", static_cast<double>(vt_total), 0);
    json_append_double(out, "vtable_catalogue_verified", static_cast<double>(vt_verified), 0);
    json_append_double(out, "vtable_catalogue_slots_in_image", static_cast<double>(vt_slots_ok), 0);
    json_append_double(out, "vtable_catalogue_names_match", static_cast<double>(vt_names_ok), 0);
    json_append_double(out, "vtable_catalogue_slots_sum", static_cast<double>(vt_slots_sum), 0);
    json_append_double(out, "vtable_catalogue_convention", static_cast<double>(vt_convention), 0);

    // THE NAME PAIRING, RE-DERIVED FROM CODE. Adjacency put the string after the table; the getter is a
    // separate route to the same fact, read out of a six-byte stub. A Mismatch would mean a name attached
    // to the wrong table -- the one error neither the extent check nor adjacency can see.
    size_t nm_confirmed = 0, nm_not_getter = 0, nm_mismatch = 0, nm_unreadable = 0;
    for (size_t i = 0; i < vt_total; ++i) {
        switch (sdk::Vtables::check_name_getter(vt_entries[i])) {
        case sdk::Vtables::NameCheck::Confirmed: ++nm_confirmed; break;
        case sdk::Vtables::NameCheck::NotAGetter: ++nm_not_getter; break;
        case sdk::Vtables::NameCheck::Mismatch: ++nm_mismatch; break;
        default: ++nm_unreadable; break;
        }
    }
    json_append_double(out, "vtable_name_confirmed", static_cast<double>(nm_confirmed), 0);
    json_append_double(out, "vtable_name_not_getter", static_cast<double>(nm_not_getter), 0);
    json_append_double(out, "vtable_name_mismatch", static_cast<double>(nm_mismatch), 0);
    json_append_double(out, "vtable_name_unreadable", static_cast<double>(nm_unreadable), 0);

    // And the accessor works on a vtable reached WITHOUT the catalogue: the live CLTInput object's own
    // table, asked what class it is.
    const auto live_name = sdk::Vtables::name_from_getter(sdk::Input::interface_vtable(), 1);
    json_append_bool(out, "vtable_live_getter_names_cltinput",
                     live_name.has_value() && *live_name == "CLTInput");

    // The bounds check a consumer actually relies on: the last valid slot resolves, one past it does not.
    const auto* renderer_entry = sdk::Vtables::find("CLTRenderer");
    json_append_bool(out, "vtable_resolve_last_slot",
                     renderer_entry != nullptr &&
                         sdk::Vtables::resolve("CLTRenderer",
                                               renderer_entry->slot_count - 1).has_value());
    json_append_bool(out, "vtable_resolve_past_end_refused",
                     renderer_entry != nullptr &&
                         !sdk::Vtables::resolve("CLTRenderer", renderer_entry->slot_count).has_value());
    json_append_bool(out, "vtable_unknown_name_refused",
                     !sdk::Vtables::resolve("NoSuchClassHere", 0).has_value() &&
                         sdk::Vtables::address("NoSuchClassHere") == 0);
    // The catalogue's recorded vtable must be the one the LIVE OBJECT holds. Note the distinction the
    // first version of this check got wrong: interface_address() is the CLTInput object, while the
    // catalogue records its VTABLE, so comparing those two could only ever disagree.
    json_append_bool(out, "vtable_catalogue_agrees_with_input",
                     sdk::Vtables::address("CLTInput") == sdk::Input::interface_vtable() &&
                         sdk::Input::interface_vtable() != 0);

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

            // ---- IS THE "PLAUSIBLE NAME HASH" ACTUALLY THE NAME HASH? ----
            //
            // fear2.genny has carried DatabaseMgrCategory+0x10 and DatabaseMgrRecord+0x14 as "plausible name
            // hash, not otherwise confirmed" for several passes. The hash function is now known -- read out of
            // CMoveMgr_Init, which computes it inline -- so the claim is testable across the whole population.
            {
                const auto cat_agree = sdk::DatabaseMgr::category_hash_agreement(e->record_a);
                const auto rec_agree = sdk::DatabaseMgr::record_hash_agreement(e->record_a);
                entry0_json += ",\"cat_hash_compared\":" + std::to_string(cat_agree.compared);
                entry0_json += ",\"cat_hash_agreeing\":" + std::to_string(cat_agree.agreeing);
                entry0_json += ",\"cat_hash_skipped\":" + std::to_string(cat_agree.skipped);
                entry0_json += ",\"cat_hash_unanimous\":" + std::string(cat_agree.unanimous() ? "true" : "false");
                entry0_json += ",\"rec_hash_compared\":" + std::to_string(rec_agree.compared);
                entry0_json += ",\"rec_hash_agreeing\":" + std::to_string(rec_agree.agreeing);
                entry0_json += ",\"rec_hash_unanimous\":" + std::string(rec_agree.unanimous() ? "true" : "false");
                // THE FUNCTION ITSELF, checked on inputs whose answer is known from the disassembly and on the
                // case-insensitivity the fold table implies.
                const auto h_gun = sdk::DatabaseMgr::hash_name("GunLead");
                const auto h_gun_lc = sdk::DatabaseMgr::hash_name("gunlead");
                const auto h_pad = sdk::DatabaseMgr::hash_name("GamePad");
                entry0_json += ",\"hash_gunlead\":" + std::to_string(h_gun.value_or(0));
                entry0_json += ",\"hash_case_insensitive\":" +
                               std::string((h_gun.has_value() && h_gun == h_gun_lc) ? "true" : "false");
                entry0_json += ",\"hash_distinct\":" +
                               std::string((h_gun.has_value() && h_pad.has_value() && h_gun != h_pad) ? "true"
                                                                                                      : "false");
                entry0_json += ",\"hash_empty_is_zero\":" +
                               std::string((sdk::DatabaseMgr::hash_name("") == 0u) ? "true" : "false");

                // ---- LOOKUP BY NAME, MIRRORING THE ENGINE'S BINARY SEARCH ----
                //
                // The engine hashes and binary-searches with NO string compare, so the arrays must be sorted by
                // name_hash. That is an invariant of the data; if it fails, the game's own by-name lookup is
                // broken for the entries out of order.
                const bool cats_sorted = sdk::DatabaseMgr::categories_sorted_by_hash(e->record_a);
                entry0_json += ",\"cats_sorted_by_hash\":" + std::string(cats_sorted ? "true" : "false");
                // EVERY CATEGORY MUST BE FINDABLE BY ITS OWN NAME, and must come back as the same pointer the
                // index walk gives -- two routes to one entry.
                size_t findable = 0, walked = 0, recs_sorted = 0, cats_with_recs = 0;
                const auto ncat = sdk::DatabaseMgr::category_count(e->record_a);
                for (size_t i = 0; i < ncat; ++i) {
                    auto* cat = sdk::DatabaseMgr::category(e->record_a, i);
                    if (cat == nullptr) {
                        continue;
                    }
                    ++walked;
                    const auto nm = sdk::DatabaseMgr::category_name(cat);
                    if (!nm.empty() && sdk::DatabaseMgr::find_category(e->record_a, nm) == cat) {
                        ++findable;
                    }
                    if (sdk::DatabaseMgr::record_count(cat) > 0) {
                        ++cats_with_recs;
                        if (sdk::DatabaseMgr::records_sorted_by_hash(cat)) {
                            ++recs_sorted;
                        }
                    }
                }
                entry0_json += ",\"cats_walked\":" + std::to_string(walked);
                entry0_json += ",\"cats_findable\":" + std::to_string(findable);
                entry0_json += ",\"cats_with_records\":" + std::to_string(cats_with_recs);
                entry0_json += ",\"cats_records_sorted\":" + std::to_string(recs_sorted);
                // A NAME THAT DOES NOT EXIST MUST YIELD NOTHING, or "findable" proves nothing.
                entry0_json += ",\"absent_category_refused\":" +
                               std::string((sdk::DatabaseMgr::find_category(e->record_a, "NoSuchCategory/AtAll") ==
                                            nullptr) ? "true" : "false");
                // AND A KNOWN TWO-LEVEL LOOKUP through the convenience overload.
                {
                    auto* known = sdk::DatabaseMgr::find_category(e->record_a, "AI/WeaponContext");
                    entry0_json += ",\"known_category_found\":" +
                                   std::string(known != nullptr ? "true" : "false");
                    std::string first_rec;
                    if (known != nullptr && sdk::DatabaseMgr::record_count(known) > 0) {
                        first_rec = sdk::DatabaseMgr::record_name(sdk::DatabaseMgr::record(known, 0));
                        auto* via_name = sdk::DatabaseMgr::find_record(e->record_a, "AI/WeaponContext", first_rec);
                        entry0_json += ",\"two_level_lookup\":" +
                                       std::string((via_name != nullptr &&
                                                    via_name == sdk::DatabaseMgr::record(known, 0))
                                                       ? "true" : "false");
                    } else {
                        entry0_json += ",\"two_level_lookup\":false";
                    }
                }
                // COLLISIONS: with tens of thousands of names and a 32-bit hash, worth measuring.
                const auto coll = sdk::DatabaseMgr::hash_collisions(e->record_a);
                entry0_json += ",\"hash_names_examined\":" + std::to_string(coll.names);
                entry0_json += ",\"hash_collisions\":" + std::to_string(coll.collisions);
                entry0_json += ",\"hash_duplicate_names\":" + std::to_string(coll.duplicates);
                // WHERE ARE THE DUPLICATES? Localising them is what turns an alarming number into a fact.
                size_t keyed_cats = 0, pool_cats = 0, pool_records = 0, pool_distinct = 0;
                for (size_t i = 0; i < ncat; ++i) {
                    auto* cat = sdk::DatabaseMgr::category(e->record_a, i);
                    if (cat == nullptr || sdk::DatabaseMgr::record_count(cat) == 0) {
                        continue;
                    }
                    if (sdk::DatabaseMgr::name_is_unique_key(cat)) {
                        ++keyed_cats;
                    } else {
                        ++pool_cats;
                        pool_records += sdk::DatabaseMgr::record_count(cat);
                        pool_distinct += sdk::DatabaseMgr::distinct_name_count(cat);
                    }
                }
                entry0_json += ",\"keyed_categories\":" + std::to_string(keyed_cats);
                entry0_json += ",\"pool_categories\":" + std::to_string(pool_cats);
                entry0_json += ",\"pool_records\":" + std::to_string(pool_records);
                entry0_json += ",\"pool_distinct_names\":" + std::to_string(pool_distinct);
                // ---- ATTRIBUTES: the third level, and a cross-route name check ----
                {
                    size_t recs = 0, attrs = 0, sorted_ok = 0, bits = 0, decoded = 0;
                    size_t multi = 0, total_values = 0, zero_count = 0, bounds_ok = 0;
                    size_t links = 0, links_resolved = 0, structs = 0;
                    uint32_t type_mask = 0;
                    for (size_t i = 0; i < ncat; ++i) {
                        auto* cat = sdk::DatabaseMgr::category(e->record_a, i);
                        const auto nrec = sdk::DatabaseMgr::record_count(cat);
                        for (size_t j = 0; j < nrec; ++j) {
                            auto* rec = sdk::DatabaseMgr::record(cat, j);
                            if (rec == nullptr) {
                                continue;
                            }
                            ++recs;
                            if (sdk::DatabaseMgr::attributes_sorted_by_hash(rec)) {
                                ++sorted_ok;
                            }
                            const auto na = sdk::DatabaseMgr::attribute_count(rec);
                            for (size_t k = 0; k < na; ++k) {
                                const auto a = sdk::DatabaseMgr::attribute_at(rec, k);
                                if (!a.has_value()) {
                                    continue;
                                }
                                ++attrs;
                                if (a->type < 32) {
                                    type_mask |= (1u << a->type);
                                }
                                if (a->num_values == 0) {
                                    ++zero_count;
                                }
                                multi += (a->num_values > 1) ? 1 : 0;
                                total_values += a->num_values;
                                if (a->is_bit()) {
                                    ++bits;
                                    if (sdk::DatabaseMgr::attribute_bool(*a, 0).has_value()) {
                                        ++decoded;
                                    }
                                } else if (a->type == sdk::DatabaseMgr::kTypeFloat) {
                                    if (sdk::DatabaseMgr::attribute_float(*a, 0).has_value()) {
                                        ++decoded;
                                    }
                                } else if (a->is_record_link()) {
                                    ++links;
                                    if (sdk::DatabaseMgr::attribute_record(*a, 0) != nullptr) {
                                        ++links_resolved;
                                    }
                                    // A link with no values has nothing to decode; counting it as decoded is
                                    // what made the "everything decoded" figure disagree with the bounds count.
                                    if (a->num_values > 0) {
                                        ++decoded;
                                    }
                                } else if (a->is_struct()) {
                                    ++structs;
                                    if (sdk::DatabaseMgr::attribute_struct(*a, 0).size() ==
                                        sdk::DatabaseMgr::struct_dword_count(a->type)) {
                                        ++decoded;
                                    }
                                } else if (sdk::DatabaseMgr::attribute_raw_dword(*a, 0).has_value()) {
                                    ++decoded;
                                }
                                // THE LAST ELEMENT MUST ADDRESS, AND ONE PAST IT MUST NOT -- the bound every
                                // engine getter enforces.
                                if (a->num_values > 0 &&
                                    a->element_address(a->num_values - 1).has_value() &&
                                    !a->element_address(a->num_values).has_value()) {
                                    ++bounds_ok;
                                }
                            }
                        }
                    }
                    entry0_json += ",\"attr_records\":" + std::to_string(recs);
                    entry0_json += ",\"attr_total\":" + std::to_string(attrs);
                    entry0_json += ",\"attr_records_sorted\":" + std::to_string(sorted_ok);
                    entry0_json += ",\"attr_bits\":" + std::to_string(bits);
                    entry0_json += ",\"attr_decoded\":" + std::to_string(decoded);
                    entry0_json += ",\"attr_type_mask\":" + std::to_string(type_mask);
                    entry0_json += ",\"attr_multi_valued\":" + std::to_string(multi);
                    entry0_json += ",\"attr_total_values\":" + std::to_string(total_values);
                    entry0_json += ",\"attr_zero_count\":" + std::to_string(zero_count);
                    entry0_json += ",\"attr_bounds_ok\":" + std::to_string(bounds_ok);
                    entry0_json += ",\"attr_links\":" + std::to_string(links);
                    entry0_json += ",\"attr_links_resolved\":" + std::to_string(links_resolved);
                    entry0_json += ",\"attr_structs\":" + std::to_string(structs);
                    // NARROWING TYPES 3/4/5: are their dwords pointers into text?
                    std::string samples;
                    for (uint8_t t : {sdk::DatabaseMgr::kTypeDwordA, sdk::DatabaseMgr::kTypeDwordB,
                                      sdk::DatabaseMgr::kTypeDwordC}) {
                        const auto smp = sdk::DatabaseMgr::sample_type(e->record_a, t, 400);
                        if (!samples.empty()) {
                            samples += ";";
                        }
                        samples += "t" + std::to_string(t) + "=" + std::to_string(smp.sampled) + "/" +
                                   std::to_string(smp.pointer_like) + "/" + std::to_string(smp.ascii_like) +
                                   "/" + std::to_string(smp.utf16_like) + "/" +
                                   std::to_string(smp.ascii_at_4);
                    }
                    entry0_json += ",\"attr_type_samples\":";
                    json_escape_append(entry0_json, samples);
                    // WHAT DO THE 33 TYPE-5 POINTERS TARGET? Neither ASCII nor UTF-16, so dump the first one's
                    // bytes rather than guess again.
                    {
                        std::string dump;
                        for (size_t i = 0; i < ncat && dump.empty(); ++i) {
                            auto* cat = sdk::DatabaseMgr::category(e->record_a, i);
                            const auto nrec = sdk::DatabaseMgr::record_count(cat);
                            for (size_t j = 0; j < nrec && dump.empty(); ++j) {
                                auto* rec = sdk::DatabaseMgr::record(cat, j);
                                const auto na = sdk::DatabaseMgr::attribute_count(rec);
                                for (size_t k = 0; k < na && dump.empty(); ++k) {
                                    const auto a = sdk::DatabaseMgr::attribute_at(rec, k);
                                    if (!a.has_value() || a->type != sdk::DatabaseMgr::kTypeDwordC) {
                                        continue;
                                    }
                                    const auto raw = sdk::DatabaseMgr::attribute_raw_dword(*a, 0);
                                    if (!raw.has_value() || *raw <= 0x10000) {
                                        continue;
                                    }
                                    char buf[8]{};
                                    dump = sdk::DatabaseMgr::category_name(cat) + "/" +
                                           sdk::DatabaseMgr::record_name(rec) + " n=" +
                                           std::to_string(a->num_values) + " ->";
                                    for (size_t b = 0; b < 16; ++b) {
                                        const auto byte = sdk::mem::read<uint8_t>(*raw + b);
                                        snprintf(buf, sizeof(buf), " %02X", byte.value_or(0));
                                        dump += buf;
                                    }
                                }
                            }
                        }
                        entry0_json += ",\"attr_type5_probe\":";
                        json_escape_append(entry0_json, dump);
                    }

                    // ---- RECOVERING ATTRIBUTE NAMES FROM MODULE STRINGS ----
                    {
                        const auto& idx = sdk::DatabaseMgr::build_name_index();
                        entry0_json += ",\"nameidx_strings\":" + std::to_string(idx.strings_scanned);
                        entry0_json += ",\"nameidx_hashes\":" + std::to_string(idx.distinct_hashes);
                        entry0_json += ",\"nameidx_ready\":" + std::string(idx.ready() ? "true" : "false");
                        // THE ROUND TRIP: a name the code definitely contains must resolve to itself.
                        const auto wh = sdk::DatabaseMgr::hash_name("WaterAffectsSpeed");
                        const auto back = wh.has_value() ? sdk::DatabaseMgr::name_for_hash(*wh)
                                                         : std::nullopt;
                        entry0_json += ",\"nameidx_roundtrip\":" +
                                       std::string((back.has_value() && *back == "WaterAffectsSpeed")
                                                       ? "true" : "false");
                        entry0_json += ",\"nameidx_roundtrip_got\":";
                        json_escape_append(entry0_json, back.value_or("<none>"));
                        entry0_json += ",\"nameidx_wh\":" + std::to_string(wh.value_or(0));
                        // Is the literal present in the module at all? Ask the index for a few known-present
                        // names to separate "scan missed it" from "hash resolved to a different string".
                        std::string probes;
                        for (const char* nm : {"WaterAffectsSpeed", "GunLead", "GamePad", "YawClamp",
                                               "PlayerGravity"}) {
                            const auto hh = sdk::DatabaseMgr::hash_name(nm);
                            const auto got = hh.has_value() ? sdk::DatabaseMgr::name_for_hash(*hh)
                                                            : std::nullopt;
                            if (!probes.empty()) {
                                probes += ";";
                            }
                            probes += std::string(nm) + "->" + got.value_or("<none>");
                        }
                        entry0_json += ",\"nameidx_probes\":";
                        json_escape_append(entry0_json, probes);
                        // A HASH NO STRING PRODUCES must not resolve, or the index would "name" anything.
                        entry0_json += ",\"nameidx_absent_refused\":" +
                                       std::string(!sdk::DatabaseMgr::name_for_hash(0xDEADBEEFu).has_value()
                                                       ? "true" : "false");
                        const auto cov = sdk::DatabaseMgr::name_coverage(e->record_a, 4000);
                        entry0_json += ",\"nameidx_distinct_attrs\":" +
                                       std::to_string(cov.distinct_attribute_hashes);
                        entry0_json += ",\"nameidx_resolved\":" + std::to_string(cov.resolved);
                        // AND A NAMED EXAMPLE, so the capability is demonstrated and not just counted.
                        std::string named;
                        {
                            auto* shared = sdk::DatabaseMgr::find_category(e->record_a, "Client/Shared");
                            if (shared != nullptr && sdk::DatabaseMgr::record_count(shared) > 0) {
                                auto* rec = sdk::DatabaseMgr::record(shared, 0);
                                const auto na = sdk::DatabaseMgr::attribute_count(rec);
                                size_t shown = 0;
                                for (size_t k = 0; k < na && shown < 6; ++k) {
                                    const auto a = sdk::DatabaseMgr::attribute_at(rec, k);
                                    if (!a.has_value()) {
                                        continue;
                                    }
                                    const auto nm = sdk::DatabaseMgr::attribute_name(*a);
                                    if (!nm.has_value()) {
                                        continue;
                                    }
                                    if (!named.empty()) {
                                        named += ",";
                                    }
                                    named += *nm + ":t" + std::to_string(a->type);
                                    ++shown;
                                }
                            }
                        }
                        entry0_json += ",\"nameidx_example\":";
                        json_escape_append(entry0_json, named);
                    }

                    // ---- ARE 7 AND 8 FLOATS? ----
                    {
                        std::string st;
                        for (uint8_t t : {sdk::DatabaseMgr::kType8Bytes, sdk::DatabaseMgr::kType12Bytes,
                                          sdk::DatabaseMgr::kType16Bytes}) {
                            const auto smp = sdk::DatabaseMgr::sample_struct_type(e->record_a, t, 200);
                            if (!st.empty()) {
                                st += ";";
                            }
                            st += "t" + std::to_string(t) + "=" + std::to_string(smp.sampled) + "/" +
                                  std::to_string(smp.all_float_like) + "/" + std::to_string(smp.all_small_int) +
                                  "/" + std::to_string(smp.any_denormal);
                        }
                        entry0_json += ",\"attr_struct_samples\":";
                        json_escape_append(entry0_json, st);
                    }

                    // ---- WHAT IS THE STRING HEADER, AND WHAT SEPARATES 4 FROM 5? ----
                    {
                        std::string sh;
                        for (uint8_t t : {sdk::DatabaseMgr::kTypeDwordB, sdk::DatabaseMgr::kTypeDwordC}) {
                            const auto smp = sdk::DatabaseMgr::sample_string_header(e->record_a, t, 400);
                            if (!sh.empty()) {
                                sh += ";";
                            }
                            sh += "t" + std::to_string(t) + "=" + std::to_string(smp.sampled) + "/z" +
                                  std::to_string(smp.header_zero) + "/h" +
                                  std::to_string(smp.header_is_text_hash) + "/r" +
                                  std::to_string(smp.text_readable) + "/ids" +
                                  std::to_string(smp.text_is_ids_key);
                        }
                        entry0_json += ",\"attr_string_headers\":";
                        json_escape_append(entry0_json, sh);
                        // A NAMED, READ EXAMPLE of each, so the accessor is demonstrated and not just counted.
                        std::string texts;
                        for (size_t i = 0; i < ncat && texts.size() < 200; ++i) {
                            auto* cat = sdk::DatabaseMgr::category(e->record_a, i);
                            const auto nrec = sdk::DatabaseMgr::record_count(cat);
                            for (size_t j = 0; j < nrec && texts.size() < 200; ++j) {
                                auto* rec = sdk::DatabaseMgr::record(cat, j);
                                const auto na = sdk::DatabaseMgr::attribute_count(rec);
                                for (size_t k = 0; k < na && texts.size() < 200; ++k) {
                                    const auto a = sdk::DatabaseMgr::attribute_at(rec, k);
                                    if (!a.has_value() || a->type != sdk::DatabaseMgr::kTypeDwordC) {
                                        continue;
                                    }
                                    const auto txt = sdk::DatabaseMgr::attribute_text(*a, 0);
                                    const auto nm = sdk::DatabaseMgr::attribute_name(*a);
                                    if (!txt.has_value()) {
                                        continue;
                                    }
                                    if (!texts.empty()) {
                                        texts += ";";
                                    }
                                    texts += nm.value_or("?") + "=" + *txt;
                                }
                            }
                        }
                        entry0_json += ",\"attr_type5_texts\":";
                        json_escape_append(entry0_json, texts);
                    }

                    // ---- WHICH CATEGORIES CARRY CAMERA AND MOVEMENT TUNABLES? ----
                    //
                    // Recon: the FOV chain was established from console variables several passes ago. If the
                    // database also ships camera settings, a mod has a data-side lever as well as a cvar one.
                    {
                        std::string cams;
                        size_t hits = 0;
                        for (size_t i = 0; i < ncat; ++i) {
                            auto* cat = sdk::DatabaseMgr::category(e->record_a, i);
                            if (cat == nullptr) {
                                continue;
                            }
                            const auto nm = sdk::DatabaseMgr::category_name(cat);
                            const bool relevant =
                                nm.find("Camera") != std::string::npos ||
                                nm.find("camera") != std::string::npos ||
                                nm.find("Movement") != std::string::npos ||
                                nm.find("Player") != std::string::npos;
                            if (!relevant) {
                                continue;
                            }
                            ++hits;
                            if (cams.size() < 400) {
                                if (!cams.empty()) {
                                    cams += ";";
                                }
                                cams += nm + "(" + std::to_string(sdk::DatabaseMgr::record_count(cat)) + ")";
                            }
                        }
                        entry0_json += ",\"cam_categories\":" + std::to_string(hits);
                        entry0_json += ",\"cam_category_list\":";
                        json_escape_append(entry0_json, cams);

                        // Client/CameraClamping is the one that matters for a head-tracked view: dump every
                        // record it holds, named and typed.
                        {
                            auto* cc = sdk::DatabaseMgr::find_category(e->record_a, "Client/CameraClamping");
                            std::string dump;
                            size_t nrec = 0, attrs = 0, named = 0;
                            if (cc != nullptr) {
                                nrec = sdk::DatabaseMgr::record_count(cc);
                                for (size_t r = 0; r < nrec; ++r) {
                                    auto* rec2 = sdk::DatabaseMgr::record(cc, r);
                                    const auto cov = sdk::DatabaseMgr::describe_coverage(rec2);
                                    attrs += cov.attributes;
                                    named += cov.named;
                                    if (!dump.empty()) {
                                        dump += " || ";
                                    }
                                    dump += "[" + sdk::DatabaseMgr::record_name(rec2) + "] ";
                                    bool first = true;
                                    for (const auto& l : sdk::DatabaseMgr::describe_record_lines(rec2, 3)) {
                                        if (!first) {
                                            dump += " | ";
                                        }
                                        dump += l;
                                        first = false;
                                    }
                                }
                            }
                            entry0_json += ",\"clamp_records\":" + std::to_string(nrec);
                            entry0_json += ",\"clamp_attrs\":" + std::to_string(attrs);
                            entry0_json += ",\"clamp_named\":" + std::to_string(named);
                            entry0_json += ",\"clamp_dump\":";
                            json_escape_append(entry0_json, dump);
                            // THE PAIRS AS FLOATS, and whether every one is an ordered (min, max) range.
                            {
                                size_t pairs = 0, ordered = 0, plausible_angles = 0;
                                std::string defaults;
                                auto* def = (cc != nullptr) ? sdk::DatabaseMgr::find_record(cc, "Default")
                                                            : nullptr;
                                for (size_t r = 0; r < nrec; ++r) {
                                    auto* rec2 = sdk::DatabaseMgr::record(cc, r);
                                    const auto na = sdk::DatabaseMgr::attribute_count(rec2);
                                    for (size_t k = 0; k < na; ++k) {
                                        const auto a = sdk::DatabaseMgr::attribute_at(rec2, k);
                                        if (!a.has_value()) {
                                            continue;
                                        }
                                        const auto p = sdk::DatabaseMgr::attribute_float_pair(*a, 0);
                                        if (!p.has_value()) {
                                            continue;
                                        }
                                        ++pairs;
                                        if (p->ordered()) {
                                            ++ordered;
                                        }
                                        // Angles in degrees: positive and within a turn.
                                        if (p->first > 0.0f && p->second > 0.0f && p->second <= 360.0f) {
                                            ++plausible_angles;
                                        }
                                        if (rec2 == def && defaults.size() < 200) {
                                            const auto nm = sdk::DatabaseMgr::attribute_name(*a);
                                            char buf[64]{};
                                            snprintf(buf, sizeof(buf), "%g/%g", static_cast<double>(p->first),
                                                     static_cast<double>(p->second));
                                            if (!defaults.empty()) {
                                                defaults += ";";
                                            }
                                            defaults += nm.value_or("?") + "=" + buf;
                                        }
                                    }
                                }
                                entry0_json += ",\"clamp_pairs\":" + std::to_string(pairs);
                                entry0_json += ",\"clamp_ordered\":" + std::to_string(ordered);
                                entry0_json += ",\"clamp_angles\":" + std::to_string(plausible_angles);
                                entry0_json += ",\"clamp_defaults\":";
                                json_escape_append(entry0_json, defaults);
                                // A NON-TYPE-6 ATTRIBUTE MUST REFUSE the pair reader, or the type check is idle.
                                bool refused = false;
                                {
                                    auto* sh2 = sdk::DatabaseMgr::find_category(e->record_a, "Client/Shared");
                                    auto* rec3 = (sh2 != nullptr && sdk::DatabaseMgr::record_count(sh2) > 0)
                                                     ? sdk::DatabaseMgr::record(sh2, 0)
                                                     : nullptr;
                                    if (rec3 != nullptr) {
                                        const auto was =
                                            sdk::DatabaseMgr::find_attribute(rec3, "WaterAffectsSpeed");
                                        refused = was.has_value() &&
                                                  !sdk::DatabaseMgr::attribute_float_pair(*was, 0).has_value();
                                    }
                                }
                                entry0_json += ",\"clamp_pair_refused\":" +
                                               std::string(refused ? "true" : "false");
                            }
                        }
                    }

                    // ---- READING Client/Shared THE WAY THE GAME DOES ----
                    //
                    // The record CMoveMgr reads its movement tunables from, and the reference's hSharedRecord.
                    // This is the payoff of the whole database chain: names from the module's literals, values
                    // decoded by type.
                    {
                        auto* shared = sdk::DatabaseMgr::find_category(e->record_a, "Client/Shared");
                        auto* rec = (shared != nullptr && sdk::DatabaseMgr::record_count(shared) > 0)
                                        ? sdk::DatabaseMgr::record(shared, 0)
                                        : nullptr;
                        const auto cov = sdk::DatabaseMgr::describe_coverage(rec);
                        entry0_json += ",\"shared_attrs\":" + std::to_string(cov.attributes);
                        entry0_json += ",\"shared_named\":" + std::to_string(cov.named);
                        entry0_json += ",\"shared_valued\":" + std::to_string(cov.valued);
                        // A handful of lines, and specifically the movement ones a VR consumer cares about.
                        std::string lines;
                        size_t emitted = 0;
                        for (const auto& l : sdk::DatabaseMgr::describe_record_lines(rec, 3)) {
                            const bool interesting =
                                l.rfind("WaterAffectsSpeed", 0) == 0 || l.rfind("GunLead", 0) == 0 ||
                                l.rfind("GamePad", 0) == 0 || l.rfind("Gravity", 0) == 0 ||
                                l.rfind("JumpVel", 0) == 0 || l.rfind("WalkVel", 0) == 0 ||
                                l.rfind("RunVel", 0) == 0 || l.rfind("CrouchVel", 0) == 0;
                            if (!interesting || emitted >= 8) {
                                continue;
                            }
                            if (!lines.empty()) {
                                lines += " | ";
                            }
                            lines += l;
                            ++emitted;
                        }
                        entry0_json += ",\"shared_movement\":";
                        json_escape_append(entry0_json, lines);
                        // And the first few lines regardless, so the dump is visible even if none of the above
                        // names exist in this build.
                        std::string head;
                        size_t hn = 0;
                        for (const auto& l : sdk::DatabaseMgr::describe_record_lines(rec, 2)) {
                            if (hn >= 6) {
                                break;
                            }
                            if (!head.empty()) {
                                head += " | ";
                            }
                            head += l;
                            ++hn;
                        }
                        entry0_json += ",\"shared_head\":";
                        json_escape_append(entry0_json, head);

                        // ---- FOLLOWING A RECORD LINK, WHICH IS THE WHOLE CHAIN IN ONE STEP ----
                        //
                        // GunLead is a type-9 link on Client/Shared. CMoveMgr hashes "GunLead" to reach a
                        // sub-record and then reads "YawClamp" and "YawBias" from it -- and those two attributes
                        // were found in _Structures. If following the link lands on a record that HAS them, the
                        // link semantics, the _Structures pool and CMoveMgr's traversal all agree.
                        {
                            std::string linked_desc;
                            bool has_yaw = false, in_pool = false;
                            if (rec != nullptr) {
                                const auto gl = sdk::DatabaseMgr::find_attribute(rec, "GunLead");
                                if (gl.has_value()) {
                                    auto* target = sdk::DatabaseMgr::attribute_record(*gl, 0);
                                    if (target != nullptr) {
                                        has_yaw = sdk::DatabaseMgr::has_attribute(target, "YawClamp") &&
                                                  sdk::DatabaseMgr::has_attribute(target, "YawBias");
                                        // Does it live in the _Structures pool, as the earlier finding implies?
                                        const auto owner = sdk::mem::read_ptr(
                                            reinterpret_cast<uintptr_t>(target) + 0x10);
                                        if (owner.has_value() && *owner != 0) {
                                            const auto cn = sdk::DatabaseMgr::category_name(
                                                reinterpret_cast<regenny::DatabaseMgrCategory*>(*owner));
                                            in_pool = (cn == sdk::DatabaseMgr::kStructurePoolCategory);
                                        }
                                        size_t shown = 0;
                                        for (const auto& l :
                                             sdk::DatabaseMgr::describe_record_lines(target, 2)) {
                                            if (shown >= 5) {
                                                break;
                                            }
                                            if (!linked_desc.empty()) {
                                                linked_desc += " | ";
                                            }
                                            linked_desc += l;
                                            ++shown;
                                        }
                                    }
                                }
                            }
                            entry0_json += ",\"link_has_yaw\":" + std::string(has_yaw ? "true" : "false");
                            entry0_json += ",\"link_in_pool\":" + std::string(in_pool ? "true" : "false");
                            entry0_json += ",\"link_desc\":";
                            json_escape_append(entry0_json, linked_desc);
                        }
                    }

                    // THE CROSS-ROUTE CHECK. These names come from gameclient's CMoveMgr_Init, which reads them
                    // as DATABASE attributes. Their hashes must appear as descriptors somewhere in the database
                    // -- names from one module's code, structures from another's data.
                    const char* kFromCode[] = {"WaterAffectsSpeed", "YawClamp", "YawBias", "GunLead", "GamePad"};
                    size_t found_from_code = 0;
                    std::string where;
                    for (const char* nm : kFromCode) {
                        bool hit = false;
                        for (size_t i = 0; i < ncat && !hit; ++i) {
                            auto* cat = sdk::DatabaseMgr::category(e->record_a, i);
                            const auto nrec = sdk::DatabaseMgr::record_count(cat);
                            for (size_t j = 0; j < nrec && !hit; ++j) {
                                auto* rec = sdk::DatabaseMgr::record(cat, j);
                                if (rec != nullptr && sdk::DatabaseMgr::has_attribute(rec, nm)) {
                                    hit = true;
                                    if (where.size() < 200) {
                                        if (!where.empty()) {
                                            where += ";";
                                        }
                                        where += std::string(nm) + "@" + sdk::DatabaseMgr::category_name(cat);
                                    }
                                }
                            }
                        }
                        if (hit) {
                            ++found_from_code;
                        }
                    }
                    entry0_json += ",\"attr_from_code_found\":" + std::to_string(found_from_code);
                    entry0_json += ",\"attr_from_code_total\":" + std::to_string(std::size(kFromCode));
                    entry0_json += ",\"attr_from_code_where\":";
                    json_escape_append(entry0_json, where);
                    // A NAME NO RECORD DEFINES must be refused, or "found" means nothing.
                    bool bogus = false;
                    for (size_t i = 0; i < ncat && !bogus; ++i) {
                        auto* cat = sdk::DatabaseMgr::category(e->record_a, i);
                        const auto nrec = sdk::DatabaseMgr::record_count(cat);
                        for (size_t j = 0; j < nrec && !bogus; ++j) {
                            auto* rec = sdk::DatabaseMgr::record(cat, j);
                            if (rec != nullptr && sdk::DatabaseMgr::has_attribute(rec, "ZzNoSuchAttributeZz")) {
                                bogus = true;
                            }
                        }
                    }
                    entry0_json += ",\"attr_bogus_refused\":" + std::string(bogus ? "false" : "true");

                    // THE TYPE TAGS, pinned against how the GAME reads each attribute. The reference source
                    // reads WaterAffectsSpeed with GetBool and YawClamp/YawBias as floats, so their type tags
                    // name two members of the enum with external evidence rather than by guessing.
                    std::string types;
                    for (const char* nm : kFromCode) {
                        for (size_t i = 0; i < ncat; ++i) {
                            auto* cat = sdk::DatabaseMgr::category(e->record_a, i);
                            const auto nrec = sdk::DatabaseMgr::record_count(cat);
                            bool done = false;
                            for (size_t j = 0; j < nrec && !done; ++j) {
                                auto* rec = sdk::DatabaseMgr::record(cat, j);
                                if (rec == nullptr) {
                                    continue;
                                }
                                const auto a = sdk::DatabaseMgr::find_attribute(rec, nm);
                                if (!a.has_value()) {
                                    continue;
                                }
                                if (!types.empty()) {
                                    types += ";";
                                }
                                types += std::string(nm) + "=" + std::to_string(a->type) +
                                         (a->is_bit() ? "(bit)" : "");
                                done = true;
                            }
                            if (done) {
                                break;
                            }
                        }
                    }
                    entry0_json += ",\"attr_code_types\":";
                    json_escape_append(entry0_json, types);
                    // A BIT ATTRIBUTE MUST REFUSE the dword reader and vice versa -- the guard that stops a
                    // caller reading 32 packed booleans as one integer.
                    bool strict = true;
                    {
                        auto* shared = sdk::DatabaseMgr::find_category(e->record_a, "Client/Shared");
                        if (shared != nullptr && sdk::DatabaseMgr::record_count(shared) > 0) {
                            auto* rec = sdk::DatabaseMgr::record(shared, 0);
                            const auto was = sdk::DatabaseMgr::find_attribute(rec, "WaterAffectsSpeed");
                            if (was.has_value()) {
                                strict = sdk::DatabaseMgr::attribute_bool(*was, 0).has_value() &&
                                         !sdk::DatabaseMgr::attribute_float(*was, 0).has_value() &&
                                         !sdk::DatabaseMgr::attribute_raw_dword(*was, 0).has_value();
                                entry0_json += ",\"attr_water_value\":" +
                                               std::string(sdk::DatabaseMgr::attribute_bool(*was, 0)
                                                                   .value_or(false)
                                                               ? "true" : "false");
                                entry0_json += ",\"attr_water_num_values\":" +
                                               std::to_string(was->num_values);
                            }
                        }
                    }
                    entry0_json += ",\"attr_readers_strict\":" + std::string(strict ? "true" : "false");
                }
                {
                    auto* pool = sdk::DatabaseMgr::find_category(e->record_a,
                                                                sdk::DatabaseMgr::kStructurePoolCategory);
                    entry0_json += ",\"pool_is_structures\":" +
                                   std::string((pool != nullptr && !sdk::DatabaseMgr::name_is_unique_key(pool))
                                                   ? "true" : "false");
                }
            }

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

// =====================================================================================
// /api/* -- read-only endpoints for the browser-based inspector (WebUi). Diagnostics
// only, per AGENT.MD rule 2: reads go through sdk:: accessors and sdk::mem guarded
// readers, nothing here calls into the engine, and absence is always JSON `null`,
// never an invented zero (see PlayerStats/TimerState/etc. below).
// =====================================================================================

using WebApiQuery = std::unordered_map<std::string, std::string>;

// ---- tiny query-string parser, local to this file -----------------------------------
//
// cmdsrv::handle_client hands the raw request target (path + '?' + query, still
// percent-encoded) straight through via Handlers::api; nothing upstream parses it.
std::string webapi_percent_decode(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    auto hex_digit = [](char h) -> int {
        if (h >= '0' && h <= '9') return h - '0';
        if (h >= 'a' && h <= 'f') return h - 'a' + 10;
        if (h >= 'A' && h <= 'F') return h - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '+') {
            out += ' ';
        } else if (c == '%' && i + 2 < s.size()) {
            const int hi = hex_digit(s[i + 1]);
            const int lo = hex_digit(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>((hi << 4) | lo);
                i += 2;
            } else {
                out += c;
            }
        } else {
            out += c;
        }
    }
    return out;
}

WebApiQuery webapi_parse_query(const std::string& request_target) {
    WebApiQuery out;
    const size_t q = request_target.find('?');
    if (q == std::string::npos) {
        return out;
    }
    const std::string qs = request_target.substr(q + 1);
    size_t start = 0;
    while (start <= qs.size()) {
        size_t amp = qs.find('&', start);
        if (amp == std::string::npos) {
            amp = qs.size();
        }
        const std::string pair = qs.substr(start, amp - start);
        if (!pair.empty()) {
            const size_t eq = pair.find('=');
            const std::string key = eq == std::string::npos ? pair : pair.substr(0, eq);
            const std::string val = eq == std::string::npos ? std::string{} : pair.substr(eq + 1);
            out[webapi_percent_decode(key)] = webapi_percent_decode(val);
        }
        start = amp + 1;
    }
    return out;
}

std::string webapi_query_string(const WebApiQuery& q, const char* key) {
    const auto it = q.find(key);
    return it != q.end() ? it->second : std::string{};
}

// A FRACTIONAL query parameter. webapi_query_int truncates, which is wrong for every value this project
// actually wants from a caller now -- an eye separation, a field of view in radians, a viewport fraction.
double webapi_query_double(const WebApiQuery& q, const char* key, double fallback) {
    const auto it = q.find(key);
    if (it == q.end() || it->second.empty()) {
        return fallback;
    }
    char* end = nullptr;
    const double v = strtod(it->second.c_str(), &end);
    // Unparsable is the fallback, not zero: "fov_x=abc" meaning "no override" is friendlier than it meaning
    // "a field of view of zero", which the engine would clamp into something visible and confusing.
    if (end == it->second.c_str()) {
        return fallback;
    }
    return v;
}

long long webapi_query_int(const WebApiQuery& q, const char* key, long long fallback) {
    const auto it = q.find(key);
    if (it == q.end() || it->second.empty()) {
        return fallback;
    }
    char* end = nullptr;
    const long long v = strtoll(it->second.c_str(), &end, 10);
    return (end != nullptr && end != it->second.c_str()) ? v : fallback;
}

// Bound every /api/* list the same way: default 100, hard cap 500, never negative.
size_t webapi_clamp_limit(long long requested) {
    if (requested <= 0) {
        return 100;
    }
    return static_cast<size_t>(std::min<long long>(requested, 500));
}

size_t webapi_clamp_offset(long long requested) { return requested > 0 ? static_cast<size_t>(requested) : 0; }

// Case-insensitive substring test -- every filter= param below. An empty needle matches
// everything, so "no filter" and "empty filter" behave identically.
bool webapi_contains_ci(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) {
        return true;
    }
    auto lower = [](std::string s) {
        for (auto& c : s) {
            c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
        }
        return s;
    };
    return lower(haystack).find(lower(needle)) != std::string::npos;
}

// The live database handle, reached exactly the way build_database_json() does: get() ->
// entry(0) -> record_a. Reused rather than re-derived so there is one path to "the" database.
const regenny::DatabaseMgrSubRecord* webapi_database_handle() {
    auto* db = sdk::DatabaseMgr::get();
    if (db == nullptr || db->entry_count() < 1) {
        return nullptr;
    }
    auto* e = db->entry(0);
    return e != nullptr ? e->record_a : nullptr;
}

// The established attribute-type mapping (see DatabaseMgr.hpp's kType* constants and their
// comments for the evidence behind each). Anything not in the mapping is genuinely unseen in
// the shipped data, so it renders as "type<N>" rather than a guessed name.
std::string webapi_db_type_name(uint8_t type) {
    switch (type) {
        case sdk::DatabaseMgr::kTypeBool: return "bool";
        case sdk::DatabaseMgr::kTypeFloat: return "float";
        case sdk::DatabaseMgr::kTypeDwordA: return "int32";
        case sdk::DatabaseMgr::kTypeString: return "string";
        case sdk::DatabaseMgr::kTypeLocalizedKey: return "localized-key";
        case sdk::DatabaseMgr::kTypeFloatPair: return "float-pair";
        case sdk::DatabaseMgr::kType12Bytes: return "vector3";
        case sdk::DatabaseMgr::kType16Bytes: return "vector4";
        case sdk::DatabaseMgr::kTypeRecordLink:
        case sdk::DatabaseMgr::kTypeRecordLinkAlt: return "record-link";
        default: return "type<" + std::to_string(static_cast<int>(type)) + ">";
    }
}

// A JSON array literal of floats, for velocity/deg/rad pairs and triples below.
std::string webapi_float_array(std::initializer_list<float> values, int decimals = 4) {
    std::string out = "[";
    bool first = true;
    for (float v : values) {
        if (!first) {
            out += ',';
        }
        first = false;
        char buf[32];
        snprintf(buf, sizeof(buf), "%.*f", decimals, v);
        out += buf;
    }
    out += ']';
    return out;
}

// A quoted "0x%06X" JSON string literal -- the width the contract specifies for
// gameclient-relative offsets (subsystem ctors, console registrar offsets), distinct from
// JsonFields::hex's fixed 8-digit width for absolute addresses.
std::string webapi_hex6_json(uintptr_t v) {
    char buf[16];
    snprintf(buf, sizeof(buf), "\"0x%06X\"", static_cast<unsigned int>(v));
    return buf;
}

// ---- GET /api/state -------------------------------------------------------------------
// ---- WHAT THE PLAYER IS HOLDING, AND WHAT THEY COULD HOLD -----------------------------------
//
// sdk::WeaponMgr's public surface, published so a wheel prototype (or the suite) can see the same
// values without linking against the mod. `limit` caps the catalogue: the weapon category is large
// and a consumer paging through it should not be forced to take all of it at once.
std::string build_weapons_json(const std::string& request_target) {
    const WebApiQuery q = webapi_parse_query(request_target);
    const auto limit = static_cast<size_t>(webapi_query_int(q, "limit", 24));
    const auto requested_select = webapi_query_string(q, "select");

    const auto chooser = sdk::WeaponMgr::chooser(0);
    const auto current = sdk::WeaponMgr::current_weapon_name(0);
    const auto last = sdk::WeaponMgr::last_weapon_name(0);
    const auto slot = sdk::WeaponMgr::current_slot(0);
    const auto index = sdk::WeaponMgr::current_weapon_index(0);
    const auto agrees = sdk::WeaponMgr::current_slot_indexes_current_weapon(0);
    const auto loadout = sdk::WeaponMgr::loadout_names(0);
    const auto arsenal = sdk::WeaponMgr::arsenal_count();
    const auto names = sdk::WeaponMgr::weapon_names();

    std::string out = "{";
    json_append_bool(out, "ok", chooser != 0);
    json_append_raw(out, "chooser", std::to_string(static_cast<uint64_t>(chooser)).c_str());
    json_append_raw(out, "weapon_object",
                    std::to_string(static_cast<uint64_t>(sdk::WeaponMgr::current_weapon_object(0))).c_str());
    json_append_string(out, "current", current.c_str());

    // The QUICK-SWITCH slot, published beside `current` on purpose: an earlier pass reported this
    // field AS the current weapon, and seeing both makes the difference visible instead of a claim.
    json_append_string(out, "last", last.c_str());
    json_append_string(out, "pending", sdk::WeaponMgr::pending_weapon_name(0).c_str());
    json_append_bool(out, "equipped", sdk::WeaponMgr::equipped(0));
    json_append_bool(out, "switching", sdk::WeaponMgr::switching(0));

    // THE MAGAZINE AND ITS RESERVE -- the two halves of the HUD's "26/385", read together so a
    // consumer comparing them is not comparing two instants.
    const auto mag = sdk::WeaponMgr::magazine_rounds(0);
    json_append_raw(out, "magazine",
                    std::to_string(mag.has_value() ? static_cast<int64_t>(*mag) : -1).c_str());
    const auto ammo_name = sdk::WeaponMgr::current_ammo_name(0);
    json_append_string(out, "ammo_type", ammo_name.c_str());
    const auto reserve = ammo_name.empty() ? std::nullopt
                                           : sdk::PlayerMgr::ammo_count(0, ammo_name);
    json_append_raw(out, "reserve",
                    std::to_string(reserve.has_value() ? static_cast<int64_t>(*reserve) : -1).c_str());
    const auto spare = sdk::WeaponMgr::spare_rounds(0);
    json_append_raw(out, "spare",
                    std::to_string(spare.has_value() ? static_cast<int64_t>(*spare) : -1).c_str());

    // ---- THE WHEEL ITSELF --------------------------------------------------------------------
    //
    // `select=<name>` asks for a weapon BY NAME and returns immediately; the selection runs on the
    // game thread over the following frames (a key press spans frames and the switch takes ~0.5 s).
    // Poll `wheel_state` for the outcome. `cancel=1` abandons an outstanding request.
    if (!requested_select.empty()) {
        json_append_bool(out, "select_accepted", WeaponWheel::get().request(requested_select));
    }
    if (webapi_query_int(q, "cancel", 0) != 0) {
        WeaponWheel::get().cancel();
    }
    json_append_raw(out, "wheel_state",
                    std::to_string(static_cast<int>(WeaponWheel::get().state())).c_str());
    json_append_bool(out, "wheel_busy", WeaponWheel::get().busy());
    json_append_string(out, "wheel_requested", WeaponWheel::get().requested().c_str());
    json_append_raw(out, "wheel_presses", std::to_string(WeaponWheel::get().presses()).c_str());
    json_append_raw(out, "wheel_frames", std::to_string(WeaponWheel::get().frames_taken()).c_str());
    json_append_string(out, "wheel_error", WeaponWheel::get().last_error().c_str());
    json_append_bool(out, "current_is_weapon", sdk::WeaponMgr::current_weapon(0) != nullptr);
    json_append_raw(out, "current_slot",
                    std::to_string(slot.has_value() ? static_cast<int64_t>(*slot) : -1).c_str());
    json_append_raw(out, "current_index",
                    std::to_string(index.has_value() ? static_cast<int64_t>(*index) : -1).c_str());

    // The loadout's self-consistency: array[index] must BE the current object.
    json_append_raw(out, "slot_agrees",
                    agrees.has_value() ? (*agrees ? "1" : "0") : "-1");
    json_append_raw(out, "loadout", std::to_string(loadout.size()).c_str());
    json_append_raw(out, "arsenal",
                    std::to_string(arsenal.has_value() ? static_cast<int64_t>(*arsenal) : -1).c_str());
    // The whole read-side answer to "how do I switch to this weapon", for the held one.
    // "Which key selects this weapon?" -- the wheel's own question, answered for any name the
    // caller passes rather than only for the weapon in hand.
    const auto requested = webapi_query_string(q, "key_for");
    if (!requested.empty()) {
        const auto vk = sdk::WeaponMgr::key_for_weapon(requested, 0);
        json_append_raw(out, "key_for_requested",
                        std::to_string(vk.has_value() ? static_cast<int>(*vk) : -1).c_str());
        const auto rs = sdk::WeaponMgr::loadout_slot_of(requested, 0);
        json_append_raw(out, "slot_for_requested",
                        std::to_string(rs.has_value() ? static_cast<int64_t>(*rs) : -1).c_str());
    }
    const auto key_cur = sdk::WeaponMgr::key_for_weapon(current, 0);
    json_append_raw(out, "key_for_current",
                    std::to_string(key_cur.has_value() ? static_cast<int>(*key_cur) : -1).c_str());
    const auto slot_cur = sdk::WeaponMgr::loadout_slot_of(current, 0);
    json_append_raw(out, "loadout_slot_of_current",
                    std::to_string(slot_cur.has_value() ? static_cast<int64_t>(*slot_cur) : -1).c_str());
    json_append_raw(out, "count", std::to_string(sdk::WeaponMgr::weapon_count()).c_str());
    json_append_raw(out, "named", std::to_string(names.size()).c_str());

    const auto k1 = sdk::WeaponMgr::slot_virtual_key(1);
    json_append_raw(out, "slot1_vk",
                    std::to_string(k1.has_value() ? static_cast<int>(*k1) : -1).c_str());
    const auto kbad = sdk::WeaponMgr::slot_virtual_key(sdk::WeaponMgr::kMaxBoundSlot + 1);
    json_append_bool(out, "slot_overflow_refused", !kbad.has_value());
    auto* missing = sdk::WeaponMgr::find_weapon("__no_such_weapon__");
    json_append_bool(out, "missing_weapon_refused", missing == nullptr);
    json_append_bool(out, "null_refused", !sdk::WeaponMgr::is_weapon(nullptr));
    json_append_bool(out, "muzzle_resolvable", sdk::WeaponMgr::muzzle_resolvable(0));

    out += "\"loadout_names\":[";
    for (size_t i = 0; i < loadout.size(); ++i) {
        if (i != 0) {
            out += ',';
        }
        json_escape_append(out, loadout[i]);
    }
    out += "],\"names\":[";
    for (size_t i = 0; i < names.size() && i < limit; ++i) {
        if (i != 0) {
            out += ',';
        }
        json_escape_append(out, names[i]);
    }
    out += "]}";
    return out;
}

std::string build_api_state_json() {
    std::string out;
    JsonFields top(out);
    top.b("ok", true);

    // Player index 0 -- the slot every other PlayerMgr-based diagnostic in this file uses
    // (see e.g. build_objects_json's pmgr_*/mm_*/cam_* fields); single-player is slot 0.
    const auto stats = sdk::PlayerMgr::player_stats(0);
    {
        std::string obj;
        {
            JsonFields jf(obj);
            jf.b("resolved", stats.has_value());
            if (stats.has_value()) {
                jf.i("health", stats->health);
                jf.i("armor", stats->armor);
                jf.i("max_health", stats->max_health);
                jf.i("max_armor", stats->max_armor);
                jf.f("air", stats->air, 4);
                jf.i("health_lost", stats->health_lost);
                jf.b("alive", stats->alive());
                jf.b("consistent", stats->consistent());
            } else {
                jf.n("health"); jf.n("armor"); jf.n("max_health"); jf.n("max_armor");
                jf.n("air"); jf.n("health_lost"); jf.n("alive"); jf.n("consistent");
            }
        }
        top.raw("player", obj);
    }

    {
        const auto flags = sdk::PlayerMgr::move_mgr_flags(0);
        const auto crouching = sdk::PlayerMgr::is_crouching(0);
        const auto moving = sdk::PlayerMgr::is_moving(0);
        const auto velocity = sdk::PlayerMgr::physics_velocity(0);
        const auto water = sdk::PlayerMgr::water_affects_speed(0);
        const auto ssm = sdk::PlayerMgr::spectator_speed_mul(0);

        std::string obj;
        {
            JsonFields jf(obj);
            if (flags.has_value()) { jf.u("flags", *flags); } else { jf.n("flags"); }
            // DECODED, because a raw dword tells a reader nothing. The bit map is established in
            // PlayerMgr::MoveFlag; unmapped is reported rather than hidden, since the producer also sets bits
            // this mapping has not accounted for (0x4 and 0x8 among them).
            if (const auto mf = sdk::PlayerMgr::movement_flags(0)) {
                jf.s("flags_decoded", sdk::PlayerMgr::movement_flag_names(mf->raw));
                jf.u("flags_unmapped", mf->unmapped());
                jf.b("sprinting", mf->sprinting());
                jf.b("melee", mf->melee());
                jf.b("grenade_held", mf->grenade_held());
                jf.b("normal_speed", mf->normal_speed());
                jf.b("counts_as_moving", mf->counts_as_moving());
                jf.b("forward", mf->forward()).b("backward", mf->backward());
                jf.b("left", mf->left()).b("right", mf->right());
                const auto d = mf->input_direction();
                jf.raw("input_dir", webapi_float_array({static_cast<float>(d[0]), static_cast<float>(d[1])}, 0));
                jf.b("input_dir_contradicts", mf->direction_contradicts());
            } else {
                jf.n("flags_decoded").n("flags_unmapped");
            }
            if (crouching.has_value()) { jf.b("crouching", *crouching); } else { jf.n("crouching"); }
            if (moving.has_value()) { jf.b("moving", *moving); } else { jf.n("moving"); }
            if (velocity.has_value()) {
                const float speed = std::sqrt((*velocity)[0] * (*velocity)[0] + (*velocity)[1] * (*velocity)[1] +
                                              (*velocity)[2] * (*velocity)[2]);
                jf.f("speed", speed, 3);
                jf.raw("velocity", webapi_float_array({(*velocity)[0], (*velocity)[1], (*velocity)[2]}, 3));
            } else {
                jf.n("speed");
                jf.n("velocity");
            }
            if (water.has_value()) { jf.b("water_affects_speed", *water); } else { jf.n("water_affects_speed"); }
            if (ssm.has_value()) { jf.f("spectator_speed_mul", *ssm, 4); } else { jf.n("spectator_speed_mul"); }
        }
        top.raw("movement", obj);
    }

    {
        std::string obj;
        {
        JsonFields jf(obj);

        auto* clamp_record = sdk::PlayerMgr::camera_clamp_record(0);
        jf.s("clamp_record", sdk::DatabaseMgr::record_name(clamp_record));

        const auto state = sdk::PlayerMgr::camera_clamp_state(0);
        if (state.has_value()) { jf.u("state", *state); } else { jf.n("state"); }

        const auto is_chase = sdk::PlayerMgr::camera_state_is_chase(0);
        if (is_chase.has_value()) { jf.b("is_chase", *is_chase); } else { jf.n("is_chase"); }

        const auto predicted = sdk::PlayerMgr::predicted_clamp_state(0);
        if (predicted.has_value()) {
            jf.s("predicted_clamp", predicted->state);
            jf.b("slide_kick_unchecked", predicted->slide_kick_unchecked);
        } else {
            jf.n("predicted_clamp");
            jf.n("slide_kick_unchecked");
        }

        // The six shipped clamp states (see PlayerMgr::predicted_clamp_state's own dispatcher
        // logic for where these six names come from). Every key is always present; a state
        // whose clamp could not be read still gets the key, with null deg/rad.
        static constexpr const char* kClampStates[] = {"StandIdle",    "StandMoving", "CrouchIdle",
                                                        "CrouchMoving", "Chase",       "SlideKick"};
        std::string clamps = "{";
        bool first_clamp = true;
        for (const char* name : kClampStates) {
            const auto deg = sdk::PlayerMgr::camera_clamp(0, name);
            const auto rad = sdk::PlayerMgr::camera_clamp_radians(0, name);
            std::string entry;
            {
                JsonFields cjf(entry);
                if (deg.has_value()) { cjf.raw("deg", webapi_float_array({deg->first, deg->second}, 4)); }
                else { cjf.n("deg"); }
                if (rad.has_value()) { cjf.raw("rad", webapi_float_array({rad->first, rad->second}, 6)); }
                else { cjf.n("rad"); }
            }
            if (!first_clamp) { clamps += ','; }
            first_clamp = false;
            clamps += '"';
            clamps += name;
            clamps += "\":";
            clamps += entry;
        }
        clamps += '}';
        jf.raw("clamps", clamps);

        const auto pitch = sdk::PlayerMgr::camera_pitch_clamp_record(0);
        if (pitch.has_value()) {
            jf.f("pitch_before", pitch->before, 4);
            jf.f("pitch_after", pitch->after, 4);
        } else {
            jf.n("pitch_before");
            jf.n("pitch_after");
        }
        const auto pitch_corrected = sdk::PlayerMgr::pitch_clamp_record_within_active(0);
        if (pitch_corrected.has_value()) { jf.b("pitch_corrected", *pitch_corrected); }
        else { jf.n("pitch_corrected"); }

        {
            const auto timer = sdk::PlayerMgr::pitch_recovery_timer(0);
            std::string tobj;
            {
                JsonFields tjf(tobj);
                if (timer.has_value()) {
                    tjf.b("active", timer->active);
                    tjf.f("duration", timer->duration, 4);
                    tjf.b("use_cached", timer->use_cached);
                } else {
                    tjf.n("active"); tjf.n("duration"); tjf.n("use_cached");
                }
            }
            jf.raw("timer", tobj);
        }

        {
            const auto aim = sdk::PlayerMgr::aim_tracking_limits();
            const auto aim_flag = sdk::PlayerMgr::ads_fov_active(0);
            // THE AIM STATE MACHINE AND THE LIMIT ACTUALLY IN FORCE.
            //
            // This block used to carry normal_deg, zoomed_deg and a bare "flag" -- a name inherited from a
            // mapping that has since been refuted, and which said nothing about what the flag does. Neither the
            // four-state machine nor the zoom fraction was reachable here at all, which made the one subsystem
            // most relevant to a head-tracked view invisible to anything reading this API.
            //
            // limit_deg is the consumer's actual question: not "what are the two limits" but "which one is
            // clamping me right now". It is computed the same way ApplyLookDelta picks it.
            const auto aim_state = sdk::PlayerMgr::aim_state_raw(0);
            const auto aim_zoomed = sdk::PlayerMgr::uses_zoomed_aim_limit(0);
            const auto aim_clk = sdk::Engine::client_time();
            const auto aim_frac =
                sdk::PlayerMgr::zoom_fraction(0, aim_clk.has_value() ? aim_clk->seconds : 0.0);
            const char* aim_state_name = "unknown";
            if (aim_state.has_value()) {
                switch (*aim_state) {
                case 0: aim_state_name = "entering_ads"; break;
                case 1: aim_state_name = "ads"; break;
                case 2: aim_state_name = "leaving_ads"; break;
                case 3: aim_state_name = "hip"; break;
                default: break;
                }
            }
            std::string aobj;
            {
                JsonFields ajf(aobj);
                if (aim.normal_degrees.has_value()) { ajf.f("normal_deg", *aim.normal_degrees, 4); }
                else { ajf.n("normal_deg"); }
                if (aim.zoomed_degrees.has_value()) { ajf.f("zoomed_deg", *aim.zoomed_degrees, 4); }
                else { ajf.n("zoomed_deg"); }
                // Renamed from "flag": it is the FOV lever specifically. Freezing it stops the zoom while
                // recoil stays ADS-light, which is what separates it from the state below.
                if (aim_flag.has_value()) { ajf.b("fov_flag", *aim_flag); } else { ajf.n("fov_flag"); }
                if (aim_state.has_value()) { ajf.u("state", *aim_state); } else { ajf.n("state"); }
                ajf.s("state_name", aim_state_name);
                if (aim_zoomed.has_value()) { ajf.b("uses_zoomed_limit", *aim_zoomed); }
                else { ajf.n("uses_zoomed_limit"); }
                if (aim_frac.has_value()) { ajf.f("zoom_fraction", static_cast<double>(*aim_frac), 4); }
                else { ajf.n("zoom_fraction"); }
                // WHICH LIMIT IS IN FORCE, by the engine's own test.
                if (aim_zoomed.has_value()) {
                    const auto in_force = *aim_zoomed ? aim.zoomed_degrees : aim.normal_degrees;
                    if (in_force.has_value()) { ajf.f("limit_deg", static_cast<double>(*in_force), 4); }
                    else { ajf.n("limit_deg"); }
                } else {
                    ajf.n("limit_deg");
                }
            }
            jf.raw("aim", aobj);
        }

        // ---- THE RENDER / VIEW BLOCK ------------------------------------------------------------
        //
        // The single most VR-relevant thing the SDK maps, and it was reachable only as a raw dump: the live
        // FOV pair, the render rect and the cinematic flag. A mod that fights a scripted camera or writes a
        // projection needs all three, and a human diagnosing "why is the view wrong" needs them first.
        //
        // The DERIVATION IS CARRIED AS A FLAG rather than hidden. camera_fov() reads a float pair off the pose
        // holder; fov_{x,y}_matches_projection() independently derives the same angles from the projection
        // matrix by walking the shader-parameter list. Live those two do NOT agree, so the pair is reported
        // with fov_cross_checked = false. Presenting the numbers without that would turn an open question into
        // an apparent fact, which is exactly how the aim selector got mis-mapped in the first place.
        {
            const auto cfv = sdk::PlayerMgr::camera_fov(0);
            const auto fy = sdk::PlayerMgr::fov_y_matches_projection(0);
            const auto fx = sdk::PlayerMgr::fov_x_matches_projection(0);
            const auto ar = sdk::PlayerMgr::aspect_ratio(0);
            const auto vr = sdk::PlayerMgr::viewport_rect(0);
            const auto cin = sdk::PlayerMgr::cinematic_active(0);
            std::string robj;
            {
                JsonFields rjf(robj);
                constexpr double kRad2Deg = 57.29577951;
                if (cfv.has_value()) {
                    rjf.f("fov_y_deg", static_cast<double>(cfv->fov_y) * kRad2Deg, 3);
                    rjf.f("fov_x_deg", static_cast<double>(cfv->fov_x) * kRad2Deg, 3);
                } else {
                    rjf.n("fov_y_deg").n("fov_x_deg");
                }
                // TRUE only when BOTH independent derivations agree with the read pair.
                rjf.b("fov_cross_checked",
                      fy.has_value() && *fy && fx.has_value() && *fx);
                if (ar.has_value()) { rjf.f("aspect", static_cast<double>(*ar), 4); } else { rjf.n("aspect"); }
                if (vr.has_value()) {
                    rjf.u("rect_w", static_cast<size_t>(vr->width));
                    rjf.u("rect_h", static_cast<size_t>(vr->height));
                } else {
                    rjf.n("rect_w").n("rect_h");
                }
                // A SCRIPTED VIEW IS NOT A BUG BUT IT IS NOT YOURS EITHER -- a VR consumer must not fight it.
                if (cin.has_value()) { rjf.b("cinematic_active", *cin); } else { rjf.n("cinematic_active"); }
            }
            jf.raw("render", robj);
        }

        }
        top.raw("camera", obj);
    }

    return out;
}

// ---- GET /api/subsystems ---------------------------------------------------------------
std::string build_api_subsystems_json() {
    std::string out;
    JsonFields top(out);
    top.b("ok", true);
    top.hex("player", sdk::PlayerMgr::slot(0).value_or(0));

    std::string items = "[";
    bool first = true;
    for (const auto& s : sdk::PlayerMgr::subsystem_slots(0)) {
        std::string item;
        {
            JsonFields jf(item);
            jf.i("offset", static_cast<long long>(s.offset));
            jf.hex("object", s.object);
            jf.hex("vtable", s.vtable);
            jf.raw("ctor", webapi_hex6_json(s.ctor));
            jf.u("size_lower_bound", s.size_lower_bound);
            if (s.name != nullptr) { jf.s("name", s.name); } else { jf.n("name"); }
            jf.b("is_class_instance", s.is_class_instance);
            jf.b("owner_is_player", s.owner_is_player);
            const uint32_t delegate_nodes =
                s.size_lower_bound > 0
                    ? static_cast<uint32_t>(sdk::Delegates::owned_nodes(s.object, s.size_lower_bound).size())
                    : 0;
            jf.u("delegate_nodes", delegate_nodes);
        }
        if (!first) { items += ','; }
        first = false;
        items += item;
    }
    items += ']';
    top.raw("items", items);
    return out;
}

// ---- GET /api/db/categories -------------------------------------------------------------
std::string build_api_db_categories_json(const WebApiQuery& q) {
    std::string out;
    JsonFields top(out);
    const auto* db = webapi_database_handle();
    if (db == nullptr) {
        top.b("ok", false);
        top.s("error", "database not resolved");
        return out;
    }
    top.b("ok", true);

    const size_t limit = webapi_clamp_limit(webapi_query_int(q, "limit", 100));
    const size_t offset = webapi_clamp_offset(webapi_query_int(q, "offset", 0));
    const std::string filter = webapi_query_string(q, "filter");

    const size_t total_categories = sdk::DatabaseMgr::category_count(db);
    std::string items = "[";
    bool first = true;
    size_t matched = 0;
    for (size_t i = 0; i < total_categories; ++i) {
        auto* cat = sdk::DatabaseMgr::category(db, i);
        if (cat == nullptr) { continue; }
        const std::string name = sdk::DatabaseMgr::category_name(cat);
        if (!webapi_contains_ci(name, filter)) { continue; }
        const size_t match_index = matched++;
        if (match_index < offset || match_index >= offset + limit) { continue; }

        std::string item;
        {
            JsonFields jf(item);
            jf.i("index", static_cast<long long>(i));
            jf.s("name", name);
            const auto hash = sdk::DatabaseMgr::hash_name(name);
            if (hash.has_value()) { jf.u("name_hash", *hash); } else { jf.n("name_hash"); }
            jf.i("record_count", static_cast<long long>(sdk::DatabaseMgr::record_count(cat)));
            jf.b("keyed", sdk::DatabaseMgr::name_is_unique_key(cat));
            jf.i("distinct_names", static_cast<long long>(sdk::DatabaseMgr::distinct_name_count(cat)));
        }
        if (!first) { items += ','; }
        first = false;
        items += item;
    }
    items += ']';
    top.i("total", static_cast<long long>(matched));
    top.i("offset", static_cast<long long>(offset));
    top.raw("items", items);
    return out;
}

// ---- GET /api/db/records -----------------------------------------------------------------
//
// Safe on _Structures (18653 records, 279 distinct names): with no filter the total is the
// category's own record_count() (O(1)) and only the requested [offset, offset+limit) window
// is ever read; with a filter every name must be read to know the true match total, but only
// up to `limit` matching records are ever turned into JSON.
std::string build_api_db_records_json(const WebApiQuery& q) {
    std::string out;
    JsonFields top(out);
    const auto* db = webapi_database_handle();
    if (db == nullptr) {
        top.b("ok", false);
        top.s("error", "database not resolved");
        return out;
    }

    const std::string category_q = webapi_query_string(q, "category");
    auto* cat = sdk::DatabaseMgr::find_category(db, category_q);
    if (cat == nullptr) {
        top.b("ok", false);
        top.s("error", "category not found");
        return out;
    }

    const size_t limit = webapi_clamp_limit(webapi_query_int(q, "limit", 100));
    const size_t offset = webapi_clamp_offset(webapi_query_int(q, "offset", 0));
    const std::string filter = webapi_query_string(q, "filter");
    const size_t record_total = sdk::DatabaseMgr::record_count(cat);

    top.b("ok", true);
    top.s("category", category_q);

    std::string items = "[";
    bool first = true;
    size_t total = 0;

    auto emit = [&](size_t index, const regenny::DatabaseMgrRecord* rec, const std::string& name) {
        std::string item;
        {
            JsonFields jf(item);
            jf.i("index", static_cast<long long>(index));
            jf.s("name", name);
            const auto hash = sdk::DatabaseMgr::hash_name(name);
            if (hash.has_value()) { jf.u("name_hash", *hash); } else { jf.n("name_hash"); }
            jf.i("attribute_count", static_cast<long long>(sdk::DatabaseMgr::attribute_count(rec)));
        }
        if (!first) { items += ','; }
        first = false;
        items += item;
    };

    if (filter.empty()) {
        total = record_total;
        const size_t end = std::min(record_total, offset + limit);
        for (size_t i = offset; i < end; ++i) {
            auto* rec = sdk::DatabaseMgr::record(cat, i);
            if (rec == nullptr) { continue; }
            emit(i, rec, sdk::DatabaseMgr::record_name(rec));
        }
    } else {
        size_t matched = 0;
        for (size_t i = 0; i < record_total; ++i) {
            auto* rec = sdk::DatabaseMgr::record(cat, i);
            if (rec == nullptr) { continue; }
            const std::string name = sdk::DatabaseMgr::record_name(rec);
            if (!webapi_contains_ci(name, filter)) { continue; }
            const size_t match_index = matched++;
            if (match_index >= offset && match_index < offset + limit) {
                emit(i, rec, name);
            }
        }
        total = matched;
    }
    items += ']';
    top.i("total", static_cast<long long>(total));
    top.i("offset", static_cast<long long>(offset));
    top.raw("items", items);
    return out;
}

// ---- GET /api/db/record -------------------------------------------------------------------
//
// Safe on _Structures: find_record() is a single lookup, never an enumeration, regardless of
// how many of the category's 18653 records share the queried name.
std::string build_api_db_record_json(const WebApiQuery& q) {
    std::string out;
    JsonFields top(out);
    const auto* db = webapi_database_handle();
    if (db == nullptr) {
        top.b("ok", false);
        top.s("error", "database not resolved");
        return out;
    }

    const std::string category_q = webapi_query_string(q, "category");
    const std::string record_q = webapi_query_string(q, "record");
    auto* cat = sdk::DatabaseMgr::find_category(db, category_q);
    if (cat == nullptr) {
        top.b("ok", false);
        top.s("error", "category not found");
        return out;
    }
    auto* rec = sdk::DatabaseMgr::find_record(cat, record_q);
    if (rec == nullptr) {
        top.b("ok", false);
        top.s("error", "record not found");
        return out;
    }

    long long max_elements_req = webapi_query_int(q, "max_elements", 4);
    max_elements_req = std::max<long long>(1, std::min<long long>(max_elements_req, 64));
    const size_t max_elements = static_cast<size_t>(max_elements_req);

    top.b("ok", true);
    top.s("category", category_q);
    top.s("record", record_q);
    const auto name_hash = sdk::DatabaseMgr::hash_name(record_q);
    if (name_hash.has_value()) { top.u("name_hash", *name_hash); } else { top.n("name_hash"); }

    // describe_record() does the type-dispatched rendering (and truncation bookkeeping); this
    // walks attribute_at() in lockstep with it -- both use the SAME skip predicate
    // (a.has_value()) in the SAME order, so described[j] is provably the j-th successfully-read
    // attribute, needed here to also reach attribute_record() for link_target.
    const auto described = sdk::DatabaseMgr::describe_record(rec, max_elements);
    const size_t attr_total = sdk::DatabaseMgr::attribute_count(rec);

    std::string items = "[";
    bool first = true;
    size_t described_index = 0;
    for (size_t i = 0; i < attr_total && described_index < described.size(); ++i) {
        const auto a = sdk::DatabaseMgr::attribute_at(rec, i);
        if (!a.has_value()) { continue; }
        const auto& d = described[described_index++];

        std::string item;
        {
            JsonFields jf(item);
            if (d.name.has_value()) { jf.s("name", *d.name); } else { jf.n("name"); }
            jf.u("name_hash", d.name_hash);
            jf.u("type", d.type);
            jf.s("type_name", webapi_db_type_name(d.type));
            jf.u("num_values", d.num_values);
            std::string rendered = d.value.text;
            if (d.value.truncated) { rendered += ", ..."; }
            jf.s("rendered", rendered);
            std::string link_name;
            if (d.type == sdk::DatabaseMgr::kTypeRecordLink || d.type == sdk::DatabaseMgr::kTypeRecordLinkAlt) {
                auto* linked = sdk::DatabaseMgr::attribute_record(*a, 0);
                if (linked != nullptr) { link_name = sdk::DatabaseMgr::record_name(linked); }
            }
            if (!link_name.empty()) { jf.s("link_target", link_name); } else { jf.n("link_target"); }
        }
        if (!first) { items += ','; }
        first = false;
        items += item;
    }
    items += ']';
    top.raw("attributes", items);
    return out;
}

// ---- GET /api/db/find ---------------------------------------------------------------------
//
// Safe on _Structures: has_attribute() is a per-record binary search over that record's OWN
// (small) attribute list, and the loop stops as soon as `limit` matches are collected --
// describe_record()/find_attribute() (the more expensive calls) only ever run on a MATCH.
// ---- /watch/* : HARDWARE DATA BREAKPOINTS ------------------------------------------------------------
//
// The reply is deliberately verbose about semantics, because the two facts most likely to send a reader down a
// wrong path are properties of the hardware rather than of this code: a data breakpoint reports the instruction
// AFTER the one that touched the memory, and x86 cannot watch reads without also catching writes.
std::string build_watch_json(const std::string& request_target) {
    const WebApiQuery q = webapi_parse_query(request_target);
    const size_t qpos = request_target.find('?');
    const std::string route = qpos == std::string::npos ? request_target : request_target.substr(0, qpos);

    auto& wp = Watchpoints::get();

    // Attribution as a nested object, reused for the hit itself and for every plausible return address.
    auto addr_json = [](uintptr_t a) {
        std::string out;
        {
            JsonFields jf(out);
            jf.hex("address", a);
            const auto info = Watchpoints::classify(a);
            if (info.known) {
                jf.s("module", info.module).hex("offset", info.offset).hex("static", info.static_address);
            } else {
                jf.n("module").n("offset").n("static");
            }
        }
        return out;
    };

    if (route == "/watch/arm") {
        const std::string addr_s = webapi_query_string(q, "addr");
        if (addr_s.empty()) {
            return "{\"ok\":false,\"error\":\"addr is required, e.g. /watch/arm?addr=0x1C6A5C40&size=4\"}";
        }
        const uintptr_t address = static_cast<uintptr_t>(strtoull(addr_s.c_str(), nullptr, 0));
        const auto size = static_cast<uint8_t>(webapi_query_int(q, "size", 4));
        const uint64_t max_hits = static_cast<uint64_t>(webapi_query_int(q, "max_hits", 4000));

        const std::string type = webapi_query_string(q, "type");
        auto access = Watchpoints::Access::Write;
        std::string note;
        if (type == "exec" || type == "execute") {
            access = Watchpoints::Access::Execute;
        } else if (type == "rw" || type == "readwrite") {
            access = Watchpoints::Access::ReadWrite;
        } else if (type == "read") {
            // Told, not silently substituted. x86 encodes execute, write, and read-or-write; there is no
            // read-only data breakpoint, so a caller asking for reads gets writes too whether or not they know.
            access = Watchpoints::Access::ReadWrite;
            note = "x86 has no read-only data breakpoint; armed as read-or-write, so writes are caught too";
        }

        const auto res = wp.arm(address, size, access, max_hits);
        std::string out;
        {
            JsonFields jf(out);
            jf.b("ok", res.ok);
            if (!res.ok) {
                jf.s("error", res.error);
            } else {
                jf.i("slot", res.slot)
                  .hex("address", address)
                  // THE EFFECTIVE length, not the requested one: an execute watch is forced to a
                  // single byte, and echoing the request would misdescribe the hardware.
                  .u("size", res.effective_size)
                  .u("threads_applied", res.threads_applied)
                  .u("max_hits", static_cast<size_t>(max_hits));
            }
            if (!note.empty()) {
                jf.s("note", note);
            }
        }
        return out;
    }

    if (route == "/watch/clear") {
        const bool all = webapi_query_int(q, "all", 0) != 0;
        if (all) {
            wp.disarm_all();
        } else {
            wp.disarm(static_cast<int>(webapi_query_int(q, "slot", -1)));
        }
        return std::string{"{\"ok\":true,\"cleared\":"} + (all ? "\"all\"" : "\"slot\"") + "}";
    }

    // Bare /watch and /watch/report both report.
    std::string slots_json = "[";
    const auto slots = wp.slots();
    for (size_t i = 0; i < slots.size(); ++i) {
        if (i != 0) {
            slots_json += ',';
        }
        std::string one;
        {
            JsonFields jf(one);
            jf.u("slot", i).b("armed", slots[i].armed);
            if (slots[i].armed || slots[i].hits != 0) {
                const char* kind = slots[i].access == Watchpoints::Access::Execute
                                       ? "exec"
                                       : (slots[i].access == Watchpoints::Access::ReadWrite ? "rw" : "write");
                jf.hex("address", slots[i].address)
                  .u("size", slots[i].size)
                  .s("type", kind)
                  .u("hits", static_cast<size_t>(slots[i].hits))
                  .u("max_hits", static_cast<size_t>(slots[i].max_hits))
                  .b("auto_disarmed", slots[i].auto_disarmed)
                  .u("threads_applied", slots[i].threads_applied);
            }
        }
        slots_json += one;
    }
    slots_json += ']';

    // NAMED `accessors`, NOT `hits`. It held "hits" and collided with the per-slot COUNT also called "hits":
    // one a number, one an array, same key in one document. A consumer parsing "hits" by find() got whichever
    // came first and silently read a `[` as a number -- which is exactly how this went wrong the first time.
    // The name is also more accurate: these are DISTINCT accessing instructions, not individual traps.
    std::string hits_json = "[";
    const auto hits = wp.hits();
    for (size_t i = 0; i < hits.size(); ++i) {
        if (i != 0) {
            hits_json += ',';
        }
        const auto& hit = hits[i];
        std::string one;
        {
            JsonFields jf(one);
            jf.u("slot", hit.slot).u("count", static_cast<size_t>(hit.count)).u("thread", hit.thread_id);
            // NAMED FOR WHAT IT IS. For a data watch this is the instruction AFTER the accessor, so a reader who
            // disassembles here sees the wrong line unless the name tells them. `is_fault` marks the execute
            // case, where the address IS the instruction.
            jf.b("is_fault", hit.is_fault).raw(hit.is_fault ? "instruction" : "eip_after", addr_json(hit.eip_after));
            std::string regs;
            {
                JsonFields r(regs);
                r.hex("eax", hit.eax).hex("ebx", hit.ebx).hex("ecx", hit.ecx).hex("edx", hit.edx);
                r.hex("esi", hit.esi).hex("edi", hit.edi).hex("ebp", hit.ebp).hex("esp", hit.esp);
            }
            jf.raw("regs", regs);
            // ECX SEPARATELY, because __thiscall puts `this` there and identifying the OBJECT is the entire
            // reason a breakpoint beats an offset scan: it distinguishes "writes +144 on something" from
            // "writes +144 on THE camera".
            jf.raw("ecx_object", addr_json(hit.ecx));
            if (hit.value_size != 0) {
                std::string v;
                for (uint8_t k = 0; k < hit.value_size; ++k) {
                    char tmp[4];
                    snprintf(tmp, sizeof(tmp), "%02X", hit.value[k]);
                    v += tmp;
                }
                jf.s("value_at_trap", v);
                if (hit.value_size == 4) {
                    float as_float = 0.0f;
                    memcpy(&as_float, hit.value.data(), sizeof(as_float));
                    if (std::isfinite(as_float)) {
                        jf.f("value_as_float", static_cast<double>(as_float), 6);
                    } else {
                        jf.n("value_as_float");
                    }
                }
            }
            // CALLERS. Raw stack words filtered to those a tracked module owns -- a heuristic, so they are
            // labelled candidates rather than a call stack. Frame pointers are omitted in this build, which is
            // why a scan is the only option that works at all.
            std::string callers = "[";
            size_t kept = 0;
            for (size_t k = 0; k < hit.stack.size() && kept < 8; ++k) {
                const uintptr_t v = hit.stack[k];
                if (v == 0) {
                    continue;
                }
                const auto info = Watchpoints::classify(v);
                if (!info.known) {
                    continue;
                }
                if (kept != 0) {
                    callers += ',';
                }
                callers += addr_json(v);
                ++kept;
            }
            callers += ']';
            jf.raw("caller_candidates", callers);
        }
        hits_json += one;
    }
    hits_json += ']';

    std::string out;
    {
        JsonFields jf(out);
        jf.b("ok", wp.handler_registered());
        jf.b("handler_registered", wp.handler_registered());
        uint64_t total = 0;
        for (const auto& s2 : slots) {
            total += s2.hits;
        }
        jf.raw("slots", slots_json);
        // A TOP-LEVEL TOTAL, so a caller never has to reach into the slot array to answer "did anything happen".
        // Its absence is what pushed the fixture into parsing an ambiguous key.
        jf.u("total_hits", static_cast<size_t>(total));
        jf.raw("accessors", hits_json);
        jf.u("distinct_accessors", hits.size());
        jf.s("semantics",
             "data watches are traps: eip_after is the instruction FOLLOWING the accessor; x86 cannot watch "
             "reads without writes; four hardware slots total, per thread");
    }
    return out;
}

std::string build_api_db_find_json(const WebApiQuery& q) {
    std::string out;
    JsonFields top(out);
    const auto* db = webapi_database_handle();
    if (db == nullptr) {
        top.b("ok", false);
        top.s("error", "database not resolved");
        return out;
    }

    const std::string attribute_q = webapi_query_string(q, "attribute");
    const size_t limit = webapi_clamp_limit(webapi_query_int(q, "limit", 50));

    top.b("ok", true);
    top.s("attribute", attribute_q);
    const auto hash = sdk::DatabaseMgr::hash_name(attribute_q);
    if (hash.has_value()) { top.u("hash", *hash); } else { top.n("hash"); }

    std::string items = "[";
    bool first = true;
    size_t scanned = 0;
    size_t collected = 0;
    if (!attribute_q.empty()) {
        const size_t ncat = sdk::DatabaseMgr::category_count(db);
        for (size_t ci = 0; ci < ncat && collected < limit; ++ci) {
            auto* cat = sdk::DatabaseMgr::category(db, ci);
            if (cat == nullptr) { continue; }
            const size_t nrec = sdk::DatabaseMgr::record_count(cat);
            for (size_t ri = 0; ri < nrec && collected < limit; ++ri) {
                auto* rec = sdk::DatabaseMgr::record(cat, ri);
                if (rec == nullptr) { continue; }
                ++scanned;
                if (!sdk::DatabaseMgr::has_attribute(rec, attribute_q)) { continue; }
                const auto attr = sdk::DatabaseMgr::find_attribute(rec, attribute_q);
                if (!attr.has_value()) { continue; }
                std::string rendered;
                for (const auto& d : sdk::DatabaseMgr::describe_record(rec, 4)) {
                    if (d.name_hash == attr->name_hash) {
                        rendered = d.value.text;
                        if (d.value.truncated) { rendered += ", ..."; }
                        break;
                    }
                }
                std::string item;
                {
                    JsonFields jf(item);
                    jf.s("category", sdk::DatabaseMgr::category_name(cat));
                    jf.s("record", sdk::DatabaseMgr::record_name(rec));
                    jf.u("type", attr->type);
                    jf.s("rendered", rendered);
                }
                if (!first) { items += ','; }
                first = false;
                items += item;
                ++collected;
            }
        }
    }
    items += ']';
    top.i("total_scanned", static_cast<long long>(scanned));
    top.raw("items", items);
    return out;
}

// ---- GET /api/console ----------------------------------------------------------------------
std::string build_api_console_json(const WebApiQuery& q) {
    std::string out;
    JsonFields top(out);
    top.b("ok", true);

    const std::string filter = webapi_query_string(q, "filter");
    const size_t limit = webapi_clamp_limit(webapi_query_int(q, "limit", 100));

    std::string items = "[";
    bool first = true;
    size_t matched = 0;
    for (const auto& cmd : sdk::Console::all()) {
        if (!webapi_contains_ci(cmd.name, filter)) { continue; }
        const size_t idx = matched++;
        if (idx >= limit) { continue; }

        std::string item;
        {
            JsonFields jf(item);
            jf.s("name", cmd.name);
            jf.hex("handler", cmd.handler);
            jf.s("module", cmd.module);
            jf.b("from_exe", cmd.from_exe);
            jf.b("runtime_registered", cmd.registered_at_runtime());
            jf.b("noop", sdk::Console::is_noop(cmd.name).value_or(false));
            const auto* reg = sdk::Console::registrar_of(cmd.name);
            if (reg != nullptr && reg->name != nullptr) { jf.s("registrar", reg->name); }
            else { jf.n("registrar"); }
            if (reg != nullptr) { jf.raw("registrar_offset", webapi_hex6_json(reg->offset)); }
            else { jf.n("registrar_offset"); }
        }
        if (!first) { items += ','; }
        first = false;
        items += item;
    }
    items += ']';
    top.i("total", static_cast<long long>(matched));
    top.raw("items", items);
    return out;
}

// ---- GET /api/vars -------------------------------------------------------------------------
std::string build_api_vars_json(const WebApiQuery& q) {
    std::string out;
    JsonFields top(out);
    top.b("ok", true);

    const std::string filter = webapi_query_string(q, "filter");
    const size_t limit = webapi_clamp_limit(webapi_query_int(q, "limit", 100));

    std::string items = "[";
    bool first = true;
    size_t matched = 0;
    for (const auto& v : sdk::Engine::cached_console_vars()) {
        if (!webapi_contains_ci(v.name, filter)) { continue; }
        const size_t idx = matched++;
        if (idx >= limit) { continue; }

        std::string item;
        {
            JsonFields jf(item);
            jf.s("name", v.name);
            jf.hex("record", v.record);
            const auto value = sdk::Engine::read_cached(v);
            if (value.has_value()) { jf.f("value", *value, 4); } else { jf.n("value"); }

            const auto* def = sdk::Engine::registered_default(v.name);
            const auto at_default = sdk::Engine::is_at_default(v.name);
            if (at_default.has_value()) { jf.b("at_default", *at_default); } else { jf.n("at_default"); }
            if (def != nullptr && def->source == sdk::Engine::DefaultSource::CodeLiteral) {
                jf.f("default", def->value, 4);
            } else {
                jf.n("default");
            }
            if (def != nullptr) {
                jf.s("default_source", def->source == sdk::Engine::DefaultSource::CodeLiteral
                                            ? "code_literal"
                                            : "database_record");
            } else {
                jf.n("default_source");
            }
        }
        if (!first) { items += ','; }
        first = false;
        items += item;
    }
    items += ']';
    top.i("total", static_cast<long long>(matched));
    top.raw("items", items);
    return out;
}

// ---- dispatcher: wired into cmdsrv::Handlers::api in Framework::initialize() -------------
// ---- /sdk/spawns -- WHAT APPEARED AND WHAT LEFT SINCE THE LAST LOOK --------
//
// Drives sdk::ObjectWatch. One watcher PER BUCKET, kept alive between requests,
// because a difference needs somewhere to remember the previous sample: each
// call reports the change since the caller's own last call on that type.
//
// Read-only. It walks buckets through snapshot_objects (POD copied out, no
// engine pointers held), which is exactly what makes it sound to run from the
// IPC thread -- see AGENT.MD rule 6 on thread affinity.
std::string build_spawns_json(const std::string& request_target) {
    const WebApiQuery q = webapi_parse_query(request_target);
    // OT_PARTICLESYSTEM by default: the bucket effects land in.
    const long long type = webapi_query_int(q, "type", 6);

    std::string out;
    JsonFields top(out);

    if (type < 0 || type > 6) {
        top.b("ok", false);
        top.s("error", "type out of range 0..6");
        return out;
    }

    // One watcher per bucket, constructed on first use and outliving the
    // request. Only the IPC thread reaches these, but the mutex is cheap and
    // makes that an enforced property rather than a remembered one.
    static std::mutex s_spawn_mutex;
    static std::vector<std::unique_ptr<sdk::ObjectWatch>> s_watchers;

    std::scoped_lock lock{s_spawn_mutex};

    if (s_watchers.empty()) {
        for (int32_t t = 0; t <= 6; ++t) {
            s_watchers.emplace_back(std::make_unique<sdk::ObjectWatch>(static_cast<sdk::ObjectType>(t)));
        }
    }

    auto& watch = *s_watchers[static_cast<size_t>(type)];

    if (webapi_query_int(q, "reset", 0) != 0) {
        watch.reset();
    }

    const auto present = watch.sample();

    top.b("ok", present.has_value());
    top.i("type", type);
    top.b("primed", watch.primed());
    top.b("truncated", watch.truncated());
    top.u("samples", static_cast<size_t>(watch.samples()));
    if (present.has_value()) {
        top.u("present", *present);
    } else {
        top.n("present");
    }
    top.u("appeared", watch.appeared().size());
    top.u("vanished", watch.vanished().size());

    // WHICH WAY the burst appeared, from the muzzle. This is the SDK's own
    // clustering (ObjectWatch::dominant_bearing), not something recomputed
    // here: a consumer asking "where did that come from" gets the same answer
    // the diagnostic reports.
    //
    // Origin defaults to the weapon muzzle, since the first question this route
    // was built to answer is where a shot's impacts land. `origin=view` uses
    // the camera instead.
    {
        const auto player = sdk::CClientShell::local_player(0);
        float origin[3]{};
        bool have_origin = false;

        if (player.has_value()) {
            if (const auto m = sdk::attached_socket(player->object, "flash"); m.has_value()) {
                origin[0] = m->transform.position.x;
                origin[1] = m->transform.position.y;
                origin[2] = m->transform.position.z;
                have_origin = true;
            }
        }

        top.b("origin_resolved", have_origin);

        if (have_origin) {
            top.f("origin_x", origin[0], 3);
            top.f("origin_y", origin[1], 3);
            top.f("origin_z", origin[2], 3);

            if (const auto bearing = watch.dominant_bearing(origin)) {
                top.f("bearing_deg", bearing->radians * 57.29577951308232, 3);
                top.u("bearing_count", bearing->count);
                top.f("bearing_distance", bearing->mean_distance, 2);
            } else {
                top.n("bearing_deg");
                top.n("bearing_count");
                top.n("bearing_distance");
            }
        }
    }

    // The appeared objects themselves, with positions -- the point of the
    // route. Bounded, because a level load appears as hundreds at once and the
    // reply still has to be a reply.
    const size_t limit = webapi_clamp_limit(webapi_query_int(q, "limit", 24));
    std::string items = "[";
    size_t emitted = 0;

    for (const auto& o : watch.appeared()) {
        if (emitted >= limit) {
            break;
        }

        if (emitted != 0) {
            items += ",";
        }

        JsonFields ji(items);
        ji.hex("address", o.address);
        ji.hex("vtable", o.vtable);
        ji.u("handle", static_cast<size_t>(o.handle));
        ji.f("x", o.position[0], 3);
        ji.f("y", o.position[1], 3);
        ji.f("z", o.position[2], 3);
        ++emitted;
    }

    items += "]";
    top.raw("new_objects", items);

    return out;
}

std::string build_api_json(const std::string& request_target) {
    const size_t q = request_target.find('?');
    const std::string route = q == std::string::npos ? request_target : request_target.substr(0, q);
    const WebApiQuery query = webapi_parse_query(request_target);

    if (route == "/api/state") { return build_api_state_json(); }
    if (route == "/api/subsystems") { return build_api_subsystems_json(); }
    if (route == "/api/db/categories") { return build_api_db_categories_json(query); }
    if (route == "/api/db/records") { return build_api_db_records_json(query); }
    if (route == "/api/db/record") { return build_api_db_record_json(query); }
    if (route == "/api/db/find") { return build_api_db_find_json(query); }
    if (route == "/api/console") { return build_api_console_json(query); }
    if (route == "/api/vars") { return build_api_vars_json(query); }
    return "{\"ok\":false,\"error\":\"unknown /api endpoint\"}";
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

    // MODS ARE REGISTERED BEFORE THE FAN-OUT, and after the frame hook so a mod's on_initialize can rely on
    // SDK resolution having happened. ViewHook is the first: it owns CPlayerCamera_ApplyLookDelta, which is the
    // only way to steer the view (writing the rotation fields is reclaimed within a frame -- measured).
    Mods::get().add(&ViewHook::get());
    // Holds simulation on while the desktop window is not active -- see FocusKeeper.hpp. Registered after
    // ViewHook because its input-leak detector reads ViewHook's look counter.
    Mods::get().add(&FocusKeeper::get());
    // Hardware data breakpoints. Registered last so its on_shutdown -- which clears every thread's debug
    // registers and unregisters the vectored handler -- runs while the rest of the mod state is still intact.
    // The frame boundary. Registered before Watchpoints so its hook is installed while the mod list is still
    // being built; retirement order is Hooks::retire()'s problem, not registration order's.
    // Synthetic input. Registered before RenderHook so its on_frame runs in the same post-poll window the
    // whole mechanism depends on -- see SyntheticInput.hpp.
    Mods::get().add(&SyntheticInput::get());
    Mods::get().add(&RenderHook::get());
    // The perspective pass -- the stereo intervention point. Independent of RenderHook: that one brackets the
    // frame, this one configures the view inside it.
    Mods::get().add(&CameraPassHook::get());
    // The head-orientation composition point. Independent of CameraPassHook: that one places the EYES, this
    // one turns the HEAD, and a VR mod needs both.
    Mods::get().add(&HeadTracking::get());
    Mods::get().add(&WeaponAgreement::get());
    Mods::get().add(&HudPassHook::get());
    // Drives a skeleton node directly -- the mechanism a VR hand or weapon rides on.
    Mods::get().add(&BoneControl::get());
    Mods::get().add(&WeaponWheel::get());
    Mods::get().add(&AmmoKeeper::get());
    Mods::get().add(&FrameCapture::get());
    Mods::get().add(&ResourceWatch::get());
    Mods::get().add(&FireRedirect::get());
    // Owns the writer that rotates the first-person rig, so head-look stops swinging the weapon.
    Mods::get().add(&ViewmodelDecouple::get());
    // Closed-loop turning: snap turn and recentre, both of which need a heading rather than a delta.
    Mods::get().add(&TurnController::get());
    // Suppresses head bob, camera sway and shake -- the view motion a headset cannot tolerate.
    Mods::get().add(&Comfort::get());
    // AFTER RenderHook: its on_initialize registers a present callback, so the hook must exist first.
    Mods::get().add(&ConsoleRunner::get());
    // Owns the VR runtime and pushes its poses into the engine. Added before Watchpoints so its
    // on_frame runs in the same order every session.
    Mods::get().add(&VR::get());
    Mods::get().add(&Watchpoints::get());
    // Before mods initialize, so a fault during their setup is reported rather than silent.
    exception_handler::install();
    Mods::get().on_initialize();

    cmdsrv::Handlers handlers;
    handlers.health = build_health_fragment;
    handlers.targets = build_targets_json;
    handlers.database = build_database_json;
    handlers.objects = build_objects_json;
    handlers.spawns = build_spawns_json;
    handlers.weapons = build_weapons_json;
    handlers.models = build_models_json;
    handlers.interfaces = build_interfaces_json;
    // READ-ONLY BY DEFAULT. The mutation probes are visible in-game, so observing state never triggers them.
    handlers.shader_params = [] { return build_shader_params_json(false); };
    handlers.write_probe = [] { return build_shader_params_json(true); };
    // THE VIEW OVERRIDE, and it MUTATES. Bounded by a frame countdown inside the mod, so the view returns to
    // the engine on its own; there is nothing to restore because the pose is recomputed every frame.
    handlers.piece = [](const std::string& request_target) {
        const WebApiQuery q = webapi_parse_query(request_target);
        const auto player = sdk::CClientShell::local_player(0);
        const regenny::LTObject* obj = player.has_value() ? player->object : nullptr;

        // Which model: the player by default, or any loaded model matched by path substring,
        // because "hide the arms" usually means the viewmodel rather than the player capsule.
        const std::string mdl = webapi_query_string(q, "model");
        if (!mdl.empty()) {
            const auto found = sdk::find_models(mdl, 1);
            obj = found.empty() ? nullptr : found.front().object;
        }

        size_t changed = 0;
        bool acted = false;
        if (obj != nullptr) {
            if (webapi_query_int(q, "unhide_all", 0) != 0) {
                changed = sdk::model_unhide_all_pieces(obj);
                acted = true;
            } else {
                const std::string name = webapi_query_string(q, "name");
                const bool hide = webapi_query_int(q, "hide", 1) != 0;
                if (!name.empty()) {
                    acted = sdk::model_set_piece_hidden(obj, name.c_str(), hide);
                } else if (q.find("index") != q.end()) {
                    acted = sdk::model_set_piece_hidden(
                        obj, static_cast<size_t>(webapi_query_int(q, "index", 0)), hide);
                }
            }
        }

        // Report every piece with its name and CURRENT hidden state, read back through the
        // engine's own getter -- which is what makes the write verifiable rather than assumed.
        std::string pieces = "[";
        size_t total = 0, hidden_now = 0;
        if (obj != nullptr) {
            total = sdk::model_piece_count(obj).value_or(0);
            for (size_t i = 0; i < total; ++i) {
                const auto nm = sdk::model_piece_name(obj, i);
                const auto hid = sdk::model_piece_hidden(obj, i);
                if (hid.value_or(false)) {
                    ++hidden_now;
                }
                char one[224];
                snprintf(one, sizeof(one), "%s{\"i\":%zu,\"name\":\"%s\",\"hidden\":%s}",
                         pieces.size() > 1 ? "," : "", i,
                         nm.has_value() ? nm->c_str() : "?",
                         hid.value_or(false) ? "true" : "false");
                pieces += one;
            }
        }
        pieces += "]";

        std::string out;
        {
            JsonFields jf(out);
            jf.b("ok", obj != nullptr).b("acted", acted)
              .u("piece_count", total).u("hidden", hidden_now).u("changed", changed)
              .raw("pieces", pieces);
        }
        return out;
    };

    handlers.skeleton = [](const std::string& request_target) {
        const WebApiQuery q = webapi_parse_query(request_target);
        const std::string filter = webapi_query_string(q, "filter");
        std::string nodes = "[";
        std::string sockets = "[";
        size_t node_total = 0, socket_total = 0;
        const auto player = sdk::CClientShell::local_player(0);
        if (player.has_value()) {
            if (const auto sk = sdk::ModelSkeleton::from_object(player->object)) {
                node_total = sk->node_count();
                socket_total = sk->socket_count();
                for (size_t i = 0; i < node_total; ++i) {
                    const auto n = sk->node_name(i);
                    if (!n.has_value()) {
                        continue;
                    }
                    if (!filter.empty() && n->find(filter) == std::string::npos) {
                        continue;
                    }
                    char one[192];
                    snprintf(one, sizeof(one), "%s{\"i\":%zu,\"name\":\"%s\"}",
                             nodes.size() > 1 ? "," : "", i, n->c_str());
                    nodes += one;
                }
                for (size_t i = 0; i < socket_total; ++i) {
                    const auto so = sk->socket(i);
                    if (!so.has_value()) {
                        continue;
                    }
                    // Report the socket's OWNING NODE too: that is the index a mod drives when
                    // it wants the thing riding this socket to move.
                    char one[256];
                    snprintf(one, sizeof(one), "%s{\"i\":%zu,\"name\":\"%s\",\"node\":%zu}",
                             sockets.size() > 1 ? "," : "", i, so->name.c_str(), so->node_index);
                    sockets += one;
                }
            }
        }
        nodes += "]";
        sockets += "]";
        std::string out;
        {
            JsonFields jf(out);
            jf.b("ok", node_total > 0).u("node_count", node_total).u("socket_count", socket_total)
              .raw("nodes", nodes).raw("sockets", sockets);
        }
        return out;
    };

    handlers.comfort = [](const std::string& request_target) {
        const WebApiQuery q = webapi_parse_query(request_target);
        auto& cf = Comfort::get();
        if (q.find("on") != q.end()) {
            cf.set_suppressed(webapi_query_int(q, "on", 0) != 0);
        }
        if (webapi_query_int(q, "reset", 0) != 0) {
            CameraPassHook::get().reset_height_excursion();
        }
        const auto o = cf.observed();
        std::string out;
        {
            JsonFields jf(out);
            jf.b("ok", o.found > 0 || !o.suppressed).b("suppressed", o.suppressed)
              .u("known", static_cast<size_t>(o.known)).u("found", static_cast<size_t>(o.found))
              .u("missing", static_cast<size_t>(o.missing))
              .u("applied", static_cast<size_t>(o.applied))
              .u("restored", static_cast<size_t>(o.restored))
              .b("bob_scale_readable", o.bob_scale_readable)
              .f("bob_scale", o.bob_scale, 4);
            // The camera's height excursion, accumulated IN PHASE by CameraPassHook -- the only
            // honest way to measure an oscillation the render thread produces.
            const auto cp = CameraPassHook::get().observed();
            jf.f("height_min", cp.height_min, 4).f("height_max", cp.height_max, 4)
              .f("height_pp", cp.height_max - cp.height_min, 4)
              .u("height_samples", static_cast<size_t>(cp.height_samples));
        }
        return out;
    };

    // ---- /xr/* -- DRIVING THE SIMULATED RUNTIME ------------------------------------------
    //
    // This is the headset and the controllers. With no hardware reachable at 32-bit, these routes
    // ARE the tracking system: a pose set here is what the runtime reports, and what the VR mod
    // then pushes into the engine. Angles in DEGREES for the same reason every other route uses
    // them -- a human reads them, and the radian conversion belongs in one place.
    handlers.xr = [](const std::string& request_target) {
        const WebApiQuery q = webapi_parse_query(request_target);
        const std::string route = request_target.substr(0, request_target.find('?'));
        auto& rt = vr::simulated_runtime();
        auto& mod = VR::get();
        constexpr double kDeg = 3.14159265358979 / 180.0;
        // The route below refuses a bad direction rather than normalizing it; this
        // carries that refusal into the response, since this handler answers with a
        // state document instead of an HTTP status.
        bool fire_aim_refused = false;
        bool ammo_floor_refused = false;
        bool capture_armed = false;

        if (route == "/xr/enable") {
            mod.set_enabled(webapi_query_int(q, "on", 1) != 0);
        } else if (route == "/xr/reset") {
            rt.reset();
        } else if (route == "/xr/ammo") {
            // Keep the player stocked (see mods/AmmoKeeper.hpp). Off by default; the
            // suite arms it around firing blocks so a drained pool cannot make an
            // unrelated check go red.
            auto& ak = AmmoKeeper::get();
            if (webapi_query_int(q, "on", 1) == 0) {
                ak.disable();
            } else {
                ammo_floor_refused = !ak.set_floor(webapi_query_int(q, "floor", 500));
            }
        } else if (route == "/xr/capture") {
            // Read the finished frame back off the GPU. `path=` writes a BMP; without it the
            // capture is timing-only. This is the project's first visual oracle that cannot be
            // stale -- it samples the buffer the engine is about to present, on the render
            // thread, in phase with it.
            if (q.find("mirror") != q.end()) {
                FrameCapture::get().set_gpu_mirror(webapi_query_int(q, "mirror", 0) != 0);
            }
            if (webapi_query_int(q, "verify_mirror", 0) != 0) {
                FrameCapture::get().request_gpu_mirror_verify();
            }
            if (q.find("stage") != q.end()) {
                const auto st = webapi_query_string(q, "stage");
                FrameCapture::get().set_stage(st == "second_eye"
                                                  ? FrameCapture::Stage::AfterSecondEye
                                                  : FrameCapture::Stage::Present);
            }
            if (q.find("continuous") != q.end()) {
                FrameCapture::get().set_continuous(webapi_query_int(q, "continuous", 0) != 0);
            }
            if (q.find("divisor") != q.end()) {
                FrameCapture::get().set_divisor(static_cast<uint32_t>(webapi_query_int(q, "divisor", 1)));
            }
            // A ONE-SHOT IS A SEPARATE COMMAND FROM THE CONTINUOUS TOGGLE. Requesting one on every
            // call meant "?continuous=0" also armed a capture, which left a one-shot pending and
            // silently blocked the callback release that stopping continuous is supposed to
            // perform. The occupancy stayed at 4 with nothing in the log to say why.
            if (q.find("continuous") == q.end()) {
                const std::string path = webapi_query_string(q, "path");
                capture_armed = FrameCapture::get().request_capture_to(path);
            }
        } else if (route == "/xr/resources") {
            // What the engine allocates, and out of which D3D pool -- the D3D9Ex gate.
            // `reset=1` starts a fresh window, which is how a caller bounds "what did THIS
            // level load create" rather than reading a total since injection.
            if (webapi_query_int(q, "reset", 0) != 0) {
                ResourceWatch::get().reset_counts();
            }
        } else if (route == "/xr/fire-origin") {
            // Start the ray at the weapon's muzzle instead of the player's eye.
            // Independent of the aim mode on purpose: it changes what the shot can
            // clip past, which a caller should opt into separately.
            FireRedirect::get().set_origin_from_weapon(webapi_query_int(q, "on", 1) != 0);
        } else if (route == "/xr/fire-controller") {
            // Aim the shot along the CONTROLLER. Unlike weapon mode this uses the pose
            // we command rather than one read back from the engine's rig, so it carries
            // pitch -- the rig's does not.
            auto& fr = FireRedirect::get();
            if (webapi_query_int(q, "on", 1) == 0) {
                fr.set_mode(FireRedirect::Mode::Off);
            } else {
                fr.set_hotkey(webapi_query_int(q, "vk", 0));
                fr.set_mode(FireRedirect::Mode::Controller);
            }
        } else if (route == "/xr/fire-weapon") {
            // SHOTS FOLLOW THE GUN. With BoneControl driving the weapon bone from the
            // controller this is hand-aimed shooting: the muzzle points where the hand
            // does, and the shot now goes there too instead of following the view.
            auto& fr = FireRedirect::get();
            if (webapi_query_int(q, "on", 1) == 0) {
                fr.set_mode(FireRedirect::Mode::Off);
            } else {
                fr.set_hotkey(webapi_query_int(q, "vk", 0));
                fr.set_mode(FireRedirect::Mode::Weapon);
            }
        } else if (route == "/xr/fire-reverse") {
            // HOLD-TO-SHOOT-BACKWARDS. The one experiment that can tell whether the
            // direction in the client's fire message decides where a shot lands:
            // point at an enemy, hold the key, fire. Damage is server-authoritative,
            // so a target that still dies proves the field is not the trace's input,
            // and one that stops taking damage proves it is.
            auto& fr = FireRedirect::get();
            if (webapi_query_int(q, "on", 1) == 0) {
                fr.set_mode(FireRedirect::Mode::Off);
            } else {
                fr.set_hotkey(webapi_query_int(q, "vk", 0x60)); // numpad 0 by default
                fr.set_mode(FireRedirect::Mode::Reverse);
            }
        } else if (route == "/xr/fire-aim") {
            // Redirect the SERVER's fire ray (see mods/FireRedirect.hpp). Takes a
            // world-space unit direction; a non-unit one is refused rather than
            // normalized, so a caller's scaling error shows up here instead of as
            // a subtly wrong aim nobody can see.
            auto& fr = FireRedirect::get();
            if (webapi_query_int(q, "on", 1) == 0) {
                fr.clear_direction();
            } else {
                const auto x = static_cast<float>(webapi_query_double(q, "x", 0.0));
                const auto y = static_cast<float>(webapi_query_double(q, "y", 0.0));
                const auto z = static_cast<float>(webapi_query_double(q, "z", 0.0));
                fire_aim_refused = !fr.set_direction(x, y, z);
            }
        } else if (route == "/xr/trigger") {
            mod.set_trigger_enabled(webapi_query_int(q, "on", 1) != 0);
        } else if (route == "/xr/input") {
            const std::string side = webapi_query_string(q, "side");
            const auto which = (side == "left") ? vr::VRRuntime::Hand::LEFT : vr::VRRuntime::Hand::RIGHT;
            const auto cur = rt.hand(which);
            rt.set_hand_inputs(which,
                               static_cast<float>(webapi_query_double(q, "trigger", cur.trigger)),
                               static_cast<float>(webapi_query_double(q, "squeeze", cur.squeeze)),
                               cur.thumbstick, cur.buttons);
        } else if (route == "/xr/hands") {
            mod.set_hands_enabled(webapi_query_int(q, "on", 1) != 0);
        } else if (route == "/xr/hand") {
            // Controller pose in RUNTIME space, metres. Absolute here; the mapping to a hand
            // OFFSET is a delta taken against the rest pose inside the mod, which is where that
            // policy belongs.
            const std::string side = webapi_query_string(q, "side");
            const auto which = (side == "left") ? vr::VRRuntime::Hand::LEFT : vr::VRRuntime::Hand::RIGHT;
            const auto current = rt.hand(which);

            vr::Pose aim = current.aim;
            aim.position = {static_cast<float>(webapi_query_double(q, "x", aim.position[0])),
                            static_cast<float>(webapi_query_double(q, "y", aim.position[1])),
                            static_cast<float>(webapi_query_double(q, "z", aim.position[2]))};

            // Orientation, when asked for. Absent yaw/pitch/roll leaves the current orientation
            // alone so a caller can move a controller without also levelling it.
            if (q.find("yaw") != q.end() || q.find("pitch") != q.end() || q.find("roll") != q.end()) {
                const double cyaw = webapi_query_double(q, "yaw", 0.0) * kDeg;
                const double cpitch = webapi_query_double(q, "pitch", 0.0) * kDeg;
                const double croll = webapi_query_double(q, "roll", 0.0) * kDeg;
                const double cy = cos(cyaw * 0.5), sy = sin(cyaw * 0.5);
                const double cp = cos(cpitch * 0.5), sp = sin(cpitch * 0.5);
                const double cr = cos(croll * 0.5), sr = sin(croll * 0.5);
                aim.orientation = {
                    static_cast<float>(cy * sp * cr + sy * cp * sr),
                    static_cast<float>(sy * cp * cr - cy * sp * sr),
                    static_cast<float>(cy * cp * sr - sy * sp * cr),
                    static_cast<float>(cy * cp * cr + sy * sp * sr)};
            }

            aim.valid = true;
            aim.tracked = true;
            rt.set_hand_pose(which, aim, aim);
        } else if (route == "/xr/head") {
            // Yaw/pitch/roll in RUNTIME space, converted to a quaternion here so a caller never
            // has to build one by hand. Order is yaw * pitch * roll, which matches how a headset
            // is naturally described (turn, then look up, then tilt).
            const double yaw = webapi_query_double(q, "yaw", 0.0) * kDeg;
            const double pitch = webapi_query_double(q, "pitch", 0.0) * kDeg;
            const double roll = webapi_query_double(q, "roll", 0.0) * kDeg;

            const double cy = cos(yaw * 0.5), sy = sin(yaw * 0.5);
            const double cp = cos(pitch * 0.5), sp = sin(pitch * 0.5);
            const double cr = cos(roll * 0.5), sr = sin(roll * 0.5);

            vr::Pose pose{};
            pose.orientation = {
                static_cast<float>(cy * sp * cr + sy * cp * sr),
                static_cast<float>(sy * cp * cr - cy * sp * sr),
                static_cast<float>(cy * cp * sr - sy * sp * cr),
                static_cast<float>(cy * cp * cr + sy * sp * sr)};
            pose.position = {static_cast<float>(webapi_query_double(q, "x", 0.0)),
                             static_cast<float>(webapi_query_double(q, "y", 1.7)),
                             static_cast<float>(webapi_query_double(q, "z", 0.0))};
            pose.valid = true;
            pose.tracked = true;
            rt.set_head_pose(pose);
        }

        const auto st = mod.state();
        const auto head = rt.head();
        std::string out;
        {
            JsonFields jf(out);
            jf.b("ok", true)
              .s("runtime", st.runtime_name)
              .b("enabled", st.enabled)
              .b("head_valid", st.head_valid)
              .u("frames", static_cast<size_t>(st.runtime_frames))
              .u("applied", static_cast<size_t>(st.applied))
              .f("head_rt_x", st.head_runtime[0], 5).f("head_rt_y", st.head_runtime[1], 5)
              .f("head_rt_z", st.head_runtime[2], 5).f("head_rt_w", st.head_runtime[3], 5)
              .f("head_eng_x", st.head_engine[0], 5).f("head_eng_y", st.head_engine[1], 5)
              .f("head_eng_z", st.head_engine[2], 5).f("head_eng_w", st.head_engine[3], 5)
              .f("head_pos_y", head.position[1], 4)
              .b("hands", st.hands)
              .u("hand_applied", static_cast<size_t>(st.hand_applied))
              .f("hand_off_x", st.hand_offset[0], 3)
              .f("hand_off_y", st.hand_offset[1], 3)
              .f("hand_off_z", st.hand_offset[2], 3)
              .f("hand_rot_x", st.hand_rotation[0], 5)
              .f("hand_rot_y", st.hand_rotation[1], 5)
              .f("hand_rot_z", st.hand_rotation[2], 5)
              .f("hand_rot_w", st.hand_rotation[3], 5)
              .b("trigger_armed", st.trigger).b("firing", st.firing)
              .u("pulls", static_cast<size_t>(st.pulls))
              .f("ammo_total", static_cast<double>(sdk::PlayerMgr::ammo_total(0).value_or(-1)), 0)
              .f("aim_yaw_deg", sdk::PlayerMgr::aim_yaw(0).value_or(0.0f) * 57.2957795, 4)
              .f("aim_pitch_deg", sdk::PlayerMgr::aim_pitch(0).value_or(0.0f) * 57.2957795, 4);

            // WHERE THE VIEW IS LOOKING, signed and in world terms. The aim's own yaw/pitch above
            // are the BODY's; these are the composed head pose's, and the difference between them
            // is the whole feature. Signed because a magnitude cannot catch an inverted axis --
            // the classic way a handedness bug survives review is that yaw looks right while
            // pitch is upside down, and only a sign shows it.
            if (const auto d = sdk::PlayerMgr::aim_vs_view(0)) {
                const float fx = d->view_forward[0];
                const float fy = d->view_forward[1];
                const float fz = d->view_forward[2];
                jf.b("view_readable", true)
                  .f("view_yaw_deg", atan2f(fx, fz) * 57.2957795f, 4)
                  .f("view_pitch_deg", asinf(fy < -1.0f ? -1.0f : (fy > 1.0f ? 1.0f : fy)) * 57.2957795f, 4);
            } else {
                jf.b("view_readable", false);
            }

            // FIRE REDIRECTION. `fr_calls` counts shots the server actually took
            // while we were watching, which is the difference between "the hook is
            // installed" and "the hook is on the path shooting goes through".
            // `fr_engine_*` is what the engine was about to fire before we wrote,
            // so a test can prove replacement rather than coincidence.
            const auto& fr = FireRedirect::get();
            const auto ed = fr.last_engine_dir();
            const auto wd = fr.last_written_dir();
            const auto og = fr.last_origin();
            jf.b("fr_hooked", fr.hooked())
              .b("fr_armed", fr.armed())
              .b("fr_aim_refused", fire_aim_refused)
              .i("fr_mode", static_cast<int>(FireRedirect::get().mode()))
              .i("fr_hotkey", FireRedirect::get().hotkey())
              .b("fr_hotkey_held", FireRedirect::get().hotkey_held())
              .u("fr_redirected_shots", FireRedirect::get().redirected_shots())
              .u("fr_builds", FireRedirect::get().builds())
              .f("fr_built_x", FireRedirect::get().built_dir()[0], 4)
              .f("fr_built_y", FireRedirect::get().built_dir()[1], 4)
              .f("fr_built_z", FireRedirect::get().built_dir()[2], 4)
              .f("fr_bo_x", FireRedirect::get().built_origin()[0], 2)
              .f("fr_bo_y", FireRedirect::get().built_origin()[1], 2)
              .f("fr_bo_z", FireRedirect::get().built_origin()[2], 2)
              .b("fc_armed", capture_armed)
              .b("fc_pending", FrameCapture::get().pending())
              .u("fc_captures", FrameCapture::get().captures())
              .u("fc_failures", FrameCapture::get().failures())
              .i("fc_hresult", FrameCapture::get().last_hresult())
              .f("fc_copy_ms", FrameCapture::get().last_copy_ms(), 3)
              .f("fc_worst_copy_ms", FrameCapture::get().worst_copy_ms(), 3)
              .f("fc_total_ms", FrameCapture::get().last_total_ms(), 3)
              .f("fc_lock_ms", FrameCapture::get().last_lock_ms(), 3)
              .f("fc_stretch_ms", FrameCapture::get().last_stretch_ms(), 3)
              .u("fc_divisor", FrameCapture::get().divisor())
              .b("fc_continuous", FrameCapture::get().continuous())
              .u("fc_cont_frames", FrameCapture::get().continuous_frames())
              .raw("fc_signature",
                   std::to_string(FrameCapture::get().last_signature()).c_str())
              .f("fc_mean_luma", FrameCapture::get().last_mean_luma(), 3)
              .f("fc_left_luma", FrameCapture::get().last_left_luma(), 3)
              .f("fc_right_luma", FrameCapture::get().last_right_luma(), 3)
              // THE GPU-RESIDENT PAIR: the surface a compositor would be handed, its copy cost,
              // and -- after ?verify_mirror=1 -- proof it holds the same picture as the CPU path.
              .b("fc_mirror_on", FrameCapture::get().gpu_mirror())
              .u("fc_mirror_frames", FrameCapture::get().gpu_mirror_frames())
              .f("fc_mirror_copy_ms", FrameCapture::get().last_gpu_copy_ms(), 4)
              .b("fc_mirror_surface", FrameCapture::get().gpu_mirror_surface() != nullptr)
              .f("fc_mirror_left_luma", FrameCapture::get().mirror_left_luma(), 3)
              .f("fc_mirror_right_luma", FrameCapture::get().mirror_right_luma(), 3)
              .b("fc_mirror_verified", FrameCapture::get().mirror_verified())
              .f("fc_mirror_ref_left", FrameCapture::get().mirror_ref_left_luma(), 3)
              .f("fc_mirror_ref_right", FrameCapture::get().mirror_ref_right_luma(), 3)
              .f("fc_cont_lock_ms", FrameCapture::get().continuous_lock_ms(), 3)
              .u("fc_width", FrameCapture::get().width())
              .u("fc_height", FrameCapture::get().height())
              .u("fc_format", FrameCapture::get().format())
              .u("fc_nonblack", FrameCapture::get().nonblack_pixels())
              .u("fc_sampled", FrameCapture::get().sampled_pixels())
              .b("rw_hooked", ResourceWatch::get().hooked())
              .b("rw_observed_any", ResourceWatch::get().observed_any())
              .b("rw_uses_managed", ResourceWatch::get().uses_managed_pool())
              .u("rw_total", ResourceWatch::get().total())
              .u("rw_pool_default", ResourceWatch::get().pool_total(0))
              .u("rw_pool_managed", ResourceWatch::get().pool_total(1))
              .u("rw_pool_sysmem", ResourceWatch::get().pool_total(2))
              .u("rw_pool_scratch", ResourceWatch::get().pool_total(3))
              .u("rw_pool_other", ResourceWatch::get().pool_total(4))
              .u("rw_tex_managed", ResourceWatch::get().count(ResourceWatch::Kind::Texture, 1))
              .u("rw_vb_managed", ResourceWatch::get().count(ResourceWatch::Kind::VertexBuffer, 1))
              .u("rw_ib_managed", ResourceWatch::get().count(ResourceWatch::Kind::IndexBuffer, 1))
              .u("rw_managed_dynamic", ResourceWatch::get().managed_dynamic())
              .u("rw_managed_static", ResourceWatch::get().managed_static())
              .u("rw_managed_rt", ResourceWatch::get().managed_rendertarget())
              .u("rw_largest_edge", ResourceWatch::get().largest_managed_edge())
              .u("rw_distinct_formats", ResourceWatch::get().distinct_formats())
              .b("fr_origin_weapon", FireRedirect::get().origin_from_weapon())
              .b("fr_origin_valid", FireRedirect::get().origin_valid())
              .u("fr_origin_writes", FireRedirect::get().origin_writes())
              .f("fr_wo_x", FireRedirect::get().weapon_origin()[0], 2)
              .f("fr_wo_y", FireRedirect::get().weapon_origin()[1], 2)
              .f("fr_wo_z", FireRedirect::get().weapon_origin()[2], 2)
              .b("fr_weapon_valid", FireRedirect::get().weapon_forward_valid())
              .f("fr_wq_x", FireRedirect::get().weapon_quat()[0], 6)
              .f("fr_wq_y", FireRedirect::get().weapon_quat()[1], 6)
              .f("fr_wq_z", FireRedirect::get().weapon_quat()[2], 6)
              .f("fr_wq_w", FireRedirect::get().weapon_quat()[3], 6)
              .f("fr_woq_x", FireRedirect::get().weapon_object_quat()[0], 6)
              .f("fr_woq_y", FireRedirect::get().weapon_object_quat()[1], 6)
              .f("fr_woq_z", FireRedirect::get().weapon_object_quat()[2], 6)
              .f("fr_woq_w", FireRedirect::get().weapon_object_quat()[3], 6)
              .f("fr_weapon_fx", FireRedirect::get().weapon_forward()[0], 4)
              .f("fr_weapon_fy", FireRedirect::get().weapon_forward()[1], 4)
              .f("fr_weapon_fz", FireRedirect::get().weapon_forward()[2], 4)
              .b("ak_enabled", AmmoKeeper::get().enabled())
              .b("ak_floor_refused", ammo_floor_refused)
              .i("ak_floor", AmmoKeeper::get().floor())
              .u("ak_sweeps", AmmoKeeper::get().sweeps())
              .u("ak_raised_total", AmmoKeeper::get().raised_total())
              .u("ak_last_raised", AmmoKeeper::get().last_raised())
              .u("fr_target", static_cast<unsigned long long>(fr.target()))
              .u("fr_desc", static_cast<unsigned long long>(fr.last_descriptor()))
              .u("fr_caller", static_cast<unsigned long long>(fr.fire_caller()))
              .u("fr_entries", fr.fire_entries())
              .u("fr_messages", fr.messages())
              .b("fr_send_hooked", fr.send_hooked())
              .u("fr_sends", fr.sends())
              .u("fr_send_caller", static_cast<unsigned long long>(fr.send_caller()))
              .f("fr_sent_x", fr.last_sent_dir()[0], 4)
              .f("fr_sent_y", fr.last_sent_dir()[1], 4)
              .f("fr_sent_z", fr.last_sent_dir()[2], 4)
              .f("fr_sent_ox", fr.last_sent_origin()[0], 2)
              .f("fr_sent_oy", fr.last_sent_origin()[1], 2)
              .f("fr_sent_oz", fr.last_sent_origin()[2], 2);
            {
                uintptr_t frames[FireRedirect::kMaxSenderFrames]{};
                const size_t n = fr.sender_frames(frames, FireRedirect::kMaxSenderFrames);
                out += ",\"fr_sender\":[";
                for (size_t i = 0; i < n; ++i) {
                    if (i != 0) { out += ','; }
                    char b[24];
                    snprintf(b, sizeof(b), "%llu", static_cast<unsigned long long>(frames[i]));
                    out += b;
                }
                out += ']';
            }
            jf
              .u("fr_calls", fr.calls())
              .u("fr_writes", fr.writes())
              .f("fr_engine_x", ed[0], 4).f("fr_engine_y", ed[1], 4).f("fr_engine_z", ed[2], 4)
              .f("fr_written_x", wd[0], 4).f("fr_written_y", wd[1], 4).f("fr_written_z", wd[2], 4)
              .f("fr_origin_x", og[0], 3).f("fr_origin_y", og[1], 3).f("fr_origin_z", og[2], 3);
        }
        return out;
    };

    handlers.turn = [](const std::string& request_target) {
        const WebApiQuery q = webapi_parse_query(request_target);
        auto& tc = TurnController::get();
        constexpr double kDeg = 3.14159265358979 / 180.0;
        if (webapi_query_int(q, "cancel", 0) != 0) {
            tc.cancel();
        } else if (webapi_query_int(q, "recentre", 0) != 0 || webapi_query_int(q, "recenter", 0) != 0) {
            tc.recentre();
        } else if (webapi_query_int(q, "level", 0) != 0) {
            tc.level();
        } else if (q.find("pitchby") != q.end()) {
            tc.pitch_by(static_cast<float>(webapi_query_double(q, "pitchby", 0.0) * kDeg));
        } else if (q.find("pitch") != q.end() && q.find("to") != q.end()) {
            // Both axes in one call -- the diagonal a VR mod issues when pointing the view at a
            // direction, rather than two settles in series.
            tc.aim_to(static_cast<float>(webapi_query_double(q, "to", 0.0) * kDeg),
                      static_cast<float>(webapi_query_double(q, "pitch", 0.0) * kDeg));
        } else if (q.find("pitch") != q.end()) {
            tc.pitch_to(static_cast<float>(webapi_query_double(q, "pitch", 0.0) * kDeg));
        } else if (q.find("by") != q.end()) {
            tc.turn_by(static_cast<float>(webapi_query_double(q, "by", 0.0) * kDeg));
        } else if (q.find("to") != q.end()) {
            tc.turn_to(static_cast<float>(webapi_query_double(q, "to", 0.0) * kDeg));
        }
        const auto o = tc.observed();
        const auto yaw = sdk::PlayerMgr::aim_yaw(0);
        std::string out;
        {
            JsonFields jf(out);
            jf.b("ok", true).b("active", o.active).b("converged", o.converged)
              .f("target_deg", o.target * 57.2957795, 4)
              .f("error_deg", o.error * 57.2957795, 4)
              .f("yaw_deg", yaw.value_or(0.0f) * 57.2957795, 4)
              .u("corrections", static_cast<size_t>(o.corrections))
              .u("completed", static_cast<size_t>(o.completed))
              .u("abandoned", static_cast<size_t>(o.abandoned))
              .b("pitch_active", o.pitch_active).b("pitch_converged", o.pitch_converged)
              .b("pitch_clamped", o.pitch_clamped)
              .f("pitch_target_deg", o.pitch_target * 57.2957795, 4)
              .f("pitch_error_deg", o.pitch_error * 57.2957795, 4)
              .f("pitch_deg", sdk::PlayerMgr::aim_pitch(0).value_or(0.0f) * 57.2957795, 4)
              .u("pitch_completed", static_cast<size_t>(o.pitch_completed))
              .u("pitch_abandoned", static_cast<size_t>(o.pitch_abandoned));
        }
        return out;
    };

    handlers.viewmodel = [](const std::string& request_target) {
        const WebApiQuery q = webapi_parse_query(request_target);
        auto& vm = ViewmodelDecouple::get();
        if (q.find("on") != q.end()) {
            vm.set_enabled(webapi_query_int(q, "on", 0) != 0);
        }
        const auto o = vm.observed();
        std::string out;
        {
            JsonFields jf(out);
            jf.b("ok", o.hooked).hex("target", o.target).b("enabled", o.enabled)
              .b("object_resolved", o.object_resolved)
              .u("calls", static_cast<size_t>(o.calls))
              .u("matched", static_cast<size_t>(o.matched))
              .u("corrected", static_cast<size_t>(o.corrected))
              .f("last_correction_deg", o.last_correction * 57.2957795, 4);
        }
        return out;
    };

    handlers.bone = [](const std::string& request_target) {
        const WebApiQuery q = webapi_parse_query(request_target);
        auto& bc = BoneControl::get();
        // `slot=` picks which bone this call drives. Defaulting to 0 keeps every existing
        // caller working; a second hand is `slot=1`, and `detach_all=1` releases the lot.
        const auto slot = static_cast<uint32_t>(webapi_query_int(q, "slot", 0));
        if (webapi_query_int(q, "detach_all", 0) != 0) {
            bc.detach_all();
            for (uint32_t i = 0; i < BoneControl::kSlots; ++i) {
                bc.clear_offset(i);
                bc.clear_rotation(i);
            }
        } else if (webapi_query_int(q, "detach", 0) != 0) {
            bc.detach(slot);
            bc.clear_offset(slot);
            bc.clear_rotation(slot);
        } else {
            // `socket=` is the form a consumer usually wants ("RightHand"); `name=` is a NODE.
            const std::string socket = webapi_query_string(q, "socket");
            const std::string name = webapi_query_string(q, "name");
            if (!socket.empty()) {
                bc.attach_to_player_socket(socket.c_str(), slot);
            } else if (!name.empty()) {
                bc.attach_to_player_node(name.c_str(), slot);
            } else if (q.find("node") != q.end()) {
                bc.attach_to_player_node(static_cast<uint32_t>(webapi_query_int(q, "node", 0)), slot);
            }
            if (q.find("x") != q.end() || q.find("y") != q.end() || q.find("z") != q.end()) {
                bc.set_offset(static_cast<float>(webapi_query_double(q, "x", 0.0)),
                              static_cast<float>(webapi_query_double(q, "y", 0.0)),
                              static_cast<float>(webapi_query_double(q, "z", 0.0)), slot);
            }
            if (q.find("qw") != q.end()) {
                bc.set_rotation(static_cast<float>(webapi_query_double(q, "qx", 0.0)),
                                static_cast<float>(webapi_query_double(q, "qy", 0.0)),
                                static_cast<float>(webapi_query_double(q, "qz", 0.0)),
                                static_cast<float>(webapi_query_double(q, "qw", 1.0)), slot);
            }
        }
        const auto o = bc.observed(slot);
        std::string out;
        {
            JsonFields jf(out);
            jf.b("ok", o.available).u("slot", slot).b("attached", o.attached).b("want_attached", o.want_attached)
              .u("node", static_cast<size_t>(o.node))
              .u("calls", static_cast<size_t>(o.calls))
              .u("writes", static_cast<size_t>(o.writes))
              .u("record_consistent", static_cast<size_t>(o.record_consistent))
              .u("record_inconsistent", static_cast<size_t>(o.record_inconsistent))
              .u("callback_thread", static_cast<size_t>(o.callback_thread))
              .u("frame_thread", static_cast<size_t>(o.frame_thread))
              .b("same_thread", o.same_thread)
              .b("readback_matches", o.readback_matches)
              .u("engine_registered", static_cast<size_t>(o.engine_registered))
              .f("seen_x", o.last_seen_position[0], 4).f("seen_y", o.last_seen_position[1], 4)
              .f("seen_z", o.last_seen_position[2], 4)
              .f("wrote_x", o.last_written_position[0], 4).f("wrote_y", o.last_written_position[1], 4)
              .f("wrote_z", o.last_written_position[2], 4)
              .f("seen_qx", o.last_seen_rotation[0], 5).f("seen_qy", o.last_seen_rotation[1], 5)
              .f("seen_qz", o.last_seen_rotation[2], 5).f("seen_qw", o.last_seen_rotation[3], 5);
        }
        return out;
    };

    handlers.hud = [](const std::string& request_target) {
        const WebApiQuery q = webapi_parse_query(request_target);
        if (webapi_query_int(q, "clear", 0) != 0) {
            HudPassHook::get().clear_offset();
        } else if (q.find("x") != q.end() || q.find("y") != q.end()) {
            HudPassHook::get().set_offset(static_cast<int32_t>(webapi_query_int(q, "x", 0)),
                                          static_cast<int32_t>(webapi_query_int(q, "y", 0)));
        }
        const auto hp = HudPassHook::get().observed();

        // Named rather than bare: an address in a log is a lookup, and the classifier already exists. Built
        // BEFORE the field block, because JsonFields closes the object when it goes out of scope.
        std::string callers = "[";
        for (size_t i = 0; i < hp.callers.size(); ++i) {
            if (hp.callers[i] == 0) {
                continue;
            }
            const auto info = Watchpoints::classify(hp.callers[i]);
            char one[224];
            snprintf(one, sizeof(one),
                     "%s{\"at\":\"0x%08" PRIXPTR "\",\"module\":\"%s\",\"static\":\"0x%08" PRIXPTR
                     "\",\"n\":%u}",
                     callers.size() > 1 ? "," : "", hp.callers[i],
                     info.module.empty() ? "?" : info.module.c_str(), info.static_address,
                     hp.caller_counts[i]);
            callers += one;
        }
        callers += "]";

        std::string out;
        {
            JsonFields jf(out);
            jf.b("ok", hp.stored_hooked)
              .u("passes", static_cast<size_t>(hp.stored_passes))
              .u("passes_last_frame", static_cast<size_t>(hp.stored_last_frame))
              .b("ortho", hp.stored_ortho)
              .b("gate", hp.offset_gate).b("gate_read", hp.offset_read)
              .b("armed", hp.offset_armed).u("writes", static_cast<size_t>(hp.offset_writes))
              .u("distinct_callers", static_cast<size_t>(hp.distinct_callers))
              .raw("callers", callers)

              .i("req_x", hp.offset_requested[0]).i("req_y", hp.offset_requested[1])
              .i("stored_x", hp.offset_stored[0]).i("stored_y", hp.offset_stored[1])
              .i("effective_x", hp.offset_effective[0]).i("effective_y", hp.offset_effective[1])
              .i("vp_left", hp.stored_viewport[0]).i("vp_top", hp.stored_viewport[1])
              .i("vp_right", hp.stored_viewport[2]).i("vp_bottom", hp.stored_viewport[3]);
        }
        return out;
    };

    handlers.head = [](const std::string& request_target) {
        const WebApiQuery q = webapi_parse_query(request_target);
        auto& ht = HeadTracking::get();
        if (webapi_query_int(q, "clear", 0) != 0) {
            ht.clear();
        } else if (q.find("yaw") != q.end() || q.find("pitch") != q.end() ||
                   q.find("roll") != q.end() || q.find("w") != q.end()) {
            // A quaternion is the honest interface -- a runtime hands you one -- but yaw/pitch in DEGREES is
            // what a person testing by hand can reason about, so both are accepted.
            if (q.find("w") != q.end()) {
                ht.set_head_rotation({static_cast<float>(webapi_query_double(q, "x", 0.0)),
                                      static_cast<float>(webapi_query_double(q, "y", 0.0)),
                                      static_cast<float>(webapi_query_double(q, "z", 0.0)),
                                      static_cast<float>(webapi_query_double(q, "w", 1.0))});
            } else {
                // AXES MEASURED, NOT ASSUMED: x = roll, y = yaw, z = pitch, established by rotating one
                // component at a time and watching where a fixed world point lands. The first version of
                // this put pitch in x and so applied ROLL whenever a caller asked to look up or down -- it
                // went unnoticed because only yaw had ever been tested, and yaw was right.
                //
                // q = q_yaw * q_pitch, expanded rather than composed at runtime.
                const double yaw = webapi_query_double(q, "yaw", 0.0) * 3.14159265358979 / 180.0;
                const double pitch = webapi_query_double(q, "pitch", 0.0) * 3.14159265358979 / 180.0;
                const double roll = webapi_query_double(q, "roll", 0.0) * 3.14159265358979 / 180.0;
                const double cy = cos(yaw * 0.5), sy = sin(yaw * 0.5);
                const double cp = cos(pitch * 0.5), sp = sin(pitch * 0.5);
                const double cr = cos(roll * 0.5), sr = sin(roll * 0.5);
                // yaw*pitch first, then roll about the resulting forward axis.
                const double qx = sy * sp, qy = sy * cp, qz = cy * sp, qw = cy * cp;
                ht.set_head_rotation({static_cast<float>(qw * sr + qx * cr),
                                      static_cast<float>(qy * cr + qz * sr),
                                      static_cast<float>(qz * cr - qy * sr),
                                      static_cast<float>(qw * cr - qx * sr)});
            }
        }
        const auto st = ht.state();

        // AN OBSERVABLE THAT DOES NOT DEPEND ON A SCREENSHOT. Project a FIXED world point through the scene
        // camera's own world_to_screen and report the pixel. If the rendered view turns, a stationary point
        // must slide across the screen; if it does not move, the view did not turn no matter what any
        // intermediate field says. The point defaults to one 500 units along +X so it is well away from the
        // camera; a caller can name its own.
        // The probe is measured INSIDE the pass detour; setting it here only names the point.
        if (q.find("px") != q.end()) {
            CameraPassHook::get().set_probe_point(static_cast<float>(webapi_query_double(q, "px", 0.0)),
                                                  static_cast<float>(webapi_query_double(q, "py", 0.0)),
                                                  static_cast<float>(webapi_query_double(q, "pz", 0.0)));
        }
        const auto cp = CameraPassHook::get().observed();

        std::string out;
        {
            JsonFields jf(out);
            jf.b("ok", st.hooked).b("enabled", st.enabled).hex("target", st.target)
              .hex("holder", st.last_holder)
              .u("writer_calls", static_cast<size_t>(st.writer_calls))
              .u("writes", static_cast<size_t>(st.writes))
              .b("readback_matches", st.readback_matches)
              .f("qx", st.requested[0], 5).f("qy", st.requested[1], 5)
              .f("qz", st.requested[2], 5).f("qw", st.requested[3], 5);
        jf.b("projected", cp.probe_projected)
          .f("proj_x", cp.probe_pixel[0], 2).f("proj_y", cp.probe_pixel[1], 2);
        }
        return out;
    };

    handlers.stereo = [](const std::string& request_target) {
        const WebApiQuery q = webapi_parse_query(request_target);
        const std::string eye_s = webapi_query_string(q, "eye");
        auto eye = CameraPassHook::Eye::Off;
        if (eye_s == "left") {
            eye = CameraPassHook::Eye::Left;
        } else if (eye_s == "right") {
            eye = CameraPassHook::Eye::Right;
        }
        const auto ipd = static_cast<float>(webapi_query_double(q, "half_ipd", 0.0));
        const bool split = webapi_query_int(q, "split", 0) != 0;

        // ONLY MUTATE WHAT THE CALLER ASKED FOR. Every route under /stereo/ lands here, so the
        // obvious "read the state" call -- /stereo/state with no parameters -- used to parse an
        // ABSENT eye as Eye::Off and apply it. Polling the state therefore CLEARED the eye it was
        // reporting, and the reading always came back Off with a rising `overridden` count.
        //
        // That cost a whole investigation: the eye looked like it was auto-expiring after ~28
        // passes, and the captures taken to measure stereo were re-asserting it before every shot
        // to work around an expiry that did not exist. set_eye() was persistent all along.
        //
        // Same shape as /xr/capture requesting a one-shot on every call. A query is not a command.
        if (q.find("eye") != q.end() || q.find("both") != q.end() || q.find("half_ipd") != q.end()) {
            // `both=1` renders BOTH eyes per frame; otherwise `eye=` selects a single one.
            if (webapi_query_int(q, "both", 0) != 0) {
                CameraPassHook::get().set_stereo(true, ipd, split);
            } else {
                CameraPassHook::get().set_stereo(false, ipd, split);
                CameraPassHook::get().set_eye(eye, ipd, split);
            }
        }
        if (q.find("centre_x") != q.end() || q.find("centre_y") != q.end()) {
            CameraPassHook::get().set_frustum_centre(
                static_cast<float>(webapi_query_double(q, "centre_x", 0.0)),
                static_cast<float>(webapi_query_double(q, "centre_y", 0.0)));
        }
        if (q.find("fov_x") != q.end() || q.find("fov_y") != q.end()) {
            CameraPassHook::get().set_fov_override(
                static_cast<float>(webapi_query_double(q, "fov_x", 0.0)),
                static_cast<float>(webapi_query_double(q, "fov_y", 0.0)));
        }
        const auto o = CameraPassHook::get().observed();
        std::string out;
        {
            JsonFields jf(out);
            jf.b("ok", o.hooked).hex("target", o.target)
              .i("eye", static_cast<int>(o.eye)).f("half_ipd", o.half_ipd, 4)
              .b("split_viewport", o.split_viewport)
              .u("passes", static_cast<size_t>(o.passes))
              .u("overridden", static_cast<size_t>(o.overridden))
              .u("rejected", static_cast<size_t>(o.rejected))
              .b("stereo", o.stereo)
              .u("draw_calls", static_cast<size_t>(o.draw_calls))
              .u("second_eye_draws", static_cast<size_t>(o.second_eye_draws));
        }
        return out;
    };

    handlers.console = [](const std::string& request_target) {
        const WebApiQuery q = webapi_parse_query(request_target);
        const size_t qpos = request_target.find('?');
        const std::string route = qpos == std::string::npos ? request_target : request_target.substr(0, qpos);

        if (route == "/console/ui") {
            std::string items = "[";
            size_t n = 0;
            for (const auto& c : sdk::UiCommands::all()) {
                if (n++ != 0) { items += ','; }
                std::string one;
                {
                    JsonFields jf(one);
                    jf.s("name", c.name).hex("handler", c.handler).u("flag", c.flag).hex("entry", c.entry);
                }
                items += one;
            }
            items += ']';
            std::string out;
            {
                JsonFields jf(out);
                jf.b("ok", n > 0).hex("table", sdk::UiCommands::table_address())
                  .u("count", n).raw("commands", items);
            }
            return out;
        }

        if (route == "/console/list") {
            const std::string filter = webapi_query_string(q, "filter");
            std::string items = "[";
            size_t n = 0;
            for (const auto& cmd : sdk::Console::all()) {
                if (!filter.empty()) {
                    std::string lower_name = cmd.name;
                    std::string lower_filter = filter;
                    for (auto& ch : lower_name) { ch = static_cast<char>(tolower(ch)); }
                    for (auto& ch : lower_filter) { ch = static_cast<char>(tolower(ch)); }
                    if (lower_name.find(lower_filter) == std::string::npos) {
                        continue;
                    }
                }
                if (n++ != 0) { items += ','; }
                std::string one;
                {
                    JsonFields jf(one);
                    jf.s("name", cmd.name).hex("handler", cmd.handler).s("module", cmd.module)
                      .b("from_exe", cmd.from_exe).b("runtime_registered", cmd.registered_at_runtime());
                }
                items += one;
            }
            items += ']';
            std::string out;
            {
                JsonFields jf(out);
                jf.b("ok", true).u("count", n).raw("commands", items);
            }
            return out;
        }

        if (route == "/console/run") {
            const std::string cmd = webapi_query_string(q, "cmd");
            const bool queued = ConsoleRunner::get().queue(cmd);
            const auto st = ConsoleRunner::get().state();
            std::string out;
            {
                JsonFields jf(out);
                jf.b("ok", queued);
                if (!queued) {
                    jf.s("error", cmd.empty() ? "cmd is required" : "queue full or command too long");
                }
                jf.b("callback_registered", st.callback_registered)
                  .u("pending", st.pending)
                  .u("executed", static_cast<size_t>(st.executed))
                  .s("last_command", st.last_command);
                const char* outcome = "none";
                switch (st.last) {
                    case ConsoleRunner::Outcome::Ran: outcome = "ran"; break;
                    case ConsoleRunner::Outcome::NotFound: outcome = "not_found"; break;
                    case ConsoleRunner::Outcome::NoHandler: outcome = "no_handler"; break;
                    default: break;
                }
                jf.s("last_outcome", outcome);
            }
            return out;
        }

        return std::string{"{\"ok\":false,\"error\":\"unknown /console route\"}"};
    };

    handlers.input = [](const std::string& request_target) {
        const WebApiQuery q = webapi_parse_query(request_target);
        const size_t qpos = request_target.find('?');
        const std::string route = qpos == std::string::npos ? request_target : request_target.substr(0, qpos);
        auto& si = SyntheticInput::get();

        std::string err;
        if (route == "/input/look-direct") {
            // THE CURSOR-INDEPENDENT ROUTE. Calls CPlayerCamera_ApplyLookToRotation on the game
            // thread rather than feeding the engine's positional mouse handler, so it works with
            // the window unfocused -- which the mouse path provably does not.
            const WebApiQuery q = webapi_parse_query(request_target);
            const auto a = static_cast<float>(webapi_query_double(q, "pitch", 0.0));
            const auto b = static_cast<float>(webapi_query_double(q, "yaw", 0.0));
            const float c = 0.0f;
            const bool sent = sdk::PlayerMgr::apply_look_delta(0, a, b);
            std::string out;
            {
                JsonFields jf(out);
                jf.b("ok", sent)
                  .f("pitch", a, 5).f("yaw", b, 5).f("unused", c, 5)
                  .f("yaw_deg", sdk::PlayerMgr::aim_yaw(0).value_or(0.0f) * 57.2957795, 4)
                  .f("pitch_deg", sdk::PlayerMgr::aim_pitch(0).value_or(0.0f) * 57.2957795, 4);
            }
            return out;
        }

        if (route == "/input/look") {
            // A LOOK DELTA through the engine's own move handler -- the VR stick-turn primitive.
            // Queued onto the game thread rather than called here: it mutates device state the
            // engine reads without a lock, and this handler runs on the IPC thread.
            si.queue_look(static_cast<int32_t>(webapi_query_int(q, "dx", 0)),
                          static_cast<int32_t>(webapi_query_int(q, "dy", 0)));
        } else if (route == "/input/tap") {
            // uint32, not uint8: mouse buttons are encoded above the virtual-key range
            // (SyntheticInput::kMouseButton + n), and truncating here turned 0x100 into 0.
            const auto vk = static_cast<uint32_t>(webapi_query_int(q, "vk", 0));
            const auto frames = static_cast<uint32_t>(webapi_query_int(q, "frames", 2));
            if (!si.tap(vk, frames)) {
                err = vk == 0 ? "vk is required" : "no free key slot";
            }
        } else if (route == "/input/hold") {
            const auto vk = static_cast<uint32_t>(webapi_query_int(q, "vk", 0));
            si.hold(vk, webapi_query_int(q, "down", 1) != 0);
        } else if (route == "/input/release") {
            si.release_all();
            // BOTH BANKS, in that order. release_all() only touches slots it still owns, so a button latched
            // by an earlier session has nothing to release. Clearing CURRENT alone is not enough either: the
            // poll shifts INCOMING into current every frame, so a stuck incoming re-latches it immediately.
            // That is exactly the state this recovered from -- current read down again within one frame.
            for (uint8_t b = 0; b < 3; ++b) {
                sdk::Input::send_mouse_button(b, false);
                sdk::Input::set_mouse_button_state(b, false);
            }
        } else {
            err = "unknown /input route";
        }

        const auto st = si.state();
        const auto lk = si.look_stats();
        std::string out;
        {
            JsonFields jf(out);
            jf.b("ok", err.empty());
            if (!err.empty()) {
                jf.s("error", err);
            }
            jf.b("keyboard_resolved", st.keyboard_resolved)
              .u("active_taps", st.active_taps)
              .u("held_keys", st.held_keys)
              .u("writes", static_cast<size_t>(st.writes))
              .u("taps_completed", static_cast<size_t>(st.taps_completed))
              .u("look_delivered", static_cast<size_t>(lk.delivered))
              .i("look_last_dx", lk.last_dx).i("look_last_dy", lk.last_dy)
              .i("look_pending_dx", lk.pending_dx).i("look_pending_dy", lk.pending_dy);
        }
        return out;
    };

    handlers.watch = build_watch_json;
    handlers.focus_keep = [](const std::string& request_target) {
        const WebApiQuery q = webapi_parse_query(request_target);
        const bool on = webapi_query_int(q, "on", 1) != 0;
        FocusKeeper::get().keep_running(on);
        const auto st = FocusKeeper::get().state();
        std::string out;
        {
            JsonFields jf(out);
            jf.b("ok", st.hook_installed);
            jf.b("enabled", st.enabled);
            jf.hex("set_paused_fn", st.set_paused_fn);
            jf.b("window_active", st.window_active);
            jf.b("lost_focus", st.lost_focus);
        }
        return out;
    };

    handlers.view_override = [](const std::string& request_target) {
        const WebApiQuery q = webapi_parse_query(request_target);
        // Yaw in whole degrees: the experiment only needs a magnitude big enough to SEE, and an integer keeps
        // the query surface to the one parser this file already has.
        const long long yaw_i = webapi_query_int(q, "yaw", 15);
        const double yaw = static_cast<double>(yaw_i);
        const long long frames = webapi_query_int(q, "frames", 600);
        // target=source writes +324 (the candidate upstream field); anything else writes +244.
        // target=render owns ONLY the camera (recommended -- see ViewHook's note on the three systems),
        // target=source writes +324, anything else writes +244.
        const auto tgt = webapi_query_string(q, "target");
        const unsigned mode = tgt == "render" ? 2u : (tgt == "source" ? 1u : 0u);
        ViewHook::get().arm_override(static_cast<float>(yaw),
                                     static_cast<uint32_t>(frames < 0 ? 0 : frames), mode);
        const auto vh = ViewHook::get().observed();
        std::string out;
        {
            JsonFields jf(out);
            jf.b("ok", true);
            jf.f("yaw_deg", yaw, 3);
            jf.u("frames", static_cast<size_t>(vh.override_frames_left));
            jf.b("pose_hook_installed", vh.pose_installed);
            jf.s("target", mode == 2 ? "render(camera only)"
                                     : (mode == 1 ? "source(+324)" : "applied(+244)"));
        }
        return out;
    };
    handlers.engine_hook = build_engine_hook_json;
    handlers.api = build_api_json;
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

    // 2b. SEAL THE HOOK REGISTRY FIRST. Mods keep receiving on_frame() until the frame hook itself
    //     is retired in step 4, and at least one of them (FireRedirect, retrying against a lazy
    //     gameserver.dll) INSTALLS hooks from there. An install landing after retire() has walked
    //     past the end of the registry leaves a patched function with no owner -- found live in
    //     gameserver.dll, jumping into an orphaned stub outside every loaded module.
    Hooks::get().seal();

    // 3. Mods yield the frame path.
    Mods::get().on_shutdown();

    // AFTER the mods, because a fault while they tear down is exactly the kind we most want
    // reported -- and BEFORE the image unmaps, because the process holds a pointer to our filter.
    exception_handler::remove();

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
