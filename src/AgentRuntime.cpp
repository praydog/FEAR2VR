#include "AgentRuntime.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#include <windows.h>
#include <winternl.h>
#include <tlhelp32.h>
#include <algorithm>

#include "Framework.hpp"
#include "Log.hpp"
#include "ipc/CommandServer.hpp"

// ---- WAIT FOR gameclient.dll THE INSTANT IT LOADS ----------------------------------------------
//
// The OEP gate puts us in before the engine builds anything, which is what we wanted -- and it also
// puts us in before gameclient.dll exists. Initialising there made every gameclient-dependent mod
// fail its one-shot on_initialize (ViewHook, SyntheticInput, CameraPassHook, HeadTracking,
// HudPassHook, RenderTimeline) and left VR dead in world. Being early is worthless if it means
// being early for the wrong module.
//
// Polling for it would work and would also be a race with whatever the engine does next.
// LdrRegisterDllNotification is exact: the loader calls us as the module is mapped, so the scans
// the SDK performs have their bytes and nothing has had a chance to run yet.
//
// Pattern taken from re2-barebones' REFramework.cpp.
typedef struct _LDR_DLL_NOTIFICATION_ENTRY {
    ULONG Flags;
    PCUNICODE_STRING FullDllName;
    PCUNICODE_STRING BaseDllName;
    PVOID DllBase;
    ULONG SizeOfImage;
} LDR_DLL_NOTIFICATION_ENTRY, *PLDR_DLL_NOTIFICATION_ENTRY;

typedef union _LDR_DLL_NOTIFICATION_DATA {
    LDR_DLL_NOTIFICATION_ENTRY Loaded;
    LDR_DLL_NOTIFICATION_ENTRY Unloaded;
} LDR_DLL_NOTIFICATION_DATA, *PLDR_DLL_NOTIFICATION_DATA;

using PLDR_DLL_NOTIFICATION_FUNCTION = void(CALLBACK*)(ULONG, PLDR_DLL_NOTIFICATION_DATA, PVOID);
using LdrRegisterDllNotification_t = NTSTATUS(NTAPI*)(ULONG, PLDR_DLL_NOTIFICATION_FUNCTION, PVOID,
                                                      PVOID*);

#define FEAR2VR_LDR_LOADED 1

static std::atomic<bool> g_gameclient_loaded{false};
static PVOID g_ldr_cookie = nullptr;

void CALLBACK ldr_notification_callback(ULONG reason, PLDR_DLL_NOTIFICATION_DATA data, PVOID) {
    // Runs under the LOADER LOCK, on the thread doing the load. Do nothing here but observe:
    // creating threads or loading libraries from this context is how a deadlock is written. The
    // supervisor picks the flag up and does the real work on its own thread.
    if (reason != FEAR2VR_LDR_LOADED || data == nullptr || data->Loaded.BaseDllName == nullptr ||
        data->Loaded.BaseDllName->Buffer == nullptr) {
        return;
    }
    std::wstring name{data->Loaded.BaseDllName->Buffer,
                      data->Loaded.BaseDllName->Length / sizeof(wchar_t)};
    std::transform(name.begin(), name.end(), name.begin(), ::towlower);
    if (name == L"gameclient.dll") {
        g_gameclient_loaded.store(true, std::memory_order_release);
    }
}

// Returns true if the notification is registered; false means fall back to polling.
bool register_ldr_notification() {
    if (GetModuleHandleW(L"gameclient.dll") != nullptr) {
        g_gameclient_loaded.store(true, std::memory_order_release);
        return true;
    }
    auto* nt = GetModuleHandleW(L"ntdll.dll");
    if (nt == nullptr) {
        return false;
    }
    auto reg = reinterpret_cast<LdrRegisterDllNotification_t>(
        GetProcAddress(nt, "LdrRegisterDllNotification"));
    if (reg == nullptr) {
        return false;
    }
    return reg(0, &ldr_notification_callback, nullptr, &g_ldr_cookie) == 0;
}

