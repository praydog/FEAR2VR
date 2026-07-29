#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

//
// THE GAME'S OWN VIEW OF THE PLAYER -- gameclient.dll's player manager, which is a different thing from the
// engine's object the shell hands out, and for a VR mod the more interesting of the two.
//
// WHY THIS EXISTS BESIDE CClientShell::local_player. The engine keeps the player as an LTObject with position
// at +0x14 and rotation at +0x20. The GAME keeps its own pose in a holder hanging off its player object, at
// +300 and +324, and that holder is also what owns the two engine objects that matter:
//
//     holder +188  ->  a VIEW ANCHOR   (engine LTObject, OT_NORMAL)
//     holder +300      position        LTVector
//     holder +324      rotation        LTRotation, unit quaternion
//     holder +600  ->  the PLAYER MODEL (engine LTObject, OT_MODEL)
//
// So "the player's position" has two answers and a caller must say which it means. This class answers with
// the game's, and hands over both engine objects so a caller can ask the engine's too.
//
// THE MODEL IS *NOT* THE OBJECT THE SHELL NAMES, and getting that wrong is instructive. It reads back as an
// OT_MODEL with dims (40, 95, 40) whose asset is char\player\player\fp_playerm05.mdl -- exactly the pair
// recorded for CClientShell's player slot -- at exactly the same world position. On that evidence an earlier
// pass called them one object "proven by two independent routes". Pointer equality says otherwise:
//
//     holder + 600            handle 0xFFFF   flags 0x00020021   client-created, no server counterpart
//     CClientShell::local_player   handle 0x1CE2   flags 0x000000B9   server-backed
//
// THERE ARE TWO CO-LOCATED PLAYER MODELS, and asset, dims and position are precisely what a duplicate
// shares. The engine's own discriminator is the handle: CLTClient_IsServerObject is nothing but
// `handle != 0xFFFF`. See is_server_backed below.
//
// WHY A CONSUMER MUST CARE: moving or hiding the client-only model is a local change, while the
// server-backed one participates in replication. A VR mod that repoints "the player model" without checking
// which it holds will get one of those two behaviours by accident.
//
// WHAT THE VIEW ANCHOR IS, measured:
//   * engine class vtbl_LTObject_OT_NORMAL -- the plainest object type there is
//   * dims (0, 0, 0) and flags 0x00000000 -- it CANNOT render and CANNOT collide, so carrying a transform is
//     the only thing it does
//   * handle 0xFFFF, i.e. client-created with no server counterpart
//   * sits 70.95 units ABOVE the model's origin (full delta -8.911, 70.948, -0.177) -- eye height
//   * its rotation is BIT-IDENTICAL to the holder's, while its position differs in the low bits: the rotation
//     is copied wholesale, the position is maintained by a separate path or lags a frame
//
// IT IS NOT CALLED THE CAMERA HERE. Every measurement above is consistent with a view anchor and not one of
// them is a code path that says so, and this project has already recorded several names taken from
// plausibility that later turned out wrong. A consumer that wants to attach a VR camera should start here --
// it is the only transform-only object tracking the player's eye -- while knowing that "the engine renders
// from this" is unverified.
//
// WHICH SLOT IS "THE PLAYER". The manager holds four slots and the game's own console commands take the FIRST
// OCCUPIED one, walking past nulls rather than assuming slot 0. Live, slot 0 is filled and 1..3 are null --
// single-player, the same four-slot shape the engine's shell uses. This class follows the game: local_player()
// means first occupied, and slot indices are available for a caller that needs a specific one.
//

namespace sdk {

class PlayerMgr {
public:
    // Manager layout, from ConsoleCmd_GetPlayerPos and its siblings.
    static constexpr size_t kSlotCount = 4;
    static constexpr uintptr_t kSlotsBegin = 80;
    static constexpr uintptr_t kSlotsEnd = 96;  // begin + 4 pointers

