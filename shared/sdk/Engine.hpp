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

    // ---- THE CAMERA'S CACHED TUNABLES, AND THE HEAD-BOB GRID -------------------------
    //
    // The camera does not look its tunables up by name each frame. One initialiser in gameclient.dll
    // (PlayerCamera_CacheTunables) resolves 67 console variables ONCE and caches each as a
    // {LTConVar* record, ILTClient* owner} PAIR in module data. It reaches them through ILTClient vtable slot 69
    // to find and slot 73 to create-with-default -- the same two slots sdk::Console documents from the other
    // side, which is how the idiom was recognised.
    //
    // WHY A CONSUMER WANTS THE CACHE RATHER THAN A LOOKUP: the cached pointer IS the record the camera reads, so
    // writing its float is what the camera sees, with no hash search and no engine call. console_var(name) finds
    // the same record by searching 535 entries; this finds it with one load.
    //
    // THE 60 HEAD-BOB VARIABLES ARE A GRID, not a list, and the cache addresses prove it: five parallel blocks
    // of twelve pairs, each block one PARAMETER, each of the twelve a (channel, axis):
    //
    //             WaveMin    WaveMax    Amp        AmpOffset  Flags
    //     base    0x1FFB00   0x1FFB60   0x1FFBC0   0x1FFC20   0x1FFC80      stride 8 within a block
    //     index   channel * 3 + axis, channels CameraOffset, CameraRotation, WeaponOffset, WeaponRotation
    //
    // 4 channels x 3 axes x 5 parameters = 60, plus 7 standalone = 67.
    //
    // FOR VR THIS IS THE COMFORT SURFACE. Head bob is the canonical VR nausea source, and this is the complete
    // set of knobs for it -- separately for the CAMERA and the WEAPON, and separately for translation and
    // rotation, so view bob can be removed while the weapon keeps moving.
    //
    // THE NAMES ARE DERIVED, WHICH MAKES THE MAPPING SELF-CHECKING. head_bob_var() composes
    // "HeadBob<Channel><Axis><Parameter>" and the suite requires the composed name to resolve, through the
    // console tables, to the SAME record the cache holds at the slot the grid formula computes. A wrong channel
    // order or axis order would still produce 60 valid names -- and they would land on the wrong records.
    enum class BobChannel : unsigned {
        CameraOffset = 0,
        CameraRotation = 1,
        WeaponOffset = 2,
        WeaponRotation = 3,
    };

    enum class BobParam : unsigned {
        WaveMin = 0,
        WaveMax = 1,
        Amp = 2,
        AmpOffset = 3,
        Flags = 4,
    };

    struct CachedVar {
        std::string name;
        uintptr_t cache_offset{};  // gameclient-relative address of the {record, owner} pair
        uintptr_t record{};        // the live LTConVar*, 0 when the cache slot is unset
        uintptr_t owner{};         // the ILTClient the camera registered through; shared by all 67
    };

    // All 67, in the initialiser's order: the seven standalone tunables then the grid. Empty when gameclient is
    // not mapped. A slot whose record is 0 is still reported -- that is a real state, meaning the initialiser
    // has not run yet.
    static std::vector<CachedVar> camera_tunable_cache();

    // One by name, case-sensitive as the cache table spells it.
    static std::optional<CachedVar> camera_tunable(std::string_view name);

    // One grid cell. axis is 0=X, 1=Y, 2=Z; nullopt for an axis out of range.
    static std::optional<CachedVar> head_bob_var(BobChannel channel, unsigned axis, BobParam param);

    // The composed console-variable name for a grid cell -- exposed so a consumer can cross-check it against
    // console_var(), or log what it is about to change.
    static std::string head_bob_var_name(BobChannel channel, unsigned axis, BobParam param);

    // The float the camera reads, straight from the cached record. nullopt when the slot is unset or the read
    // faulted.
    static std::optional<float> read_cached(const CachedVar& var);

    // Store into the cached record's float, which is the field the engine reads. False when the slot is unset or
    // the write faulted. This changes engine-owned data with no notification, exactly as write_console_var does.
    static bool write_cached(const CachedVar& var, float value);

    // Do all 67 cache slots agree with what the console tables hold for the same names? The cross-check that
    // ties the cache to the tables: two independent routes to one record. Returns {agreeing, populated}.
    static std::pair<size_t, size_t> camera_tunable_agreement();

    // ---- EVERY CACHED CONSOLE VARIABLE IN THE GAME DLL, BY DISCOVERY -----------------
    //
    // The camera's 67 tunables are not special. THE WHOLE GAME DLL CACHES ITS CONSOLE VARIABLES THIS WAY: a
    // {LTConVar* record, ILTClient* owner} pair in .data per variable, resolved once and then read with a single
    // load. Live there are 474 such pairs in 62 contiguous runs, the camera's being one run of 61.
    //
    // WHY DISCOVERY RATHER THAN A TABLE: a hardcoded list of 474 offsets would be unverifiable and would rot.
    // These are found by SCANNING gameclient's .data for the pattern -- second word equal to the one ILTClient
    // every pair shares, first word a plausible LTConVar whose name reads as an identifier. The section bounds
    // come from the PE headers at runtime and the owner from the interface registry, so nothing here is a
    // literal address.
    //
    // AND THE RESULT IS CHECKABLE, which is the point: every discovered record must be findable in the console
    // tables BY ITS OWN NAME, and the address the tables give must equal the cached pointer. That is a data scan
    // and a hash-table walk agreeing -- two routes that share no code.
    //
    // WHAT IT IS FOR: this is the game's entire per-frame tunable surface with a writable pointer for each, and
    // for VR specifically it includes DisableCameraShake, DisableOverlayFX and the head-bob grid. A consumer
    // changes one with a four-byte store through read_cached/write_cached, no lookup and no engine call.
    //
    // The scan is bounded and reports what it found; it never assumes a count.
    static std::vector<CachedVar> cached_console_vars(size_t limit = 4096);

    // One by exact name, from the discovered set.
    static std::optional<CachedVar> find_cached_var(std::string_view name);

    // The ILTClient every pair's owner word holds, resolved through the registry. 0 when unavailable, which is
    // what makes discovery return nothing rather than scanning for a null.
    static uintptr_t cached_var_owner();

    static constexpr size_t kCameraTunableCount = 67;
    static constexpr size_t kHeadBobChannels = 4;
    static constexpr size_t kHeadBobAxes = 3;
    static constexpr size_t kHeadBobParams = 5;
};

} // namespace sdk
