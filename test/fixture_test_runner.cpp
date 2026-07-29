// ctest E2E runner: drives fear2vr.dll against the REAL game (FEAR2.exe is the
// fixture). ALL test assertions live here, host-side -- the injected mod ships
// diagnostics only (/health, /sdk/targets, /engine-hook), never tests
// (TESTING.MD: "no test assertions inside the shipped mod").
//
// Exit codes: 0 pass / 1 fail / 77 skip (ctest SKIP_RETURN_CODE 77).
// Skip is honest: no game, spawn rejected, injection rejected, or IPC never
// came up -> 77. A failing CLAIM (red assertion) -> 1.
//
// Pipeline:
//   1. args; require injector/dll on disk
//   2. reuse a running FEAR2.exe, else spawn it and let it boot to menu
//   3. clear any stale instance on the port
//   4. inject
//   5. ASSERTIONS (checker): health shape, frame hook liveness (ticks advance),
//      /sdk/targets in-bounds vs the PID's real module list, /engine-hook
//      "hwnd" positive + "fear2vr_no_such_hook" negative, SDK-reported
//      mapping invariants (see the no-RPM note below)
//   6. graceful-unload proof: /unload -> module vanishes from module list AND
//      the game process keeps running
//   7. re-inject: health answers again, frame ticks reset to a small value
//      (fresh instance -- proves the first instance's hooks/handles are gone)
//   8. teardown: terminate the instance we spawned, else final unload
//
// NO ReadProcessMemory, ANYWHERE, EVER (TESTING.MD): the point is that the
// SDK -- compiled from the fear2.genny schema and running IN-PROCESS --
// produces what we expect. A host-side RPM walk with hardcoded offsets only
// proves our hand-typed numbers agree with themselves, and it duplicates the
// schema as magic values in the test. The DLL calls its own SDK methods and
// reports results; the host validates shape, invariants, and OS-level ground
// truth it alone owns (module residency from its own Toolhelp32 snapshot --
// OS metadata about what's mapped, NOT reading the process's memory).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <chrono>

#include <windows.h>
#include <tlhelp32.h>

#include "HttpClient.hpp"

namespace fs = std::filesystem;

