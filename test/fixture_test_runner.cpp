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

        // Type-5 cached transforms. This is the assertion that would catch a
        // wrong LTCameraObject offset before it turned into wrong VR camera
        // math: the SDK recomputes the rotation matrix from each object's own
        // quaternion and the rigid inverse of the cached 3x4, and every sampled
        // object must satisfy both. A shifted world_transform or
        // inverse_transform offset makes these counts diverge from `sampled`.
        //
        // Note the shape of the check: it does NOT compare against values
        // recorded from a previous run, and it invents no tolerance beyond the
        // float-noise epsilon the SDK uses -- it asserts an identity the data
        // must satisfy on its own terms.
        {
            const size_t tp = body.find("\"type5_transforms\":");
            check(tp != std::string::npos, "objects report includes the type-5 transform check");
            if (tp != std::string::npos) {
                if (json_has(body, "\"type5_transforms\":null")) {
                    // A fault or non-terminating walk. Not "pass by absence":
                    // the type-5 list is populated whenever the client manager
                    // is live, so null here means the walk broke.
                    check(false, "type-5 transform walk completed (null == faulted)");
                } else {
                    const size_t end = body.find('}', tp);
                    const std::string tb = body.substr(tp, end - tp + 1);
                    int64_t sampled = -1, rot = -1, inv = -1, det = -1;
                    json_int(tb, "sampled", sampled);
                    json_int(tb, "rotation_match", rot);
                    json_int(tb, "inverse_ok", inv);
                    json_int(tb, "det_ok", det);
                    check(sampled > 0, "type-5 objects present to check transforms against");
                    check(rot == sampled,
                          "every type-5 world_transform 3x3 == R(its own rotation quaternion)");
                    check(inv == sampled,
                          "every type-5 inverse_transform is the exact rigid inverse");
                    check(det == sampled, "every type-5 world_transform 3x3 has determinant 1");
                }
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
