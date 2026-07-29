#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

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

    // The engine's GLOBAL FORCE vector, copied out by the engine's own getter (dump
    // 0x405C39, __stdcall(float* out), reading CClientMgr+0x1440).
    //
    // Live it reads (0, -980, 0) and its three neighbouring floats are zero, so this
    // is gravity in the engine's units -- 980 of them per second squared, downward on
    // -Y. Named for the concept rather than for gravity because the getter copies a
    // free vector out of a mutable field: the engine can point it anywhere, and the
    // reference SDK carries a matching per-object m_GlobalForceOverride, which only
    // makes sense against a global that is not definitionally gravity.
    //
    // Useful to a mod that has to agree with the engine about "down" -- a VR comfort
    // horizon, a thrown object's arc, or anything deciding which way up the player is.
    // Reading it beats hardcoding -980 because a level or a script may change it.
    struct ForceVector {
        float x;
        float y;
        float z;
    };

    // nullopt when the getter could not be located or the call faulted.
    static std::optional<ForceVector> global_force();

    // ---- CONSOLE VARIABLES -------------------------------------------------------
    //
    // The engine's tunables, by name. Live there are 192 of them, including
    // HDR_ToneMapExponent, HDR_Blur, MotionBlur_PassCount, PhysicsBulletForce,
    // PhysicsExplosionForce, BodyCapRadius and AddAmbientLightHigh -- which makes this
    // the widest read surface a mod has without hooking anything.
    //
    // Reached the way the engine reaches it: a 128-bucket hash table on CClientMgr,
    // keyed by the SAME case-insensitive hash already mapped for skeleton node names
    // (reimplementing it reproduced all 191 stored hashes exactly). Lookup is therefore
    // case-insensitive, as the engine's own is.
    struct ConVar {
        std::string name;
        // The engine's float. GetSConValueFloat returns exactly this.
        float value;
        // The text form. Usually a formatted copy of `value`, but some entries carry no
        // string at all while `value` is meaningful -- so prefer `value` unless the text
        // is specifically wanted. Empty when the entry has none.
        std::string text;
    };

    // Case-insensitive, matching the engine. nullopt when no such variable exists.
    static std::optional<ConVar> console_var(const char* name);

    // Every variable in the table, in bucket order. The walk is bounded and
    // cycle-checked: it reads a live structure the engine mutates.
    static std::vector<ConVar> console_vars();
};

} // namespace sdk
