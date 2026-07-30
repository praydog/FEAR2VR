#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "../Mod.hpp"

// ---- THE MOTION A VR PLAYER DID NOT ASK FOR ----------------------------------------------
//
// Head bob, camera sway and camera shake are polish on a monitor and nausea in a headset: the
// view moves without the head moving, which is the single most reliable way to make someone ill.
// Every VR mod turns them off.
//
// MEASURED, so this is a real problem and not a received opinion: walking forward moves the
// camera's Y over a 13.48 unit range, with the engine's own `bob_active` true in 19 of 20 samples.
//
// THE ENGINE ALREADY HAS THE SWITCHES, which is worth knowing before hooking anything -- these are
// console variables it reads itself, so setting them puts the game in a state it already supports
// rather than one this mod invented:
//
//     HeadBobSpeedScale        1.0  -> 0   kills the bob wave outright
//     CameraSwayXSpeed         3.0  -> 0   the horizontal idle sway
//     CameraSwayYSpeed         1.0  -> 0   the vertical idle sway
//     DisableCameraShake       0.0  -> 1   the engine's own shake switch, already a boolean
//
// ---- WHY THIS IS A MOD AND NOT A FREE FUNCTION -------------------------------------------
//
// A console variable is ENGINE state and it outlives this DLL. Setting one and unloading leaves
// the player's game permanently altered, with nothing left to explain why -- the same hazard as a
// hidden model piece or a latched mouse button, both of which bit this project before. So the
// originals are captured on the first suppression and restored on release AND on shutdown.
class Comfort final : public Mod {
public:
    static Comfort& get() {
        static Comfort inst;
        return inst;
    }

    std::string_view get_name() const override { return "Comfort"; }

    void on_shutdown() override;

    // ---- CONSUMER API ------------------------------------------------------------------
    //
    // Suppress or restore the view motion. Idempotent: suppressing twice captures the originals
    // once, and restoring when nothing is suppressed does nothing.
    //
    // Returns false when the console-variable table could not be read at all. A variable missing
    // from a particular build is NOT a failure -- it is reported in `observed().missing` and the
    // rest are still applied, because a comfort setting that gives up entirely because one knob
    // moved between builds is worse than one that does what it can and says so.
    bool set_suppressed(bool on);
    bool suppressed() const;

    struct Observed {
        bool suppressed{};
        uint32_t known{};     // variables this mod knows how to suppress
        uint32_t found{};     // of those, present in this build
        uint32_t missing{};   // absent -- reported, not fatal
        uint32_t applied{};   // successfully written on the last suppression
        uint32_t restored{};  // successfully written back on the last release
        // The bob scale, read live. The one number that says whether the suppression is in force
        // right now, independent of this mod's own bookkeeping.
        float bob_scale{};
        bool bob_scale_readable{};
    };

    Observed observed() const;

private:
    Comfort() = default;
};
