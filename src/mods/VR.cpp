#include "VR.hpp"

#include <atomic>
#include <cmath>

#include "sdk/PlayerMgr.hpp"
#include "sdk/Input.hpp"
#include "sdk/Object.hpp"
#include "BoneControl.hpp"
#include "SyntheticInput.hpp"
#include "HeadTracking.hpp"
#include "Log.hpp"

namespace {

std::atomic<bool> g_enabled{false};
std::atomic<uint64_t> g_applied{0};
std::atomic<bool> g_head_valid{false};
std::atomic<bool> g_hands{false};
std::atomic<uint64_t> g_hand_applied{0};
std::atomic<float> g_hand_off[3]{};
std::atomic<bool> g_hand_attached{false};

// The controller pose the hand offset is measured FROM. Captured the first time hands are armed,
// so "no movement" means the hand sits exactly where the animation put it.
std::atomic<float> g_hand_rest[3]{};
std::atomic<bool> g_have_rest{false};

// And the orientation it is measured from, for the same reason: BoneControl composes our rotation
// with the animation's, so an ABSOLUTE controller orientation would fight the animation instead of
// riding it. What the bone wants is "how far has the controller turned since rest".
std::atomic<float> g_hand_rest_rot[4]{};
std::atomic<float> g_hand_rot[4]{};
std::atomic<bool> g_trigger{false};
std::atomic<bool> g_firing{false};
std::atomic<uint64_t> g_pulls{0};

// Half-pull. A single threshold would chatter at the boundary, so release is lower than press --
// the standard hysteresis a physical trigger needs when it is being read as a button.
constexpr float kTriggerPress = 0.5f;
constexpr float kTriggerRelease = 0.35f;

// The engine's fire button, in the encoding SyntheticInput uses: mouse buttons live above the
// virtual-key range, and 0x100 is the left one.
constexpr uint8_t kMouseFire = 0;

// Last head orientation seen, in both spaces. Reported so a consumer -- or the fixture -- can
// check the conversion against its input without recomputing it, which would just be asserting
// the same arithmetic twice.
std::atomic<float> g_head_rt[4]{};
std::atomic<float> g_head_eng[4]{};

} // namespace

VR& VR::get() {
    static VR inst{};
    return inst;
}

std::optional<std::string> VR::on_initialize() {
    vr::simulated_runtime().initialize();
    LOGX("[vr] simulated runtime up (%s)", std::string(vr::simulated_runtime().name()).c_str());
    return std::nullopt;
}

void VR::on_shutdown() {
    g_enabled.store(false, std::memory_order_relaxed);
    // Release the view. HeadTracking composes rather than overrides, so clearing is enough -- the
    // engine's own identity write stands on the very next frame and there is nothing to restore.
    HeadTracking::get().clear();
    set_trigger_enabled(false);
    set_hands_enabled(false);
    vr::simulated_runtime().destroy();
}

std::array<float, 4> VR::runtime_to_engine_rotation(const std::array<float, 4>& q) {
    // Mirror along Z: conjugating a rotation by diag(1,1,-1) negates X and Y, leaves Z and W.
    return {-q[0], -q[1], q[2], q[3]};
}

std::array<float, 3> VR::runtime_to_engine_position(const std::array<float, 3>& p) {
    return {p[0] * kUnitsPerMetre, p[1] * kUnitsPerMetre, -p[2] * kUnitsPerMetre};
}

void VR::set_trigger_enabled(bool enabled) {
    g_trigger.store(enabled, std::memory_order_relaxed);

    if (!enabled && g_firing.exchange(false, std::memory_order_relaxed)) {
        // NEVER LEAVE THE TRIGGER HELD. A latched fire button outlives this mod -- the engine keeps
        // the state -- and would burn the magazine with nobody driving it.
        sdk::Input::send_mouse_button(kMouseFire, false);
    }
}

bool VR::trigger_enabled() const {
    return g_trigger.load(std::memory_order_relaxed);
}

void VR::set_hands_enabled(bool enabled) {
    g_hands.store(enabled, std::memory_order_relaxed);

    if (!enabled) {
        // Release the bone outright. Clearing the offset alone would leave our callback registered
        // on the engine's list contributing an identity, which is a different state from "not
        // driving the hand" and one the suite can tell apart.
        BoneControl::get().clear_offset();
        BoneControl::get().clear_rotation();
        BoneControl::get().detach();
        g_hand_attached.store(false, std::memory_order_relaxed);
        g_have_rest.store(false, std::memory_order_relaxed);
    }
}

bool VR::hands_enabled() const {
    return g_hands.load(std::memory_order_relaxed);
}

