#pragma once

#include <cstdint>
#include <optional>
#include <string>


// Forward declaration rather than the generated header: this class only hands out
// pointers to LTObject, so a caller that wants to dereference one includes it itself.
namespace regenny {
class LTObject;
}

namespace sdk {

// Engine-side CClientShell (CNetHandler subclass in the LithTech sources).
// Reached through CClientMgr::client_shell() (m_pClientShellDE at +0x1434).
//
// regenny::CClientShell now exists and this class reads through it. An earlier version
// of this comment listed the three fields CClientShell::Update touches -- a byte at
// +0x69, a 0xFFFF-sentineled uint16[4] at +0x60, and a resolved uint32[4] at +0x6C --
// and said none had a confident semantic yet. Two of the three do now: the pair at
// +0x60/+0x6C is the LOCAL PLAYER, by handle and by pointer. See local_player() below.
class CClientShell {
public:
    // Live instance via the client manager. nullptr when no shell exists
    // (before engine init / after world teardown).
    static CClientShell* get();

    // Runtime address of CClientShell::Update (dump 0x40CC5E) -- called every
    // frame by CClientMgr::Update once a shell exists; our frame hook anchor.
    //
    // THIS IS THE ENGINE-SIDE WRAPPER, and knowing that matters when choosing where to hook. Its
    // body dispatches the GAME DLL's per-frame entry points -- IClientShell slots 2, 4 and 3, in
    // that order, which are PreUpdate, Update and PostUpdate on gameclient.dll's CGameClientShell --
    // through the global g_pIClientShell, not through any field of this class.
    //
    // So a detour here runs BEFORE and AFTER the game's whole frame, which is what the framework
    // wants. A mod needing to sit inside the game's own update, between its subsystems, wants one of
    // those three slots instead. See the IClientShell slot map in reversing/fear2.genny for how the
    // indices were established -- they do NOT match the reference SDK's declaration order.
    static uintptr_t update_fn();

    // ---- the shell's TWO CLOCKS ------------------------------------------------
    //
    // The engine exports a 24-byte accessor for each (dump 0x407490 and 0x406BDC);
    // both load g_pClientMgr, follow +0x1434, and return 0.0 if no shell exists. They
    // read two different doubles, and the difference between them was MEASURED.
    //
    // Sampled across 1.5 s of wall time with the game paused at a menu:
    //
    //   shell+0x08   1186.6610  1186.6610  1186.6610   advanced 0.0000  (ratio 0.000)
    //   shell+0x10  58816.4770 58816.9790 58817.9840   advanced 1.5070  (ratio 1.005)
    //
    // So one clock stops with the game and the other does not:
    //
    //   * game_time_seconds() STOPS when the game is paused. Gameplay timers and
    //     anything that should pause with the player belong here.
    //   * real_time_seconds() keeps running regardless.
    //
    // BUT READ THE NEXT PARAGRAPH BEFORE USING THE REAL ONE FOR ANYTHING SMOOTH.
    // It is real time and it is accurate in aggregate, but it is QUANTISED: polled
    // 58 times over 2.97 s it produced only 6 distinct values, stepping by 0.5010 to
    // 0.5060 seconds. That is a ~2 Hz clock. It is fine for "how many seconds since
    // X" and useless for anything per-frame -- driving view smoothing or tracking
    // from it would produce half-second stutter, which is worse than the frozen
    // clock it was reached for.
    //
    // FOR PER-FRAME WALL TIME use CClientMgr::last_sample_time_ms() instead: over the
    // same window it produced 57 distinct values stepping 46-54 ms, i.e. one update
    // per engine frame at millisecond resolution. That is the clock a VR mod wants.
    // This one answers a different, coarser question.
    //
    // nullopt when no shell exists, which the engine's own accessors cannot express:
    // they return 0.0 both for "no shell" and for "time is zero", and those are
    // genuinely different answers right after a load.
    static std::optional<double> game_time_seconds();
    static std::optional<double> real_time_seconds();

