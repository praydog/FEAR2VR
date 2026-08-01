#include "VR.hpp"

#include "CameraPassHook.hpp"
#include "FramePublisher.hpp"

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
std::atomic<float> g_hand_off[2][3]{};
std::atomic<bool> g_hand_attached[2]{};

// The controller pose the hand offset is measured FROM. Captured the first time hands are armed,
// so "no movement" means the hand sits exactly where the animation put it.
std::atomic<float> g_hand_rest[2][3]{};
std::atomic<bool> g_have_rest[2]{};

// And the orientation it is measured from, for the same reason: BoneControl composes our rotation
// with the animation's, so an ABSOLUTE controller orientation would fight the animation instead of
// riding it. What the bone wants is "how far has the controller turned since rest".
std::atomic<float> g_hand_rest_rot[2][4]{};
std::atomic<float> g_hand_rot[2][4]{};
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
        // BOTH hands -- releasing one and leaving the other registered is exactly the
        // asymmetry BoneControl::detach_all exists to prevent.
        for (uint32_t slot = 0; slot < 2; ++slot) {
            BoneControl::get().clear_offset(slot);
            BoneControl::get().clear_rotation(slot);
            BoneControl::get().detach(slot);
            g_hand_attached[slot].store(false, std::memory_order_relaxed);
            g_have_rest[slot].store(false, std::memory_order_relaxed);
        }
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

void VR::set_use_host_pose(bool on) {
    if (on) {
        FramePublisher::get().open();
    }

    m_use_host_pose.store(on, std::memory_order_release);
    LOGX("[vr] head pose source: %s", on ? "the OpenXR host" : "local/simulated");

    // A POSE WITH NO CONSUMER LOOKS EXACTLY LIKE A BROKEN HEADSET. Selecting the host as the pose
    // source does nothing visible unless the mod is also applying poses to the engine, and the
    // symptom of forgetting -- "moving my head does nothing" -- points at tracking rather than at
    // a switch. Say so plainly instead of leaving it to be rediscovered.
    if (on && !enabled()) {
        LOGX("[vr] NOTE: head pose is coming from the host, but this mod is NOT enabled -- nothing "
             "will move until /xr/enable?on=1");
    }
}

void VR::set_neutral_camera_offset(bool on) {
    if (on) {
        if (!m_have_saved_cam_off) {
            if (const auto cur = sdk::PlayerMgr::camera_attached_offset(); cur.has_value()) {
                m_saved_cam_off = *cur;
                m_have_saved_cam_off = true;
                LOGX("[vr] camera attached offset saved: %.3f %.3f %.3f", (*cur)[0], (*cur)[1],
                     (*cur)[2]);
            }
        }

        sdk::PlayerMgr::set_camera_attached_offset({0.0f, 0.0f, 0.0f});
    } else if (m_have_saved_cam_off) {
        // Restore EXACTLY what was there. These are the game's own console variables, not ours, and
        // leaving them zeroed would change the flatscreen game for the rest of the session.
        sdk::PlayerMgr::set_camera_attached_offset(m_saved_cam_off);
    }

    m_neutral_cam_off.store(on, std::memory_order_release);
    LOGX("[vr] camera attached offset %s", on ? "NEUTRALISED" : "restored");
}

void VR::set_pin_eye_height(bool on) {
    if (on) {
        // Recaptured on every enable: the reference must be the offset while looking LEVEL and
        // FORWARD, and switching the feature on is the only moment we can reasonably assume that.
        // `recenter` recaptures it, for when it was enabled at a bad moment.
        m_have_eye_ref = false;
    }

    m_pin_eye.store(on, std::memory_order_release);

    if (!on) {
        // Hand the camera back exactly as it was found. A mod that stops correcting must also stop
        // applying its last correction, or "off" quietly means "frozen at whatever it last was".
        CameraPassHook::get().set_position_offset(0.0f, 0.0f, 0.0f);
    }

    LOGX("[vr] eye height pinning %s", on ? "ON" : "off");
}

void VR::recenter() {
    m_have_eye_ref = false;
    m_recenter.store(true, std::memory_order_release);
    LOGX("[vr] roomscale origin will be recaptured");
}

void VR::set_roomscale(bool on) {
    if (on) {
        FramePublisher::get().open();
        m_recenter.store(true, std::memory_order_release);
    }

    m_roomscale.store(on, std::memory_order_release);

    if (!on) {
        CameraPassHook::get().set_position_offset(0.0f, 0.0f, 0.0f);
    }

    LOGX("[vr] roomscale %s", on ? "ON" : "off");
}