// FEAR2.exe is SteamStub-wrapped: .text is ciphertext until the stub decrypts it in memory, and the
// launcher can inject before that happens. A hook written then patches bytes the stub is about to
// overwrite.
//
// The plaintext is the signal. sub_46F715 begins `fldz; push esi` = D9 EE 56, known from the
// FEAR2_dump.exe IDB. Separate function because MSVC rejects __try in anything requiring unwinding.
// ---- BOOTSTRAP GATE: HOLD THE ENGINE AT SteamStub's CALL TO OEP --------------------------------
//
// The launcher injects us while the process is parked at the entry point, which is the only way to
// exist before the engine builds anything. Two facts make the naive versions fail:
//
//   - FEAR2.exe is SteamStub-wrapped, so .text is CIPHERTEXT until the stub runs. A byte patch
//     placed now is destroyed by decryption; measured, and it silently cost us
//     Renderer_SetPresentationParams.
//   - Watching for decryption and then reacting is a RACE. Measured twice: with the DLL loaded and
//     spinning on a plaintext probe, r_InitRender was caught while SetPresentationParams had
//     already run.
//
// So gate EXECUTION rather than observing it. FEAR2.exe 0x7761F8 is `call [ebp+var_434]`, the last
// call SteamStub's `start` makes before its epilogue, and its target is the real OEP (0x652ED8). At
// that instruction the image is fully decrypted and the engine has not started.
//
// A HARDWARE execute breakpoint is what makes this safe: DR7 R/W=00 changes no bytes, so it can be
// armed while .bind is still ciphertext and stub self-verification has nothing to observe. A
// debug port was tried instead and produced "T:0000065432", an error no other path produced.
//
// This is deliberately self-contained rather than using the Watchpoints mod: that mod is
// initialised by the framework, and the framework is precisely what this gate exists to run first.
namespace {

// THE OEP ITSELF, not SteamStub's call to it. 0x7761F8 (`call [ebp+var_434]`) looked like the
// dispatch, and the breakpoint there was verified armed and still armed after release -- dr0
// 0x007761F8, dr7 0x1, thread alive -- yet never faulted. So that call is one of several tails in
// `start` and not the taken path.
//
// The destination cannot be avoided. And a hardware breakpoint does not care that .text is still
// ciphertext when it is armed: DRs match an ADDRESS, not content, so the fault happens the moment
// the decrypted OEP executes.
constexpr uintptr_t kOepCallRva = 0x252ED8;  // FEAR2.exe 0x652ED8, the real OEP

std::atomic<bool> g_gate_hit{false};
std::atomic<bool> g_gate_release{false};
uintptr_t g_gate_addr = 0;
PVOID g_gate_veh = nullptr;

LONG CALLBACK gate_veh(EXCEPTION_POINTERS* info) {
    if (info->ExceptionRecord->ExceptionCode != static_cast<DWORD>(EXCEPTION_SINGLE_STEP) ||
        g_gate_addr == 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (reinterpret_cast<uintptr_t>(info->ExceptionRecord->ExceptionAddress) != g_gate_addr) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    // Execute watches FAULT, so we are here BEFORE the call runs. Park until the supervisor has the
    // hooks in, then take the breakpoint back out and fall through into OEP.
    g_gate_hit.store(true, std::memory_order_release);
    while (!g_gate_release.load(std::memory_order_acquire)) {
        Sleep(1);
    }
    info->ContextRecord->Dr0 = 0;
    info->ContextRecord->Dr7 &= ~static_cast<DWORD>(0x1);
    return EXCEPTION_CONTINUE_EXECUTION;
}

// The engine's thread: everything in this process that is not us. At injection time that is the
// primary thread, still parked at the launcher's entry-point gate.
bool arm_gate_on_other_threads(uintptr_t addr) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return false;
    }
    const DWORD self_pid = GetCurrentProcessId();
    const DWORD self_tid = GetCurrentThreadId();
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    uint32_t armed = 0;
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != self_pid || te.th32ThreadID == self_tid) {
                continue;
            }
            HANDLE th = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME,
                                   FALSE, te.th32ThreadID);
            if (th == nullptr) {
                continue;
            }
            SuspendThread(th);
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(th, &ctx)) {
                ctx.Dr0 = static_cast<DWORD>(addr);
                // Slot 0 local enable; R/W=00 (execute) and LEN=00 (1 byte) are already zero.
                ctx.Dr7 = (ctx.Dr7 & ~static_cast<DWORD>(0xF0000)) | 0x1;
                ctx.Dr6 = 0;
                ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS;
                if (SetThreadContext(th, &ctx)) {
                    ++armed;
                }
            }
            ResumeThread(th);
            CloseHandle(th);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);
    return armed != 0;
}

// Returns true if the gate is armed and the caller should wait for it.
bool install_bootstrap_gate() {
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (base == 0) {
        return false;
    }
    g_gate_addr = base + kOepCallRva;
    g_gate_veh = AddVectoredExceptionHandler(1, gate_veh);
    if (g_gate_veh == nullptr) {
        LOGX("[gate] AddVectoredExceptionHandler failed -- continuing without the OEP gate");
        return false;
    }
    if (!arm_gate_on_other_threads(g_gate_addr)) {
        RemoveVectoredExceptionHandler(g_gate_veh);
        g_gate_veh = nullptr;
        LOGX("[gate] could not arm the OEP breakpoint -- continuing without it");
        return false;
    }
    LOGX("[gate] armed hardware execute breakpoint at 0x%08IX (SteamStub -> OEP)", g_gate_addr);
    return true;
}

} // namespace

