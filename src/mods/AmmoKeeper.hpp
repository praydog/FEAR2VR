#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "../Mod.hpp"

// ---- KEEPING THE PLAYER STOCKED -------------------------------------------
//
// Holds every ammo type the player CARRIES at or above a floor, so a session can
// keep firing indefinitely. Two uses, and the second is why it is a mod rather
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
// It does NOT refill the clip -- `PlayerMgr::replenish_ammo` tops the reserve the
// weapon reloads FROM, which is the field the engine owns. A caller still has to
// let the weapon reload, and that is deliberate: forcing the clip would mean
// writing weapon state mid-fire, where the engine is the only safe writer.
//
// It also does not grant types held at zero (see replenish_ammo) -- sustaining a
// loadout and inventing one are different features, and only the first is wanted
// while measuring.
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

    // Observability: how many top-ups have been performed and how many ammo types
    // the last sweep actually raised. `raised` staying at 0 while enabled means
    // the player is already above the floor -- not that the mechanism is broken.
    uint64_t sweeps() const { return m_sweeps.load(std::memory_order_relaxed); }
    uint64_t raised_total() const { return m_raised_total.load(std::memory_order_relaxed); }
    uint32_t last_raised() const { return m_last_raised.load(std::memory_order_relaxed); }

private:
    AmmoKeeper() = default;

    // The pool only moves when the player fires, so a sweep every frame would be
    // ~240 pointless walks a second over 52 database records. This is the
    // bounded-work rule the frame hook already has to obey.
    static constexpr uint32_t kSweepInterval = 30;

    std::atomic<bool> m_enabled{false};
    std::atomic<int32_t> m_floor{0};
    std::atomic<uint32_t> m_countdown{0};
    std::atomic<uint64_t> m_sweeps{0};
    std::atomic<uint64_t> m_raised_total{0};
    std::atomic<uint32_t> m_last_raised{0};
};
