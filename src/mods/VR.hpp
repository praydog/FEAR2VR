#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "../Mod.hpp"
#include "vr/runtimes/SimulatedRuntime.hpp"

// ---- THE VR MOD: A RUNTIME, AND THE ENGINE IT DRIVES ----------------------------------------
//
// Owns the active `vr::VRRuntime` and, once per frame, pushes what it reports into the engine:
// the head pose becomes the camera's composed rotation, and the controller poses become the
// weapon and hands.
//
// Consumers below this line never learn which backend is live. That is the point of the
// abstraction and it is load-bearing for this project specifically -- the simulated backend is
// the ONLY one available at 32-bit today (Meta's simulator and operator layer are both x64), so
// every piece of VR behaviour has to be buildable and testable against it and then work unchanged
// against real OpenXR.
//
// ---- THE COORDINATE CONVERSION, WHICH IS THE PART THAT MUST BE EXACTLY RIGHT -----------------
//
// Runtime space is OpenXR's: right-handed, +X right, +Y up, -Z forward, metres.
// Engine space is LithTech's: left-handed, +X right, +Y up, +Z forward, game units.
//
// The two differ by a mirror along Z. Mirroring a rotation means conjugating it by that mirror,
// diag(1,1,-1), and for a quaternion that negates the X and Y components while leaving Z and W:
//
//     (x, y, z, w)_openxr  ->  (-x, -y, z, w)_lithtech
//
// Positions flip Z and scale by units-per-metre.
//
// It is written once, here, and asserted rather than believed: a yaw applied in runtime space
// must produce the SAME yaw in the engine, and a pitch the same pitch with the same sign. Get the
// handedness wrong and yaw still looks plausible while pitch inverts, which is the classic way
// this bug survives a casual look.
class VR final : public Mod {
public:
    static VR& get();

    std::string_view get_name() const override { return "VR"; }

    std::optional<std::string> on_initialize() override;
    void on_frame() override;

    // ---- REAL HEAD POSE, FROM THE 64-BIT HOST --------------------------------------------------
    //
    // Feed the pose the OpenXR host publishes into the runtime, so the engine's camera follows the
    // wearer's head. Deliberately routed through the SAME set_head_pose() the simulated runtime and
    // the test harness already use: every piece of composition, clamping and restore behaviour
    // below it has been verified against synthetic poses, and swapping the SOURCE of the pose keeps
    // all of that rather than opening a second path that has to be re-proven.
    void set_use_host_pose(bool on);
    bool using_host_pose() const { return m_use_host_pose.load(std::memory_order_acquire); }

    // How many frames carried a fresh, tracked pose from the host, and how many found nothing new.
    uint64_t host_pose_updates() const { return m_host_pose_updates.load(std::memory_order_relaxed); }
    uint64_t host_pose_stale() const { return m_host_pose_stale.load(std::memory_order_relaxed); }

    // The HostState sequence whose pose the engine is currently rendering with. Published alongside
    // each frame so the host can submit that frame with the pose it was actually drawn from.
    uint32_t last_host_sequence() const { return m_last_host_seq_pub.load(std::memory_order_acquire); }
    void on_shutdown() override;

    // ---- CONSUMER API ----------------------------------------------------------------------

    // Start driving the engine from the runtime. Off by default: it moves the player's view, and
    // a mod that seizes the camera the moment it loads cannot be tested against a baseline.
    void set_enabled(bool enabled);
    bool enabled() const;

    // The live runtime. Never null -- the simulated one exists from initialization, so callers do
    // not need a null check that would only ever be false.
    vr::VRRuntime& runtime() const;

    // ---- THE CONVERSION, EXPOSED BECAUSE IT IS USEFUL AND TESTABLE -------------------------
    //
    // A runtime-space orientation in the engine's convention. Public because the fixture asserts
    // it directly, and because any consumer composing its own poses needs the same rule rather
    // than a second, subtly different copy of it.
    static std::array<float, 4> runtime_to_engine_rotation(const std::array<float, 4>& q);

    // A runtime-space position in engine units. `units_per_metre` is a measured property of the
    // game, not a constant of VR -- see `kUnitsPerMetre`.
    static std::array<float, 3> runtime_to_engine_position(const std::array<float, 3>& p);

