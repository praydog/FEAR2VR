#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

// ILTPhysics, the engine's client-side physics interface -- CLTPhysicsClient, 18 slots, all accounted for.
//
// WHY A VR MOD CARES: this is where objects' velocity, acceleration and movement live, and locomotion has to
// reconcile the headset's motion with the character's.
//
// BUT NOT FOR THE PLAYER, AND THIS HEADER USED TO SAY OTHERWISE. It advised reading what the engine "already
// thinks the player is doing" as the cheap way in. The engine thinks nothing: gameclient's per-frame player
// update brackets its seven subsystem updaters with THREE calls that store a ZERO vector --
// SetAcceleration before, then SetVelocity and SetAcceleration after -- so velocity() and acceleration() on a
// local player read zero no matter what the player is doing. The stores are unconditional: the caller pushes
// `fldz` into a stack triple and passes its address, with no branch. See velocity_zeroed_by_game() below and
// kPlayerUpdateDispatcher for the call site.
//
// The game owns the player's motion and drives the object directly; the engine's integrator is deliberately
// starved for it. A consumer wanting player motion reads sdk::PlayerMgr::movement_state() or ::speed(), which
// is the controller-side velocity the game recomputes each frame as (position - last_position) / dt.
//
// HOW THE SLOT MAP WAS ESTABLISHED, because half of it is worth more than the other half:
//
//   TEN SLOTS FROM STRINGS the functions reference themselves ("CLTPhysicsClient::SetVelocity",
//   "ILTPhysics::GetObjectDims", "CommonLT::GetVelocity"). Those are ground truth from the binary.
//
//   SEVEN FROM BEHAVIOUR, each landing exactly where the reference tree's ILTPhysics declares it. The
//   reference order is PRESERVED WITH DELETIONS -- the offset between reference index and slot drifts
//   monotonically (-3, -2, -2, -4, -4, -4, -4, -5, -5, -5) across the string-anchored slots, so nothing is
//   reordered. But ORDERING ALONE COULD NOT NAME THE GAPS: four slots stood against seven candidate
//   methods. Each was pinned by what it does -- slot 2 tests object type 2 (worldmodel) and returns the
//   LT_YES/LT_NO pair 0x56/0x57; slots 3 and 4 read and write one float at this+4; slot 14 copies three
//   floats out of g_pClientMgr+0x1440.
//
//   ONE SLOT IS NOT IN THE REFERENCE AT ALL: slot 7, a SetObjectDims variant taking an extra out-vector.
//
// FOUR REFERENCE METHODS ARE ABSENT from this build: GetMass/SetMass, GetFrictionCoefficient/
// SetFrictionCoefficient, GetForceIgnoreLimit/SetForceIgnoreLimit, and SetGlobalForce. A consumer porting
// code written against the reference headers will find those missing rather than misplaced -- which is the
// practical reason this map is written down slot by slot instead of "it matches ILTPhysics".
//
// WHAT THIS HEADER DOES AND DOES NOT DO. It exposes the read-only queries that need no object handle, and
// it hands over the slot indices for everything else. It does NOT call the mutators: MoveObject and
// UpdateMovement run engine physics, and doing that from an arbitrary thread is a decision for the
// consumer, not a convenience for this SDK.
namespace sdk {

class Physics {
public:
    // Verified slot map. Names as the reference declares them, so code written against ILTPhysics reads
    // across directly.
    enum class Slot : size_t {
        Destructor = 0,
        InterfaceImplementation = 1,
        IsWorldObject = 2,
        GetStairHeight = 3,
        SetStairHeight = 4,
        GetObjectDims = 5,
        SetObjectDims = 6,
        SetObjectDimsEx = 7,  // FEAR 2 addition, absent from the reference
        GetVelocity = 8,
        SetVelocity = 9,
        GetAcceleration = 10,
        SetAcceleration = 11,
        MoveObject = 12,
        GetStandingOn = 13,
        GetGlobalForce = 14,
        UpdateMovement = 15,
        MovePushObjects = 16,
        RotatePushObjects = 17,
    };

    static constexpr size_t kSlotCount = 18;

    // The ILTPhysics instance, resolved through the interface registry rather than a fixed address --
    // it is null before the database resolves it and can go null again. 0 when unavailable.
    static uintptr_t instance();

    // The vtable the live instance holds, and its class name asked of the binary rather than assumed.
    // A consumer that has just been handed an interface pointer should check this before calling into it.
    static uintptr_t vtable();
    static std::optional<std::string> class_name();

    // Resolved address of one slot, bounds-checked into the exe. Handed over so a consumer can call the
    // mutators deliberately; nullopt when the instance is absent or the entry is not engine code.
    static std::optional<uintptr_t> slot_address(Slot slot);

    // ---- QUERIES THAT NEED NO OBJECT HANDLE -------------------------------------------
    //
    // These are the two the engine answers from its own state, so they can be called safely without
    // resolving a player object first. Both go through the interface's real vtable slot, so a wrong slot
    // index would show up as a fault or nonsense rather than a silently plausible number.

    // The stair-step height the engine will let an object climb. Read from the interface's own field.
    static std::optional<float> stair_height();

    // The global force vector -- gravity, in practice. Copied out of the client manager.
    static std::optional<std::array<float, 3>> global_force();

    // ---- QUERIES ABOUT AN OBJECT ------------------------------------------------------
    //
    // Take a live HOBJECT. Each returns nullopt when the interface is unavailable, the call faulted, or
    // the engine itself reported failure -- the LTRESULT is checked rather than discarded, so a caller
    // cannot mistake "the engine refused" for "the value is zero".

    static std::optional<bool> is_world_object(uintptr_t object);
    static std::optional<std::array<float, 3>> velocity(uintptr_t object);
    static std::optional<std::array<float, 3>> acceleration(uintptr_t object);
    static std::optional<std::array<float, 3>> object_dims(uintptr_t object);

    // LT_OK is 0; the LT_YES/LT_NO pair this build uses for booleans is 0x56/0x57, which is how
    // IsWorldObject reports its answer rather than through a bool return.
    // ---- IS A VELOCITY READING A MOTION SOURCE? ---------------------------------------
    //
    // True when this engine object is a LOCAL PLAYER's, whose velocity and acceleration gameclient zeroes every
    // frame (see the note at the top of this header). A consumer should ask before believing a zero: for a
    // player it means "the game drives this", not "it is not moving".
    //
    // False for every other object AND when the answer cannot be determined -- gameclient absent, no player
    // resolved. That direction is deliberate: it never claims an object IS zeroed without having matched it
    // against a resolved local player.
    static bool velocity_zeroed_by_game(uintptr_t engine_object);

    // The gameclient-relative dispatcher that does the zeroing, so a consumer can read or hook it. It runs the
    // player's seven per-frame subsystem updates between the stores.
    static constexpr uintptr_t kPlayerUpdateDispatcher = 0x10D0A0;

    static constexpr int32_t kLtOk = 0;
    static constexpr int32_t kLtYes = 0x56;
    static constexpr int32_t kLtNo = 0x57;
};

}  // namespace sdk
