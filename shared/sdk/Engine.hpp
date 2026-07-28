#pragma once

#include <cstdint>

// Engine-level (binary-global) entry points of the FEAR2 client engine,
// i.e. the things that belong to FEAR2.exe itself rather than to any object.
namespace sdk {

class Engine {
public:
    // cis_GetEngineHook(const char* name, void** out) -- the engine's
    // name->variable bridge (lithtech winclientde_impl.cpp). Known names in
    // the FEAR2 build (dump 0x46AA1E): "hwnd", "cshell_hinstance".
    // Returns the engine's LTRESULT code (0 == LT_OK). `out` untouched on
    // unknown names (the engine function returns LT_ERROR and never writes).
    static int get_engine_hook(const char* name, void** out);

    // Runtime address of cis_GetEngineHook (dump 0x46AA1E; __stdcall, retn 8).
    static uintptr_t get_engine_hook_fn();

    // Engine main window HWND (value of the hWnd global; dump 0x6E4724).
    static void* main_hwnd();

    // Address of the hWnd global slot itself (for diagnostics/tests).
    static uintptr_t main_hwnd_slot();
};

} // namespace sdk