    // The LOCAL CLIENT IDs, with the engine's own bound and sentinel.
    //
    // CLTClient::GetLocalClientID (dump 0x408D59) reads a 4-entry byte table at
    // shell+0x24 where 0xFF means "no client in this slot", and rejects an index of 4
    // or more. Both the table's length and the sentinel are the engine's, not
    // guesses -- the function logs its own name when the index is out of range.
    //
    // Why a mod cares: this is the first step in "which of these objects is ME".
    // The engine identifies clients by these ids, so a mod that needs the local
    // player -- which for a VR mod is nearly everything -- starts here rather than
    // by guessing at object names or positions. Live, slot 0 holds id 0 and the other
    // three are 0xFF, which is what single-player looks like.
    //
    // nullopt for an out-of-range index, a missing shell, or an empty slot; the three
    // are deliberately not distinguished, because a caller can do nothing different
    // about them.
    static std::optional<uint8_t> local_client_id(unsigned index);

    // How many local client slots are filled (0..4). A split-screen or multi-client
    // session would report more than one; single-player reports 1.
    static std::optional<unsigned> local_client_count();

    // The shell's frame-interval constant: bit-identical 1/60 across samples, so it
    // is CONFIGURED rather than measured. Exposed because a mod wanting the engine's
    // notion of a target frame length should read it rather than assume 60Hz, but do
    // not mistake it for a measured delta -- it does not move.
    static std::optional<float> frame_interval_seconds();

    // ---- THE LOCAL PLAYER ---------------------------------------------------------
    //
    // The object a VR mod is anchored to, reachable directly instead of by searching.
    // Earlier passes found the player's viewmodel by enumerating every model and
    // matching a path substring; the shell simply names it.
    //
    // The shell keeps one slot per local client -- four of them, the same width and the
    // same index space as local_client_id() above -- in TWO parallel forms: the object's
    // HANDLE, and the already-resolved POINTER. Live slot 0 holds handle 7394 and the
    // pointer that handle resolves to, and the object is an OT_MODEL whose .mdl is
    // `char\player\player\fp_playerm05.mdl` with dims (40, 95, 40) -- a player-sized
    // collision box. Slots 1..3 are empty, which is what single-player looks like.
    //
    // WHICH FORM TO USE: the handle for anything that goes through an ILT* entry point
    // (those take an HOBJECT), the pointer for a direct read through this SDK. They are
    // the same object; the pair exists so a caller does not have to convert.
    //
    // THE POINTER IS REFRESHED EVERY FRAME and this function VERIFIES the pair before handing it
    // back -- it resolves the handle through the manager and returns nullopt if that disagrees with
    // the stored pointer, or if the manager is absent and the check cannot be made at all.
    //
    // THAT IS A BEST-EFFORT CONSISTENCY CHECK, NOT A FRESHNESS GUARANTEE. The two reads are not
    // atomic, and nothing stops the object being unregistered or freed the instant after they agree
    // -- so the returned pointer carries the same lifetime caveat as every other LTObject* in this
    // SDK. What the check buys is refusing a pair that is ALREADY torn; it cannot promise one that
    // stays valid. Use it on the engine thread, or use it immediately and do not store it across a
    // frame; the HANDLE is the durable identity. CClientShell::Update ends by looping the four slots and
    // calling CClientShell_ResolveLocalPlayerObject on each, which resolves the HANDLE through the
    // manager and stores the result. So the handle is the durable identity and the pointer is a
    // per-frame resolution of it.
    //
    // An empty slot is handle 0xFFFF, which is the resolver's own sentinel -- not zero.
    struct LocalPlayer {
        const regenny::LTObject* object;
        uint16_t handle;
    };

    // nullopt for an out-of-range index, a missing shell, an empty slot, a handle that does not
    // resolve to the stored pointer, or an absent CClientMgr -- which is the case where the pair
    // CANNOT be verified, and is therefore refused rather than trusted. None of them are
    // distinguished, because a caller can do nothing different about any of them.
    static std::optional<LocalPlayer> local_player(unsigned index);

    // A DIAGNOSTIC, not the path a consumer needs: reports whether the raw slot's handle and pointer
    // agree. local_player() already REFUSES a disagreeing pair, so a normal caller never has to ask
    // -- this exists to observe how often the shell's per-frame refresh is caught mid-flight, which
    // local_player() deliberately hides by failing closed.
    //
    // nullopt when the slot is empty or either side cannot be read; false means they disagree.
    static std::optional<bool> local_player_raw_pair_agrees(unsigned index);

