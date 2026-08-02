#include "AmmoKeeper.hpp"

#include <cstdio>

#include "ConsoleRunner.hpp"
#include "Log.hpp"
#include "sdk/PlayerMgr.hpp"
#include "sdk/WeaponMgr.hpp"

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
    m_countdown.store(0, std::memory_order_relaxed); // check on the next frame, not in 30
    m_enabled.store(true, std::memory_order_release);
    LOGX("[ammo] keeping the carried type at >= %d rounds", rounds);
    return true;
}

void AmmoKeeper::disable() {
    // Nothing to undo. Every round the player holds was granted by the server as
    // a normal pickup, so there is no "before" state to restore -- and taking the
    // ammo back would be a change the game never made on its own.
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
    m_sweeps.fetch_add(1, std::memory_order_relaxed);

    // The type currently being fired is the only one that can drain mid-measurement,
    // and topping it up is what stops the auto-switch. A weapon picked up later gets
    // its own type on the sweep after the switch.
    const auto ammo = sdk::WeaponMgr::current_ammo_name();
    if (ammo.empty()) {
        return;
    }

    // Read the reserve rather than granting unconditionally: ACQUIREAMMO is a real
    // pickup, so an unconditional grant would keep announcing itself to the player.
    const auto held = sdk::PlayerMgr::ammo_count(0, ammo);
    if (!held.has_value()) {
        return;
    }

    // DID THE LAST ASK ACTUALLY MOVE ANYTHING? The floor is what we WANT; the game
    // decides what a pickup gives, and every type has its own maximum. Rockets cap
    // at 15, so a floor of 500 asked forever and got 27 grants in 20 seconds -- the
    // reserve can never reach a floor above the type's ceiling.
    //
    // So a grant that leaves the reserve where it was teaches us that type's
    // ceiling, and from then on the ceiling is the trigger. Firing still drops the
    // reserve below it and still triggers a top-up; what stops is asking for rounds
    // the game was never going to hand over.
    //
    // EQUALITY, NOT <=. A reserve that came back LOWER does not mean the grant was refused -- it
    // means the player FIRED while the ask was in flight, and a burst is exactly when this mod is
    // being used. Reading that as a ceiling pins it at whatever the reserve happened to be
    // mid-burst, permanently, and the keeper then sits satisfied below the real ceiling and never
    // tops up again. The fixture caught this by firing immediately after enabling the keeper.
    //
    // Declining to learn is the safe direction: the next sweep simply asks again.
    if (!m_pending.empty() && m_pending == ammo) {
        if (*held == m_pending_before) {
            m_ceiling_ammo = ammo;
            m_ceiling = *held;
            LOGX("[ammo] %s tops out at %d -- treating that as its floor", ammo.c_str(), *held);
        }
        m_pending.clear();
    }

    auto want = m_floor.load(std::memory_order_relaxed);
    if (m_ceiling_ammo == ammo && m_ceiling < want) {
        want = m_ceiling;
    }
    if (*held >= want) {
        return;
    }

    // Quoted as ONE argument. MSG re-quotes each argument when it builds the wire
    // string, so an unquoted "ACQUIREAMMO SMG" would arrive as two targets.
    //
    // Sized to the runner's own limit: a longer line is refused by queue() anyway.
    char line[128]{};
    const int written = std::snprintf(line, sizeof(line), "MSG player \"ACQUIREAMMO %s\"", ammo.c_str());
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(line)) {
        return; // an ammo name that long is not one the database holds
    }

    if (ConsoleRunner::get().queue(line)) {
        m_grants.fetch_add(1, std::memory_order_relaxed);
        m_countdown.store(kGrantCooldown, std::memory_order_relaxed);
        m_pending = ammo;
        m_pending_before = *held;
        LOGX("[ammo] asked the server for %s (held %d, want %d)", ammo.c_str(), *held, want);
    }
}

void AmmoKeeper::on_shutdown() {
    disable();
}
