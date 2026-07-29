#include "Framework.hpp"

#include <atomic>
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
#include "sdk/Render.hpp"
#include "sdk/VisTree.hpp"
#include "sdk/interfaces/All.hpp"

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
    char buf[3072];
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
             sec_read_ok, sec_with_planes, sec_plane_total, player_sector,
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
    return buf;
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
                const auto pose = skel->pose_a(*idx);
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
                    if (std::isfinite(wt->position.x) && std::isfinite(wt->position.y) &&
                        std::isfinite(wt->position.z)) {
                        ++sock_xform_finite;
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
    char sum[1152];
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
             "\"sock_xform_max_dist\":%.2f,\"sock_camera_measured\":%zu,"
             "\"sock_camera_above\":%zu,\"sock_camera_max_height\":%.2f}",
             *taken, with_skeleton, resolved_wanted, emitted, handles_seen, handles_round_trip,
             handles_absent, mgr->handle_table_size().value_or(0), weapons.size(),
             everything.size(), bone_slots, bone_slots_live, models_with_palette, anim_ok,
             anim_index_in_range, anim_frac_in_range, anim_blending, anim_nodes_in_range,
             anim_nodes_named, anim_nodes_ordered, anim_named, piece_answers, piece_hidden,
             piece_counts, piece_named, piece_roundtrip, sock_xform_ok, sock_xform_stale,
             sock_xform_unit, sock_xform_finite, sock_xform_clean,
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
               api_tree_miss_at_leaf = 0, api_tree_hit_slot_probed = 0;
        // The worst disagreement between the engine's placement of an attached child and
        // our own composition for its socket handle. A float, not a count: the interesting
        // result is the magnitude.
        float api_att_worst_err = 0.0f, api_brush_worst_rt = 0.0f,
              api_brush_worst_origin = 0.0f, api_brush_worst_rot = 0.0f;
        std::vector<sdk::CClientMgr::ObjectSnapshot> snaps(4096);
        for (size_t t = 0; t < sdk::CClientMgr::object_list_count(); ++t) {
            const auto taken = mgr->snapshot_objects(static_cast<sdk::ObjectType>(t), snaps.data(),
                                                    snaps.size());
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
                        if (sk->find_socket("socket_left_eye").has_value() &&
                            sk->find_socket("socket_right_eye").has_value()) {
                            ++api_socket_eyes;
                        }
                    }
                }
                if (const auto r = sdk::is_renderable(obj); r.value_or(false)) {
                    ++api_renderable;
                }
            }
        }
        char ab[1408];
        const int abw = snprintf(ab, sizeof(ab),
                 ",\"object_api\":{\"objects\":%zu,\"info_ok\":%zu,\"renderable\":%zu,"
                 "\"cameras\":%zu,\"cameras_with_bit11\":%zu,\"with_handle\":%zu,"
                 "\"with_slot\":%zu,\"identities_agree\":%zu,\"addressable\":%zu,"
                 "\"with_attachments\":%zu,\"attachments\":%zu,\"att_child_ok\":%zu,"
                 "\"att_socketed\":%zu,\"att_resolved\":%zu,\"att_is_socket\":%zu,"
                 "\"att_measured\":%zu,\"att_placed\":%zu,\"att_worst_err\":%.4f,"
                 "\"socket_total\":%zu,\"socket_ok\":%zu,\"socket_named_node\":%zu,"
                 "\"socket_roundtrip\":%zu,\"socket_camera\":%zu,\"socket_eyes\":%zu,"
                 "\"node_xform_ok\":%zu,\"node_xform_stale\":%zu,\"node_xform_clean\":%zu,"
                 "\"node_xform_clean_sane\":%zu,\"camera_node_clean\":%zu,"
                 "\"dims_ok\":%zu,\"dims_nonneg\":%zu,\"dims_zero\":%zu,"
                 "\"standing\":%zu,\"standing_sane\":%zu,\"standing_node\":%zu,"
                 "\"color_ok\":%zu,\"color_packed_ok\":%zu,\"color_default\":%zu,"
                 "\"color_translucent\":%zu,\"brush\":%zu,\"brush_roundtrip\":%zu,"
                 "\"brush_rt_exact\":%zu,\"brush_origin_ok\":%zu,"
                 "\"brush_worst_rt\":%.5f,\"brush_worst_origin\":%.5f,"
                 "\"brush_quality\":%zu,\"brush_trusted\":%zu,\"brush_matrix\":%zu,"
                 "\"brush_origin_agrees\":%zu,\"brush_worst_rot\":%.5f,"
                 "\"cull_ok\":%zu,\"cull_sphere\":%zu,\"cull_box\":%zu,\"cull_none\":%zu,"
                 "\"cull_sane\":%zu,\"cull_compared\":%zu,\"cull_current\":%zu,"
                 "\"tree_asked\":%zu,\"tree_linked\":%zu,\"tree_nonempty\":%zu,"
                 "\"tree_self_found\":%zu,\"tree_nonwm\":%zu,"
                 "\"tree_nonwm_found\":%zu,\"tree_wm_missed\":%zu,"
                 "\"miss_slot_found\":%zu,\"miss_max_depth\":%zu,\"miss_at_leaf\":%zu,"
                 "\"hit_max_depth\":%zu}",
                 api_objects, api_info_ok, api_renderable, api_cameras, api_camera_bit,
                 api_with_handle, api_with_slot, api_identities_agree, api_addressable,
                 api_with_attachments, api_attachments, api_att_child_ok,
                 api_att_socketed, api_att_resolved, api_att_is_socket,
                 api_att_measured, api_att_placed, api_att_worst_err,
                 api_socket_total, api_socket_ok,
                 api_socket_named_node, api_socket_roundtrip, api_socket_camera,
                 api_socket_eyes, api_node_xform_ok, api_node_xform_stale,
                 api_node_xform_clean, api_node_xform_clean_sane, api_camera_node_clean,
                 api_dims_ok, api_dims_nonneg, api_dims_zero, api_standing,
                 api_standing_sane, api_standing_node, api_color_ok, api_color_packed_ok,
                 api_color_default, api_color_translucent, api_brush, api_brush_roundtrip,
                 api_brush_rt_exact, api_brush_origin_ok, api_brush_worst_rt,
                 api_brush_worst_origin, api_brush_quality, api_brush_trusted,
                 api_brush_matrix, api_brush_origin_agrees, api_brush_worst_rot,
                 api_cull_ok, api_cull_sphere, api_cull_box, api_cull_none,
                 api_cull_sane, api_cull_compared, api_cull_current,
                 api_tree_asked, api_tree_linked, api_tree_nonempty, api_tree_self_found,
                 api_tree_nonwm, api_tree_nonwm_found, api_tree_wm_missed,
                 api_tree_miss_slot_found, api_tree_miss_max_depth, api_tree_miss_at_leaf);
        if (abw < 0 || static_cast<size_t>(abw) >= sizeof(ab)) {
            out += ",\"object_api\":{\"error\":\"truncated\"}";
        } else {
            out += ab;
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
    const auto* entries = sdk::interfaces::all_interfaces();
    const size_t count = sdk::interfaces::all_interface_count();
    for (size_t i = 0; i < count; ++i) {
        if (i != 0) {
            out += ",";
        }
        const auto& e = entries[i];
        const auto a = e.agreement();
        void* via_getter = e.get();
        char b[384];
        snprintf(b, sizeof(b),
                 "{\"name\":\"%s\",\"holders\":%zu,\"non_null\":%zu,\"all_agree\":%s,"
                 "\"value\":\"0x%08" PRIXPTR "\",\"getter\":\"0x%08" PRIXPTR "\","
                 "\"getter_matches\":%s}",
                 e.name, a.total, a.non_null, a.all_agree ? "true" : "false",
                 reinterpret_cast<uintptr_t>(a.value),
                 reinterpret_cast<uintptr_t>(via_getter),
                 (via_getter == a.value) ? "true" : "false");
        out += b;
    }
    out += "]}";
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
