#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "../Mod.hpp"

// THE ENGINE'S PLACEMENT OF THE HELD WEAPON, AGAINST OURS, IN ONE FRAME.
//
// `sdk::attached_socket` composes a mount point from the asset's socket record and the bone cache; the engine
// separately moves the attached object to that same point with its own arithmetic. Agreement between them is
// the strongest available evidence that the socket composition is correct -- it is two producers of one
// value, not a value compared against itself.
//
// It is only evidence when both are read in the same frame. This mod does the comparison on the frame
// callback for that reason, and publishes the weapon's per-frame travel alongside it so a consumer can tell
// a composition fault (wrong by units, always) from sampling skew (proportional to motion, zero at rest).
class WeaponAgreement final : public Mod {
public:
    static WeaponAgreement& get() {
        static WeaponAgreement inst;
        return inst;
    }

    std::string_view get_name() const override { return "WeaponAgreement"; }

    std::optional<std::string> on_initialize() override;
    void on_shutdown() override;

    // Measured on the engine's own update tick -- the same tick that places attached objects, so the two
    // producers cannot be read a frame apart.
    void on_frame() override;

    struct Observed {
        bool running{};
        uint64_t frames{};      // frames in which both producers resolved
        bool valid{};           // the last frame produced a comparison
        float disagreement{};   // |engine position - our composition|, same frame
        float step{};           // how far the weapon moved since the previous frame
        float worst{};          // the largest disagreement seen, over frames whose step was ~0
        uint64_t still_frames{};// frames where the weapon was effectively stationary
    };

    Observed observed() const;

private:
    WeaponAgreement() = default;
};
