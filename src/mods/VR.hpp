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

    // WORLD SCALE. Provisional and flagged as such: nothing in this project has yet measured
    // FEAR2's units against a physical metre. It is used only for controller POSITION offsets,
    // where being wrong scales hand separation rather than breaking anything structural, and it
    // is deliberately not used for the head pose, whose rotation carries no scale at all.
    //
    // Deriving it properly (from gravity, or the player capsule against a known human height) is
    // a task in its own right and is not guessed at here.
    static constexpr float kUnitsPerMetre = 64.0f;

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
    void set_hands_enabled(bool enabled);
    bool hands_enabled() const;

    State state() const;

private:
    VR() = default;

    void update_hands();
};
