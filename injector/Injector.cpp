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

#include <fstream>
#include <cstring>
#include <iterator>
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

// The 4 GB session runs from the LAA COPY, so its image name is FEAR2_laa.exe rather than
// FEAR2.exe. Every by-name lookup has to accept both or --inject/--status/--unload miss it and a
// second --launch starts a duplicate. The copy wins when both exist: that is the session with the
// address space.
uint32_t find_game_pid(const std::string& configured) {
    std::string copy = configured;
    const size_t dot = copy.rfind('.');
    if (dot != std::string::npos) {
        copy.insert(dot, "_laa");
        if (const uint32_t p = find_pid(copy)) return p;
    }
    return find_pid(configured);
}

bool do_inject_pid(const Config& cfg, uint32_t pid, bool allow_resident);

bool do_inject(const Config& cfg, bool allow_resident = false) {
    const uint32_t pid = find_game_pid(cfg.process);
    if (pid == 0) {
        printf("[injector] process %s not found\n", cfg.process.c_str());
        return false;
    }
    return do_inject_pid(cfg, pid, allow_resident);
}

// Injection by PID. The launcher already knows the exact process it created and verified, and the
// LAA copy runs under a different image name -- looking it up again by name would either miss it or
// find a stale one.
bool do_inject_pid(const Config& cfg, uint32_t pid, bool allow_resident = false) {

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
    const uint32_t pid = find_game_pid(cfg.process);
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
    const uint32_t pid = find_game_pid(cfg.process);
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
    const uint32_t pid = find_game_pid(cfg.process);
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

// ---- 4 GB WITHOUT TOUCHING THE SHIPPED EXE -----------------------------------------------------
//
// FEAR2.exe ships without PE LARGE_ADDRESS_AWARE, so it is capped at 2 GB. With this mod resident
// that ceiling is hit in normal play -- measured at 1635 MB committed with a largest free block of
// 10.9 MB -- and it surfaces as crashes that look like anything but memory: a null dereference in
// our readback ring, and an access violation inside XAudio2 with no frame of ours on the stack.
//
// The kernel reads that flag at process CREATION; SteamStub validates afterwards. So the game is
// started from an LAA COPY and made to look like the original before the stub ever runs:
//
//   1. Copy FEAR2.exe -> FEAR2_laa.exe and set the bit on the COPY. The shipped file is opened
//      read-only and never written, so no crash or power cut can damage the installation.
//   2. CreateProcess the copy SUSPENDED with steam.exe as PROC_THREAD_ATTRIBUTE_PARENT_PROCESS.
//      Without the parent the stub reports "Application load error 5:0000065434" -- it checks who
//      started it.
//   3. Patch a jmp over the ENTRY POINT to a small stub. The entry point specifically: at
//      CREATE_SUSPENDED the thread is still in LdrInitializeThunk and PEB->Ldr does not exist yet,
//      so spoofing from outside at that moment cannot touch the LDR entry -- measured, it dies
//      ~2 s in. At the EP the loader has run and the stub has not.
//   4. The stub rewrites PEB->Ldr's main entry (FullDllName/BaseDllName), ProcessParameters->
//      ImagePathName, and the mapped Characteristics back to the pristine original, publishes a
//      status word, then spins on a gate so this side can VERIFY before the game continues.
//   5. Release the gate; the stub restores the entry point bytes and jumps to it.
//
// A debugger was tried for step 3 and rejected: DEBUG_ONLY_THIS_PROCESS produced an error code no
// other path produced, so the debug port itself is a tell. The stub needs no debugger.
//
// Verified: ProcessParameters and both LDR names read back as the original path, Characteristics
// 0x0102, allocation above 0x80000000 succeeds, process alive well past every rejection window, and
// the shipped exe's SHA-256 is unchanged.

constexpr const wchar_t* kCopySuffix = L"_laa.exe";
constexpr uint32_t kStubDone = 0xC0FFEE01;

using NtQueryInformationProcess_t = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);