static bool exe_is_decrypted() {
    constexpr uintptr_t kProbeRva = 0x6F715;
    constexpr uint8_t kProbe[3] = {0xD9, 0xEE, 0x56};
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (base == 0) {
        return false;
    }
    __try {
        const auto* p = reinterpret_cast<const uint8_t*>(base + kProbeRva);
        return p[0] == kProbe[0] && p[1] == kProbe[1] && p[2] == kProbe[2];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}


namespace runtime {

void run_supervisor(void* self_raw, int32_t ipc_port) {
    HMODULE self = static_cast<HMODULE>(self_raw);

    // Resolve our own path once; the log derives from it. Grow the buffer until
    // the path fits -- a truncated path would silently misplace the log.
    std::wstring self_str(MAX_PATH, L'\0');
    for (;;) {
        DWORD n = GetModuleFileNameW(self, self_str.data(), static_cast<DWORD>(self_str.size()));
        if (n == 0 || (n == self_str.size() && GetLastError() == ERROR_INSUFFICIENT_BUFFER)) {
            if (n == 0 || self_str.size() > 64 * 1024) {
                self_str = L"fear2vr"; // give up: log still lands somewhere sane
                break;
            }
            self_str.resize(self_str.size() * 2);
            continue;
        }
        self_str.resize(n);
        break;
    }
    const std::filesystem::path self_path{self_str};

    std::string log_path = self_path.string();
    {
        const size_t dot = log_path.find_last_of('.');
        if (dot != std::string::npos && log_path.find_last_of("\\/") < dot) {
            log_path.erase(dot);
        }
        log_path += ".log";
    }
    logger::init(log_path.c_str());
    LOGX("[main] supervisor thread start (module %s)", self_path.string().c_str());

    // g_framework is assigned exactly once here and deliberately NEVER reset:
    // after a clean unmap the whole image goes away; on the dormant path the
    // object must stay for any straggler. Destruction never runs in-process.
    // Hold the engine at SteamStub's call to OEP while the framework installs its hooks. Falls back
    // to the plaintext poll if the gate cannot be armed -- worse (it is a race) but not broken.
    const bool gated = install_bootstrap_gate();
    if (gated) {
        for (int i = 0; i < 30000 && !g_gate_hit.load(std::memory_order_acquire); ++i) {
            Sleep(1);
        }
        LOGX("[gate] %s", g_gate_hit.load(std::memory_order_acquire)
                              ? "engine parked at OEP -- installing hooks now"
                              : "OEP was never reached; proceeding ungated");
    } else {
        bool decrypted = false;
        for (int i = 0; i < 4000000 && !decrypted; ++i) {
            decrypted = exe_is_decrypted();
            if (!decrypted) {
                SwitchToThread();
            }
        }
        LOGX("[main] exe %s", decrypted ? "decrypted" : "NEVER matched its plaintext signature");
    }

    // ---- RELEASE, THEN WAIT FOR gameclient.dll ------------------------------------------------
    //
    // The engine cannot load gameclient.dll while it is parked at OEP, so holding the gate until
    // the module appears would deadlock. Release first, then wait for the loader to tell us.
    {
        const bool have_notify = register_ldr_notification();
        if (gated) {
            g_gate_release.store(true, std::memory_order_release);
            LOGX("[gate] released -- waiting for gameclient.dll before initialising");
        }
        bool loaded = false;
        for (int i = 0; i < 60000 && !loaded; ++i) {
            loaded = g_gameclient_loaded.load(std::memory_order_acquire) ||
                     GetModuleHandleW(L"gameclient.dll") != nullptr;
            if (!loaded) {
                Sleep(1);
            }
        }
        LOGX("[main] gameclient.dll %s (%s)", loaded ? "present -- initialising" : "NEVER appeared",
             have_notify ? "loader notification" : "polling fallback");
    }

    g_framework = std::make_unique<Framework>(self, ipc_port);
    bool ipc_up = g_framework->initialize();



    if (!ipc_up) {
        // Without IPC no /unload can ever arrive; leave the module resident but
        // inert (hooks were never installed -- initialize() installs nothing
        // before starting IPC).
        LOGX("[main] framework init failed; going dormant (module stays mapped)");
        ExitThread(0);
    }

    // Poll for a hot-reload/unload request from the IPC channel.
    while (!cmdsrv::unload_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    LOGX("[main] unload requested; retiring");
    const bool safe_to_unmap = g_framework->shutdown();

    // Never destruct the framework in-process: on the clean path the CRT would
    // otherwise run ~unique_ptr during DLL_PROCESS_DETACH (unmap-time destructors
    // touching mapped state are a classic unload crash). Same contract on the
    // dormant path: the object must stay fully valid for stragglers.
    (void)g_framework.release();

    if (safe_to_unmap) {
        LOGX("[main] unmapping (clean unload)");
        // Never returns; the thread exits as the image unmaps.
        FreeLibraryAndExitThread(self, 0);
    }

    // Dormant fallback: a straggler could not be proven clear. The module stays
    // mapped; the injector loads the next build under a fresh filename.
    LOGX("[main] dormant: module stays mapped; load next build under a new name");
    ExitThread(0);
}

} // namespace runtime
