#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "../Mod.hpp"

// ---- TURNING THE PLAYER TO A HEADING, WHICH OPEN-LOOP INPUT CANNOT DO --------------------
//
// `Input::send_mouse_look` delivers a look DELTA through the engine's own handler, which is the
// right way in -- sensitivity, acceleration and the pitch clamp all apply as they do for a mouse.
// The price is that the gain is not constant: dx=200 turns ~28.5-28.9 degrees, dx=400 turns less
// than twice that, and the first delta after an injection was measured turning roughly double a
// later identical one. So "turn 30 degrees" cannot be issued as a single number.
//
// Every VR turning control needs exactly that, though:
//
//   * SNAP TURN -- "face 30 degrees further round" on a stick flick.
//   * RECENTRE -- "make the body face where I am looking", after a head-tracked view has been
//     decoupled from the aim and the player has turned their head far enough to want to walk
//     that way. Movement is AIM-relative (measured: velocity bearing equals aim_yaw to 0.00
//     degrees), so recentring is what makes "walk where I look" possible at all.
//
// This closes the loop: read `PlayerMgr::aim_yaw`, issue a correction, repeat until the error is
// inside tolerance. It runs on the frame tick because each correction has to be observed before the
// next is chosen, and because the input has to be queued on the game thread anyway.
class TurnController final : public Mod {
public:
    static TurnController& get() {
        static TurnController inst;
        return inst;
    }

    std::string_view get_name() const override { return "TurnController"; }

    void on_frame() override;
    void on_shutdown() override;

    // ---- CONSUMER API ------------------------------------------------------------------
    //
    // Turn the player to an ABSOLUTE heading, in radians, in the same convention as
    // `PlayerMgr::aim_yaw` (atan2(x, z); +Z is zero, increasing toward +X). Returns immediately;
    // the turn runs over the following frames. Issuing a new target replaces the current one.
    void turn_to(float yaw_radians);

    // Turn by a RELATIVE amount -- what a snap-turn button sends. Resolved against the heading at
    // the moment the call lands, not against a previous target, so repeated flicks accumulate the
    // way a player expects.
    bool turn_by(float delta_radians);

    // RECENTRE: turn the body to face where the head is looking, then the composed head pose is
    // pointing straight ahead again. No-op when nothing is composed, because then the two already
    // agree and the "turn" would be a rounding error's worth of jitter.
    bool recentre();

    // ---- PITCH: THE OTHER HALF OF AIMING, AND WHY IT IS HERE ---------------------------
    //
    // Shots follow the VIEW, not the weapon (measured: turning the head 30 degrees moves the
    // impacts 30 degrees while the aim never moves). So in VR, aiming the view IS aiming the gun,
    // and a controller that can only turn the body can only aim in one axis.
    //
    // Pitch is not just yaw with a different letter, in three ways that matter:
    //
    //   * IT IS CLAMPED, and the clamp MOVES. The engine selects it per player state -- standing
    //     -80/+85 degrees, crouching -42/+85 (both measured live, both matching the Client/
    //     CameraClamping record the decompiler shows). A target outside the current clamp can
    //     never be reached, so this class asks `PlayerMgr::pitch_limits` and aims at the nearest
    //     reachable pitch instead of spending its whole budget against a wall.
    //   * IT DOES NOT WRAP. Yaw is modular; pitch is an interval. Wrapping it would turn "look
    //     30 degrees further up" near the top into a dive.
    //   * THE ENGINE NEVER RECENTRES IT. Recoil walks the aim upward and leaves it there
    //     (measured: four bursts, +5.3 to +8.6 degrees, no recovery). Something has to put it
    //     back, and open-loop input cannot, for the same non-constant-gain reason as yaw.
    //
    // Absolute pitch, radians, in `PlayerMgr::aim_pitch`'s convention: positive is UP. Silently
    // clamped to what the engine currently allows.
    void pitch_to(float pitch_radians);

    // Relative pitch, resolved against the aim at the moment the call lands.
    bool pitch_by(float delta_radians);

    // Both axes at once -- what a VR mod does when pointing the view at a direction the player's
    // hand or head is indicating. Cheaper and smoother than two calls: the corrections share a
    // frame and go out as ONE look delta, so the view moves along a diagonal rather than
    // staircasing through two separate settles.
    void aim_to(float yaw_radians, float pitch_radians);

    // Put the pitch back on the horizon. The specific thing recoil makes necessary, and the
    // precondition for any measurement that depends on where shots land -- an aim resting at
    // +8.6 degrees puts every round in the ceiling.
    void level();

    // Abandon any turn in progress and leave the player where they are.
    void cancel();

    struct Observed {
        bool active{};
        float target{};        // radians
        float error{};         // radians, current heading minus target, signed and wrapped
        uint32_t corrections{};// deltas issued for the CURRENT turn
        uint64_t completed{};  // turns that converged
        uint64_t abandoned{};  // turns that hit the iteration cap without converging
        bool converged{};      // the last turn finished inside tolerance

        // ---- pitch axis, tracked separately ----
        //
        // Separate counters rather than a shared "active", because the two axes finish at
        // different times and a consumer polling for "is my aim there yet" needs to know which
        // half is still moving.
        bool pitch_active{};
        float pitch_target{};      // radians, already clamped to the engine's live limits
        float pitch_error{};       // radians, current pitch minus target
        bool pitch_converged{};
        bool pitch_clamped{};      // the requested target was outside the limits and was moved
        uint64_t pitch_completed{};
        uint64_t pitch_abandoned{};
    };

    Observed observed() const;

private:
    TurnController() = default;
};
