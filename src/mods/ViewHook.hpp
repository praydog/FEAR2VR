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
        uint64_t calls{};        // total detour entries since install
        uintptr_t last_this{};   // the CPlayerCamera the last call ran on
        // THE THIRD ARGUMENT, unnamed on purpose. Hex-Rays types it `float a3`, and it read exactly 0.0
        // across 7843 live calls while the player looked around -- so whatever it carries, "pitch" is not
        // established and naming it that would put a guess in an interface.
        float last_a3{};
        bool installed{};
        uintptr_t target{};      // resolved address, 0 when the pattern missed
    };

    Observed observed() const;

private:
    ViewHook() = default;
};
