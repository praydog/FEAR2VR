#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "../Mod.hpp"

// THE VIEW WRITER, HOOKED.
//
// CPlayerCamera_ApplyLookDelta is where the camera's rotation is actually produced: it adds a look delta and
// clamps the resulting pitch. Owning it is the ONLY way to steer the view, and that is a measurement rather
// than a design preference -- writing the camera object's rotation or the holder's outer operand is reclaimed
// by the engine within one frame (PlayerMgr::probe_camera_object_rotation and probe_outer_operand both return
// Reclaimed on a running game). A head-tracked view therefore cannot poke a field; it has to be the writer.
//
// TWO HOOKS, ONE CONCEPT. ApplyLookDelta is the LOOK INPUT path and PlayerCamera_UpdateViewPose is the
// PER-FRAME path; a head-tracked view needs the second, because it must update whether or not the player
// touched the mouse. Measured: the frame hook ticks ~300/s while ApplyLookDelta stays at exactly 0 with the
// mouse still. Both live here because they are the same subsystem, and their counters are kept apart because
// the difference in what drives them is the finding.
//
// THIS PASS INSTALLS AND OBSERVES ONLY. The detour counts calls, records the arguments it was handed, and
// passes straight through. No behaviour changes, because the claim under test here is narrow and worth
// establishing on its own: that this function is reachable, hookable, called every frame the camera updates,
// and retires cleanly on uninject. An override written before that is demonstrated would be debugging two
// things at once.
//
// ABI, from the disassembly and not from the decompiler's guess:
//     __thiscall, `this` in ecx
//     three 4-byte stack arguments -- BOTH exits are `retn 0Ch`
// An x86 detour is written `__fastcall(this, edx_dummy, ...)` (AGENT.MD rule 1). That works because fastcall
// is callee-cleans and emits the matching `retn 0Ch`; a wrong arity would corrupt esp on every call, so the
// count comes from the return instruction.
class ViewHook final : public Mod {
public:
    static ViewHook& get();

    std::string_view get_name() const override { return "ViewHook"; }

    // Resolves the target and installs through Hooks::get(). Returns a message on failure; an unresolved
    // pattern is reported rather than fatal, so the rest of the framework still comes up.
    std::optional<std::string> on_initialize() override;

    // NOTHING HERE. The hook does the work on the engine's own thread; a per-frame poll would only duplicate
    // it. Kept unimplemented deliberately rather than omitted, so the next reader does not add one.
    void on_frame() override {}

    // Retiring is Hooks::get()'s job (it disables every hook under one lock during shutdown), so a mod must
    // NOT remove its own hook here -- see Mod.hpp and TESTING.MD's uninject contract.
    void on_shutdown() override {}

    // ---- DIAGNOSTICS, data only ---------------------------------------------------------------
    //
    // Read by the /sdk/shader-params reporter so the host-side suite can assert the hook FIRES rather than
    // merely that it installed. Static shape is necessary and never sufficient (TESTING.MD rule 3).
    struct Observed {
        // ---- ApplyLookDelta: the LOOK INPUT path -----------------------------------------------
        uint64_t calls{};        // total detour entries since install
        uintptr_t last_this{};   // the CPlayerCamera the last call ran on
        // THE THIRD ARGUMENT, unnamed on purpose. Hex-Rays types it `float a3`, and it read exactly 0.0
        // across 7843 live calls while the player looked around -- so whatever it carries, "pitch" is not
        // established and naming it that would put a guess in an interface.
        float last_a3{};
        bool installed{};
        uintptr_t target{};      // resolved address, 0 when the pattern missed

        // ---- UpdateViewPose: the PER-FRAME path ------------------------------------------------
        //
        // Held separately rather than summed, because the whole point of hooking the second one is that the two
        // have DIFFERENT drivers: ApplyLookDelta only runs when the player looks, this one runs from the
        // camera's update. A single counter would hide exactly the distinction being established.
        uint64_t pose_calls{};
        uintptr_t pose_last_this{};
        bool pose_installed{};
        uintptr_t pose_target{};

        // SAME-PHASE agreement between the applied pose and the camera object, sampled inside the detour
        // AFTER the original runs. Out-of-band this question has no answer -- the writer runs every frame, so
        // an IPC-thread reader always lands mid-update -- which is why the counters live here.
        uint64_t pose_agree_equal{};
        uint64_t pose_agree_differ{};
        uint64_t pose_agree_other{};   // unreadable or torn

        // ---- THE OVERRIDE EXPERIMENT ------------------------------------------------------------
        //
        // Writes a yaw offset into the applied pose after the original computes it, then checks on the NEXT
        // frame whether the camera object came to hold what we wrote. That is the go/no-go for head tracking:
        // if propagation carries our value, this hook is the place to drive a VR view from.
        uint32_t override_frames_left{};  // auto-disarms, so a forgotten override cannot wedge the view
        float override_yaw_deg{};
        uint64_t override_applied{};      // frames we wrote a pose on
        uint64_t override_carried{};      // frames the camera object then held OUR value
        uint64_t override_rejected{};     // frames the write itself was refused
        // Did OUR pose still stand at the next frame? Distinguishes "the engine recomputed the pose" from
        // "the pose stood but nothing propagated it into the object" -- opposite problems with one symptom.
        uint64_t override_pose_held{};
        // THE RUBBER-BAND AMPLITUDE: how far the field drifted from what we wrote, before we corrected it.
        // Only measurable inside the hook -- the drift is created and erased between frames, so an out-of-band
        // sampler reads a still value no matter how hard the two writers fight.
        float override_max_drift_deg{};
        uint64_t override_drift_frames{};
    };

    // Arm the override for `frames` frames at `yaw_deg`. Bounded on purpose: the view returns to the engine by
    // itself when the count runs out, because the engine recomputes the pose every frame -- there is nothing to
    // restore and no way for a crash mid-experiment to leave the camera stuck.
    // `write_source` picks the field: false writes the APPLIED pose at +244 (derived -- lands, blurs the
    // view, does not survive the frame), true writes the VIEW rotation at +324 (the source candidate).
    void arm_override(float yaw_deg, uint32_t frames, bool write_source);

    Observed observed() const;

private:
    ViewHook() = default;
};
