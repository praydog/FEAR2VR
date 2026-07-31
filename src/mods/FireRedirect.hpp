#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <safetyhook.hpp>

#include "../Mod.hpp"

// ---- WHERE A SHOT'S DIRECTION ACTUALLY COMES FROM -------------------------
//
// Not the client. Three rw-watchpoint sweeps (camera holder pose, shell player
// object rotation, camera object rotation) found ZERO accessors unique to the
// trigger being down, because the client replicates its rotation every frame
// whether or not you fire. The ray is built and traced in gameserver.dll, by
// `Weapon_FireServer` -- reversing/ENGINE_NOTES.md records the hunt.
//
// That function receives a descriptor:
//
//     +0x00  float[3]  DIRECTION (unit)
//     +0x0C  float[3]  ORIGIN
//
// derived from its own arithmetic, not guessed: it computes
// `FireOffset * dir + origin` and hands the resulting segment to the physics
// intersect. Only a direction can be scaled by a scalar and added to a point.
//
// ---- WHAT THIS MOD ACTUALLY DELIVERS TODAY --------------------------------
//
// OBSERVATION, which works and is asserted by the suite: every shot the server
// takes is seen here, with the direction and origin it was built from, and the
// fixture proves that direction predicts where bullets are measured to land.
//
// REDIRECTION, which does NOT work yet. Writing the descriptor's direction has
// been tried at three points on this path and moved bullets at none of them:
//
//   Weapon_FireServer entry          asked +40 deg, impacts moved +1.23
//   Weapon_FireHitscanVector entry   asked +25 deg, impacts moved +0.06
//   Weapon_TraceShot entry           asked +25 deg, impacts moved +0.29
//
// Every one of those wrote successfully. The write counter is therefore not
// evidence of anything and the impact bearing is the only honest measure. The
// reading these three results support is that descriptor+0 is a RECORD of the
// aim, derived alongside the trace from the weapon's own state rather than
// consumed as its input -- so the field that matters is further upstream and
// has not been found. `set_direction` is kept because the machinery is exactly
// what the next hook point will need, not because it currently steers a shot.
//
// ---- WHY A DESCRIPTOR AND NOT THE PLAYER'S AIM ----------------------------
//
// Writing the descriptor is downstream of spread and pellet generation, so a
// single write redirects the whole shot -- shotguns included -- rather than
// correcting each of `VectorsPerRound` pellets. And it does not fight head
// tracking: overriding the player's aim rotation would seize a composed value
// and force every consumer that derived from it to be re-derived by hand,
// which is the mistake this project already catalogued once (see "THE FIELD IS
// RECLAIMED"). The gun points where the hand points; the view stays the view.
//
// ---- MID HOOK, NOT INLINE -------------------------------------------------
//
// `Weapon_FireServer` is `__userpurge`: `this` in ecx, one argument in edi, the
// descriptor on the stack. No C++ signature expresses that, so an inline hook
// cannot forward the call. A mid hook at the entry instruction reads the
// descriptor from `esp+4` (return address at `esp+0`) and lets the original
// code run untouched.
class FireRedirect final : public Mod {
public:
    static FireRedirect& get();

    std::string_view get_name() const override { return "FireRedirect"; }
    std::optional<std::string> on_initialize() override;
    void on_frame() override;
    void on_shutdown() override;

    // Arm with a world-space unit direction. Rejected (returns false) unless the
    // vector is finite and within 1e-3 of unit length: a non-unit direction is
    // how a mis-scaled or half-written value announces itself, and a silently
    // normalized one would hide the bug instead.
    bool set_direction(float x, float y, float z);
    void clear_direction();

    bool armed() const { return m_armed.load(std::memory_order_relaxed); }
    bool hooked() const { return m_hooked.load(std::memory_order_relaxed); }
    uintptr_t target() const { return m_target.load(std::memory_order_relaxed); }

    // Observability: shots seen, shots redirected, and the direction the engine
    // held when we last looked -- so a test can prove the engine's own value was
    // replaced rather than merely that we wrote somewhere.
    uint64_t calls() const { return m_calls.load(std::memory_order_relaxed); }
    uint64_t writes() const { return m_writes.load(std::memory_order_relaxed); }
    std::array<float, 3> last_engine_dir() const;
    std::array<float, 3> last_written_dir() const;
    std::array<float, 3> last_origin() const;

private:
    FireRedirect() = default;

    static void on_fire(SafetyHookContext& ctx);

    std::atomic<bool> m_armed{false};
    std::atomic<bool> m_hooked{false};
    std::atomic<uintptr_t> m_target{0};
    std::atomic<uint64_t> m_calls{0};
    std::atomic<uint64_t> m_writes{0};
    std::atomic<uint32_t> m_retry_countdown{0};

    // Plain atomics rather than a mutex: the detour runs on the engine's fire
    // path and must not block, and a torn read of a direction is bounded by the
    // unit-length check at the arming end.
    std::atomic<float> m_dir[3]{};
    std::atomic<float> m_engine_dir[3]{};
    std::atomic<float> m_written_dir[3]{};
    std::atomic<float> m_origin[3]{};
};
