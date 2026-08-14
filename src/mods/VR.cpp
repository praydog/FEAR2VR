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
#include "sdk/CClientShell.hpp"
#include "sdk/Model.hpp"
#include "sdk/Actions.hpp"
#include "sdk/WeaponMgr.hpp"
#include "SyntheticInput.hpp"
#include "HeadTracking.hpp"
#include "Log.hpp"

namespace {

std::atomic<bool> g_enabled{false};
std::atomic<uint64_t> g_applied{0};
std::atomic<bool> g_head_valid{false};
std::atomic<bool> g_hands{false};

// ---- ACTIONS, NOT KEYS -------------------------------------------------------------------------
//
// These used to be hardcoded virtual keys ('W', VK_SPACE, 'R', 'E', VK_SHIFT, ...), which is only
// correct on a default profile: rebind jump and the controller's jump button presses a key that no
// longer jumps. sdk::Actions resolves an action to whatever input the PLAYER has bound to the
// engine's own command id, so the controller drives the action rather than a key that usually means
// it.
//
// The values below are the shipped defaults, kept ONLY as a last resort for when no profile can be
// read at all -- see action_input(). They are the behaviour this mod already had, so falling back
// is never worse than before, and it is logged rather than silent.
constexpr uint32_t kDefaultForward = 'W';
constexpr uint32_t kDefaultBackward = 'S';
constexpr uint32_t kDefaultStrafeLeft = 'A';
constexpr uint32_t kDefaultStrafeRight = 'D';
constexpr uint32_t kDefaultJump = 0x20;    // VK_SPACE
constexpr uint32_t kDefaultReload = 0x52;  // 'R'
constexpr uint32_t kDefaultUse = 0x45;     // 'E'
constexpr uint32_t kDefaultCrouch = 0x43;  // 'C' -- only used if the profile has Crouch unbound

// Order matches the bit order in update_locomotion.
constexpr std::array<sdk::Action, 4> kLocoActions{sdk::Action::Forward, sdk::Action::Backward,
                                                  sdk::Action::StrafeLeft,
                                                  sdk::Action::StrafeRight};
constexpr std::array<uint32_t, 4> kLocoDefaults{kDefaultForward, kDefaultBackward,
                                                kDefaultStrafeLeft, kDefaultStrafeRight};

// The input to press for `action`. Resolved from the player's bindings; `fallback` is used only
// when the profile is unreadable, and that is reported once so a bug report can say so.
uint32_t action_input(sdk::Action action, uint32_t fallback) {
    if (const auto bound = sdk::Actions::input_for(action)) {
        return *bound;
    }
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true, std::memory_order_relaxed)) {
        LOGX("[vr] no bindings available -- falling back to the shipped default keys, which are "
             "wrong for any player who has rebound them");
    }
    return fallback;
}

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
constexpr uint8_t kDefaultFireButton = 0;

// Fire keeps the direct mouse path -- its ordering against the engine's own poll matters, see the
// call site -- but WHICH button comes from the binding. A player who has fire on a key rather than
// a mouse button is handled too: that lands above the mouse range and goes through SyntheticInput.
void issue_fire(bool down) {
    const uint32_t code = action_input(sdk::Action::Fire, 0x100u + kDefaultFireButton);
    if (code >= 0x100u) {
        sdk::Input::send_mouse_button(static_cast<uint8_t>(code - 0x100u), down);
    } else {
        SyntheticInput::get().hold(code, down);
    }
}

// Last head orientation seen, in both spaces. Reported so a consumer -- or the fixture -- can
// check the conversion against its input without recomputing it, which would just be asserting
// the same arithmetic twice.
std::atomic<float> g_head_rt[4]{};
std::atomic<float> g_head_eng[4]{};

// ---- THE POSE THAT CAME OVER THE WIRE, KEPT VERBATIM -------------------------------------------
//
// The host must state, in xrEndFrame, the pose the pixels were drawn from. The only value that is
// certainly that pose is the one the host itself sent and we then rendered with -- so it is latched
// here exactly as received, in the host's own space, and handed back untouched.
//
// It is NOT reconstructed from engine state. FrameCapture used to rebuild it from
// camera_rotation_operands and aim_yaw, which had three ways to come up empty; each failure
// published rendered_valid = 0 and made the host fall back to its index lookup, which is the stale
// pose the timewarp then reprojected against. That is what `absent 1490` counted in the host's own
// log, and it is positional because those getters depend on what the game's camera is doing.
//
// A round trip cannot be absent, cannot be approximate, and cannot disagree about spaces.
std::atomic<float> g_head_xr_wire[4]{};
std::atomic<bool> g_head_xr_wire_valid{false};

