#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "Mod.hpp"

// ---- WHERE THE HUD'S AFFINE LIVES, SO A WATCHPOINT CAN BE POINTED AT IT -----------------------
//
// The interface is one Scaleform element. Its draw wrapper is FEAR2.exe 0x0046F715, and the object
// carrying the geometry is `elem[1]` -- resolvable only at runtime, since it is a heap object.
// Inside it: `+108` the scale (screenH/720), `+136..148` the viewport rect, and `+152..172` the 2x3
// affine that places the content.
//
// Six mechanisms have been ruled out by measurement, and the last of them -- substituting known-good
// values at draw time -- failed because the movie has ALREADY reflowed its layout by then. So the
// thing to find is the WRITER of that affine, upstream of the draw, and the way to find a writer in
// this project is `/watch/arm`, not another guess at an input.
//
// STRICTLY READ-ONLY. An earlier version of this wrote those fields and had to be deleted: a
// save/substitute/restore across eleven engine floats corrupts the geometry outright if a write
// fails partway through. This one only reports the address to point a hardware watch at.
class HudGeomProbe final : public Mod {
public:
    static HudGeomProbe& get();

    std::string_view get_name() const override { return "HudGeomProbe"; }
    void on_frame() override;

    struct State {
        bool hooked;
        uintptr_t element;
        uintptr_t inner;       // the object holding the geometry
        uintptr_t affine_addr; // inner + 152 -- arm /watch/arm here
        uint64_t calls;
    };
    State state() const;

private:
    HudGeomProbe() = default;
    void try_install();
};
