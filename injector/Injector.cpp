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

void print_usage() {
    printf("usage: injector.exe <--inject|--unload|--reload|--status> [--process FEAR2.exe] "
           "[--dll path\\to\\fear2vr.dll] [--port 8798]\n");
}

} // namespace

int main(int argc, char** argv) {
    Config cfg;
    enum class Action { None, Inject, Unload, Reload, Status } action = Action::None;

    for (int32_t i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--inject") action = Action::Inject;
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

    if (action == Action::None) {
        print_usage();
        return 2;
    }

    bool ok = false;
    switch (action) {
        case Action::Inject: ok = do_inject(cfg); break;
        case Action::Unload: ok = do_unload(cfg); break;
        case Action::Reload: ok = do_reload(cfg); break;
        case Action::Status: do_status(cfg); return 0;
        default: break;
    }
    return ok ? 0 : 1;
}
