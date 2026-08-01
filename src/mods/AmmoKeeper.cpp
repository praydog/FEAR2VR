#include "AmmoKeeper.hpp"

#include "Hooks.hpp"
#include "Log.hpp"
#include "sdk/Memory.hpp"
#include "sdk/WeaponMgr.hpp"

namespace {

constexpr const char* kServerDebitHook = "Weapon_HandleClientFireMessage::ammo_debit";

std::atomic<bool> g_server_hold{false};
std::atomic<uint64_t> g_blocked{0};

// Runs immediately AFTER `sub [ecx+eax*4], esi`, where ecx is the server player's ammo array and
// eax is still the ammo type index. The debit has already happened; this puts the slot back.
//
// RESTORING, NOT SUPPRESSING. Zeroing the consumed amount so the subtraction did nothing was tried
// first and drove the reserve NEGATIVE, with the game auto-switching weapons because it believed
// the pool was empty -- something else in the fire path reconciles against that subtraction. Let it
// happen, then top the slot back up, and none of the engine's own arithmetic is disturbed.
std::atomic<int32_t> g_server_floor{500};

void ammo_debit_mid(safetyhook::Context& ctx) {
    if (!g_server_hold.load(std::memory_order_relaxed)) {
        return;
    }

    const auto base = static_cast<uintptr_t>(ctx.ecx);
    const auto index = static_cast<uintptr_t>(ctx.eax);

    if (base == 0 || index > 64) {
        return;
    }

    const int32_t floor = g_server_floor.load(std::memory_order_relaxed);
    const uintptr_t slot = base + index * sizeof(int32_t);

    // Only ever RAISE it. Writing unconditionally would fight a pickup that legitimately put more
    // in the pool than the floor.
    if (const auto have = sdk::mem::read<int32_t>(slot); have.has_value() && *have < floor) {
        if (sdk::mem::write<int32_t>(slot, floor)) {
            g_blocked.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

}  // namespace



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
    // INSTALL LAZILY. gameserver.dll only exists once a session does, so this cannot be resolved
    // at framework init -- and a failure here is "no session yet", not "broken".
    if (Hooks::get().find(kServerDebitHook) == nullptr) {
        if (const uintptr_t site = sdk::WeaponMgr::server_ammo_debit_site(); site != 0) {
            if (Hooks::get().install_mid(kServerDebitHook, reinterpret_cast<void*>(site),
                                         &ammo_debit_mid)) {
                LOGX("[ammo] holding the SERVER's pool at gameserver+0x%08zX",
                     static_cast<size_t>(site));
            }
        } else {
            LOGX("[ammo] server ammo debit site unresolved (no session yet?) -- client-side only");
        }
    }

    g_server_floor.store(static_cast<int32_t>(rounds), std::memory_order_relaxed);
    g_server_hold.store(true, std::memory_order_relaxed);
    m_enabled.store(true, std::memory_order_release);
    LOGX("[ammo] keeping every carried type at >= %d rounds", rounds);
    return true;
}

bool AmmoKeeper::server_hold() const {
    return g_server_hold.load(std::memory_order_relaxed);
}

uint64_t AmmoKeeper::server_debits_blocked() const {
    return g_blocked.load(std::memory_order_relaxed);
}

void AmmoKeeper::disable() {
    // The pool is left where it is. Restoring the original counts would be the
    // wrong contract: the player has been firing, so "the value before" is not a
    // state the game was ever going to return to, and writing it back would take
    // away rounds they legitimately still hold.
    g_server_hold.store(false, std::memory_order_relaxed);
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
