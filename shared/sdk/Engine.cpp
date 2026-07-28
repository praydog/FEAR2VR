#include "Engine.hpp"

#include <windows.h>

#include <utility/Seh.hpp>

#include "Log.hpp"
#include "Modules.hpp"

namespace sdk {

// cis_GetEngineHook -- FEAR2_dump.exe 0x46AA1E (__stdcall, `retn 8` verified):
//   68 ["hwnd"] | FF 74 24 08 | E8 [rel32] | 85 C0 | 59 59 | 75 10 |
//   8B 44 24 08 | 8B 0D [hWnd] | 89 08 | 33 C0 | EB 30
// NOTE: the mov ecx,[imm32] operand at +0x1A is &hWnd (main window global).
static constexpr const char* kGetEngineHook =
    "68 ? ? ? ? FF 74 24 08 E8 ? ? ? ? 85 C0 59 59 75 10 8B 44 24 08 8B 0D ? ? ? ? 89 08 33 C0 EB 30";

using GetEngineHookFn = int(__stdcall*)(const char* name, void** out_data);

namespace {

// Own function scope: __try cannot share a function with static-local
// initialization (MSVC C2712), hence no lambda here.
uintptr_t resolve_hwnd_slot() {
    constexpr uint32_t kGetEngineHook_HWndOperand = 0x1A;
    const uintptr_t fn = Engine::get_engine_hook_fn();
    if (fn == 0) {
        return 0;
    }
    uintptr_t slot = 0;
    KANANLIB_SEH_TRY {
        slot = *reinterpret_cast<uintptr_t*>(fn + kGetEngineHook_HWndOperand);
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        LOGX("[sdk] crashed reading &hWnd operand");
        return 0;
    }
    return slot;
}

int call_engine_hook(uintptr_t fn, const char* name, void** out) {
    int rc = -1;
    KANANLIB_SEH_TRY {
        rc = reinterpret_cast<GetEngineHookFn>(fn)(name, out);
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    return rc;
}

} // namespace

uintptr_t Engine::get_engine_hook_fn() {
    static const uintptr_t s_fn = Modules::get().scan_exe(kGetEngineHook, "cis_GetEngineHook");
    return s_fn;
}

int Engine::get_engine_hook(const char* name, void** out) {
    const uintptr_t fn = get_engine_hook_fn();
    if (fn == 0 || name == nullptr || out == nullptr) {
        return -1; // not LT_ERROR; "our side could not make the call"
    }
    return call_engine_hook(fn, name, out);
}

uintptr_t Engine::main_hwnd_slot() {
    static const uintptr_t s_slot = resolve_hwnd_slot();
    return s_slot;
}

void* Engine::main_hwnd() {
    const uintptr_t slot = main_hwnd_slot();
    if (slot == 0) {
        return nullptr;
    }
    void* hwnd = nullptr;
    KANANLIB_SEH_TRY {
        hwnd = *reinterpret_cast<void**>(slot);
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    return hwnd;
}

} // namespace sdk
