#include "VR.hpp"

#include <windows.h>

#include <cmath>

#include "CameraPassHook.hpp"
#include "FramePublisher.hpp"

#include <atomic>
#include <cmath>

#include "sdk/PlayerMgr.hpp"
#include "sdk/Input.hpp"
#include "sdk/Memory.hpp"
#include "sdk/Object.hpp"
#include "BoneControl.hpp"
#include "Hooks.hpp"
#include "ViewHook.hpp"
#include "sdk/CClientMgr.hpp"
#include "sdk/Model.hpp"
#include "SyntheticInput.hpp"
#include "HeadTracking.hpp"
#include "Log.hpp"

namespace {

std::atomic<bool> g_enabled{false};
std::atomic<uint64_t> g_applied{0};
std::atomic<bool> g_head_valid{false};
std::atomic<bool> g_hands{false};

// Forward, back, strafe left, strafe right -- the engine's own bindings, driven as keys so movement
// stays aim-relative and animation-correct. Order matches the bit order in update_locomotion.
constexpr std::array<uint32_t, 4> kLocoKeys{'W', 'S', 'A', 'D'};

// The game's own default bindings. Sent as keys so the engine's binding table, cooldowns and
// animation gating all apply exactly as they do for a keyboard player.
constexpr uint32_t kKeyJump = 0x20;    // VK_SPACE
constexpr uint32_t kKeyReload = 0x52;  // 'R'

constexpr float kStickDeadZone = 0.30f;
constexpr float kSnapFire = 0.70f;   // beyond this, a snap fires
constexpr float kSnapRearm = 0.30f;  // and the stick must return inside this before the next one
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

    // GIVE THE PLAYER THEIR BODY BACK. Unlike the view -- which the engine rewrites every frame, so
    // simply stopping is enough -- a cleared visibility flag is a change to the game's own state
    // that nothing else will undo. Leaving it would mean unloading the mod left an invisible player
    // model for the rest of the session, with nothing to connect the two.
    if (m_hide_body.load(std::memory_order_acquire)) {
        m_hide_body.store(false, std::memory_order_release);
        apply_body_visibility(true);
    }

