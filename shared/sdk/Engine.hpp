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
    // The engine's tunables, by name, out of a 128-bucket hash table keyed by the SAME case-insensitive
    // hash already mapped for skeleton node names (reimplementing it reproduced all 191 stored hashes
    // exactly). Lookup is therefore case-insensitive, as the engine's own is.
    //
    // THERE ARE TWO OF THESE TABLES, and an earlier version of this comment claimed the CClientMgr one was
    // "the widest read surface a mod has". It is the smaller of the two:
    //
    //     CClientMgr + 0xC0     192 entries   HDR_Blur, Spawn_Debug, HealScale, MotionBlur_PassCount
    //     console source        535 entries   ScreenWidth, ApplyWorldOffset, WeaponLagReversed
    //
    // Both hold LTConVar records of identical shape and both are searched by the same ConVarTable_Find --
    // the table base arrives in ECX, which is why the decompiler shows the stack argument as unused and why
    // one finder appears to serve one table. The populations OVERLAP but neither contains the other
    // (PRBForceCP is in both; ScreenWidth only in the source table; HDR_Blur only in CClientMgr's).
    //
    // So a consumer asking "does this build have setting X" must ask BOTH, which is what console_var does.
    struct ConVar {
        // THE RECORD'S ADDRESS, which is what makes a variable writable: its first field IS the float the
        // engine reads, so a consumer that wants to change a tunable stores four bytes here rather than
        // going through a setter. Also the identity a caller should cache -- the records outlive lookups,
        // while a name search does not.
        //
        // This is the same pointer ILTClient's public lookup returns (see sdk::Console for that entry
        // point): slot 69 hands back an LTConVar* and slot 71 is nothing but `*(float*)record`.
        uintptr_t address{};

        std::string name;
        // The engine's float. GetSConValueFloat returns exactly this.
        float value;
        // The text form. Usually a formatted copy of `value`, but some entries carry no
        // string at all while `value` is meaningful -- so prefer `value` unless the text
        // is specifically wanted. Empty when the entry has none.
        std::string text;
    };

    // Case-insensitive, matching the engine. Searches the console source table first and then CClientMgr's,
    // because a name can be in either and only the pair covers the build. nullopt when it is in neither.
    static std::optional<ConVar> console_var(const char* name);

    // CClientMgr's table (192 live), in bucket order. The walk is bounded and cycle-checked: it reads a
    // live structure the engine mutates.
    static std::vector<ConVar> console_vars();

    // The console SOURCE table (535 live) -- the larger population, and the one the engine's own public
    // ILTClient lookup searches. Same record type, same walk, different base.
    static std::vector<ConVar> console_source_vars();

    // Write a variable's float, by storing into the record's first field -- which is what the engine reads.
    // Returns false when the name is in neither table or the store faulted.
    //
    // THIS CHANGES GAME BEHAVIOUR, and the two caveats are real. The engine samples most of these once per
    // frame, so a write takes effect on the next one; but a few are read only at level load, and writing
    // those does nothing until the next load. And a VarTrack on the game side caches nothing -- it reads the
    // record each time -- so a write is visible to the game without telling it anything.
    static bool write_console_var(const char* name, float value);

    // ---- THE CAMERA AND COMFORT TUNABLES ---------------------------------------------
    //
    // The console SOURCE table is where CPlayerCamera::Init puts the entire camera tuning surface, and for a
    // VR mod this is the comfort layer: head bob, view sway, damage kick, weapon lag and the FOV all live
    // here as plain floats with no engine call needed to change them.
    //
    // FovY IS THE FIELD OF VIEW, and it is worth stating plainly because this project previously recorded
    // that "no FOV field exists anywhere on the camera class, and FEAR2.exe contains no fov string". Both
    // were true and both were about the ENGINE. The FOV is a game-side console variable.
    //
    // The catalogue below is curated rather than complete -- 535 variables exist and most are irrelevant --
    // and every entry was read live. `vr_suggested` is the value a VR consumer most likely wants, and it is
    // ADVICE, not a measurement: nothing here has been verified to produce a comfortable result, only to be
    // the knob that controls the named behaviour.
    struct CameraTunable {
        const char* name;
        float live_default;  // read live from this build
        float vr_suggested;  // advice for a stereo consumer; see the caveat above
        const char* what;
    };
    static const std::vector<CameraTunable>& camera_tunables();

    // Both tables, source first. What a consumer wanting "every tunable this build has" should call; names
    // present in both appear twice, deliberately, since the two records are distinct objects.
    static std::vector<ConVar> all_console_vars();
};

} // namespace sdk
