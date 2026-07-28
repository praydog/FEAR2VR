#include "Framework.hpp"

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
#include "sdk/Engine.hpp"

std::unique_ptr<Framework> g_framework;

namespace {

// --- frame hook (the framework's first real hook) ---------------------------
// Detour on CClientShell::Update (sdk-mapped anchor). Counts ticks and fans
// out Mods::on_frame. x86 __thiscall (ecx=this) -> __fastcall shim (edx dummy).
int __fastcall frame_tick_detour(void* _this, void* edx) {
    Framework* fw = Framework::get();
    if (fw != nullptr) {
        fw->note_frame_tick();
    }
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
    char buf[1024];
    const auto* exe = sdk::Modules::get().exe();
    snprintf(buf, sizeof(buf),
             "{\"ok\":true,\"exe_base\":\"0x%08X\",\"exe_size\":\"0x%08X\","
             "\"client_mgr_update\":\"0x%08X\",\"client_shell_update\":\"0x%08X\","
             "\"get_engine_hook\":\"0x%08X\",\"g_pClientMgr_slot\":\"0x%08X\","
             "\"hWnd_slot\":\"0x%08X\",\"client_mgr\":\"0x%08X\","
             "\"client_shell\":\"0x%08X\",\"main_hwnd\":\"0x%08X\",\"database_mgr\":\"0x%08X\"}",
             static_cast<uint32_t>(exe->base), static_cast<uint32_t>(exe->size),
             static_cast<uint32_t>(sdk::CClientMgr::update_fn()),
             static_cast<uint32_t>(sdk::CClientShell::update_fn()),
             static_cast<uint32_t>(sdk::Engine::get_engine_hook_fn()),
             static_cast<uint32_t>(sdk::CClientMgr::instance_slot()),
             static_cast<uint32_t>(sdk::Engine::main_hwnd_slot()),
             static_cast<uint32_t>(reinterpret_cast<uintptr_t>(sdk::CClientMgr::get())),
             static_cast<uint32_t>(reinterpret_cast<uintptr_t>(sdk::CClientShell::get())),
             static_cast<uint32_t>(reinterpret_cast<uintptr_t>(sdk::Engine::main_hwnd())),
             static_cast<uint32_t>(reinterpret_cast<uintptr_t>(sdk::DatabaseMgr::get())));
    return buf;
}

std::string build_engine_hook_json(const std::string& name) {
    void* out = nullptr;
    const int rc = sdk::Engine::get_engine_hook(name.c_str(), &out);
    char buf[256];
    snprintf(buf, sizeof(buf), "{\"ok\":true,\"rc\":%d,\"value\":\"0x%08X\"}", rc,
             static_cast<uint32_t>(reinterpret_cast<uintptr_t>(out)));
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