    // Player-object and holder layout.
    static constexpr uintptr_t kHolder = 252;
    static constexpr uintptr_t kViewAnchor = 188;
    static constexpr uintptr_t kPosition = 300;
    static constexpr uintptr_t kRotation = 324;
    static constexpr uintptr_t kModelObject = 600;

    struct Pose {
        std::array<float, 3> position{};
        std::array<float, 4> rotation{};  // x, y, z, w -- w LAST, matching regenny::LTRotation

        // Is the rotation a real orientation? A unit-length quaternion is the single strongest cheap check
        // that a rotation offset is right: a wrong one does not produce norm 1. Exposed because a consumer
        // holding a Pose it cached, or one it built itself, wants the same test.
        bool rotation_is_unit(float tolerance = 0.001f) const;
    };

    struct Player {
        uintptr_t object{};       // the game-side player object -- the slot's contents
        uintptr_t holder{};       // object + 252
        Pose pose{};              // the game's pose, from the holder
        uintptr_t view_anchor{};  // engine LTObject, transform-only; 0 when absent

        // The CLIENT-ONLY player model -- not the object CClientShell::local_player returns. See the note
        // above: two co-located models exist and only the handle tells them apart.
        uintptr_t model_object{}; // engine OT_MODEL; 0 when absent
    };

    // The engine's own server/client discriminator, which is literally `handle != 0xFFFF` --
    // CLTClient_IsServerObject's entire body. Exposed because it is the ONLY field that separates the two
    // player models, and a consumer holding an LTObject from anywhere needs the same test.
    //
    // nullopt when the handle cannot be read; false means client-created with no server counterpart.
    static constexpr uint16_t kNoServerHandle = 0xFFFF;
    static std::optional<bool> is_server_backed(uintptr_t object);

    // The manager object, or 0 when gameclient.dll is not mapped. This is a POINTER variable in the DLL's
    // data, so the value is the object rather than the global's address.
    static uintptr_t manager();

    // One slot's contents (a game-side player object), or nullopt for an out-of-range index or an empty
    // slot. Empty and unreadable are deliberately not distinguished: a caller can do nothing different.
    static std::optional<uintptr_t> slot(unsigned index);

    // How many of the four slots are filled. Live this is 1; a split-screen session would report more.
    static unsigned occupied_slot_count();

    // The index the game's own commands would use -- the first non-empty slot, not slot 0.
    static std::optional<unsigned> first_occupied_slot();

    // Everything above for one slot, read in one pass. The pose is validated: a holder whose rotation is not
    // unit-length is refused rather than returned, because that means the offsets are wrong and a caller
    // would otherwise get a plausible-looking transform.
    static std::optional<Player> player(unsigned index);

    // The first occupied slot, which is what "the local player" means to the game.
    static std::optional<Player> local_player();

    //
    // HELPERS, exposed because a consumer needs the same questions answered that this class needed.
    //

    // Read a pose out of a holder address the caller already has. Validated as above.
    static std::optional<Pose> read_pose(uintptr_t holder);

    // Is this quaternion unit-length? The free function behind Pose::rotation_is_unit, for a caller holding
    // four floats from somewhere else.
    static bool quaternion_is_unit(const std::array<float, 4>& q, float tolerance = 0.001f);

    // The view anchor's offset from the model's origin -- the eye offset, and the number a VR mod needs to
    // reconcile a headset pose with the body. Computed from the two ENGINE objects' positions rather than
    // from the holder, so it measures the same thing the engine would.
    //
    // nullopt when either object is missing. Live it reads (-8.911, 70.948, -0.177).
    static std::optional<std::array<float, 3>> eye_offset(unsigned index);

    // Do the anchor's rotation and the holder's pose agree exactly? Live this is true bit-for-bit, and it is
    // the cheapest way for a consumer to tell whether the anchor it is about to read has been refreshed this
    // frame -- the position lags, the rotation does not.
    static std::optional<bool> anchor_rotation_matches_pose(unsigned index);
};

}  // namespace sdk