// STAGED at the shared-memory read, COMMITTED at the composition. Two stages because the value and
// the instant come from different places: only the read knows the exact bytes, and only the compose
// knows which frame they belong to.
std::atomic<float> g_head_xr_pending[4]{};
std::atomic<bool> g_head_xr_pending_valid{false};

} // namespace

// The three actions that carry a manual override. An explicit value wins so the fixture can pin a
// key; otherwise the player's own binding decides, with the historical default only if no profile
// can be read at all.
uint32_t VR::sprint_input() const {
    const uint32_t pinned = m_sprint_vk.load(std::memory_order_relaxed);
    return pinned != 0 ? pinned : action_input(sdk::Action::Sprint, 0x10);
}

uint32_t VR::melee_input() const {
    const uint32_t pinned = m_melee_vk.load(std::memory_order_relaxed);
    return pinned != 0 ? pinned : action_input(sdk::Action::Melee, 'V');
}

uint32_t VR::reflex_input() const {
    const uint32_t pinned = m_reflex_vk.load(std::memory_order_relaxed);
    return pinned != 0 ? pinned : action_input(sdk::Action::Reflex, 0x11);
}

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

// ---- LATE LATCHING, THE PART OF IT WE CAN ACTUALLY DO -------------------------------------------
//
// Meta's technique latches the pose on the GPU, immediately before the queued commands execute, so
// the render uses a pose sampled long after the CPU recorded the work. We cannot do that: D3D9, and
// the engine's shaders are not ours to rewrite.
//
// But the same principle has a large unclaimed win one level up. The pose is ingested at the START
// of the engine's update, in on_frame, and the view is not built until draw_scene_detour -- after
// all the game logic, on the same thread. Every millisecond of that update is latency we add for
// nothing. Re-reading the pose at the draw and correcting the camera is the CPU-side equivalent of
// the latch, and it is exact rather than predictive: this is where the head IS, not a guess about
// where it will be.
//
// THE DELTA MUST BE CONJUGATED BY THE HEADING, and the first version of this was wrong about that.
// HeadTracking does NOT write the head raw: it writes `heading * head * heading^-1` as the outer
// operand, so the head turns in the BODY's frame. Replacing the head therefore means
//
//     camera_new = heading * (head_new * head_old^-1) * heading^-1 * camera_old
//
// Left-multiplying by the bare delta, as this did, differs from that by a conjugation -- same
// magnitude, rotated axis, scaling with the size of the head motion. That is indistinguishable from
// the defect being hunted, which is a poor thing for a fix to introduce.
//
// The published sequence is advanced with it, so the frame is stamped with the pose it was actually
// drawn from. A latch that did not do that would buy latency and pay for it in exactly the
// mis-association this whole hunt has been about.
std::optional<std::array<float, 4>> VR::late_latch_head() {
    if (!m_late_latch.load(std::memory_order_relaxed)) {
        return std::nullopt;
    }

    const auto* host = FramePublisher::get().host_state();
    if (host == nullptr) {
        return std::nullopt;
    }

    const uint32_t seq = host->sequence;
    if ((seq & 1u) != 0u || host->valid == 0u) {
        return std::nullopt;  // mid-write, or the runtime has no pose
    }

    const std::array<float, 4> fresh{host->orientation[0], host->orientation[1],
                                     host->orientation[2], host->orientation[3]};
    if (host->sequence != seq) {
        return std::nullopt;  // torn
    }

    // PENDING, not committed. Reading a pose is not the same instant as RENDERING with one: the
    // read happens in the mod's tick, the composition into the camera's additive slot happens later
    // inside PlayerCamera_UpdateAttachedRotation, and the readback the host consumes is a frame
    // deep on top of that. Stamping a captured frame with the pose read at the START of that chain
    // describes an earlier instant than the pixels, which is a frame or two of lag -- visible, and
    // the one thing the old reconstruction got right, because it read the engine's holder AFTER the
    // fact and so could not be early.
    //
    // So this only stages the value. HeadTracking commits it at the moment it actually composes,
    // which is what defines what the frame is drawn from.
    for (size_t i = 0; i < 4; ++i) {
        g_head_xr_pending[i].store(fresh[i], std::memory_order_relaxed);
    }
    g_head_xr_pending_valid.store(true, std::memory_order_release);

    const auto now = runtime_to_engine_rotation(fresh);
    const std::array<float, 4> was{g_head_eng[0].load(std::memory_order_relaxed),
                                   g_head_eng[1].load(std::memory_order_relaxed),
                                   g_head_eng[2].load(std::memory_order_relaxed),
                                   g_head_eng[3].load(std::memory_order_relaxed)};
    const float n = was[0] * was[0] + was[1] * was[1] + was[2] * was[2] + was[3] * was[3];
    if (n < 0.5f) {
        return std::nullopt;  // nothing composed yet this session
    }

    // delta = now * conj(was), both unit, so the conjugate is the inverse.
    const std::array<float, 4> inv{-was[0], -was[1], -was[2], was[3]};
    const std::array<float, 4> d{
        now[3] * inv[0] + now[0] * inv[3] + now[1] * inv[2] - now[2] * inv[1],
        now[3] * inv[1] - now[0] * inv[2] + now[1] * inv[3] + now[2] * inv[0],
        now[3] * inv[2] + now[0] * inv[1] - now[1] * inv[0] + now[2] * inv[3],
        now[3] * inv[3] - now[0] * inv[0] - now[1] * inv[1] - now[2] * inv[2]};

    // ---- NOTHING IS COMMITTED UNTIL THE WHOLE LATCH CAN SUCCEED --------------------------------
    //
    // This lookup used to sit BELOW the state advance, and that ordering was the bug behind "one
    // area is laggy the whole time while every counter reads clean":
    //
    //   - m_late_latches counted the ATTEMPT, so a failed latch still read as a success. Measured
    //     1.00 latches per frame in the bad area, which was a false assurance rather than evidence.
    //   - m_last_host_seq_pub advanced, so FrameCapture stamped the NEW sequence onto pixels that
    //     were drawn with the PREVIOUS pose, and the host reprojected against a pose the frame was
    //     never rendered from.
    //   - worst, g_head_eng advanced to `now`. That is the reference the NEXT delta is measured
    //     from, so the correction silently lost a step and never got it back. Repeated every frame
    //     the lookup fails, which is why the lag was constant in one place rather than a glitch:
    //     the camera holder reads zero in scripted and non-standard camera states.
    //
    // The delta is applied about the body's yaw, so without a heading there is no correct axis to
    // apply it about -- bailing is right. Bailing HALFWAY is what was wrong.
    const auto heading = sdk::PlayerMgr::aim_yaw(0);
    if (!heading.has_value()) {
        return std::nullopt;
    }

    for (size_t i = 0; i < 4; ++i) {
        g_head_eng[i].store(now[i], std::memory_order_relaxed);
    }
    m_last_host_sequence = seq;
    m_last_host_seq_pub.store(seq, std::memory_order_release);
    m_late_latches.fetch_add(1, std::memory_order_relaxed);

    const float hh = *heading * 0.5f;
    const std::array<float, 4> hq{0.0f, std::sin(hh), 0.0f, std::cos(hh)};
    const std::array<float, 4> hqi{0.0f, -std::sin(hh), 0.0f, std::cos(hh)};
    auto qmul = [](const std::array<float, 4>& a, const std::array<float, 4>& b) {
        return std::array<float, 4>{a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
                                    a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
                                    a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
                                    a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
    };
    return qmul(qmul(hq, d), hqi);
}

std::array<float, 4> VR::runtime_to_engine_rotation(const std::array<float, 4>& q) {
    // Mirror along Z: conjugating a rotation by diag(1,1,-1) negates X and Y, leaves Z and W.
    return {-q[0], -q[1], q[2], q[3]};
}

std::array<float, 3> VR::runtime_to_engine_position(const std::array<float, 3>& p) {
    return {p[0] * kUnitsPerMetre, p[1] * kUnitsPerMetre, -p[2] * kUnitsPerMetre};
}

// Called from the composition detour, at the instant the pose is written into the camera's additive
// slot. THAT is the moment that defines what the frame renders from -- everything captured after it
// and before the next composition belongs to this pose.
void VR::commit_wire_head_pose() {
    if (!g_head_xr_pending_valid.load(std::memory_order_acquire)) {
        return;
    }
    for (size_t i = 0; i < 4; ++i) {
        g_head_xr_wire[i].store(g_head_xr_pending[i].load(std::memory_order_relaxed),
                                std::memory_order_relaxed);
    }
    g_head_xr_wire_valid.store(true, std::memory_order_release);
}

bool VR::wire_head_pose(float out[4]) {
    if (!g_head_xr_wire_valid.load(std::memory_order_acquire)) {
        return false;
    }
    for (size_t i = 0; i < 4; ++i) {
        out[i] = g_head_xr_wire[i].load(std::memory_order_relaxed);
    }
    return true;
}


void VR::set_trigger_enabled(bool enabled) {
    g_trigger.store(enabled, std::memory_order_relaxed);

    if (!enabled && g_firing.exchange(false, std::memory_order_relaxed)) {
        // NEVER LEAVE THE TRIGGER HELD. A latched fire button outlives this mod -- the engine keeps
        // the state -- and would burn the magazine with nobody driving it.
        issue_fire(false);
        if (m_sprinting.exchange(false, std::memory_order_relaxed)) {
            // Engine state OUTLIVES this DLL, so a held key is ours to put back. Leaving Shift down
            // through an uninject leaves the player sprinting with no controller attached to it.
            SyntheticInput::get().hold(sprint_input(), false);
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
    // THE TIMEOUT IS TWO COMPOSITOR FRAMES, AND 22 WAS TWO FRAMES AT 90 Hz (11.1 x 2 = 22.2).
    //
    // This runtime presents at 72 Hz, where the period is 13.9 ms and two frames is 27.8 -- so the
    // old constant was 1.58 periods, not two, and a single slow host iteration overran it. The host
    // log shows exactly that headroom being eaten: per-frame wait maxes at 11.6 ms with upload
    // spikes to 10.2 ms on the same iteration.
    //
    // A wait that expires does NOT stall the game; the return is discarded and the frame publishes
    // free-running, so its pose age slips a step relative to its neighbours -- one frame warped by
    // a different amount than the frames either side of it, which is a single visible jump.
    //
    // SCOPE OF THE CLAIM: measured live at 0.5 timeouts/s against 70.7 frames/s, so this population
    // is 0.7% of frames and can only be about the OCCASIONAL jitter. It is explicitly NOT an
    // explanation for continuous lag in one location -- that is steady-state and this is sporadic,
    // and no timestamp correlation between these timeouts and a seen jump has been measured yet.
    //
    // 34 ms is ~2.45 periods at 72 Hz: comfortably past two frames, still far short of the give-up
    // path (30 consecutive timeouts) that exists for a host which is genuinely gone.
    //
    // It SHOULD be derived from the runtime's own display period rather than assumed. Until the
    // host publishes that, this is a constant that has now been wrong once for exactly that reason.
    // ---- THE XR FRAME PROTOCOL IS NOT OPTIONAL ---------------------------------------------------
    //
    // This block used to sit behind m_paced, which DEFAULTS FALSE -- so out of the box the game
    // issued no WAIT, BEGIN or END at all and the architecture existed only when a legacy toggle
    // happened to be on. That is the same defect as late_latch defaulting off, which left the pose
    // path publishing nothing for an entire session while every counter read healthy.
    //
    // A v8 host means the protocol is available, and available means used. m_paced now gates only
    // the LEGACY tick fallback, which is all it was ever really about.
    {
        // ---- THE FRAME LOOP, DRIVEN FROM HERE ---------------------------------------------------
        //
        // WAIT then BEGIN, both blocking, at the game's own once-per-frame boundary. The runtime
        // throttles whoever blocks in xrWaitFrame, and that is now THIS thread -- so pacing and ASW
        // apply to the process doing the work instead of being relayed to it as a cadence. See
        // FRAME_LOOP.md; the measurement that motivated it is in there too.
        //
        // FALLS BACK, deliberately: without the handshake block (an older host, or one not up yet)
        // the old tick path still paces us. A missing host must degrade to the previous behaviour,
        // not to no pacing at all.
        auto& fp = FramePublisher::get();
        if (m_frame_rpc.load(std::memory_order_relaxed) && fp.handshake_ready()) {
            // ---- CLOSE THE PREVIOUS FRAME BEFORE OPENING ANOTHER -----------------------------
            //
            // FrameCapture::on_present is where END normally goes, and at the MAIN MENU that
            // function never runs at all -- the menu draws no scene, and the frame counter sat at
            // 0/s while the UI published at 53/s, which is how this was found. So the begin was
            // never matched, the host waited its whole 100 ms for an END nobody sent, and the game
            // then waited on that host iteration: a timeout cascade, measured in the headset as
            // roughly 1 fps at the menu and a 40-60 fps sag in world.
            //
            // A begin that is still outstanding here means the render path did not close it. Close
            // it now, before asking for another frame, so exactly one END follows every BEGIN
            // whatever path the game took. Belongs at the TOP of the update rather than the bottom
            // for the same reason the guard in on_present is a destructor: there is no path out of a
            // frame that can skip it.
            if (m_frame_begun.load(std::memory_order_relaxed)) {
                fp.xr_end();
                m_frame_begun.store(false, std::memory_order_relaxed);
            }
            const auto lease = fp.xr_wait();
            m_frame_lease_ok.store(lease.ok, std::memory_order_relaxed);
            if (lease.ok) {
                // BEGIN before anything is drawn -- the spec's order, and the reason this is not
                // simply folded into the WAIT reply. A failed begin means we must not ask for END.
                m_frame_begun.store(fp.xr_begin(), std::memory_order_relaxed);
            } else {
                m_frame_begun.store(false, std::memory_order_relaxed);
            }
        } else if (m_paced.load(std::memory_order_acquire)) {
            // No handshake block: an older host, or one not up yet. Fall back to the relayed tick,
            // which is what m_paced has always meant. It comes out with the rest of the pacing
            // machinery once the handshake is exercised against the trace baseline.
            fp.wait_for_host_tick();
        }
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
                // ---- ORIENTATION ONLY, AS A DIAGNOSTIC -------------------------------------
                //
                // Splits a distance-dependent error from a distance-independent one, which no
                // amount of reasoning has managed to do. Rotating the head moves EVERYTHING by
                // the same angle whatever its distance; TRANSLATING it moves near things more
                // than far ones. The eyes orbit the neck, so every rotation carries both.
                //
                // With position frozen, a judder that is really translation disappears and one
                // that is really rotation is untouched. The view will feel pinned in place --
                // that is the point, not a regression.
                if (!m_head_position.load(std::memory_order_relaxed)) {
                    pose.position = {0.0f, 0.0f, 0.0f};
                }
                pose.valid = true;
                pose.tracked = true;

                if (host->sequence == seq) {  // still unchanged: the read was clean
                    rt.set_head_pose(pose);
                    // WHICH THREAD APPLIES THE POSE, recorded because the whole question of
                    // pose-to-frame association turns on whether this is the thread that draws.
                    // If it is, ingest and render are one ordered sequence and the association is
                    // exact. If it is not, the renderer can be drawing a camera built from an
                    // OLDER tick while we stamp the newest pose onto it.
                    m_apply_tid.store(::GetCurrentThreadId(), std::memory_order_relaxed);
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
    issue_fire(now);
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
        for (size_t i = 0; i < kLocoActions.size(); ++i) {
            si.hold(action_input(kLocoActions[i], kLocoDefaults[i]), false);
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
        for (size_t i = 0; i < kLocoActions.size(); ++i) {
            const uint32_t bit = 1u << i;
            if ((wanted & bit) != (m_held_keys & bit)) {
                si.hold(action_input(kLocoActions[i], kLocoDefaults[i]), (wanted & bit) != 0u);
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
    // ASK THE GAME. CClientWeapon+0x38 is the object the weapon draws as, so there is nothing to
    // search for and nothing to be fooled by.
    //
    // This replaced a nearest-client-only-visible-model-under-weapons\ search, which worked right
    // up until the trigger was pulled: muzzle effects and shell casings are also client-only weapon
    // models and spawn AT THE MUZZLE, nearer than the gun, so the selector flapped across 43
    // objects in three seconds of fire and the weapon visibly snapped back to its engine position.
    // The heuristic is what FOUND this field -- it produced a known-good pointer to match against --
    // and then it was deleted rather than left beside its replacement.
    return sdk::WeaponMgr::current_weapon_model(0);
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
    // ---- THE CONTROLLER, IN WORLD SPACE ----------------------------------------------------
    //
    // ABSOLUTE, NOT A DELTA. An amended transform still carries everything the engine wanted the
    // gun to do -- the camera's rotation, recoil, sway, the weapon animation -- so the wearer sees
    // their controller's contribution ON TOP of all of it and, correctly, does not call that
    // "attached to my hand". Welding it means discarding the engine's value entirely.
    //
    // The pose is built relative to the HEAD rather than to a captured rest pose, because the
    // physical offset from headset to controller is the thing the wearer can actually see: hold the
    // controller a foot to the right of your face and the gun should be a foot to the right of the
    // view, at any facing, with no calibration step.
    float px = m_weapon_probe[0].load(std::memory_order_relaxed);
    float py = m_weapon_probe[1].load(std::memory_order_relaxed);
    float pz = m_weapon_probe[2].load(std::memory_order_relaxed);
    std::array<float, 4> turn{0.0f, 0.0f, 0.0f, 1.0f};
    bool absolute = false;
    uintptr_t anchor_obj = 0;

    auto& rt = vr::simulated_runtime();
    const auto hand = rt.hand(vr::VRRuntime::Hand::RIGHT);
    const auto head = rt.head();
    const auto objs = sdk::PlayerMgr::engine_objects(0);
    const auto cam = (objs.has_value() && objs->camera != 0)
                         ? sdk::object_info(reinterpret_cast<const regenny::LTObject*>(objs->camera))
                         : std::nullopt;

    if (hand.active && hand.aim.valid && head.valid && cam.has_value()) {
        // ---- X/Z FROM THE HEAD, Y FROM THE ROOM ------------------------------------------
        //
        // The two references are different ON PURPOSE, because the camera they anchor to treats the
        // axes differently. Horizontally the engine's camera FOLLOWS the head -- that is what
        // roomscale does -- so a head-relative offset is consistent with it. Vertically it does
        // NOT: the eye is PLACED at a fixed height above the player's root by stance, precisely so
        // that ducking does not sink the character. Measuring the controller against the head's
        // real Y therefore compares against a reference the anchor does not share, and the whole
        // difference lands on the gun: crouch in the room with the controller resting on a desk and
        // the weapon rises by exactly how far the wearer dropped.
        //
        // So height is measured against the ROOM ORIGIN -- the play-space reference the eye pin is
        // itself defined against -- which does not move when the wearer ducks.
        const std::array<float, 3> from_head{hand.aim.position[0] - head.position[0],
                                             hand.aim.position[1] - m_room_origin[1],
                                             hand.aim.position[2] - head.position[2]};
        const auto e = runtime_to_engine_position(from_head);

        // Into the world, by the BODY's heading. Using the view here would put the head back into
        // the result, which is the whole thing being removed.
        const auto yaw = sdk::PlayerMgr::aim_yaw(0);
        const float cy = yaw.has_value() ? cosf(*yaw) : 1.0f;
        const float sy = yaw.has_value() ? sinf(*yaw) : 0.0f;

        // ---- THE VERTICAL ANCHOR IS THE PLAYER'S ROOT, NOT THE CAMERA -------------------
        //
        // The camera OBJECT's height swings with head pitch -- measured at 29-31 units across a
        // look up and down, in lockstep with the engine's own eye_height. The eye PIN stabilises
        // the RENDERED eye, not this object, so anchoring the gun here inherited the full swing:
        // hold the controller still, look down, and the weapon rises by a third of a metre.
        //
        // The player model's position is pitch-invariant -- measured swing 0.00 over the same
        // sweep -- so height is anchored there plus the eye height the pin is placing the view at.
        // Horizontal stays on the camera, which is correct and already behaves: it FOLLOWS the head
        // there, which is what makes a head-relative X/Z offset consistent with it.
        const auto crouched = sdk::PlayerMgr::is_crouching(0);
        const float eye = crouched.value_or(false) ? m_eye_crouch.load(std::memory_order_acquire)
                                                   : m_eye_stand.load(std::memory_order_acquire);
        const auto root = (objs.has_value() && objs->model != 0)
                              ? sdk::object_info(reinterpret_cast<const regenny::LTObject*>(objs->model))
                              : std::nullopt;
        const float base_y = root.has_value() ? root->position.y + eye : cam->position.y;

        // ---- ALL THREE AXES ANCHOR ON THE ROOT ------------------------------------------
        //
        // The camera OBJECT carries the engine's own neck-bone orbit -- vertically it swings 29-31
        // units across a look up and down -- and anchoring to it put that orbit straight into the
        // gun. The player's root does not move with head rotation at all (measured swing 0.00), and
        // body roomscale keeps it following the head's real travel 1:1 (45.37 cm -> 45.39 units),
        // so it is both stable under rotation and correct under movement.
        //
        // CONFIRMED IN THE HEADSET, which is the only reason this is written the way it is: an
        // attempt to improve on it afterwards -- referencing the head position roomscale had
        // committed, to cover displace_player's dead band and collision -- was reverted because the
        // owner reported it broken. The dead-band leak is real and is NOT worth chasing; it costs
        // less than the thing that fixes it.
        // AN OFFSET FROM THE ROOT, not a world position: the detour adds the root's position at
        // the instant it writes the transform. Finishing the sum here computes it before the engine
        // moves the player and applies it after, which is the slight trailing seen when walking.
        anchor_obj = (root.has_value() && objs.has_value()) ? objs->model : 0;

        px += e[0] * cy + e[2] * sy;
        py += eye + e[1];
        pz += -e[0] * sy + e[2] * cy;

        if (anchor_obj == 0) {
            // No anchor to resolve later, so fall back to a finished world position.
            px += cam->position.x;
            py += base_y - eye;
            pz += cam->position.z;
        }

        // The controller's ABSOLUTE orientation, converted and then turned into the world by the
        // same heading. No rest pose and no delta: whatever the controller points at, the gun does.
        const auto q = runtime_to_engine_rotation(hand.aim.orientation);
        const float half = yaw.value_or(0.0f) * 0.5f;
        const regenny::LTRotation ry{0.0f, sinf(half), 0.0f, cosf(half)};
        const auto world = sdk::multiply_rotations(ry, regenny::LTRotation{q[0], q[1], q[2], q[3]});
        turn = {world.x, world.y, world.z, world.w};
        absolute = true;
    }

    // ONE OWNER: ViewHook already hooks LTObject_SetPosRot, so the amendment is handed to it
    // rather than hooked a second time. Installing a second inline hook on that address crashed
    // the game on unload -- see Hooks::install.
    // The ANCHOR, published separately from the placement: when the gun drifts, the question is
    // always whether the offset moved or the thing it is measured from did, and one number cannot
    // answer it.
    m_weapon_anchor[0].store(cam.has_value() ? cam->position.x : 0.0f, std::memory_order_relaxed);
    m_weapon_anchor[1].store(m_weapon_place[1].load(std::memory_order_relaxed),
                             std::memory_order_relaxed);
    m_weapon_anchor[2].store(cam.has_value() ? cam->position.z : 0.0f, std::memory_order_relaxed);
    m_weapon_abs.store(absolute, std::memory_order_relaxed);
    // PUBLISHED AS A WORLD POSITION even though an offset is what gets handed over, because a
    // consumer comparing this against anything else in the world needs the same units it does. The
    // offset alone read as a wild swing in a diagnostic that subtracted the player's position from
    // it -- a measurement invalidated by the very change it was checking.
    const auto pub_root =
        anchor_obj != 0 ? sdk::object_info(reinterpret_cast<const regenny::LTObject*>(anchor_obj))
                        : std::nullopt;
    const float pub_x = pub_root.has_value() ? pub_root->position.x + px : px;
    const float pub_y = pub_root.has_value() ? pub_root->position.y + py : py;
    const float pub_z = pub_root.has_value() ? pub_root->position.z + pz : pz;
    m_weapon_place[0].store(pub_x, std::memory_order_relaxed);
    m_weapon_place[1].store(pub_y, std::memory_order_relaxed);
    m_weapon_place[2].store(pub_z, std::memory_order_relaxed);
    ViewHook::get().set_weapon_amend(obj, {px, py, pz}, turn, absolute, anchor_obj);
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
        si.hold(sprint_input(), want_sprint);
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
            if (si.tap(reflex_input())) {
                m_reflex_toggles.fetch_add(1, std::memory_order_relaxed);
            }
        }

        m_left_trigger_down = down;
    }

    // ---- LEFT Y: CYCLE WEAPON, VIA THE WHEEL ---------------------------------------------------
    //
    // The literal Y button on the left controller, which the host publishes as its own bit from
    // /user/hand/left/input/y/click -- not the left hand's B-equivalent, and scoped by subaction
    // path so it cannot be confused with the right controller's.
    //
    // Sent as a real wheel notch rather than a key, because that is what the game binds weapon
    // cycling to. Rising edge only: the wheel is an impulse, and a held button would spin the
    // whole arsenal past the player at frame rate.
    if (left.active) {
        const uint32_t left_now = left.buttons;
        const uint32_t left_pressed = left_now & ~m_last_left_buttons;
        m_last_left_buttons = left_now;

        if ((left_pressed & vr::VRRuntime::kButtonY) != 0u) {
            si.queue_wheel(1);
            m_weapon_cycles.fetch_add(1, std::memory_order_relaxed);
        }

        // CROUCH ON X. A tap, like Jump and Reload above and for the same reason: the engine
        // consumes a press EDGE, so re-asserting the key every frame would overwrite the very
        // transition it is watching for.
        //
        // Whether that reads as a toggle or a hold is the GAME's business, not ours -- FEAR2 has a
        // crouch-toggle option, and a single edge does the right thing under either setting, where
        // holding the key would fight the toggle. The binding comes from the wearer's own profile;
        // the constant is only a fallback for a profile with Crouch unbound.
        if ((left_pressed & vr::VRRuntime::kButtonX) != 0u) {
            if (si.tap(action_input(sdk::Action::Crouch, kDefaultCrouch))) {
                m_crouches.fetch_add(1, std::memory_order_relaxed);
            }
        }
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
        if (si.tap(action_input(sdk::Action::Jump, kDefaultJump))) {
            m_jumps.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // DUAL-BOUND: reload AND use, from one press. The engine gates each on its own conditions -- a
    // reload with a full magazine is a no-op, and use with nothing in reach is a no-op -- so in any
    // given moment at most one of them has an effect, and the wearer gets a single button that does
    // the obvious thing whether they are standing at a door or holding an empty gun.
    //
    // Two taps in one frame is safe: SyntheticInput has 16 key slots and each claims its own, so
    // neither overwrites the other's edge.
    if ((pressed & vr::VRRuntime::kButtonB) != 0u) {
        if (si.tap(action_input(sdk::Action::Reload, kDefaultReload))) {
            m_reloads.fetch_add(1, std::memory_order_relaxed);
        }
        if (si.tap(action_input(sdk::Action::Use, kDefaultUse))) {
            m_uses.fetch_add(1, std::memory_order_relaxed);
        }
    }

    if ((pressed & vr::VRRuntime::kButtonThumbstick) != 0u) {
        if (si.tap(melee_input())) {
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

// ---- THE FRAME THE HAND BONE ACTUALLY LIVES IN -------------------------------------------------
//
// A controller pose is measured in the runtime's ROOM space. The bone it drives hangs off the
// player object, whose whole chain rotates with the body and the view. Nothing converted between
// the two, so the offset was reinterpreted in whatever direction the character happened to face:
// an error of (hand offset) x (rotation since the rest pose was captured). Standing still it looks
// perfect; turning, the hand swings out and settles, which reads as LAG THAT SCALES WITH HOW MUCH
// YOU MOVE -- and is the reason every rate in the pipeline measured full speed (143.6 callbacks and
// writes per second, record consistent on every one) while the hands still looked like they updated
// a few times a second. Nothing was late; the values were wrong.
//
// The SHELL player object, not PlayerMgr's: they are different allocations and only this one's
// rotation is written (AGENTS.md, "the two player objects"). It is also the object the first-person
// rig hangs off, so its rotation IS the parent frame of these bones.
//
// Read on the game thread, once per hand per frame. Ideally this would be read inside the
// node-control callback, which is the exact instant the engine evaluates the skeleton -- that costs
// at most a frame of the body's rotation, against the unbounded error being removed here, and the
// callback is deliberately kept free of calls outside its own translation unit.
std::optional<regenny::LTRotation> body_frame_rotation() {
    const auto p = sdk::CClientShell::local_player(0);
    if (!p.has_value() || p->object == nullptr) {
        return std::nullopt;
    }
    const auto info = sdk::object_info(reinterpret_cast<const regenny::LTObject*>(p->object));
    if (!info.has_value()) {
        return std::nullopt;
    }
    return info->rotation;
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

    // INTO THE CHARACTER'S FRAME. conj(body) takes a room-space vector into the space the bone's
    // parent chain is expressed in. Without a body rotation we leave the delta as-is rather than
    // dropping the hand: uncompensated tracking is the old behaviour, and still better than none.
    const auto body = body_frame_rotation();
    std::array<float, 3> local_delta = engine_delta;
    if (body.has_value()) {
        const regenny::LTRotation inv{-body->x, -body->y, -body->z, body->w};
        const auto v = sdk::rotate_vector(inv, regenny::LTVector{engine_delta[0], engine_delta[1],
                                                                 engine_delta[2]});
        local_delta = {v.x, v.y, v.z};
    }

    for (size_t i = 0; i < 3; ++i) {
        g_hand_off[slot][i].store(local_delta[i], std::memory_order_relaxed);
    }

    bc.set_offset(local_delta[0], local_delta[1], local_delta[2], slot);

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

    // SAME CONVERSION, for the same reason. The callback applies this as q * current, with current
    // in the bone's parent space, so q has to be in that space too -- conj(body) * q_room. Leaving
    // the orientation in room space is what made the hands rotate away from where they were
    // pointing as the body turned, distinct from and on top of the positional swing above.
    std::array<float, 4> local_rot = engine_rot;
    if (body.has_value()) {
        const regenny::LTRotation inv{-body->x, -body->y, -body->z, body->w};
        const auto r = sdk::multiply_rotations(
            inv, regenny::LTRotation{engine_rot[0], engine_rot[1], engine_rot[2], engine_rot[3]});
        local_rot = {r.x, r.y, r.z, r.w};
    }

    for (size_t i = 0; i < 4; ++i) {
        g_hand_rot[slot][i].store(local_rot[i], std::memory_order_relaxed);
    }

    bc.set_rotation(local_rot[0], local_rot[1], local_rot[2], local_rot[3], slot);
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

                        // A DEAD BAND THAT ACCUMULATES RATHER THAN DISCARDS. The band itself is
                        // wanted: the headset never reads exactly still, and a stream of
                        // sub-millimetre writes would fight the engine's own movement code for no
                        // visible benefit.
                        //
                        // But dropping the step while ADVANCING the reference throws the motion
                        // away, and slow movement is nothing but sub-threshold steps -- so walking
                        // sideways slowly moved the character not at all, while the same distance
                        // covered briskly worked. Vertical was unaffected because the camera keeps
                        // that term and never went through this gate, which is exactly the "except
                        // the Y axis" in the report.
                        //
                        // Holding the reference back until a move COMMITS is the whole fix: the
                        // delta is then measured from the last position actually delivered, so it
                        // grows until it crosses the band and nothing is ever lost. No accumulator
                        // is needed -- the reference IS the accumulator.
                        if (step[0] * step[0] + step[2] * step[2] > 0.01f) {
                            if (sdk::PlayerMgr::displace_player(0, step)) {
                                m_body_moves.fetch_add(1, std::memory_order_relaxed);
                            }
                            m_last_room_xz[0] = rx;
                            m_last_room_xz[1] = rz;
                        }
                    } else {
                        m_last_room_xz[0] = rx;
                        m_last_room_xz[1] = rz;
                    }

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
