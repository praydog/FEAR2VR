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
    // Let the OpenXR runtime drive the game's frame rate, the way xrWaitFrame does for a native VR
    // title. Off by default: it changes the game's pacing, which is not something to do implicitly.
    // ---- WHERE THE EYE IS, RATHER THAN WHERE THE ENGINE PUTS IT --------------------------------
    //
    // PIN: hold the eye at the body's own height plus its eye height, cancelling the vertical swing
    // the engine applies as the view pitches -- measured at 54 cm down at -80 degrees, and
    // non-monotonic going up. Real heads do not do that, and in a headset the wearer's head has not
    // moved at all, so the engine's curve is pure artefact.
    //
    // ROOMSCALE: add the wearer's movement within their play space. Captured relative to wherever
    // they were when it was switched on, so enabling it never teleports anyone.
    void update_camera_offset();
    void apply_body_visibility(bool visible);

    // ---- WHEN THE EYE REFERENCE MAY BE SAMPLED -------------------------------------------------
    //
    // The pin cancels the neck orbit by holding the eye offset at the value it has with the head
    // UNROTATED, so the reference may only be taken at moments when the orbit is known to be zero.
    // A constant reference cannot work (a crouch legitimately moves the eye ~29 units and the pin
    // would "correct" it back); a continuously tracking one cannot either (it would follow the
    // orbit and cancel nothing). Gating the update on neutrality is what makes it work at all.
    //
    // PER AXIS, because the artefact is not the same shape on each. Swept live at the reported
    // problem spot: the VERTICAL offset depends on pitch ALONE -- identical to the hundredth across
    // every yaw from -20 to +20 degrees -- while X and Z depend on both. So the vertical reference
    // may be refreshed at any heading provided the head is level, which lets it re-sync to stance
    // far more often than a single all-axis test would allow.
    struct Neutrality {
        bool pitch_level{};  // may refresh the VERTICAL reference
        bool fully{};        // may refresh the horizontal reference too
    };

    Neutrality head_neutrality() const;

    // ---- HOW FAST THE REFERENCE IS ALLOWED TO MOVE ---------------------------------------------
    //
    // The reference must follow the body -- stance, crouch, whatever the engine legitimately does
    // to eye height -- but it must NOT be seen doing it. Snapping it to the live value on every
    // level frame made the eye height visibly restless: the engine's own bob and footstep motion
    // came straight through, and the wearer felt the correction working rather than the result.
    //
    // So the reference is eased toward the live value with a time constant instead of assigned.
    // Fast head rotation still gets cancelled in full, because the gate closes the moment the head
    // leaves level and the reference simply holds; only the SLOW following is slowed further.
    void set_eye_smoothing_ms(float ms);
    float eye_smoothing_ms() const { return m_eye_tau_ms.load(std::memory_order_acquire); }
    bool head_is_neutral() const { return head_neutrality().fully; }

    // False while a recenter is still waiting for the wearer to look ahead. Published, because
    // "waiting" and "broken" are indistinguishable from inside a headset otherwise.
    bool eye_reference_captured() const { return m_have_eye_ref; }

    // The two contributions, published separately. A combined number cannot say WHICH half is
    // displacing the wearer, and the two have completely different causes.
    std::array<float, 3> pin_contribution() const { return m_pin_part; }
    std::array<float, 3> room_contribution() const { return m_room_part; }
    std::array<float, 3> eye_reference() const { return m_eye_ref_vec; }
    // ---- THE ENGINE'S OWN CAMERA OFFSET, NEUTRALISED -------------------------------------------
    //
    // The engine builds the camera as `socket_pose.position + rotate(CameraAttachedOffset, camera
    // rotation)`. That offset is a fixed vector in the CAMERA's frame, so once a headset is driving
    // the rotation, every head movement swings the eye around a pivot: measured at 11.8 units of
    // travel for a 90 degree yaw and 29 units for an 80 degree pitch, felt in the headset as the
    // camera sliding backwards when looking up.
    //
    // For a flatscreen FPS that offset is a deliberate framing choice. In VR it is a lever arm on
    // the wearer's neck, and the honest value is zero -- the headset already reports where the head
    // actually is. Three float stores through the cached console variables; the originals are kept
    // so switching this off restores the game exactly.
    void set_neutral_camera_offset(bool on);
    bool neutral_camera_offset() const { return m_neutral_cam_off.load(std::memory_order_acquire); }

    // ---- EYE HEIGHT TRIM -----------------------------------------------------------------------
    //
    // Added to the pinned eye position, in engine units (one unit is one centimetre). The engine's
    // own camera sits where a flatscreen shooter wants it, which in a headset puts the wearer
    // inside the character's chest -- the model's eye socket is not the same thing as a comfortable
    // viewpoint, and no amount of correctness elsewhere fixes it. This is a preference, so it is a
    // number rather than a switch.
    // ---- WHERE THE EYE GOES, PER STANCE --------------------------------------------------------
    //
    // A fixed height above the player's ROOT, one value standing and one crouched. Not derived from
    // the engine's camera, which is posed for a flatscreen view on a body whose head bone swings on
    // a lever -- placing the eye outright is what makes the neck orbit vanish rather than be
    // cancelled. Defaults are the measured live values: 74.98 standing, 45.50 crouched.
    void set_eye_heights(float standing, float crouched);
    float eye_standing() const { return m_eye_stand.load(std::memory_order_acquire); }
    float eye_crouched() const { return m_eye_crouch.load(std::memory_order_acquire); }

    void set_eye_height_trim(float units);
    float eye_height_trim() const { return m_eye_trim.load(std::memory_order_acquire); }

    // ---- THE PLAYER'S OWN BODY -----------------------------------------------------------------
    //
    // Hide the client-side player model. In "aim with the head" mode the body is a liability: it is
    // posed for a flatscreen camera, so from an eye inside it the wearer sees the inside of a
    // chest and a neck. The weapon and hands are a SEPARATE object -- the viewmodel -- so they
    // survive this untouched, which is exactly the split that makes it worth doing at all.
    //
    // Reapplied every frame rather than once: the model is recreated on respawn and level change,
    // and a one-shot would come back visible with nothing to say why.
    void set_hide_body(bool on);
    bool hide_body() const { return m_hide_body.load(std::memory_order_acquire); }
    uint64_t body_hides() const { return m_body_hides.load(std::memory_order_relaxed); }

    void set_pin_eye_height(bool on);
    bool pin_eye_height() const { return m_pin_eye.load(std::memory_order_acquire); }
    void set_roomscale(bool on);

    // ---- WALKING THE CHARACTER, NOT JUST THE CAMERA --------------------------------------------
    //
    // With camera-only roomscale the wearer walks their VIEW out of their own body: the character
    // stands still while the eye drifts across the room. This moves the player object instead, so
    // the two stay together.
    //
    // X AND Z ONLY. Vertical is deliberately excluded -- pushing the player object down sinks them
    // through the floor, and standing up would launch them; real height is already handled by the
    // eye placement, which is where it belongs.
    //
    // Incremental, frame to frame, rather than absolute against an origin: an absolute mapping
    // would fight every other thing that moves the player (walking, being pushed, scripted motion)
    // by dragging them back to wherever the headset says.
    void set_roomscale_body(bool on);

    // A displacement asked for from OFF the game thread -- the diagnostic route that proves the move
    // primitive works without a headset. It is QUEUED rather than applied, because
    // sdk::Physics::move_object runs the engine's collision sweep and is game-thread-only; a route
    // that called it directly would be testing the feature through a path the feature never uses.
    // Accumulates, so two requests inside one frame become a single move of their sum.
    void queue_body_nudge(float dx, float dz);

    // ---- THE FIRST-PERSON WEAPON -----------------------------------------------------------------
    //
    // WHICH OBJECT, because four other candidates look right and are not. The rendered gun is a
    // CLIENT-ONLY OT_MODEL (handle 0xFFFF) whose filename lives under `weapons\\`, sitting within a
    // metre of the player, and -- the part that actually discriminates -- carrying kVisible. Its
    // server-handled twin sits beside it with the bit clear, as do both copies of the player model.
    // Everything this project previously aimed at the weapon aimed at one of those instead.
    //
    // AMENDED, NOT ASSIGNED. The engine rebuilds this object's transform every frame from the
    // attachment socket, so an external write is reclaimed; the offset is re-applied per frame on the
    // game thread, which is the same discipline HeadTracking and the HUD already use.
    void set_weapon_override(bool on);
    bool weapon_override() const { return m_weapon_override.load(std::memory_order_acquire); }
    uintptr_t weapon_object() const { return m_weapon_obj.load(std::memory_order_relaxed); }
    uint64_t weapon_writes() const { return m_weapon_writes.load(std::memory_order_relaxed); }

    // A fixed offset in the engine's world axes, for proving the write lands before any controller
    // is involved. Zero once the controller drives it.
    void set_weapon_probe(float x, float y, float z);

    // ---- THE THUMBSTICKS -----------------------------------------------------------------------
    //
    // Left stick walks, right stick snap-turns. Both go through the engine's OWN input path --
    // SyntheticInput's key holds and TurnController's closed-loop turn -- for the same reason
    // send_mouse_look exists rather than a write to the aim: sensitivity, acceleration, the pitch
    // clamp, footstep timing, animation and every other downstream consumer then behave exactly as
    // they do for a player on a keyboard. Driving the movement state directly bypasses all of it.
    //
    // SNAP RATHER THAN SMOOTH TURN by default, because smooth yaw is the single worst comfort
    // offender in VR and the engine's look gain is not constant, so an open-loop smooth turn cannot
    // even hold a rate. `/xr/capture?snap_deg=` sets the step; 0 disables turning.
    void set_locomotion(bool on);
    bool locomotion() const { return m_locomotion.load(std::memory_order_acquire); }
    uint64_t stick_turns() const { return m_stick_turns.load(std::memory_order_relaxed); }

    // One instantaneous turn, in DEGREES, positive to the right. Shared by the right stick and the
    // test route so the route exercises the path the stick actually uses.
    bool snap_turn(float degrees);
    uint32_t locomotion_keys() const { return m_loco_keys.load(std::memory_order_relaxed); }

    // ---- FACE BUTTONS --------------------------------------------------------------------------
    //
    // A jumps, B reloads, both on the right controller. Sent as KEY TAPS through SyntheticInput
    // rather than as engine calls, so they go down the same path a keyboard does and inherit the
    // game's own bindings, cooldowns and animation gating. A direct call to a reload function would
    // work exactly once and then desynchronise the weapon state machine.
    //
    // Counted per action, because "the button does nothing" and "the button fires and the game
    // refuses" look identical from inside a headset.
    uint64_t jumps() const { return m_jumps.load(std::memory_order_relaxed); }
    uint64_t reloads() const { return m_reloads.load(std::memory_order_relaxed); }

    // ---- STICK CLICKS: SPRINT AND MELEE ----------------------------------------------------------
    //
    // Left stick in sprints, right stick in melees.
    //
    // SPRINT IS A LEVEL AND MELEE IS AN EDGE, and they are not interchangeable. Sprint has to be held
    // for as long as the wearer holds the stick down, so it tracks the button's state; melee is a
    // single swing the engine takes off a press transition, and re-asserting it every frame would
    // overwrite the very edge it is looking for.
    //
    // THE KEYS ARE SETTINGS, NOT CONSTANTS, because the game lets the player rebind them -- a
    // hardcoded VK is correct only for a default profile and fails silently for anyone else.
    // Defaults are this machine's measured bindings: Shift sprints, V melees.
    void set_sprint_vk(uint32_t vk) { m_sprint_vk.store(vk, std::memory_order_relaxed); }
    void set_melee_vk(uint32_t vk) { m_melee_vk.store(vk, std::memory_order_relaxed); }
    uint32_t sprint_vk() const { return m_sprint_vk.load(std::memory_order_relaxed); }
    uint32_t melee_vk() const { return m_melee_vk.load(std::memory_order_relaxed); }

    bool sprinting() const { return m_sprinting.load(std::memory_order_relaxed); }
    uint64_t melees() const { return m_melees.load(std::memory_order_relaxed); }

    // ---- REFLEX TIME, ON THE LEFT TRIGGER --------------------------------------------------------
    //
    // The game's reflex time is a TOGGLE: one key press turns it on, the next turns it off. So the
    // left trigger sends ONE tap per pull, on the rising edge -- treating it as a level would toggle
    // the state every frame the trigger was held and leave it wherever the frame count happened to
    // land.
    //
    // Hysteresis on an analogue axis for the same reason the fire trigger has it: a trigger resting
    // near the threshold would otherwise chatter, and here each chatter is a state flip rather than
    // a stray shot.
    //
    // GENERIC VK_CONTROL, not VK_LCONTROL -- the engine's keyboard array is indexed by the unsided
    // virtual key, which is what the sprint measurement established.
    void set_reflex_vk(uint32_t vk) { m_reflex_vk.store(vk, std::memory_order_relaxed); }
    uint32_t reflex_vk() const { return m_reflex_vk.load(std::memory_order_relaxed); }
    uint64_t reflex_toggles() const { return m_reflex_toggles.load(std::memory_order_relaxed); }

    void set_snap_degrees(float deg) { m_snap_deg.store(deg, std::memory_order_relaxed); }
    float snap_degrees() const { return m_snap_deg.load(std::memory_order_relaxed); }

    // How many clean controller blocks have been taken from the host, and the last sequence taken.
    // Published because "the hands do not move" has three distinct causes -- the host is not
    // publishing, the block is being rejected as torn, or it arrives and nothing consumes it -- and
    // only a counter separates them.
    uint64_t hand_pose_updates() const { return m_hand_pose_updates.load(std::memory_order_relaxed); }
    bool roomscale_body() const { return m_room_body.load(std::memory_order_acquire); }
    uint64_t body_moves() const { return m_body_moves.load(std::memory_order_relaxed); }
    bool roomscale() const { return m_roomscale.load(std::memory_order_acquire); }
    void recenter();

    void set_paced(bool on);
    bool paced() const { return m_paced.load(std::memory_order_acquire); }

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
    std::atomic<bool> m_paced{false};
    std::atomic<bool> m_pin_eye{false};
    std::atomic<bool> m_roomscale{false};
    std::atomic<bool> m_room_body{false};
    std::atomic<uint64_t> m_body_moves{0};
    std::atomic<float> m_nudge_x{0.0f};
    std::atomic<float> m_nudge_z{0.0f};
    std::atomic<uint64_t> m_hand_pose_updates{0};
    std::atomic<bool> m_weapon_override{false};
    std::atomic<uintptr_t> m_weapon_obj{0};
    std::atomic<uint64_t> m_weapon_writes{0};
    std::atomic<float> m_weapon_probe[3]{};
    std::array<float, 3> m_weapon_rest{};
    std::array<float, 4> m_weapon_rest_rot{0.0f, 0.0f, 0.0f, 1.0f};
    bool m_have_weapon_rest{false};
    std::atomic<bool> m_locomotion{false};
    std::atomic<uint64_t> m_stick_turns{0};
    std::atomic<uint32_t> m_loco_keys{0};
    std::atomic<float> m_snap_deg{30.0f};
    bool m_snap_armed{true};
    uint32_t m_held_keys{0};
    uint32_t m_last_buttons{0};
    std::atomic<uint64_t> m_jumps{0};
    std::atomic<uint64_t> m_reloads{0};
    std::atomic<uint64_t> m_melees{0};
    std::atomic<bool> m_sprinting{false};
    // VK_SHIFT, the GENERIC one, and this was measured rather than assumed. The engine's keyboard
    // device array is indexed by the unsided virtual key: holding 0x10 while moving sets
    // MoveFlag::Sprinting (flags 0x40801), while VK_LSHIFT 0xA0 and VK_RSHIFT 0xA1 both leave it
    // clear. Defaulting to the sided key is why the first version of this did nothing at all.
    std::atomic<uint32_t> m_sprint_vk{0x10};
    std::atomic<uint32_t> m_melee_vk{'V'};
    std::atomic<uint32_t> m_reflex_vk{0x11};  // VK_CONTROL, unsided
    std::atomic<uint64_t> m_reflex_toggles{0};
    bool m_left_trigger_down{false};
    uint32_t m_last_hands_sequence{0};
    float m_last_room_xz[2]{};
    bool m_have_last_room{false};
    std::atomic<bool> m_recenter{true};
    float m_room_origin[3]{};
    std::array<float, 3> m_eye_ref_vec{};
    bool m_have_eye_ref{false};
    std::atomic<float> m_eye_stand{74.98f};
    std::atomic<float> m_eye_crouch{45.50f};
    uint32_t m_last_recenter_serial{0};
    std::atomic<float> m_eye_tau_ms{350.0f};
    int64_t m_last_offset_tick{0};
    std::array<float, 3> m_pin_part{};
    std::array<float, 3> m_room_part{};
    std::atomic<float> m_eye_trim{0.0f};
    std::atomic<bool> m_hide_body{false};
    std::atomic<uint64_t> m_body_hides{0};
    uint32_t m_saved_body_flags{0};
    bool m_have_body_flags{false};
    std::atomic<bool> m_neutral_cam_off{false};
    std::array<float, 3> m_saved_cam_off{};
    bool m_have_saved_cam_off{false};
    std::atomic<uint64_t> m_host_pose_updates{0};
    std::atomic<uint64_t> m_host_pose_stale{0};
    uint32_t m_last_host_sequence{0};
    std::atomic<uint32_t> m_last_host_seq_pub{0};

    VR() = default;

    void update_hands();
    void update_locomotion();
    void update_buttons();
    void update_weapon();
    uintptr_t find_weapon_object();

    // One hand. `slot` is the BoneControl slot it drives, and each hand keeps its own rest
    // pose -- sharing one would make every offset a delta from wherever the OTHER hand
    // happened to start.
    void drive_hand(vr::VRRuntime::Hand which, uint32_t slot, const char* socket);
    void update_trigger();
};
