#include "AmmoKeeper.hpp"

#include "Log.hpp"
#include "sdk/PlayerMgr.hpp"

AmmoKeeper& AmmoKeeper::get() {
    static AmmoKeeper instance;
    return instance;
}

std::optional<std::string> AmmoKeeper::on_initialize() {
    return std::nullopt;
}

bool AmmoKeeper::set_floor(int32_t rounds) {
    if (rounds <= 0) {
        return false;
    }
    m_floor.store(rounds, std::memory_order_relaxed);
    m_countdown.store(0, std::memory_order_relaxed); // sweep on the next frame, not in 30
    m_enabled.store(true, std::memory_order_release);
    LOGX("[ammo] keeping every carried type at >= %d rounds", rounds);
    return true;
}

void AmmoKeeper::disable() {
    // The pool is left where it is. Restoring the original counts would be the
    // wrong contract: the player has been firing, so "the value before" is not a
    // state the game was ever going to return to, and writing it back would take
    // away rounds they legitimately still hold.
    m_enabled.store(false, std::memory_order_release);
}

void AmmoKeeper::on_frame() {
    if (!m_enabled.load(std::memory_order_acquire)) {
        return;
    }

    auto remaining = m_countdown.load(std::memory_order_relaxed);
    if (remaining > 0) {
        m_countdown.store(remaining - 1, std::memory_order_relaxed);
        return;
    }
    m_countdown.store(kSweepInterval, std::memory_order_relaxed);

    const size_t raised = sdk::PlayerMgr::replenish_ammo(0, m_floor.load(std::memory_order_relaxed));
    m_sweeps.fetch_add(1, std::memory_order_relaxed);
    m_last_raised.store(static_cast<uint32_t>(raised), std::memory_order_relaxed);
    if (raised != 0) {
        m_raised_total.fetch_add(raised, std::memory_order_relaxed);
    }
}

void AmmoKeeper::on_shutdown() {
    disable();
}
