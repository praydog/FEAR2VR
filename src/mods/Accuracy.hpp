#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "../Mod.hpp"

// ---- TIGHTENING WEAPON SPREAD ---------------------------------------------
//
// In VR the player aims with their hand, and the engine's hip-fire spread cone is
// far wider than the precision the wearer can now actually deliver -- so shots
// miss a target the controller is pointing dead at. This scales that cone.
//
// ONE CONSOLE VARIABLE, NO HOOK. `SkillAimAccuracy` multiplies the weapon's
// perturb, and it does nothing else in the entire client: all three of its call
// sites sit immediately after the perturb accessor, covering primary fire,
// alternate fire and grenades.
//
//     perturb = GetWeaponPerturb(stats) * SkillAimAccuracy   // gameclient 0x1012DC80
//     Weapon_SendClientFireMessage(this, perturb, origin, dir)
//
// THE NAME IS BACKWARDS FROM WHAT IT DOES. Despite reading "Accuracy", the value
// multiplies PERTURB, so LOWER is more accurate: 1.0 is stock, 0.25 is a quarter
// of the cone, 0.0 is no spread at all.
//
// WHY THIS AND NOT A HOOK. The perturb is computed client-side and travels to the
// server in the fire message, which then traces the ray -- so the client's value
// is the one that decides where the shot goes, and scaling it needs no
// interception. `GetWeaponPerturb` itself interpolates the weapon record's
// Perturb range by a normalised accumulator that movement, firing and turning all
// drive; scaling the result leaves all of that intact and merely narrows the cone
// it produces, so a weapon still bloats as it fires -- just proportionally less.
//
// IT OUTLIVES THE DLL. A console variable is engine state, so the original is
// captured on the first arm and written back on release and on shutdown, the same
// discipline Comfort follows. The value is also re-asserted periodically, because
// a level load re-applies the difficulty's skill values over the top.
class Accuracy final : public Mod {
public:
    static Accuracy& get();

    std::string_view get_name() const override { return "Accuracy"; }
    void on_frame() override;
    void on_shutdown() override;

    // Arm with a perturb multiplier in [0, 1]. Outside that range is refused: above
    // 1.0 would make the game LESS accurate than stock, which no caller means to ask
    // for, and a negative would flip the cone inside out.
    bool set_scale(float scale);

    // Restore the value the game had before the first arm.
    bool release();

    bool armed() const { return m_armed.load(std::memory_order_relaxed); }
    float scale() const { return m_scale.load(std::memory_order_relaxed); }
    float original() const { return m_original.load(std::memory_order_relaxed); }
    bool resolved() const { return m_resolved.load(std::memory_order_relaxed); }
    uint64_t reasserts() const { return m_reasserts.load(std::memory_order_relaxed); }

private:
    Accuracy() = default;

    static constexpr const char* kVar = "SkillAimAccuracy";

    // The variable is created lazily by the fire path and re-applied at level load, so
    // this is a slow poll rather than a one-shot write. 60 frames is far below any rate
    // at which a stale value could matter and costs one table lookup.
    static constexpr uint32_t kCheckInterval = 60;

    std::atomic<bool> m_armed{false};
    std::atomic<bool> m_resolved{false};
    std::atomic<float> m_scale{1.0f};
    std::atomic<float> m_original{1.0f};
    std::atomic<bool> m_captured{false};
    std::atomic<uint32_t> m_countdown{0};
    std::atomic<uint64_t> m_reasserts{0};
};
