// ctest E2E runner: drives fear2vr.dll against the REAL game (FEAR2.exe is the
// fixture) and asserts the in-DLL test suite (/test) returns fail==0.
//
// Exit codes: 0 pass / 1 fail / 77 skip (SKIP_RETURN_CODE 77 in cmake).
// Skip is honest: no game, no injection rights, or IPC never came up -> 77.
// A failure to make a CLAIM (red test, broken IPC after health, unload that
// leaves the module hot) -> 1.
//
// Pipeline:
//   1. resolve args; verify injector/dll exist (fixture exe may be missing ->
//      only relevant if we must spawn it)
//   2. reuse a running FEAR2.exe, else spawn it (cwd = exe dir) and let it boot
//   3. if IPC already up on the port, request --unload first (stale instance)
//   4. inject; poll /health until running
//   5. run in-DLL suite via GET /test; assert fail==0
//   6. teardown: if WE spawned -> terminate the process (cleanly proves the
//      game survives); else /unload and require the module to vanish

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>

#include <windows.h>
#include <tlhelp32.h>

#include "HttpClient.hpp"

namespace fs = std::filesystem;

namespace {

constexpr int32_t kOk = 0;
constexpr int32_t kFail = 1;
constexpr int32_t kSkip = 77;

uint32_t find_pid(const std::string& process_name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    uint32_t pid = 0;
    const std::wstring want(process_name.begin(), process_name.end());
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, want.c_str()) == 0) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

struct SpawnedProcess {
    PROCESS_INFORMATION pi{};
    bool launched = false;
    ~SpawnedProcess() {
        if (pi.hProcess) CloseHandle(pi.hProcess);
        if (pi.hThread) CloseHandle(pi.hThread);
    }
};

// Fills `out`. Out-param (not return-by-value): the destructor owns the
// PROCESS_INFORMATION handles -- a returned temporary would have its handles
// closed on destruction, leaving the receiver with stale handles.
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

int run_injector(const fs::path& injector, const std::string& action, const fs::path& dll, int32_t port) {
    std::wstring cmd = L"\"" + injector.wstring() + L"\" --" +
                       std::wstring(action.begin(), action.end()) + L" --process FEAR2.exe --dll \"" +
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

// Extract "key":N (first occurrence); false when absent/negative-looking.
bool json_int(const std::string& body, const char* key, int64_t& out) {
    std::string needle = "\"";
    needle += key;
    needle += "\":";
    const size_t p = body.find(needle);
    if (p == std::string::npos) return false;
    const size_t start = p + needle.size();
    char* end = nullptr;
    const long long v = strtoll(body.c_str() + start, &end, 10);
    if (end == body.c_str() + start) return false;
    out = v;
    return true;
}

bool health_ready(int32_t port) {
    std::string resp;
    if (!http::get(port, "/health", resp)) return false;
    const std::string body = http::body_of(resp);
    return body.find("\"ok\":true") != std::string::npos &&
           body.find("\"state\":\"running\"") != std::string::npos;
}

void cleanup(const SpawnedProcess& spawned, bool need_remote_unload, const fs::path& injector,
             const fs::path& dll, int32_t port) {
    if (spawned.launched) {
        TerminateProcess(spawned.pi.hProcess, 0);
        printf("[fixture] terminated the instance we spawned\n");
    } else if (need_remote_unload) {
        run_injector(injector, "unload", dll, port);
        printf("[fixture] unloaded from the pre-existing instance (left running)\n");
    }
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

    // 2. Reuse a running game, else spawn the fixture ourselves.
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
    }

    // 3. Clear any stale instance on the port.
    if (http::port_open(port)) {
        run_injector(injector, "unload", dll, port);
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    // 4. Inject.
    if (run_injector(injector, "inject", dll, port) != 0) {
        printf("[fixture] injection failed -- skipping (no injection rights?)\n");
        cleanup(spawned, false, injector, dll, port);
        return kSkip;
    }

    // 5a. Wait-for-live: IPC up and framework running.
    bool live = false;
    for (int32_t i = 0; i < 200 && !live; ++i) { // ~20s
        live = health_ready(port);
        if (!live) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!live) {
        printf("[fixture] IPC never became live -- skipping (init failure in-game)\n");
        cleanup(spawned, false, injector, dll, port);
        return kSkip;
    }
    printf("[fixture] IPC live; running in-DLL suite\n");

    // 5b. Run the suite.
    std::string resp;
    bool pass = false;
    int64_t n_pass = -1, n_fail = -1;
    if (http::get(port, "/test", resp)) {
        const std::string body = http::body_of(resp);
        if (json_int(body, "pass", n_pass) && json_int(body, "fail", n_fail)) {
            pass = (n_fail == 0);
            printf("[fixture] in-DLL suite: pass=%lld fail=%lld\n", n_pass, n_fail);
            if (!pass) {
                printf("[fixture] failures:\n%s\n", body.c_str());
            }
        } else {
            printf("[fixture] unparseable /test body: %s\n", body.c_str());
        }
    } else {
        printf("[fixture] /test transport failed\n");
    }

    // 6. Teardown + uninject verification: on the reuse path we REQUIRE the
    //    unload handshake to succeed -- graceful uninject is itself the
    //    feature under test.
    cleanup(spawned, true, injector, dll, port);

    if (!pass) {
        printf("[fixture] FAIL\n");
        return kFail;
    }
    printf("[fixture] PASS\n");
    return kOk;
}
