#include "Accuracy.hpp"

#include <cmath>

#include "Log.hpp"
#include "sdk/Engine.hpp"

Accuracy& Accuracy::get() {
    static Accuracy instance;
    return instance;
}

bool Accuracy::set_scale(float scale) {
    if (!(scale >= 0.0f) || scale > 1.0f) {
        return false; // the !(>=) form also rejects NaN
    }

    // CAPTURE BEFORE THE FIRST WRITE, and only then -- re-arming at a new scale must not
    // record our own value as the thing to restore.
    if (!m_captured.load(std::memory_order_acquire)) {
        if (const auto cur = sdk::Engine::console_var(kVar)) {
            m_original.store(cur->value, std::memory_order_relaxed);
            m_captured.store(true, std::memory_order_release);
        }
        // Absent is not a failure here. The fire path creates it with a default of 1.0 the
        // first time a shot is taken, so an arm at the main menu resolves on the next
        // check instead of being refused -- and 1.0 is then the correct original anyway.
    }

    m_scale.store(scale, std::memory_order_relaxed);
    m_armed.store(true, std::memory_order_release);
    m_countdown.store(0, std::memory_order_relaxed); // apply on the next frame, not in 60

    LOGX("[accuracy] perturb scaled to %.2f (1.0 is stock, 0.0 is no spread)", scale);
    return true;
}

bool Accuracy::release() {
    if (!m_armed.exchange(false, std::memory_order_acq_rel)) {
        return true;
    }
    if (!m_captured.load(std::memory_order_acquire)) {
        return true; // never wrote anything, so there is nothing to put back
    }
    const float original = m_original.load(std::memory_order_relaxed);
    const bool ok = sdk::Engine::write_console_var(kVar, original);
    LOGX("[accuracy] restored %s to %.2f%s", kVar, original, ok ? "" : " -- WRITE FAILED");
    return ok;
}

void Accuracy::on_frame() {
    if (!m_armed.load(std::memory_order_acquire)) {
        return;
    }

    auto remaining = m_countdown.load(std::memory_order_relaxed);
    if (remaining > 0) {
        m_countdown.store(remaining - 1, std::memory_order_relaxed);
        return;
    }
    m_countdown.store(kCheckInterval, std::memory_order_relaxed);

    const auto cur = sdk::Engine::console_var(kVar);
    if (!cur.has_value()) {
        m_resolved.store(false, std::memory_order_relaxed);
        return; // not created yet -- the first shot makes it
    }

    // First sighting of a variable that did not exist when we armed: this is its
    // pre-modification value, so it is the one to restore.
    if (!m_captured.load(std::memory_order_acquire)) {
        m_original.store(cur->value, std::memory_order_relaxed);
        m_captured.store(true, std::memory_order_release);
    }

    m_resolved.store(true, std::memory_order_relaxed);

    // RE-ASSERT ONLY WHEN IT HAS MOVED. A level load re-applies the difficulty's skill
    // values over ours, and comparing first means the common case is a read with no write
    // and no log line.
    const float want = m_scale.load(std::memory_order_relaxed);
    if (cur->value != want) {
        if (sdk::Engine::write_console_var(kVar, want)) {
            m_reasserts.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void Accuracy::on_shutdown() {
    release();
}
