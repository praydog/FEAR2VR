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

// ClientTime_GetSeconds -- FEAR2_dump.exe 0x406DED, and
// ClientTime_GetMilliseconds -- 0x406DFC. Both are 15-byte leaves:
//   A1 [g_pClientMgr] | 8B 80 F4 13 00 00 | <load> | C3
// The tail is what distinguishes them: `fld qword ptr [eax+38h]` (DD 40 38) for the
// double, `mov eax, [eax+30h]` (8B 40 30) for the millisecond count. The global's
// address is wildcarded; the +0x13F4 displacement is not, because that is the field
// identifying which manager member is being followed.
static constexpr const char* kClientTimeSeconds =
    "A1 ? ? ? ? 8B 80 F4 13 00 00 DD 40 38 C3";
static constexpr const char* kClientTimeMillis =
    "A1 ? ? ? ? 8B 80 F4 13 00 00 8B 40 30 C3";

// No arguments, so __cdecl and __stdcall are indistinguishable here.
using GetTimeSecondsFn = double(*)();
using GetTimeMillisFn = uint32_t(*)();

// CClientMgr_GetGlobalForce -- FEAR2_dump.exe 0x405C39, __stdcall(float* out),
// `retn 4`:
//   A1 [g_pClientMgr] | 8B 4C 24 04 | 05 40 14 00 00 |
//   D9 00 D9 19 | D9 40 04 D9 59 04 | D9 40 08 33 C0 D9 59 08 | C2 04 00
// Three fld/fstp pairs copying mgr+0x1440..+0x1448 into the caller's buffer, with
// `xor eax, eax` for the LT_OK return wedged between the last load and store.
static constexpr const char* kGetGlobalForce =
    "A1 ? ? ? ? 8B 4C 24 04 05 40 14 00 00 D9 00 D9 19 D9 40 04 D9 59 04 D9 40 08 33 C0 "
    "D9 59 08 C2 04 00";

using GetGlobalForceFn = int(__stdcall*)(float* out);

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

// Both engine calls inside ONE guard, so the pair is read from one instant. Reading
// them under separate guards would let a frame boundary fall between and produce a
// seconds/milliseconds mismatch that never existed.
bool call_client_time(uintptr_t fn_s, uintptr_t fn_ms, double* out_s, uint32_t* out_ms) {
    bool ok = false;
    KANANLIB_SEH_TRY {
        *out_s = reinterpret_cast<GetTimeSecondsFn>(fn_s)();
        *out_ms = reinterpret_cast<GetTimeMillisFn>(fn_ms)();
        ok = true;
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return ok;
}

// The engine writes THREE floats through the pointer we hand it, so the buffer is
// sized here and not by the callee's word. A short buffer would be a stack overwrite
// the guard could not catch, because the write would be perfectly legal.
bool call_global_force(uintptr_t fn, float out[3]) {
    bool ok = false;
    KANANLIB_SEH_TRY {
        reinterpret_cast<GetGlobalForceFn>(fn)(out);
        ok = true;
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return ok;
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

std::optional<Engine::ClientTime> Engine::client_time() {
    static const uintptr_t s_sec =
        Modules::get().scan_exe(kClientTimeSeconds, "ClientTime_GetSeconds");
    static const uintptr_t s_ms =
        Modules::get().scan_exe(kClientTimeMillis, "ClientTime_GetMilliseconds");
    if (s_sec == 0 || s_ms == 0) {
        return std::nullopt;
    }
    ClientTime out{};
    if (!call_client_time(s_sec, s_ms, &out.seconds, &out.milliseconds)) {
        return std::nullopt;
    }
    return out;
}

std::optional<Engine::ForceVector> Engine::global_force() {
    static const uintptr_t s_fn =
        Modules::get().scan_exe(kGetGlobalForce, "CClientMgr_GetGlobalForce");
    if (s_fn == 0) {
        return std::nullopt;
    }
    float v[3] = {0.0f, 0.0f, 0.0f};
    if (!call_global_force(s_fn, v)) {
        return std::nullopt;
    }
    return ForceVector{v[0], v[1], v[2]};
}

} // namespace sdk