    // WORLD SCALE, MEASURED FROM THE ENGINE'S OWN GRAVITY. One unit is one CENTIMETRE.
    //
    // CClientMgr_GetGlobalForce reports (0, -980, 0). Earth gravity is 9.80665 m/s^2, so
    //
    //     980 units/s^2  /  9.80665 m/s^2  =  99.93 units/metre
    //
    // and 980 is not a coincidence -- it is 9.8 m/s^2 written in cm/s^2. This is the engine
    // stating its own scale, not an inference from anatomy, which is why it is trustworthy where
    // the anthropometric anchors were not: they disagreed wildly (an "eye offset" of 75.6 units
    // implies 44 units/m at a 1.7 m eye height, while a 40-unit stair implies 200 at a 20 cm
    // riser). Both were measuring something other than what their names suggested.
    //
    // Corroborated at 100 u/m: the 40-unit step height becomes a 0.40 m maximum step-up, which is
    // an ordinary value for a shooter, and the hands sit 15-20 units below the eye, i.e. 15-20 cm.
    //
    // THE PREVIOUS VALUE WAS 64, INVENTED. It was flagged provisional and it was 36% wrong, which
    // is what a plausible-looking guess buys: every controller position was silently under-scaled
    // and nothing looked broken.
    //
    // A level that changed gravity would break the derivation, not the scale -- so the fixture
    // asserts the premise (global force magnitude is 980) rather than trusting it forever.
    static constexpr float kUnitsPerMetre = 100.0f;

    struct State {
        bool enabled{};
        std::string runtime_name{};
        uint64_t runtime_frames{};
        uint64_t applied{};        // frames where a head pose reached the engine
        bool head_valid{};
        std::array<float, 4> head_runtime{};
        std::array<float, 4> head_engine{};

        bool hands{};
        uint64_t hand_applied{};
        std::array<float, 3> hand_offset{};   // engine units, applied to the RightHand socket
        std::array<float, 4> hand_rotation{};  // engine-space delta from the controller's rest pose

        bool trigger{};
        bool firing{};
        uint64_t pulls{};
    };

    // ---- CONTROLLERS -> THE WEAPON HAND -----------------------------------------------------
    //
    // The right controller drives the socket the weapon hangs off ("RightHand"), through
    // BoneControl. Off separately from the head, because the two are independently useful and
    // independently able to look wrong: a mod author debugging hand placement should not have to
    // have the view seized as well.
    //
    // Position is a DELTA from the controller's rest pose, not an absolute. The runtime's origin
    // is a room-scale floor point with no relationship to where the game's arm happens to be, so
    // an absolute mapping would fling the hand across the level on the first frame. A delta means
    // "however you are holding it, move the hand by that much", which is both correct at rest and
    // the only mapping that works before world scale is measured.
    // The right controller's trigger pulls the weapon's trigger. Edge-triggered against a
    // threshold rather than passed through as an analogue value, because the engine's firing input
    // is a BUTTON -- there is no partial-pull semantics to preserve, and re-asserting a held button
    // every frame would suppress the press edge the engine actually consumes.
    //
    // Inert until a runtime reports a non-zero trigger, so it cannot fire by merely existing.
    void set_trigger_enabled(bool enabled);
    bool trigger_enabled() const;

    void set_hands_enabled(bool enabled);
    bool hands_enabled() const;

    State state() const;

private:
    std::atomic<bool> m_use_host_pose{false};
    std::atomic<uint64_t> m_host_pose_updates{0};
    std::atomic<uint64_t> m_host_pose_stale{0};
    uint32_t m_last_host_sequence{0};
    std::atomic<uint32_t> m_last_host_seq_pub{0};

    VR() = default;

    void update_hands();

    // One hand. `slot` is the BoneControl slot it drives, and each hand keeps its own rest
    // pose -- sharing one would make every offset a delta from wherever the OTHER hand
    // happened to start.
    void drive_hand(vr::VRRuntime::Hand which, uint32_t slot, const char* socket);
    void update_trigger();
};