    // How many local player slots are filled (0..4). Single-player reports 1.
    static std::optional<unsigned> local_player_count();
};

// ---- THE GAME DLL'S SHELL --------------------------------------------------------------
//
// CClientShell above is the ENGINE's per-frame driver. This is the other side of that call: the
// IClientShell implementation living in gameclient.dll, whose class calls itself
// "CGameClientShell". CClientShell::Update dispatches three of its vtable slots every frame, and
// those are the hook sites for a mod that needs to run INSIDE the game's own frame rather than
// bracketing it.
//
// WHY THE SLOT MAP IS TRUSTWORTHY: it is not taken from the reference SDK's declaration order,
// which does not line up. Slot 1 is IBase::_InterfaceImplementation and returns the literal
// "CGameClientShell", so slot 0 is an implementation-only leading slot the published interface does
// not contain -- which is what fixes the +2 offset. Corroborated by slot 2 being a single `retn`
// (the reference notes PreUpdate exists for organisation only) and slot 4 being much the largest.
//
// WHAT available() DOES AND DOES NOT RE-CHECK, precisely. It calls slot 1 and compares the string,
// which is safe (a pure return of a constant) and re-verifies WHERE THE INTERFACE STARTS: the string
// proves slot 1 is IBase's single virtual, so slot 0 is implementation-only and the published
// interface begins at slot 2. That is the +2 anchor.
//
// It does NOT by itself re-check WHICH of slots 2/3/4 is which -- the identity string and module
// containment would all still hold if those three were reordered. For that, see
// slots_match_mapped_shapes() below, which checks each one's prologue.
class GameClientShell {
public:
    // Slot 1's string, which is the interface's own name for its implementation. nullopt when the
    // interface is unresolved or a read faulted.
    static std::optional<std::string> implementation_name();

    // Does this build match the mapped slot layout? True only when slot 1 reports
    // "CGameClientShell". Check once at startup; every accessor below already checks it.
    static bool available();

    // ---- PER-FRAME HOOK ANCHORS ----------------------------------------------
    //
    // Runtime addresses of the three slots CClientShell::Update calls, in call order:
    // PreUpdate (slot 2), Update (slot 4), PostUpdate (slot 3). All inside gameclient.dll.
    //
    // 0 when unavailable, so a caller cannot accidentally hook a null or an engine-side address.
    // NOTE PreUpdate is EMPTY in this build -- a single `retn` -- so hooking it gets you a
    // reliable per-frame callback with nothing of the game's own to preserve.
    static uintptr_t pre_update_fn();

    // Does PreUpdate RETURN IMMEDIATELY? True when its first byte is `retn` (0xC3).
    //
    // That is what the byte proves and all it proves -- the ENTRY POINT returns at once. It says
    // nothing about bytes further in, which may be unreachable code, padding or another block
    // entirely; this is not a claim that the function is one byte long.
    //
    // Still useful to a consumer, and that is the point: a detour on a function whose entry returns
    // immediately has no game behaviour to preserve, so it can do its work and return without
    // calling the original. As validation it is weaker than it looks -- it distinguishes slot 2 from
    // a slot holding real work, but not from another empty slot.
    //
    // NOT AN INVARIANT -- it is a property of the shipped game code, which the reference SDK
    // explains ("the only benefit to having code in PreUpdate() is purely organizational"). A build
    // that fills it in would report false without anything being wrong.
    static bool pre_update_is_empty();

    // Do slots 2, 3 and 4 still have the PROLOGUES they had when this map was made? This is the
    // per-slot evidence available at runtime, and what makes a reordering of the three detectable
    // rather than silently accepted:
    //
    //   slot 2  `retn` followed by 0xCC int3 padding -- returns at once, and the padding is the
    //           compiler's filler, so there is genuinely no further body
    //   slot 3  `sub esp, 0x128` then pushes -- a large fixed frame, no frame pointer
    //   slot 4  `push ebp; mov ebp, esp; and esp, 0xFFFFFFC0` -- a frame pointer AND 64-byte stack
    //           alignment, which a function gets for holding aligned SSE locals. Fitting for the
    //           heavyweight of the three, and quite distinct from slot 3's shape.
    //
    // WHAT THIS IS WORTH: it distinguishes the mapped assignment from the plausible failure -- the
    // three being reordered -- because no two of these shapes match. It is NOT a proof of semantics:
    // it cannot tell PreUpdate from any other empty function, and a rebuild of the same game code
    // could legitimately change a prologue. Use it as a gate before hooking, and expect a false
    // negative on a different build rather than a wrong hook.
    static bool slots_match_mapped_shapes();
    static uintptr_t update_fn();
    static uintptr_t post_update_fn();
};

} // namespace sdk