std::wstring reg_str(HKEY root, const wchar_t* path, const wchar_t* name) {
    wchar_t buf[512]{};
    DWORD cb = sizeof(buf);
    if (RegGetValueW(root, path, name, RRF_RT_REG_SZ, nullptr, buf, &cb) != ERROR_SUCCESS) return {};
    return buf;
}
uint32_t reg_dword(HKEY root, const wchar_t* path, const wchar_t* name) {
    DWORD v = 0, cb = sizeof(v);
    if (RegGetValueW(root, path, name, RRF_RT_REG_DWORD, nullptr, &v, &cb) != ERROR_SUCCESS) return 0;
    return v;
}

// Steam's launch context, DERIVED rather than observed. Asking Steam to start the game first just
// to read its environment works but is slow and visible, and everything in it is available here.
// Verified against a live capture: the STEAMID computed below matched exactly.
struct SteamContext {
    std::wstring cmdline;
    std::wstring environment;  // packed, double-null terminated
    bool ok = false;
};

SteamContext derive_steam_context(const std::wstring& exe) {
    SteamContext c;
    std::wstring steam_path = reg_str(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
    for (auto& ch : steam_path) if (ch == L'/') ch = L'\\';
    const std::wstring user = reg_str(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"AutoLoginUser");
    const uint32_t active =
        reg_dword(HKEY_CURRENT_USER, L"Software\\Valve\\Steam\\ActiveProcess", L"ActiveUser");
    if (steam_path.empty() || active == 0) return c;

    // 32-bit account id -> SteamID64.
    const unsigned long long steamid = static_cast<unsigned long long>(active) + 76561197960265728ull;

    // The player's launch options, so things like "+windowed 1" survive.
    std::wstring opts;
    {
        const std::wstring lc = steam_path + L"\\userdata\\" + std::to_wstring(active) +
                                L"\\config\\localconfig.vdf";
        std::ifstream in(lc, std::ios::binary);
        if (in) {
            const std::string all((std::istreambuf_iterator<char>(in)),
                                  std::istreambuf_iterator<char>());
            size_t at = all.find("\"" + std::string(kSteamAppId) + "\"");
            while (at != std::string::npos) {
                const std::string seg = all.substr(at, 1500);
                const size_t lo = seg.find("\"LaunchOptions\"");
                if (lo != std::string::npos) {
                    const size_t q1 = seg.find('"', lo + 15);
                    const size_t q2 = q1 == std::string::npos ? q1 : seg.find('"', q1 + 1);
                    if (q2 != std::string::npos) {
                        const std::string s = seg.substr(q1 + 1, q2 - q1 - 1);
                        opts.assign(s.begin(), s.end());
                    }
                    break;
                }
                at = all.find("\"" + std::string(kSteamAppId) + "\"", at + 1);
            }
        }
    }
    c.cmdline = L"\"" + exe + L"\"";
    if (!opts.empty()) c.cmdline += L" " + opts;

    std::wstring env;
    for (wchar_t* p = GetEnvironmentStringsW(); p != nullptr && *p != L'\0';) {
        const std::wstring one(p);
        // Ours win; drop any inherited Steam vars so they cannot conflict.
        if (one.rfind(L"Steam", 0) != 0 && one.rfind(L"STEAMID=", 0) != 0) {
            env += one;
            env.push_back(L'\0');
        }
        p += one.size() + 1;
    }
    const std::wstring appid(kSteamAppId, kSteamAppId + strlen(kSteamAppId));
    const std::wstring steam_vars[] = {
        L"SteamAppId=" + appid,        L"SteamGameId=" + appid,
        L"SteamOverlayGameId=" + appid, std::wstring(L"SteamClientLaunch=1"),
        std::wstring(L"SteamEnv=1"),   L"SteamPath=" + steam_path,
        L"SteamAppUser=" + user,       L"SteamUser=" + user,
        L"STEAMID=" + std::to_wstring(steamid)};
    for (const std::wstring& kv : steam_vars) {
        env += kv;
        env.push_back(L'\0');
    }
    env.push_back(L'\0');
    c.environment = env;
    c.ok = !user.empty();
    return c;
}

// The COPY is what gets modified; the shipped file is only ever read.
bool make_laa_copy(const std::wstring& src, const std::wstring& dst, uint16_t* orig_chars,
                   uint32_t* ep_rva, uint32_t* chars_rva) {
    std::error_code ec;
    fs::remove(dst, ec);
    fs::copy_file(src, dst, ec);
    if (ec) return false;

    std::fstream fp(dst, std::ios::in | std::ios::out | std::ios::binary);
    if (!fp) return false;
    uint32_t pe = 0;
    fp.seekg(0x3C);
    fp.read(reinterpret_cast<char*>(&pe), 4);
    char sig[4]{};
    fp.seekg(pe);
    fp.read(sig, 4);
    if (std::memcmp(sig, "PE\0\0", 4) != 0) return false;
    *chars_rva = pe + 22;
    fp.seekg(static_cast<std::streamoff>(pe) + 40);
    fp.read(reinterpret_cast<char*>(ep_rva), 4);
    uint16_t c = 0;
    fp.seekg(*chars_rva);
    fp.read(reinterpret_cast<char*>(&c), 2);
    *orig_chars = c;
    c |= 0x0020;
    fp.seekp(*chars_rva);
    fp.write(reinterpret_cast<const char*>(&c), 2);
    fp.flush();
    return static_cast<bool>(fp);
}

bool rpm(HANDLE h, uintptr_t a, void* b, size_t n) {
    SIZE_T got = 0;
    return ReadProcessMemory(h, reinterpret_cast<LPCVOID>(a), b, n, &got) && got == n;
}
bool wpm(HANDLE h, uintptr_t a, const void* b, size_t n) {
    DWORD old = 0;
    VirtualProtectEx(h, reinterpret_cast<LPVOID>(a), n, PAGE_EXECUTE_READWRITE, &old);
    SIZE_T put = 0;
    const bool ok = WriteProcessMemory(h, reinterpret_cast<LPVOID>(a), b, n, &put) != FALSE;
    return ok && put == n;
}

// Returns the pid of a running, correctly-spoofed 4 GB game, or 0. FAILS CLOSED: any step that
// cannot be proven kills the suspended process rather than resuming a half-spoofed SteamStub.
// Where the game is installed, without asking Steam to start it. Steam records the library that
// holds each app, so this is a lookup rather than a guess.
std::wstring game_exe_path() {
    std::wstring steam = reg_str(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
    for (auto& ch : steam) if (ch == L'/') ch = L'\\';
    if (steam.empty()) return {};

    std::vector<std::wstring> libs{steam};
    std::ifstream vdf(steam + L"\\steamapps\\libraryfolders.vdf", std::ios::binary);
    if (vdf) {
        const std::string all((std::istreambuf_iterator<char>(vdf)), std::istreambuf_iterator<char>());
        size_t at = all.find("\"path\"");
        while (at != std::string::npos) {
            const size_t q1 = all.find('"', at + 6);
            const size_t q2 = q1 == std::string::npos ? q1 : all.find('"', q1 + 1);
            if (q2 == std::string::npos) break;
            std::string p = all.substr(q1 + 1, q2 - q1 - 1);
            std::string un;
            for (size_t i = 0; i < p.size(); ++i) {
                if (p[i] == '\\' && i + 1 < p.size() && p[i + 1] == '\\') ++i;
                un.push_back(p[i]);
            }
            libs.emplace_back(un.begin(), un.end());
            at = all.find("\"path\"", q2);
        }
    }
    for (const auto& lib : libs) {
        const std::wstring cand = lib + L"\\steamapps\\common\\FEAR2\\FEAR2.exe";
        if (fs::exists(cand)) return cand;
    }
    printf("[laa] could not find FEAR2.exe in any Steam library\n");
    return {};
}

uint32_t launch_with_laa(const Config& cfg, const std::wstring& exe) {
    const std::wstring copy = fs::path(exe).parent_path().wstring() + L"\\" +
                              fs::path(exe).stem().wstring() + kCopySuffix;

    const SteamContext ctx = derive_steam_context(exe);
    if (!ctx.ok) {
        printf("[laa] could not derive Steam's launch context from the registry\n");
        return 0;
    }
    uint16_t orig_chars = 0;
    uint32_t ep_rva = 0, chars_rva = 0;
    if (!make_laa_copy(exe, copy, &orig_chars, &ep_rva, &chars_rva)) {
        printf("[laa] could not build the LAA copy\n");
        return 0;
    }

    const uint32_t steam_pid = find_pid("steam.exe");
    HANDLE hsteam = steam_pid ? OpenProcess(PROCESS_CREATE_PROCESS, FALSE, steam_pid) : nullptr;
    if (hsteam == nullptr) {
        printf("[laa] Steam is not running (it must be, to parent the game to it)\n");
        return 0;
    }

    SIZE_T asz = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &asz);
    std::vector<uint8_t> abuf(asz);
    auto* alist = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(abuf.data());
    PROCESS_INFORMATION pi{};
    bool made = false;
    if (InitializeProcThreadAttributeList(alist, 1, 0, &asz) &&
        UpdateProcThreadAttribute(alist, 0, PROC_THREAD_ATTRIBUTE_PARENT_PROCESS, &hsteam,
                                  sizeof(hsteam), nullptr, nullptr)) {
        STARTUPINFOEXW si{};
        si.StartupInfo.cb = sizeof(si);
        si.lpAttributeList = alist;
        std::wstring cmd = ctx.cmdline;
        made = CreateProcessW(copy.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                              EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT |
                                  CREATE_SUSPENDED,
                              const_cast<wchar_t*>(ctx.environment.c_str()),
                              fs::path(exe).parent_path().wstring().c_str(), &si.StartupInfo,
                              &pi) != FALSE;
    }
    DeleteProcThreadAttributeList(alist);
    CloseHandle(hsteam);
    if (!made) {
        printf("[laa] CreateProcess failed (%lu)\n", GetLastError());
        std::error_code ec; fs::remove(copy, ec);
        return 0;
    }

    // From here every failure path must terminate: a resumed process with a half-applied spoof is
    // worse than no launch at all.
    const auto give_up = [&](const char* why) -> uint32_t {
        printf("[laa] %s -- terminating rather than resuming half-spoofed\n", why);
        TerminateProcess(pi.hProcess, 0);
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        std::error_code ec; fs::remove(copy, ec);
        return 0;
    };

    auto nq = reinterpret_cast<NtQueryInformationProcess_t>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));
    struct PBI32 { LONG ExitStatus; ULONG_PTR Peb; ULONG_PTR Aff; LONG Prio; ULONG_PTR Pid;
                   ULONG_PTR Parent; } pbi{};
    if (nq == nullptr || nq(pi.hProcess, 0, &pbi, sizeof(pbi), nullptr) != 0 || pbi.Peb == 0) {
        return give_up("could not read the PEB");
    }
    const uintptr_t peb = pbi.Peb;
    uint32_t base = 0;
    if (!rpm(pi.hProcess, peb + 8, &base, 4) || base == 0) return give_up("no image base");
    const uintptr_t ep = base + ep_rva;
    DWORD hdr_prot = 0;

    const std::wstring& real = exe;
    const std::wstring bname = fs::path(exe).filename().wstring();
    const auto flen = static_cast<uint16_t>(real.size() * 2);
    const auto blen = static_cast<uint16_t>(bname.size() * 2);

    auto* data = static_cast<uint8_t*>(VirtualAllocEx(pi.hProcess, nullptr, 0x1000,
                                                      MEM_COMMIT | MEM_RESERVE,
                                                      PAGE_EXECUTE_READWRITE));
    auto* codep = static_cast<uint8_t*>(VirtualAllocEx(pi.hProcess, nullptr, 0x1000,
                                                       MEM_COMMIT | MEM_RESERVE,
                                                       PAGE_EXECUTE_READWRITE));
    if (data == nullptr || codep == nullptr) return give_up("no room for the stub");
    const auto d32 = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(data));
    const auto c32 = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(codep));
    const uint32_t fptr = d32, bptr = d32 + flen + 2, status = d32 + 0xF00, gate = d32 + 0xF08;
    if (!wpm(pi.hProcess, fptr, real.c_str(), flen + 2) ||
        !wpm(pi.hProcess, bptr, bname.c_str(), blen + 2)) {
        return give_up("could not stage the strings");
    }
    const uint64_t zero = 0;
    wpm(pi.hProcess, status, &zero, 8);

    uint8_t ep_orig[5]{};
    if (!rpm(pi.hProcess, ep, ep_orig, 5)) return give_up("could not read the entry point");

    std::vector<uint8_t> sc;
    const auto b = [&](std::initializer_list<uint8_t> v) {
        sc.insert(sc.end(), v.begin(), v.end());
    };
    const auto d16 = [&](uint16_t v) { sc.insert(sc.end(), reinterpret_cast<uint8_t*>(&v),
                                                 reinterpret_cast<uint8_t*>(&v) + 2); };
    const auto dw = [&](uint32_t v) { sc.insert(sc.end(), reinterpret_cast<uint8_t*>(&v),
                                                reinterpret_cast<uint8_t*>(&v) + 4); };
    b({0x60});                                   // pushad
    b({0x64, 0xA1, 0x30, 0x00, 0x00, 0x00});     // mov eax, fs:[30h]   PEB
    b({0x8B, 0x48, 0x0C}); b({0x8B, 0x49, 0x0C});// ecx = Ldr -> first module entry
    b({0x66, 0xC7, 0x41, 0x24}); d16(flen);      // FullDllName.Length
    b({0x66, 0xC7, 0x41, 0x26}); d16(flen + 2);
    b({0xC7, 0x41, 0x28}); dw(fptr);
    b({0x66, 0xC7, 0x41, 0x2C}); d16(blen);      // BaseDllName
    b({0x66, 0xC7, 0x41, 0x2E}); d16(blen + 2);
    b({0xC7, 0x41, 0x30}); dw(bptr);
    b({0x8B, 0x48, 0x10});                       // ecx = ProcessParameters
    b({0x66, 0xC7, 0x41, 0x38}); d16(flen);      // ImagePathName
    b({0x66, 0xC7, 0x41, 0x3A}); d16(flen + 2);
    b({0xC7, 0x41, 0x3C}); dw(fptr);
    b({0x66, 0xC7, 0x05}); dw(base + chars_rva); d16(orig_chars);   // pristine Characteristics
    // ORDER MATCHES THE PROVEN SEQUENCE: publish, park on the gate, and only restore the entry
    // point once released. Restoring earlier was the difference against the working version.
    b({0xC7, 0x05}); dw(status); dw(kStubDone);
    b({0xF3, 0x90});                             // pause
    b({0x83, 0x3D}); dw(gate); b({0x00});        // cmp dword [gate], 0
    b({0x74, 0xF5});                             // je -11 (spin)
    b({0xC7, 0x05}); dw(static_cast<uint32_t>(ep));
    sc.insert(sc.end(), ep_orig, ep_orig + 4);
    b({0xC6, 0x05}); dw(static_cast<uint32_t>(ep) + 4); b({ep_orig[4]});
    b({0x61});                                   // popad
    b({0x68}); dw(static_cast<uint32_t>(ep)); b({0xC3});  // push ep; ret
    if (!wpm(pi.hProcess, c32, sc.data(), sc.size())) return give_up("could not write the stub");

    // The stub writes Characteristics back ITSELF, and the PE header page is mapped READ-ONLY.
    // Without this the stub access-violates on that store -- which is exactly what was observed:
    // exit code 0xC0000005 with the entry point already correctly hooked and the stub bytes intact.
    // wpm() only unprotects for writes made from THIS side; it does nothing for the stub's own.
    {
        if (!VirtualProtectEx(pi.hProcess, reinterpret_cast<LPVOID>(base + chars_rva), 2,
                              PAGE_READWRITE, &hdr_prot)) {
            return give_up("could not unprotect the PE header for the stub");
        }
    }

    DWORD ep_prot = 0;
    if (!VirtualProtectEx(pi.hProcess, reinterpret_cast<LPVOID>(ep), 5, PAGE_EXECUTE_READWRITE,
                          &ep_prot)) {
        return give_up("could not unprotect the entry point");
    }

    uint8_t jmp5[5] = {0xE9};
    const int32_t rel = static_cast<int32_t>(c32) - static_cast<int32_t>(ep + 5);
    std::memcpy(jmp5 + 1, &rel, 4);
    if (!wpm(pi.hProcess, ep, jmp5, 5)) return give_up("could not hook the entry point");
    FlushInstructionCache(pi.hProcess, reinterpret_cast<LPCVOID>(ep), 5);

    ResumeThread(pi.hThread);

    uint32_t st = 0;
    for (int i = 0; i < 600 && st != kStubDone; ++i) {      // ~12s
        if (!rpm(pi.hProcess, status, &st, 4)) break;
        if (st != kStubDone) Sleep(20);
    }
    if (st != kStubDone) {
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        printf("[laa] stub did not report (process exit %lu; 259 = still running)\n", code);
        return give_up("the stub never reported in");
    }

    // VERIFY before letting SteamStub run. Anything unexpected and we stop.
    const auto reads_as = [&](uint32_t at, const std::wstring& want) {
        struct U32 { uint16_t len, max; uint32_t buf; } s{};
        if (!rpm(pi.hProcess, at, &s, sizeof(s)) || s.len != want.size() * 2) return false;
        std::wstring got(s.len / 2, L'\0');
        if (!rpm(pi.hProcess, s.buf, got.data(), s.len)) return false;
        return got == want;
    };
    uint32_t params = 0, ldr = 0, first = 0;
    uint16_t chars_now = 0;
    rpm(pi.hProcess, peb + 0x10, &params, 4);
    rpm(pi.hProcess, peb + 0x0C, &ldr, 4);
    rpm(pi.hProcess, ldr + 0x0C, &first, 4);
    rpm(pi.hProcess, base + chars_rva, &chars_now, 2);
    if (!reads_as(params + 0x38, real) || !reads_as(first + 0x24, real) ||
        !reads_as(first + 0x2C, bname) || chars_now != orig_chars) {
        return give_up("the spoof did not verify");
    }

    // NOT INJECTING HERE, THOUGH THE GATE IS THE RIGHT MOMENT IN PRINCIPLE. Tried and reverted:
    // loading the DLL at the entry point got our hooks in before the device existed (verified by
    // log order), but Renderer_SetPresentationParams then never fired again -- the patch had been
    // written into a region SteamStub had not finished decrypting, and was discarded. The plaintext
    // probe only checks sub_46F715, which is decrypted earlier than the renderer's code.
    //
    // Doing this properly needs a per-region readiness check, or a second gate at the decrypted
    // OEP. See ENGINE_NOTES.
    FlushInstructionCache(pi.hProcess, reinterpret_cast<LPCVOID>(ep), 5);
    const uint32_t go = 1;
    if (!wpm(pi.hProcess, gate, &go, 4)) return give_up("could not release the gate");

    // The stub restores the entry point only after the gate opens, so wait until the bytes are
    // actually back before putting the page protections down. Leaving the header and the entry
    // point writable would mean the "pristine" image still has anomalous pages.
    bool ep_restored = false;
    for (int i = 0; i < 250 && !ep_restored; ++i) {
        uint8_t now[5]{};
        if (rpm(pi.hProcess, ep, now, 5) && std::memcmp(now, ep_orig, 5) == 0) {
            ep_restored = true;
            break;
        }
        Sleep(20);
    }
    if (!ep_restored) {
        return give_up("the entry point was never restored");
    }
    {
        DWORD tmp = 0;
        const bool a = VirtualProtectEx(pi.hProcess, reinterpret_cast<LPVOID>(ep), 5, ep_prot,
                                        &tmp) != FALSE;
        const bool bch = VirtualProtectEx(pi.hProcess, reinterpret_cast<LPVOID>(base + chars_rva), 2,
                                          hdr_prot, &tmp) != FALSE;
        if (!a || !bch) {
            return give_up("could not put the page protections back");
        }
        FlushInstructionCache(pi.hProcess, reinterpret_cast<LPCVOID>(ep), 5);
    }

    const uint32_t pid = pi.dwProcessId;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    printf("[laa] running with a 4 GB address space (pid %u); the shipped exe was never written\n",
           pid);
    return pid;
}

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

