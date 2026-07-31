#include "SimulatedRuntime.hpp"

#include <mutex>

namespace vr {

namespace {

Pose rest_head(float height) {
    Pose p{};
    p.position = {0.0f, height, 0.0f};
    p.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
    p.valid = true;
    // TRACKED, deliberately. A simulated pose is synthetic in origin but it is not "held" or
    // extrapolated, and a consumer that ignores untracked poses should still see this one --
    // otherwise the simulated backend would exercise a different code path from the real one,
    // which is precisely what a test harness must not do.
    p.tracked = true;
    return p;
}

} // namespace

void SimulatedRuntime::initialize() {
    reset();
    m_loaded = true;
}

void SimulatedRuntime::destroy() {
    m_loaded = false;
}

void SimulatedRuntime::reset() {
    std::unique_lock lock{m_pose_mtx};

    m_head = rest_head(kRestEyeHeight);

    for (size_t i = 0; i < m_hands.size(); ++i) {
        const float side = (i == static_cast<size_t>(Hand::LEFT)) ? -kRestHandSpread : kRestHandSpread;

        HandState h{};
        h.aim.position = {side, kRestHandHeight, kRestHandForward};
        h.aim.orientation = {0.0f, 0.0f, 0.0f, 1.0f};
        h.aim.valid = true;
        h.aim.tracked = true;
        // Grip starts co-located with aim. On real hardware they differ by roughly a 45 degree
        // pitch, but inventing that offset here would be inventing hardware we are not modelling:
        // a consumer that needs them distinct should be given real values, not a plausible guess.
        h.grip = h.aim;
        h.active = true;
        m_hands[i] = h;
    }
}

VRRuntime::Error SimulatedRuntime::synchronize_frame() {
    m_frame_count.fetch_add(1, std::memory_order_relaxed);
    return Error::SUCCESS;
}

void SimulatedRuntime::set_head_pose(const Pose& pose) {
    std::unique_lock lock{m_pose_mtx};
    m_head = pose;
}

void SimulatedRuntime::set_hand_pose(Hand which, const Pose& aim, const Pose& grip) {
    std::unique_lock lock{m_pose_mtx};
    auto& h = m_hands[static_cast<size_t>(which)];
    h.aim = aim;
    h.grip = grip;
}

void SimulatedRuntime::set_hand_inputs(Hand which, float trigger, float squeeze,
                                       const std::array<float, 2>& stick, uint32_t buttons) {
    std::unique_lock lock{m_pose_mtx};
    auto& h = m_hands[static_cast<size_t>(which)];
    h.trigger = trigger;
    h.squeeze = squeeze;
    h.thumbstick = stick;
    h.buttons = buttons;
}

void SimulatedRuntime::set_hand_active(Hand which, bool active) {
    std::unique_lock lock{m_pose_mtx};
    m_hands[static_cast<size_t>(which)].active = active;
}

SimulatedRuntime& simulated_runtime() {
    static SimulatedRuntime s_runtime{};
    return s_runtime;
}

} // namespace vr
