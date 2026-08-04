// LoadLibrary-based injector for fear2vr.dll.
//
//   injector.exe --inject   --process FEAR2.exe [--dll path] [--port 8798]
//   injector.exe --unload   --process FEAR2.exe [--dll path] [--port 8798]
//   injector.exe --reload   --process FEAR2.exe [--dll path] [--port 8798]
//   injector.exe --status   --process FEAR2.exe [--dll path] [--port 8798]
//
// Defaults: DLL resolves next to injector.exe as fear2vr.dll; process FEAR2.exe.
//
// Unload does NOT FreeLibrary remotely: it asks the injected DLL to unhook and
// unmap itself (HTTP /unload), then polls the target's module list until our
// module is gone. Module presence is checked via the module list -- independent
// of the IPC server -- so "loaded" vs "IPC up" are reported separately.
//
// Reload paints the DLL under a fresh filename (fear2vr.<n>.dll) before
// injecting, so DllMain runs even if a prior instance stayed dormant-resident.

#include <winsock2.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

#include <shellapi.h>
#include <tlhelp32.h>

#include "HttpClient.hpp"

namespace fs = std::filesystem;

namespace {

struct Config {
    std::string process = "FEAR2.exe";
    fs::path dll;
    int32_t port = 8798;
};

// ---- process / module helpers --------------------------------------------

uint32_t find_pid(const std::string& process_name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    uint32_t pid = 0;
    std::wstring want(process_name.begin(), process_name.end());
    std::transform(want.begin(), want.end(), want.begin(), ::towlower);
    if (Process32FirstW(snap, &pe)) {
        do {
            std::wstring got = pe.szExeFile;
            std::transform(got.begin(), got.end(), got.begin(), ::towlower);
            if (got == want) {
                pid = pe.th32ProcessID;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pid;
}

// Is a module named `basename` (case-insensitive) loaded in `pid`?
bool module_loaded(uint32_t pid, const std::string& basename) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    std::wstring want(basename.begin(), basename.end());
    std::transform(want.begin(), want.end(), want.begin(), ::towlower);
    bool found = false;
    if (Module32FirstW(snap, &me)) {
        do {
            const std::wstring full = me.szModule;
            const size_t slash = full.find_last_of(L"\\/");
            std::wstring name = slash == std::wstring::npos ? full : full.substr(slash + 1);
            std::transform(name.begin(), name.end(), name.begin(), ::towlower);
            if (name == want) {
                found = true;
                break;
            }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return found;
}

// Any fear2vr*.dll resident (catches dormant reload leftovers)? Returns first
// basename found, empty if none.
std::string any_resident_instance(uint32_t pid) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return {};
    MODULEENTRY32W me{};
    me.dwSize = sizeof(me);
    std::string found;
    if (Module32FirstW(snap, &me)) {
        do {
            const std::wstring full = me.szModule;
            const size_t slash = full.find_last_of(L"\\/");
            std::wstring name = slash == std::wstring::npos ? full : full.substr(slash + 1);
            std::string narrow(name.begin(), name.end());
            std::transform(narrow.begin(), narrow.end(), narrow.begin(), ::tolower);
            if (narrow.rfind("fear2vr", 0) == 0 && narrow.size() >= 11 &&
                narrow.compare(narrow.size() - 4, 4, ".dll") == 0) {
                found = narrow;
                break;
            }
        } while (Module32NextW(snap, &me));
    }
    CloseHandle(snap);
    return found;
}

// ---- actions --------------------------------------------------------------

bool do_inject(const Config& cfg, bool allow_resident = false) {
    const uint32_t pid = find_pid(cfg.process);
    if (pid == 0) {
        printf("[injector] process %s not found\n", cfg.process.c_str());
        return false;
    }

    // A resident identical BASENAME means DllMain would not rerun (LoadLibrary
    // short-circuits same-name loads); a DIFFERENT basename (reload's fresh
    // name) is safe -- a dormant old instance is inert by design.
    const std::string resident = any_resident_instance(pid);
    if (!resident.empty() && !allow_resident) {
        printf("[injector] %s already resident; refusing double-inject (use --reload)\n", resident.c_str());
        return false;
    }

    const std::wstring dll_w = fs::absolute(cfg.dll).wstring();

    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION |
                                  PROCESS_VM_WRITE | PROCESS_VM_READ,
                              FALSE, pid);
    if (proc == nullptr) {
        printf("[injector] OpenProcess failed (%lu)\n", GetLastError());
        return false;
    }

    const size_t bytes = (dll_w.size() + 1) * sizeof(wchar_t);
    LPVOID remote = VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (remote == nullptr) {
        printf("[injector] VirtualAllocEx failed (%lu)\n", GetLastError());
        CloseHandle(proc);
        return false;
    }
    if (!WriteProcessMemory(proc, remote, dll_w.data(), bytes, nullptr)) {
        printf("[injector] WriteProcessMemory failed (%lu)\n", GetLastError());
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        CloseHandle(proc);
        return false;
    }

    const auto load_library = reinterpret_cast<LPTHREAD_START_ROUTINE>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW"));
    HANDLE thread = CreateRemoteThread(proc, nullptr, 0, load_library, remote, 0, nullptr);
    if (thread == nullptr) {
        printf("[injector] CreateRemoteThread failed (%lu)\n", GetLastError());
        VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
        CloseHandle(proc);
        return false;
    }
    WaitForSingleObject(thread, 15000);
    DWORD rc = 0;
    GetExitCodeThread(thread, &rc); // HMODULE low bits; 0 = LoadLibrary failed
    CloseHandle(thread);
    VirtualFreeEx(proc, remote, 0, MEM_RELEASE);
    CloseHandle(proc);

    if (rc == 0) {
        printf("[injector] LoadLibraryW returned NULL in target\n");
        return false;
    }

    // Confirm via the module list (authoritative; the HMODULE exit code is
    // truncated on some architectures and meaningless for dormant-reload cases).
    for (int32_t i = 0; i < 50; ++i) {
        if (module_loaded(pid, cfg.dll.filename().string())) {
            printf("[injector] injected %s into %s (pid %lu)\n", cfg.dll.filename().string().c_str(),
                   cfg.process.c_str(), pid);
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    printf("[injector] LoadLibrary returned but module not visible; injection FAILED\n");
    return false;
}

bool do_unload(const Config& cfg) {
    const uint32_t pid = find_pid(cfg.process);
    if (pid == 0) {
        printf("[injector] process %s not found\n", cfg.process.c_str());
        return false;
    }
    const std::string resident = any_resident_instance(pid);
    if (resident.empty()) {
        printf("[injector] no fear2vr instance resident; nothing to unload\n");
        return true;
    }

    std::string resp;
    if (!http::post(cfg.port, "/unload", "", resp)) {
        // IPC down but module resident: a dormant prior instance. Nothing to do
        // remotely; its hooks are already retired, its IPC already stopped.
        printf("[injector] %s resident but IPC unreachable (dormant?) -- cannot unload remotely\n",
               resident.c_str());
        return false;
    }
    printf("[injector] unload requested; waiting for module to unmap...\n");

    for (int32_t i = 0; i < 300; ++i) { // 30s budget (CREATE of quiescence can take ~2s+)
        if (any_resident_instance(pid).empty()) {
            printf("[injector] module unmapped cleanly\n");
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    printf("[injector] module still resident after timeout (dormant fallback -- hooks are retired;\n"
           "           inject the next build under a fresh filename via --reload)\n");
    return false;
}

bool do_reload(const Config& cfg) {
    const uint32_t pid = find_pid(cfg.process);
    if (pid == 0) {
        printf("[injector] process %s not found\n", cfg.process.c_str());
        return false;
    }

    // Fresh filename: fear2vr.<timestamp>.dll next to the source DLL.
    const fs::path dir = cfg.dll.parent_path();
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path fresh = dir / (cfg.dll.stem().string() + "." + std::to_string(stamp) + ".dll");

    std::error_code ec;
    fs::copy_file(cfg.dll, fresh, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        // A still-injecting (locked) source shouldn't happen; but a locked binary
        // mid-rebuild must be a loud failure, not a silent skip.
        printf("[injector] cannot copy %s to fresh name: %s\n", cfg.dll.string().c_str(),
               ec.message().c_str());
        return false;
    }

    // Best-effort unload of any live instance first (ignore dormant failure).
    if (!any_resident_instance(pid).empty()) {
        (void)do_unload(cfg);
    }

    Config fresh_cfg = cfg;
    fresh_cfg.dll = fresh;
    const bool ok = do_inject(fresh_cfg, true);
    if (ok) {
        printf("[injector] reloaded as %s\n", fresh.filename().string().c_str());
    }
    return ok;
}

void do_status(const Config& cfg) {
    const uint32_t pid = find_pid(cfg.process);
    printf("[injector] process %s: %s\n", cfg.process.c_str(), pid ? "RUNNING" : "not found");
    if (pid == 0) return;

    const std::string resident = any_resident_instance(pid);
    printf("[injector] module fear2vr*.dll: %s\n", resident.empty() ? "not loaded" : resident.c_str());

    std::string resp;
    if (http::get(cfg.port, "/health", resp)) {
        printf("[injector] IPC: UP -- %s\n", http::body_of(resp).c_str());
    } else {
        printf("[injector] IPC: DOWN (port %d)\n", cfg.port);
    }
}

// ---- ONE STEP -----------------------------------------------------------
//
// Everything a person has to do to play, in a single action: bring up the OpenXR host, ask Steam
// for the game, wait for it to appear, inject. The mod arms itself once there is a world, so there
// is nothing after this -- no script, no HTTP, no ordering to get right.
//
// Waiting for the process rather than launching FEAR2.exe directly is deliberate: Steam wants to
// start its own game (DRM, overlay, cloud saves), and hunting for the install path is a guess that
// breaks on every library layout that isn't this machine's.

// F.E.A.R. 2: Project Origin. Steam's own id -- the launcher does not need to know where the game
// is installed, only who to ask for it.
constexpr const char* kSteamAppId = "16450";

// The host, looked for beside the injector first (release layout), then in the build tree.
fs::path find_host(const fs::path& self_dir) {
    const fs::path candidates[] = {
        self_dir / "xr64.exe",
        self_dir / ".." / ".." / "build64" / "RelWithDebInfo" / "xr64.exe",
        self_dir / ".." / ".." / "build64" / "Release" / "xr64.exe",
    };
    for (const auto& c : candidates) {
        std::error_code ec;
        if (fs::exists(c, ec)) return fs::canonical(c, ec);
    }
    return {};
}

bool start_host(const fs::path& self_dir) {
    if (find_pid("xr64.exe") != 0) {
        printf("[launch] host already running\n");
        return true;
    }
    const fs::path host = find_host(self_dir);
    if (host.empty()) {
        printf("[launch] xr64.exe not found next to the injector -- start it yourself\n");
        return false;
    }
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::wstring cmd = host.wstring();
    // Its own console: the host's frame loop logs continuously and would bury the launcher's output.
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE,
                                   nullptr, host.parent_path().c_str(), &si, &pi);
    if (!ok) {
        printf("[launch] could not start %ls (error %lu)\n", host.c_str(), GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    printf("[launch] host started: %ls\n", host.c_str());
    return true;
}

bool do_launch(const Config& cfg, bool start_game, int32_t wait_seconds) {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    const fs::path self_dir = fs::path(self).parent_path();

    start_host(self_dir);

    if (find_pid(cfg.process.c_str()) == 0 && start_game) {
        const std::string url = std::string("steam://rungameid/") + kSteamAppId;
        printf("[launch] asking Steam for the game (%s)\n", url.c_str());
        const std::wstring wurl(url.begin(), url.end());
        ShellExecuteW(nullptr, L"open", wurl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    printf("[launch] waiting for %s", cfg.process.c_str());
    uint32_t pid = 0;
    for (int32_t elapsed = 0; elapsed < wait_seconds; ++elapsed) {
        pid = find_pid(cfg.process.c_str());
        if (pid != 0) break;
        printf(".");
        fflush(stdout);
        Sleep(1000);
    }
    printf("\n");
    if (pid == 0) {
        printf("[launch] %s never appeared after %ds -- start it and run this again\n",
               cfg.process.c_str(), wait_seconds);
        return false;
    }

    // WAIT FOR gameclient.dll, NOT FOR THE PROCESS. Injecting the moment FEAR2.exe exists produces a
    // mod that loads, hooks d3d9 and runs frames while every SDK scan fails with "gameclient.dll
    // module unresolved" -- the game is playable, nothing reports an error, and VR never arms.
    // gameserver.dll has a late-resolve path and recovers; gameclient.dll does not, so its absence
    // at injection is permanent for the session. It is the module the SDK is built on, which makes
    // it the only honest readiness signal here.
    printf("[launch] found pid %u, waiting for gameclient.dll\n", pid);
    bool client_up = false;
    for (int32_t elapsed = 0; elapsed < 180; ++elapsed) {
        if (module_loaded(pid, "gameclient.dll")) {
            client_up = true;
            break;
        }
        Sleep(1000);
    }
    if (!client_up) {
        // Injecting anyway rather than refusing: a layout this check does not understand should
        // degrade to the old behaviour, not to a launcher that will not launch. Loud, because a
        // session that starts here is the broken one described above.
        printf("[launch] gameclient.dll never appeared -- injecting anyway, but SDK scans will "
               "likely fail and VR will not arm\n");
    }

    return do_inject(cfg);
}

void print_usage() {
    printf("usage: injector.exe [--launch|--inject|--unload|--reload|--status]\n");
    printf("  --launch   start the host, ask Steam for the game, inject when it appears (default)\n");
    printf("  options: [--process FEAR2.exe] [--dll path\\to\\fear2vr.dll] [--port 8798]\n");
    printf("           [--no-game] do not ask Steam to start the game, just wait for it\n");
    printf("           [--wait N] seconds to wait for the game (default 300)\n");
}

} // namespace

int main(int argc, char** argv) {
    Config cfg;
    enum class Action { None, Inject, Unload, Reload, Status, Launch } action = Action::None;
    bool start_game = true;
    int32_t wait_seconds = 300;

    for (int32_t i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--inject") action = Action::Inject;
        else if (a == "--launch") action = Action::Launch;
        else if (a == "--no-game") start_game = false;
        else if (a == "--wait") wait_seconds = atoi(next().c_str());
        else if (a == "--unload") action = Action::Unload;
        else if (a == "--reload") action = Action::Reload;
        else if (a == "--status") action = Action::Status;
        else if (a == "--process") cfg.process = next();
        else if (a == "--dll") cfg.dll = next();
        else if (a == "--port") cfg.port = atoi(next().c_str());
        else {
            print_usage();
            return 2;
        }
    }

    if (cfg.dll.empty()) {
        // Default: fear2vr.dll next to this executable.
        wchar_t self[MAX_PATH];
        GetModuleFileNameW(nullptr, self, MAX_PATH);
        cfg.dll = fs::path(self).parent_path() / "fear2vr.dll";
    }

    // NO ARGUMENTS MEANS PLAY. Double-clicking the injector is the shortest path there is, and it
    // is the one a release note can describe in a single sentence.
    if (action == Action::None) action = Action::Launch;

    bool ok = false;
    switch (action) {
        case Action::Inject: ok = do_inject(cfg); break;
        case Action::Unload: ok = do_unload(cfg); break;
        case Action::Reload: ok = do_reload(cfg); break;
        case Action::Status: do_status(cfg); return 0;
        case Action::Launch: ok = do_launch(cfg, start_game, wait_seconds); break;
        default: break;
    }
    return ok ? 0 : 1;
}
