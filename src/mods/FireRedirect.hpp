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

    // How the outgoing direction is replaced.
    enum class Mode {
        Off,        // pass the client's own direction through untouched
        Absolute,   // replace it with a fixed world-space direction (set_direction)
        Reverse,    // negate whatever the client was about to send
        Weapon,     // aim along the WEAPON's own muzzle -- YAW ONLY, see the note below
        Controller, // aim along the VR controller, composed onto the body's heading
    };

    // REVERSE is not a novelty: it is the only redirection whose effect a HUMAN can
    // judge without instrumentation. Point at an enemy, hold the key, and shoot --
    // if the target still takes damage then the direction in this message does not
    // decide where the shot goes, and if it stops taking damage it does. Nothing
    // this project can measure from outside the process settles that, because
    // impact EFFECTS may be client-predicted while DAMAGE is server-authoritative.
    //
    // It also works from any orientation with no host-side arithmetic, because the
    // negation happens inside the hook against the value the client actually built.
    void set_mode(Mode mode);

    // WEAPON MODE IS YAW-ONLY, MEASURED. The muzzle transform reachable from the
    // player object carries the BODY's heading and no pitch -- the body stands
    // upright while the camera pitches -- so its error tracks the aim's pitch exactly:
    //
    //     aim pitch     0 deg   -25 deg   +25 deg
    //     socket +Z     3.5     27.8      21.8      (weapon object was worse: 13.6/16.3/36.1)
    //
    // +Z IS the barrel axis (it beat every other basis vector at every pitch); the
    // pitch simply is not in that transform. Kept because it is honest about what it
    // does, but CONTROLLER mode is the one a VR player wants.
    //
    // CONTROLLER MODE IS THE VR FEATURE. `BoneControl` already drives the weapon's bone
    // from the controller, so once the shot follows the muzzle the gun genuinely
    // points where your hand does -- rather than the shot following the view while
    // the model merely looks aimed.
    //
    // The muzzle direction is sampled on the GAME THREAD in on_frame and cached,
    // not read inside the hook: resolving a socket can evaluate a dirty skeleton,
    // and doing that from inside the fire path would mutate engine state in the
    // middle of the engine's own call. One frame of staleness is the price, and it
    // is the same age as the pose the player is looking at.
    // The direction as the client's own builder just produced it, read at the point
    // BEFORE the local effect prediction consumes it. Published so the stack offset
    // it is recovered from can be PROVEN against what the sender reports rather than
    // trusted -- a wrong offset here writes into an unrelated local.
    std::array<float, 3> built_dir() const;
    std::array<float, 3> built_origin() const;
    uint64_t builds() const { return m_builds.load(std::memory_order_relaxed); }

    // The muzzle socket's raw rotation. Published because which of its basis axes
    // points down the barrel is a property of the ART, not of the engine, and the
    // only honest way to pick one is to compare all three against a direction
    // already known to be the true aim.
    std::array<float, 4> weapon_quat() const;
    std::array<float, 4> weapon_object_quat() const;

    std::array<float, 3> weapon_forward() const;
    bool weapon_forward_valid() const { return m_weapon_ok.load(std::memory_order_relaxed); }
    Mode mode() const { return m_mode.load(std::memory_order_relaxed); }

    // Hold-to-apply. While a hotkey is set, redirection only happens for shots taken
    // with that key DOWN, so a player keeps normal fire the rest of the time and the
    // A/B comparison is one keypress apart. 0 disables the gate (always apply).
    void set_hotkey(int vk) { m_hotkey.store(vk, std::memory_order_relaxed); }
    int hotkey() const { return m_hotkey.load(std::memory_order_relaxed); }
    bool hotkey_held() const { return m_hotkey_held.load(std::memory_order_relaxed); }

    // Shots whose direction was actually replaced. The number a human needs to see
    // before believing a negative result: "the enemy took damage" means nothing if
    // this did not move while they were holding the key.
    uint64_t redirected_shots() const { return m_writes.load(std::memory_order_relaxed); }

    // ---- WHERE THE SHOT STARTS ---------------------------------------------
    //
    // The engine starts the ray at the player's EYE, which is right for a game
    // played down a crosshair and wrong for one played with a gun in your hand: a
    // shot should leave the BARREL. With the muzzle held out to the side, an
    // eye-origin ray takes a different path through the world -- around a corner the
    // player is peeking past, for instance -- so the two disagree exactly where it
    // matters most.
    //
    // Kept as a SEPARATE toggle from the direction, deliberately. Moving the ray's
    // start changes what it can clip through, so a consumer that only wants
    // hand-aiming should not silently get a moved origin as well.
    //
    // Unlike the muzzle's ROTATION, its POSITION is trustworthy: the pitch that is
    // missing from the socket transform is a rotation, and a point does not have one.
    void set_origin_from_weapon(bool enabled);
    bool origin_from_weapon() const { return m_origin_from_weapon.load(std::memory_order_relaxed); }
    bool origin_valid() const { return m_origin_ok.load(std::memory_order_relaxed); }
    std::array<float, 3> weapon_origin() const;
    uint64_t origin_writes() const { return m_origin_writes.load(std::memory_order_relaxed); }

    // Arm with a world-space unit direction. Rejected (returns false) unless the
    // vector is finite and within 1e-3 of unit length: a non-unit direction is
    // how a mis-scaled or half-written value announces itself, and a silently
    // normalized one would hide the bug instead.
    bool set_direction(float x, float y, float z);
    void clear_direction();

    bool armed() const { return m_armed.load(std::memory_order_relaxed); }
    bool hooked() const { return m_hooked.load(std::memory_order_relaxed); }
    uintptr_t target() const { return m_target.load(std::memory_order_relaxed); }

    // The descriptor the last shot used. Published so a hardware watch can be
    // armed on the field itself: three hook points have now written desc+0
    // without moving a bullet, so the question is no longer "where do I write
    // it" but "who WRITES it, and what do they read to decide" -- which is a
    // trap-it question, not a scan-it one.
    uintptr_t last_descriptor() const { return m_last_desc.load(std::memory_order_relaxed); }

    // The RETURN ADDRESS into whoever called the server fire entry. Weapon_FireServer
    // has eight static callers and the descriptor is built on the caller's stack, so
    // naming the one the PLAYER goes through is the difference between reading one
    // function and reading eight. Costs a hook and one stack read.
    uintptr_t fire_caller() const { return m_fire_caller.load(std::memory_order_relaxed); }
    // Return address into whoever calls the CLIENT sender -- names which of its two
    // static callers builds the direction, and therefore where the local effect
    // prediction is likely to live.
    uintptr_t send_caller() const { return m_send_caller.load(std::memory_order_relaxed); }
    uint64_t fire_entries() const { return m_fire_entries.load(std::memory_order_relaxed); }
    uint64_t messages() const { return m_messages.load(std::memory_order_relaxed); }
    uint64_t sends() const { return m_sends.load(std::memory_order_relaxed); }
    bool send_hooked() const { return m_send_hooked.load(std::memory_order_relaxed); }
    std::array<float, 3> last_sent_dir() const;
    std::array<float, 3> last_sent_origin() const;

    // Stack values at the SERVER's fire-message handler that land inside
    // gameclient.dll. Single player runs a local server IN-PROCESS, so if the
    // client dispatches its fire message synchronously the sender's frames are
    // still on the stack when the handler runs -- which names the client-side
    // writer without a static hunt. Empty means the message is queued instead,
    // which is itself the answer to a different question.
    static constexpr size_t kMaxSenderFrames = 8;
    size_t sender_frames(uintptr_t* out, size_t cap) const;

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
    static void on_fire_entry(SafetyHookContext& ctx);
    static void on_message(SafetyHookContext& ctx);
    static void on_send_fire(SafetyHookContext& ctx);
    static void on_vectors_built(SafetyHookContext& ctx);

    std::atomic<bool> m_armed{false};
    std::atomic<Mode> m_mode{Mode::Off};
    std::atomic<int> m_hotkey{0};
    std::atomic<bool> m_hotkey_held{false};
    std::atomic<bool> m_weapon_ok{false};
    std::atomic<uint64_t> m_builds{0};
    std::atomic<float> m_built_dir[3]{};
    std::atomic<float> m_built_origin[3]{};
    std::atomic<bool> m_origin_from_weapon{false};
    std::atomic<bool> m_origin_ok{false};
    std::atomic<uint64_t> m_origin_writes{0};
    std::atomic<float> m_weapon_origin[3]{};
    std::atomic<float> m_weapon_fwd[3]{};
    std::atomic<float> m_weapon_quat[4]{};
    std::atomic<float> m_weapon_obj_quat[4]{};
    std::atomic<bool> m_hooked{false};
    std::atomic<uintptr_t> m_target{0};
    std::atomic<uintptr_t> m_last_desc{0};
    std::atomic<uintptr_t> m_fire_caller{0};
    std::atomic<uint64_t> m_fire_entries{0};
    std::atomic<uint64_t> m_messages{0};
    std::atomic<uint64_t> m_sends{0};
    std::atomic<uintptr_t> m_send_caller{0};
    std::atomic<bool> m_send_hooked{false};
    std::atomic<float> m_sent_dir[3]{};
    std::atomic<float> m_sent_origin[3]{};
    std::atomic<uintptr_t> m_sender[kMaxSenderFrames]{};
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
