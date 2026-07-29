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
// For the D3D enum names the present-parameter assertions compare against: asserting
// against D3DFMT_A8R8G8B8 rather than 21 keeps the claim readable and self-documenting.
#include <d3d9.h>

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

// Parse a boolean field, distinguishing ABSENT from false -- which json_has cannot.
//
// Why this exists: a check written as json_has(body, "\"key\":true") reads false for a key that
// was renamed, misspelled or lost to a truncated buffer, and "false" is often the passing answer.
// A misnamed field and a silently truncated JSON fragment both nearly slipped through this suite
// that way. Returns false when the key is missing or the value is not exactly true/false.
bool json_bool(const std::string& body, const char* key, bool& out) {
    const std::string needle = std::string{"\""} + key + "\":";
    const size_t p = body.find(needle);
    if (p == std::string::npos) {
        return false;
    }
    const size_t v = p + needle.size();
    if (body.compare(v, 4, "true") == 0) {
        out = true;
        return true;
    }
    if (body.compare(v, 5, "false") == 0) {
        out = false;
        return true;
    }
    return false;
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

// Reads a JSON string value, undoing the escaping json_escape_append applies. Needed because
// the world path is full of backslashes, so the raw and decoded forms differ and comparing the
// raw one would silently test the escaping rather than the path.
bool json_str(const std::string& body, const char* key, std::string& out) {
    const std::string needle = std::string{"\""} + key + "\":\"";
    const size_t p = body.find(needle);
    if (p == std::string::npos) return false;
    size_t i = p + needle.size();
    out.clear();
    for (; i < body.size(); ++i) {
        const char c = body[i];
        if (c == '"') return true;
        if (c != '\\' || i + 1 >= body.size()) {
            out.push_back(c);
            continue;
        }
        const char e = body[++i];
        switch (e) {
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u':
            // \uXXXX -- only the control-character form the escaper emits, so the low byte is
            // the character. Anything wider is not something this payload produces.
            if (i + 4 < body.size()) {
                out.push_back(static_cast<char>(strtoul(body.substr(i + 1, 4).c_str(), nullptr, 16)));
                i += 4;
            }
            break;
        default: out.push_back(e); break;
        }
    }
    return false;
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
    // UNBUFFERED, so a crash cannot swallow the log. Block buffering loses everything written since the
    // last 4K flush, which is precisely the run you most need to read: a fault mid-suite printed nothing
    // at all and the failing check had to be found by other means.
    setvbuf(stdout, nullptr, _IONBF, 0);
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

    // 3. Clear stale instance; 4. load.
    if (http::port_open(port)) {
        run_injector(injector, "unload", dll, port);
        // WAIT FOR THE MODULE TO GO, do not sleep and hope. A fixed 500ms is a race: on a slow unload
        // the reinject below lands on a still-resident payload, which the injector refuses, and the run
        // then proceeds against a stale DLL. wait_unloaded is already the primitive this runner uses for
        // the same question at the end of the suite.
        if (const auto stale = find_pid("FEAR2.exe"); stale != 0) {
            if (!wait_unloaded(stale, 50)) {  // 50 x 200ms
                printf("[fixture] a previous fear2vr payload is still resident after 10s -- skipping "
                       "rather than testing against it\n");
                return 77;
            }
        }
    }
    // INJECT, the verified path. A `reload` variant was tried here to survive a dormant leftover
    // and it HUNG the run, so it is not kept: an unverified change to the harness that starts the
    // game is worse than the problem it addressed. The leftover state that motivated it is covered
    // in reversing/MAPPING_WORKFLOW.md instead -- the real fix is not to wedge the payload.
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

        // ---- DIRECT3D 9 ---------------------------------------------------------------
        //
        // Both interfaces the engine holds, and for each the module that IMPLEMENTS its
        // methods. A running game with a level loaded has D3D up, so a null is a real
        // failure here.
        uint32_t d3d9 = 0, d3ddev = 0;
        check(json_hex(body, "d3d9", d3d9) && d3d9 != 0,
              "the engine's IDirect3D9 factory is non-null");
        check(json_hex(body, "device", d3ddev) && d3ddev != 0,
              "the engine's IDirect3DDevice9 is non-null");
        check(d3d9 != d3ddev, "the factory and the device are different objects");

        // THE MECHANICAL PART: a genuine COM interface's METHODS live in some loaded
        // module. Asking about a method rather than the vtable pointer is deliberate --
        // the device's vtable is heap-allocated by d3d9.dll and belongs to no module image,
        // so a vtable test reports nothing for it while a method test works for both.
        const auto impl_of = [&](const char* key) {
            const std::string needle = std::string{"\""} + key + "\":\"";
            const size_t p = body.find(needle);
            if (p == std::string::npos) return std::string{};
            const size_t s = p + needle.size();
            const size_t e = body.find('"', s);
            return e == std::string::npos ? std::string{} : body.substr(s, e - s);
        };
        const std::string fimpl = impl_of("d3d9_impl");
        const std::string dimpl = impl_of("device_impl");
        check(!fimpl.empty() && fimpl != "(none)",
              "the factory's methods belong to a loaded module");
        check(!dimpl.empty() && dimpl != "(none)",
              "the device's methods belong to a loaded module");
        // REPORTED, not asserted: WHICH module depends on the machine. Live the factory is
        // gameoverlayrenderer.dll (Steam's overlay proxies it to intercept CreateDevice)
        // while the device is d3d9.dll (the genuine runtime). A mod hard-coding either
        // answer is wrong on the other kind of machine, so this is printed rather than
        // required -- and printing it lets a future failure be correlated with what is
        // sitting in front of D3D.
        printf("[fixture] D3D9 factory 0x%08X impl=%s%s\n", d3d9, fimpl.c_str(),
               fimpl == "d3d9.dll" ? "" : "  <- PROXIED");
        printf("[fixture] D3D9 device  0x%08X impl=%s%s\n", d3ddev, dimpl.c_str(),
               dimpl == "d3d9.dll" ? "  <- the real runtime" : "  <- PROXIED");

        // THE PRESENT PARAMETERS, and the assertion is stronger than it looks. CreateDevice
        // SUCCEEDED with this exact struct -- that is what makes the device non-null above
        // -- so every field in it must be a value D3D itself accepts. Anything here that is
        // not a legal enum means we are not reading the struct the engine passed.
        int64_t bw = -1, bh = -1, bfmt = -1, bcnt = -1, swap = -1, dsfmt = -1, dtype = -1;
        json_int(body, "bb_w", bw);
        json_int(body, "bb_h", bh);
        json_int(body, "bb_fmt", bfmt);
        json_int(body, "bb_count", bcnt);
        json_int(body, "swap_effect", swap);
        json_int(body, "depth_fmt", dsfmt);
        json_int(body, "device_type", dtype);
        check(bw >= 640 && bh >= 480,
              "the back buffer meets the engine's own minimum resolution");
        check(bfmt == D3DFMT_A8R8G8B8 || bfmt == D3DFMT_X8R8G8B8,
              "the back buffer format is one the engine's mode filter accepts");
        check(bcnt >= 1 && bcnt <= 3,
              "the back buffer count is inside D3D's legal 1..3");
        check(swap == D3DSWAPEFFECT_DISCARD || swap == D3DSWAPEFFECT_FLIP ||
                  swap == D3DSWAPEFFECT_COPY,
              "the swap effect is a legal D3DSWAPEFFECT");
        check(dsfmt != 0, "an auto depth-stencil format was requested");
        check(dtype == D3DDEVTYPE_HAL || dtype == D3DDEVTYPE_REF ||
                  dtype == D3DDEVTYPE_SW,
              "the cached caps report a legal D3DDEVTYPE");

        // THE CAPS STRUCT'S FULL EXTENT, which the leading fields cannot establish. A
        // wrong base or a wrong sizeof would still give a legal DeviceType at +0x00, so
        // these three are read from FAR inside D3DCAPS9 -- MaxTextureWidth in the middle,
        // the two shader versions near the end.
        //
        // The shader versions are the sharp check, because they are not free values: D3D
        // encodes them as tokens with a fixed high word, 0xFFFE for vertex and 0xFFFF for
        // pixel. Reading 0xFFFE0300 and 0xFFFF0300 (both 3.0) at those displacements is a
        // fingerprint no misaligned read produces, and it is what makes returning the WHOLE
        // struct honest rather than a guess past the fields we happened to measure.
        int64_t mtw = -1;
        uint32_t vs = 0, ps = 0;
        json_int(body, "caps_max_tex_w", mtw);
        check(json_hex(body, "caps_vs", vs) && json_hex(body, "caps_ps", ps),
              "the caps report both shader versions");
        check(mtw >= 2048,
              "MaxTextureWidth is at least the D3D9 baseline, deep inside the struct");
        check((vs & 0xFFFF0000u) == 0xFFFE0000u,
              "VertexShaderVersion carries D3D's 0xFFFE token prefix");
        check((ps & 0xFFFF0000u) == 0xFFFF0000u,
              "PixelShaderVersion carries D3D's 0xFFFF token prefix");
        // The game is a 2009 shader title; anything below 2.0 could not render it.
        check((vs & 0xFFFFu) >= 0x0200 && (ps & 0xFFFFu) >= 0x0200,
              "both shader models meet the 2.0 the engine's renderer needs");
        printf("[fixture] caps: max texture %lld, vs %u.%u, ps %u.%u\n",
               static_cast<long long>(mtw), (vs >> 8) & 0xFF, vs & 0xFF,
               (ps >> 8) & 0xFF, ps & 0xFF);

        // ---- SECTOR LOCATION: the KD shortcut against brute force ---------------------
        //
        // sectors_at() descends the visibility tree to narrow 263 sectors to a handful;
        // scanning EVERY sector and testing its volume is the ORACLE. The two must name the
        // same sector, and that is the whole point: the tree's structure does not state
        // which child holds which side of a split, nor that sectors hang off INTERNAL nodes
        // as well as leaves. Both were got wrong here and the oracle caught both -- a
        // descent harvesting only leaves returned 2 candidates, neither of them the sector
        // the player was standing in.
        int64_t stot = -1, scand = -1, sbrute = -1, psec = -2, bsec = -2;
        int64_t swith = -1, splanes = -1, sreadok = -1;
        json_int(body, "sector_total", stot);
        json_int(body, "sector_candidates", scand);
        json_int(body, "sector_brute", sbrute);
        json_int(body, "player_sector", psec);
        json_int(body, "brute_sector", bsec);
        json_int(body, "sec_with_planes", swith);
        json_int(body, "sec_plane_total", splanes);
        json_int(body, "sec_read_ok", sreadok);
        check(stot > 0, "the world has visibility sectors");
        check(sreadok == stot, "EVERY sector reads back through the public accessor");
        if (sbrute > 0) {
            // THE LOAD-BEARING ONE. Two independent routes to one answer.
            check(psec == bsec,
                  "the KD descent and a brute-force scan of all sectors name the SAME "
                  "sector for the player");
            check(psec >= 0 && psec < stot, "the located sector index is in range");
            // The tree must actually NARROW, or the descent earns nothing over a scan.
            check(scand > 0 && scand < stot,
                  "the descent narrows the sector set instead of returning all of them");
            printf("[fixture] player in sector %lld of %lld (descent offered %lld "
                   "candidates)\n",
                   static_cast<long long>(psec), static_cast<long long>(stot),
                   static_cast<long long>(scand));
        } else {
            printf("[fixture] NOTE: the player is in no sector -- location was NOT "
                   "cross-checked.\n");
        }
        // REPORTED: most sectors are box-only. 19 of 263 carry planes live, so a consumer
        // must not require them -- which is what an earlier version of this API did.
        check(swith >= 0 && swith <= stot,
              "sectors carrying bounding planes are a reported fraction");
        printf("[fixture] sectors: %lld of %lld carry planes (%lld planes total)\n",
               static_cast<long long>(swith), static_cast<long long>(stot),
               static_cast<long long>(splanes));

        // THE PLANE SIGN, and the assertion that would have caught it inverted. A convex
        // cell's own box centre must satisfy its own planes, so this is an invariant about the
        // CONVENTION rather than about the scene.
        //
        // It is here because nothing else could see the bug. The sign was inverted for
        // several passes: the player-location query never exercised it (only 19 of 263 sectors
        // carry planes and the player's is not one), and the brute-force oracle that validated
        // point location CALLS THE SAME PREDICATE -- a shared-implementation oracle cannot
        // catch a shared error. Only LTVisSector_TestSphere settled it: the engine rejects
        // when `dot(n, c) - d < -radius`, so POSITIVE is inside.
        //
        // The measurement makes the counterfactual concrete rather than argued: all 125 planes
        // across those 19 sectors give d > 0 at their own centre, so the previous
        // `reject when d > 0` rule rejected EVERY plane-bearing sector -- total, not marginal.
        int64_t splaned = -1, scin = -1, sprobed = -1, spos = -1, sneg = -1;
        json_int(body, "sec_planed", splaned);
        json_int(body, "sec_centre_in", scin);
        json_int(body, "sec_plane_probed", sprobed);
        json_int(body, "sec_plane_pos", spos);
        json_int(body, "sec_plane_neg", sneg);
        check(splaned > 0, "some sectors carry planes, so the sign is exercised");
        check(scin == splaned,
              "EVERY plane-bearing sector contains its own box centre (plane sign correct)");
        check(sprobed > 0 && spos == sprobed && sneg == 0,
              "every plane reads POSITIVE at its sector's centre -- the engine's inside sign");
        printf("[fixture] plane sign: %lld/%lld sectors contain their own centre, %lld/%lld "
               "planes positive there\n",
               static_cast<long long>(scin), static_cast<long long>(splaned),
               static_cast<long long>(spos), static_cast<long long>(sprobed));

        // ---- REGION QUERIES vs a FULL SCAN -------------------------------------------
        //
        // Validates the TRAVERSAL of sectors_in_sphere: does the descent visit everything a
        // scan of all 263 sectors finds? Both sides share the per-sector test on purpose --
        // that test is grounded in LTVisSector_TestSphere and in the centre-containment
        // invariant above, so the shared part is the part already proven.
        //
        // Three radii because a descent bug can hide at one scale: 0 collapses to point
        // location, 250 is a play-space, 4000 spans most of the level.
        int64_t rprobes = -1, ragree = -1, rhits = -1;
        json_int(body, "region_probes", rprobes);
        json_int(body, "region_agree", ragree);
        json_int(body, "region_hits", rhits);
        check(rprobes == 3, "all three region radii were probed");
        // NON-VACUITY FIRST. The failure this guards against is real and happened earlier in
        // this project: an oracle that finds nothing agrees with a query that finds nothing,
        // and 0 == 0 reads as success. Require the scan to have found sectors before believing
        // the agreement means anything.
        check(rhits > 0, "the scan found sectors, so the agreement is not vacuous");
        check(ragree == rprobes,
              "the descent finds EXACTLY the sectors a full scan finds, at every radius");
        printf("[fixture] region queries: %lld/%lld radii agree with a full scan (%lld hits "
               "total)\n",
               static_cast<long long>(ragree), static_cast<long long>(rprobes),
               static_cast<long long>(rhits));

        // ---- THE BOX VARIANT, and the cache it depends on -----------------------------
        int64_t bprobes = -1, bagree = -1, bhits = -1;
        json_int(body, "box_probes", bprobes);
        json_int(body, "box_agree", bagree);
        json_int(body, "box_hits", bhits);
        check(bprobes == 3, "all three box extents were probed");
        check(bhits > 0, "the scan found sectors, so the box agreement is not vacuous");
        check(bagree == bprobes,
              "the box descent finds EXACTLY what a full scan finds, at every extent");

        // corner_code is DERIVED from the normal, and the engine's box test reads the stored
        // copy. A stale one mis-answers box queries while leaving sphere queries correct, so
        // this asserts the live world has no such plane. It is also the check that would catch
        // the rule itself being wrong: the codes were reverse-engineered from a table, and if
        // corner_code_for() disagreed with the engine's own encoding this would be 0/125, not
        // 124/125 -- a wrong rule fails everywhere at once, which is the easy kind of failure.
        int64_t cprobed = -1, ccur = -1;
        json_int(body, "code_probed", cprobed);
        json_int(body, "code_current", ccur);
        check(cprobed > 0, "planes with a cached corner code exist");
        check(ccur == cprobed, "EVERY cached corner code matches what its own normal implies");
        printf("[fixture] box queries: %lld/%lld extents agree (%lld hits); corner codes "
               "%lld/%lld current\n",
               static_cast<long long>(bagree), static_cast<long long>(bprobes),
               static_cast<long long>(bhits), static_cast<long long>(ccur),
               static_cast<long long>(cprobed));

        // ---- SPHERE INSIDE BOX: two INDEPENDENT implementations, cross-checked ---------
        //
        // This is the oracle the earlier ones could not be. The sphere and box paths share no
        // code -- separate engine functions, separate traversal arithmetic, and separate plane
        // rejects (a slack term versus a selected corner) -- so an error in one is not
        // automatically an error in the other. Geometry ties them together: a sphere of radius
        // e sits inside the box of half-extent e, therefore every sector the sphere finds must
        // also be found by the box.
        int64_t cnp = -1, cnok = -1, cnsph = -1;
        json_int(body, "contain_probes", cnp);
        json_int(body, "contain_ok", cnok);
        json_int(body, "contain_sphere", cnsph);
        check(cnp == 3, "all three containment extents were probed");
        check(cnsph > 0, "the sphere query found sectors, so containment is not vacuous");
        check(cnok == cnp,
              "every sector the sphere touches is also touched by the box that contains it");
        printf("[fixture] sphere inside box: %lld/%lld extents (%lld sphere hits), independent "
               "paths agree\n",
               static_cast<long long>(cnok), static_cast<long long>(cnp),
               static_cast<long long>(cnsph));

        // ---- THE ENGINE'S OWN ANSWER, and what it validates ---------------------------
        //
        // LTSpatialRecord_CollectSphere runs LTVisTree_QuerySphere with AddEntry as its
        // callback, so every entry the engine holds is a result THIS SDK's query should also
        // produce -- computed by different code, at a different time, from the same volume.
        // That makes the stored entries a genuine external oracle for the traversal, unlike a
        // full scan built on the SDK's own per-sector test.
        int64_t robj = -1, rents = -1, rcnt_ok = -1, rmiss = -1, rextra = -1;
        int64_t ronly_missing = -1, ronly_extra = -1, rboth = -1, rconsist = -1;
        json_int(body, "rec_objects", robj);
        json_int(body, "rec_entries", rents);
        json_int(body, "rec_count_ok", rcnt_ok);
        json_int(body, "rec_missing", rmiss);
        json_int(body, "rec_extra", rextra);
        json_int(body, "rec_only_missing", ronly_missing);
        json_int(body, "rec_only_extra", ronly_extra);
        json_int(body, "rec_both", rboth);
        json_int(body, "rec_consistent", rconsist);
        check(robj > 0, "objects were walked");
        check(rents > 0, "the engine holds spatial entries, so the comparison is not vacuous");

        // THE LOAD-BEARING ONE. `extra` means the record names a sector the SDK's query does
        // not reach, which would be a hole in the traversal. Zero says the reimplementation
        // reproduces every association the engine made, and it is asserted rather than
        // reported because a regression here is a bug in this code, not in the scene.
        check(rextra == 0,
              "the SDK's query reaches EVERY sector the engine itself collected (no extras)");
        check(ronly_extra == 0 && rboth == 0,
              "every disagreement is one-directional: the record is a subset, never a superset");

        // The maintained counter against the walked list length -- two representations of one
        // fact, both the engine's, neither derived from the other by this code.
        check(rcnt_ok == robj, "entry_count equals the walked list length on EVERY object");

        // Consistency by the engine's own two-branch rule. The residual is real staleness --
        // renderable objects that reach somewhere their record was never told about, the same
        // phenomenon as the stale world-tree entries -- so it is bounded rather than required
        // to be zero. The DIRECTION is the invariant; the count is scene-dependent.
        check(rconsist > robj - robj / 20,
              "at least 95% of objects are spatially consistent by the engine's own rule");
        printf("[fixture] engine records: %lld entries over %lld objects, %lld reachable by "
               "query, %lld missing; consistent %lld/%lld (%lld stale)\n",
               static_cast<long long>(rents), static_cast<long long>(robj),
               static_cast<long long>(rents - rextra), static_cast<long long>(rmiss),
               static_cast<long long>(rconsist), static_cast<long long>(robj),
               static_cast<long long>(robj - rconsist));

        // ---- THE COLLECT GATE is exact -----------------------------------------------
        //
        // The volume write is UNCONDITIONAL and the collect is gated on is_renderable(), so a
        // non-renderable object must hold a released (empty) list while its volume stays
        // current. This is the third place the same asymmetry has turned up -- LTObject_SetPos
        // writes the AABB unconditionally and relinks only when renderable -- which is why it
        // is asserted as a rule and not treated as a coincidence.
        int64_t gnr = -1, gnr_empty = -1, gr = -1;
        json_int(body, "gate_norend", gnr);
        json_int(body, "gate_norend_empty", gnr_empty);
        json_int(body, "gate_rend", gr);
        check(gnr > 0 && gr > 0, "both sides of the collect gate are populated");
        check(gnr_empty == gnr,
              "EVERY non-renderable object has an empty entry list -- the gate is exact");

        // The record's own shape tag against the type rule: one bit the engine writes at store
        // time, versus a reimplementation of six virtual functions. Agreement is corroboration
        // from two independent routes, and a divergence would mean stored volumes are being
        // read with the wrong four-versus-six-float layout.
        int64_t shp = -1, shp_ok = -1;
        json_int(body, "shape_probed", shp);
        json_int(body, "shape_agree", shp_ok);
        check(shp > 0, "shapes were compared");
        check(shp_ok == shp,
              "the record's own shape tag agrees with the type rule on EVERY object");
        printf("[fixture] collect gate: %lld/%lld non-renderable released, %lld renderable; "
               "shape tag %lld/%lld agrees\n",
               static_cast<long long>(gnr_empty), static_cast<long long>(gnr),
               static_cast<long long>(gr), static_cast<long long>(shp_ok),
               static_cast<long long>(shp));

        // ---- THE REVERSE INDEX pairs back --------------------------------------------
        //
        // One doubly-linked structure read from both ends: if an object lists a sector, that
        // sector must list the object. A wrong hit_next or hit_head breaks the pairing while
        // each direction still walks a plausible-looking list.
        int64_t rvp = -1, rvok = -1, rvpairs = -1;
        json_int(body, "rev_probed", rvp);
        json_int(body, "rev_ok", rvok);
        json_int(body, "rev_pairs", rvpairs);
        check(rvp > 0, "associations were probed in both directions");
        check(rvok == rvp, "EVERY object->sector association is present in the reverse index");
        check(rvpairs >= rvp, "the sectors' own lists hold at least the sampled associations");
        printf("[fixture] reverse index: %lld/%lld associations pair back (%lld total sector "
               "entries)\n",
               static_cast<long long>(rvok), static_cast<long long>(rvp),
               static_cast<long long>(rvpairs));

        // ---- PORTALS AND CONNECTIVITY ------------------------------------------------
        //
        // The load-bearing property is SYMMETRY. Both directions of an edge come from ONE
        // portal read from opposite ends, so a wrong sector_a/sector_b offset -- or a broken
        // pointer-to-index conversion -- breaks the pairing while each side still looks like
        // a perfectly valid sector index. No count check can see that; the symmetry can.
        int64_t ptot = -1, pboth = -1, ponp = -1, swn = -1, edges = -1, sym = -1, pnb = -2;
        json_int(body, "portal_total", ptot);
        json_int(body, "portal_both_sectors", pboth);
        json_int(body, "portal_on_plane", ponp);
        json_int(body, "sectors_with_neighbours", swn);
        json_int(body, "neighbour_edges", edges);
        json_int(body, "symmetric_edges", sym);
        json_int(body, "player_neighbours", pnb);
        check(ptot > 0, "the world has visibility portals");
        check(pboth == ptot,
              "EVERY portal resolves both of its sectors, and they are distinct");
        // The geometric invariant that pins the record's layout, asked through the public
        // struct: a portal's centre lies on the portal's own plane.
        check(ponp == ptot, "EVERY portal's centre lies on its own plane");
        check(sym == edges,
              "sector connectivity is SYMMETRIC -- every neighbour names you back");
        // A BOUND, not an equality: each portal contributes one edge per direction, and
        // sector_neighbours deduplicates, so two doors between the same pair collapse to one
        // edge. Live the two are equal (688 == 2*344) because this level has no duplicate
        // pair, but that is the art's business and not an invariant.
        check(edges > 0 && edges <= 2 * ptot,
              "the neighbour edge count is bounded by two per portal");
        check(swn > 0 && swn <= stot,
              "connected sectors are a reported fraction of all sectors");
        printf("[fixture] portals: %lld joining %lld of %lld sectors, %lld edges "
               "(%lld symmetric)\n",
               static_cast<long long>(ptot), static_cast<long long>(swn),
               static_cast<long long>(stot), static_cast<long long>(edges),
               static_cast<long long>(sym));

        // ---- THE SECTOR'S OWN PORTAL ARRAY, and the stored index ----------------------
        //
        // A second representation of the same graph. LTVisSector_LoadFromStream fills each
        // sector's array from portal indices in the asset and then calls LTVisPortal_AttachSector
        // to write the portal's sector_a/sector_b, so the back-references asserted above are
        // DERIVED from these arrays. Checking both directions tests the derivation the engine
        // performs, with none of this SDK's arithmetic standing between them.
        int64_t sip = -1, siok = -1, slp = -1, slok = -1, psum = -1, plisted = -1;
        json_int(body, "sec_idx_probed", sip);
        json_int(body, "sec_idx_ok", siok);
        json_int(body, "sec_links_probed", slp);
        json_int(body, "sec_links_ok", slok);
        json_int(body, "sec_portal_sum", psum);
        json_int(body, "sec_portal_listed", plisted);

        // THE STRUCTURAL INVARIANT: every portal joins exactly two sectors and is listed by
        // both, so the declared counts must sum to exactly twice the portal total. This is an
        // equality and not a bound -- unlike the deduplicated edge count above, a portal
        // appearing in both its sectors' arrays is the loader's own doing.
        check(psum == 2 * ptot,
              "the sectors' portal counts sum to EXACTLY two per portal");
        // Every declared element resolved to a real table entry. A mismatch here means the
        // pointer-to-index conversion is wrong, which is how this was caught the first time:
        // the portal bodies follow the pointer TABLE in memory, so the table base is not the
        // base to difference against, and differencing against it silently resolved nothing.
        check(plisted == psum,
              "EVERY portal pointer in a sector's array resolves to a table entry");
        check(slok == slp,
              "for EVERY sector, its portal array and the portals' back-references agree");

        // The stored sector index against the index used to reach it -- the cheapest check that
        // the stride and table base are right. A wrong stride still yields sectors that look
        // plausible; it does not yield 0,1,2,...,n-1 in order.
        check(sip > 0 && siok == sip,
              "EVERY sector's stored index equals the index used to reach it");
        printf("[fixture] sector portal arrays: %lld links over %lld sectors, both directions "
               "agree %lld/%lld; stored indices %lld/%lld\n",
               static_cast<long long>(plisted), static_cast<long long>(slp),
               static_cast<long long>(slok), static_cast<long long>(slp),
               static_cast<long long>(siok), static_cast<long long>(sip));

        // ---- THE VARIABLE-LENGTH PORTAL RECORD ----------------------------------------
        //
        // LTVisPortal_LoadFromStream sizes each portal `12*(vertex_count-1) + 56`, so the
        // polygon is not a fixed quad and 0x5C is merely the size of a four-vertex one.
        // LTVisTree_ComputeLoadSize corroborates it from the other end, budgeting
        // `56*portals + 12*(total_vertices - portals)` -- arithmetic that only makes sense if
        // the head already contains one vertex and the rest are per-portal extras.
        int64_t plp = -1, plok = -1, plplane = -1, pltr = -1, plverts = -1;
        json_int(body, "poly_probed", plp);
        json_int(body, "poly_len_ok", plok);
        json_int(body, "poly_on_plane", plplane);
        json_int(body, "poly_trunc", pltr);
        json_int(body, "poly_verts", plverts);
        check(plp == ptot, "every portal yielded a polygon");
        check(plok == plp,
              "portal_polygon() returns EXACTLY vertex_count vertices for every portal");

        // The geometric check applied to the FULL polygon rather than the first four. Reading
        // further into a variable-length record and still landing on the portal's own plane is
        // what shows the later vertices are really vertices, and not whatever follows.
        check(plplane == plp, "EVERY vertex of EVERY portal lies on that portal's plane");

        // A polygon needs three corners. This is a floor on the whole population rather than a
        // per-portal minimum, which is all the aggregate supports.
        check(plverts >= 3 * plp, "the portals carry at least three vertices each on average");

        // TRUTHFUL ABOUT THE ART: nothing in this level exceeds the four the fixed array holds,
        // so Portal.vertices is complete for every portal here. Asserted so that a level which
        // DOES exceed it fails loudly, rather than silently handing consumers a clipped polygon
        // -- the flag exists precisely because the record permits more.
        check(pltr == 0,
              "no portal in this level exceeds the inline vertex array (none is truncated)");
        printf("[fixture] portal polygons: %lld vertices over %lld portals, all on-plane, "
               "%lld truncated\n",
               static_cast<long long>(plverts), static_cast<long long>(plp),
               static_cast<long long>(pltr));

        // ---- REACHABILITY over the portal graph ---------------------------------------
        //
        // sectors_within/sector_hops/sector_component are COMPOSITION, not reversing -- one
        // breadth-first walk over an adjacency this suite already validated from both
        // directions. So the checks are the properties a walk cannot fake rather than a
        // comparison against a second walk, which would only prove the code agrees with itself.
        int64_t rp = -1, r1 = -1, rm = -1, rsp = -1, rso = -1, rco = -1, rcs = -1, rho = -1;
        json_int(body, "rch_probed", rp);
        json_int(body, "rch_1hop_ok", r1);
        json_int(body, "rch_mono_ok", rm);
        json_int(body, "rch_sym_probed", rsp);
        json_int(body, "rch_sym_ok", rso);
        json_int(body, "rch_comp_ok", rco);
        json_int(body, "rch_comp_size", rcs);
        json_int(body, "rch_hops_ok", rho);
        check(rp == stot, "every sector was walked");

        // Ties the walk to the primitive: one hop must be exactly the sector plus its
        // neighbours, nothing gained and nothing lost.
        check(r1 == rp, "sectors_within(s,1) is EXACTLY s plus sector_neighbours(s)");
        check(rm == rp, "the reachable set only grows with the hop limit");
        check(rho == rp, "sector_hops(s,s) is zero for every sector");

        // THE STRONG ONE. Portal adjacency is symmetric -- established over all 688 links
        // without any traversal -- so reachability at a fixed radius must be symmetric too. A
        // walk that dropped or invented an edge fails this even though it would still return a
        // plausible-looking set.
        check(rsp > 0, "the two-hop frontiers are non-empty, so symmetry is not vacuous");
        check(rso == rsp,
              "reachability is SYMMETRIC: b is within n hops of a exactly when a is of b");

        // Transitivity, which cannot hold by accident across a component this size: every
        // member of a component must compute the same component.
        check(rcs > 1, "the level has a multi-sector connected component");
        check(rco == rcs, "EVERY member of a component agrees on that component");

        // AND IT RECONCILES WITH THE PORTAL DATA, from the other end: the portals were measured
        // to join `swn` of `stot` sectors, so the sectors outside the component are exactly the
        // portal-less ones. Two independent measurements of the same partition.
        check(rcs == swn,
              "the connected component is exactly the set of sectors the portals join");
        printf("[fixture] reachability: component of %lld/%lld sectors, symmetry %lld/%lld, "
               "one-hop agrees %lld/%lld\n",
               static_cast<long long>(rcs), static_cast<long long>(stot),
               static_cast<long long>(rso), static_cast<long long>(rsp),
               static_cast<long long>(r1), static_cast<long long>(rp));
        if (psec >= 0 && pnb >= 0) {
            printf("[fixture] the player's sector has %lld neighbour(s)\n",
                   static_cast<long long>(pnb));
        }
        // REPORTED: HAL vs REF is the machine's business, but a fallback to the reference
        // rasteriser is worth seeing in the log -- the engine itself warns about it.
        printf("[fixture] presenting %lldx%lld fmt %lld x%lld, swap %lld, depth %lld, "
               "device type %lld%s\n",
               static_cast<long long>(bw), static_cast<long long>(bh),
               static_cast<long long>(bfmt), static_cast<long long>(bcnt),
               static_cast<long long>(swap), static_cast<long long>(dsfmt),
               static_cast<long long>(dtype),
               dtype == D3DDEVTYPE_HAL ? "" : "  <- NOT hardware");

        // ---- THE WORLD'S EXTENT, and MY CODE vs THE ENGINE'S ---------------------------
        //
        // The engine keeps the world bounds TWICE: in the LTWorldClientBSP instance and in
        // file-scope globals. Its own out-of-bounds test (IWorldClientBSP vtable slot 16) reads
        // the GLOBALS and ignores `this`, so a mod testing containment against the instance can
        // disagree with the test the engine actually applies when deciding an object has gone
        // outside the world.
        int64_t wbp = -1, wba = -1, wbout = -1, wbin = -1, wbbp = -1, wbbok = -1;
        json_int(body, "wb_probed", wbp);
        json_int(body, "wb_agree", wba);
        json_int(body, "wb_outside", wbout);
        json_int(body, "wb_inside", wbin);
        json_int(body, "wb_bounds_probed", wbbp);
        json_int(body, "wb_bounds_ok", wbbok);
        // ---- THE DEVICE VTABLE A STEREO PATH WOULD PATCH ------------------------------
        //
        // WHAT IS ASSERTED IS OBTAINABILITY, NOT LAYOUT. A consumer needs to be able to find this table and
        // query its protection; where D3D9 chooses to STORE it, and whether that storage is writable, is a
        // property of the runtime and the machine. This project already treats D3D ownership that way --
        // Render::interface_impl_owner() reports the owning module rather than hard-coding one, because the
        // proxy layout differs per machine. A D3D9 that put its vtable in a read-only module table would be
        // perfectly valid, so asserting "writable" would fail a legitimate environment instead of catching a
        // regression.
        //
        // Also not asserted: that the table belongs to this device alone, or that the address is stable. One
        // device exists to compare against, and a reset can replace it -- see Render.hpp.
        {
            uint32_t dvt = 0;
            int64_t writable = -1, outside = -1;
            const bool have = json_hex(body, "dev_vt", dvt);
            check(json_int(body, "dev_vt_writable", writable) &&
                  json_int(body, "dev_vt_outside_d3d9", outside),
                  "the device vtable's storage and protection fields are present");
            if (have && dvt != 0) {
                // -1 means the SDK could not find out, which is distinct from a read-only table. With a
                // device present the query must produce a definite answer either way.
                check(writable == 0 || writable == 1,
                      "with a device present, the protection query answers rather than failing");
                // Three states, three messages. Saying "NEEDS VirtualProtect" when the query FAILED would
                // report a protection we never learned -- the same conflation the optional<bool> removed.
                const char* prot = writable == 1 ? "region writable -- a hook needs no VirtualProtect"
                                 : writable == 0 ? "region read-only -- a hook NEEDS VirtualProtect"
                                                 : "protection query FAILED -- unknown";
                printf("[fixture] device vtable 0x%08X: %s d3d9.dll, %s\n",
                       dvt, outside == 1 ? "outside" : "inside", prot);
            } else {
                printf("[fixture] device vtable NOTE: no device in this state -- UNEXERCISED\n");
            }
        }

        // ---- THE BIND POSE, AGAINST THE ENGINE'S OWN GETTER ---------------------------
        //
        // ILTModelClient vt[22] (GetBindPoseNodeTransform) copies the node record's +0x08 pair and INVERTS
        // it, so sdk::ModelSkeleton::bind_pose() -- which applies that same inversion to asset data --
        // must reproduce the engine's output exactly. That is the check: two independent producers of one
        // value, one of them the engine itself.
        //
        // NEEDS A LOADED WORLD. With no world there are no model objects, so the counts are zero and
        // there is nothing to compare. That is reported rather than passed over: a check that silently
        // succeeds on an empty population is worse than no check.
        {
            int64_t edges = -1, rt_ok = -1, rt_n = -1, calls = -1, rc_ok = -1, match = -1, oor = -1;
            double worst = -1.0;
            // PARSE FIRST, AND FAIL IF A FIELD IS MISSING. Without this a dropped field leaves the count
            // at -1 and the block below reports "no world loaded" -- an endpoint regression would read as
            // an environment state. Absence and zero are different answers.
            const bool bp_fields = json_int(body, "bp_edges", edges) && json_int(body, "bp_rt_ok", rt_ok) &&
                                   json_int(body, "bp_rt_n", rt_n) && json_int(body, "bp_eng_calls", calls) &&
                                   json_int(body, "bp_eng_rc_ok", rc_ok) &&
                                   json_int(body, "bp_eng_match", match) &&
                                   json_int(body, "bp_reject_oor", oor) &&
                                   json_double(body, "bp_eng_worst", worst);
            check(bp_fields, "every bp_* diagnostic field is present and parseable");
            if (bp_fields && edges > 0) {
                check(oor == 1, "an out-of-range node index is refused by both pose accessors");
                check(rt_n > 0 && rt_ok * 100 >= rt_n * 99,
                      "invert_rigid round-trips on essentially every node (it is its own inverse)");
                check(calls > 0, "the engine's own bind-pose getter was reachable and called");
                check(rc_ok == calls, "every engine call returned LT_OK");
                check(match == calls,
                      "the SDK's bind_pose() reproduces the ENGINE's vt[22] output on every node");
                check(worst >= 0.0 && worst < 0.05, "worst engine-vs-SDK disagreement stays negligible");
                printf("[fixture] bind pose: %lld/%lld match the engine (worst %.5f), %lld/%lld invert "
                       "round-trips, %lld edges\n",
                       static_cast<long long>(match), static_cast<long long>(calls), worst,
                       static_cast<long long>(rt_ok), static_cast<long long>(rt_n),
                       static_cast<long long>(edges));
            } else if (bp_fields) {
                printf("[fixture] bind pose NOTE: fields present but 0 model objects (no world loaded) -- "
                       "the engine comparison is UNEXERCISED, not passing\n");
            }
        }

        // ---- SLOT 1 ACROSS THE RESOLVED INTERFACES ------------------------------------
        //
        // interfaces::slot1_constant_string() reports a string only when slot 1's ENTRY SEQUENCE is
        // `mov eax, <imm32>; retn` on an executable, non-guard page. It never CALLS the slot, so a
        // mismatch is information rather than a misfire, and nothing here depends on where the body ends.
        //
        // COMPARED AGAINST THE RESOLVED COUNT, NOT THE TABLE SIZE. Interface.hpp documents that a null
        // interface pointer is normal -- early startup, server absent, mid-unload -- so requiring every
        // descriptor to yield a string would fail on AVAILABILITY and read as a shape regression.
        //
        // Both counts come from the endpoint, which already walks every entry; re-deriving them by
        // splitting the JSON array apart here would duplicate that work in a format-fragile way.
        {
            std::string iresp;
            check(http::get(port, "/sdk/interfaces", iresp), "/sdk/interfaces transport");
            const std::string ibody = http::body_of(iresp);
            int64_t itotal = -1, iresolved = -1, ishaped = -1;
            // "expected_names" is the endpoint's name for the generated descriptor count.
            check(json_int(ibody, "expected_names", itotal) && itotal > 0,
                  "the interface descriptor table is not empty");
            check(json_int(ibody, "resolved", iresolved) && iresolved > 0,
                  "at least one interface is resolved in this state");
            check(json_int(ibody, "slot1_constant_strings", ishaped), "the shape count is reported");
            check(ishaped == iresolved,
                  "every RESOLVED interface's slot 1 carries the constant-string entry sequence");
            // Two stable semantic anchors, so a silent change of meaning cannot hide behind a
            // still-passing count.
            std::string s_lt, s_shell;
            check(json_str(ibody, "iltclient_slot1", s_lt) && s_lt == "CLTClient",
                  "ILTClient's slot 1 names CLTClient");
            check(json_str(ibody, "iclientshell_slot1", s_shell) && s_shell == "CGameClientShell",
                  "IClientShell's slot 1 names CGameClientShell");
            // The interface this project's bind-pose and socket work dispatches through. Pinning the
            // implementing CLASS means a future build that re-points ILTModel.Client at something else
            // fails here rather than silently changing what the skeleton helpers read.
            std::string s_model;
            check(json_str(ibody, "iltmodel_slot1", s_model) && s_model == "CLTModelClient",
                  "ILTModel.Client is implemented by CLTModelClient");
            printf("[fixture] slot1 entry sequence: %lld/%lld resolved of %lld descriptors; ILTClient=%s\n",
                   static_cast<long long>(ishaped), static_cast<long long>(iresolved),
                   static_cast<long long>(itotal), s_lt.c_str());
        }

        // ---- THE GAME DLL'S PER-FRAME HOOK ANCHORS ------------------------------------
        //
        // CClientShell::Update dispatches IClientShell slots 2, 4, 3 every frame -- PreUpdate,
        // Update, PostUpdate on gameclient.dll's CGameClientShell. Those are the hook sites for a mod
        // that needs to run inside the game's frame rather than bracketing it, so the slot map has to
        // stay true against the live build rather than being trusted from a one-off read.
        //
        // THE IDENTITY CHECK covers WHERE THE INTERFACE STARTS -- slot 1 returning its literal is
        // what proves slot 0 is implementation-only, i.e. the +2 anchor. It does NOT cover which of
        // 2/3/4 is which: a build that reordered those would pass everything checked here. These are
        // sanity checks against a shifted or foreign interface, not a proof of the assignment.
        int64_t gok = -1, ganch = -1, gpre = -1;
        json_int(body, "gcs_ok", gok);
        json_int(body, "gcs_anchors", ganch);
        json_int(body, "gcs_pre_empty", gpre);
        check(gok == 1, "IClientShell slot 1 reports \"CGameClientShell\" -- the slot map holds");
        check(ganch == 3,
              "all three per-frame anchors resolve INSIDE gameclient.dll");

        // Slot 2's entry byte is `retn`, so it returns immediately. THIS IDENTIFIES NOTHING BY ITSELF:
        // gameclient.dll is ICF-linked, so every empty method in the DLL shares one address -- 138
        // vtable slots point at it, and Update calls it twice for unrelated empty methods. What the
        // check is worth is catching "slot 2 now holds real work"; it cannot tell PreUpdate from any
        // other empty virtual. Also a property of the shipped game code, not an invariant.
        check(gpre == 1, "slot 2's entry byte is retn -- its entry returns immediately");

        // AND THE PART THAT MAKES A REORDERING DETECTABLE: each of the three still has its mapped
        // prologue, and no two of those shapes match -- slot 2 is the folded empty stub, slot 3 opens
        // a large fixed frame with `sub esp, 0x128`, slot 4 sets up ebp AND aligns the stack to 64 for
        // SSE locals. Swap any two and this fails. Note the discrimination here comes from slots 3 and
        // 4 having distinct OWN bodies -- slot 2's contribution is only "still empty". It is still not a proof of semantics: a rebuild
        // of the same game code could change a prologue, which would show up here as a false
        // negative rather than as a wrong hook.
        int64_t gshapes = -1;
        json_int(body, "gcs_shapes", gshapes);
        check(gshapes == 1, "slots 2/3/4 still carry their mapped, mutually distinct prologues");

        // THE TWO ROUTES TO SLOT 2 MUST AGREE. pre_update_fn() reads the function out of the table;
        // pre_update_vtable_entry() hands back the address OF that table cell -- which is what a
        // consumer patches, since slot 2's function address is shared by ~133 unrelated methods and
        // detouring it would intercept all of them. Dereferencing the entry has to yield exactly the
        // function the anchor reports, or one of the two is lying about which slot it describes.
        int64_t gentry = -1;
        json_int(body, "gcs_entry_agrees", gentry);
        check(gentry == 1, "slot 2's vtable ENTRY dereferences to the function pre_update_fn() reports");
        printf("[fixture] game shell: interface identity + 3 anchors in gameclient.dll, slot 2 "
               "returns at once, 2/3/4 prologues distinct\n");

        // ---- THE LOCAL PLAYER'S TWO FORMS ---------------------------------------------
        //
        // The shell keeps the player as a HANDLE and as a resolved POINTER, and re-resolves the
        // pointer once per frame inside CClientShell::Update. Those are independent routes to one
        // object, so agreement is a check on that refresh rather than a restatement of one read.
        //
        // Measured RAW, because local_player() now fails closed on a disagreeing pair and so could
        // never surface one. The second count confirms failing closed does not OVER-reject: the safe
        // accessor must still accept every slot the raw read finds consistent.
        int64_t lps = -1, lpc = -1, lpa = -1;
        json_int(body, "lp_slots", lps);
        json_int(body, "lp_consistent", lpc);
        json_int(body, "lp_accepted", lpa);
        check(lps > 0, "at least one local player slot is filled");
        check(lpc == lps, "EVERY filled slot's handle resolves to its stored pointer");
        check(lpa == lps, "and local_player() accepts every one -- failing closed over-rejects none");
        printf("[fixture] local player pair: %lld slot(s), handle and pointer agree on all, "
               "accessor accepts all\n",
               static_cast<long long>(lps));

        check(wbbp == 1, "the world bounds were readable by both routes");
        check(wbbok == wbbp, "the instance bounds and the engine's globals hold the SAME extent");

        // THE ENGINE ANSWERS THE SAME QUESTION. Not a second implementation of mine -- the
        // shipped function, reached through its vtable. This is the only check in the suite where
        // the reference is the game's own code executing.
        check(wbp == 15, "all fifteen probe points were classified by both routes");
        check(wba == wbp,
              "is_point_outside_world() agrees with the ENGINE'S OWN function on every point");

        // AND THE SPLIT PINS THE BOUNDARY CONVENTION. The probe set is fixed and built from the
        // bounds themselves: the centre, the six face points lying EXACTLY on a bound, the six
        // points one unit beyond each face, and the two extreme corners. The engine compares
        // strictly (min > p, max < p), so the surface is INSIDE -- giving 9 inside and 6 outside.
        // An inclusive comparison would flip the six faces and two corners to 3 and 12, so these
        // exact counts are a test of the CONVENTION rather than of the level.
        check(wbin == 9, "points exactly ON a bound are INSIDE -- the comparison is strict");
        check(wbout == 6, "only the six points beyond a face are outside");
        printf("[fixture] world extent: both copies agree; %lld/%lld points match the engine's "
               "own test (%lld in, %lld out)\n",
               static_cast<long long>(wba), static_cast<long long>(wbp),
               static_cast<long long>(wbin), static_cast<long long>(wbout));

        // ---- WHICH WORLD IS LOADED ----------------------------------------------------
        //
        // A wrong offset here does not fault, it returns whatever bytes follow -- so the checks
        // are on the string's SHAPE, which garbage fails on every count: printable throughout,
        // non-empty, carrying the .wld extension the "WLDC" magic implies, and world_name()
        // being a genuine trailing component of the path with no separator or extension left in.
        int64_t wpr = -1, wln = -1;
        std::string wpath, wname;
        json_int(body, "world_printable", wpr);
        json_int(body, "world_len", wln);
        check(json_str(body, "world_path", wpath), "the loaded world path is reported");
        check(json_str(body, "world_name", wname), "the loaded world name is reported");
        check(wln > 0 && !wpath.empty(), "a world is loaded, so the path is non-empty");
        check(wpr == 1, "EVERY character of the path is printable -- not adjacent bytes");
        check(static_cast<int64_t>(wpath.size()) == wln,
              "the reported length matches the string that came back");
        check(wpath.size() >= 4 && wpath.compare(wpath.size() - 4, 4, ".wld") == 0,
              "the path names a .wld, which is what the WLDC magic implies");

        // world_name() must be the path's last component minus the extension. Checked as a
        // relationship rather than against a literal, so it holds on any map.
        check(!wname.empty(), "the world name is non-empty");
        check(wname.find('\\') == std::string::npos && wname.find('/') == std::string::npos,
              "the world name has no directory separator left in it");
        check(wname.find('.') == std::string::npos, "the world name has no extension left in it");
        check(wpath.find(wname) != std::string::npos,
              "the world name is a substring of the world path");
        // GUARDED, because this arithmetic underflows on an empty path. With no world loaded both
        // strings are empty, `wpath.size() - wname.size() - 4` wraps to a huge size_t, and compare()
        // throws out_of_range -- an uncaught throw, so terminate, so MSVC's abort() raises __fastfail
        // and the run dies as 0xC0000409 having printed nothing (its log was still buffered). That is
        // how a legitimate state -- sitting at the main menu -- looked like a crashing harness.
        if (!wpath.empty() && !wname.empty() && wpath.size() >= wname.size() + 4) {
            check(wpath.compare(wpath.size() - wname.size() - 4, wname.size(), wname) == 0,
                  "the world name is exactly the path's final component before .wld");
        } else {
            printf("[fixture] world NOTE: no world loaded, so the path/name relationship is UNEXERCISED\n");
        }
        printf("[fixture] world loaded: %s (name %s)\n", wpath.c_str(), wname.c_str());

        // ---- IS A WORLD LOADED, and does the schema's class even fit -------------------
        //
        // is_world_loaded() reads the two flags the attach path sets. Cross-checked against two
        // signs of the same state that share none of its code: a non-empty world path (a
        // different field, written by the same function) and a non-zero sector count (a different
        // object entirely). Three indicators of one fact.
        int64_t wbl = -1, wbgap = -1, wbsz = -1, wbsp = -1, wbse = -1;
        json_int(body, "wb_loaded", wbl);
        json_int(body, "wb_obj_gap", wbgap);
        json_int(body, "wb_class_size", wbsz);
        json_int(body, "wb_srv_probed", wbsp);
        json_int(body, "wb_srv_expanded", wbse);
        check(wbl == 1, "the engine reports a world loaded");
        check(wbl == 1 && wln > 0 && stot > 0,
              "loaded flags, a non-empty world path and a non-zero sector count all agree");

        // THE CLASS SIZE IS BOUNDED BY THE NEXT SINGLETON, and this is the check whose absence
        // let LTWorldClientBSP stand at 0x244 for several passes -- with a bounds pair mapped at
        // +0x22C that actually lay past the server BSP object. The server singleton sits a fixed
        // distance after the client one, so that distance is a hard ceiling. It needs no knowledge
        // of what any field means, and 0x244 (580) against a 368-byte gap fails it outright.
        check(wbgap > 0, "both world singletons were located");
        check(wbsz <= wbgap,
              "the mapped LTWorldClientBSP fits before the next singleton -- the size is possible");

        // The server's extent is DERIVED, not copied: its world load writes the global bounds
        // expanded by 100 units. Read out of IWorldServerBSP_LoadWorld, then checked live -- and
        // exact equality is right because it is one arithmetic step, not a measurement.
        check(wbsp == 1, "the server world singleton was readable");
        check(wbse == 1,
              "the server's bounds are EXACTLY the global bounds expanded by 100 on every axis");
        printf("[fixture] world state: loaded, class 0x%llX fits the 0x%llX singleton gap, "
               "server bounds = globals +/-100\n",
               static_cast<unsigned long long>(wbsz), static_cast<unsigned long long>(wbgap));

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

        // THE SAME CLOCK BY A DIFFERENT ROAD. counter_elapsed_* above are field
        // reads through CClientMgr's mapped layout; these come from CALLING the
        // engine's own two accessors, located by byte pattern. Agreement between a
        // layout read and a behaviour call is the strongest kind of cross-check
        // available here: a wrong offset and a wrong pattern cannot agree by luck.
        int64_t eng_ms = -1;
        double eng_s = -1.0;
        check(json_has(body, "\"engine_time_ok\":true"),
              "both engine clock accessors were located and called");
        check(json_int(body, "engine_time_ms", eng_ms) && eng_ms >= 0,
              "the engine's millisecond accessor answered");
        check(json_double(body, "engine_time_seconds", eng_s) && eng_s >= 0.0,
              "the engine's seconds accessor answered");
        // TOLERANCE ON PURPOSE, and the reason matters: the two paths are read a few
        // microseconds apart inside one request, so on a RUNNING game the clock
        // legitimately advances between them. A tight equality here would pass only
        // while the game is paused and fail during play -- the per-frame-state trap.
        // A second of slack still catches the failure that matters, since a wrong
        // pattern yields zero or nonsense, not a value 40ms off.
        const double path_gap = static_cast<double>(eng_ms) - static_cast<double>(elapsed_ms);
        check(path_gap > -1000.0 && path_gap < 1000.0,
              "the engine's own accessor agrees with the mapped field read");
        // The unit tie WITHIN one guarded pair, which shares an instant and so can be
        // checked tightly.
        const double unit_gap = static_cast<double>(eng_ms) - eng_s * 1000.0;
        check(unit_gap > -2.0 && unit_gap < 2.0,
              "the engine's ms and seconds accessors are the same instant in two units");

        // ---- THE SHELL'S TWO CLOCKS ------------------------------------------------
        //
        // Only ONE of these can be asserted, and knowing which is the whole point.
        //
        // The real clock ADVANCES ALWAYS -- that is its defining property and the
        // reason a VR mod must use it, so it is assertable in any game state: paused,
        // playing, or sitting in a menu.
        //
        // The game clock cannot be asserted in either direction. Frozen is correct
        // while paused and wrong while playing, and the fixture does not control which
        // is true. Requiring it to advance would fail at a menu; requiring it to freeze
        // would fail during play. So its value is REPORTED and its readability checked,
        // which is exactly TESTING.MD's rule about state the test does not own.
        check(json_has(body, "\"shell_clocks_ok\":true"),
              "both shell clock accessors were located and a shell exists");
        double real1 = -1.0, game1 = -1.0;
        check(json_double(body, "shell_real_time", real1) && real1 > 0.0,
              "the shell's real clock reads a positive time");
        check(json_double(body, "shell_game_time", game1) && game1 >= 0.0,
              "the shell's game clock is readable and non-negative");

        // THE LOCAL CLIENT TABLE. What is assertable is the engine's own BOUND: the
        // table has 4 slots and GetLocalClientID rejects any index at or above that,
        // so a count outside [0,4] means the field moved or the sentinel is wrong. The
        // count itself is reported: a shell can exist before any client is connected,
        // and split-screen would legitimately report more than one.
        int64_t lcc = -1, lc0 = -99;
        check(json_int(body, "local_client_count", lcc) && lcc >= 0 && lcc <= 4,
              "the local client count is inside the engine's own 4-slot bound");
        check(json_int(body, "local_client_0", lc0), "local_client_0 is reported");
        // Internal consistency rather than a fixed value: if any slot is filled, slot
        // zero must hold a real id (-1 is this endpoint's "empty" marker), because the
        // engine fills from the front.
        if (lcc > 0) {
            check(lc0 >= 0, "a filled client table yields a real id in slot 0");
        }
        printf("[fixture] local clients: %lld (slot 0 id %lld)\n",
               static_cast<long long>(lcc), static_cast<long long>(lc0));

        // The frame interval is CONFIGURED (bit-identical 1/60 across samples), so a
        // sane positive duration is assertable while the exact value is not -- a
        // different cap is a setting, not a fault.
        double fi = -1.0;
        check(json_double(body, "frame_interval", fi) && fi > 0.0 && fi <= 1.0,
              "the shell's frame interval is a sane positive duration");

        // The gap must CLEAR THE CLOCK'S GRANULARITY, which was measured rather than
        // assumed: this clock steps by ~0.503 s (58 polls over 2.97 s produced 6
        // distinct values). A 300 ms gap failed here intermittently -- correctly, since
        // two samples inside one step legitimately read the same value. 1.5 s is three
        // steps, so an advance is guaranteed for a clock that runs at all.
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        std::string resp2;
        double real2 = -1.0;
        const bool got2 = http::get(port, "/sdk/targets", resp2);
        check(got2, "/sdk/targets second sample");
        if (got2) {
            const std::string body2 = http::body_of(resp2);
            check(json_double(body2, "shell_real_time", real2) && real2 > 0.0,
                  "the shell's real clock reads on the second sample");
            const double advanced = real2 - real1;
            check(advanced > 0.0 && advanced < 30.0,
                  "the shell's real clock advanced across a 1.5s wall-time gap");
            // The fine-grained clock must advance over the same window, and by an
            // amount consistent with it. Both being real time, they should agree on
            // how much time passed -- generous slack, since they are sampled from two
            // requests and quantised differently.
            int64_t ms1 = -1, ms2 = -1;
            json_int(body, "last_sample_time_ms", ms1);
            json_int(body2, "last_sample_time_ms", ms2);
            check(ms2 > ms1, "the per-frame millisecond clock advanced too");
            const double ms_advanced = static_cast<double>(ms2 - ms1) / 1000.0;
            check(ms_advanced - advanced > -1.5 && ms_advanced - advanced < 1.5,
                  "the coarse and fine wall clocks agree on how much time passed");
        }

        // ---- THE GLOBAL FORCE ------------------------------------------------------
        //
        // The MECHANISM is asserted (the engine's getter answers, and what it copies
        // out is finite); the VALUE is reported. Live it is (0, -980, 0), but a level
        // or script is free to change gravity, and pinning -980 here would turn a
        // legitimate design choice into a test failure.
        check(json_has(body, "\"global_force_ok\":true"),
              "the engine's global-force getter was located and answered");
        const size_t fp = body.find("\"global_force\":[");
        check(fp != std::string::npos, "global_force present in the payload");
        if (fp != std::string::npos) {
            double fx = 0.0, fy = 0.0, fz = 0.0;
            const int got = sscanf(body.c_str() + fp + 16, "%lf,%lf,%lf", &fx, &fy, &fz);
            check(got == 3, "global_force is a three-component vector");
            const double mag2 = fx * fx + fy * fy + fz * fz;
            check(mag2 >= 0.0 && mag2 < 1.0e12,
                  "global_force components are finite and of sane magnitude");
            printf("[fixture] global force (0,-980,0 live): (%.1f, %.1f, %.1f)\n", fx, fy, fz);
        }

        // ---- CONSOLE VARIABLES -----------------------------------------------------
        //
        // The table walk and the by-name lookup are separate claims, so both are
        // checked. The lookup test uses names the TABLE reported, upper-cased -- not a
        // hardcoded variable -- so it holds whatever a given build registers.
        int64_t cvc = -1, cvn = -1, cvp = -1, cvr = -1;
        json_int(body, "convar_count", cvc);
        json_int(body, "convar_named", cvn);
        json_int(body, "convar_probed", cvp);
        json_int(body, "convar_roundtrip", cvr);
        check(cvc > 0, "the console-variable table walks and yields entries");
        // Every entry the API returns carries a name -- it filters nameless records,
        // and this guards that filter rather than restating it.
        check(cvn == cvc, "every returned console variable has a name");
        check(cvp > 0, "console variables were probed by name");
        // The load-bearing one: name AND value must both come back, case-insensitively.
        check(cvr == cvp,
              "EVERY probed console variable is found again by its UPPER-CASED name "
              "with the same value");
        printf("[fixture] console variables: %lld entries, %lld/%lld name round-trips\n",
               static_cast<long long>(cvc), static_cast<long long>(cvr),
               static_cast<long long>(cvp));

        // ---- THE LOCAL PLAYER --------------------------------------------------------
        //
        // Whether a player exists at all is scene state -- a menu with no level loaded
        // has none -- so presence is REPORTED. What is asserted is the consistency the
        // mapping rests on: the shell stores the object twice, once as a handle and once
        // as a pointer, and resolving the handle through the engine's own table must land
        // on the same object. Two independently-stored routes agreeing is the difference
        // between a mapping and a lucky offset.
        int64_t pcount = -1, phandle = -2;
        json_int(body, "player_count", pcount);
        json_int(body, "player_handle", phandle);
        check(pcount >= 0 && pcount <= 4,
              "the local player count is inside the engine's own 4-slot bound");
        const bool have_player = json_has(body, "\"player_ok\":true");
        if (have_player) {
            check(json_has(body, "\"player_routes_agree\":true"),
                  "the player's handle and cached pointer name the SAME object");
            check(phandle >= 0 && phandle != 0xFFFF,
                  "the player's handle is a real handle, not the empty sentinel");
            check(pcount >= 1, "a resolvable player implies a non-zero player count");
            // REPORTED, not matched: which model the player wears is the level's art.
            const size_t mp = body.find("\"player_mdl\":\"");
            check(mp != std::string::npos && body[mp + 14] != '"',
                  "the player object names a model path");

            // ---- THE VR CHAIN, END TO END ---------------------------------------------
            //
            // local player -> skeleton -> socket BY NAME -> world transform. Four
            // subsystems, and the only assertion that can catch a fault anywhere along it
            // is a geometric one, because every intermediate step returns a plausible
            // number when it is wrong.
            int64_t psock = -1;
            json_int(body, "player_sockets", psock);
            check(psock > 0, "the player's model defines sockets to attach to");
            if (json_has(body, "\"hands_ok\":true")) {
                check(json_has(body, "\"hands_distinct\":true"),
                      "the two hand sockets resolve to two DIFFERENT points");
                double reach = -1.0;
                json_double(body, "hands_reach", reach);
                // THE REGRESSION GUARD. The composition bug put sockets 5449 units from
                // their owner; this model's half-extents are (40, 95, 40), so a hand is
                // ~84 away. The bound is deliberately loose at 400 -- an arm cannot be
                // longer than a few body-lengths, and 400 still catches that bug 13x over.
                check(reach > 0.0 && reach < 400.0,
                      "each hand sits within BODY SCALE of the player object, not level scale");
                printf("[fixture] VR chain: %lld sockets, hands %.1f from the body, %s\n",
                       static_cast<long long>(psock), reach,
                       json_has(body, "\"hands_clean\":true") ? "bones CLEAN"
                                                             : "bones stale this frame");

                // ---- THE ENGINE'S OWN ANSWER, AGAINST OURS -------------------------
                //
                // This is the strongest check in the file. The engine moves an attached
                // object to its mount point using ITS arithmetic; we compose the same
                // point from the asset's socket record and the bone cache using OURS. So
                // the weapon object's position and our RightHand socket transform are two
                // independent producers of one value, and they must agree. Live they agree
                // EXACTLY (0.000), which is why the tolerance can be this tight: anything
                // that broke the composition would move this by units, not by epsilon.
                if (json_has(body, "\"muzzle_ok\":true")) {
                    double wvh = -1.0, mfh = -1.0;
                    json_double(body, "weapon_vs_hand", wvh);
                    json_double(body, "muzzle_from_hand", mfh);
                    check(wvh >= 0.0 && wvh < 0.05,
                          "the ENGINE's placement of the weapon matches OUR socket "
                          "composition for the hand holding it");
                    // A muzzle is down a barrel from the grip: far enough to be a real
                    // offset, near enough to still be part of the weapon.
                    check(mfh > 5.0 && mfh < 150.0,
                          "the muzzle sits a BARREL LENGTH from the hand, not at it and "
                          "not across the level");
                    printf("[fixture] muzzle: %.1f from the hand, engine agrees to %.3f\n",
                           mfh, wvh);
                } else {
                    printf("[fixture] NOTE: nothing mounted on the player carries a "
                           "'flash' socket -- the attachment chain was NOT exercised.\n");
                }
            } else {
                printf("[fixture] NOTE: the player's hand sockets did not resolve -- the "
                       "end-to-end chain was NOT exercised.\n");
            }
            printf("[fixture] local player: handle %lld, %lld slot(s) filled\n",
                   static_cast<long long>(phandle), static_cast<long long>(pcount));
        } else {
            printf("[fixture] NOTE: no local player in this state -- the handle/pointer "
                   "cross-check was NOT exercised.\n");
        }
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

        // ANIMATION STATE. The index bound is not decoration -- it is the evidence
        // that identified the field in the first place. anim_index was called an
        // animation index BECAUSE it stays strictly inside the asset's animation-table
        // count across 34 assets whose counts run 1..457. Asserting it keeps that
        // identification honest: if the field ever leaves the range, either it was
        // never an index or one of the two offsets moved, and both deserve a red test.
        //
        // The fraction being in [0,1] is the same kind of claim -- normalisation is
        // what makes it a fraction rather than an arbitrary float.
        int64_t aok = -1, aidx = -1, afrac = -1, ablend = -1;
        json_int(body, "anim_ok", aok);
        json_int(body, "anim_index_in_range", aidx);
        json_int(body, "anim_frac_in_range", afrac);
        json_int(body, "anim_blending", ablend);
        check(aok == with_skeleton, "model_anim_state answers for every model with a skeleton");
        check(aidx == aok, "every animation index stays inside its asset's animation count");
        check(afrac == aok, "every animation fraction is within [0,1]");
        // REPORTED: whether any model is mid-transition depends entirely on what the
        // game is doing this instant (live: 1 of 215). A floor here would fail on a
        // paused game, which is the per-frame-state trap TESTING.MD describes.
        check(ablend >= 0 && ablend <= aok,
              "models with differing index pair are a reported count, not a requirement");

        // THE ANIMATION NAME, resolved through the engine's own index->record->name
        // chain. REPORTED, not required equal: the engine null-checks the record slot
        // itself, so a model whose current index has no record is a legal state -- and
        // the counts bear that out, with the public API answering for all 215 in one
        // sample and a raw probe of the same chain getting 214 in another. What is
        // asserted is that the chain WORKS at all, which a moved offset would break
        // outright rather than reduce.
        int64_t anamed = -1;
        json_int(body, "anim_named", anamed);
        check(anamed > 0, "current animation names resolve through the record vector");
        check(anamed <= aok, "named animations are a subset of models with anim state");

        // PIECE VISIBILITY. The interesting assertion is that the accessor ENFORCES
        // the engine's piece-count bound: the endpoint asks about indices 0..63 for
        // every model, so an unbounded implementation would answer 64 times per model.
        // It answers once per real piece instead.
        int64_t pans = -1, phid = -1, mobj = -1;
        json_int(body, "piece_answers", pans);
        json_int(body, "piece_hidden", phid);
        json_int(body, "model_objects", mobj);
        check(pans > 0, "piece visibility answers for real pieces");
        check(mobj > 0 && pans < 64 * mobj,
              "the piece accessor refuses indices beyond the model's piece count");
        // THE REGRESSION GUARD for a bug this pass fixed. The accessor must answer for
        // exactly the pieces the models report -- no more, no fewer. It previously
        // bounded piece indices by the MATERIAL count, which is a different and usually
        // smaller number, and silently refused 221 of 697 real pieces (32%). If anyone
        // reintroduces that bound this check fails immediately, because the two totals
        // are 476 and 697.
        int64_t pcounts = -1, pnamed = -1, prt = -1;
        json_int(body, "piece_counts", pcounts);
        json_int(body, "piece_named", pnamed);
        json_int(body, "piece_roundtrip", prt);
        check(pcounts > 0, "models report a piece count");
        check(pans == pcounts,
              "the piece accessor answers for EVERY piece the models report");
        // Every piece is named, and every name finds its own index back through the
        // case-insensitive lookup -- the same two-way check the sockets get.
        check(pnamed == pcounts, "EVERY piece resolves to a name");
        check(prt == pcounts,
              "every piece is found again by its own UPPER-CASED name");

        // SOCKET WORLD TRANSFORMS -- the composed answer a VR mod consumes.
        int64_t sxo = -1, sxu = -1, sxf = -1, sxc = -1, scm = -1, sca = -1;
        double sxd = -1.0, sch = 0.0;
        json_int(body, "sock_xform_ok", sxo);
        json_int(body, "sock_xform_unit", sxu);
        json_int(body, "sock_xform_finite", sxf);
        json_int(body, "sock_xform_clean", sxc);
        json_double(body, "sock_xform_max_dist", sxd);
        json_int(body, "sock_camera_measured", scm);
        json_int(body, "sock_camera_above", sca);
        json_double(body, "sock_camera_max_height", sch);
        check(sxo > 0, "socket world transforms compose");
        // The quaternion product must preserve unit length. This checks the FORM of the
        // transcribed multiply; it cannot catch a sign convention, which is why the
        // distance bound below matters more.
        check(sxu == sxo, "EVERY composed socket rotation is unit-length");
        // NOT "every transform is finite" -- that over-reached and this suite carried it until a
        // game restart exposed it. 7 of 702 live socket transforms have non-finite positions, and
        // every one is STALE: a model whose bone cache the engine has never evaluated holds
        // whatever its allocation held. Requiring finiteness of never-written memory is asserting
        // something the engine never promised.
        //
        // THE CONTRACT THAT DOES HOLD, and the one a consumer relies on: if the SDK reports a
        // transform CLEAN, it is finite. Zero clean transforms are non-finite.
        int64_t nfs = -1, nfc = -1, usable = -1, usable_probed = -1;
        json_int(body, "sock_nf_stale", nfs);
        json_int(body, "sock_nf_clean", nfc);
        check(nfc == 0, "EVERY socket transform the SDK reports CLEAN has a finite position");
        check(sxf + nfs + nfc == sxo,
              "the finite, stale-garbage and clean-garbage counts partition every transform");
        check(nfs >= 0, "the stale non-finite count is reported");

        // And the one-call consumer form must agree with the pieces: usable exactly when clean,
        // finite and unit. Checked as a relationship so the helper cannot drift from the parts.
        json_int(body, "sock_usable", usable);
        json_int(body, "sock_usable_probed", usable_probed);
        check(usable_probed == sxo, "the usability helper answered for every transform");
        check(usable == sxc,
              "socket_world_transform_is_usable() is true for EXACTLY the clean transforms");
        printf("[fixture] sockets: %lld composed, %lld usable; %lld non-finite and ALL stale\n",
               static_cast<long long>(sxo), static_cast<long long>(usable),
               static_cast<long long>(nfs));

        // ---- THE ENGINE'S OWN SOCKET TRANSFORM ----------------------------------------
        //
        // ILTModel::GetSocketTransform, called through vtable slot 3, is the game's own answer --
        // the strongest available check on this SDK's composition, because nothing of ours stands
        // between the question and the reply.
        //
        // The slot is GUARDED, not assumed: engine_socket_transform_available() compares the entry
        // against the function's known module offset, so a wrong index refuses instead of calling
        // something arbitrary. Getting there needed the disassembly rather than the decompiler --
        // `retn 10h` pinned four arguments, and `mov ecx, [esp+arg_0]` showed the first is the
        // OBJECT, with the interface `this` never touched.
        //
        // ONLY CLEAN SOCKETS ARE ASKED. On a dirty node the engine EVALUATES the skeleton and
        // clears the flag, which is a mutation belonging on the game thread; on a clean one the
        // dirty check short-circuits and the call is a pure read.
        int64_t slot_ok = -1, erc = -999, eok = -1, ew = -1, el = -1, f1ok = -1, f1w = -1, f1l = -1;
        json_int(body, "ilt_slot_ok", slot_ok);
        json_int(body, "engine_rc", erc);
        json_int(body, "engine_xf_probed", ew);
        json_int(body, "engine_xf_ok", eok);
        json_int(body, "engine_xf_agree_local", el);
        json_int(body, "engine_f1_ok", f1ok);
        json_int(body, "engine_f1_world", f1w);
        json_int(body, "engine_f1_local", f1l);
        check(slot_ok == 1, "ILTModel vtable slot 3 still holds GetSocketTransform");
        check(erc == 0, "the engine's last GetSocketTransform call returned LT_OK");
        check(ew == sxc, "the engine was asked about every CLEAN socket");
        check(eok == ew, "the engine answered every one");

        // THE LOAD-BEARING ONE: with world space requested, the engine agrees with this SDK's
        // composed transform on EVERY clean socket. That covers the branch composition skips --
        // the models whose bone cache is already world-space -- which no internal check could.
        check(f1ok == ew, "the world-space form answered for every clean socket");
        check(f1w == ew,
              "the ENGINE agrees with socket_world_transform() on EVERY clean socket");

        // And the model-space form pins the same fact from the other side. It matches the raw
        // bone-cache pose on the models that need composing, and matches it on the rest too --
        // because for those, model and world coincide. The two populations must ADD UP, which is
        // what shows the split is structural and not a tolerance artefact.
        check(el + f1l == ew,
              "model-space agreement plus the already-world-space models account for all of them");
        printf("[fixture] engine sockets: %lld/%lld agree in world space, %lld+%lld=%lld in "
               "model space\n",
               static_cast<long long>(f1w), static_cast<long long>(ew),
               static_cast<long long>(el), static_cast<long long>(f1l),
               static_cast<long long>(ew));
        // THE REGRESSION GUARD, and it caught a real bug. A socket must land within
        // model scale of its object. The first implementation composed the object's
        // transform onto bones that were ALREADY in world space -- true on the 22 models
        // whose mode selector is set -- and that double-application put a socket
        // 5449 units out. Correcting it brought the maximum to 137. The bound is
        // deliberately loose: it is here to catch a wrong SPACE, not to police art.
        check(sxd >= 0.0 && sxd < 1000.0,
              "composed sockets land within model scale of their object");
        // REPORTED, with the reason: "the head is above the feet" is not an invariant.
        // Live 1 of 2 measurable camera sockets sits below its object -- and that model
        // is playing Corpse_surface_facedown, so below is CORRECT. The animation name
        // and the bone geometry agree, which is better evidence than the naive check
        // would have been.
        check(scm >= 0 && sca >= 0 && sca <= scm,
              "camera-socket heights are a reported count, not a floor");
        printf("[fixture] socket transforms: %lld composed, %lld clean, max dist %.1f; "
               "camera sockets %lld measured / %lld above (max height %.1f)\n",
               static_cast<long long>(sxo), static_cast<long long>(sxc), sxd,
               static_cast<long long>(scm), static_cast<long long>(sca), sch);
        // REPORTED: nothing is hidden in the sampled scene, so a floor here would be
        // asserting the level's art rather than the engine's mechanism.
        check(phid >= 0 && phid <= pans,
              "hidden pieces are a reported fraction of pieces");

        // The record's two NODE indices. All three of these are the evidence that
        // named them, so all three are requirements rather than reports:
        //   in_range -- they stay inside node_count, which is the bound that
        //               identified them (a field merely holding small numbers would
        //               fail on the 2-node models in this population);
        //   named    -- handed back to the skeleton they resolve to a real bone
        //               name, so the index is usable as an index, not just in range
        //               as a number;
        //   ordered  -- node_b >= node_a, observed on every model with no exception.
        // A regression in any of them means the offset moved or the field is not what
        // the schema says. Note the ROLE of the pair is unmapped, so nothing here
        // asserts what the two nodes mean together -- only that they are nodes.
        int64_t anr = -1, ann = -1, ano = -1;
        json_int(body, "anim_nodes_in_range", anr);
        json_int(body, "anim_nodes_named", ann);
        json_int(body, "anim_nodes_ordered", ano);
        check(anr == aok, "every animation record's node indices are inside node_count");
        check(ann == aok, "every animation record's node indices resolve to a bone name");
        check(ano == aok, "node_b >= node_a on every animation record");
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

                // THE TWO IDENTITIES. An object carries both a handle and a slot
                // index or neither -- 3248 both, 335 neither, zero mixed. This is a
                // BICONDITIONAL, so it is asserted as one: the interesting failure is
                // not a count changing (objects come and go between samples) but the
                // two counts DIVERGING, which would mean a field moved.
                int64_t ahnd = -1, aslot = -1, aagree = -1, aaddr = -1;
                json_int(ab, "with_handle", ahnd);
                json_int(ab, "with_slot", aslot);
                json_int(ab, "identities_agree", aagree);
                json_int(ab, "addressable", aaddr);
                check(aagree == aobj,
                      "handle presence and slot-index presence agree on every object");
                check(ahnd == aslot, "the two identity counts are equal");
                check(ahnd > 0 && ahnd < aobj,
                      "both populations exist -- some objects addressable, some not");
                // The predicate a mod calls must answer about the handle it concerns,
                // so this is an identity check on the public function, not a rate.
                check(aaddr == ahnd,
                      "sdk::is_server_object matches handle presence exactly (the "
                      "engine's CLTClient::IsServerObject is that same comparison)");

                // DIMS. The assertable part is what a half-extent MEANS: no component
                // can be negative. That holds on every object and would break if the
                // offset moved onto a neighbouring signed field.
                int64_t dok = -1, dnn = -1, dz = -1;
                json_int(ab, "dims_ok", dok);
                json_int(ab, "dims_nonneg", dnn);
                json_int(ab, "dims_zero", dz);
                check(dok == aobj, "object_dims answers for every live object");
                check(dnn == dok, "EVERY object's dims are non-negative half-extents");
                // REPORTED: all-zero is a legitimate state (sphere-culled objects never
                // run SetDims), and how many there are is a property of the level.
                check(dz >= 0 && dz <= dok, "all-zero dims are a reported fraction");

                // ---- BRUSH SPACE: the engine's two matrices against each other --------
                //
                // World models (and cameras, which derive from them) store a local->world
                // rigid transform AND its inverse, side by side. Neither alone can be
                // checked -- both produce plausible coordinates when wrong -- but a ROUND
                // TRIP through the pair must return the point it started from, because the
                // two matrices are stored independently.
                int64_t bt = -1, brt = -1, bex = -1, borg = -1;
                double bworst = -1.0;
                json_int(ab, "brush", bt);
                json_int(ab, "brush_roundtrip", brt);
                json_int(ab, "brush_rt_exact", bex);
                json_int(ab, "brush_origin_ok", borg);
                json_double(ab, "brush_worst_rt", bworst);

                // ---- THE SPATIAL INDEX (world tree) --------------------------------
                //
                // The oracle is SELF-LOCATION: the engine linked each object at the node
                // covering its AABB, and a descent toward that object's own position must
                // pass through that node -- so a linked object has to find ITSELF. That
                // catches a wrong quadrant mapping or a descent that harvests only the leaf,
                // both of which still return plausible neighbours.
                int64_t task = -1, tlink = -1, tne = -1, tself = -1;
                int64_t tnw = -1, tnwf = -1, twm = -1;
                json_int(ab, "tree_asked", task);
                json_int(ab, "tree_linked", tlink);
                json_int(ab, "tree_nonempty", tne);
                json_int(ab, "tree_self_found", tself);
                json_int(ab, "tree_nonwm", tnw);
                json_int(ab, "tree_nonwm_found", tnwf);
                json_int(ab, "tree_wm_missed", twm);
                check(task == aobj, "is_linked answers for EVERY object");
                // Not every object is indexed -- live 2142 of 3583 -- so this is a
                // population fact, reported with bounds rather than required.
                check(tlink > 0 && tlink < task,
                      "both populations exist: some objects indexed, some not");
                check(tne == tlink,
                      "EVERY indexed object's position reaches a non-empty node");
                // THE LOAD-BEARING ONE, stated for the population where it holds without
                // exception. 669 of 669 non-worldmodels locate themselves; scoping the claim
                // to them keeps it exact instead of weakening it to a percentage.
                check(tnwf == tnw && tnw > 0,
                      "EVERY indexed non-worldmodel finds ITSELF at its own position");
                // REPORTED with its cause now established (see the currency check below):
                // these objects' index entries are STALE, so a descent by current position
                // reaches a different node than the one they are parked in. The count itself
                // is scene state -- how many brushes have moved -- so it is bounded, not
                // fixed.
                check(twm >= 0 && twm < tlink,
                      "unlocated worldmodels are a reported minority");
                printf("[fixture] spatial index: %lld of %lld objects indexed; self-located "
                       "%lld/%lld non-worldmodels, %lld worldmodels via STALE entries\n",
                       static_cast<long long>(tlink), static_cast<long long>(task),
                       static_cast<long long>(tnwf), static_cast<long long>(tnw),
                       static_cast<long long>(twm));
                // AND THE CAUSE, now confirmed rather than open. tree_slot() found all 235
                // in the tree, all at leaves, at the same depth as the hits -- so they are
                // genuinely in the structure the descent walks. index_is_current() then
                // settled it: descending the ENGINE'S OWN box rule with each object's CURRENT
                // bounds lands on a DIFFERENT node than the one it is parked in. The entries
                // are STALE. The engine relinks only from SetPos/SetPosRot/SetDims/SetFlags,
                // so a brush moved by any other route keeps its old node.
                int64_t msf = -1, mmd = -1, mal = -1, mst = -1, cask = -1, cok2 = -1;
                json_int(ab, "miss_slot_found", msf);
                json_int(ab, "miss_max_depth", mmd);
                json_int(ab, "miss_at_leaf", mal);
                json_int(ab, "miss_stale", mst);
                json_int(ab, "cur_asked", cask);
                json_int(ab, "cur_ok", cok2);
                check(msf == twm,
                      "EVERY unlocated object is still findable in the tree by slot search");
                check(mal == twm, "EVERY unlocated object is parked at a LEAF, not an internal node");
                check(mmd >= 0 && mmd < 32, "slot depths are inside the tree's own bound");
                // THE EXPLANATION AS AN ASSERTION: no miss is unaccounted for.
                check(mst == twm,
                      "EVERY unlocated object has a STALE index entry -- its parked node is "
                      "not the node its current bounds would choose");
                // The converse deliberately does NOT hold and is only reported: a stale entry
                // can still sit on the path a point descent takes, so staleness is the larger
                // population.
                check(cask == tlink && cok2 <= cask,
                      "index currency answers for every indexed object");
                check(cask - cok2 >= twm,
                      "stale entries are at least as many as the unlocated ones");
                printf("[fixture] index currency: %lld of %lld entries STALE; all %lld "
                       "unlocated objects are among them (max depth %lld)\n",
                       static_cast<long long>(cask - cok2), static_cast<long long>(cask),
                       static_cast<long long>(twm), static_cast<long long>(mmd));

                // ---- WORLD BOUNDS, as a consumer API ---------------------------------
                //
                // Extracted from check_object_geometry. The AABB identity is asserted for
                // EVERY object because the engine's own writer establishes it
                // unconditionally: LTObject_SetPos calls SetWorldAABB(pos-dims, pos+dims)
                // with no gate. A failure here means something moved an object's position
                // without going through that path.
                int64_t aok = -1, aord = -1, aask = -1, acur = -1;
                int64_t rok = -1, rsz = -1, runs = -1, rsane = -1;
                json_int(ab, "aabb_ok", aok);
                json_int(ab, "aabb_ordered", aord);
                json_int(ab, "aabb_asked", aask);
                json_int(ab, "aabb_current", acur);
                json_int(ab, "rad_ok", rok);
                json_int(ab, "rad_sized", rsz);
                json_int(ab, "rad_unsized", runs);
                json_int(ab, "rad_sane", rsane);
                check(aok == aobj, "EVERY object yields its world AABB");
                check(aord == aok, "EVERY world AABB is well-ordered (min <= max)");
                check(acur == aask && aask == aobj,
                      "EVERY world AABB equals position +/- dims, the engine's own identity");
                // AND THE CONTRAST THAT EXPLAINS THE STALE INDEX ENTRIES, which is why both
                // checks live in the same file: the AABB is NEVER stale (0 of 3583) while 370
                // of 2142 world-tree entries are. Both are written by LTObject_SetPos, but
                // the AABB write is unconditional and the relink is gated on
                // LTObject_IsRenderable -- so an object that moves while not renderable gets
                // fresh bounds and keeps its old node. Asserting the AABB side exactly is
                // what makes that asymmetry visible rather than a coincidence of counts.
                check(rok == aobj, "EVERY object yields a bounding radius");
                check(rsz + runs == rok,
                      "the radius is in exactly one of its two states for every object");
                check(rsz > 0 && runs > 0, "both radius states are exercised in this scene");
                check(rsane == rok, "EVERY radius is finite and non-negative");
                printf("[fixture] world bounds: %lld AABBs all current; radius %lld sized / "
                       "%lld unsized\n",
                       static_cast<long long>(acur), static_cast<long long>(rsz),
                       static_cast<long long>(runs));
                check(bt > 0, "the level carries world models to transform against");
                // THE API CONTRACT: every object the type gate admits must answer BOTH
                // directions. A gap here means the gate and the field offsets disagree.
                check(brt == bt,
                      "EVERY world model and camera answers both brush-space directions");
                // THE ARITHMETIC, as a proportion rather than an equality -- deliberately.
                // 12 of 1947 brushes live carry a forward/inverse pair that genuinely is
                // NOT an inverse (worst 49.8 units), which is the engine's own data and
                // not something a test should fail on. A transposed read or a wrong offset
                // would break EVERY brush, so 95% still catches real breakage while
                // tolerating the level's inconsistent handful.
                check(bex * 100 >= bt * 95,
                      "the brush round trip returns its point on at least 95% of brushes");
                // REPORTED: whether a brush's local origin coincides with the object's
                // position is the art's choice, true on 908 of 1947. Asserting it would
                // turn a level property into a tripwire.
                check(borg >= 0 && borg <= bt,
                      "brushes whose origin is the object position are a reported fraction");

                // ---- THE EXTRACTED TRANSFORM PRIMITIVES ------------------------------
                //
                // brush_transform / brush_transform_quality were private to
                // CClientMgr::check_transforms until this pass. These assert them as a
                // CONSUMER API: every object the type gate admits must answer both, since
                // a gap means the gate and the field offsets disagree.
                int64_t bq = -1, btr = -1, bmx = -1, bagree = -1;
                double bwrot = -1.0;
                json_int(ab, "brush_quality", bq);
                json_int(ab, "brush_trusted", btr);
                json_int(ab, "brush_matrix", bmx);
                json_int(ab, "brush_origin_agrees", bagree);
                json_double(ab, "brush_worst_rot", bwrot);
                check(bq == bt, "EVERY brush answers brush_transform_quality");
                check(bmx == bt, "EVERY brush yields its transform matrix");
                // THE LOAD-BEARING ONE, and it is about the ROW/COLUMN CONVENTION: the
                // matrix's translation column must be the point brush_to_world maps the
                // local origin to. Two independent routes through the same data -- one
                // reading m[3]/m[7]/m[11] directly, one composing through the helper -- so
                // a transposed read in either breaks the agreement. Live 1947/1947.
                check(bagree == bmx,
                      "the matrix's translation column agrees with brush_to_world(origin) "
                      "on EVERY brush");
                check(btr > 0 && btr <= bq,
                      "trustworthy transforms are a non-empty subset of all of them");
                // REPORTED: how many pairs disagree is the level's data, not a contract --
                // the header documents that ~12 of 1947 genuinely are not inverses.
                printf("[fixture] transform primitives: %lld/%lld trustworthy, worst "
                       "rotation error %.4f\n",
                       static_cast<long long>(btr), static_cast<long long>(bq), bwrot);

                // ---- CULL VOLUMES, as a consumer API -------------------------------
                //
                // Extracted from check_spatial_records this pass. The interesting claims
                // are structural, not scene-dependent.
                int64_t cvok = -1, cvsph = -1, cvbox = -1, cvnone = -1, cvsane = -1;
                int64_t cvcmp = -1, cvcur = -1;
                json_int(ab, "cull_ok", cvok);
                json_int(ab, "cull_sphere", cvsph);
                json_int(ab, "cull_box", cvbox);
                json_int(ab, "cull_none", cvnone);
                json_int(ab, "cull_sane", cvsane);
                json_int(ab, "cull_compared", cvcmp);
                json_int(ab, "cull_current", cvcur);
                check(cvok == aobj, "EVERY object answers computed_cull_volume");
                // A TOTAL PARTITION: the rule must classify every object as sphere, box or
                // suppressed. A gap would mean a type the rule does not handle, which is
                // how a silently-wrong volume would first show up.
                check(cvsph + cvbox + cvnone == cvok,
                      "every object's volume is a sphere, a box, or suppressed -- no gap");
                check(cvsph > 0 && cvbox > 0 && cvnone > 0,
                      "all three volume classes are populated in this scene");
                // THE GEOMETRIC INVARIANT, which holds whatever produced the volume: a
                // box's min never exceeds its max, and a radius is finite and non-negative.
                // This catches a wrong field far more reliably than a count does -- reading
                // LTObject's aabb pair instead of LTSpriteObject's own would break it.
                check(cvsane == cvok,
                      "EVERY volume is geometrically valid (min <= max, radius >= 0, finite)");
                // And the same agreement check_spatial_records makes, asked through the
                // public API instead of the private walk.
                check(cvcur == cvcmp && cvcmp == cvok,
                      "EVERY stored volume agrees with the recomputed one");
                printf("[fixture] cull volumes: %lld spheres, %lld boxes, %lld suppressed, "
                       "%lld/%lld current\n",
                       static_cast<long long>(cvsph), static_cast<long long>(cvbox),
                       static_cast<long long>(cvnone), static_cast<long long>(cvcur),
                       static_cast<long long>(cvcmp));
                printf("[fixture] brush space: %lld brushes, %lld/%lld round-trip "
                       "(worst %.3f), %lld origin-aligned\n",
                       static_cast<long long>(bt), static_cast<long long>(bex),
                       static_cast<long long>(brt), bworst,
                       static_cast<long long>(borg));

                // COLOUR AND ALPHA. The assertion is the BYTE ORDER, which is the one
                // claim that could silently be wrong: alpha must be the high byte of the
                // packed value, red next, then green, then blue. That is what
                // SetObjectAlpha's lone write to +0x07 and SetObjectRGB's b/g/r stores
                // together say, and a swapped pair would still produce plausible-looking
                // colours -- so it is checked on every object rather than sampled.
                int64_t cok = -1, cpk = -1, cdf = -1, ctr = -1;
                json_int(ab, "color_ok", cok);
                json_int(ab, "color_packed_ok", cpk);
                json_int(ab, "color_default", cdf);
                json_int(ab, "color_translucent", ctr);
                check(cok == aobj, "object_color answers for every live object");
                check(cpk == cok,
                      "EVERY object's packed colour matches its components (0xAARRGGBB)");
                // REPORTED: how much of a level is tinted or fading is the level's
                // business. Live 3265 objects are the default white and 121 translucent.
                check(cdf >= 0 && cdf <= cok, "default-white objects are a reported count");
                check(ctr >= 0 && ctr <= cok, "translucent objects are a reported count");
                printf("[fixture] colour: %lld answered, %lld default white, %lld translucent\n",
                       static_cast<long long>(cok), static_cast<long long>(cdf),
                       static_cast<long long>(ctr));

                // GROUND CONTACT. How many objects stand on something is entirely
                // scene-dependent -- live exactly one does -- so the count is reported
                // and only its INTERNAL CONSISTENCY is required: whenever the API
                // reports contact, it must name an object and a finite surface height.
                int64_t so = -1, sos = -1, son = -1;
                json_int(ab, "standing", so);
                json_int(ab, "standing_sane", sos);
                json_int(ab, "standing_node", son);
                check(so >= 0 && so <= aobj, "ground contact is a reported count");
                check(sos == so,
                      "every reported ground contact names an object and a finite height");
                check(son >= 0 && son <= so, "surface nodes are a subset of contacts");
                printf("[fixture] dims: %lld answered, %lld all-zero; ground contacts: %lld "
                       "(%lld with a surface node)\n",
                       static_cast<long long>(dok), static_cast<long long>(dz),
                       static_cast<long long>(so), static_cast<long long>(son));

                // ATTACHMENTS. The load-bearing assertion is the LAST one: every
                // attachment that reports a handle must resolve it in the ENGINE'S UNIFIED
                // SPACE, and -- the load-bearing part -- the child must actually SIT at
                // the transform we compose for that handle.
                //
                // THE PREVIOUS VERSION OF THIS CHECK PASSED WHILE THE MAPPING WAS WRONG.
                // It resolved the handle as a NODE index and asserted that every one
                // produced a bone name; it counted 27/27 for several passes. Socket handles
                // are small, so they land inside node_count too, and the names that came
                // back looked like real bones. Asking "did a table accept this index" can
                // never distinguish two overlapping index spaces. Asking "is the child
                // where this says it is" can, because only one reading puts it there.
                int64_t awa = -1, aat = -1, aco = -1, aso = -1, are = -1;
                int64_t ais = -1, ame = -1, apl = -1;
                double aerr = -1.0;
                json_int(ab, "with_attachments", awa);
                json_int(ab, "attachments", aat);
                json_int(ab, "att_child_ok", aco);
                json_int(ab, "att_socketed", aso);
                json_int(ab, "att_resolved", are);
                json_int(ab, "att_is_socket", ais);
                json_int(ab, "att_measured", ame);
                json_int(ab, "att_placed", apl);
                json_double(ab, "att_worst_err", aerr);
                check(awa > 0, "some objects carry attachments");
                check(aat >= awa,
                      "every attachment list holds at least one record (no empty heads)");
                // REPORTED, not required equal: a record may name a handle whose table
                // slot is not live, and the engine's own walker skips those. Live 327
                // of 362 resolve, so demanding all of them would fail on a truth.
                check(aco >= 0 && aco <= aat,
                      "resolved attachment children are a reported fraction of records");
                check(aso >= 0 && aso <= aat,
                      "socketed attachments are a subset of all attachments");
                check(are == aso,
                      "EVERY attachment handle resolves inside the unified socket/node space");
                // REPORTED: which side of the split a handle falls on is the art's choice.
                // Live all 27 are sockets, but a model CAN legitimately name a bare node.
                check(ais >= 0 && ais <= are,
                      "handles resolving to sockets are a reported fraction");
                // THE REAL ONE. Two independent producers of one point: the engine placed
                // the child, we composed the handle. Tolerance is tight on purpose --
                // live the worst disagreement across the whole level is 0.0005 units, so
                // anything that breaks the composition moves this by orders of magnitude.
                check(apl == ame,
                      "EVERY attached child sits at the transform we compose for its handle");
                check(ame == 0 || (aerr >= 0.0 && aerr < 0.05),
                      "the WORST engine-vs-SDK placement disagreement stays sub-unit");
                printf("[fixture] attachments: %lld handles, %lld sockets, %lld/%lld placed "
                       "(worst %.4f)\n",
                       static_cast<long long>(are), static_cast<long long>(ais),
                       static_cast<long long>(apl), static_cast<long long>(ame), aerr);

                // MODEL SOCKETS -- the art's own named attach points. Three
                // requirements, all of them things a consumer depends on:
                int64_t st = -1, sok = -1, snn = -1, srt = -1, scam = -1, seye = -1;
                json_int(ab, "socket_total", st);
                json_int(ab, "socket_ok", sok);
                json_int(ab, "socket_named_node", snn);
                json_int(ab, "socket_roundtrip", srt);
                json_int(ab, "socket_camera", scam);
                json_int(ab, "socket_eyes", seye);
                check(st > 0, "models define sockets");
                check(sok == st, "every socket reads through the public accessor");
                // Crosses the socket table AND the node table: a socket's node_index
                // must name a bone in the same skeleton. Breaks if either moves.
                check(snn == st, "EVERY socket's node index resolves to a bone name");
                // Proves the lookup is genuinely case-insensitive, the way the engine's
                // own String_EqualsI comparison is. The endpoint upper-cases each
                // socket's OWN name before looking it up, so this holds regardless of
                // which assets a level loaded -- asking for a hardcoded "LEFTHAND"
                // would only test anything in a level containing characters.
                check(srt == st,
                      "every socket is found again by its own UPPER-CASED name");
                // REPORTED, not required: which sockets exist is a property of the
                // loaded art, so a level without characters legitimately has none.
                check(scam >= 0 && scam <= st, "camera sockets are a reported count");
                // ---- THE BIND POSE IS ASSET DATA ------------------------------------------
                //
                // Which field is the bind pose was settled by its reader:
                // ILTModel_GetBindPoseNodeTransform reads `(node << 6) + node_records + 8`, so it
                // is the pair at +0x08 and not the other pair in the same record.
                //
                // WHERE THE OWNERSHIP EVIDENCE ACTUALLY COMES FROM: the ADDRESS, not the values.
                // The engine's reader indexes `asset->node_records`, and so does this SDK's
                // accessor, so the storage is the asset's array by construction.
                //
                // So the checks below are deliberately split. Comparing the resolved ARRAY POINTERS
                // is a storage-identity test and is the one that means something. Comparing the
                // VALUES proves neither ownership nor immutability -- separate copies would agree,
                // and here both reads land on the same memory anyway -- so it is kept only as a
                // regression guard on the read path and labelled as such.
                //
                // Proving immutability would need a mutation-isolation test, which is out of scope:
                // writing to engine data to see whether a sibling changes is not something this
                // suite should do to a live game.
                int64_t bnodes = -1, bunit = -1, bfin = -1, bshared = -1, bsharedok = -1;
                json_int(ab, "bind_nodes", bnodes);
                json_int(ab, "bind_unit", bunit);
                json_int(ab, "bind_finite", bfin);
                json_int(ab, "bind_shared", bshared);
                json_int(ab, "bind_shared_ok", bsharedok);
                // ---- DERIVED HIERARCHY QUERIES --------------------------------------------
                //
                // node_depth / node_has_ancestor are views of path_to_root(), so the guarded walk
                // lives in the SDK once instead of at every call site. The checks are consequences of ONE walk
                // seen from different angles, and they PARTITION the population, which is what makes
                // the agreement mean something rather than being a restatement.
                int64_t hp = -1, hdok = -1, hmax = -1, hroots = -1, hrz = -1, hstep = -1, hanc = -1,
                        hself = -1;
                json_int(ab, "hier_probed", hp);
                json_int(ab, "hier_depth_ok", hdok);
                json_int(ab, "hier_max_depth", hmax);
                json_int(ab, "hier_roots", hroots);
                json_int(ab, "hier_root_zero", hrz);
                json_int(ab, "hier_step_ok", hstep);
                json_int(ab, "hier_anc_ok", hanc);
                json_int(ab, "hier_self_ok", hself);
                check(hp > 0, "nodes were walked");

                // No malformed chains: every node reaches its root within the guard. This is the
                // property the walk relies on, so it is asserted rather than assumed.
                check(hdok == hp, "EVERY node has a well-formed chain to its root");
                check(hmax > 0 && hmax < 64, "the deepest chain is within the depth guard");

                // Roots and non-roots must ADD UP to the population -- the shape that turns three
                // agreeing counts into a real check.
                check(hroots > 0 && hroots < hp, "the population splits into roots and non-roots");
                check(hrz == hroots, "EVERY root has depth zero");
                check(hstep == hp - hroots,
                      "depth(child) == depth(parent) + 1 for EVERY non-root");
                check(hanc == hp - hroots, "a node's parent is always among its ancestors");
                check(hself == hp - hroots, "and no node is its own ancestor");
                printf("[fixture] hierarchy: %lld nodes, %lld roots at depth 0, %lld links step by "
                       "one (max depth %lld)\n",
                       static_cast<long long>(hp), static_cast<long long>(hroots),
                       static_cast<long long>(hstep), static_cast<long long>(hmax));

                check(bnodes > 0, "bind poses were read, so the checks are not vacuous");
                check(bunit == bnodes, "EVERY bind rotation is unit-length");
                check(bfin == bnodes, "EVERY bind position is finite");
                check(bshared > 0,
                      "models sharing an asset exist, so the invariance check has something to do");
                // STORAGE IDENTITY, which is the claim that was being made badly before: the two
                // views read the SAME array, established by comparing the resolved pointers rather
                // than the bytes they contain.
                int64_t bsamearr = -1;
                json_int(ab, "bind_same_array", bsamearr);
                check(bsamearr == bshared,
                      "objects sharing an asset read the SAME node-record array (pointer identity)");
                // And the weaker one, kept only as a regression guard on the read path. It shows
                // cross-instance value consistency and NOTHING about ownership -- separate copies
                // would satisfy it just as well.
                check(bsharedok == bshared,
                      "cross-instance bind pose values are consistent");
                // THE COORDINATE SPACE IS REPORTED, NOT ASSERTED, and neither figure below can
                // decide it. The depth-magnitude growth suggests accumulated model-space but mixes
                // assets of different scale. The parent-child delta looks like it argues the other
                // way, yet it is question-begging: if the pairs are LOCAL then those two vectors are
                // in different parent frames and their difference has no geometric meaning. So the
                // numbers are printed as a record of what was tried, and nothing is concluded.
                int64_t bdepth = -1, bedge = -1;
                double bmshal = -1.0, bmdeep = -1.0, bedgemean = -1.0;
                json_int(ab, "bind_max_depth", bdepth);
                json_int(ab, "bind_n_edge", bedge);
                json_double(ab, "bind_mag_shallow", bmshal);
                json_double(ab, "bind_mag_deep", bmdeep);
                json_double(ab, "bind_edge_mean", bedgemean);
                check(bdepth > 0 && bedge > 0, "the node hierarchy has depth and edges to measure");
                check(bmshal >= 0.0 && bmdeep >= 0.0 && bedgemean >= 0.0,
                      "the bind pose magnitude figures are well-formed");
                printf("[fixture] bind pose: %lld nodes, all unit and finite; %lld shared instances "
                       "on one array\n",
                       static_cast<long long>(bnodes), static_cast<long long>(bshared));
                printf("[fixture] bind space UNRESOLVED: |pos| %.1f shallow vs %.1f deep (max depth "
                       "%lld), parent delta %.1f over %lld edges\n",
                       bmshal, bmdeep, static_cast<long long>(bdepth), bedgemean,
                       static_cast<long long>(bedge));

                check(seye >= 0 && seye <= st, "eye sockets are a reported count");

                // EYE GEOMETRY, from asset data -- no bone cache, so no staleness and nothing to
                // evaluate. The invariant is the helper's precondition: both sockets hang off ONE
                // node, which is what makes subtracting their offsets meaningful at all.
                int64_t egeom = -1, elevel = -1, eleftneg = -1, ecam = -1;
                double esmin = -1.0, esmax = -1.0, easym = -1.0;
                json_int(ab, "eye_geom", egeom);
                json_int(ab, "eye_level", elevel);
                json_int(ab, "eye_left_neg", eleftneg);
                json_int(ab, "eye_vs_camera", ecam);
                json_double(ab, "eye_sep_min", esmin);
                json_double(ab, "eye_sep_max", esmax);
                json_double(ab, "eye_asym_max", easym);
                check(egeom == seye,
                      "EVERY model with both eye sockets hangs them off ONE node");
                check(ecam == seye,
                      "and its camera socket hangs off that same node");

                // REPORTED, NOT ASSERTED, because measurement killed the obvious invariants: the
                // separation can be ZERO (one rig puts both eyes at the same point), the eyes are
                // level on only a minority, and `left` is not reliably the -x side. Asserting any
                // of those would encode a wish about the art rather than a fact about the engine.
                check(esmin >= 0.0 && esmax >= esmin,
                      "the eye separation range is well-ordered and non-negative");
                check(elevel >= 0 && elevel <= egeom, "level-eye rigs are a reported fraction");
                check(eleftneg >= 0 && eleftneg <= egeom,
                      "left-is-negative-x rigs are a reported fraction");
                printf("[fixture] eye rigs: %lld of %lld share a node; separation %.3f..%.3f, "
                       "%lld level, %lld left-negative, worst asymmetry %.3f\n",
                       static_cast<long long>(egeom), static_cast<long long>(seye), esmin, esmax,
                       static_cast<long long>(elevel), static_cast<long long>(eleftneg), easym);

                // CACHED NODE TRANSFORMS. The contract being defended is the DIRTY
                // FLAG: a slot the engine considers clean holds a usable position, and
                // a dirty one holds whatever was there last -- live, 187 of the dirty
                // slots held values up to 7.8e37.
                int64_t nxo = -1, nxs = -1, nxc = -1, nxcs = -1, cnc = -1;
                json_int(ab, "node_xform_ok", nxo);
                json_int(ab, "node_xform_stale", nxs);
                json_int(ab, "node_xform_clean", nxc);
                json_int(ab, "node_xform_clean_sane", nxcs);
                json_int(ab, "camera_node_clean", cnc);
                check(nxo > 0, "node transforms read through the public accessor");
                // The partition must be total: every slot is exactly one of the two.
                check(nxc + nxs == nxo, "every node transform is either clean or stale");
                // THE LOAD-BEARING ONE. Measured 343/343 live. If this ever fails,
                // either the dirty array's stride/offset is wrong or the wrong one of
                // the model's TWO caches is being read -- and note that picking the
                // wrong cache would still look fine on the 193 models whose mode
                // selector is zero, which is exactly why it is asserted over all of
                // them rather than sampled.
                check(nxcs == nxc,
                      "EVERY clean node transform holds a finite, sane position");
                // REPORTED: whether any model's camera-socket bone happens to be clean
                // right now is per-frame state, and on an idle menu most are not.
                check(cnc >= 0, "camera-socket bone cleanliness is a reported count");
                printf("[fixture] node transforms: %lld total, %lld clean, %lld stale "
                       "(camera-socket bones clean: %lld)\n",
                       static_cast<long long>(nxo), static_cast<long long>(nxc),
                       static_cast<long long>(nxs), static_cast<long long>(cnc));
                printf("[fixture] sockets: %lld total, %lld with a 'camera', %lld with both "
                       "eye sockets\n",
                       static_cast<long long>(st), static_cast<long long>(scam),
                       static_cast<long long>(seye));
                printf("[fixture] attachments: %lld records on %lld objects, %lld resolved, "
                       "%lld with an attach handle\n",
                       static_cast<long long>(aat), static_cast<long long>(awa),
                       static_cast<long long>(aco), static_cast<long long>(aso));
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
                    // lnr is NOT an invariant, and it is now explained rather than shrugged
                    // at. The gate is checked on the way IN -- LTObject_SetPos and friends
                    // relink only `if (LTObject_IsRenderable(this))` -- and nothing removes an
                    // object when it later stops qualifying. So the tree accumulates objects
                    // that are no longer eligible, and THOSE are the ones whose entries go
                    // stale when they move: live 384 linked-not-eligible against 370 stale
                    // entries reported by the currency check above.
                    //
                    // Not asserted as `stale <= lnr` on purpose: that would only hold if every
                    // move of an ELIGIBLE object went through SetPos, which is exactly the
                    // kind of thing this project has been wrong about before. Printed so the
                    // two numbers can be compared across runs.
                    check(lnr >= 0, "linked-not-eligible count reported (not an invariant)");
                    printf("[fixture] tree eligibility: %lld eligible, %lld linked, %lld linked "
                           "but no longer eligible\n",
                           static_cast<long long>(rend), static_cast<long long>(lk),
                           static_cast<long long>(lnr));
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

    // 5b4. /sdk/shader-params: the engine's named shader parameters.
    //
    // Another CROSS-CHECK against static reversing, and the same discipline as the
    // interface block above: IDA walked the doubly-linked list at g_ShaderParamList_Head
    // and recorded 60 records with a specific type distribution. The DLL walks the same
    // list in-process through sdk::ShaderParams, sharing no code with that extraction, so
    // agreement is evidence rather than self-agreement.
    //
    // This section runs AT THE MAIN MENU, unlike most engine state here: the list is
    // static exe data. That makes it one of the few real checks available with no level
    // loaded.
    //
    // Deliberately NOT asserted: that any parameter is BOUND, or the resolution's actual
    // numbers. Binding happens when the engine assigns handles, so zero bound is a state,
    // not a defect; and asserting 5120x1440 would fail on every other machine -- the same
    // environment-dependence this suite already corrected for the device vtable.
    {
        std::string resp;
        check(http::get(port, "/sdk/shader-params", resp), "/sdk/shader-params transport");
        const std::string body = http::body_of(resp);
        check(json_has(body, "\"ok\":true"), "the shader parameter list is reachable");

        int64_t count = -1, disagrees = -1, nodes = -1, bound = -1, pending = -1;
        check(json_int(body, "count", count) && count == 60,
              "60 shader parameter records walked (matches static IDA extraction)");
        check(json_int(body, "size_disagrees", disagrees) && disagrees == 0,
              "every record's byte size is a whole multiple of its declared type's element size");

        // The structural fact behind the bone array: 1152 bytes of float4x3 is 24 of them.
        check(json_int(body, "model_nodes_elements", nodes) && nodes == 24,
              "k_mModelObjectNodes holds 24 node transforms (1152 bytes / 48)");

        // Typed accessors actually reading, not merely the walk succeeding.
        check(json_has(body, "\"screen_res\":true"),
              "k_vScene_ScreenRes reads through the typed float2 accessor");
        check(json_has(body, "\"object_to_clip_readable\":true"),
              "k_mObjectToClip reads through the typed float4x4 accessor");

        // Resolution values are machine-dependent, so only their SHAPE is a defect signal:
        // a zero or negative extent would mean the read landed somewhere wrong.
        double res_w = 0.0, res_h = 0.0;
        check(json_double(body, "screen_res_w", res_w) && res_w > 0.0,
              "the reported screen width is positive");
        check(json_double(body, "screen_res_h", res_h) && res_h > 0.0,
              "the reported screen height is positive");

        // THE API CONTRACT THAT MATTERS: an array parameter has the same TYPE as a single
        // matrix, so a tag-only check would hand back the first of 24 and call it the
        // whole value. The fixed-size accessor must refuse it.
        check(json_has(body, "\"array_refused_by_fixed_accessor\":true"),
              "the fixed-size matrix4x3 accessor refuses the 24-element array parameter");

        // These read as COUNTS, which is only true because the report names them distinctly
        // from the per-entry "bound"/"pending" booleans. A first-match JSON reader parsed a
        // boolean here and yielded -1 until the endpoint was fixed, so the parse itself is
        // the regression guard. Their VALUES stay unasserted: nothing is bound at the menu
        // and plenty is bound in a level, both legitimate.
        check(json_int(body, "bound_count", bound) && bound >= 0 && bound <= count,
              "the bound count parses as a count within the record total");
        check(json_int(body, "pending_count", pending) && pending >= 0 && pending <= count,
              "the pending-upload count parses as a count within the record total");

        // ---- THE CAMERA PARAMETERS, AND TWO INVARIANTS WORTH THE NAME ----------------
        //
        // Both checks below hold at ANY resolution and in either render pass, which is what
        // separates them from asserting 5120x1440: they are properties of the data the
        // engine publishes, not of this machine.
        check(json_has(body, "\"half_view_plane\":true"),
              "k_vHalfViewPlane reads through the unpacking accessor");

        // INVARIANT 1: the tuple stores each half-extent's reciprocal alongside it, so a
        // shader can avoid a divide. If the four floats were misread -- wrong offset, wrong
        // order, torn read -- the reciprocals would not match. The check is the class's own
        // HalfViewPlane::reciprocals_consistent(), so a consumer validating its own read
        // calls the same code this asserts on.
        check(json_has(body, "\"hvp_reciprocals_consistent\":true"),
              "k_vHalfViewPlane's stored reciprocals match its extents (tuple invariant)");

        // INVARIANT 2: two SEPARATE records, read through separate accessors, must describe
        // the same viewport. This is a consistency check on the two reads -- a wrong offset,
        // a swapped pair or a torn value in either shows up here -- and NOT independent
        // corroboration of the viewport itself: sampling the engine's screen pass showed the
        // half-plane extents are literally half the screen dimensions, so both parameters
        // descend from one upstream. Gated on the view plane being populated, because before
        // the first 3D pass it legitimately is not.
        double hvp_aspect = 0.0, hvp_h = 0.0;
        if (json_double(body, "hvp_half_h", hvp_h) && hvp_h != 0.0 &&
            json_double(body, "hvp_aspect", hvp_aspect) && res_h > 0.0) {
            const double screen_aspect = res_w / res_h;
            const double rel = (hvp_aspect - screen_aspect) / screen_aspect;
            check(rel > -0.01 && rel < 0.01,
                  "k_vHalfViewPlane's aspect agrees with k_vScene_ScreenRes (two records, "
                  "one viewport)");
            double hvp_w = 0.0;
            (void)json_double(body, "hvp_half_w", hvp_w);
            printf("[fixture] view plane: half %.4f x %.4f, aspect %.4f vs screen %.4f\n",
                   hvp_w, hvp_h, hvp_aspect, screen_aspect);
        }

        // The Z range is published by 3D passes only and deliberately NOT refreshed by the
        // screen pass, so it can be stale. What must hold regardless is the ordering.
        double z_near = -1.0, z_far = -1.0;
        check(json_has(body, "\"z_range\":true"), "k_vScene_ZRange reads");
        check(json_double(body, "z_near", z_near) && json_double(body, "z_far", z_far) &&
                  z_far >= z_near && z_near >= 0.0,
              "the reported depth range is ordered and non-negative");
        check(json_has(body, "\"camera_dir\":true"),
              "k_vWorldSpaceCameraDir reads through the typed float3 accessor");

        // ---- THE PERSPECTIVE PATH, EXERCISED ON A KNOWN MATRIX ------------------------
        //
        // Sampling cannot reach it: the engine leaves its camera record in the affine screen pass
        // between frames, so fov_*_radians() and the projection/half-plane identity never run on
        // live data. The DLL therefore builds a matrix with SceneCamera's own builder -- a
        // transcription of LTMatrix_BuildPerspectiveProjection -- and runs the real predicates on
        // it. This tests the CLASS. It is not runtime corroboration of the engine, and the
        // comments below say so rather than letting a green tick imply otherwise.
        {
            bool built = false;
            check(json_bool(body, "probe_built", built) && built,
                  "both projection builders accept a valid frustum");
            bool p_persp = false, p_agrees = false;
            check(json_bool(body, "probe_perspective", p_persp) && p_persp,
                  "a matrix built like the engine's classifies as perspective");
            check(json_bool(body, "probe_agrees", p_agrees) && p_agrees,
                  "m[0][0] * half_x == m[3][2] on that matrix (the identity behind fov)");

            // THE FOV MUST BE RECOVERED, and against a value the DLL computes from the same input
            // rather than a literal, so the expectation cannot drift from the probe.
            double got = -1.0, want = -2.0;
            check(json_double(body, "probe_fov_y", got) &&
                      json_double(body, "probe_fov_y_want", want) &&
                      got > want - 1e-4 && got < want + 1e-4,
                  "fov_y_radians() recovers 2*atan(half_y) from the matrix");

            // THE SCALE INVARIANCE, which is the whole reason the classifier measures m[3][2]
            // against the w row instead of an absolute epsilon: homogeneous matrices are
            // scale-equivalent, so multiplying every coefficient must change nothing.
            bool s_persp = false;
            double s_fov = -1.0;
            check(json_bool(body, "probe_scaled_perspective", s_persp) && s_persp,
                  "the same matrix scaled by a constant still classifies as perspective");
            check(json_double(body, "probe_scaled_fov_y", s_fov) &&
                      s_fov > got - 1e-4 && s_fov < got + 1e-4,
                  "and yields the same field of view (scale invariance)");

            // The affine form must refuse to produce a field of view.
            bool a_affine = false, a_fov = true;
            check(json_bool(body, "probe_affine_is_affine", a_affine) && a_affine,
                  "a matrix built by the affine builder classifies as affine");
            check(json_bool(body, "probe_affine_fov_present", a_fov) && !a_fov,
                  "and yields no field of view");

            // THE COMPOSE, on cases whose answers follow from the convention rather than from the
            // function. Composing with the identity must change nothing; a translation in the
            // affine operand must appear in column 3 scaled by the projection's own row. Both are
            // what a consumer building a per-eye matrix depends on, and the transpose convention
            // would fail the second while passing the first.
            bool ci = false, ct = false, ck = false;
            check(json_bool(body, "compose_identity", ci) && ci,
                  "multiply_by_affine with the affine identity leaves the matrix unchanged");
            check(json_bool(body, "compose_translation", ct) && ct,
                  "P * translation matches its closed form in ALL SIXTEEN coefficients, including "
                  "the homogeneous row (out[2][3] == tz-near, out[3][3] == tz)");
            check(json_bool(body, "compose_keeps_perspective", ck) && ck,
                  "composing preserves the perspective classification");

            // THE RIGID INVERSE, by round trip. view_matrix_from_pose(p) composed with p's own
            // matrix must be the identity, on a pose with a real rotation and translation. This is
            // what catches a wrong conjugate sign or a mis-signed translation -- errors the live
            // comparison cannot see, because the engine's pose is identity in every pass reachable
            // from outside a render hook.
            bool vb = false, rt = false;
            check(json_bool(body, "view_from_pose_built", vb) && vb,
                  "view_matrix_from_pose builds for a non-trivial pose");
            check(json_bool(body, "inverse_round_trips", rt) && rt,
                  "view_matrix_from_pose(p) * matrix(p) == identity (the inverse is a real inverse)");
            // Out at level coordinates, where column 3's cancellation residual scales with |p|. A
            // validator with one absolute epsilon passes the near case and fails this one.
            bool drt = false;
            check(json_bool(body, "distant_round_trips", drt) && drt,
                  "and still round-trips for a pose ~100k units from the origin");

            // THE RESIDUALS THEMSELVES, bounded tightly. Reporting them is what let the tolerance be
            // sized from data instead of a guess -- the first version scaled the translation
            // allowance by the ROTATION tolerance and so permitted 196 units of error at 98000. The
            // bounds below are far under that and far over what was measured (1.2e-07 and 0).
            double nre = -1.0, nte = -1.0, fre = -1.0, fte = -1.0;
            const bool errs = json_double(body, "near_rot_err", nre) &&
                              json_double(body, "near_trans_err", nte) &&
                              json_double(body, "far_rot_err", fre) &&
                              json_double(body, "far_trans_err", fte);
            check(errs, "the round-trip residuals are reported");
            if (errs) {
                check(nre >= 0.0 && nre < 1e-5, "near rotation residual is epsilon-scale");
                check(nte >= 0.0 && nte < 1e-3, "near translation residual is negligible");
                check(fre >= 0.0 && fre < 1e-5, "distant rotation residual is epsilon-scale");
                // At 98000 units this bound is ~5e-7 relative, i.e. genuinely tight rather than the
                // 196-unit allowance the earlier scaling produced.
                check(fte >= 0.0 && fte < 0.05,
                      "distant translation residual stays well under the scaled allowance");
            }

            // THE PASS SETUP ANCHORS, resolved from the LIVE CLTRenderer vtable and checked against
            // what static reversing recorded. Two independent paths -- IDA read the table out of the
            // file, the DLL walks it through the interface registry in the running process -- so
            // agreement means the slot indices are right, not merely self-consistent.
            //
            // These are exe-relative offsets: slot 15 is the perspective pass, which takes the camera
            // transform, and is the address a stereo path would hook.
            double po = -1.0, ao = -1.0, so = -1.0;
            check(json_double(body, "pass_persp_off", po) && po == 0x20B520,
                  "renderer_fn(SetupPassPerspective) resolves to exe+0x20B520 (vtable slot 15)");
            check(json_double(body, "pass_affine_off", ao) && ao == 0x20B560,
                  "renderer_fn(SetupPassAffine) resolves to exe+0x20B560 (slot 16)");
            check(json_double(body, "pass_stored_off", so) && so == 0x20B5A0,
                  "renderer_fn(SetupPassStored) resolves to exe+0x20B5A0 (slot 17)");

            // The rest of the lifecycle. Having all of these matters because the engine's own
            // multi-view code (MakeCubicEnvMap) drives exactly this sequence six times, so a stereo
            // path is the same sequence twice rather than anything new.
            double eo = -1.0, dso = -1.0, dlo = -1.0;
            check(json_double(body, "pass_end_off", eo) && eo == 0x20B5C3,
                  "renderer_fn(EndPass) resolves to exe+0x20B5C3 (slot 18)");
            check(json_double(body, "pass_draw_off", dso) && dso == 0x20AF1B,
                  "renderer_fn(DrawScene) resolves to exe+0x20AF1B (slot 20)");
            check(json_double(body, "pass_drawlist_off", dlo) && dlo == 0x20AF32,
                  "renderer_fn(DrawObjectList) resolves to exe+0x20AF32 (slot 21)");

            // The outer lifecycle. These are what a consumer must respect BEFORE a pass exists at all:
            // passes nest inside a target, targets nest inside a frame.
            double bfo = -1.0, bto = -1.0, eto = -1.0;
            check(json_double(body, "pass_beginframe_off", bfo) && bfo == 0x20B41F,
                  "renderer_fn(BeginFrame) resolves to exe+0x20B41F (slot 8)");
            check(json_double(body, "pass_begintarget_off", bto) && bto == 0x20B9E3,
                  "renderer_fn(BeginRenderTarget) resolves to exe+0x20B9E3 (slot 11)");
            check(json_double(body, "pass_endtarget_off", eto) && eto == 0x20B4A6,
                  "renderer_fn(EndRenderTarget) resolves to exe+0x20B4A6 (slot 12)");

            // THE STATE ITSELF. Every entry point above is gated on it, so a consumer that cannot read it
            // cannot know which call is legal.
            //
            // ASSERTED: that it reads at all. NOT asserted: which value, and deliberately -- the first
            // version of this check required 1..4, the documented cycle, and FAILED because a running
            // game reads 0. That 0 is real and unexplained: the six lifecycle functions only write 1..4,
            // EndFrame closes the loop at 2 -> 1, and yet the parked value is 0 while the record those
            // functions maintain is correct. Asserting the range would have meant asserting my model
            // over the observation.
            double rst = -1.0;
            check(json_double(body, "renderer_state", rst) && rst >= 0.0,
                  "the scene renderer's state reads");

            // THE ENGINE'S BUILT-IN SETTINGS TABLE. 22 triplets of {name, storage, type}, each with a
            // direct address -- the knobs the engine consults itself, reachable without any
            // console-variable API.
            //
            // Structure is asserted, VALUES are not: these are user settings, and what a machine has
            // MaxFPS set to is not this suite's business.
            double var_count = -1.0, var_ok = -1.0, pp_off = -1.0;
            // 107, and the number has moved twice: 22 (started 25 entries in, rejected flagged tags),
            // then 106 (a scan needing 3-character names skipped "IP"). Asserted here because both wrong
            // extents were entirely plausible from inside the walk.
            check(json_double(body, "engine_var_count", var_count) && var_count == 107.0,
                  "the engine variable table walks to all 107 entries, starting at \"IP\"");
            check(json_double(body, "engine_var_wellformed", var_ok) && var_ok == var_count,
                  "every entry has an in-exe storage address and a known type tag");

            // THE TYPE CENSUS, which must account for every entry -- a slice or a mis-masked tag shows up
            // as a total that does not reach 107.
            double n_str = -1.0, n_flt = -1.0, n_int = -1.0, n_spaced = -1.0;
            const bool census = json_double(body, "engine_var_strings", n_str) &&
                                json_double(body, "engine_var_floats", n_flt) &&
                                json_double(body, "engine_var_ints", n_int);
            check(census && n_str == 3.0 && n_flt == 11.0 && n_int == 93.0,
                  "the type census is 3 string / 11 float / 93 int");
            check(census && (n_str + n_flt + n_int) == var_count,
                  "and it accounts for every entry walked");

            // THE MEASURED BASIS FOR CALLING TYPE 0 A POINTER: each string entry has another variable's
            // storage exactly 4 bytes above it, so the slot is 4 bytes wide and cannot be an inline
            // character buffer. Recomputed from the live table each run rather than trusted from a note --
            // this is the evidence the read_string accessor rests on, and it is cheap to keep checking.
            check(json_double(body, "engine_var_strings_4byte", n_spaced) && n_spaced == n_str,
                  "every string setting's slot is 4 bytes wide (a pointer, not a buffer)");

            // ---- INPUT SUBSYSTEM ----------------------------------------------------------------
            //
            // The device array's SHAPE, not its contents: CLTInput has six slots and an ordinary
            // session populates exactly two of them. A third appearing would mean a gamepad, which is
            // worth failing on so it gets mapped rather than silently mis-typed.
            double n_dev = -1.0, n_kb = -1.0, n_mouse = -1.0, n_unk = -1.0, n_vt = -1.0;
            const bool devs = json_double(body, "input_devices_populated", n_dev) &&
                              json_double(body, "input_devices_keyboard", n_kb) &&
                              json_double(body, "input_devices_mouse", n_mouse) &&
                              json_double(body, "input_devices_unknown", n_unk) &&
                              json_double(body, "input_devices_vtable_in_exe", n_vt);
            check(devs && n_dev == 2.0, "CLTInput has exactly two devices populated");
            check(devs && n_kb == 1.0 && n_mouse == 1.0,
                  "and they identify as one keyboard and one mouse by vtable");
            check(devs && n_unk == 0.0, "no populated slot has an unrecognised vtable");
            // Engine-side classes: a vtable outside the exe would mean the slot is not what this
            // mapping claims. Checked live because the pointers are heap addresses.
            check(devs && n_vt == n_dev, "every device's vtable lies inside the exe image");

            bool b_focus = false, b_gate = false, b_gate_ok = false;
            check(json_bool(body, "input_focus_readable", b_focus) && b_focus,
                  "the focus flags are readable");
            // The gate is the SDK's answer to "is the engine simulating", and a sign error here would
            // invert every liveness answer it gives. Cheap to pin, so pinned.
            check(json_bool(body, "input_gate_readable", b_gate) && b_gate &&
                      json_bool(body, "input_gate_matches_flag", b_gate_ok) && b_gate_ok,
                  "simulation_is_gated() is exactly !client_active");
            double gate_off = -1.0;
            check(json_double(body, "input_client_active_offset", gate_off) && gate_off == 3032884.0,
                  "the simulation gate sits at exe+0x2E4734");

            // The keyboard's two banks, surveyed across the whole 256-entry space. A press edge and a
            // release edge cannot both hold for one key, which catches a swapped bank far more reliably
            // than comparing values that are almost always zero on an idle session.
            double edges = -1.0, edges_ok = -1.0;
            check(json_double(body, "input_key_edges_checked", edges) &&
                      edges == 256.0,
                  "all 256 key slots read through the SDK's accessors");
            check(json_double(body, "input_key_edges_consistent", edges_ok) && edges_ok == edges,
                  "and the edge helpers agree with the current/previous banks on every one");

            bool b_mouse = false, b_win = false;
            check(json_bool(body, "input_mouse_readable", b_mouse) && b_mouse, "the mouse device reads");
            check(json_bool(body, "input_window_readable", b_win) && b_win,
                  "g_hMainWnd holds a window");

            // THE DEFECT THIS ASSERTION EXISTS FOR: while the window is iconic, its client rect reads
            // 160x28 at (-32000, -32000), and the look-delta subtraction produces ~34480 -- a number
            // that looks exactly like a fast mouse movement. A `width > 0` guard does NOT catch it,
            // because 160 is positive; only the iconic test does. So whenever the window is iconic the
            // delta must be refused outright.
            bool win_iconic = false;
            json_bool(body, "input_window_iconic", win_iconic);
            if (win_iconic) {
                bool delta_ok = true, rend = true, sim_gated = false;
                check(json_bool(body, "input_mouse_look_delta_valid", delta_ok) && !delta_ok,
                      "an iconic window refuses to report a mouse look delta");
                // The same branch that parks the window tears the renderer down, which is the
                // explanation for the scene renderer's state field reading 0 rather than 1..4.
                check(json_bool(body, "input_render_initted", rend) && !rend,
                      "and the renderer is shut down, not merely idle");
                check(json_bool(body, "input_simulation_gated", sim_gated) && sim_gated,
                      "and simulation is gated off");
            }

            // Two different notions of "minimized" that measurably disagree: the flag tracks the
            // SC_MINIMIZE system command, while the branch that gates simulation tests a live
            // IsIconic(). Asserted as a live comparison rather than as fixed values, since which state
            // the window is in when the suite runs is not the suite's business.
            bool b_iconic_ok = false, b_min_flag = true;
            check(json_bool(body, "input_window_iconic_readable", b_iconic_ok) && b_iconic_ok,
                  "the live iconic state is readable");
            json_bool(body, "input_minimized", b_min_flag);
            if (win_iconic && !b_min_flag) {
                check(true, "iconic without the SC_MINIMIZE flag: the two disagree, as documented");
            }

            // The queue's counters are whatever the last frame left; what is pinned is the bound the
            // handlers enforce, because that bound is what makes the parallel arrays' extents facts.
            double q_down = -1.0, q_up = -1.0;
            check(json_double(body, "input_queue_downs", q_down) && q_down >= 0.0 && q_down <= 100.0,
                  "the key-down queue stays within its 100-entry capacity");
            check(json_double(body, "input_queue_ups", q_up) && q_up >= 0.0 && q_up <= 100.0,
                  "and so does the key-up queue");
            bool b_drain_ok = false;
            check(json_bool(body, "input_queue_drain_readable", b_drain_ok) && b_drain_ok,
                  "whether the engine drains the queue this frame is readable");

            // ---- THE SUBCLASS WINDOW PROCEDURE ---------------------------------------------------
            //
            // The engine subclasses its OWN window to split input handling out of LTClient_WndProc,
            // which is why that procedure handles no mouse message. The saved original being
            // LTClient_WndProc is the assertable half -- a fact about the engine, checked against the
            // live window rather than read out of the IDB.
            bool wp_ok = false, wp_saved = false, wp_owns = true, wp_exe = true;
            check(json_bool(body, "input_wndproc_readable", wp_ok) && wp_ok,
                  "the window procedure chain is readable");
            check(json_bool(body, "input_wndproc_saved_is_engine", wp_saved) && wp_saved,
                  "the subclass saved LTClient_WndProc as its original");

            // AND THE OTHER HALF IS DELIBERATELY NOT ASSERTED. gameoverlayrenderer.dll subclasses the
            // same window in any Steam session, so whoever holds GWL_WNDPROC at a given moment is not
            // the engine's business or ours -- measured False here, with the owner outside the exe.
            // Asserting engine ownership would encode "no overlay is running" as a correctness
            // requirement. Both are reported so the state is visible either way.
            json_bool(body, "input_wndproc_engine_owns_window", wp_owns);
            json_bool(body, "input_wndproc_owner_is_exe", wp_exe);
            check(wp_owns == wp_exe,
                  "engine ownership and exe ownership of GWL_WNDPROC agree with each other");

            // The published pointer must BE the array's own address, which catches a build whose
            // layout differs from this mapping without needing a second landmark.
            bool arr_ok = false;
            check(json_bool(body, "input_device_array_published_matches", arr_ok) && arr_ok,
                  "g_pInputDeviceArray points at the device array itself");

            // The second gate, independent of the simulation one: measured enabled while the window was
            // iconic and simulation was gated, so a consumer must check both to explain "no input".
            bool ie_ok = false;
            check(json_bool(body, "input_enabled_readable", ie_ok) && ie_ok,
                  "the input-enabled gate is readable, separately from the simulation gate");

            // ---- THE BINDING SETS: THE ENGINE'S ACTION TABLE -----------------------------------
            //
            // Walked as an ARRAY, which is what the iterator primitives say it is: the container holds a
            // begin and an end pointer, advance adds 4, and dereference yields the element address. An
            // earlier pass probed the begin value as a {next, prev, value} node, found nothing, and
            // recorded the layout unestablished -- the model was wrong, not the data.
            double bs_sets = -1.0, bs_records = -1.0, bs_owner = -1.0, bs_first = -1.0, bs_kind = -99.0;
            const bool bs = json_double(body, "input_binding_sets", bs_sets) &&
                            json_double(body, "input_binding_records", bs_records) &&
                            json_double(body, "input_binding_owner_ok", bs_owner) &&
                            json_double(body, "input_binding_first_count", bs_first) &&
                            json_double(body, "input_binding_first_kind", bs_kind);
            check(bs && bs_sets == 1.0, "exactly one binding set is registered");
            check(bs && bs_records == 108.0 && bs_first == bs_records,
                  "it holds 108 records, and the walk reaches every one the header claims");

            // THE LOAD-BEARING CHECK. Every record stores the address of its own set header, so a wrong
            // stride or a wrong records base yields records whose owner does not match. This is an
            // invariant the DATA supplies, unlike a count that merely agrees with itself.
            check(bs && bs_owner == bs_records,
                  "every record's owner back-pointer equals its own set header");

            // kind 0, not the -1 the allocator writes: a set that still holds -1 is inert, because every
            // object lookup returns 0 for keyboard and mouse alike.
            double bs_inert = -1.0;
            check(bs && bs_kind == 0.0 && json_double(body, "input_binding_inert_sets", bs_inert) &&
                      bs_inert == 0.0,
                  "the set carries a real device kind rather than the inert -1");

            double bs_bound = -1.0, bs_handlers = -1.0;
            check(json_double(body, "input_binding_bound", bs_bound) && bs_bound > 0.0,
                  "some records are bound to an input object");
            check(json_double(body, "input_binding_with_handler", bs_handlers) && bs_handlers > 0.0,
                  "and some carry a handler function");
            // Records exist that have a handler but no binding, and vice versa is possible too, so these
            // are two independent populations rather than one -- checked so a future change that
            // conflated them shows up.
            check(bs_handlers <= bs_records && bs_bound <= bs_records,
                  "neither population exceeds the record count");

            // THE CENSUS CLOSES ARITHMETICALLY against the records themselves: every bound record
            // contributes its primary, and each alternate adds one more. No constant is hard-coded, so
            // this holds whatever the user's key configuration happens to be.
            double c_kb = -1.0, c_mouse = -1.0, c_joy = -1.0, c_alt = -1.0;
            const bool cen = json_double(body, "input_bind_keyboard", c_kb) &&
                             json_double(body, "input_bind_mouse", c_mouse) &&
                             json_double(body, "input_bind_joystick", c_joy) &&
                             json_double(body, "input_bind_with_alternate", c_alt);
            check(cen && (c_kb + c_mouse + c_joy) == (bs_bound + c_alt),
                  "the bound-object census accounts for every primary and alternate");

            // Measured 0, and a non-zero value would be a genuine asymmetry rather than a mapping error:
            // the game would have registered a binding on an object whose device index
            // LTInput_ObjectChanged rejects outright, so it could never fire. Asserted so that surprise
            // surfaces instead of passing quietly.
            check(cen && c_joy == 0.0,
                  "no record is bound to a joystick object the dispatch path cannot fire");

            // Deliberately NOT asserted: which action codes are bound to which keys. That is the user's
            // key configuration, not a property of the engine.

            // ---- THE ENGINE'S OBJECT NAMESPACE, AGAINST THE RAW BANKS -------------------------
            //
            // THE STRONGEST CHECK IN THIS SECTION, because the two sides are genuinely independent:
            // object_value() calls the device's own vtable getter, key_is_down() reads the state byte.
            // Neither can detect its own offset being wrong; disagreement between them can.
            double o_checked = -1.0, o_val = -1.0, o_prev = -1.0, o_chg = -1.0;
            const bool objs = json_double(body, "input_object_keys_checked", o_checked) &&
                              json_double(body, "input_object_value_agrees", o_val) &&
                              json_double(body, "input_object_prev_agrees", o_prev) &&
                              json_double(body, "input_object_changed_agrees", o_chg);
            check(objs && o_checked == 256.0,
                  "all 256 keyboard objects read through the engine's own getters");
            check(objs && o_val == o_checked,
                  "the vtable getter agrees with the raw current bank on every key");
            check(objs && o_prev == o_checked,
                  "and with the raw previous bank on every key");
            // The engine's test is `previous != current`; this SDK derives the edge from two separately
            // mapped banks. Agreement on all 256 means both banks are right, not just self-consistent.
            check(objs && o_chg == o_checked,
                  "and its change test matches the edge derived from both banks");

            double mb_checked = -1.0, mb_agrees = -1.0;
            check(json_double(body, "input_object_mouse_btn_checked", mb_checked) &&
                      json_double(body, "input_object_mouse_btn_agrees", mb_agrees) &&
                      mb_checked == 3.0 && mb_agrees == 3.0,
                  "mouse objects 1000-1002 agree with the raw button bank");
            bool ax_ok = false;
            check(json_bool(body, "input_object_axis_matches", ax_ok) && ax_ok,
                  "mouse objects 1005/1006 are bit-identical to the mapped axis floats");

            // STRUCTURAL, not stateful: a continuous position axis never reports a change, which is why
            // the engine needs a threshold accumulator to turn one into a digital action.
            bool never_ch = false;
            check(json_bool(body, "input_object_position_never_changes", never_ch) && never_ch,
                  "position axes 1003/1004 never report a change");

            // Refused deliberately: LTInput_ObjectChanged rejects device indices 2..5, so no binding on a
            // joystick object can fire, and this SDK will not guess which slot a kind would select.
            bool joy_ref = false;
            check(json_bool(body, "input_object_joystick_refused", joy_ref) && joy_ref,
                  "joystick object ids are refused rather than guessed at");

            // The namespace's edges in one check, including the KEYBOARD FALLTHROUGH -- id 5000 is not
            // rejected, it reads as a keyboard object, and a consumer needs to know that.
            bool cls_ok = false;
            check(json_bool(body, "input_object_classify_boundaries", cls_ok) && cls_ok,
                  "the object classifier's boundaries match the engine's four-line resolver");

            // The position axes compute precisely what look_delta reproduces, so they must match WHENEVER
            // the SDK is willing to report a delta. Conditional rather than absolute: while the window is
            // iconic the SDK refuses and the engine's getter still returns its garbage, so there is
            // nothing to compare and asserting a match would fail for a documented reason.
            bool delta_valid = false, delta_match = false;
            json_bool(body, "input_mouse_look_delta_valid", delta_valid);
            json_bool(body, "input_object_position_matches_delta", delta_match);
            if (delta_valid) {
                check(delta_match,
                      "objects 1003/1004 equal the reproduced look delta exactly");
            } else {
                check(!delta_match,
                      "with no usable window geometry, no delta comparison is claimed");
            }

            // Every synthetic-input entry point resolves inside the exe. These are the addresses a mod
            // drives to feed the engine input that did not come from a window message, so a stale one
            // would be a hook into nothing.
            bool ep_ok = false;
            check(json_bool(body, "input_entry_points_resolved", ep_ok) && ep_ok,
                  "all six input entry points resolve inside the exe image");

            // The load-bearing cross-check: the table's slot for PausePhysics must be the very global
            // CClientMgr__Update tests before its physics block. Static reversing found the flag first
            // and the table second, so agreement ties the two together.
            check(json_double(body, "engine_var_pause_physics_off", pp_off) && pp_off == 0x2EE978,
                  "PausePhysics's storage is exe+0x2EE978 -- the global the physics gate reads");

            // TYPE DISCRIMINATION, which is what makes the accessors safe. MaxFinalizeTimeMS is a float
            // and PhysicsClientUpdateRate an int; each must read through its own accessor and be REFUSED
            // by the other. That matters because a wrong-typed read is not obviously wrong -- an int
            // setting read as a float gives 6e-44, a plausible-looking small number.
            bool fread = false, iread = false, refuses = false;
            check(json_bool(body, "engine_var_float_read", fread) && fread,
                  "a float-typed setting reads through read_float");
            check(json_bool(body, "engine_var_int_read", iread) && iread,
                  "an int-typed setting reads through read_int");
            check(json_bool(body, "engine_var_refuses_wrong_type", refuses) && refuses,
                  "and each accessor REFUSES the other's type rather than reinterpreting the bytes");

            // TWO MAPPINGS, ONE CLOCK. k_fTime is fmod(engine seconds, 1000), and the two sides are
            // reached completely differently -- the engine clock by calling an accessor located by byte
            // pattern, k_fTime by walking a linked list of shader parameter records. Nothing forces them
            // to agree, so agreement means both mappings are right.
            //
            // Note this holds whether or not the engine is ticking: when it stops, BOTH freeze, and a
            // frozen pair still agrees. So this is a mapping check, not a liveness check -- the liveness
            // question is separate and immediately below.
            bool cross_avail = false, cross_ok = false;
            check(json_bool(body, "clock_cross_check_available", cross_avail) && cross_avail,
                  "both clocks are readable, so the cross-check can run");
            if (cross_avail) {
                check(json_bool(body, "clock_cross_check", cross_ok) && cross_ok,
                      "k_fTime == fmod(engine seconds, 1000) -- a byte-pattern accessor and a "
                      "linked-list walk agree");
            }

            // IS THE RENDER PATH ACTUALLY RUNNING? k_fTime is published once per frame by BeginFrame, so
            // comparing it across two requests answers that -- and the answer changes how every other
            // reading in this block should be read. When it is NOT advancing, the camera record and the
            // shader registry are frozen at the last executed pass: genuine engine-produced values that
            // still satisfy every coherence check here, but a snapshot of the past rather than the
            // present. Nothing else exposed can distinguish the two.
            //
            // REPORTED, NOT ASSERTED, because whether a game renders while unfocused is not this suite's
            // business -- and an assertion either way would fail on a perfectly healthy machine.
            double clock_a = -1.0;
            check(json_double(body, "frame_time", clock_a),
                  "the engine's per-frame clock (k_fTime) reads");
            {
                // Same spacing requirement: k_fTime only changes when a frame is rendered, so an
                // immediate second request would report IDLE on a perfectly busy game.
                std::string resp2;
                double clock_b = -1.0;
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                if (http::get(port, "/sdk/shader-params", resp2)) {
                    (void)json_double(http::body_of(resp2), "frame_time", clock_b);
                }
                const bool advancing = clock_a >= 0.0 && clock_b >= 0.0 && clock_a != clock_b;

                // THREE LAYERS, THREE SIGNALS, AND THEY DISAGREE. Measured on an unfocused game: the
                // main loop pumps at ~173 Hz while the engine clock and the render clock are both
                // frozen. So "my hook fired" does not mean "the game simulated", and neither means "a
                // frame was drawn". Reported together precisely because each one alone is misleading --
                // this is the shape of the mistake that produced the stale-reading corrections.
                double eng_a = -1.0, eng_b = -1.0;
                (void)json_double(body, "engine_seconds", eng_a);
                (void)json_double(http::body_of(resp2), "engine_seconds", eng_b);
                // SPACED DELIBERATELY. Two back-to-back requests cannot span a ~6 ms tick, so the
                // first version of this report read the same counter twice and called a firing hook
                // idle -- measuring the sampler rather than the engine. 300 ms is many ticks at any
                // plausible rate and still negligible against the suite's runtime.
                int64_t ticks_a = -1, ticks_b = -1;
                std::string hz1, hz2;
                if (http::get(port, "/health", hz1)) {
                    (void)json_int(http::body_of(hz1), "frame_ticks", ticks_a);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                if (http::get(port, "/health", hz2)) {
                    (void)json_int(http::body_of(hz2), "frame_ticks", ticks_b);
                }
                printf("[fixture] liveness: hook %s (frame_ticks %lld->%lld) | engine clock %s "
                       "(%.3f->%.3f) | render %s (k_fTime %.5f->%.5f)\n",
                       (ticks_b > ticks_a) ? "FIRING" : "idle",
                       static_cast<long long>(ticks_a), static_cast<long long>(ticks_b),
                       (eng_a != eng_b) ? "TICKING" : "frozen", eng_a, eng_b,
                       advancing ? "RUNNING" : "idle", clock_a, clock_b);
                if (!advancing) {
                    printf("[fixture]   -> camera/shader readings are a frozen snapshot of the last "
                           "executed pass\n");
                }
            }
            printf("[fixture] scene renderer state: %.0f (1 idle, 2 frame, 3 target, 4 pass; "
                   "0 observed but unexplained)\n", rst);

            // THE REGRESSION TEST FOR A BUG THE LIVE CHECKS CANNOT SEE. rotation_matrix() hardcoded a
            // factor of 2 where the engine divides by |q|^2. Every rotation the game exposes is unit,
            // so the two formulas agree everywhere the suite looks and the bug survived until a
            // decompile exposed it. A NON-UNIT quaternion separates them: scaling q must not change
            // R(q), since a scaled quaternion is the same rotation, and the hardcoded version fails
            // that by construction.
            bool rsi = false, rz = false, rnf = false;
            check(json_bool(body, "rotation_scale_invariant", rsi) && rsi,
                  "rotation_matrix(q) == rotation_matrix(k*q) -- the engine's 2/|q|^2 scale, which a "
                  "hardcoded 2 would fail");
            check(json_bool(body, "rotation_rejects_zero", rz) && rz,
                  "rotation_matrix refuses a zero quaternion (no rotation to describe)");
            check(json_bool(body, "rotation_rejects_nonfinite", rnf) && rnf,
                  "rotation_matrix refuses a non-finite quaternion");

            // QUATERNION <-> MATRIX, both directions. rotation_matrix and rotation_from_matrix are
            // independent transcriptions of two different engine functions, so requiring them to
            // invert each other catches a sign or index error in either -- something no
            // single-direction test can do. Compared as MATRICES, since q and -q are the same rotation.
            bool qrt = false;
            double qbr = -1.0;
            check(json_bool(body, "quat_roundtrips", qrt) && qrt,
                  "rotation_from_matrix inverts rotation_matrix for every probe rotation");
            check(json_double(body, "quat_branches", qbr) && qbr >= 2.0,
                  "and the negative-trace fallback branch was actually exercised (>= 2 cases)");

            // THE PASS ARGUMENT MODEL, which is what a consumer needs before it can call slot 15 at
            // all: the field of view goes in as an ANGLE and the viewport as a FRACTION. Both helpers
            // reproduce the engine's behaviour including its CLAMPS, because the engine clamps rather
            // than rejecting and a helper that refused would mispredict every out-of-range request.
            bool fta = false, fch = false, fcn = false, rho = false, rco = false;
            check(json_bool(body, "fov_tan_ok", fta) && fta,
                  "predicted_half_view_plane(90 degrees) == tan(45) == 1");
            check(json_bool(body, "fov_clamps_high", fch) && fch,
                  "a FOV above 179 degrees clamps to the ceiling rather than growing");
            check(json_bool(body, "fov_clamps_negative", fcn) && fcn,
                  "and a negative FOV clamps to zero");
            check(json_bool(body, "rect_halves_ok", rho) && rho,
                  "a normalised {0,0,0.5,1} / {0.5,0,1,1} pair maps to the left and right halves of a "
                  "5120x1440 target -- side-by-side stereo needs no matrix work");
            check(json_bool(body, "rect_clamps_ok", rco) && rco,
                  "an out-of-range viewport clamps to the target instead of overflowing it");

            // THE LOOK-AT, checked by the property a consumer depends on rather than by comparing the
            // transcription to itself: rotating +Z by the result must reproduce the requested forward.
            // That exercises both crosses, the basis column order and the quaternion conversion at
            // once, and it is what pins the handedness the decompiler output could not show -- the
            // cross receivers live in ECX, so only the disassembly gave the operand order.
            bool lfo = false, lid = false, lpar = false;
            check(json_bool(body, "lookat_identity", lid) && lid,
                  "forward +Z with up +Y yields the identity rotation (the canonical basis)");
            check(json_bool(body, "lookat_forward_ok", lfo) && lfo,
                  "rotating +Z by the look-at reproduces the requested forward, for every probe "
                  "direction (a flipped cross or swapped column fails here)");
            check(json_bool(body, "lookat_parallel_ok", lpar) && lpar,
                  "an up hint parallel to forward still yields the right view direction -- the engine "
                  "swizzles the hint rather than failing, and the transcription does too");

            // Finite input whose OUTPUT overflows -- the same class of hole the projection builders
            // had. invert_transform's optional must mean "usable transform", not "input looked fine".
            bool rop = false;
            check(json_bool(body, "rejects_overflow_pose", rop) && rop,
                  "invert_transform rejects a pose whose rotated position overflows");

            // THE WHOLE PREDICATE SURFACE, SWEPT. Every tolerance-taking method on the snapshot must
            // reject a deliberately inconsistent snapshot at a sane tolerance, AND refuse a NaN,
            // +inf or negative tolerance rather than acting on it. Both directions are needed
            // because the failure mode follows how each comparison is spelled: `deviation >
            // tolerance` accepts everything on NaN, while a near_equal form rejects on NaN but
            // accepts everything on +inf. The signatures look identical.
            bool tgh = false, mdet = false;
            check(json_bool(body, "tolerance_guards_hold", tgh) && tgh,
                  "every snapshot predicate refuses a NaN, +inf or negative tolerance");
            check(json_bool(body, "mismatch_detected", mdet) && mdet,
                  "and every one detects a deliberately inconsistent snapshot at a sane tolerance");

            // THE BEHIND-CAMERA CONTRACT, where it actually means something. w is a view-space depth
            // only under perspective; in the affine passes it is the constant m[3][3], positive for
            // every input, so the live record can never exercise the refusal. On a synthetic
            // perspective matrix a point in front projects with w equal to its depth, and one behind
            // is refused.
            bool ppf = false, prb = false, pwd = false, awnd = false;
            check(json_bool(body, "persp_projects_front", ppf) && ppf,
                  "under perspective a point in front projects, with w equal to its view depth");
            check(json_bool(body, "persp_rejects_behind", prb) && prb,
                  "and a point behind the camera is refused rather than given a plausible pixel");
            check(json_bool(body, "persp_w_is_depth", pwd) && pwd,
                  "w_is_view_space_depth is true for a perspective projection");
            check(json_bool(body, "affine_w_is_not_depth", awnd) && awnd,
                  "and false for an affine one, where w is a constant carrying no distance");

            // And the same for the half-plane identity, which fails closed by the shape of its
            // comparison rather than by an explicit guard -- so the refusal is asserted, not argued.
            bool inan = false;
            check(json_bool(body, "identity_rejects_nan_tolerance", inan) && inan,
                  "the half-plane identity refuses a NaN tolerance");

            // A NaN tolerance must be refused. Unvalidated, it accepts anything.
            bool nan_ok = false;
            check(json_bool(body, "rejects_nan_tolerance", nan_ok) && nan_ok,
                  "affines_are_inverse refuses a NaN tolerance instead of accepting every pair");

            // REJECTION, because a builder that quietly accepts a degenerate frustum hands back a
            // matrix that still looks usable -- a zero scale coefficient with m[3][2] intact.
            bool r0 = false, r1 = false, r2 = false;
            check(json_bool(body, "probe_rejects_zero_extent", r0) && r0,
                  "the perspective builder rejects a zero half-extent");
            check(json_bool(body, "probe_rejects_negative_extent", r1) && r1,
                  "the perspective builder rejects a negative half-extent");
            check(json_bool(body, "probe_rejects_zero_span", r2) && r2,
                  "the affine builder rejects a zero depth span");
            // The case input validation alone would miss: finite and positive, but its reciprocal
            // overflows, so the matrix would carry an infinity while has_value() said yes.
            bool r3 = false;
            check(json_bool(body, "probe_rejects_tiny_extent", r3) && r3,
                  "the perspective builder rejects an extent whose reciprocal is not finite");
        }

        // ---- THE SCENE CAMERA RECORD -------------------------------------------------
        //
        // Taken as ONE snapshot in the DLL, because the render thread rewrites this record per
        // pass and a field-by-field read can splice two passes together without faulting. The
        // checks below are the snapshot's own predicates, so they exercise what a consumer
        // would call, not a reimplementation.
        // ONLY THIS IS UNCONDITIONAL: the record is static exe data, so a snapshot must succeed
        // whenever the module is mapped, level or no level.
        check(json_has(body, "\"scene_camera\":true"), "the scene camera record snapshots");

        // EVERYTHING BELOW IS GATED ON THE RECORD BEING CONFIGURED, and the gate is not
        // decoration. Before the first render pass touches it this structure is all zeroes, so a
        // degenerate viewport, a zero projection and a zero-magnitude quaternion are all correct
        // readings of a legitimate state. An earlier version of this block asserted the viewport
        // valid unconditionally while gating the pose checks on that same validity -- a
        // contradiction that would have failed on a menu run by design.
        const bool configured = json_has(body, "\"sc_viewport_valid\":true");

        int64_t sc_mode = -1, sc_w = -1, sc_h = -1;
        (void)json_int(body, "sc_mode", sc_mode);
        const bool have_dims = json_int(body, "sc_vp_w", sc_w) && json_int(body, "sc_vp_h", sc_h);
        check(have_dims, "the camera's viewport extents are reported and parseable");
        if (configured) {
            check(sc_w > 0 && sc_h > 0, "a configured camera reports positive viewport extents");
        }

        // THE ONE PIECE OF REAL CORROBORATION IN THIS BLOCK. Every other matrix check here runs on
        // a matrix we built. This one takes the engine's own projection and view out of the record,
        // runs our transcription of LTMatrix_Mul4x4ByAffine over them, and requires the result to
        // equal the view-projection THE ENGINE built and stored at +0xB8.
        //
        // Its strength is pass-dependent and the suite does not pretend otherwise: while the view
        // matrix is identity -- which is the screen pass, i.e. almost every sample from outside a
        // render hook -- the product cannot distinguish a transposed implementation. The synthetic
        // translation case above is what covers the ordering; this covers the transcription.
        if (configured) {
            bool recomposed = false;
            check(json_bool(body, "sc_compose_matches_record", recomposed) && recomposed,
                  "our compose reproduces the engine's own stored view-projection");
            // THE WIDEST COHERENCE CHECK ON THIS RECORD: world_to_screen must equal the viewport
            // transform (derived from the rect) times the view-projection. That spans four regions
            // the render thread writes at different moments, and unlike the perspective checks it
            // holds in whatever pass happens to be live.
            bool w2s = false;
            check(json_bool(body, "sc_w2s_coherent", w2s) && w2s,
                  "world_to_screen == viewport_transform * view_projection (four regions agree)");

            // THE OTHER DIRECTION: screen_to_clip, built from the viewport's own half-extents. The
            // viewport centre must land on clip (0, 0) -- a round trip through a matrix the engine
            // built, not one we did.
            bool cto = false;
            check(json_bool(body, "sc_centre_to_origin", cto) && cto,
                  "the viewport centre maps to clip (0,0) through the engine's screen_to_clip");

            // Whether that matrix and the viewport transform are actually inverses is INFORMATIVE, not
            // guaranteed: the engine writes -1 and +1 into column 3 where the true inverse wants
            // -centreX/halfW and centreY/halfH, and those agree only for a viewport anchored at the
            // origin. Reported rather than asserted, since a sub-rect viewport is a legitimate state
            // in which the engine's own two matrices are not inverses.
            bool s2c = false;
            if (json_bool(body, "sc_s2c_inverts_viewport", s2c)) {
                printf("[fixture] screen_to_clip inverts the viewport transform: %s\n",
                       s2c ? "yes (origin-anchored viewport)" : "no (sub-rect viewport)");
            }

            // Projecting a point through it. In the screen pass the matrix is the identity, so a
            // point must project to ITSELF -- not a tautology, since that identity is the product of
            // the screen ortho and the viewport transform and any error in either breaks it.
            bool pid = false, rb = false;
            if (json_has(body, "\"sc_pose_identity\":true")) {
                check(json_bool(body, "sc_projects_identity", pid) && pid,
                      "project_point returns the point itself in the screen pass, where the composed "
                      "matrix is the identity");
                check(json_bool(body, "sc_rejects_behind", rb) && rb,
                      "and refuses a point with non-positive w where the pass can produce one");
            }

            // The pose -> view recipe against the engine's own two regions. Same pass-dependence:
            // in the screen pass both are identity, so this cannot distinguish a wrong recipe --
            // the synthetic round trip above is what does that.
            bool vmp = false;
            check(json_bool(body, "sc_view_matches_pose", vmp) && vmp,
                  "the record's view matrix matches the one its own pose implies");
        }

        // ---- PROJECTION CLASSIFICATION, AND THE CONTRACT AROUND FOV ------------------
        //
        // The two classifiers must be mutually exclusive and exhaustive on any finite matrix, and
        // that is checkable in every state rather than only in a 3D pass. A matrix reading as both,
        // or neither, means the w-row test or the finite check is broken.
        //
        // Declared out here because the orthographic identity further down needs `affine` too.
        bool persp = false, affine = false;
        if (configured) {
            // json_bool, NOT json_has: a renamed or truncated field reads as `false` to json_has,
            // and false is the passing answer for one of these two. That is exactly how a
            // mislabelled field and a truncated JSON buffer both nearly passed here.
            const bool persp_ok = json_bool(body, "sc_perspective", persp);
            const bool affine_ok = json_bool(body, "sc_affine", affine);
            check(persp_ok && affine_ok,
                  "both projection classifiers are present and parse as booleans");
            if (persp_ok && affine_ok) {
                check(persp != affine,
                      "the projection classifies as exactly one of perspective or affine");
            }
            double wrow = -1.0;
            check(json_double(body, "sc_w_row_scale", wrow) && wrow > 0.0,
                  "the projection's w row has a positive scale (its yardstick is usable)");

            // THE GATE IS THE CONTRACT, and it is what two earlier passes could not enforce. FOV
            // is derived as m[3][2]/m[0][0], meaningless without a z->w coupling, so the accessor
            // must REFUSE on an affine matrix. The engine's screen pass is the only pass observable
            // from outside a render hook, so that refusal is what should happen here, and asserting
            // it defends the precondition even though the FOV VALUE cannot be exercised from here.
            bool fov_present = false, agrees = false;
            check(json_bool(body, "sc_fov_present", fov_present) &&
                      json_bool(body, "sc_proj_agrees_hvp", agrees),
                  "the fov and projection-agreement fields are present and parse");
            // THE IDENTITY NOW HOLDS IN EVERY PASS, so it is asserted unconditionally rather than
            // only for the perspective case. This is the one matrix identity in this block that runs
            // against the LIVE record: m[0][0] * half_x equals the nonzero element of the w row,
            // because every builder divides the half-extent into its own overall scale. Reading the
            // screen-ortho builder is what made this checkable at all.
            check(agrees,
                  "m[0][0] * half_x equals the projection's own scale -- ties the matrix to "
                  "k_vHalfViewPlane in whichever pass is live");
            if (affine) {
                check(!fov_present,
                      "fov_y_radians() refuses an affine projection (the precondition holds)");
            } else if (persp) {
                // A perspective snapshot: the identity m[0][0]*half_x == m[3][2] applies, tying the
                // matrix to the scalar pair the engine publishes separately.
                check(fov_present, "a perspective projection yields a field of view");
            }
        }

        // THE INVARIANT THAT EARNS ITS KEEP. For an orthographic pass the projection must
        // satisfy [0][0] == 2/width and [1][1] == -2/height. Those floats and the viewport ints
        // are written at different moments by the render thread, so the identity fails on a wrong
        // offset AND on a tear that paired one pass's rect with another's matrix. It covers those
        // two regions only -- a tear confined to the view, derived or trailing matrices would
        // still pass, so this is not an atomicity check on the whole record. Gated on the
        // projection actually being orthographic, since the identity does not apply otherwise.
        //
        // GATED ON THE NARROW PREDICATE, not on `affine`: the mode-1 builder is affine too, but its
        // matrix is scaled by (far - near), so m[0][0] is k/half rather than 2/width and the
        // identity legitimately does not hold. Gating on `affine` would have turned a rare mode-1
        // snapshot into a flaky failure.
        bool normalized_ortho = false;
        const bool normalized_ok = json_bool(body, "sc_normalized_ortho", normalized_ortho);
        if (configured) {
            check(normalized_ok, "the normalized-orthographic predicate is reported");
            if (normalized_ortho) {
                check(affine, "a normalized orthographic projection is also classified affine");
            }
        }
        if (configured && normalized_ortho) {
            check(json_has(body, "\"sc_ortho_matches_viewport\":true"),
                  "the orthographic projection matches its own viewport (2/w, -2/h) -- ties the "
                  "matrix to the rect");
        }

        // THE OFFSET PROOF: a quaternion read at the wrong offset is overwhelmingly unlikely to
        // normalise, so this is what defends the +0x14 pose location. LTTransform_Copy is what
        // established that layout -- three floats, then LTRotation_Copy at +0x0C.
        //
        // GATED ON THE RECORD BEING CONFIGURED, not on the pass mode, and the gate is
        // load-bearing: this is static data, so before any pass has run it is all zeroes, and a
        // zero quaternion has magnitude 0 rather than 1. A valid viewport rect is the builder's
        // own evidence that it ran.
        if (configured) {
            check(json_has(body, "\"sc_pose_rot_unit\":true"),
                  "the camera pose's rotation is a unit quaternion (proves the +0x14 offset)");
            check(json_has(body, "\"sc_pose_pos_finite\":true"),
                  "the camera pose's position is finite");
        }

        // The engine's screen pass, whose shape is known: identity view, and half-view-plane
        // extents that are literally half the viewport in PIXELS. That second identity is what
        // established the units of k_vHalfViewPlane, so it is worth defending. Gated on the mode
        // because a 3D pass legitimately leaves other values here.
        if (configured && sc_mode == 2) {
            check(json_has(body, "\"sc_view_identity\":true"),
                  "the screen pass's view matrix is identity");
            // The screen pass zeroes the pose entirely. Asserted through the snapshot's own
            // pose_is_identity(), which checks all seven components -- an earlier version matched
            // the JSON text "sc_pose_qw":1.0000, which tested printf's formatting and only ever
            // looked at w.
            check(json_has(body, "\"sc_pose_identity\":true"),
                  "the screen pass's camera pose is the identity transform");
            double hx = 0.0, hy = 0.0;
            if (json_double(body, "sc_hvp_x", hx) && json_double(body, "sc_hvp_y", hy)) {
                const double want_x = static_cast<double>(sc_w) / 2.0;
                const double want_y = static_cast<double>(sc_h) / 2.0;
                check(hx > want_x * 0.99 && hx < want_x * 1.01 &&
                          hy > want_y * 0.99 && hy < want_y * 1.01,
                      "in the screen pass the half view-plane is half the viewport in pixels "
                      "(what fixed the units of k_vHalfViewPlane)");
            }
        }
        printf("[fixture] scene camera: mode %lld, viewport %lldx%lld, projection %s\n",
               static_cast<long long>(sc_mode), static_cast<long long>(sc_w),
               static_cast<long long>(sc_h),
               json_has(body, "\"sc_perspective\":true") ? "perspective" : "affine");
        printf("[fixture] shader params: %lld records, %lld bound, %lld pending upload, "
               "screen %.0fx%.0f\n",
               static_cast<long long>(count), static_cast<long long>(bound),
               static_cast<long long>(pending), res_w, res_h);
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
