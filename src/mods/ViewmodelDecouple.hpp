#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "../Mod.hpp"

// ---- STOPPING THE WEAPON FROM FOLLOWING YOUR HEAD ----------------------------------------
//
// THE PROBLEM, MEASURED. `HeadTracking` composes a head pose into the camera's additive slot, so
// the view turns and the aim does not -- exactly as intended, and confirmed: view-vs-aim tracks
// the commanded yaw to three decimals. But the first-person arms and the weapon swing anyway. At
// 25 degrees the hand socket moves 17.8 units and the muzzle 35.1, which chord geometry pins to a
// rigid rotation about the camera (radii 41.1 and 81.1, differing by the 39.49 hand-to-muzzle
// distance).
//
// It is NOT the animation. Every candidate bone -- `aimer`, `Pelvis_Cam`, `Pelvis`, `Torso`,
// `attach` -- holds a rotation that does not move by a thousandth of a degree across a head sweep.
//
// A write watch found it: the object the first-person rig hangs off has its rotation rewritten,
// through the engine's own `LTObject_SetRotation`, every time the view changes -- and to the
// VIEW's orientation. Live, that object's forward sits 0.2-0.7 degrees off the camera at any head
// yaw, while its angle to the aim grows with the yaw exactly.
//
// WHICH OBJECT, because there are two and picking the wrong one wastes a session:
// `CClientShell::local_player`'s, not `PlayerMgr`'s. The latter's rotation is never written at all.
//
// ---- THE CORRECTION -----------------------------------------------------------------------
//
// The camera's rotation is the engine's own product `outer * inner`: the additive slot times the
// player's aim. The rig is being set to (approximately) that product, so removing the head pose is
// one multiplication:
//
//     corrected = conj(outer) * incoming
//
// With nothing composed, `outer` is identity and the correction is the identity -- so this mod is
// inert until a head pose exists, rather than something that has to be switched on in step with it.
class ViewmodelDecouple final : public Mod {
public:
    static ViewmodelDecouple& get() {
        static ViewmodelDecouple inst;
        return inst;
    }

    std::string_view get_name() const override { return "ViewmodelDecouple"; }

    std::optional<std::string> on_initialize() override;
    void on_frame() override;

    // ---- CONSUMER API ------------------------------------------------------------------
    //
    // Arm the correction. Off by default: a mod that silently reorients the player's arms the
    // moment it loads is not something a consumer can reason about.
    void set_enabled(bool on);
    bool enabled() const;

    struct Observed {
        bool hooked{};            // the setter was resolved and the hook installed
        uintptr_t target{};       // the engine entry we own
        bool enabled{};
        uint64_t calls{};         // setter invocations seen, for ALL objects
        uint64_t matched{};       // those on the object the viewmodel rides
        uint64_t corrected{};     // those we actually rewrote (armed AND head pose non-identity)
        // The last correction's magnitude, in radians: the angle between what the engine asked for
        // and what we passed on. Zero while nothing is composed, which is the inert case.
        float last_correction{};
        bool object_resolved{};   // the shell player object is known this frame
    };

    Observed observed() const;

private:
    ViewmodelDecouple() = default;
};