void VR::set_enabled(bool enabled) {
    g_enabled.store(enabled, std::memory_order_relaxed);

    if (!enabled) {
        HeadTracking::get().clear();
    }
}

bool VR::enabled() const {
    return g_enabled.load(std::memory_order_relaxed);
}

vr::VRRuntime& VR::runtime() const {
    return vr::simulated_runtime();
}

void VR::on_frame() {
    auto& rt = vr::simulated_runtime();

    if (!rt.ready()) {
        return;
    }

    // Advance the runtime every frame regardless of whether we are driving the engine. A frame
    // counter that only ticks while enabled would make "is the runtime alive" and "is the mod
    // armed" the same question, and they are not.
    rt.synchronize_frame();
    rt.update_poses();
    rt.update_input();

    update_hands();
    update_trigger();

    const auto head = rt.head();
    g_head_valid.store(head.valid, std::memory_order_relaxed);

    const auto engine = runtime_to_engine_rotation(head.orientation);

    for (size_t i = 0; i < 4; ++i) {
        g_head_rt[i].store(head.orientation[i], std::memory_order_relaxed);
        g_head_eng[i].store(engine[i], std::memory_order_relaxed);
    }

    if (!g_enabled.load(std::memory_order_relaxed) || !head.valid) {
        return;
    }

    // THE HEAD POSE, COMPOSED. HeadTracking writes the camera's OUTER operand, so the result is
    // `head * aim`: the view turns and the body, the aim and the weapon stay where the player put
    // them. Position is deliberately not applied -- the outer operand is a rotation, and moving
    // the camera's origin is a separate mechanism with separate consequences (clipping through
    // geometry, and the body offset) that is not in scope here.
    // ---- INTO THE BODY'S FRAME, NOT THE WORLD'S ------------------------------------------
    //
    // The engine composes `outer * inner`, where inner is the player's aim. Writing the head
    // rotation straight into outer applies it about WORLD axes, which is wrong for every axis
    // except yaw: pitching 20 degrees while the body faced 26.86 degrees produced 17.851 of
    // pitch, and 20*cos(26.86) = 17.84. Roll cross-talked into both yaw and pitch. Yaw alone
    // survived because both spaces share +Y up, which is exactly why this bug is easy to ship.
    //
    // Conjugating by the aim fixes it: with outer = aim * head * aim^-1, the engine's own
    // composition collapses to
    //
    //     outer * inner = (aim * head * aim^-1) * aim = aim * head
    //
    // i.e. the head applied in the BODY's frame, which is what a headset on a turning body does.
    const auto operands = sdk::PlayerMgr::camera_rotation_operands(0);

    if (!operands.has_value()) {
        return;
    }

    // THE BASIS IS THE BODY'S HEADING, NOT ITS FULL ORIENTATION. Conjugating by the whole aim was
    // measurably better than not conjugating at all -- pitch went exact -- but it left yaw at
    // 30.164 degrees with 0.884 of pitch drift, because the aim carries the player's PITCH too
    // (-6.578 here) and yawing about a tilted axis is not yawing.
    //
    // A neck does not work that way. Head yaw is about the spine, which stays vertical however far
    // up or down you happen to be looking, so the correct basis is the heading alone.
    const auto heading = sdk::PlayerMgr::aim_yaw(0);

    if (!heading.has_value()) {
        return;
    }

    const float half = *heading * 0.5f;
    const regenny::LTRotation aim{0.0f, std::sin(half), 0.0f, std::cos(half)};
    const regenny::LTRotation aim_inv{0.0f, -std::sin(half), 0.0f, std::cos(half)};
    const regenny::LTRotation head_local{engine[0], engine[1], engine[2], engine[3]};

    const auto outer = sdk::multiply_rotations(sdk::multiply_rotations(aim, head_local), aim_inv);

    HeadTracking::get().set_head_rotation({outer.x, outer.y, outer.z, outer.w});
    g_applied.fetch_add(1, std::memory_order_relaxed);
}

void VR::update_trigger() {
    if (!g_trigger.load(std::memory_order_relaxed)) {
        return;
    }

    const auto right = vr::simulated_runtime().hand(vr::VRRuntime::Hand::RIGHT);
    const bool was = g_firing.load(std::memory_order_relaxed);
    const bool now = was ? (right.trigger > kTriggerRelease) : (right.trigger > kTriggerPress);

    if (now == was) {
        return;
    }

    // EDGE ONLY. The engine consumes a press edge; re-asserting every frame would keep overwriting
    // the transition it is looking for -- the same reason SyntheticInput applies keys after the
    // engine's own poll rather than before it.
    sdk::Input::send_mouse_button(kMouseFire, now);
    g_firing.store(now, std::memory_order_relaxed);

    if (now) {
        g_pulls.fetch_add(1, std::memory_order_relaxed);
    }
}

