#pragma once

#include <cstdint>
#include <optional>

namespace sdk {

// Engine-side CClientShell (CNetHandler subclass in the LithTech sources).
// Reached through CClientMgr::client_shell() (m_pClientShellDE at +0x1434).
//
// No regenny::CClientShell struct yet -- CClientShell::Update (dump
// 0x40CC5E) touches at least a byte field @+0x69, a uint16_t[4] handle
// table @+0x60 (0xFFFF-sentineled, resolved through what looks like a
// generic engine handle-manager indirection), and the resolved uint32_t[4]
// @+0x6C, but none of these were pinned down to a confident semantic this
// pass (see reversing/MAPPING_WORKFLOW.md: don't force a struct/field name
// without confident evidence). Deliberately out of scope, not forgotten --
// next slice if this class needs more than update_fn()/get().
class CClientShell {
public:
    // Live instance via the client manager. nullptr when no shell exists
    // (before engine init / after world teardown).
    static CClientShell* get();

    // Runtime address of CClientShell::Update (dump 0x40CC5E) -- called every
    // frame by CClientMgr::Update once a shell exists; our frame hook anchor.
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
};

} // namespace sdk
