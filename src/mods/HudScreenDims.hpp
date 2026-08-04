#pragma once

#include <atomic>
#include <cstdint>
#include <string_view>

#include "Mod.hpp"

// ---- WHAT THE INTERFACE IS TOLD THE SCREEN IS ---------------------------------------------------
//
// The HUD's content occupies a FIXED FRACTION of whatever stage it is given:
//
//     stage 4320x2224 : content 4107 x 1328  -> 0.951 wide, 0.597 tall
//     stage 2560x1440 : content 2434 x  860  -> 0.951 wide, 0.597 tall   (movie viewport rewritten,
//                                                                         write verified by reading
//                                                                         the object back)
//     control at 1440 : content 2428 x 1332  -> 0.949 wide, 0.925 tall
//
// The fraction does not move when the stage moves, so the stage is not the vertical input. The one
// thing still reporting native in both rows is the engine's screen size, and 1332/0.925 == 1440 --
// the interface lays out vertically against 1440 and horizontally against the real width.
//
// ILTClient carries the query in a FUNCTION POINTER FIELD at +0x2C, not a vtable slot: gameclient
// does `mov ecx, g_pILTClient / mov edx, [ecx+2Ch] / call edx`. The callee writes width and height
// into its out-parameter at +132 and +136 -- offsets taken from HUD_ComputeLayoutRect's own frame
// table and independently corroborated by g_RMode using the identical pair.
//
// This was attempted once before scoped to "inside the HUD bracket" and never fired even once: the
// dims are queried during game-logic and update, not during rendering. Session-scoped is the
// correction, and it is gated on supersampling being active so the engine's own resolutions are
// untouched.
class HudScreenDims final : public Mod {
public:
    static HudScreenDims& get();

    std::string_view get_name() const override { return "HudScreenDims"; }
    void on_frame() override;

    struct State {
        bool hooked;
        uintptr_t fn;      // resolved [g_pILTClient + 0x2C]
        uint64_t calls;    // times the engine asked
        uint64_t rewrites; // times we answered with the UI size
        int32_t last_w;    // what the engine would have said
        int32_t last_h;
    };
    State state() const;

private:
    HudScreenDims() = default;
    void try_install();
};
