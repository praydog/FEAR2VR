#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "../Mod.hpp"

// THE ORTHO PASS -- CLTRenderer's vtable slot 16, `SetupPassAffine`.
//
// WHY A VR MOD CARES. The perspective passes are now drawn once per eye, but the HUD is painted by a
// SEPARATE pass that runs once and covers the whole target, so in a side-by-side frame it lies across both
// halves and the seam. Making it per-eye starts with establishing which pass it is and what it honours.
//
// Slot 16's wrapper takes the SAME arguments as slot 15 -- (camera, fov[2], rect[4], depthMin, depthMax) --
// but the body does NOT build its viewport from that rect. It reads the bound target's dimensions out of the
// record at +0x174 and offsets them by a pair stored at +0x170. That is the difference a per-eye HUD has to
// work with: the record is the input, not the argument.
class HudPassHook final : public Mod {
public:
    static HudPassHook& get() {
        static HudPassHook inst;
        return inst;
    }

    std::string_view get_name() const override { return "HudPassHook"; }

    std::optional<std::string> on_initialize() override;

    struct Observed {
        bool hooked{};
        uintptr_t target{};
        uint64_t passes{};              // affine setups seen
        uint32_t passes_last_frame{};   // how many ran between the last two frame boundaries
        // What the last one was configured with, captured as the ARGUMENTS rather than read back, so this
        // says what the caller asked for even when the record derives something else.
        std::array<float, 4> rect{};
        float depth_min{};
        float depth_max{};
        // And what the record held afterwards: the viewport it actually derived, plus whether the projection
        // it produced is orthographic. That pairing is the evidence this is the HUD pass and not another
        // perspective one.
        std::array<int32_t, 4> viewport{};
        bool ortho{};
        bool record_read{};

        // SLOT 17, `SetupPassStored`, hooked beside slot 16 because the question "which slot configures the
        // ortho pass the HUD is painted in" is only answerable by watching both. Slot 16 turned out never to
        // run in normal play, so an unmeasured assumption about it would have been wrong.
        bool stored_hooked{};
        uint64_t stored_passes{};
        uint32_t stored_last_frame{};
        bool stored_ortho{};
        std::array<int32_t, 4> stored_viewport{};

        // THE VIEWPORT OFFSET GATE, read in phase. From the IPC thread the descriptor's target pointer is
        // null -- no target is bound between frames -- so the gate reads "unknown" and says nothing. Inside
        // the pass a target is bound by construction.
        bool offset_read{};
        bool offset_gate{};
        std::array<int32_t, 2> offset_effective{};
        std::array<int32_t, 2> offset_stored{};

        bool offset_armed{};
        std::array<int32_t, 2> offset_requested{};
        uint64_t offset_writes{};   // passes in which the shift was actually applied
    };

    // ARM THE VIEWPORT SHIFT. Writing +0x17C from outside is reclaimed -- the descriptor is rebuilt every
    // time a target is bound, which happens before each of the ~10 screen passes in a frame -- so the offset
    // has to be written inside the pass entry, before the engine derives its rect from it. That is what this
    // arms; `clear_offset` releases it and the engine's own value takes over on the very next pass.
    void set_offset(int32_t x, int32_t y);
    void clear_offset();

    Observed observed() const;

private:
    HudPassHook() = default;
};