// ---- THE SECOND READINESS SIGNAL -----------------------------------------------------------
//
// gameclient.dll below says the SDK's base module is mapped. This says the SteamStub has finished
// with FEAR2.exe: the shipped exe is CEG/SteamStub-wrapped, so its .text is CIPHERTEXT until the
// stub decrypts it at runtime, and injecting before that makes every exe pattern scan miss. Those
// misses LATCH -- a function-local static caches the failure deliberately, because the exe is
// always mapped so a miss is normally definitive -- which leaves the session dead for its whole
// lifetime with no error that says why.
//
// Loading gameclient.dll is itself done by decrypted code, so in principle it already implies the
// stub has run and this check is redundant. It is here anyway because the two signals are cheap
// and the failure they guard against is silent and permanent: a session that scans early does not
// crash, it just never arms VR. Belt and braces is the right trade when the alternative is
// reasoning about the ordering of another vendor's startup.
//
// Zero-sized and tiny helper windows (splash, IME) are skipped -- they are visible but they are
// not the engine's.
bool has_visible_window(uint32_t pid) {
    struct Search {
        uint32_t pid;
        bool found;
    } search{pid, false};

    EnumWindows(
        [](HWND hwnd, LPARAM param) -> BOOL {
            auto* s = reinterpret_cast<Search*>(param);
            DWORD owner = 0;
            GetWindowThreadProcessId(hwnd, &owner);
            if (owner != s->pid || !IsWindowVisible(hwnd)) {
                return TRUE;
            }
            RECT r{};
            if (GetWindowRect(hwnd, &r) && (r.right - r.left) > 320 && (r.bottom - r.top) > 240) {
                s->found = true;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));

    return search.found;
}

bool do_launch(const Config& cfg, bool start_game, bool start_xr_host, int32_t wait_seconds) {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    const fs::path self_dir = fs::path(self).parent_path();

    // OPT-OUT, because starting the host is not a passive act. xr64.exe loads the active OpenXR
    // runtime, which starts that vendor's services and can raise a firewall prompt on a machine
    // whose owner asked for neither. A test run wants the LAA launch and nothing else, so
    // `--no-host` gives it the 4 GB session without waking a headset.
    if (start_xr_host) {
        start_host(self_dir);
    }

    // ONE DEADLINE FOR THE WHOLE LAUNCH, shared by finding the process and by the readiness gate
    // below. Giving each phase its own fresh `--wait N` means `--wait 30` can legitimately take
    // sixty seconds, which is not what the option appears to promise to anyone reading it or
    // passing it through (resume_game.py forwards its --launch-timeout straight into this).
    const ULONGLONG launch_deadline =
        GetTickCount64() + static_cast<ULONGLONG>(wait_seconds > 0 ? wait_seconds : 0) * 1000ull;

    const auto wait_for_game = [&]() {
        uint32_t p = 0;
        while (GetTickCount64() < launch_deadline) {
            p = find_game_pid(cfg.process);
            if (p != 0) break;
            printf(".");
            fflush(stdout);
            Sleep(1000);
        }
        printf("\n");
        return p;
    };

    uint32_t pid = find_game_pid(cfg.process);

    // ---- START THE GAME WITH A 4 GB ADDRESS SPACE ----------------------------------------------
    //
    // No steam:// here: the context is derived from the registry, so nothing has to be launched and
    // thrown away first. The shipped exe is never written -- see launch_with_laa.
    if (pid == 0 && start_game) {
        std::wstring exe = game_exe_path();
        if (!exe.empty()) {
            pid = launch_with_laa(cfg, exe);
        }
        if (pid == 0) {
            // NO SILENT FALLBACK. Launching through Steam here would start the game in the 2 GB
            // configuration that causes the crashes this exists to prevent, and it would do it
            // without being asked. Report and stop.
            printf("[laa] not starting the game. Run Steam's copy yourself if you want a 2 GB "
                   "session, then re-run with --inject.\n");
            return false;
        }
    } else if (pid == 0) {
        printf("[launch] waiting for %s", cfg.process.c_str());
        pid = wait_for_game();
    }

    if (pid == 0) {
        printf("[launch] %s never appeared after %ds -- start it and run this again\n",
               cfg.process.c_str(), wait_seconds);
        return false;
    }

    // TWO SIGNALS, BOTH REQUIRED, because they guard different failures.
    //
    // gameclient.dll: injecting the moment FEAR2.exe exists produces a mod that loads, hooks d3d9
    // and runs frames while every SDK scan fails with "gameclient.dll module unresolved" -- the
    // game is playable, nothing reports an error, and VR never arms. gameserver.dll has a
    // late-resolve path and recovers; gameclient.dll does not, so its absence at injection is
    // permanent for the session. It is the module the SDK is built on.
    //
    // A visible window: the SteamStub has finished decrypting FEAR2.exe's .text. See
    // has_visible_window above for why a latched pattern miss is worse than a crash.
    printf("[launch] found pid %u, waiting for gameclient.dll and the engine window\n", pid);
    bool client_up = false;
    bool window_up = false;
    // do/while, so ONE sample always happens: `--wait 0` means "do not wait", not "do not look".
    // An already-running, already-ready game must still pass the gate rather than be refused for
    // having no budget left to confirm what is already true.
    do {
        client_up = client_up || module_loaded(pid, "gameclient.dll");
        window_up = window_up || has_visible_window(pid);
        if (client_up && window_up) {
            break;
        }
        if (GetTickCount64() >= launch_deadline) {
            break;
        }
        Sleep(1000);
    } while (true);
    if (!client_up || !window_up) {
        // REFUSE, rather than injecting anyway. This used to degrade to injecting on the argument
        // that a layout the check does not understand should not stop the launcher -- but the
        // session it produces is the silently-dead one both signals exist to prevent: exe pattern
        // misses latch forever, the game stays playable, and VR simply never arms. Nothing
        // downstream can tell that apart from a code regression, and the test fixture now gates on
        // this exit code, so injecting here would turn a broken session into a green launch and a
        // suite full of reds that look like the mod's fault.
        //
        // The game is left RUNNING. Someone who knows better than this check can inject into it
        // deliberately, which is an informed override rather than a silent default.
        printf("[launch] not ready within the %ds budget (gameclient.dll %s, window %s) -- NOT "
               "injecting.\n", wait_seconds, client_up ? "yes" : "NO", window_up ? "yes" : "NO");
        printf("[launch] the game is running (pid %u). If this check is wrong for your layout, "
               "inject explicitly: injector.exe --inject\n", pid);
        return false;
    }

    return do_inject_pid(cfg, pid);
}

void print_usage() {
    printf("usage: injector.exe [--launch|--inject|--unload|--reload|--status]\n");
    printf("  --launch   start the host, start the game with a 4 GB address space, inject (default)\n");
    printf("  options: [--process FEAR2.exe] [--dll path\\to\\fear2vr.dll] [--port 8798]\n");
    printf("           [--no-game] do not start the game, just wait for one already running\n");
    printf("           [--no-host] do not start xr64.exe; launch and inject only\n");
    printf("           [--wait N] seconds to wait for the game (default 300)\n");
}

} // namespace

int main(int argc, char** argv) {
    Config cfg;
    enum class Action { None, Inject, Unload, Reload, Status, Launch } action = Action::None;
    bool start_game = true;
    bool start_xr_host = true;
    int32_t wait_seconds = 300;

    for (int32_t i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--inject") action = Action::Inject;
        else if (a == "--launch") action = Action::Launch;
        else if (a == "--no-game") start_game = false;
        else if (a == "--no-host") start_xr_host = false;
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
        case Action::Launch: ok = do_launch(cfg, start_game, start_xr_host, wait_seconds); break;
        default: break;
    }
    return ok ? 0 : 1;
}
