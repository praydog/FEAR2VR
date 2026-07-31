#pragma once

#include <atomic>

#include "VRRuntime.hpp"

// ---- A RUNTIME WITH NO RUNTIME BEHIND IT ---------------------------------------------------
//
// Poses come from whatever last set them -- in practice the HTTP command server -- and frames
// advance on demand. There is no headset, no compositor and no OpenXR loader involved, which is
// what makes it usable at all here: the Meta XR Simulator and the Meta XR Operator layer are both
// x64-only, and FEAR2.exe is x86, so the ordinary way to develop VR without hardware is closed to
// this project.
//
// WHAT IT IS FOR, precisely. Everything upstream of presentation: head pose composition into the
// camera, controller poses driving the weapon and hands, the geometry that decides where a shot
// goes when the view and the aim disagree. None of that needs a compositor, and all of it is
// where the interesting bugs are.
//
// WHAT IT CANNOT TELL YOU. Nothing about OpenXR conformance. We would be writing both sides, so
// it validates that our code agrees with our assumptions -- swapchain lifecycle, image layouts,
// predicted display time handling and pose/projection mismatch are exactly the class of bug it
// would reproduce rather than catch. Those need a real runtime, and this project has three
// 32-bit ones installed to use when the render path exists.
//
// ---- STEP-DRIVEN, WHICH A REAL RUNTIME CANNOT BE -------------------------------------------
//
// `synchronize_frame` here does not wait on a display refresh; it increments a counter. A test can
// therefore advance exactly one frame and assert the consequence, instead of sleeping and hoping.
// That matters more than it sounds: nearly every flake this project has fought -- settle loops,
// quiescence gates, "wait 350ms and re-read" -- comes from asserting against a clock nobody
// controls. For the VR half, we control it.
namespace vr {

class SimulatedRuntime final : public VRRuntime {
public:
    std::string_view name() const override { return "SIMULATED"; }
    Type type() const override { return Type::SIMULATED; }
    bool ready() const override { return m_loaded; }

    // Bring the runtime up. Poses start at a plausible standing pose rather than at the origin,
    // because a head at (0,0,0) is under the floor and every consumer would then be exercised
    // against a configuration that cannot occur.
    void initialize();
    void destroy() override;

    // Advance one frame. Returns SUCCESS always -- there is nothing here that can fail, and
    // inventing a failure mode would be inventing a test case that does not correspond to
    // anything.
    Error synchronize_frame() override;

    // ---- THE CONTROL SURFACE ---------------------------------------------------------------
    //
    // What the HTTP routes drive. Deliberately absolute rather than incremental: a test that sets
    // a pose and asserts a consequence is reproducible, whereas one that nudges from wherever the
    // last test left things is not.
    void set_head_pose(const Pose& pose);
    void set_hand_pose(Hand which, const Pose& aim, const Pose& grip);
    void set_hand_inputs(Hand which, float trigger, float squeeze, const std::array<float, 2>& stick,
                         uint32_t buttons);
    void set_hand_active(Hand which, bool active);

    // Back to the standing rest pose, everything released. The equivalent of taking the headset
    // off and putting the controllers down.
    void reset();

private:
    // Eye height in METRES, runtime space. 1.7 is the figure the Meta XR Simulator itself reports
    // for a standing user, observed live on this machine, so a pose taken from a real session and
    // one taken from here start in the same place.
    static constexpr float kRestEyeHeight = 1.7f;

    // Hands at a relaxed low-ready: forward of the chest, slightly apart and below the eyeline.
    static constexpr float kRestHandForward = -0.35f;  // -Z is forward in runtime space
    static constexpr float kRestHandHeight = 1.25f;
    static constexpr float kRestHandSpread = 0.20f;
};

// The process-wide instance. A free function rather than a singleton member so the runtime can be
// swapped for a real one later without every call site naming the concrete class.
SimulatedRuntime& simulated_runtime();

} // namespace vr
