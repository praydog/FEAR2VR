#pragma once

#include <cstdint>
#include <optional>

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

    // The engine's own CLIENT CLOCK, read through the two accessors the engine
    // exports for it rather than by re-reading its fields (dump 0x406DED returns the
    // double, 0x406DFC the millisecond count; both are 15-byte leaf functions that
    // load g_pClientMgr, follow +0x13F4, and read one field).
    //
    // TWO THINGS A CONSUMER MUST KNOW, both measured rather than assumed:
    //
    // 1. THIS CLOCK STOPS. Sampled twice across a full second of wall time with the
    //    game sitting at a menu, both values were byte-identical (127286 ms /
    //    127.286 s). It is game time, not wall time, so anything that must keep
    //    running while the game is paused -- head tracking above all -- MUST NOT be
    //    driven from it. Anything that should freeze with the game should be.
    //
    // 2. The two fields are the SAME instant in different units: milliseconds and
    //    seconds, related by exactly 1000. That is worth knowing because it means
    //    picking one is a matter of resolution and integer-vs-float, not of meaning,
    //    and because it gives a caller a free consistency check.
    struct ClientTime {
        double seconds;
        uint32_t milliseconds;
    };

    // nullopt when either accessor could not be located or the call faulted.
    static std::optional<ClientTime> client_time();
};

} // namespace sdk
