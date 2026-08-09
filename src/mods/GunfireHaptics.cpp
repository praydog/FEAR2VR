#include "GunfireHaptics.hpp"

#include "xr/SharedFrame.hpp"

#include "Log.hpp"

#include "FireRedirect.hpp"
#include "FramePublisher.hpp"

GunfireHaptics& GunfireHaptics::get() {
    static GunfireHaptics instance;
    return instance;
}

std::optional<std::string> GunfireHaptics::on_initialize() {
    // Nothing to hook. This mod owns no engine state and installs no detour -- it reads a counter
    // another mod already maintains and writes into a shared-memory ring. That is the entire
    // reason it can be this small, and it is why arming it cannot break a session.
    return std::nullopt;
}

void GunfireHaptics::set_enabled(bool on) {
    const bool was = m_enabled.exchange(on, std::memory_order_relaxed);

    // DISARMING MUST SILENCE, not merely stop queueing. The pulse already in flight is up to `ms`
    // long and is being played by the HOST, so switching off mid-buzz would otherwise leave the
    // controller running with nothing left that intends to stop it.
    if (was && !on) {
        silence();
    }
}

void GunfireHaptics::on_shutdown() {
    // Same reasoning as set_enabled, and this is the case that actually bites: the mod is about to
    // be unmapped, so if a pulse outlives it there is no code left anywhere in the game that knows
    // the controller is buzzing. The host is a separate process and will happily keep playing it.
    silence();
}

void GunfireHaptics::silence() {
    // BOTH HANDS, not just the configured one. `hand` can be changed while a pulse is in flight,
    // so the hand that is buzzing is not necessarily the hand that is selected now -- stopping
    // only the current one leaves the other running for exactly the caller who reconfigured
    // mid-session, which is the person most likely to be testing this.
    auto& fp = FramePublisher::get();
    const bool left = fp.stop_haptic(xr::kHandLeft);
    const bool right = fp.stop_haptic(xr::kHandRight);

    // WARN ONLY IF SOMETHING COULD ACTUALLY BE PLAYING. An unopened publisher is the NORMAL state
    // of a flatscreen session -- no host, no mapping -- so an unconditional warning here would
    // print an alarming failure on every ordinary unload and train everyone to ignore it. If this
    // mod never queued a pulse there is nothing to silence and nothing to say.
    //
    // When it HAS queued one, a failed stop is worth shouting about: it silently did nothing at
    // all until the registration order was fixed, because Mods::on_shutdown runs in registration
    // order and FrameCapture closes the shared mapping in its own shutdown. The symptom is a
    // controller left buzzing after the mod unmaps, which nobody would trace back to a
    // registration-order question.
    if ((!left || !right) && m_pulses.load(std::memory_order_relaxed) != 0) {
        LOGX("[gunfire-haptics] could not queue stops (left %d right %d) after %llu pulse(s) -- the "
             "frame publisher is closed, so anything still playing in the host runs to its own "
             "length",
             left ? 1 : 0, right ? 1 : 0,
             static_cast<unsigned long long>(m_pulses.load(std::memory_order_relaxed)));
    }
}

void GunfireHaptics::set_hand(uint32_t hand) {
    m_hand.store(hand == xr::kHandLeft ? xr::kHandLeft : xr::kHandRight,
                 std::memory_order_relaxed);
}

void GunfireHaptics::set_pulse(int32_t ms, float amplitude) {
    // Clamped the same way FramePublisher clamps, and for the same reason: a runtime may reject an
    // amplitude outside [0,1]. Written as "greater than zero" so a NaN lands on 0 rather than
    // surviving every comparison. Duration is left to the publisher, which turns anything
    // non-positive into XR_MIN_HAPTIC_DURATION.
    m_ms.store(ms < 0 ? 0 : ms, std::memory_order_relaxed);
    m_amplitude.store(amplitude > 0.0f ? (amplitude < 1.0f ? amplitude : 1.0f) : 0.0f,
                      std::memory_order_relaxed);
}

void GunfireHaptics::on_frame() {
    if (!m_enabled.load(std::memory_order_relaxed)) {
        // Disarmed: forget where the counter was, so re-arming seeds afresh instead of firing one
        // pulse for every shot taken while it was off.
        m_primed = false;
        return;
    }

    const uint64_t sends = FireRedirect::get().sends();

    // SEEDED ON THE FIRST ARMED FRAME, never from zero. The counter has been climbing since
    // injection, so treating its whole history as unfired would empty the ring into the wearer's
    // hand the instant the feature is switched on.
    if (!m_primed) {
        m_last_sends = sends;
        m_primed = true;
        return;
    }

    if (sends == m_last_sends) {
        return;
    }

    // ONE PULSE PER SHOT. An earlier version collapsed a frame's worth of fire messages into a
    // single pulse on the theory that a hand cannot resolve a burst anyway -- but that is a
    // judgement about feel, made silently, and it makes a minigun and a single shot produce the
    // same output. If a burst turns out to read badly the coalescing belongs behind a switch, not
    // baked into the only consumer of the ring.
    //
    // BOUNDED BY THE RING, because the alternative is worse than dropping. Queueing more entries
    // than kHapticSlots inside one frame laps the ring before the host has read any of it, so the
    // oldest are overwritten mid-copy and the commit stamp makes the host discard them -- the
    // pulses are lost either way, just less honestly. Capping here means the drop is COUNTED and
    // visible in the diagnostic rather than showing up as a torn-entry statistic on the host.
    const uint64_t fired = sends - m_last_sends;
    m_shots.fetch_add(fired, std::memory_order_relaxed);
    m_last_sends = sends;

    const uint64_t to_queue = fired < xr::kHapticSlots ? fired : xr::kHapticSlots;
    m_dropped.fetch_add(fired - to_queue, std::memory_order_relaxed);

    const int32_t ms = m_ms.load(std::memory_order_relaxed);
    const int64_t duration_ns = ms > 0 ? static_cast<int64_t>(ms) * 1'000'000 : -1;
    const uint32_t hand = m_hand.load(std::memory_order_relaxed);
    const float amplitude = m_amplitude.load(std::memory_order_relaxed);

    for (uint64_t i = 0; i < to_queue; ++i) {
        // Frequency unspecified: the runtime picks whatever its hardware does best, which is what
        // OpenXR's zero means and what a generic "gun went off" pulse wants.
        if (!FramePublisher::get().request_haptic(hand, duration_ns, 0.0f, amplitude)) {
            // A FAILED QUEUE IS NOT AN ERROR HERE. It means the frame publisher is not open -- no
            // host, no shared mapping, nothing to feel it -- which is the normal state of a
            // flatscreen session. The shots/pulses split is what tells that apart, and it is
            // reported rather than logged so a stuck trigger cannot flood the log.
            break;
        }
        m_pulses.fetch_add(1, std::memory_order_relaxed);
    }
}