void VR::update_hands() {
    if (!g_hands.load(std::memory_order_relaxed)) {
        return;
    }

    auto& rt = vr::simulated_runtime();
    const auto right = rt.hand(vr::VRRuntime::Hand::RIGHT);

    if (!right.active || !right.aim.valid) {
        return;
    }

    auto& bc = BoneControl::get();

    if (!g_hand_attached.load(std::memory_order_relaxed)) {
        if (!bc.attach_to_player_socket("RightHand")) {
            return;
        }
        g_hand_attached.store(true, std::memory_order_relaxed);
    }

    if (!g_have_rest.load(std::memory_order_relaxed)) {
        for (size_t i = 0; i < 3; ++i) {
            g_hand_rest[i].store(right.aim.position[i], std::memory_order_relaxed);
        }
        for (size_t i = 0; i < 4; ++i) {
            g_hand_rest_rot[i].store(right.aim.orientation[i], std::memory_order_relaxed);
        }
        g_have_rest.store(true, std::memory_order_relaxed);
    }

    // DELTA FROM REST, converted into the engine's space and units.
    const std::array<float, 3> delta{
        right.aim.position[0] - g_hand_rest[0].load(std::memory_order_relaxed),
        right.aim.position[1] - g_hand_rest[1].load(std::memory_order_relaxed),
        right.aim.position[2] - g_hand_rest[2].load(std::memory_order_relaxed)};

    const auto engine_delta = runtime_to_engine_position(delta);

    for (size_t i = 0; i < 3; ++i) {
        g_hand_off[i].store(engine_delta[i], std::memory_order_relaxed);
    }

    bc.set_offset(engine_delta[0], engine_delta[1], engine_delta[2]);

    // ---- ORIENTATION, AS A DELTA IN THE ENGINE'S SPACE -------------------------------------
    //
    // rest^-1 * current is "how far the controller has turned", in runtime space. Converting the
    // DELTA rather than each pose separately matters: the mirror-along-Z conversion is not a
    // rotation, so converting two poses and subtracting afterwards is not the same operation and
    // gets the handedness wrong in a way that only shows on the off-axes.
    const auto& rr = g_hand_rest_rot;
    const regenny::LTRotation rest_inv{-rr[0].load(std::memory_order_relaxed),
                                       -rr[1].load(std::memory_order_relaxed),
                                       -rr[2].load(std::memory_order_relaxed),
                                       rr[3].load(std::memory_order_relaxed)};
    const regenny::LTRotation now{right.aim.orientation[0], right.aim.orientation[1],
                                  right.aim.orientation[2], right.aim.orientation[3]};
    const auto turned = sdk::multiply_rotations(rest_inv, now);

    const auto engine_rot = runtime_to_engine_rotation({turned.x, turned.y, turned.z, turned.w});

    for (size_t i = 0; i < 4; ++i) {
        g_hand_rot[i].store(engine_rot[i], std::memory_order_relaxed);
    }

    bc.set_rotation(engine_rot[0], engine_rot[1], engine_rot[2], engine_rot[3]);
    g_hand_applied.fetch_add(1, std::memory_order_relaxed);
}

VR::State VR::state() const {
    State s{};
    s.enabled = g_enabled.load(std::memory_order_relaxed);
    s.runtime_name = std::string(vr::simulated_runtime().name());
    s.runtime_frames = vr::simulated_runtime().frame_count();
    s.applied = g_applied.load(std::memory_order_relaxed);
    s.head_valid = g_head_valid.load(std::memory_order_relaxed);
    s.hands = g_hands.load(std::memory_order_relaxed);
    s.trigger = g_trigger.load(std::memory_order_relaxed);
    s.firing = g_firing.load(std::memory_order_relaxed);
    s.pulls = g_pulls.load(std::memory_order_relaxed);
    s.hand_applied = g_hand_applied.load(std::memory_order_relaxed);
    for (size_t i = 0; i < 3; ++i) {
        s.hand_offset[i] = g_hand_off[i].load(std::memory_order_relaxed);
    }
    for (size_t i = 0; i < 4; ++i) {
        s.hand_rotation[i] = g_hand_rot[i].load(std::memory_order_relaxed);
    }

    for (size_t i = 0; i < 4; ++i) {
        s.head_runtime[i] = g_head_rt[i].load(std::memory_order_relaxed);
        s.head_engine[i] = g_head_eng[i].load(std::memory_order_relaxed);
    }

    return s;
}
