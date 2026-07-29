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
    char buf[1280];
    const auto* exe = sdk::Modules::get().exe();
    auto* client_mgr = sdk::CClientMgr::get();
    const auto engine_time = sdk::Engine::client_time();
    const auto shell_game = sdk::CClientShell::game_time_seconds();
    const auto shell_real = sdk::CClientShell::real_time_seconds();
    const auto force = sdk::Engine::global_force();

    // start_shell_list_count is std::optional: nullopt means the SDK's own
    // walk did NOT terminate (corrupt list / wrong mapping) or faulted.
    // Reported as -1 so a consumer can distinguish "didn't terminate" from
    // "terminated with 0 entries" WITHOUT restating the SDK's internal cap
    // (see that method's comment -- the cap must not leak outside the SDK).
    const std::optional<size_t> shell_count =
        client_mgr != nullptr ? client_mgr->start_shell_list_count() : std::optional<size_t>{};

    snprintf(buf, sizeof(buf),
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
             "\"global_force\":[%f,%f,%f],\"global_force_ok\":%s}",
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
             force.has_value() ? "true" : "false");
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
            if (*anim_n > 0 && anim->index < *anim_n && anim->index_b < *anim_n) {
                ++anim_index_in_range;
            }
            if (anim->fraction >= 0.0f && anim->fraction <= 1.0f) {
                ++anim_frac_in_range;
            }
            if (anim->index != anim->index_b) {
                ++anim_blending;
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
    char sum[640];
    const int want = snprintf(sum, sizeof(sum),
             "],\"model_objects\":%zu,\"with_skeleton\":%zu,\"wanted_resolved\":%zu,\"listed\":%zu,"
             "\"handles_seen\":%zu,\"handles_round_trip\":%zu,\"handles_absent\":%zu,"
             "\"handle_table_slots\":%zu,\"found_weapons\":%zu,\"found_all\":%zu,"
             "\"bone_slots\":%zu,\"bone_slots_live\":%zu,\"models_with_palette\":%zu,"
             "\"anim_ok\":%zu,\"anim_index_in_range\":%zu,\"anim_frac_in_range\":%zu,"
             "\"anim_blending\":%zu,\"anim_nodes_in_range\":%zu,"
             "\"anim_nodes_named\":%zu,\"anim_nodes_ordered\":%zu}",
             *taken, with_skeleton, resolved_wanted, emitted, handles_seen, handles_round_trip,
             handles_absent, mgr->handle_table_size().value_or(0), weapons.size(),
             everything.size(), bone_slots, bone_slots_live, models_with_palette, anim_ok,
             anim_index_in_range, anim_frac_in_range, anim_blending, anim_nodes_in_range,
             anim_nodes_named, anim_nodes_ordered);
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
               api_identities_agree = 0, api_addressable = 0;
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
                }
                if (const auto r = sdk::is_renderable(obj); r.value_or(false)) {
                    ++api_renderable;
                }
            }
        }
        char ab[384];
        const int abw = snprintf(ab, sizeof(ab),
                 ",\"object_api\":{\"objects\":%zu,\"info_ok\":%zu,\"renderable\":%zu,"
                 "\"cameras\":%zu,\"cameras_with_bit11\":%zu,\"with_handle\":%zu,"
                 "\"with_slot\":%zu,\"identities_agree\":%zu,\"addressable\":%zu}",
                 api_objects, api_info_ok, api_renderable, api_cameras, api_camera_bit,
                 api_with_handle, api_with_slot, api_identities_agree, api_addressable);
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