void VR::set_paced(bool on) {
    if (on) {
        FramePublisher::get().open();
    }

    m_paced.store(on, std::memory_order_release);
    LOGX("[vr] frame pacing %s -- the runtime's clock %s the game's", on ? "ON" : "off",
         on ? "now drives" : "no longer drives");
}

void VR::on_frame() {
    auto& rt = vr::simulated_runtime();

    if (!rt.ready()) {
        return;
    }

    // ---- WAIT FOR THE RUNTIME'S FRAME CLOCK ----------------------------------------------------
    //
    // This runs inside CClientShell::Update, so blocking here paces the ENTIRE game loop -- exactly
    // what xrWaitFrame does for a native VR application, relayed across the process boundary.
    //
    // Unpaced, the game ran at 140-150 fps into a 90 Hz compositor and juddered, while the same
    // build alt-tabbed to ~72 fps looked perfect. A FASTER game producing a WORSE picture is the
    // signature of two unrelated clocks: frames and poses were being generated on different
    // cadences and the beat between them is what the wearer sees. Matching the clocks removes it at
    // the source, where compensating downstream could only ever approximate it.
    //
    // The timeout is deliberately about two compositor frames: long enough to wait for a tick that
    // is coming, short enough that a tick which is NOT coming costs little before the give-up
    // counter takes over.
    if (m_paced.load(std::memory_order_acquire)) {
        FramePublisher::get().wait_for_host_tick(22);
    }

    // Advance the runtime every frame regardless of whether we are driving the engine. A frame
    // counter that only ticks while enabled would make "is the runtime alive" and "is the mod
    // armed" the same question, and they are not.
    rt.synchronize_frame();
    rt.update_poses();
    rt.update_input();

    update_camera_offset();

    // The wearer's actual head, if the host is publishing it. Read before anything consumes poses
    // this frame, so the engine sees where the head IS rather than where it was last frame.
    if (m_use_host_pose.load(std::memory_order_acquire)) {
        if (const auto* host = FramePublisher::get().host_state(); host != nullptr) {
            // Seqlock: odd means the host is mid-write, and an unchanged sequence means no new
            // pose. Either way the previous pose stands -- never a torn one, never a zeroed one.
            const uint32_t seq = host->sequence;

            if ((seq & 1u) == 0u && seq != m_last_host_sequence && host->valid != 0u) {
                vr::Pose pose{};
                pose.orientation = {host->orientation[0], host->orientation[1],
                                    host->orientation[2], host->orientation[3]};
                pose.position = {host->position[0], host->position[1], host->position[2]};
                pose.valid = true;
                pose.tracked = true;

                if (host->sequence == seq) {  // still unchanged: the read was clean
                    rt.set_head_pose(pose);
                    m_last_host_sequence = seq;
                    m_last_host_seq_pub.store(seq, std::memory_order_release);

                    // RENDER WITH THE HEADSET'S FIELD OF VIEW, or the projection layer is a lie.
                    // The host publishes the smallest SYMMETRIC half-angles containing the
                    // headset's asymmetric frustum; the engine wants FULL angles, hence the
                    // doubling -- its own default reads 1.695 rad, which is 97 degrees across and
                    // could only be a full angle.
                    if (host->fov_x > 0.0f && host->fov_y > 0.0f) {
                        CameraPassHook::get().set_fov_override(host->fov_x * 2.0f,
                                                               host->fov_y * 2.0f);
                    }
                    m_host_pose_updates.fetch_add(1, std::memory_order_relaxed);
                }
            } else {
                m_host_pose_stale.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

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
    // BOTH HANDS. Index 0 is the right, 1 the left, matching BoneControl's slots -- a
    // two-handed grip and every off-hand interaction needs the pair driven together, and
    // until BoneControl grew slots only one of them could be.
    drive_hand(vr::VRRuntime::Hand::RIGHT, 0, "RightHand");
    drive_hand(vr::VRRuntime::Hand::LEFT, 1, "LeftHand");
}

void VR::drive_hand(vr::VRRuntime::Hand which, uint32_t slot, const char* socket) {
    auto& rt = vr::simulated_runtime();
    const auto hand = rt.hand(which);

    if (!hand.active || !hand.aim.valid) {
        return;
    }

    auto& bc = BoneControl::get();

    if (!g_hand_attached[slot].load(std::memory_order_relaxed)) {
        if (!bc.attach_to_player_socket(socket, slot)) {
            return;
        }
        g_hand_attached[slot].store(true, std::memory_order_relaxed);
    }

    // REST IS PER HAND. Sharing one rest pose would make each hand's offset a delta from
    // wherever the OTHER one happened to start.
    if (!g_have_rest[slot].load(std::memory_order_relaxed)) {
        for (size_t i = 0; i < 3; ++i) {
            g_hand_rest[slot][i].store(hand.aim.position[i], std::memory_order_relaxed);
        }
        for (size_t i = 0; i < 4; ++i) {
            g_hand_rest_rot[slot][i].store(hand.aim.orientation[i], std::memory_order_relaxed);
        }
        g_have_rest[slot].store(true, std::memory_order_relaxed);
    }

    // DELTA FROM REST, converted into the engine's space and units.
    const std::array<float, 3> delta{
        hand.aim.position[0] - g_hand_rest[slot][0].load(std::memory_order_relaxed),
        hand.aim.position[1] - g_hand_rest[slot][1].load(std::memory_order_relaxed),
        hand.aim.position[2] - g_hand_rest[slot][2].load(std::memory_order_relaxed)};

    const auto engine_delta = runtime_to_engine_position(delta);

    for (size_t i = 0; i < 3; ++i) {
        g_hand_off[slot][i].store(engine_delta[i], std::memory_order_relaxed);
    }

    bc.set_offset(engine_delta[0], engine_delta[1], engine_delta[2], slot);

    // ---- ORIENTATION, AS A DELTA IN THE ENGINE'S SPACE -------------------------------------
    //
    // rest^-1 * current is "how far the controller has turned", in runtime space. Converting the
    // DELTA rather than each pose separately matters: the mirror-along-Z conversion is not a
    // rotation, so converting two poses and subtracting afterwards is not the same operation and
    // gets the handedness wrong in a way that only shows on the off-axes.
    const regenny::LTRotation rest_inv{-g_hand_rest_rot[slot][0].load(std::memory_order_relaxed),
                                       -g_hand_rest_rot[slot][1].load(std::memory_order_relaxed),
                                       -g_hand_rest_rot[slot][2].load(std::memory_order_relaxed),
                                       g_hand_rest_rot[slot][3].load(std::memory_order_relaxed)};
    const regenny::LTRotation now{hand.aim.orientation[0], hand.aim.orientation[1],
                                  hand.aim.orientation[2], hand.aim.orientation[3]};
    const auto turned = sdk::multiply_rotations(rest_inv, now);

    const auto engine_rot = runtime_to_engine_rotation({turned.x, turned.y, turned.z, turned.w});

    for (size_t i = 0; i < 4; ++i) {
        g_hand_rot[slot][i].store(engine_rot[i], std::memory_order_relaxed);
    }

    bc.set_rotation(engine_rot[0], engine_rot[1], engine_rot[2], engine_rot[3], slot);
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
        s.hand_offset[i] = g_hand_off[0][i].load(std::memory_order_relaxed);
    }
    for (size_t i = 0; i < 4; ++i) {
        s.hand_rotation[i] = g_hand_rot[0][i].load(std::memory_order_relaxed);
    }

    for (size_t i = 0; i < 4; ++i) {
        s.head_runtime[i] = g_head_rt[i].load(std::memory_order_relaxed);
        s.head_engine[i] = g_head_eng[i].load(std::memory_order_relaxed);
    }

    return s;
}

void VR::update_camera_offset() {
    const bool pin = m_pin_eye.load(std::memory_order_acquire);
    const bool room = m_roomscale.load(std::memory_order_acquire);

    if (!pin && !room) {
        return;
    }

    float dx = 0.0f;
    float dy = 0.0f;
    float dz = 0.0f;

    if (pin) {
        // WHERE THE EYE SHOULD BE: the body's own origin plus its eye height. Both are published by
        // PlayerMgr and were cross-checked against the live camera earlier -- eye_height + body_y
        // equals cam_y to within 0.003 units when the view is level, which is what makes the
        // difference below a measurement of the pitch artefact rather than a guess at it.
        // PIN THE WHOLE EYE OFFSET, not just its height.
        //
        // `eye_offset` is the camera object's position MINUS the body model's -- the eye's position
        // relative to the body, which is exactly the quantity that should not change when only the
        // head turns. Measured with the body stationary and the head rotated:
        //
        //     yaw  +90 deg   dx  +0.10  dy   0.00  dz -11.82
        //     pitch +80 deg  dx +25.41  dy  -0.32  dz -14.76
        //     pitch -80 deg  dx +11.47  dy -53.88  dz  -6.66
        //
        // The yaw figures trace a chord of 2r*sin(theta/2) with r about 8.4 units -- a NECK. FEAR
        // carries a full body model, the "Camera" socket sits on its head bone, and the injected
        // head rotation swings that bone. For a flatscreen shooter that lever arm is free realism;
        // in a headset it is a second neck fighting the wearer's real one.
        //
        // Not CameraAttachedOffset, which was the obvious suspect and is already 0,0,0 here --
        // zeroing it changed nothing, measured. The motion is in the socket itself.
        if (const auto off = sdk::PlayerMgr::eye_offset(0); off.has_value()) {
            // ONLY CAPTURE WHILE THE HEAD IS NEUTRAL, and wait as long as it takes.
            //
            // The reference has to be the eye offset with NO neck orbit in it, and the orbit is
            // zero only when the head is level and facing forward. Capturing at an arbitrary
            // moment bakes that moment's orbit into the reference and displaces the wearer
            // permanently -- observed directly: a recenter taken while looking down moved the
            // reference from (-6.89, 74.77, -4.73) to (-6.13, 53.88, -4.24), and the wearer then
            // sat feet away from the player for the rest of the session.
            //
            // Deferring is better than clamping or averaging: the correct sample exists a moment
            // later, as soon as the wearer looks ahead, and using a wrong one now cannot be undone
            // without noticing it first.
            if (!m_have_eye_ref && head_is_neutral()) {
                m_eye_ref_vec = *off;
                m_have_eye_ref = true;
                LOGX("[vr] eye offset reference captured while neutral: %.2f %.2f %.2f",
                     m_eye_ref_vec[0], m_eye_ref_vec[1], m_eye_ref_vec[2]);
            }

            if (!m_have_eye_ref) {
                // Nothing to correct against yet. Applying a half-formed correction would be worse
                // than applying none.
                m_pin_part = {};
                return;
            }

            m_pin_part = {m_eye_ref_vec[0] - (*off)[0], m_eye_ref_vec[1] - (*off)[1],
                          m_eye_ref_vec[2] - (*off)[2]};
            dx += m_pin_part[0];
            dy += m_pin_part[1];
            dz += m_pin_part[2];
        }
    }

    if (room) {
        if (const auto* host = FramePublisher::get().host_state(); host != nullptr) {
            if (host->valid != 0u) {
                if (m_recenter.exchange(false, std::memory_order_acq_rel)) {
                    m_room_origin[0] = host->position[0];
                    m_room_origin[1] = host->position[1];
                    m_room_origin[2] = host->position[2];
                }

                // ONE UNIT IS ONE CENTIMETRE in this engine -- established by measurement, not by
                // assumption -- so metres from the runtime scale by 100.
                constexpr float kMetresToUnits = 100.0f;
                const float rx = (host->position[0] - m_room_origin[0]) * kMetresToUnits;
                const float ry = (host->position[1] - m_room_origin[1]) * kMetresToUnits;

                // HANDEDNESS: OpenXR is right-handed with -Z FORWARD; this engine is left-handed
                // with +Z forward. So Z negates and X does not, which is why walking forward came
                // out backwards while left and right were already correct.
                //
                // The orientation path agrees, and that is the corroboration rather than a second
                // guess: the head quaternion arrives as (x, y, z, w) and the engine holds
                // (-x, -y, z, w), which is precisely the quaternion form of flipping the Z axis.
                // One convention, consistently applied to both halves of a pose.
                const float rz = -(host->position[2] - m_room_origin[2]) * kMetresToUnits;

                // Rotated into the BODY's heading, not the camera's: leaning left should move the
                // eye left of where the player is facing, whatever the head is looking at.
                const auto yaw = sdk::PlayerMgr::aim_yaw(0);
                const float c = yaw.has_value() ? cosf(*yaw) : 1.0f;
                const float s = yaw.has_value() ? sinf(*yaw) : 0.0f;

                m_room_part = {rx * c + rz * s, ry, -rx * s + rz * c};
                dx += m_room_part[0];
                dy += m_room_part[1];
                dz += m_room_part[2];
            }
        }
    }

    CameraPassHook::get().set_position_offset(dx, dy, dz);
}

bool VR::head_is_neutral() const {
    const auto* host = FramePublisher::get().host_state();

    if (host == nullptr || host->valid == 0u) {
        return false;
    }

    // The rotation angle of a unit quaternion is 2*acos(|w|), so |w| alone answers "how far from
    // identity is this" without unpacking axes. cos(7.5 deg) for a 15 degree total cone: generous
    // enough that a wearer looking roughly ahead qualifies, tight enough that the residual orbit is
    // under a centimetre on an 8.4 cm lever arm.
    constexpr float kNeutralCosHalf = 0.99144f;
    const float w = host->orientation[3];
    return (w < 0.0f ? -w : w) >= kNeutralCosHalf;
}
