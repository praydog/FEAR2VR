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
    };

    Observed observed() const;

private:
    TurnController() = default;
};