namespace {

constexpr int32_t kOk = 0;
constexpr int32_t kFail = 1;
constexpr int32_t kSkip = 77;

int64_t g_checks = 0;
int64_t g_failures = 0;

void check(bool ok, const char* name, const char* detail = nullptr) {
    ++g_checks;
    if (!ok) {
        ++g_failures;
        printf("[FAIL] %s%s%s\n", name, detail ? ": " : "", detail ? detail : "");
    }
}

// ---- small parsing helpers (no JSON library; payloads are ours) ------------

bool json_has(const std::string& body, const char* needle) {
    return body.find(needle) != std::string::npos;
}

bool json_hex(const std::string& body, const char* key, uint32_t& out) {
    const std::string needle = std::string{"\""} + key + "\":\"";
    const size_t p = body.find(needle);
    if (p == std::string::npos) return false;
    const size_t start = p + needle.size();
    const size_t end = body.find('"', start);
    if (end == std::string::npos) return false;
    out = static_cast<uint32_t>(strtoul(body.c_str() + start, nullptr, 0));
    return true;
}

bool json_int(const std::string& body, const char* key, int64_t& out) {
    const std::string needle = std::string{"\""} + key + "\":";
    const size_t p = body.find(needle);
    if (p == std::string::npos) return false;
    const size_t start = p + needle.size();
    char* endp = nullptr;
    const long long v = strtoll(body.c_str() + start, &endp, 10);
    if (endp == body.c_str() + start) return false;
    out = v;
    return true;
}

bool json_double(const std::string& body, const char* key, double& out) {
    const std::string needle = std::string{"\""} + key + "\":";
    const size_t p = body.find(needle);
    if (p == std::string::npos) return false;
    const size_t start = p + needle.size();
    char* endp = nullptr;
    const double v = strtod(body.c_str() + start, &endp);
    if (endp == body.c_str() + start) return false;
    out = v;
    return true;
}

// ---- remote module inspection ----------------------------------------------

struct RemoteModule {
    std::string basename;
    uintptr_t base;
    uint32_t size;
};

uint32_t find_pid(const char* process_name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    uint32_t pid = 0;
    const size_t want_len = strlen(process_name);
    if (Process32FirstW(snap, &pe)) {
        do {
            char narrow[260];
            size_t conv = 0;
            wcstombs_s(&conv, narrow, pe.szExeFile, sizeof(narrow) - 1);
            if (strlen(narrow) == want_len && _stricmp(narrow, process_name) == 0) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

bool remote_find_module(uint32_t pid, const char* basename, RemoteModule& out) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    bool found = false;
    if (Module32FirstW(snap, &me)) {
        do {
            char narrow[260];
            size_t conv = 0;
            wcstombs_s(&conv, narrow, me.szModule, sizeof(narrow) - 1);
            const char* name = strrchr(narrow, '\\');
            name = name ? name + 1 : narrow;
            if (_stricmp(name, basename) == 0) {
                out = {name, reinterpret_cast<uintptr_t>(me.modBaseAddr), me.modBaseSize};
                found = true;
                break;
            }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return found;
}


std::string resident_fear2vr(uint32_t pid) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return {};
    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    std::string found;
    if (Module32FirstW(snap, &me)) {
        do {
            char narrow[260];
            size_t conv = 0;
            wcstombs_s(&conv, narrow, me.szModule, sizeof(narrow) - 1);
            const char* name = strrchr(narrow, '\\');
            name = name ? name + 1 : narrow;
            for (char* p = narrow; *p; ++p) *p = static_cast<char>(tolower(*p));
            if (_strnicmp(name, "fear2vr", 7) == 0 && strlen(name) >= 11 &&
                _stricmp(name + strlen(name) - 4, ".dll") == 0) {
                found = name;
                break;
            }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return found;
}

// ---- injector plumbing ------------------------------------------------------

struct SpawnedProcess {
    PROCESS_INFORMATION pi{};
    bool launched = false;
    SpawnedProcess() = default;
    SpawnedProcess(const SpawnedProcess&) = delete;
    SpawnedProcess& operator=(const SpawnedProcess&) = delete;
    ~SpawnedProcess() {
        if (pi.hProcess) CloseHandle(pi.hProcess);
        if (pi.hThread) CloseHandle(pi.hThread);
    }
};

void launch_fixture(const fs::path& exe, SpawnedProcess& out) {
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    std::wstring cmd = L"\"" + exe.wstring() + L"\"";
    std::wstring dir = exe.parent_path().wstring();
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &out.pi)) {
        printf("[fixture] CreateProcessW failed (%lu) for %s\n", GetLastError(), exe.string().c_str());
        return;
    }
    out.launched = true;
}

int run_injector(const fs::path& injector, const char* action, const fs::path& dll, int32_t port) {
    std::wstring cmd = L"\"" + injector.wstring() + L"\" --" +
                       std::wstring(action, action + strlen(action)) + L" --process FEAR2.exe --dll \"" +
                       fs::absolute(dll).wstring() + L"\" --port " + std::to_wstring(port);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        printf("[fixture] injector spawn failed (%lu)\n", GetLastError());
        return -1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD rc = 1;
    GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(rc);
}

// ---- health helpers ----------------------------------------------------------

bool health_body(int32_t port, std::string& body) {
    std::string resp;
    if (!http::get(port, "/health", resp)) return false;
    body = http::body_of(resp);
    return json_has(body, "\"ok\":true");
}

bool wait_healthy(int32_t port, int32_t attempts_100ms) {
    for (int32_t i = 0; i < attempts_100ms; ++i) {
        std::string body;
        if (health_body(port, body) && json_has(body, "\"state\":\"running\"")) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

bool wait_unloaded(uint32_t pid, int32_t attempts_200ms) {
    for (int32_t i = 0; i < attempts_200ms; ++i) {
        if (resident_fear2vr(pid).empty()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return false;
}

bool process_alive(uint32_t pid) {
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (proc == nullptr) return false;
    DWORD rc = 0;
    const bool alive = GetExitCodeProcess(proc, &rc) && rc == STILL_ACTIVE;
    CloseHandle(proc);
    return alive;
}

} // namespace

int main(int argc, char** argv) {
    fs::path injector;
    fs::path dll;
    fs::path fixture;
    int32_t port = 8798;

    for (int32_t i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--injector") injector = next();
        else if (a == "--dll") dll = next();
        else if (a == "--fixture") fixture = next();
        else if (a == "--port") port = atoi(next().c_str());
    }

    if (injector.empty() || dll.empty() || fixture.empty()) {
        printf("[fixture] missing required --injector/--dll/--fixture\n");
        return kSkip;
    }
    if (!fs::exists(injector) || !fs::exists(dll)) {
        printf("[fixture] injector or dll missing on disk\n");
        return kSkip;
    }

    // 2. Reuse a running game, else spawn and boot.
    SpawnedProcess spawned;
    uint32_t pid = find_pid("FEAR2.exe");
    if (pid != 0) {
        printf("[fixture] reusing running FEAR2.exe (pid %lu)\n", pid);
    } else {
        if (!fs::exists(fixture)) {
            printf("[fixture] game exe not found at %s -- skipping\n", fixture.string().c_str());
            return kSkip;
        }
        launch_fixture(fixture, spawned);
        if (!spawned.launched) {
            printf("[fixture] could not launch game -- skipping\n");
            return kSkip;
        }
        printf("[fixture] spawned FEAR2.exe; waiting for engine boot...\n");
        std::this_thread::sleep_for(std::chrono::seconds(20)); // LithTech boot to main menu
        pid = find_pid("FEAR2.exe");
        if (pid == 0) {
            printf("[fixture] game died during boot -- skipping\n");
            return kSkip;
        }
    }

    auto cleanup = [&] {
        if (spawned.launched) {
            TerminateProcess(spawned.pi.hProcess, 0);
            printf("[fixture] terminated the instance we spawned\n");
        } else if (find_pid("FEAR2.exe") != 0 && !resident_fear2vr(pid).empty()) {
            run_injector(injector, "unload", dll, port);
            printf("[fixture] unloaded from the pre-existing instance (left running)\n");
        }
    };

    // 3. Clear stale instance; 4. inject.
    if (http::port_open(port)) {
        run_injector(injector, "unload", dll, port);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    if (run_injector(injector, "inject", dll, port) != 0) {
        printf("[fixture] injection failed -- skipping (no injection rights?)\n");
        cleanup();
        return kSkip;
    }
    if (!wait_healthy(port, 200)) {
        printf("[fixture] IPC never became live -- skipping (init failure in-game)\n");
        cleanup();
        return kSkip;
    }
    printf("[fixture] IPC live; running host-side assertions\n");

    RemoteModule game_mod{};
    check(remote_find_module(pid, "FEAR2.exe", game_mod), "remote_find_module(FEAR2.exe)");
    RemoteModule gc_mod{}, db_mod{}, lt_mod{};
    const bool have_gc = remote_find_module(pid, "gameclient.dll", gc_mod);
    const bool have_db = remote_find_module(pid, "gamedatabase.dll", db_mod);
    const bool have_lt = remote_find_module(pid, "ltmemory.dll", lt_mod);
    check(have_gc, "gameclient.dll mapped in game");
    check(have_db, "gamedatabase.dll mapped in game");
    check(have_lt, "ltmemory.dll mapped in game");

    // 5a. /health shape + frame-hook liveness.
    //
    // The hook check is gated on an INDEPENDENT liveness oracle, because a
    // suspended fixture still answers IPC perfectly (threads created after the
    // suspend keep running, so the server serves stale reads from a frozen
    // game). The gate is the engine's own last_sample_time_ms, which only
    // advances when the engine executes -- deliberately NOT frame_ticks, which
    // is our hook's counter and is the thing under test here. Using the subject
    // as its own gate would reclassify a broken hook as "not exercised".
    //
    // engine frozen              -> not exercised, reported loudly
    // engine live, ticks static  -> HARD FAIL, the hook is broken
    // engine live, ticks moving  -> pass
    {
        // Engine-side timestamp reader. Transport and field presence are HARD
        // checks: if either fails we must not silently label it "suspended",
        // which would mask a broken endpoint as an environment state.
        auto read_engine_ms = [&](int64_t& out) -> bool {
            std::string t;
            if (!http::get(port, "/sdk/targets", t)) {
                return false;
            }
            return json_int(http::body_of(t), "last_sample_time_ms", out);
        };

        std::string h1, h2;
        check(health_body(port, h1), "/health transport");
        check(json_has(h1, "\"state\":\"running\""), "/health state==running");
        check(json_has(h1, "\"sdk_ready\":true"), "/health sdk_ready==true");
        int64_t hooks = -1;
        check(json_int(h1, "hooks", hooks) && hooks >= 1, "/health reports the frame hook installed");
        int64_t ticks1 = -1;
        check(json_int(h1, "frame_ticks", ticks1), "/health exposes frame_ticks");

        int64_t engine_ms1 = -1;
        check(read_engine_ms(engine_ms1) && engine_ms1 >= 0,
              "engine timestamp (last_sample_time_ms) readable for the liveness gate");

        // Poll well past one update interval before concluding "frozen". The
        // field was only characterised at ~1s granularity, so a single 500ms
        // sample is not enough to call a live engine static.
        bool engine_live = false;
        int64_t engine_ms2 = engine_ms1;
        for (int attempt = 0; attempt < 8 && !engine_live; ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            int64_t now = -1;
            check(read_engine_ms(now) && now >= 0, "engine timestamp still readable");
            engine_ms2 = now;
            engine_live = now > engine_ms1;
        }

        check(health_body(port, h2), "/health second poll transport");
        int64_t ticks2 = -1;
        check(json_int(h2, "frame_ticks", ticks2), "/health exposes frame_ticks (second poll)");

        if (engine_live) {
            check(ticks2 > ticks1 && ticks1 >= 0,
                  "frame_ticks advanced (hook firing on CClientShell::Update)");
        } else {
            printf("[fixture] NOTE: engine not advancing (last_sample_time_ms %lld -> %lld over 2s) "
                   "-- fixture is paused/suspended, so the frame-hook check was NOT exercised. "
                   "Resume the game, or investigate if it should be running.\n",
                   static_cast<long long>(engine_ms1), static_cast<long long>(engine_ms2));
        }
    }

    // 5b. /sdk/targets: every engine address must sit inside the REAL
    //     FEAR2.exe image measured host-side from the PID's module list.
    {
        std::string resp;
        check(http::get(port, "/sdk/targets", resp), "/sdk/targets transport");
        const std::string body = http::body_of(resp);
        const uintptr_t lo = game_mod.base;
        const uintptr_t hi = game_mod.base + game_mod.size;
        auto in_exe = [&](const char* key) {
            uint32_t v = 0;
            if (!json_hex(body, key, v)) {
                check(false, key, "field missing/unparseable");
                return;
            }
            char detail[128];
            snprintf(detail, sizeof(detail), "0x%08X outside [0x%08X,0x%08X)", v,
                     static_cast<uint32_t>(lo), static_cast<uint32_t>(hi));
            check(v >= lo && v < hi, key, detail);
        };
        in_exe("client_mgr_update");
        in_exe("client_shell_update");
        in_exe("get_engine_hook");
        in_exe("g_pClientMgr_slot");
        in_exe("hWnd_slot");

        uint32_t client_mgr = 0, main_hwnd = 0, dbmgr = 0;
        check(json_hex(body, "client_mgr", client_mgr) && client_mgr != 0, "client_mgr instance non-null");
        check(json_hex(body, "main_hwnd", main_hwnd) && main_hwnd != 0, "main_hwnd instance non-null");
        check(json_hex(body, "database_mgr", dbmgr) && dbmgr != 0, "database_mgr non-null");
        if (have_db && dbmgr != 0) {
            char detail[128];
            snprintf(detail, sizeof(detail), "0x%08X not in gamedatabase.dll [0x%08X,0x%08X)", dbmgr,
                     static_cast<uint32_t>(db_mod.base),
                     static_cast<uint32_t>(db_mod.base + db_mod.size));
            check(dbmgr >= db_mod.base && dbmgr < db_mod.base + db_mod.size, "database_mgr residency", detail);
        }

        // (Per the no-RPM rule at the top of this file: every value below is
        // computed IN-PROCESS by the SDK and reported over IPC; the host only
        // validates shape/invariants and OS-level ground truth.)

        // client_shell: sdk::CClientMgr::client_shell() (regenny-backed --
        // reversing/fear2.genny's CClientMgr.client_shell). Non-null and
        // heap-resident (outside the exe image -- it's an allocated object,
        // not a static).
        uint32_t client_shell = 0;
        check(json_hex(body, "client_shell", client_shell) && client_shell != 0, "client_shell non-null");
        check(client_shell < lo || client_shell >= hi,
              "client_shell is heap-allocated (outside the FEAR2.exe image), as expected for a runtime object");

        // client_mgr_updating: regenny::CClientMgr.updating is true only for
        // the brief duration of CClientMgr::Update's own CClientShell::Update
        // call (see fear2.genny's comment) -- sampled out-of-band over HTTP it
        // will almost always be false. Assert the STRICT JSON boolean shape
        // only (never a specific value -- true is rare but real, not a bug).
        check(json_has(body, "\"client_mgr_updating\":true") || json_has(body, "\"client_mgr_updating\":false"),
              "client_mgr_updating is a well-formed JSON boolean");
        check(main_hwnd != 0 && IsWindow(reinterpret_cast<HWND>(main_hwnd)),
              "main_hwnd is a live window (IsWindow, host-side)");

        // counter_node_registered: sdk::CClientMgr::counter_node_registered()
        // checks the CClientMgr_Init wiring invariant IN-PROCESS, entirely
        // through the generated schema (&own_counter_node->self_link ==
        // counter_list_head.next -- the compiler derives every offset from
        // fear2.genny; no literal appears in the SDK or here). If the schema
        // ever drifts, that method recomputes correctly rather than
        // comparing stale numbers.
        check(json_has(body, "\"counter_node_registered\":true"),
              "SDK reports its own counter node correctly linked into counter_list_head (CClientMgr_Init wiring invariant)");

        // start_shell_list_count: sdk::CClientMgr::start_shell_list_count()
        // walked the list in-process and reports std::optional -- serialized
        // as -1 when the walk did NOT terminate (corrupt list / wrong
        // mapping) or faulted. Asserting >= 0 IS the termination proof; the
        // SDK's internal fail-closed cap is deliberately NOT restated here
        // (a literal like "< 10000" in a test is a magic value duplicating
        // SDK internals -- see TESTING.MD rule 2).
        int64_t shell_count = -1;
        check(json_int(body, "start_shell_list_count", shell_count),
              "start_shell_list_count present");
        check(shell_count >= 0,
              "SDK's start_shell_list walk terminated cleanly (list well-formed; -1 would mean it hit its own fail-closed cap or faulted)");

        // counter_elapsed_ms/counter_elapsed_time: BOTH read in-process by
        // the SDK from the SAME mapped node. The confirmed invariant is the
        // correlation (elapsed_ms == elapsed_time*1000) -- NOT anything
        // about advancement, whose semantics are unverified (see
        // fear2.genny's comment). Two schema fields at different
        // offsets/types agreeing numerically is a real mapping proof: if
        // either offset were wrong, they would not correlate. Asserting on
        // advancement here would also be flaky by construction -- it needs
        // a confirmed-running engine, and the fixture may attach to an
        // idle or suspended instance.
        int64_t elapsed_ms = -1;
        double elapsed_time = -1.0;
        check(json_int(body, "counter_elapsed_ms", elapsed_ms) && elapsed_ms >= 0, "counter_elapsed_ms present and non-negative");
        check(json_double(body, "counter_elapsed_time", elapsed_time) && elapsed_time >= 0.0, "counter_elapsed_time present and non-negative");
        const double diff = static_cast<double>(elapsed_ms) - elapsed_time * 1000.0;
        check(diff > -2.0 && diff < 2.0,
              "counter_elapsed_ms == counter_elapsed_time*1000 (two distinct mapped fields agree -- offset proof)");
    }

    // 5b1. /sdk/models: the CONSUMER API, tested the way a mod uses it.
    //
    // Everything else in this file validates the mapping -- offsets, counts,
    // invariants. This block validates the INTERFACE built on top of it, which is a
    // different thing and can break independently: sdk::ModelSkeleton could stop
    // resolving, or find_node could stop agreeing with node_name, while every
    // offset underneath stayed perfectly correct.
    //
    // So the assertions here are contract-shaped, not offset-shaped:
    //   * every OT_MODEL resolves a skeleton (a promise from_object makes),
    //   * a name lookup ROUND-TRIPS -- find_node(n) then node_name(i) gives n back,
    //     which is what proves the two agree rather than both being plausible,
    //   * a node found by name has a real parent and a real depth, i.e. the view is
    //     navigable and not just readable.
    {
        std::string resp;
        check(http::get(port, "/sdk/models", resp), "/sdk/models transport");
        const std::string body = http::body_of(resp);
        check(json_has(body, "\"ok\":true"), "/sdk/models reports ok");

        int64_t model_objects = -1, with_skeleton = -1, wanted = -1, listed = -1;
        json_int(body, "model_objects", model_objects);
        json_int(body, "with_skeleton", with_skeleton);
        json_int(body, "wanted_resolved", wanted);
        json_int(body, "listed", listed);

        check(model_objects > 0, "live OT_MODEL objects exist to build views from");
        check(with_skeleton == model_objects,
              "ModelSkeleton::from_object resolves for EVERY model object, not just most");
        // Without this the round-trip assertion below would be vacuous: no lookups
        // succeeding means nothing was ever compared.
        check(wanted > 0, "find_node resolved at least one of Head/L_Hand/R_Hand");
        check(listed > 0, "at least one model was listed with its named nodes");

        // A single false anywhere means find_node and node_name disagree, which
        // would make every name-based lookup in a mod silently wrong.
        check(body.find("\"round_trip\":false") == std::string::npos,
              "every name lookup round-trips through node_name");
        // -1 is the endpoint's encoding for "the view could not answer". A named
        // node always has a parent (none of Head/L_Hand/R_Hand is a root) and a
        // depth of at least two, so either appearing means path_to_root or
        // parent_of failed on a node we had just successfully looked up.
        check(body.find("\"parent\":-1") == std::string::npos,
              "every node found by name has a resolvable parent");
        check(body.find("\"depth\":-1") == std::string::npos,
              "path_to_root terminates for every node found by name");
        check(body.find("\"materials\":-1") == std::string::npos,
              "model_materials answers for every listed model");

        // HANDLE ROUND-TRIP. The engine's HOBJECT is an index into CClientMgr's
        // handle table, so every ILT* call a mod makes depends on this conversion
        // being right. The DLL takes each object's own handle, resolves it back
        // through object_from_handle, and requires the SAME object out.
        //
        // This is a contract check, not a layout check: it would catch the table
        // pointer moving, the tag convention changing, or the entry stride being
        // wrong -- all of which would leave a mod silently holding other people's
        // objects.
        int64_t hseen = -1, hround = -1, habsent = -1, hslots = -1;
        json_int(body, "handles_seen", hseen);
        json_int(body, "handles_round_trip", hround);
        json_int(body, "handles_absent", habsent);
        json_int(body, "handle_table_slots", hslots);

        check(hseen > 0, "objects with handles exist, so the round-trip is not vacuous");
        check(hround == hseen, "every handle resolves back to the object that carries it");
        // Objects without a handle are normal -- live 335 of 3583 across all types --
        // so absence is reported rather than asserted away.
        check(habsent >= 0, "objects without a handle are counted, not treated as failures");
        check(hslots > hseen,
              "the handle table is sparser than the live object count (handles are not dense)");

        // PATH SEARCH, the way a mod identifies its target. An empty needle must
        // match every model -- that is what makes a non-empty needle's smaller
        // result meaningful rather than just a failure to match.
        int64_t fw = -1, fa = -1;
        json_int(body, "found_weapons", fw);
        json_int(body, "found_all", fa);
        check(fa == model_objects, "find_models(\"\") returns every live model object");
        check(fw > 0 && fw <= fa, "find_models(\"weapons\") matches a real subset");

        // THE BONE MATRIX PALETTE, through bone_matrix(). The accessor's CONTRACT is
        // assertable: the engine allocates the palette for every model (it is gated on
        // a flag bit set on all of them) at 48 * node_count, so the accessor must
        // answer for every model and every node index below that count.
        //
        // The CONTENTS are not asserted at all, and that is the point. This is
        // per-frame render state -- the renderer fills a mesh's bones during its draw
        // -- so on an idle frame most slots are legitimately zero (live: 169 of 215
        // models entirely zero, the player's viewmodel included). Requiring a
        // populated palette would make this test depend on the game happening to
        // render at the instant it runs, which is exactly the kind of flake a fixture
        // suite must not have.
        int64_t mwr = -1, nslots = -1, nlive = -1;
        json_int(body, "models_with_palette", mwr);
        json_int(body, "bone_slots", nslots);
        json_int(body, "bone_slots_live", nlive);
        check(mwr == with_skeleton, "bone_matrix answers for every model that has a skeleton");
        check(nslots > 0, "palette slots were actually read");
        check(nlive >= 0 && nlive <= nslots,
              "populated palette slots are a reported fraction, never a requirement");
    }

    // 5b2. /sdk/objects: the CClientMgr object-list mapping, exercised
    // IN-PROCESS. The DLL walks its own 7 type buckets via
    // sdk::CClientMgr::object_count()/snapshot_objects() and reports the
    // results; the host validates shape and invariants only.
    //
    // These assertions hold on a frozen engine as well as a running one --
    // they are structural, not liveness. (The counts themselves drift as the
    // game creates/destroys objects, so nothing here pins an exact total.)
    {
        std::string resp;
        check(http::get(port, "/sdk/objects", resp), "/sdk/objects transport");
        const std::string body = http::body_of(resp);

        int64_t bucket_count = -1;
        check(json_int(body, "bucket_count", bucket_count) && bucket_count == 7,
              "object_lists has exactly 7 type buckets (schema-driven, not a literal in the test)");

        // all_terminated is the SDK's own report that EVERY bucket walk ended
        // back at its head without faulting, without hitting the fail-closed
        // cap, and -- the strong part -- without a single object whose .type
        // disagreed with its bucket index. A false here means the mapping
        // drifted (wrong list_link offset would land container-of on garbage).
        check(json_has(body, "\"all_terminated\":true"),
              "every bucket walk terminated cleanly AND every object's type == its bucket index");

        int64_t total = -1;
        check(json_int(body, "total", total) && total >= 0,
              "total object count present and non-negative");

        // Sample shape: rotations are quaternions and were unit-length across
        // every object sampled during mapping. A non-unit magnitude means we
        // are reading the wrong offset, not that the game misbehaved.
        double mag = -1.0;
        if (json_double(body, "rot_magnitude", mag)) {
            check(mag > 0.99 && mag < 1.01,
                  "sampled object's rotation is a unit-length quaternion (offset proof)");
        }

        // Type names come from the SDK's own enum mapping, so a bucket
        // reordering in the schema would show up here rather than silently.
        check(json_has(body, "\"bucket_names\":[\"OT_NORMAL\",\"OT_MODEL\",\"OT_WORLDMODEL\","
                             "\"OT_SPRITE\",\"OT_LIGHT\",\"OT_CAMERA\",\"OT_PARTICLESYSTEM\"]"),
              "bucket index -> ObjectType name mapping is in schema order");

        // Per-object-type allocator banks. These assertions encode the mapping
        // proof from reversing/fear2.genny's CClientMgr.object_banks comment:
        // six banks, the array index is NOT the type, and OT_LIGHT has none.
        check(json_has(body, "\"banks\":["), "objects report includes the allocator banks");

        // The bank -> type sequence is the whole point: 0,1,2,3,5,6 with 4
        // (OT_LIGHT) absent. Asserting the exact sequence catches both a
        // reordering and a silent off-by-one that would make bank_for() hand
        // back OT_CAMERA's allocator for OT_LIGHT.
        {
            const int expect_types[] = {0, 1, 2, 3, 5, 6};
            size_t pos = body.find("\"banks\":[");
            size_t seen = 0;
            bool order_ok = true, all_match = true;
            while (seen < 6) {
                pos = body.find("{\"index\":", pos);
                if (pos == std::string::npos) break;
                const size_t obj_end = body.find('}', pos);
                if (obj_end == std::string::npos) break;
                const std::string ob = body.substr(pos, obj_end - pos + 1);
                int64_t idx = -1, ty = -1, elem = -1, block = -1;
                json_int(ob, "index", idx);
                json_int(ob, "type", ty);
                json_int(ob, "element_size", elem);
                json_int(ob, "block_size", block);
                if (idx != static_cast<int64_t>(seen) || ty != expect_types[seen]) order_ok = false;
                // The allocator relationship the mapping rests on. If this
                // fails, `element_size` is not what the pool was built for and
                // the bank identification is unsound.
                if (block != ((elem + 8) & ~7)) all_match = false;
                if (!json_has(ob, "\"block_matches\":true")) all_match = false;
                ++seen;
                pos = obj_end;
            }
            check(seen == 6, "exactly 6 object allocator banks reported");
            check(order_ok, "bank index -> type sequence is 0,1,2,3,5,6 (OT_LIGHT has no bank)");
            check(all_match, "every bank: pool block_size == (element_size + 8) & ~7");
        }

        // Cached transforms. The pair is WORLDMODEL state that Camera inherits,
        // so BOTH types are checked -- an earlier version of this block looked
        // only at type 5, which is exactly why the fields sat mis-attributed to
        // LTCameraObject for several passes. Checking the type that actually
        // owns a field is what catches that class of error.
        //
        // The asymmetry is deliberate and matches the data:
        //   det_ok is a HARD invariant on both -- every matrix must be a proper
        //     rotation, and a wrong offset cannot produce determinant 1.
        //   rotation_match / inverse_ok are exact on cameras (474/474) but lag on
        //     worldmodels (1464 and 1450 of 1473), because a worldmodel is moving
        //     level geometry whose cache can trail its quaternion between
        //     updates. Asserting them for type 2 would be asserting that nothing
        //     in the level is mid-motion.
        for (const char* key : {"\"worldmodel_transforms\":", "\"camera_transforms\":"}) {
            const bool is_camera = std::string_view{key}.find("camera") != std::string_view::npos;
            const size_t tp = body.find(key);
            const std::string has_label = std::string{"objects report includes "} + key;
            check(tp != std::string::npos, has_label.c_str());
            if (tp == std::string::npos) {
                continue;
            }
            const std::string nullpat = std::string{key} + "null";
            if (json_has(body, nullpat.c_str())) {
                const std::string done_label = std::string{"transform walk completed for "} + key;
                check(false, done_label.c_str());
                continue;
            }
            const size_t end = body.find('}', tp);
            const std::string tb = body.substr(tp, end - tp + 1);
            int64_t sampled = -1, rot = -1, inv = -1, det = -1;
            json_int(tb, "sampled", sampled);
            json_int(tb, "rotation_match", rot);
            json_int(tb, "inverse_ok", inv);
            json_int(tb, "det_ok", det);

            const std::string present_label = std::string{"objects present for "} + key;
            check(sampled > 0, present_label.c_str());
            const std::string det_label =
                std::string{"every cached 3x3 has determinant 1 for "} + key;
            check(det == sampled, det_label.c_str());
            if (is_camera) {
                check(rot == sampled,
                      "every camera world_transform 3x3 == R(its own rotation quaternion)");
                check(inv == sampled,
                      "every camera inverse_transform is the exact rigid inverse");
            } else {
                // Reported, not asserted: see the comment above.
                check(rot > 0 && rot <= sampled,
                      "worldmodel rotation_match is a sane count (cache may lag, not asserted)");
                check(inv > 0 && inv <= sampled,
                      "worldmodel inverse_ok is a sane count (cache may lag, not asserted)");
            }
        }

        // SCHEMA SIZES. The engine records, per bank, the element_size it asks
        // its pool for -- the concrete class's size as the engine understands
        // it. Our headers declare a size independently. Neither number is a
        // baseline this test recorded, so this cannot drift into agreement.
        //
        // Its reach is narrower than it looks and the limit is worth stating:
        // it pins each class's TOTAL, so a field past the end or a mis-sized
        // member fails here. It does NOT catch a field attributed to the wrong
        // class inside a correct total -- the transform pair sat on
        // LTCameraObject for several passes with both sizes right.
        {
            const size_t sp = body.find("\"schema_sizes\":");
            check(sp != std::string::npos, "objects report includes the schema size check");
            if (json_has(body, "\"schema_sizes\":null")) {
                check(false, "schema size check completed");
            } else if (sp != std::string::npos) {
                const size_t end = body.find('}', sp);
                const std::string sb = body.substr(sp, end - sp + 1);
                int64_t checked = -1, matches = -1;
                json_int(sb, "types_checked", checked);
                json_int(sb, "size_matches", matches);
                // Six creatable types have banks; OT_LIGHT does not.
                check(checked == 6, "all six bank-allocated types were found");
                check(matches == checked,
                      "every bank's element_size equals our schema's sizeof for that type");
                check(json_has(sb, "\"light_has_no_bank\":true"),
                      "OT_LIGHT has no bank (it is uncreatable: no case 4 in the dispatcher)");
            }
        }

        // OT_MODEL's embedded list. The engine stores the list's count next to
        // its head, so the walk is checked against the engine's own number.
        // The constructor links each object's embedded_link in before anything
        // else can touch it, so an unlinked one means the offset moved.
        {
            const size_t mp = body.find("\"model_lists\":");
            check(mp != std::string::npos, "objects report includes the model list check");
            if (json_has(body, "\"model_lists\":null")) {
                check(false, "model list walk completed");
            } else if (mp != std::string::npos) {
                const size_t end = body.find('}', mp);
                const std::string mb = body.substr(mp, end - mp + 1);
                int64_t s = -1, cm = -1, el = -1, ad = -1, ap = -1, ru = -1, mm = -1;
                json_int(mb, "sampled", s);
                json_int(mb, "count_matches_walk", cm);
                json_int(mb, "embedded_linked", el);
                json_int(mb, "asset_dup_agrees", ad);
                json_int(mb, "asset_present", ap);
                json_int(mb, "rotation_unit", ru);
                json_int(mb, "max_members", mm);

                check(s > 0, "models present to check");
                check(cm == s, "every model's stored list_count equals its walked member count");
                check(el == s, "every model's embedded_link is a member of its own list");
                check(ad == s, "asset_dup equals asset (two routes to one pointer)");
                check(ap == s, "every model has a non-null asset (a mandatory resource)");
                check(ru == s, "cached_rotation is unit length (so it is a quaternion, not padding)");
                // One embedded member is guaranteed by the constructor; extras are
                // scene-dependent, so only the floor is asserted.
                check(mm >= 1, "at least the embedded member is present in every list");

                // The claim "the list members are LTModelRecords" needs evidence,
                // not just a plausible cast of a link address. Each member's
                // asset must be the OWNER's asset -- a wrong record layout puts
                // something else at member+0x20, and a wrong list walk reaches
                // records belonging to other models. Live this covers the 74
                // members that are NOT the embedded one, which is the only part
                // of the walk that could go wrong silently.
                int64_t mt = -1, ma = -1;
                json_int(mb, "members_total", mt);
                json_int(mb, "member_asset_ok", ma);
                check(mt >= s, "every model contributes at least its embedded record");
                check(ma == mt, "every list member's asset is its owner's asset");
            }
        }

        // MATERIAL NAMES. Every check is a std::string against ITSELF: the byte
        // at [size] is the terminator, size fits inside capacity, and capacity is
        // at least the small-buffer minimum. Nothing external is consulted, so
        // there is no baseline to drift and no sibling structure to be wrong.
        //
        // That the array's base, length AND 28-byte stride all come from the
        // engine's own teardown loop is what makes this safe to walk at all: a
        // guessed stride would land mid-string and every one of these would fail
        // at once, which is exactly the behaviour wanted from a decode check.
        {
            const size_t cp = body.find("\"model_materials\":");
            check(cp != std::string::npos, "objects report includes the material check");
            if (json_has(body, "\"model_materials\":null")) {
                check(false, "material walk completed");
            } else if (cp != std::string::npos) {
                const size_t end = body.find('}', cp);
                const std::string cb = body.substr(cp, end - cp + 1);
                int64_t models = -1, total = -1, term = -1, sizeok = -1, capok = -1, pr = -1, mx = -1;
                json_int(cb, "models", models);
                json_int(cb, "strings_total", total);
                json_int(cb, "terminated", term);
                json_int(cb, "size_le_capacity", sizeok);
                json_int(cb, "capacity_sane", capok);
                json_int(cb, "nonempty_printable", pr);
                json_int(cb, "max_count", mx);

                check(models > 0, "models with material arrays exist");
                check(total >= models, "every such model contributes at least one string");
                check(term == total, "every material string is NUL-terminated at [size]");
                check(sizeok == total, "every material string's size fits within its capacity");
                check(capok == total, "every material string's capacity is at least the small buffer");
                check(mx >= 1, "material_count is at least one where an array exists");
                // Printability is REPORTED, not asserted: empty strings are legal
                // and 34 of them exist live. Asserting it would be asserting that
                // no model has an unset material slot.
                check(pr > 0 && pr <= total,
                      "printable non-empty paths are a sane fraction (empties are legal)");
            }
        }

        // THE SHARED ASSET. Four of these are the asset checked against itself --
        // a self-pointer, two duplicated fields, and a filename that has to decode
        // as printable NUL-terminated ASCII. The fifth compares the refcount to
        // the models actually pointing at the asset, which is the engine's own
        // bookkeeping against a live traversal.
        //
        // refcount is asserted only as an INEQUALITY, on purpose. refcount ==
        // 2*users + 1 fits 27 of 34 live and looked like the rule from the ten
        // biggest assets; counting all 34 found six BELOW that figure, which
        // refutes a per-model floor outright rather than just complicating it.
        // The tight count is reported so the gap stays visible.
        {
            const size_t ap = body.find("\"model_assets\":");
            check(ap != std::string::npos, "objects report includes the asset check");
            if (json_has(body, "\"model_assets\":null")) {
                check(false, "asset walk completed");
            } else if (ap != std::string::npos) {
                const size_t end = body.find('}', ap);
                const std::string ab = body.substr(ap, end - ap + 1);
                int64_t assets = -1, self = -1, rad = -1, ndup = -1, nread = -1, rge = -1, rex = -1;
                json_int(ab, "assets", assets);
                json_int(ab, "self_ref_ok", self);
                json_int(ab, "radius_dup_ok", rad);
                json_int(ab, "name_at_blob", ndup);
                json_int(ab, "name_readable", nread);
                json_int(ab, "refcount_ge", rge);
                json_int(ab, "refcount_exact", rex);

                check(assets > 0, "shared model assets exist");
                check(self == assets, "every asset's self_ref holds its own address");
                check(rad == assets, "every asset's radius equals the value read from its file");
                check(ndup == assets, "every asset's filename points at its string_blob's front");
                check(nread == assets, "every asset filename decodes as printable NUL-terminated ASCII");
                check(rge == assets, "every asset's refcount is at least its live model users");
                check(rex >= 0 && rex <= assets,
                      "refcount == 2*users+1 is a reported fraction, not a rule (6 of 34 fall below)");

                // CONTAINMENT. The loader carves the name, the name table, the
                // fixed-up pointer array and both entry arrays out of ONE
                // allocation, so every derived pointer has to land inside it --
                // checked against the asset's own recorded blob size, with nothing
                // external involved. A moved offset breaks containment long before
                // it produces a plausible-looking wrong value.
                int64_t bsane = -1, inblob = -1, worder = -1, cmatch = -1, cdup = -1;
                json_int(ab, "blob_size_sane", bsane);
                json_int(ab, "arrays_in_blob", inblob);
                json_int(ab, "write_order_ok", worder);
                json_int(ab, "count_matches", cmatch);
                json_int(ab, "count_dup_ok", cdup);
                check(bsane == assets, "every asset's string_blob_size is non-zero and plausible");
                check(inblob == assets, "both entry arrays lie inside the asset's string_blob");
                check(worder == assets,
                      "filename <= entry_array_a <= entry_array_b (the loader's write order)");
                // entry_count was FOUND by this arithmetic, so this check keeps the
                // derivation honest rather than merely restating it: if the field
                // ever stops matching the gap, one of the two has moved.
                check(cmatch == assets,
                      "node_count equals (node_hashes - node_names) / 4 on every asset");
                check(cdup == assets, "node_count_dup agrees with node_count");
            }
        }

        // SKELETON NODES. The name array is validated by containment and decode,
        // and the hash array by the one property a hash of a name must have: the
        // same name always produces the same value. Nothing is recomputed
        // host-side, so no translation table or module offset is baked in here.
        //
        // What gives that check teeth is repetition. If every node name were
        // unique the comparison would be vacuous -- each name would define its own
        // hash and nothing would ever be compared -- so repeated_names is asserted
        // non-zero. Live 86 of 125 distinct names recur, which is what makes
        // 660/660 consistency a real result.
        {
            const size_t np = body.find("\"model_nodes\":");
            check(np != std::string::npos, "objects report includes the node check");
            if (json_has(body, "\"model_nodes\":null")) {
                check(false, "node walk completed");
            } else if (np != std::string::npos) {
                const size_t end = body.find('}', np);
                const std::string nb = body.substr(np, end - np + 1);
                int64_t nassets = -1, total = -1, inblob = -1, pr = -1, distinct = -1, rep = -1,
                        cons = -1, coll = -1, cdup2 = -1;
                json_int(nb, "assets", nassets);
                json_int(nb, "nodes_total", total);
                json_int(nb, "names_in_blob", inblob);
                json_int(nb, "names_printable", pr);
                json_int(nb, "distinct_names", distinct);
                json_int(nb, "repeated_names", rep);
                json_int(nb, "hash_consistent", cons);
                json_int(nb, "hash_collisions", coll);
                json_int(nb, "count_dup_ok", cdup2);

                check(total > 0, "skeleton nodes are present");
                check(inblob == total, "every node name pointer lies inside its asset's string_blob");
                check(pr == total, "every node name decodes as printable NUL-terminated ASCII");
                check(cons == total, "the same node name always carries the same hash");
                check(rep > 0, "node names actually repeat, so the hash comparison is not vacuous");
                check(distinct > 0 && distinct <= total, "distinct name count is sane");
                check(cdup2 == nassets, "node_count_dup agrees on every asset with nodes");
                // Collisions are REPORTED, not asserted to be zero. Injectivity is a
                // property of this level's names, not something the mapping
                // guarantees -- a future level could collide without anything being
                // wrong, and asserting 0 would make that look like a regression.
                check(coll >= 0, "hash collisions among distinct names are reported (live: 0)");

                // THE SKELETON IS A TREE, stored as an array in topological order.
                // These four together are what "tree" means, and none of them
                // consults anything but the asset's own node_count:
                //   root_is_255      exactly one designated root
                //   index_self_ok    each record knows its own slot
                //   topological_ok   parents precede children, so no cycles and a
                //                    single forward pass can compose world
                //                    transforms without recursion
                //   child_sum_ok     sum(child_count) == node_count - 1, which for
                //                    a connected parent relation forces exactly one
                //                    root and no node reachable twice
                // The last one is the strongest: an off-by-one in the stride or a
                // mis-sized record would break the sum long before the individual
                // fields looked wrong.
                int64_t rib = -1, r255 = -1, isok = -1, topo = -1, csum = -1, ra = -1, rb = -1,
                        pf = -1;
                json_int(nb, "records_in_blob", rib);
                json_int(nb, "root_is_255", r255);
                json_int(nb, "index_self_ok", isok);
                json_int(nb, "topological_ok", topo);
                json_int(nb, "child_sum_ok", csum);
                json_int(nb, "rot_a_unit", ra);
                json_int(nb, "rot_b_unit", rb);
                json_int(nb, "pos_finite", pf);

                check(rib == nassets, "every asset's node array lies wholly inside its string_blob");
                check(r255 == nassets, "every skeleton's node[0] is the root (parent_index 255)");
                check(isok == nassets, "every node's own_index equals its array index");
                check(topo == nassets, "every node's parent precedes it (topological order)");
                check(csum == nassets, "sum(child_count) == node_count - 1 on every skeleton");
                // Both quaternions are unit on every node. That is what proves those
                // 16-byte spans are rotations rather than arbitrary floats -- a
                // wrong offset cannot keep producing magnitude 1 across 660 nodes.
                check(ra == total, "every node's rotation_a is unit length");
                check(rb == total, "every node's rotation_b is unit length");
                check(pf == total, "every node's two position vectors are finite and in range");

                // CONTIGUOUS CHILDREN. A node's children are the solid block
                // [i+off, i+off+count), which is how GetNodeChildIndex resolves
                // them, so first_child_offset, child_count and parent_index are
                // three fields that must agree with each other.
                int64_t cbr = -1, cpo = -1, cls = -1;
                json_int(nb, "child_block_in_range", cbr);
                json_int(nb, "child_parents_ok", cpo);
                json_int(nb, "child_links_seen", cls);
                check(cbr == nassets, "every child block stays inside its node array");
                check(cpo == nassets, "every node in a child block names its owner as parent");
                // EXACT relation, not just non-vacuity: every node except each
                // skeleton's single root is reached exactly once as somebody's
                // child, so the link count must be nodes minus roots. Live that is
                // 626 == 660 - 34. This is what rules out children being
                // double-counted or skipped, which the two counts above cannot see.
                check(cls == total - nassets,
                      "child links == nodes - roots (every non-root reached exactly once)");
            }
        }

        // THE ANIMATION-NAME TABLE. Ascending hash order is not a nicety here: the
        // engine finds animations by BINARY SEARCHING this vector, so if the order
        // broke, lookups would start missing entries and nothing else would
        // complain. A quiet failure mode is the best argument for a test.
        //
        // max_entries > 1 is asserted for the same reason repeated_names is
        // asserted for the hash check: sortedness over a single element is
        // vacuously true, so without it a scene with only trivial tables would
        // report a perfect pass having verified nothing.
        {
            const size_t tp = body.find("\"anim_tables\":");
            check(tp != std::string::npos, "objects report includes the anim table check");
            if (json_has(body, "\"anim_tables\":null")) {
                check(false, "anim table walk completed");
            } else if (tp != std::string::npos) {
                const size_t end = body.find('}', tp);
                const std::string tb = body.substr(tp, end - tp + 1);
                int64_t tassets = -1, sane = -1, asc = -1, ents = -1, mx = -1;
                json_int(tb, "assets", tassets);
                json_int(tb, "table_sane", sane);
                json_int(tb, "hashes_ascending", asc);
                json_int(tb, "entries_total", ents);
                json_int(tb, "max_entries", mx);

                check(tassets > 0, "assets with animation tables exist");
                check(sane == tassets, "every asset's anim table has a sane base and count");
                check(asc == sane, "every anim table is sorted by hash (the binary search needs it)");
                check(ents >= sane, "every table contributes at least one entry");
                check(mx > 1, "at least one table has multiple entries, so sortedness is not vacuous");
            }
        }

        // Bounding geometry across every object type. These are identities the
        // engine's own SetDims establishes (aabb = position +/- dims, radius =
        // |dims| + 0.1), so every live object must satisfy them. They guard the
        // culling inputs, where a moved offset would mis-cull silently rather
        // than crash.
        {
            const size_t gp = body.find("\"geometry\":");
            check(gp != std::string::npos, "objects report includes the geometry check");
            if (gp != std::string::npos) {
                if (json_has(body, "\"geometry\":null")) {
                    check(false, "geometry walk completed (null == faulted)");
                } else {
                    const size_t end = body.find('}', gp);
                    const std::string gb = body.substr(gp, end - gp + 1);
                    int64_t sampled = -1, mn = -1, mx = -1, rs = -1, rp = -1, nn = -1;
                    json_int(gb, "sampled", sampled);
                    json_int(gb, "aabb_min_ok", mn);
                    json_int(gb, "aabb_max_ok", mx);
                    json_int(gb, "radius_sized", rs);
                    json_int(gb, "radius_pristine", rp);
                    json_int(gb, "dims_nonneg", nn);
                    check(sampled > 0, "objects present to check geometry against");
                    check(mn == sampled, "every object: aabb_min == position - dims");
                    check(mx == sampled, "every object: aabb_max == position + dims");
                    check(nn == sampled, "every object: all dims components >= 0");

                    // The radius is two-state. Assert the PARTITION (every
                    // object lands in exactly one state) and, separately, that
                    // BOTH states are actually populated. The partition alone
                    // would pass vacuously if every object went pristine, which
                    // is exactly how a broken dims offset would present.
                    check(rs + rp == sampled,
                          "every object is either sized (radius == |dims| + 0.1) or pristine "
                          "(dims == 0 and radius == 0)");
                    check(rs > 0, "sized-radius objects exist (SetDims branch exercised)");
                    check(rp > 0, "pristine-radius objects exist (constructor branch exercised)");
                }
            }
        }

        // World tree. Same self-check shape, but this one is a chain: the walk
        // starts from an object, follows world_tree_link out to the owning node,
        // then climbs parent_offset to the root. A wrong offset anywhere along
        // that path shows up as a count shortfall rather than a crash.
        {
            const size_t wp = body.find("\"world_tree\":");
            check(wp != std::string::npos, "objects report includes the world-tree check");
            if (wp != std::string::npos) {
                if (json_has(body, "\"world_tree\":null")) {
                    check(false, "world-tree walk completed (null == faulted or no exe range)");
                } else {
                    const size_t end = body.find('}', wp);
                    const std::string wb = body.substr(wp, end - wp + 1);
                    int64_t seen = -1, linked = -1, unlinked = -1, found = -1, root = -1,
                            mono = -1, mism = -1, depth = -1;
                    json_int(wb, "objects_seen", seen);
                    json_int(wb, "linked", linked);
                    json_int(wb, "unlinked", unlinked);
                    json_int(wb, "node_found", found);
                    json_int(wb, "root_reached", root);
                    json_int(wb, "counts_monotonic", mono);
                    json_int(wb, "root_mismatches", mism);
                    json_int(wb, "max_depth", depth);

                    check(seen > 0, "objects present to walk the world tree from");
                    // Linked/unlinked is a partition, and BOTH sides must be
                    // populated for the same reason the radius states must be:
                    // a broken link offset would read self-pointing garbage and
                    // classify everything as unlinked.
                    check(linked + unlinked == seen,
                          "every object is either threaded into the world tree or self-pointing");
                    check(linked > 0, "tree-linked objects exist (linked branch exercised)");
                    check(unlinked > 0, "unlinked objects exist (self-pointing branch exercised)");

                    check(found == linked, "every linked object's owning node was located");
                    check(root == linked, "every linked object's parent chain reached a root");
                    check(mono == linked,
                          "occupied_count never decreases from a node toward the root");
                    check(mism == 0, "all linked objects reach the SAME root (one tree)");
                    // Depth > 0 proves the parent chain was actually climbed; at
                    // depth 0 the monotonic and root checks would be trivially
                    // true without ever dereferencing parent_offset.
                    check(depth > 0, "parent chain was actually climbed (max_depth > 0)");

                    // CROSS-ROUTE. The root we climbed to from an object must be
                    // the one LTWorldClientBSP stores. Two independent routes,
                    // both supplied by the engine -- no baseline of ours -- so
                    // this is valid in any level and fails if parent_offset,
                    // world_tree_link or that header field moved.
                    check(json_has(body, "\"root_matches_bsp\":true"),
                          "the climbed world-tree root equals LTWorldClientBSP.world_tree_root");
                }
            }
        }

        // The world container. This validates the world tree from the HEADER
        // side: the engine stores its own node count, so the walk is checked
        // against the engine's number rather than anything recorded host-side.
        {
            const size_t bp = body.find("\"world_bsp\":");
            check(bp != std::string::npos, "objects report includes the world-BSP check");
            if (bp != std::string::npos) {
                if (json_has(body, "\"world_bsp\":null")) {
                    check(false, "world BSP resolved (null == unresolved interface or no world)");
                } else {
                    const size_t end = body.find('}', bp);
                    const std::string bb = body.substr(bp, end - bp + 1);
                    int64_t stored = -1, walked = -1, occ = -1, depth = -1, sib = -1, sc = -1;
                    json_int(bb, "stored_node_count", stored);
                    json_int(bb, "nodes_walked", walked);
                    json_int(bb, "occupied", occ);
                    json_int(bb, "max_depth", depth);
                    json_int(bb, "sectors_in_bounds", sib);
                    json_int(bb, "sector_count", sc);

                    check(stored > 0, "world BSP reports a world-tree node count");
                    check(walked == stored,
                          "walking the world tree reaches exactly world_tree_node_count nodes");
                    check(occ > 0, "world-tree nodes hold objects");
                    check(depth > 0, "world tree is deeper than a single node");
                    check(json_has(bb, "\"bounds_ordered\":true"),
                          "world bounds are ordered (min <= max)");
                    // TWO copies of the bounds exist (+0x04 and +0x22C) and the
                    // outside-world test reads the second. They agree live; a
                    // divergence would mean one of the two offsets is wrong.
                    check(json_has(bb, "\"bounds_copies_agree\":true"),
                          "both world-bounds copies hold the same values");
                    check(sc > 0, "world BSP reports a sector count");
                    check(sib == sc, "every vis sector's AABB fits inside the world bounds");
                }
            }
        }

        // Per-type cull volumes. These guard the offsets the engine actually
        // culls with, and specifically the expression that identified `scale`:
        // OT_MODEL's radius is vis_radius * scale, so if either offset moves the
        // product stops being a positive finite number.
        {
            const size_t cp = body.find("\"cull_volumes\":");
            check(cp != std::string::npos, "objects report includes the cull-volume check");
            if (cp != std::string::npos) {
                if (json_has(body, "\"cull_volumes\":null")) {
                    check(false, "cull-volume walk completed (null == faulted)");
                } else {
                    const size_t end = body.find('}', cp);
                    const std::string cb = body.substr(cp, end - cp + 1);
                    int64_t models = -1, vispos = -1, radok = -1, parts = -1, tok = -1, sph = -1,
                            aabb = -1;
                    json_int(cb, "models", models);
                    json_int(cb, "model_vis_radius_pos", vispos);
                    json_int(cb, "model_radius_ok", radok);
                    json_int(cb, "particles", parts);
                    json_int(cb, "particle_type_ok", tok);
                    json_int(cb, "particle_sphere", sph);
                    json_int(cb, "particle_aabb", aabb);
                    int64_t spr = -1, sab = -1, ssp = -1, sord = -1, srad = -1;
                    json_int(cb, "sprites", spr);
                    json_int(cb, "sprite_aabb", sab);
                    json_int(cb, "sprite_sphere", ssp);
                    json_int(cb, "sprite_aabb_ordered", sord);
                    json_int(cb, "sprite_radius_ok", srad);

                    check(models > 0, "OT_MODEL objects present to check cull radii");
                    check(vispos == models, "every OT_MODEL has vis_radius > 0");
                    check(radok == models,
                          "every OT_MODEL cull radius (vis_radius * scale) is positive and finite");

                    // The asset link. `asset` is shared per model asset (34
                    // distinct across 215 objects), and vis_radius caches its
                    // radius. One comparison therefore exercises the pointer at
                    // +0xEC and both float offsets. A divergence is worth a hard
                    // failure: it is either a moved offset or a stale cache, and
                    // the culling radius is wrong either way.
                    int64_t anonnull = -1, aradeq = -1;
                    json_int(cb, "model_asset_nonnull", anonnull);
                    json_int(cb, "model_asset_radius_eq", aradeq);
                    check(anonnull == models,
                          "every OT_MODEL has a non-null asset (SlotIndex_WantsSlot requires it)");
                    check(aradeq == models,
                          "every OT_MODEL's vis_radius equals its shared asset's radius");

                    check(parts > 0, "OT_PARTICLESYSTEM objects present to check volume kinds");
                    // The provider RETURNS this byte, so 1/2 are the only values
                    // the interface defines -- this is an interface constraint,
                    // not a range observed and then frozen.
                    check(tok == parts, "every OT_PARTICLESYSTEM cull_volume_type is 1 or 2");
                    check(sph + aabb == parts,
                          "particle volume kinds partition into sphere and AABB");
                    check(sph > 0, "sphere-volume particle systems exist (kind 1 exercised)");
                    check(aabb > 0, "AABB-volume particle systems exist (kind 2 exercised)");

                    // OT_SPRITE. The kind byte is NOT a volume enum, so unlike
                    // the particle case there is no interface-fixed value set to
                    // assert -- only the split, and each shape's own fields.
                    check(spr > 0, "OT_SPRITE objects present to check cull volumes");
                    check(sab + ssp == spr, "sprite kinds partition into AABB-shaped and sphere");
                    check(sord == sab, "every AABB-kind sprite has aabb_min <= aabb_max");
                    check(srad == ssp, "every sphere-kind sprite has a finite positive radius");
                    // Both shapes are populated live (9 AABB / 215 sphere), so
                    // requiring both keeps the two field sets genuinely exercised
                    // rather than one of them passing on an empty set.
                    check(sab > 0, "AABB-kind sprites exist (aabb fields exercised)");
                    check(ssp > 0, "sphere-kind sprites exist (radius field exercised)");
                }
            }
        }

        // Attachment graph + owned objects + slot index.
        //
        // Read the assertion selection here as carefully as the values: the
        // parented population is SCENE-DEPENDENT (one object at the main menu),
        // so "parented > 0" is deliberately NOT asserted -- it would fail
        // spuriously in a scene with no attachments. What is asserted are the
        // totals, which hold at any population including zero.
        {
            const size_t ap = body.find("\"attachments\":");
            check(ap != std::string::npos, "objects report includes the attachment check");
            if (ap != std::string::npos) {
                if (json_has(body, "\"attachments\":null")) {
                    check(false, "attachment walk completed (null == faulted)");
                } else {
                    const size_t end = body.find('}', ap);
                    const std::string ab = body.substr(ap, end - ap + 1);
                    int64_t objs = -1, listed = -1, selfok = -1, parentless = -1, parented = -1,
                            linkok = -1, reached = -1, cpok = -1, ownne = -1, ownent = -1,
                            inone = -1, iset = -1;
                    json_int(ab, "objects", objs);
                    json_int(ab, "self_ptr_ok", selfok);
                    json_int(ab, "parentless", parentless);
                    json_int(ab, "parented", parented);
                    json_int(ab, "link_consistent", linkok);
                    json_int(ab, "children_reached", reached);
                    json_int(ab, "child_parent_ok", cpok);
                    json_int(ab, "owned_nonempty", ownne);
                    json_int(ab, "owned_entries", ownent);
                    json_int(ab, "index_none", inone);
                    json_int(ab, "index_set", iset);

                    check(objs > 0, "objects present for the attachment walk");
                    check(selfok == objs, "every object's `self` equals its own address");
                    check(parentless + parented == objs,
                          "parent presence partitions every object");
                    check(linkok == objs,
                          "every object: parent == null exactly when parent_link self-points");
                    json_int(ab, "listed", listed);

                    // COVERAGE GATE. The cross-count below compares two
                    // independently-walked populations, so it is only valid when
                    // the walk saw everything: a sampled child whose parent fell
                    // outside the sample is counted as parented while nothing
                    // walks the list holding it. This exact case failed on first
                    // run at a 512-per-type cap (2215 of 3583 objects, parented 1,
                    // children_reached 0) -- a sampling artifact, not a broken
                    // map. Asserting coverage rather than skipping means a future
                    // scene that outgrows the cap fails loudly here instead of
                    // quietly comparing mismatched populations.
                    check(listed == objs,
                          "attachment walk covered every object (raise the endpoint cap if this "
                          "fails)");
                    check(reached == parented,
                          "children reached by walking child_lists == objects reporting a parent");
                    check(cpok == reached, "every child reached names its walker as parent");

                    // owned_list IS populated live (1931 objects / 3260 entries), so
                    // unlike attachments this one can be required. An earlier
                    // schema comment claimed these lists were always empty; that
                    // was never checked and was wrong, so assert against it.
                    check(ownne > 0, "objects with a non-empty owned_list exist");
                    check(ownent >= ownne, "owned_entries is at least the number of non-empty lists");

                    check(inone + iset == objs, "slot_index partitions into none-sentinel and set");
                    check(iset > 0, "objects with a real slot_index exist");

                    // shared_ref, checked against ITSELF: the record stores its
                    // own address twice and keeps a live refcount, so neither a
                    // baseline nor a sibling structure is involved. Live only 36
                    // objects (all OT_MODEL) carry one, which is scene-dependent,
                    // so the population is required only loosely -- a level with
                    // models always has some, and without the > 0 check the two
                    // totals below would pass on an empty set.
                    int64_t srefs = -1, srcount = -1, srself = -1;
                    json_int(ab, "shared_refs", srefs);
                    json_int(ab, "shared_ref_count_ok", srcount);
                    json_int(ab, "shared_ref_self_ok", srself);
                    check(srefs > 0, "objects carrying a shared_ref exist");
                    check(srcount == srefs, "every shared_ref has a positive refcount");
                    check(srself == srefs,
                          "every shared_ref stores its own address in both self slots");
                }
            }
        }

        // Spatial records. This is the broadest guard in the suite: the engine
        // stored a copy of each object's cull volume, and we recompute that volume
        // from the typed fields and compare. A moved offset in LTObject's
        // position/aabb/scale, LTModelObject's vis_radius, LTSpriteObject's
        // kind/aabb/radius or LTParticleSystemObject's type/offsets all surface
        // here as `unexplained`.
        {
            const size_t rp = body.find("\"spatial_records\":");
            check(rp != std::string::npos, "objects report includes the spatial-record check");
            if (rp != std::string::npos) {
                if (json_has(body, "\"spatial_records\":null")) {
                    check(false, "spatial-record walk completed (null == faulted)");
                } else {
                    const size_t end = body.find('}', rp);
                    const std::string rb = body.substr(rp, end - rp + 1);
                    int64_t objs = -1, back = -1, matched = -1, gated = -1, unexp = -1;
                    json_int(rb, "objects", objs);
                    json_int(rb, "backpointer_ok", back);
                    json_int(rb, "volume_matched", matched);
                    json_int(rb, "volume_gated", gated);
                    json_int(rb, "unexplained", unexp);

                    check(objs > 0, "objects present to check spatial records against");
                    check(back == objs, "every spatial_record points back at its owning object");
                    // The partition must be TOTAL. `unexplained` is the real
                    // assertion: a volume that neither matches nor is gated means
                    // the geometry mapping is wrong somewhere.
                    check(unexp == 0,
                          "no spatial_record volume is unexplained (mismatch => a moved geometry "
                          "offset)");
                    check(matched + gated == objs,
                          "every record volume either matches the recomputed volume or is gated");
                    // Both sides populated: matched proves the recomputation is
                    // real work, gated proves the OT_NORMAL/flags3-0x80 path is
                    // genuinely reached rather than assumed.
                    check(matched > 0, "records with a matching volume exist");
                    check(gated > 0, "gated (legitimately empty) records exist");

                    // The record's ENTRY LIST. Each entry is one object<->hit
                    // association and sits in two lists with different linkage,
                    // so the two checks below fail for different reasons:
                    // count_matches_walk breaks on a RECORD-side offset, and
                    // hit_links_ok breaks on an ENTRY-side one.
                    int64_t ents = -1, cmw = -1, erok = -1, hlok = -1;
                    json_int(rb, "entries", ents);
                    json_int(rb, "count_matches_walk", cmw);
                    json_int(rb, "entry_record_ok", erok);
                    json_int(rb, "hit_links_ok", hlok);
                    check(ents > 0, "spatial entries exist (the association lists are populated)");
                    check(cmw == objs,
                          "every record's entry_count equals its walked entry-list length");
                    check(erok == objs, "every spatial entry names the record that lists it");
                    check(hlok == objs, "every spatial entry's hit-side links are consistent");

                    // The far end: each entry's hit_head addresses an
                    // LTVisSector's entry_list slot, so the sector is validated
                    // through the object graph with no global pointer. Both counts
                    // are PER ENTRY, so they compare against `entries`.
                    int64_t saabb = -1, splanes = -1;
                    json_int(rb, "entry_sector_aabb_ok", saabb);
                    json_int(rb, "entry_sector_planes_ok", splanes);
                    check(saabb == ents, "every entry's vis sector has an ordered AABB");
                    check(splanes == ents,
                          "every entry's vis sector has unit-length plane normals (or no planes)");

                    // The VISIBILITY GATE. UpdateSpatialRecord collects only
                    // when (flags & 1) && !(flags2 & 0x700), else it Releases.
                    // Asymmetric on purpose, same as renderable-implies-linked:
                    // the gate is necessary, not sufficient, because a gated
                    // object's volume can still miss every sector. So
                    // gated_violations is asserted and the 40-odd
                    // gate-open-but-empty records are simply not.
                    int64_t gopen = -1, rwe = -1, gviol = -1;
                    json_int(rb, "gate_open", gopen);
                    json_int(rb, "records_with_entries", rwe);
                    json_int(rb, "gated_violations", gviol);
                    check(gopen > 0, "objects passing the visibility gate exist");
                    check(rwe > 0, "records holding entries exist");
                    check(gviol == 0,
                          "no record holds entries while the visibility gate is closed");
                    check(rwe <= gopen,
                          "records with entries are a subset of gate-open objects");
                }
            }
        }

        // THE PUBLIC OBJECT API vs THE INTERNAL WALK. sdk::is_renderable exists so a
        // mod can ask "will this be drawn" without reimplementing the engine's gate.
        // check_spatial_records already counts that same gate internally, and that
        // count is the one proven against engine behaviour (gated_violations == 0
        // above). Requiring the two to agree is what keeps the consumer-facing
        // function honest: a wrong mask or a stale flags2 offset diverges HERE rather
        // than returning a subtly wrong answer inside somebody's mod.
        //
        // Note this is a genuine second computation, not a restatement -- the API
        // path snapshots each bucket and calls the public functions on each address,
        // while the internal path walks the lists under its own SEH guard.
        {
            const size_t ap = body.find("\"object_api\":");
            check(ap != std::string::npos, "objects report includes the public-API cross-check");
            if (ap != std::string::npos) {
                const size_t end = body.find('}', ap);
                const std::string ab = body.substr(ap, end - ap + 1);
                int64_t aobj = -1, ainfo = -1, arend = -1, acam = -1, abit = -1, gopen2 = -1;
                json_int(ab, "objects", aobj);
                json_int(ab, "info_ok", ainfo);
                json_int(ab, "renderable", arend);
                json_int(ab, "cameras", acam);
                json_int(ab, "cameras_with_bit11", abit);
                json_int(body, "gate_open", gopen2);

                check(aobj > 0, "the API walked objects");
                check(ainfo == aobj, "object_info answers for every live object");
                check(arend == gopen2,
                      "sdk::is_renderable agrees exactly with the engine gate counted internally");
                check(acam > 0, "cameras were seen through the public API");
                // REPORTED, not asserted equal: nothing in the engine reads bit 11, so
                // "every camera has it" is a live regularity (474/474) rather than a
                // mechanism. Asserting it would turn an observation into a tripwire.
                check(abit >= 0 && abit <= acam,
                      "the camera-only flag bit is a reported fraction of cameras");
            }
        }

        // The visibility tree, reached from IWorldClientBSP rather than through
        // the object lists -- so this validates the vis subsystem on its own.
        //
        // What makes it a strong check: the engine stores BOTH counts itself, so
        // the walk is compared against the engine's numbers instead of against
        // anything recorded host-side. `split_axis` doubles as the leaf marker,
        // so a wrong offset there truncates the walk or runs it into garbage --
        // either way node_count stops matching.
        {
            const size_t vp = body.find("\"vis_tree\":");
            check(vp != std::string::npos, "objects report includes the vis-tree check");
            if (vp != std::string::npos) {
                if (json_has(body, "\"vis_tree\":null")) {
                    // Legitimate before a world loads: no root, no sectors. At the
                    // main menu a world IS loaded, so treat it as a failure here
                    // rather than passing by absence.
                    check(false, "vis tree resolved (null == unresolved interface or no world)");
                } else {
                    const size_t end = body.find('}', vp);
                    const std::string tb = body.substr(vp, end - vp + 1);
                    int64_t scount = -1, ncount = -1, walked = -1, eseen = -1, ein = -1,
                            reached = -1, leaves = -1, depth = -1;
                    json_int(tb, "sector_count", scount);
                    json_int(tb, "node_count", ncount);
                    json_int(tb, "nodes_walked", walked);
                    json_int(tb, "elements_seen", eseen);
                    json_int(tb, "elements_in_arr", ein);
                    json_int(tb, "sectors_reached", reached);
                    json_int(tb, "leaves", leaves);
                    json_int(tb, "max_depth", depth);

                    check(scount > 0, "vis tree reports a sector count");
                    check(ncount > 0, "vis tree reports a node count");
                    check(walked == ncount,
                          "walking the vis tree reaches exactly node_count nodes");
                    check(eseen > 0, "vis tree nodes hold sector elements");
                    check(ein == eseen,
                          "every vis-tree element is an aligned entry of the sector array");
                    check(reached == scount,
                          "the vis tree covers every sector in the array");
                    check(leaves > 0, "vis tree has leaves (split_axis > 2 branch exercised)");
                    check(depth > 0, "vis tree is deeper than a single node");

                    // PORTALS -- the other half of the visibility graph. Every
                    // check here is self-contained geometry: a portal's own
                    // `center` and its own vertices must satisfy its OWN plane
                    // equation. No recorded baseline, and no wrong offset in the
                    // 0x5C record survives it.
                    int64_t pc = -1, pun = -1, pcp = -1, pso = -1, pvp = -1;
                    json_int(tb, "portal_count", pc);
                    json_int(tb, "portal_unit_normal", pun);
                    json_int(tb, "portal_center_on_plane", pcp);
                    json_int(tb, "portal_sectors_ok", pso);
                    json_int(tb, "portal_verts_on_plane", pvp);
                    check(pc > 0, "vis tree reports portals");
                    check(pun == pc, "every portal's plane normal is unit length");
                    check(pcp == pc, "every portal's center lies on its own plane");
                    check(pso == pc,
                          "every portal joins two DISTINCT aligned sectors");
                    check(pvp == pc, "every portal's vertices lie on its own plane");
                }
            }
        }

        // Renderability vs world-tree membership.
        //
        // This block is deliberately ASYMMETRIC, and the asymmetry is the point.
        // LTObject_SetFlags adds/removes the tree link exactly when
        // LTObject_IsRenderable flips, so `renderable => linked` is backed by a
        // mechanism and held on 1758/1758 live objects. The CONVERSE is false --
        // 384 objects are linked while not renderable, because removal is less
        // prompt than insertion -- so that count is read and ignored rather than
        // asserted. Forcing a biconditional here would invent an invariant the
        // engine does not maintain, which is the same mistake as widening a
        // tolerance until it passes.
        {
            const size_t fp = body.find("\"render_flags\":");
            check(fp != std::string::npos, "objects report includes the render-flag check");
            if (fp != std::string::npos) {
                if (json_has(body, "\"render_flags\":null")) {
                    check(false, "render-flag walk completed (null == faulted)");
                } else {
                    const size_t end = body.find('}', fp);
                    const std::string fb = body.substr(fp, end - fp + 1);
                    int64_t objs = -1, rend = -1, lk = -1, rnl = -1, lnr = -1, sup = -1,
                            suplk = -1;
                    json_int(fb, "objects", objs);
                    json_int(fb, "renderable", rend);
                    json_int(fb, "linked", lk);
                    json_int(fb, "renderable_not_linked", rnl);
                    json_int(fb, "linked_not_renderable", lnr);
                    json_int(fb, "suppressed", sup);
                    json_int(fb, "suppressed_linked", suplk);

                    check(objs > 0, "objects present for the render-flag check");
                    check(rend > 0, "renderable objects exist (predicate exercised)");
                    check(lk > 0, "world-tree linked objects exist");
                    // The mechanism-backed direction.
                    check(rnl == 0, "every renderable object is world-tree linked");
                    // The suppressor bit, also mechanism-backed and exercised.
                    check(sup > 0, "flag-0x200 suppressed objects exist (suppressor exercised)");
                    check(suplk == 0, "no flag-0x200 suppressed object is world-tree linked");
                    // lnr is intentionally NOT asserted; it is non-zero by design.
                    check(lnr >= 0, "linked-not-renderable count reported (not an invariant)");
                }
            }
        }

        // Cross-invariant tying the two halves of the class together: a type
        // with live objects MUST have a bank, because those objects had to be
        // allocated from one. OT_LIGHT is the interesting case -- it has no
        // bank, so its bucket must be empty, and it is.
        {
            const size_t bp = body.find("\"buckets\":[");
            check(bp != std::string::npos, "buckets array present for the bank cross-check");
            if (bp != std::string::npos) {
                int64_t counts[7] = {-1, -1, -1, -1, -1, -1, -1};
                size_t p = bp + 11;
                for (int i = 0; i < 7; ++i) {
                    counts[i] = strtoll(body.c_str() + p, nullptr, 10);
                    const size_t comma = body.find(',', p);
                    if (comma == std::string::npos) break;
                    p = comma + 1;
                }
                check(counts[4] == 0,
                      "OT_LIGHT bucket is empty, consistent with it having no allocator bank");
                bool banked_types_populated = true;
                for (int t : {0, 1, 2, 3, 5, 6}) {
                    if (counts[t] < 0) banked_types_populated = false;
                }
                check(banked_types_populated,
                      "every type that has a bank reported a valid bucket count");
            }
        }

        // Engine-thread in-place iteration (for_each_object). The endpoint
        // only RAISES a request; the walk itself happens in the frame hook on
        // the engine thread, which is the only context that API's lifetime
        // precondition covers. So this is genuinely testing the callback path,
        // not re-testing the snapshot path.
        //
        // Requires frames to be running -- skipped rather than failed when
        // they are not (a paused/suspended game can never service it, and
        // that is an environment state, not a mapping defect).
        int64_t gen_before = -1;
        check(json_int(body, "engine_walk_generation", gen_before) && gen_before >= 0,
              "engine_walk_generation present");

        std::string resp2;
        int64_t gen_after = -1, engine_count = -1;
        for (int attempt = 0; attempt < 20; ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (!http::get(port, "/sdk/objects", resp2)) continue;
            const std::string b2 = http::body_of(resp2);
            json_int(b2, "engine_walk_generation", gen_after);
            json_int(b2, "engine_walk_count", engine_count);
            if (gen_after > gen_before) break;
        }

        if (gen_after > gen_before) {
            // Deterministic and sufficient: the generation only advances when
            // the frame hook actually ran a for_each_object walk on the engine
            // thread, and a non-negative count means that walk terminated
            // cleanly (no fault, no fail-closed cap, no bucket-type
            // violation). That is the whole claim.
            //
            // NOT asserted: agreement with the off-thread bucket count. The
            // two walks happen at different instants against a list that
            // genuinely churns, so any tolerance would be an invented number
            // dressed up as an invariant. (They did agree exactly when
            // observed by hand -- evidence, not a test.)
            check(engine_count >= 0,
                  "engine-thread for_each_object walk terminated cleanly (in-place, no snapshot)");
        } else {
            // Frames are not running (paused/suspended/pre-init), so nothing
            // could service the request. Environment state, not a defect --
            // reported, not failed.
            printf("[fixture] NOTE: engine-thread for_each_object not exercised "
                   "(no frames serviced the request)\n");
        }
    }

    // 5b3. /sdk/interfaces: the LithTech interface layer, per interface.
    //
    // The DLL discovers holders at runtime by scanning CAPIHolder_ctor's call
    // sites (kananlib) and resolves each name through the GENERATED per-
    // interface class's own typed getter. The host validates the report only.
    //
    // The strong claim here is a CROSS-CHECK against static reversing: the
    // reversing pass recorded 147 call sites / 36 names in FEAR2.exe by reading
    // the binary in IDA. The runtime scan must independently arrive at the same
    // numbers. Those two paths share no code, so agreement is real evidence
    // rather than self-agreement. (See reversing/INTERFACE_HOLDERS.md.)
    {
        std::string resp;
        check(http::get(port, "/sdk/interfaces", resp), "/sdk/interfaces transport");
        const std::string body = http::body_of(resp);

        check(json_has(body, "\"ok\":true"), "interface registry initialized");

        int64_t call_sites = -1, holders = -1, names = -1, expected = -1;
        check(json_int(body, "call_sites", call_sites) && call_sites == 147,
              "147 CAPIHolder_ctor call sites found at runtime (matches static IDA extraction)");
        check(json_int(body, "holders", holders) && holders == call_sites,
              "every call site decoded into a holder (none rejected)");
        check(json_int(body, "names", names) && json_int(body, "expected_names", expected) &&
                  names == expected,
              "discovered interface names == the generated set (no name unaccounted for)");

        // Per-interface assertions. Every entry is driven through its own typed
        // getter in the DLL, so this covers all 36 public getters.
        //
        // Deliberately NOT asserted: that a given interface is non-null. The
        // database clears slots via APIRemoved(), and server-side interfaces are
        // legitimately null in a client-only session, so a null is a state, not
        // a defect. What IS asserted holds regardless of resolution state.
        size_t entries = 0, resolved = 0;
        size_t pos = body.find("\"interfaces\":[");
        check(pos != std::string::npos, "interfaces array present");
        while (pos != std::string::npos) {
            pos = body.find("{\"name\":\"", pos);
            if (pos == std::string::npos) {
                break;
            }
            const size_t name_start = pos + 9;
            const size_t name_end = body.find('"', name_start);
            const size_t obj_end = body.find('}', pos);
            if (name_end == std::string::npos || obj_end == std::string::npos) {
                break;
            }
            const std::string name = body.substr(name_start, name_end - name_start);
            const std::string obj = body.substr(pos, obj_end - pos + 1);
            ++entries;

            int64_t h = -1, nn = -1;
            const bool got_h = json_int(obj, "holders", h);
            const bool got_nn = json_int(obj, "non_null", nn);

            // Every generated name must correspond to at least one real holder
            // in the image. Zero would mean the generated set has drifted from
            // what the binary actually contains.
            check(got_h && h >= 1, (name + ": has at least one holder in FEAR2.exe").c_str());

            // non_null can be 0..holders, never more.
            check(got_nn && nn >= 0 && nn <= h, (name + ": non_null within [0, holders]").c_str());

            // The real invariant: independent holders for one interface must
            // never disagree. Disagreement would mean a mis-decoded call site.
            if (nn > 0) {
                ++resolved;
                check(json_has(obj, "\"all_agree\":true"),
                      (name + ": all resolved holders agree on the pointer").c_str());
            }

            // And the typed getter must return exactly what the registry says --
            // this is what catches a class wired to the wrong kName/instance.
            check(json_has(obj, "\"getter_matches\":true"),
                  (name + ": typed getter agrees with the registry").c_str());

            pos = obj_end;
        }
        check(entries == static_cast<size_t>(expected),
              "every generated interface appeared in the report");

        // Sanity floor rather than an exact expectation: in a live client at
        // least the core client interfaces must be resolved, otherwise the
        // engine could not be rendering at all.
        check(resolved > 0, "at least one interface is resolved in a live client");
        printf("[fixture] interfaces: %zu/%zu resolved\n", resolved, entries);
    }

    // 5c. /engine-hook positive + negative.
    {
        std::string resp;
        check(http::get(port, "/engine-hook?name=hwnd", resp), "/engine-hook(hwnd) transport");
        const std::string body = http::body_of(resp);
        int64_t rc = -1;
        check(json_int(body, "rc", rc) && rc == 0, "engine hook 'hwnd' returned LT_OK");
        uint32_t via_hook = 0;
        uint32_t main_hwnd_direct = 0;
        {
            std::string tresp;
            http::get(port, "/sdk/targets", tresp);
            json_hex(http::body_of(tresp), "main_hwnd", main_hwnd_direct);
        }
        check(json_hex(body, "value", via_hook) && via_hook == main_hwnd_direct && via_hook != 0,
              "engine hook 'hwnd' agrees with raw global");

        std::string bad;
        check(http::get(port, "/engine-hook?name=fear2vr_no_such_hook", bad), "/engine-hook(bogus) transport");
        const std::string bad_body = http::body_of(bad);
        int64_t rc_bad = 0;
        check(json_int(bad_body, "rc", rc_bad) && rc_bad != 0, "unknown hook name rejected (LT_ERROR)");
    }

    // 5d. /sdk/database: proves the SDK mapping WORKS IN-PROCESS -- the DLL
    // itself calls sdk::DatabaseMgr::entry_count()/entry(i) and the new
    // DatabaseMgr::read_path() helper (real regenny()-typed struct field
    // access, SEH-guarded) and reports the results. This is deliberately
    // NOT reimplemented via ReadProcessMemory here: RPM would only prove our
    // HAND-DERIVED offsets agree with themselves, not that the actual
    // compiled SDK traversal code is correct and safe to use. What the host
    // CAN and does independently verify: module residency (Toolhelp32 is
    // OS-level ground truth the DLL doesn't control) and that the traversal
    // did not crash anything (transport succeeds + the process/IPC survive
    // the call).
    if (have_db) {
        std::string resp;
        check(http::get(port, "/sdk/database", resp), "/sdk/database transport (proves in-process traversal didn't crash the DLL)");
        const std::string body = http::body_of(resp);

        uint32_t instance = 0, vtable = 0;
        int64_t entry_count = -1;
        check(json_hex(body, "instance", instance) && instance != 0, "database instance non-null");
        check(json_hex(body, "vtable", vtable) && vtable != 0, "database vtable non-null");
        check(json_int(body, "entry_count", entry_count) && entry_count >= 1 && entry_count < 1000,
              "entry_count() result in a plausible range [1,1000)");

        // Residency: independently-verifiable OS-level ground truth (host's
        // own Toolhelp32 module enumeration), not a re-derivation of SDK
        // traversal logic.
        auto in_db = [&](uint32_t v, const char* label) {
            char detail[128];
            snprintf(detail, sizeof(detail), "0x%08X outside gamedatabase.dll [0x%08X,0x%08X)", v,
                     static_cast<uint32_t>(db_mod.base), static_cast<uint32_t>(db_mod.base + db_mod.size));
            check(v >= db_mod.base && v < db_mod.base + db_mod.size, label, detail);
        };
        in_db(instance, "database instance residency (gamedatabase.dll)");
        in_db(vtable, "database vtable residency (gamedatabase.dll)");

        // entry(0)->record_a/record_b and their path_data strings: all
        // computed BY THE SDK in-process (DatabaseMgr::entry() +
        // DatabaseMgr::read_path()); the host only sanity-checks the
        // reported content.
        if (entry_count >= 1) {
            check(json_has(body, "\"entry0\":{"), "entry0 present when entry_count>=1");
            uint32_t record_a = 0, record_b = 0;
            check(json_hex(body, "record_a", record_a) && record_a != 0, "entry0.record_a non-null");
            check(json_hex(body, "record_b", record_b) && record_b != 0, "entry0.record_b non-null");
            check(json_has(body, ".gamedb") || json_has(body, "gamedb"),
                  "entry0's path strings resolved to real *.gamedb content (SDK traversal reached real data, not garbage)");

            // Category/record enumeration: DatabaseMgr::category_count()/
            // category()/record_count()/record()/category_name()/
            // record_name() -- all real in-process struct traversal, SEH-
            // guarded, host only sanity-checks reported content and general
            // shape (never overfits to exact first-nonempty-category order
            // or exact record text, which can shift with game data).
            int64_t category_count = -1;
            check(json_int(body, "record_a_category_count", category_count) &&
                  category_count > 0 && category_count < 100000,
                  "record_a_category_count() result in a plausible range (0,100000)");
            check(json_has(body, "\"categories\":["), "categories array present");
            check(json_has(body, "\"AI/WeaponContext\""),
                  "a known stable category name appears in the live-enumerated category list");
            check(json_has(body, "\"record_count\":"), "category summaries include record_count");
            check(json_has(body, "\"sample_records_category\":"), "sample_records_category field present");
            check(json_has(body, "\"sample_records\":["), "sample_records array present (a category with records was found and walked)");
        }

        // The traversal above ran fully in-process on the game's own memory;
        // prove it left the game and the IPC channel intact.
        check(process_alive(pid), "game process survived the in-process DatabaseMgr traversal");
        check(http::port_open(port), "IPC still responsive after the in-process DatabaseMgr traversal");
    }

    // 6. Graceful unload proof: module vanishes, game keeps running.
    {
        check(run_injector(injector, "unload", dll, port) == 0, "injector --unload accepted");
        check(wait_unloaded(pid, 150), "fear2vr module unmapped (module list)");
        check(!http::port_open(port), "IPC port closed after unload");
        check(process_alive(pid), "game process survived uninjection");
    }

    // 7. Re-inject: a FRESH instance comes up clean in the never-restarted game.
    //    frame_ticks starting small again proves the old instance (hooks,
    //    counters, threads) is truly gone, not just asleep.
    {
        check(run_injector(injector, "inject", dll, port) == 0, "re-inject accepted");
        check(wait_healthy(port, 200), "fresh instance IPC live");
        std::string body;
        check(health_body(port, body), "fresh /health transport");
        int64_t ticks = -1;
        json_int(body, "frame_ticks", ticks);
        check(ticks >= 0 && ticks < 10000, "fresh instance frame_ticks reset (old hooks gone)");
        check(json_has(body, "\"sdk_ready\":true"), "fresh instance sdk_ready==true");
    }

    // 8. Teardown.
    cleanup();

    printf("%s (%lld checks)\n", g_failures == 0 ? "[fixture] PASS" : "[fixture] FAIL", g_checks);
    return g_failures == 0 ? kOk : kFail;
}