    // Same reasoning for the camera offset: it is ours, and it must not outlive us.
    CameraPassHook::get().set_position_offset(0.0f, 0.0f, 0.0f);
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
        if (m_sprinting.exchange(false, std::memory_order_relaxed)) {
            // Engine state OUTLIVES this DLL, so a held key is ours to put back. Leaving Shift down
            // through an uninject leaves the player sprinting with no controller attached to it.
            SyntheticInput::get().hold(m_sprint_vk.load(std::memory_order_relaxed), false);
        }
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

void VR::set_eye_smoothing_ms(float ms) {
    m_eye_tau_ms.store(ms < 0.0f ? 0.0f : ms, std::memory_order_release);
    LOGX("[vr] eye reference smoothing %.0f ms", ms);
}

void VR::set_eye_heights(float standing, float crouched) {
    m_eye_stand.store(standing, std::memory_order_release);
    m_eye_crouch.store(crouched, std::memory_order_release);
    LOGX("[vr] eye heights: %.2f standing, %.2f crouched", standing, crouched);
}

void VR::set_eye_height_trim(float units) {
    m_eye_trim.store(units, std::memory_order_release);
    LOGX("[vr] eye height trim %+.1f units", units);
}

void VR::set_hide_body(bool on) {
    m_hide_body.store(on, std::memory_order_release);

    if (!on) {
        apply_body_visibility(true);
    }

    LOGX("[vr] player body %s", on ? "HIDDEN" : "visible");
}

void VR::apply_body_visibility(bool visible) {
    const auto p = sdk::PlayerMgr::player(0);

    if (!p.has_value() || p->model_object == 0) {
        return;
    }

    auto* obj = reinterpret_cast<regenny::LTObject*>(p->model_object);
    uint32_t flags = 0;

    if (!sdk::mem::copy(&flags, reinterpret_cast<uintptr_t>(&obj->flags), sizeof(flags))) {
        return;
    }

    if (!visible && !m_have_body_flags) {
        // Saved once, so "show" restores what the game had rather than a bit we invented.
        m_saved_body_flags = flags;
        m_have_body_flags = true;
    }

    const uint32_t wanted = visible
                                ? (m_have_body_flags ? m_saved_body_flags : flags)
                                : (flags & ~sdk::object_flags::kVisible);

    if (wanted == flags) {
        return;
    }

    // The engine's render gate is `(flags & 1) && !(flags2 & 0x700)`, evaluated when objects are
    // collected for drawing -- so clearing the bit is enough and there is no cache to invalidate.
    if (sdk::mem::write(reinterpret_cast<uintptr_t>(&obj->flags), wanted)) {
        m_body_hides.fetch_add(1, std::memory_order_relaxed);
    }
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

namespace {

// std::atomic<float> has no fetch_add before C++20's overload set covers it uniformly here, and a CAS
// loop is the honest way to accumulate one from another thread.
void atomic_add_float(std::atomic<float>& slot, float value) {
    float current = slot.load(std::memory_order_relaxed);
    while (!slot.compare_exchange_weak(current, current + value, std::memory_order_relaxed)) {
    }
}

}  // namespace

void VR::queue_body_nudge(float dx, float dz) {
    atomic_add_float(m_nudge_x, dx);
    atomic_add_float(m_nudge_z, dz);
}

void VR::set_roomscale_body(bool on) {
    if (on) {
        FramePublisher::get().open();
    }

    // Drop the baseline either way, so switching on does not deliver a lurch made of every
    // millimetre the wearer moved while it was switched off.
    m_have_last_room = false;
    m_room_body.store(on, std::memory_order_release);
    LOGX("[vr] roomscale BODY movement %s", on ? "ON" : "off");
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

    // DRAINED FIRST, and outside every other gate: this is the diagnostic path, and it has to work
    // whether or not a runtime is ready, or it can only be exercised in the configuration it is meant
    // to be proving.
    {
        const float nx = m_nudge_x.exchange(0.0f, std::memory_order_relaxed);
        const float nz = m_nudge_z.exchange(0.0f, std::memory_order_relaxed);
        if ((nx != 0.0f || nz != 0.0f) && sdk::PlayerMgr::displace_player(0, {nx, 0.0f, nz})) {
            m_body_moves.fetch_add(1, std::memory_order_relaxed);
        }
    }

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

    // ---- THE CONTROLLERS, FROM THE HOST --------------------------------------------------------
    //
    // Fed in the RUNTIME's convention, deliberately UNCONVERTED. drive_hand performs the
    // mirror-along-Z itself and performs it on the DELTA from the rest pose, because that mirror is
    // not a rotation: converting two poses and subtracting afterwards is a different operation and
    // gets the handedness wrong in a way that only shows on the off-axes. Converting here as well
    // would be precisely that bug, applied twice.
    if (const auto* hands = FramePublisher::get().hands_state(); hands != nullptr) {
        const uint32_t seq = hands->sequence;

        // Same seqlock discipline as the head: odd means mid-write, and the sequence is re-read
        // after the copy so a block that changed underneath us is discarded rather than believed.
        if ((seq & 1u) == 0u && seq != m_last_hands_sequence) {
            const xr::HandsState snapshot = *hands;

            if (hands->sequence == seq) {
                for (uint32_t i = 0; i < 2; ++i) {
                    const auto which = (i == xr::kHandLeft) ? vr::VRRuntime::Hand::LEFT
                                                            : vr::VRRuntime::Hand::RIGHT;
                    const auto& src = snapshot.hand[i];

                    const auto to_pose = [&src](const xr::HandPose& p) {
                        vr::Pose out{};
                        out.position = {p.position[0], p.position[1], p.position[2]};
                        out.orientation = {p.orientation[0], p.orientation[1], p.orientation[2],
                                           p.orientation[3]};
                        out.valid = p.valid != 0u;
                        // TRACKED IS PER HAND, NOT PER POSE: the runtime reports it for the
                        // controller, and a consumer that wants to ignore an inferred pose is asking
                        // about the device rather than about aim versus grip.
                        out.tracked = src.tracked != 0u;
                        return out;
                    };

                    rt.set_hand_pose(which, to_pose(src.aim), to_pose(src.grip));
                    rt.set_hand_inputs(which, src.trigger, src.squeeze,
                                       {src.stick[0], src.stick[1]}, src.buttons);
                    rt.set_hand_active(which, src.active != 0u);
                }

                m_last_hands_sequence = seq;
                m_hand_pose_updates.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    update_hands();
    update_trigger();
    update_locomotion();
    update_buttons();
    update_weapon();

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

void VR::set_locomotion(bool on) {
    if (!on) {
        // RELEASE WHAT WE ARE HOLDING, or turning the feature off leaves the player walking into a
        // wall forever with no key to let go of. The engine holds key state, not us.
        auto& si = SyntheticInput::get();
        for (const auto vk : kLocoKeys) {
            si.hold(vk, false);
        }
        m_held_keys = 0;
        m_loco_keys.store(0, std::memory_order_relaxed);
    }

    m_locomotion.store(on, std::memory_order_release);
    LOGX("[vr] stick locomotion %s", on ? "ON" : "off");
}

void VR::update_locomotion() {
    if (!m_locomotion.load(std::memory_order_acquire)) {
        return;
    }

    auto& rt = vr::simulated_runtime();
    const auto left = rt.hand(vr::VRRuntime::Hand::LEFT);
    const auto right = rt.hand(vr::VRRuntime::Hand::RIGHT);
    auto& si = SyntheticInput::get();

    // ---- WALKING -------------------------------------------------------------------------------
    //
    // A DEAD ZONE, because a thumbstick does not rest at exactly zero and a controller left on a
    // desk would otherwise walk the player across the level. 0.30 is generous on purpose: this is a
    // digital mapping, so anything above the threshold is full speed and precision buys nothing.
    uint32_t wanted = 0;

    if (left.active) {
        const float x = left.thumbstick[0];
        const float y = left.thumbstick[1];

        if (y > kStickDeadZone) {
            wanted |= 1u << 0;  // forward
        } else if (y < -kStickDeadZone) {
            wanted |= 1u << 1;  // back
        }

        if (x < -kStickDeadZone) {
            wanted |= 1u << 2;  // strafe left
        } else if (x > kStickDeadZone) {
            wanted |= 1u << 3;  // strafe right
        }
    }

    // EDGE-DRIVEN, not re-asserted: hold() is a level, and writing the same level every frame would
    // fight the engine's own poll for no reason. Only transitions are sent.
    if (wanted != m_held_keys) {
        for (size_t i = 0; i < kLocoKeys.size(); ++i) {
            const uint32_t bit = 1u << i;
            if ((wanted & bit) != (m_held_keys & bit)) {
                si.hold(kLocoKeys[i], (wanted & bit) != 0u);
            }
        }
        m_held_keys = wanted;
        m_loco_keys.store(wanted, std::memory_order_relaxed);
    }

    // ---- SNAP TURN -----------------------------------------------------------------------------
    //
    // A SCHMITT TRIGGER, not a threshold: the stick has to come back inside the re-arm band before
    // another turn can fire. Without that, holding the stick over spins the player continuously at
    // frame rate, which is both wrong and the most reliable way to make someone sick.
    const float snap = m_snap_deg.load(std::memory_order_relaxed);

    if (snap <= 0.0f || !right.active) {
        return;
    }

    const float rx = right.thumbstick[0];

    if (m_snap_armed && (rx > kSnapFire || rx < -kSnapFire)) {
        snap_turn(rx > 0.0f ? snap : -snap);
        m_snap_armed = false;
    } else if (!m_snap_armed && rx < kSnapRearm && rx > -kSnapRearm) {
        m_snap_armed = true;
    }
}

bool VR::snap_turn(float degrees) {
    // ---- ONE FRAME, NOT A CONVERGING LOOP --------------------------------------------------------
    //
    // This used TurnController::turn_by, which is a CLOSED LOOP: it issues a mouse-look delta, waits
    // three frames for it to land, requires two consecutive in-tolerance readings and converges in
    // four to six corrections. That is the right design for turn_to -- the mouse path's gain is not
    // constant, so an open-loop delta cannot hit a heading -- and it is exactly wrong for a snap.
    // Fifteen-odd frames of visible stepping is what "it moves at 20 fps and looks like it is
    // fighting something" IS: each step is one correction, and the overshoot each one corrects for
    // is the fight.
    //
    // apply_look_delta has NO SUCH PROBLEM and therefore needs no loop: the engine's own look entry
    // has a gain of exactly 1 -- measured, +0.05 rad in gives 2.8648 degrees out, with no
    // sensitivity curve and no acceleration -- and it persists, because the camera's own update does
    // not re-derive the rotation afterwards. A caller asking for 30 degrees gets 30 degrees, once.
    //
    // YAW ONLY. Pitch is deliberately left at zero: this path applies no clamp, so driving pitch
    // through it would push the view somewhere the engine itself would never allow.
    constexpr float kDegToRad = 3.14159265f / 180.0f;

    if (!sdk::PlayerMgr::apply_look_delta(0, 0.0f, degrees * kDegToRad)) {
        return false;
    }

    m_stick_turns.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void VR::set_weapon_override(bool on) {
    if (!on) {
        m_have_weapon_rest = false;
        // Disarm by clearing the TARGET, so ViewHook's detour stops matching. Nothing is retired
        // here: unhooking is the uninject path's job, not a toggle's.
        ViewHook::get().set_weapon_amend(0, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f});
    }
    m_weapon_override.store(on, std::memory_order_release);
    LOGX("[vr] weapon override %s", on ? "ON" : "off");
}

void VR::set_weapon_probe(float x, float y, float z) {
    m_weapon_probe[0].store(x, std::memory_order_relaxed);
    m_weapon_probe[1].store(y, std::memory_order_relaxed);
    m_weapon_probe[2].store(z, std::memory_order_relaxed);
}

uintptr_t VR::find_weapon_object() {
    // Re-resolved every frame rather than cached: the weapon object is destroyed and recreated on
    // every weapon switch, and a cached pointer would be writing into freed memory the moment the
    // player changes gun -- which is exactly the shape of the bug that corrupted the heap earlier
    // in this project.
    auto* mgr = sdk::CClientMgr::get();
    const auto me = sdk::PlayerMgr::engine_objects(0);

    if (mgr == nullptr || !me.has_value() || me->model == 0) {
        return 0;
    }

    const auto anchor = sdk::object_info(reinterpret_cast<const regenny::LTObject*>(me->model));
    if (!anchor.has_value()) {
        return 0;
    }

    std::vector<sdk::CClientMgr::ObjectSnapshot> snaps(512);
    const auto got = mgr->snapshot_objects(static_cast<sdk::ObjectType>(1), snaps.data(), snaps.size());
    if (!got.has_value()) {
        return 0;
    }

    uintptr_t best = 0;
    float best_d2 = 200.0f * 200.0f;

    for (size_t i = 0; i < *got; ++i) {
        const auto* o = reinterpret_cast<const regenny::LTObject*>(snaps[i].address);
        const auto oi = sdk::object_info(o);

        // CLIENT-ONLY, VISIBLE, and a weapon asset. The server-handled twin fails the handle test,
        // the hidden player models fail the kVisible test, and world pickups fail the distance test.
        if (!oi.has_value() || oi->handle != 0xFFFFu ||
            (oi->flags & sdk::object_flags::kVisible) == 0u) {
            continue;
        }

        const auto fn = sdk::model_filename(o);
        if (!fn.has_value() || fn->rfind("weapons\\", 0) != 0) {
            continue;
        }

        const float dx = oi->position.x - anchor->position.x;
        const float dy = oi->position.y - anchor->position.y;
        const float dz = oi->position.z - anchor->position.z;
        const float d2 = dx * dx + dy * dy + dz * dz;

        if (d2 < best_d2) {
            best_d2 = d2;
            best = snaps[i].address;
        }
    }

    return best;
}

void VR::update_weapon() {
    if (!m_weapon_override.load(std::memory_order_acquire)) {
        return;
    }

    const uintptr_t obj = find_weapon_object();
    m_weapon_obj.store(obj, std::memory_order_relaxed);

    if (obj == 0) {
        return;
    }

    // OWN THE WRITER RATHER THAN THE FIELD. A direct write to LTObject.position was measured
    // landing (91 writes) and changing nothing on screen: the engine rebuilds this object's
    // transform from its attachment every frame, so the field is reclaimed before it is drawn.
    // LTObject_SetPosRot is what does the rebuilding -- found by arming a hardware write watch on
    // the object's position, which reported 115 hits from a single accessor at FEAR2.exe+0x202C7
    // with ecx holding the weapon object.
    // ---- THE CONTROLLER, AS A DELTA FROM ITS REST POSE -------------------------------------
    //
    // A delta rather than an absolute pose, for the same reason drive_hand takes one: the engine
    // puts the gun where the weapon animation wants it, and what a wearer means by "the gun follows
    // my hand" is that MOVING the hand moves the gun by the same amount -- not that the gun
    // teleports to the controller's position in play space, which has no relationship to where the
    // weapon belongs on screen.
    //
    // The rest pose is captured the first frame a valid aim exists, so it is whatever the wearer
    // was holding when tracking came up.
    float px = m_weapon_probe[0].load(std::memory_order_relaxed);
    float py = m_weapon_probe[1].load(std::memory_order_relaxed);
    float pz = m_weapon_probe[2].load(std::memory_order_relaxed);
    std::array<float, 4> turn{0.0f, 0.0f, 0.0f, 1.0f};

    const auto hand = vr::simulated_runtime().hand(vr::VRRuntime::Hand::RIGHT);

    if (hand.active && hand.aim.valid) {
        if (!m_have_weapon_rest) {
            m_weapon_rest = hand.aim.position;
            m_weapon_rest_rot = hand.aim.orientation;
            m_have_weapon_rest = true;
        }

        const std::array<float, 3> moved{hand.aim.position[0] - m_weapon_rest[0],
                                         hand.aim.position[1] - m_weapon_rest[1],
                                         hand.aim.position[2] - m_weapon_rest[2]};
        // ---- INTO THE BODY'S FRAME ---------------------------------------------------------
        //
        // The controller delta is measured in PLAY space, which is bolted to the room. The value it
        // amends is in WORLD space. Between them sits the player's heading, and leaving it out is
        // what made the gun depend on where the headset was pointing: a fixed room-space offset
        // points a fixed world direction, so it is only ever correct for one facing, and every
        // other facing gets it rotated by however far the body has turned since.
        //
        // The BODY's heading, not the camera's. The gun hangs off the player, and using the view
        // would make looking around re-aim the offset -- which is the symptom this fixes.
        const auto yaw = sdk::PlayerMgr::aim_yaw(0);
        const float cy = yaw.has_value() ? cosf(*yaw) : 1.0f;
        const float sy = yaw.has_value() ? sinf(*yaw) : 0.0f;

        const auto engine_move = runtime_to_engine_position(moved);
        px += engine_move[0] * cy + engine_move[2] * sy;
        py += engine_move[1];
        pz += -engine_move[0] * sy + engine_move[2] * cy;

        // CONVERT THE DELTA, never the two poses separately: the runtime-to-engine mapping is a
        // mirror, not a rotation, so converting each pose and subtracting afterwards is a different
        // operation and gets the handedness wrong on the off-axes.
        const regenny::LTRotation rest_inv{-m_weapon_rest_rot[0], -m_weapon_rest_rot[1],
                                           -m_weapon_rest_rot[2], m_weapon_rest_rot[3]};
        const regenny::LTRotation now{hand.aim.orientation[0], hand.aim.orientation[1],
                                      hand.aim.orientation[2], hand.aim.orientation[3]};
        const auto turned = sdk::multiply_rotations(rest_inv, now);
        const auto engine_turn = runtime_to_engine_rotation({turned.x, turned.y, turned.z, turned.w});

        // THE SAME CHANGE OF FRAME, for the rotation: conjugate the play-space turn by the body's
        // heading so it names an axis in the world rather than an axis in the room.
        //   q_world = R(yaw) * q_play * conj(R(yaw))
        const float half = yaw.value_or(0.0f) * 0.5f;
        const regenny::LTRotation ry{0.0f, sinf(half), 0.0f, cosf(half)};
        const regenny::LTRotation ry_inv{-ry.x, -ry.y, -ry.z, ry.w};
        const regenny::LTRotation q_play{engine_turn[0], engine_turn[1], engine_turn[2],
                                         engine_turn[3]};
        const auto q_world = sdk::multiply_rotations(sdk::multiply_rotations(ry, q_play), ry_inv);
        turn = {q_world.x, q_world.y, q_world.z, q_world.w};
    } else {
        m_have_weapon_rest = false;
    }


    // ONE OWNER: ViewHook already hooks LTObject_SetPosRot, so the amendment is handed to it
    // rather than hooked a second time. Installing a second inline hook on that address crashed
    // the game on unload -- see Hooks::install.
    ViewHook::get().set_weapon_amend(obj, {px, py, pz}, turn);
    m_weapon_writes.store(ViewHook::get().weapon_amendments(), std::memory_order_relaxed);
}

void VR::update_buttons() {
    // Deliberately NOT gated on the locomotion switch: jumping and reloading are not locomotion, and
    // a wearer who turns the sticks off should not silently lose their face buttons too.
    if (!g_hands.load(std::memory_order_relaxed)) {
        return;
    }

    auto& rt = vr::simulated_runtime();
    const auto right = rt.hand(vr::VRRuntime::Hand::RIGHT);
    const auto left = rt.hand(vr::VRRuntime::Hand::LEFT);
    auto& si = SyntheticInput::get();

    // ---- SPRINT: A LEVEL, TRACKED IN BOTH DIRECTIONS -------------------------------------------
    //
    // Handled before the active check, and RELEASED when the controller goes away: a sprint key left
    // down because a controller slept is a player who sprints forever with nothing to let go of.
    const bool want_sprint = left.active && (left.buttons & vr::VRRuntime::kButtonThumbstick) != 0u;

    if (want_sprint != m_sprinting.load(std::memory_order_relaxed)) {
        si.hold(m_sprint_vk.load(std::memory_order_relaxed), want_sprint);
        m_sprinting.store(want_sprint, std::memory_order_relaxed);
    }

    // ---- REFLEX TIME: ONE TAP PER PULL ---------------------------------------------------------
    //
    // Rising edge only, with the same hysteresis band the fire trigger uses. Because the game's
    // reflex is a toggle, a level would flip it once per frame for as long as the trigger was held.
    if (left.active) {
        const bool down = m_left_trigger_down ? (left.trigger > kTriggerRelease)
                                              : (left.trigger > kTriggerPress);

        if (down && !m_left_trigger_down) {
            if (si.tap(m_reflex_vk.load(std::memory_order_relaxed))) {
                m_reflex_toggles.fetch_add(1, std::memory_order_relaxed);
            }
        }

        m_left_trigger_down = down;
    }

    if (!right.active) {
        // Do not clear the remembered mask here. A controller that sleeps mid-press would otherwise
        // come back looking like a fresh press of every button that was down when it went away.
        return;
    }

    const uint32_t now = right.buttons;
    const uint32_t pressed = now & ~m_last_buttons;  // rising edges only
    m_last_buttons = now;

    // A TAP, NOT A HOLD. The engine consumes a press EDGE for both of these, and re-asserting the
    // key every frame overwrites the very transition it is looking for -- the same reason
    // SyntheticInput applies keys after the engine's own poll rather than before it.
    if ((pressed & vr::VRRuntime::kButtonA) != 0u) {
        if (si.tap(kKeyJump)) {
            m_jumps.fetch_add(1, std::memory_order_relaxed);
        }
    }

    if ((pressed & vr::VRRuntime::kButtonB) != 0u) {
        if (si.tap(kKeyReload)) {
            m_reloads.fetch_add(1, std::memory_order_relaxed);
        }
    }

    if ((pressed & vr::VRRuntime::kButtonThumbstick) != 0u) {
        if (si.tap(m_melee_vk.load(std::memory_order_relaxed))) {
            m_melees.fetch_add(1, std::memory_order_relaxed);
        }
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
    if (m_hide_body.load(std::memory_order_acquire)) {
        apply_body_visibility(false);
    }

    const bool pin = m_pin_eye.load(std::memory_order_acquire);
    const bool room = m_roomscale.load(std::memory_order_acquire);
    const float trim = m_eye_trim.load(std::memory_order_acquire);

    if (!pin && !room && trim == 0.0f) {
        return;
    }

    // Real elapsed time, so the easing below behaves the same at 60 fps and at 144.
    LARGE_INTEGER now{};
    ::QueryPerformanceCounter(&now);
    static const int64_t freq = [] {
        LARGE_INTEGER f{};
        ::QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();

    float dt = 0.0f;

    if (m_last_offset_tick != 0 && freq != 0) {
        dt = static_cast<float>(static_cast<double>(now.QuadPart - m_last_offset_tick) /
                                static_cast<double>(freq));
        // A hitch must not translate into a jump: clamped to a frame or two of easing.
        dt = dt < 0.0f ? 0.0f : (dt > 0.1f ? 0.1f : dt);
    }

    m_last_offset_tick = now.QuadPart;

    float dx = 0.0f;
    float dy = 0.0f;
    float dz = 0.0f;

    if (pin) {
        // ---- PLACE THE EYE, DO NOT MEASURE IT --------------------------------------------------
        //
        // Everything before this tried to CORRECT the engine's camera against a reference, and
        // every version of that failed in a different way: a captured reference baked in the
        // stance, a tracking one followed the artefact, a gated one left a dead zone, and a
        // smoothed one still drifted. The whole approach was wrong. The engine's eye position is
        // not a quantity worth preserving in VR -- it is posed for a flatscreen camera on a body
        // whose head bone swings on an 8.4 cm lever.
        //
        // So the eye is PLACED instead: a fixed offset from the player's ROOT, chosen by stance.
        // `eye_offset` is the camera relative to the body model, so subtracting it lands the eye
        // exactly on the root and the wanted height puts it back up. Nothing to reference, nothing
        // to track, nothing to drift, and the neck lever disappears because the engine's camera
        // position stops being an input at all.
        if (const auto off = sdk::PlayerMgr::eye_offset(0); off.has_value()) {
            const bool crouched = sdk::PlayerMgr::is_crouching(0).value_or(false);
            const float wanted = crouched ? m_eye_crouch.load(std::memory_order_acquire)
                                          : m_eye_stand.load(std::memory_order_acquire);

            // Horizontal goes to zero -- the eye sits directly above the root, which is the one
            // horizontal position that cannot swing with the head.
            m_pin_part = {-(*off)[0], wanted - (*off)[1], -(*off)[2]};
            dx += m_pin_part[0];
            dy += m_pin_part[1];
            dz += m_pin_part[2];
            m_have_eye_ref = true;
        }
    }

    if (room) {
        if (const auto* host = FramePublisher::get().host_state(); host != nullptr) {
            if (host->valid != 0u) {
                // THE RUNTIME RECENTRED. Its LOCAL space origin moved, so every position we have
                // been differencing against is meaningless -- recapture rather than carry the
                // error. This is the event that was missing when the camera ended up offset to one
                // side after a recentre in the headset.
                if (host->recenter_serial != m_last_recenter_serial) {
                    m_last_recenter_serial = host->recenter_serial;
                    m_recenter.store(true, std::memory_order_release);
                    LOGX("[vr] runtime recentred -- recapturing the roomscale origin");
                }

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

                const float wx = rx * c + rz * s;
                const float wz = -rx * s + rz * c;

                // The body path only takes the horizontal term if it can actually deliver it.
                // Otherwise the camera keeps it: trading a working offset for a refused write is
                // how "roomscale on, nothing moves" happened.
                if (m_room_body.load(std::memory_order_acquire) &&
                    sdk::PlayerMgr::can_displace_player(0)) {
                    // THE CHARACTER MOVES, so the camera must not: applying both would double every
                    // step. Vertical stays on the camera, where it cannot sink anyone through a
                    // floor.
                    m_room_part = {0.0f, ry, 0.0f};

                    if (m_have_last_room) {
                        // THE DELTA IS TAKEN IN PLAY SPACE AND ROTATED, never the other way round.
                        // wx/wz are the accumulated offset already expressed in the body's heading,
                        // so differencing THEM makes a turn look like a step: stand a metre off
                        // centre, turn 180 degrees without moving a muscle, and the two offsets
                        // differ by two metres. A play-space delta is zero whenever the wearer did
                        // not actually move, whatever the body is facing.
                        const float px = rx - m_last_room_xz[0];
                        const float pz = rz - m_last_room_xz[1];
                        const std::array<float, 3> step{px * c + pz * s, 0.0f, -px * s + pz * c};

                        // A dead band, because the headset never reads exactly still and a stream
                        // of sub-millimetre writes would fight the engine's own movement code for
                        // no visible benefit.
                        if (step[0] * step[0] + step[2] * step[2] > 0.01f) {
                            if (sdk::PlayerMgr::displace_player(0, step)) {
                                m_body_moves.fetch_add(1, std::memory_order_relaxed);
                            }
                        }
                    }

                    m_last_room_xz[0] = rx;
                    m_last_room_xz[1] = rz;
                    m_have_last_room = true;
                } else {
                    m_room_part = {wx, ry, wz};
                    m_have_last_room = false;
                }

                dx += m_room_part[0];
                dy += m_room_part[1];
                dz += m_room_part[2];
            }
        }
    }

    // THE TRIM IS INDEPENDENT of the pin, deliberately. It was applied inside the pin at first, and
    // therefore did nothing whenever the pin was still waiting for a neutral head -- a preference
    // that silently ignores you is worse than one that is not there.
    dy += trim;

    CameraPassHook::get().set_position_offset(dx, dy, dz);
}

VR::Neutrality VR::head_neutrality() const {
    const auto* host = FramePublisher::get().host_state();

    if (host == nullptr || host->valid == 0u) {
        return {};
    }

    const float x = host->orientation[0];
    const float y = host->orientation[1];
    const float z = host->orientation[2];
    const float w = host->orientation[3];

    // Euler angles in the runtime's frame: Y is up, so yaw is about Y and pitch about X. Unpacked
    // rather than judged from |w| alone, which was the previous test -- |w| measures TOTAL rotation
    // and cannot tell a level head turned sideways from a centred head looking down, and those two
    // have to be treated differently.
    const float sin_pitch = 2.0f * (w * x - y * z);
    const float pitch = asinf(sin_pitch < -1.0f ? -1.0f : (sin_pitch > 1.0f ? 1.0f : sin_pitch));
    const float yaw = atan2f(2.0f * (w * y + x * z), 1.0f - 2.0f * (y * y + z * z));
    const float roll = atan2f(2.0f * (w * z + x * y), 1.0f - 2.0f * (x * x + z * z));

    // FOUR DEGREES OF PITCH, not fifteen. The vertical offset moves 4 to 5 units per 10 degrees of
    // pitch near centre (74.78 level, 69.65 at -10, 78.93 at +10), so the old 15 degree cone left
    // up to 7 cm of vertical motion uncorrected. Invisible in open space; against a nearby wall the
    // parallax makes it obvious, which is exactly how it was found.
    constexpr float kPitchLimit = 4.0f * 0.0174532925f;
    constexpr float kRollLimit = 8.0f * 0.0174532925f;
    constexpr float kYawLimit = 8.0f * 0.0174532925f;

    Neutrality n{};
    n.pitch_level = fabsf(pitch) <= kPitchLimit && fabsf(roll) <= kRollLimit;
    n.fully = n.pitch_level && fabsf(yaw) <= kYawLimit;
    return n;
}
