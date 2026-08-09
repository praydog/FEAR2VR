#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "../Mod.hpp"

// ---- A PULSE IN THE HAND WHEN THE GUN GOES OFF --------------------------------------------------
//
// The haptic ring (shared/xr/SharedFrame.hpp) had no producer other than the /vr/haptic diagnostic,
// which means the whole path -- ring, commit stamps, host consumer, xrApplyHapticFeedback -- was
// only ever exercised by hand. This is its first real consumer, and it is deliberately the simplest
// one that a wearer can verify without instrumentation: pull the trigger, feel the controller.
//
// WHY IT WATCHES A COUNTER INSTEAD OF HOOKING THE SHOT. FireRedirect already owns the fire path and
// counts client fire messages; adding a second hook to the same function would mean two detours on
// one address for no gain, and putting the pulse INSIDE FireRedirect would couple aiming to
// feedback. Polling its counter on the frame boundary costs one relaxed load and is decoupled at
// the price of at most one frame of latency -- about 11 ms at 90 Hz, which is well under the
// threshold at which a hand can tell a buzz from the bang that caused it.
//
// WHY `sends()` AND NOT THE SERVER-SIDE HOOK. `sends()` counts Weapon_SendClientFireMessage, which
// the client emits ONCE PER SHOT: a shotgun sends a single message carrying a perturb, and the
// server spreads it into pellets afterwards. The server-side trace hook runs per pellet, so driving
// haptics from it would fire eight pulses for one trigger pull and read as a broken buzz rather
// than a shot. One message, one pulse.
//
// DEFAULT OFF, because it is a physical side effect. Nothing in the suite or the launcher should
// start vibrating a controller because a mod happened to load.
class GunfireHaptics final : public Mod {
public:
    static GunfireHaptics& get();

    std::string_view get_name() const override { return "GunfireHaptics"; }

    std::optional<std::string> on_initialize() override;
    void on_frame() override;

    // STOPS ANY PULSE STILL RUNNING. A pulse is up to `ms` long and lives in the HOST, so a mod
    // that disarms or unloads mid-buzz leaves the controller vibrating with nothing left to turn
    // it off -- the same class of leak Comfort's console variables have, and the same rule
    // applies: state that outlives the DLL is restored, not abandoned.
    void on_shutdown() override;

    void set_enabled(bool on);
    bool enabled() const { return m_enabled.load(std::memory_order_relaxed); }

    // Which hand the weapon is in. Right by default; a left-handed wearer flips it rather than
    // being told the mod does not support them.
    void set_hand(uint32_t hand);
    uint32_t hand() const { return m_hand.load(std::memory_order_relaxed); }

    // Pulse shape, passed to the runtime as-is. Duration in milliseconds, amplitude in [0,1];
    // 0 ms means XR_MIN_HAPTIC_DURATION, "the shortest the runtime can produce".
    void set_pulse(int32_t ms, float amplitude);
    int32_t pulse_ms() const { return m_ms.load(std::memory_order_relaxed); }
    float amplitude() const { return m_amplitude.load(std::memory_order_relaxed); }

    // Observability: shots seen since arming, pulses actually queued, and shots dropped because a
    // single frame produced more than the ring holds. shots != pulses when the publisher is
    // closed, which is the difference between "the gun fired" and "the wearer felt it" -- and the
    // only way to tell those apart from outside.
    uint64_t shots_seen() const { return m_shots.load(std::memory_order_relaxed); }
    uint64_t pulses_queued() const { return m_pulses.load(std::memory_order_relaxed); }
    uint64_t shots_dropped() const { return m_dropped.load(std::memory_order_relaxed); }

private:
    // Queues a stop for BOTH hands. Shared by disarming and shutdown -- see the definitions.
    void silence();

    std::atomic<bool> m_enabled{false};
    std::atomic<uint32_t> m_hand{1};  // xr::kHandRight
    std::atomic<int32_t> m_ms{0};     // 0 -> XR_MIN_HAPTIC_DURATION
    std::atomic<float> m_amplitude{1.0f};

    // The last value of FireRedirect::sends() this mod acted on. Seeded on the first frame after
    // arming rather than from zero: the counter has been climbing since injection, and replaying
    // every shot the player already took as a burst of buzzing is a memorable way to get this
    // feature turned straight back off.
    uint64_t m_last_sends{0};
    bool m_primed{false};

    std::atomic<uint64_t> m_shots{0};
    std::atomic<uint64_t> m_pulses{0};
    std::atomic<uint64_t> m_dropped{0};
};
