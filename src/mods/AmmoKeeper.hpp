#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "../Mod.hpp"

// ---- KEEPING THE PLAYER STOCKED -------------------------------------------
//
// Holds the ammo type the player is FIRING at or above a floor, so a session can
// keep shooting indefinitely. Two uses, and the second is why it is a mod rather
// than a test helper:
//
//   * A practice/sandbox mode for a VR player who wants to work on aim.
//   * Unattended measurement. Firing is the only thing this project does that
//     the level cannot replace, and a drained pool has repeatedly produced reds
//     in checks that have nothing to do with weapons -- an empty magazine makes
//     the game AUTO-SWITCH, and the next measurement samples a different gun
//     mid-swap. TESTING.MD records four separate instances.
//
// OFF BY DEFAULT. It changes game state a player can see, so nothing arms it
// implicitly; the suite turns it on for the firing blocks and off afterwards.
//
// ---- IT ASKS THE GAME, IT DOES NOT WRITE MEMORY ---------------------------
//
// The top-up is one console line:
//
//     MSG player "ACQUIREAMMO <ammo type>"
//
// which is the engine's OWN object-message channel. The client packs it into
// message 236 and sends it to the server; the server resolves the target keyword
// `player` to CPlayerObj (`activeplayer`, `otherplayers`, `Team0`, `Team1` and
// `System` are the others), validates the ammo name against the database, and
// grants an ordinary pickup. Nothing here is a patch: it is the same path a
// level designer uses to hand the player ammo from a script.
//
// THE FIRST VERSION WROTE THE CLIENT'S AMMO ARRAY DIRECTLY, AND THAT WAS WRONG IN
// A WAY WORTH RECORDING. The client's reserve is COSMETIC. The server keeps its
// own counts and will not fire from an empty one, so a client-side refill
// produced a HUD reading 500 while the weapon dry-fired and auto-switched -- and
// an RPG whose animation played with no rocket ever spawning, because the shot
// was client-predicted and the server refused it. Three attempts to intercept the
// server's debit with a mid-hook crashed the game (see REVERSING_LESSONS.md).
// Going through the message makes the SERVER the one that grants, so both halves
// agree by construction and no hook exists to crash.
//
// Measured: reserve 333 -> 499 with the magazine topped from 33 -> 49, and firing
// afterwards decrements normally. The grant refills the clip as a real pickup
// does, which the memory-writing version could not do safely.
class AmmoKeeper final : public Mod {
public:
    static AmmoKeeper& get();

    std::string_view get_name() const override { return "AmmoKeeper"; }
    std::optional<std::string> on_initialize() override;
    void on_frame() override;
    void on_shutdown() override;

    // Arm with a floor in rounds. A floor <= 0 is refused rather than treated as
    // "off", so a caller that miscomputes one finds out instead of silently
    // disarming the thing it just asked for.
    bool set_floor(int32_t rounds);
    void disable();

    bool enabled() const { return m_enabled.load(std::memory_order_relaxed); }
    int32_t floor() const { return m_floor.load(std::memory_order_relaxed); }

    // Observability: how many times the floor was checked, and how many grants
    // were actually asked for. `grants` staying at 0 while enabled means the
    // player is already above the floor -- not that the mechanism is broken.
    uint64_t sweeps() const { return m_sweeps.load(std::memory_order_relaxed); }
    uint64_t grants() const { return m_grants.load(std::memory_order_relaxed); }

private:
    AmmoKeeper() = default;

    // The pool only moves when the player fires, so checking every frame would be
    // pointless work. This is the bounded-work rule the frame hook already obeys.
    static constexpr uint32_t kSweepInterval = 30;

    // After asking, wait longer than a sweep: the grant is a round trip to the
    // server and the reserve does not move until it lands. Without this the next
    // sweep still sees the old value and asks again, and a burst of duplicate
    // pickups is both wasteful and visible to the player.
    static constexpr uint32_t kGrantCooldown = 120;

    std::atomic<bool> m_enabled{false};
    std::atomic<int32_t> m_floor{0};
    std::atomic<uint32_t> m_countdown{0};
    std::atomic<uint64_t> m_sweeps{0};
    std::atomic<uint64_t> m_grants{0};

    // GAME THREAD ONLY -- read and written solely inside on_frame(), which is why
    // they are plain members while everything a route reads is atomic.
    //
    // `m_pending` is the type whose grant has not been confirmed yet, and
    // `m_ceiling` is what the game turned out to be willing to give for
    // `m_ceiling_ammo`. Learned, never assumed: the database surely holds a maximum
    // per ammo type, but the value that matters is what a pickup ACTUALLY yields,
    // and observing it costs one comparison.
    std::string m_pending;
    int32_t m_pending_before{0};
    std::string m_ceiling_ammo;
    int32_t m_ceiling{0};
};
