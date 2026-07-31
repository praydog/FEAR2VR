#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "../Mod.hpp"

// A HEAD ORIENTATION COMPOSED ON TOP OF THE PLAYER'S AIM.
//
// The camera's rotation is `LTRotation_Multiply(holder[+552], holder[+324])` -- an additive outer operand
// times the player's own aim -- and the outer operand is IDENTITY whenever nothing is leaning or shaking the
// camera. Writing a headset orientation there is the whole feature: the engine keeps composing, so the head
// turns freely while the body, the aim and the weapon stay exactly where the player put them.
//
// ---- WHY THIS AND NOT THE SetPosRot OVERRIDE -------------------------------------------------------------
//
// ViewHook can own LTObject_SetPosRot and replace the camera's rotation outright. That works, and it is how
// this project first proved the view could be driven, but it seizes the ENTIRE rotation: the player's aim is
// then something the mod has to re-derive and feed back, and a partial job produced the jitter and
// rubber-banding recorded in reversing/ENGINE_NOTES.md.
//
// The outer operand is the composition point the engine already has. Nothing needs re-deriving.
//
// ---- THE FIELD IS RECLAIMED, SO THE WRITER IS OWNED -------------------------------------------------------
//
// PlayerCamera_UpdateAttachedRotation writes +552 unconditionally every frame -- an attached object's rotation
// in the special case, identity in the ordinary one. A write from anywhere else is overwritten before it is
// read, which `PlayerMgr::probe_outer_operand()` reports as Reclaimed.
//
// So this hooks that function and writes AFTER the original returns. Same conclusion the camera object forced,
// and the same shape as the frustum centre: the engine computes, then we amend, then it consumes.
//
// Off by default -- it moves the player's view.
class HeadTracking final : public Mod {
public:
    static HeadTracking& get();

    std::string_view get_name() const override { return "HeadTracking"; }

    std::optional<std::string> on_initialize() override;
    void on_frame() override {}
    void on_shutdown() override;

    // THE HEADSET'S ORIENTATION, as a quaternion in the engine's convention (x, y, z, w).
    //
    // A quaternion rather than Euler angles because that is what a runtime hands you, and converting to angles
    // and back would introduce an order convention this code has no reason to invent.
    //
    // Composed as the OUTER operand, so the resulting view is `head * aim`: turning the head does not move the
    // aim, and moving the aim does not fight the head.
    void set_head_rotation(const std::array<float, 4>& rotation);

    // Stop contributing. The engine's own identity write then stands and the view returns to the player's aim
    // on the very next frame -- there is nothing to restore, which is a property of composing rather than
    // overriding.
    void clear();

    struct State {
        bool enabled{};
        bool hooked{};
        uintptr_t target{};
        uint64_t writer_calls{};   // times the engine's writer ran
        uint64_t writes{};         // times we amended it
        uintptr_t last_holder{};
        std::array<float, 4> requested{};
        // What the outer operand held immediately after our write, read back in the same detour. In phase
        // with the frame it affects, unlike anything sampled from the IPC thread.
        std::array<float, 4> readback{};
        bool readback_matches{};
    };

    State state() const;

private:
    HeadTracking() = default;
};
