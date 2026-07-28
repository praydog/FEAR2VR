#include "Framework.hpp"

#include <chrono>
#include <iterator>
#include <thread>
#include <vector>

#include <windows.h>
#include <tlhelp32.h>

#include "GameTests.hpp"
#include "Hooks.hpp"
#include "Log.hpp"
#include "sdk/Modules.hpp"
#include "sdk/Engine.hpp"

namespace engine = sdk::engine;

Framework* Framework::s_instance = nullptr;

namespace {

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

    // Thread32Next stops on ERROR_NO_MORE_FILES at the end of a clean walk;
    // any other error means we may have missed threads -> not ok.
    enumeration_ok = (GetLastError() == ERROR_NO_MORE_FILES);

    CloseHandle(snap);
    return handles;
}

// Suspend all threads, then verify NO thread has EIP inside our module while we
// are the only runnable thread. Allocation-free between suspend and resume (no
// heap, no loader lock).
bool suspend_and_verify_clear(std::vector<HANDLE>& threads, uintptr_t base, size_t size) {
    if (base == 0 || size == 0) {
        return false; // can't verify -> fail closed
    }

    std::vector<HANDLE> suspended;
    suspended.reserve(threads.size());
    bool ok = true;

    for (HANDLE h : threads) {
        if (h == nullptr) {
            ok = false; // uninspectable thread -> not safe
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
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

} // namespace

Framework::Framework(void* self_module, int32_t ipc_port) : m_self(self_module), m_ipc_port(ipc_port) {
    s_instance = this;
}

bool Framework::initialize() {
    bool expected = false;
    if (!m_initialized.compare_exchange_strong(expected, true)) {
        LOGX("[framework] initialize() called twice -- ignored");
        return true;
    }

    LOGX("[framework] initializing (self=%p, ipc port %d)", m_self, m_ipc_port);

    m_sdk_ready = sdk::initialize();
    if (!m_sdk_ready) {
        LOGX("[framework] sdk::initialize FAILED (missing modules?); continuing with degraded SDK");
        // Not fatal: IPC still comes up so tooling observes the failure state.
    }

    // In-DLL fixture tests. Registered ONCE here, before IPC starts, so no
    // registration races a /test handler on the server thread.
    gametests::add("sdk.modules.required_resolved", [](gametests::TestSink& ts) {
        // Every REQUIRED FEAR2 module must be mapped in-process with sane
        // geometry: base inside usermode and PE-derived size above the header
        // floor. (Residency proof, not mere non-null; see TESTING.MD.)
        for (sdk::Module* m : {sdk::exe(), sdk::game_client(), sdk::game_database(), sdk::lt_memory()}) {
            const bool mapped = m->handle != nullptr && m->base != 0;
            ts.check(mapped, std::string{m->name} + " not mapped in FEAR2.exe process");
            if (!mapped) {
                continue;
            }
            ts.check(m->base >= 0x10000 && m->base < 0x80000000ull,
                     std::string{m->name} + " base outside usermode range");
            ts.check(m->size >= 0x1000 && m->size < 0x8000000,
                     std::string{m->name} + " implausible image size");
        }
        // gameserver.dll is lazy: assert only that its absence is HONEST
        // (handle==0 iff base==0 iff size==0), never a partially-filled row.
        const sdk::Module* gs = sdk::game_server();
        const bool absent = gs->handle == nullptr;
        ts.check(absent == (gs->base == 0) && absent == (gs->size == 0),
                 "gameserver.dll partially resolved (dishonest absence)");
    });

    gametests::add("sdk.modules.distinct_images", [](gametests::TestSink& ts) {
        // No required module's base may lie inside another module's image.
        // Catches GetModuleHandle resolving a wrong same-named DLL.
        sdk::Module* mods[] = {sdk::exe(), sdk::game_client(), sdk::game_database(), sdk::lt_memory()};
        for (size_t i = 0; i < std::size(mods); ++i) {
            for (size_t j = 0; j < std::size(mods); ++j) {
                if (i == j || mods[i]->handle == nullptr || mods[j]->size == 0) {
                    continue;
                }
                const uintptr_t a = mods[i]->base;
                const uintptr_t b0 = mods[j]->base;
                ts.check(a < b0 || a >= b0 + mods[j]->size,
                         std::string{mods[i]->name} + " base inside " + mods[j]->name + " image");
            }
        }
    });

    gametests::add("sdk.engine.targets_resolved", [](gametests::TestSink& ts) {
        ts.check(engine::resolve(), "engine::resolve() failed (pattern miss)");
        if (!engine::is_resolved()) {
            return;
        }
        const sdk::Module* exe = sdk::exe();
        const auto in_exe = [&](uintptr_t a) {
            return a >= exe->base && a < exe->base + exe->size;
        };
        // Residency proof (TESTING.MD rule 1): every target must sit inside
        // the FEAR2.exe image, not merely be non-null.
        ts.check(in_exe(engine::targets().client_mgr_update), "CClientMgr::Update outside FEAR2.exe image");
        ts.check(in_exe(engine::targets().client_shell_update), "CClientShell::Update outside FEAR2.exe image");
        ts.check(in_exe(engine::targets().get_engine_hook), "cis_GetEngineHook outside FEAR2.exe image");
        ts.check(in_exe(engine::targets().p_g_pClientMgr), "&g_pClientMgr outside FEAR2.exe image");
        ts.check(in_exe(engine::targets().p_g_hMainWnd), "&hWnd outside FEAR2.exe image");
    });

    gametests::add("sdk.engine.bridge_callable", [](gametests::TestSink& ts) {
        if (!engine::resolve()) {
            ts.check(false, "engine::resolve() failed");
            return;
        }
        ts.check(engine::get_client_mgr() != nullptr,
                 "g_pClientMgr null (engine not initialized?)");
        void* hwnd_slot_value = engine::get_main_hwnd();
        ts.check(hwnd_slot_value != nullptr && IsWindow(static_cast<HWND>(hwnd_slot_value)),
                 "main window global is not a live HWND");

        // Positive evidence: the real engine getter resolves \"hwnd\" to exactly
        // the same value the raw global holds (two independent paths agree),
        // and it is a live window.
        auto hook_fn = engine::get_engine_hook();
        ts.check(hook_fn != nullptr, "cis_GetEngineHook pointer null");
        if (hook_fn == nullptr) {
            return;
        }
        void* out = nullptr;
        const int rc = hook_fn("hwnd", &out);
        ts.check(rc == 0, "cis_GetEngineHook(\"hwnd\") did not return LT_OK");
        ts.check(out == hwnd_slot_value,
                 "cis_GetEngineHook(\"hwnd\") disagrees with raw hWnd global");
        ts.check(out != nullptr && IsWindow(static_cast<HWND>(out)),
                 "cis_GetEngineHook(\"hwnd\") returned a non-window");

        // Negative evidence: a bogus name MUST be rejected (LT_ERROR != 0) and
        // MUST NOT clobber the out-param (poison value remains).
        void* poison = reinterpret_cast<void*>(static_cast<uintptr_t>(0xDEADBEEF));
        const int rc_bad = hook_fn("fear2vr_no_such_engine_hook", &poison);
        ts.check(rc_bad != 0, "unknown engine hook name was accepted");
        ts.check(poison == reinterpret_cast<void*>(static_cast<uintptr_t>(0xDEADBEEF)),
                 "unknown engine hook name clobbered the out-param");
    });

    gametests::add("sdk.engine.database_mgr", [](gametests::TestSink& ts) {
        void* mgr = engine::get_database_mgr();
        ts.check(mgr != nullptr, "LTGetIDatabaseMgr export missing or returned null");
        if (mgr == nullptr) {
            return;
        }
        const sdk::Module* db = sdk::game_database();
        const auto in_db = [&](uintptr_t a) { return a >= db->base && a < db->base + db->size; };
        // The object is the static CDatabaseMgr singleton INSIDE the dll image;
        // its first field is the vtable, also inside the image.
        uint32_t vtable = 0;
        if (!engine::safe_read32(mgr, vtable)) {
            ts.check(false, "IDatabaseMgr object unreadable");
            return;
        }
        ts.check(in_db(reinterpret_cast<uintptr_t>(mgr)), "IDatabaseMgr object outside gamedatabase.dll image");
        ts.check(in_db(vtable), "IDatabaseMgr vtable outside gamedatabase.dll image");
    });

    gametests::add("hooks.hot_path_round_trip", [](gametests::TestSink& ts) {
        // Marquee contract: attach a SafetyHook inline detour to the REAL
        // per-frame engine function (CClientShell::Update), watch it fire, then
        // retire it and watch it STOP -- against the live game, no restart.
        if (!engine::resolve()) {
            ts.check(false, "engine::resolve() failed");
            return;
        }
        const uintptr_t target = engine::targets().client_shell_update;

        static std::atomic<uint32_t> s_tick_count{0};
        struct Detour {
            // x86 __thiscall (ecx=this) -> __fastcall shim (edx dummy).
            static int __fastcall tick(void* _this, void* /*edx*/) {
                s_tick_count.fetch_add(1, std::memory_order_relaxed);
                auto* hook = hooks().find("clientshell_update_tick");
                if (hook == nullptr || !*hook) {
                    return 0; // retired while in-flight: skip original call
                }
                return hook->original<int(__fastcall*)(void*, void*)>()(_this, nullptr);
            }
        };

        const uint8_t original_first_byte = *reinterpret_cast<const uint8_t*>(target);
        ts.check(original_first_byte == 0x55, "hook target prologue unexpected (not push ebp)");

        ts.check(hooks().install("clientshell_update_tick", reinterpret_cast<void*>(target),
                                 reinterpret_cast<void*>(&Detour::tick)),
                 "safetyhook install on CClientShell::Update failed");
        if (hooks().find("clientshell_update_tick") == nullptr) {
            return; // install failed; cannot proceed safely
        }

        // (a) Prove it fires: bounded wait for >0 ticks.
        bool fired = false;
        for (int i = 0; i < 200 && !fired; ++i) { // 2s budget
            fired = s_tick_count.load(std::memory_order_relaxed) > 0;
            if (!fired) {
                Sleep(10);
            }
        }
        const uint32_t c_fired = s_tick_count.load(std::memory_order_relaxed);
        ts.check(fired, "hook never fired -- mapping wrong or shell not updating");

        // (b) Retire it and prove silence: the count must FREEZE.
        ts.check(hooks().retire_one("clientshell_update_tick"), "retire_one failed");
        const uint32_t c1 = s_tick_count.load(std::memory_order_relaxed);
        Sleep(250);
        const uint32_t c2 = s_tick_count.load(std::memory_order_relaxed);
        ts.check(c2 == c1, "hook kept firing after retire (graceful removal broken)");

        // (c) Byte restore proof: the game's original prologue first byte is back.
        ts.check(*reinterpret_cast<const uint8_t*>(target) == original_first_byte,
                 "original prologue not restored after retire");
        LOGX("[test] hot_path_round_trip: %u ticks while armed, frozen at %u after retire", c_fired, c2);
    });

    // Health callback: atomic loads only -- must be callable on the server
    // thread at any time, including mid-shutdown.
    cmdsrv::Handlers handlers;
    handlers.health = [this]() -> std::string {
        std::string s = "\"pid\":" + std::to_string(GetCurrentProcessId());
        s += ",\"state\":\"";
        s += m_shutting_down.load() ? "shutting_down" : "running";
        s += "\",";
        s += "\"sdk_ready\":";
        s += m_sdk_ready ? "true" : "false";
        s += ",\"hooks\":" + std::to_string(hooks().count());
        s += ",\"hooks_retired\":" + std::to_string(hooks().retired_count());
        s += ",\"tests\":" + std::to_string(gametests::count());
        return s;
    };
    handlers.test = [this]() -> std::string {
        if (m_shutting_down.load()) {
            return "{\"pass\":0,\"fail\":0,\"failures\":[{\"name\":\"*\",\"error\":\"framework shutting down\"}]}";
        }
        return gametests::run_all();
    };

    if (!cmdsrv::start(m_ipc_port, std::move(handlers))) {
        LOGX("[framework] IPC server failed to start on port %d (in use?)", m_ipc_port);
        return false;
    }
    LOGX("[framework] IPC command server on http://127.0.0.1:%d", m_ipc_port);
    LOGX("[framework] initialized; %zu tests registered", gametests::count());
    return true;
}

bool Framework::shutdown() {
    if (m_shutting_down.exchange(true)) {
        LOGX("[framework] shutdown() re-entry -- refusing");
        return false; // already retired; a second pass would double-retire
    }

    // 2. Stop IPC: joins the socket thread, so afterwards no handler is
    //    executing and no new /unload or /test can arrive.
    cmdsrv::stop();
    LOGX("[framework] IPC stopped");

    // 3. Retire hooks so no new detour fires during the quiescence scan.
    //    Fail-closed: a failed disable means the DLL must stay mapped.
    const bool hooks_retired = hooks().retire();

    // 4-5. Prove quiescence: with every other thread suspended, no EIP sits in
    //    our module. Hook InlineHook objects leak on success (straggler-in-
    //    trampoline safety; see Hooks.hpp) -- whether we unmap or stay dormant,
    //    there is nothing here left to free safely.
    const uintptr_t base = reinterpret_cast<uintptr_t>(m_self);
    const size_t size = sdk::module_size(static_cast<HMODULE>(m_self));

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
