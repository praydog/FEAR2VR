#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// `class`, not `struct`: regenny generates these as classes, and MSVC ENCODES THE TAG in the
// mangled name (U for struct, V for class). Declaring them struct here compiled cleanly and then
// failed at link with an unresolved is_weapon whose signature looked identical in the error text.
namespace regenny {
class DatabaseMgrRecord;
class DatabaseMgrCategory;
}  // namespace regenny

namespace sdk {

// ---- THE PLAYER'S WEAPONS ---------------------------------------------------------------------
//
// A VR weapon wheel needs three things the rest of the SDK could not yet answer: what the player is
// holding, what the game could hand them, and how to change it. All three live around the WEAPON
// CHOOSER, the subsystem at player slot +244 whose console variables ('KeepCurrentAmmo',
// 'ChooserAutoSwitchTime', 'ChooserAutoSwitchFreq') named it in an earlier pass.
//
// HOW THE CURRENT-WEAPON FIELD WAS ESTABLISHED, because "a pointer that looks like a weapon" is a
// weak claim and this project has been burned by those:
//
//   1. Every field of the chooser was swept for a pointer whose +0x10 (DatabaseMgrRecord's
//      owner_category) resolves to a category whose name contains "Weapon". That is a TYPED test
//      against the database's own back-pointer, not a guess from a plausible-looking value -- an
//      earlier sweep keyed only on "has a record at +668" returned "GhostAttack1" and
//      "book_chunk_red2", which is exactly the false positive the category check removes.
//
//   2. Two fields survived: +420 read "Unarmed" and +512 read "Assault Rifle".
//
//   3. THE DISCRIMINATOR WAS BEHAVIOUR. Tapping the weapon-slot keys moved +512 to "Submachinegun"
//      while +420 stayed "Unarmed". A field that follows the player's weapon IS the current weapon;
//      one that does not is something else. Neither the offsets' values nor their neighbourhood
//      could have told these apart -- only making the game change one of them.
//
// +420 is NOT named "holster" or "default" here. It held "Unarmed" across every switch, which is
// consistent with several roles the reference source suggests (m_hDefaultWeapon, m_hHolsterWeapon)
// and distinguishes none of them. It is exposed as what it is: a second, stable handle.
class WeaponMgr {
public:
    // The weapon chooser subsystem for a player slot, or 0 when there is no player (menu, loading).
    static uintptr_t chooser(unsigned player_index = 0);

    // ---- WHAT THE PLAYER IS HOLDING ------------------------------------------------------------
    //
    // The database record for the weapon in hand. nullptr when there is no player, or when the
    // field does not hold a record from the weapon category -- never a wrong record, because the
    // category is re-checked on every read rather than trusted from when the offset was found.
    static regenny::DatabaseMgrRecord* current_weapon(unsigned player_index = 0);
    static std::string current_weapon_name(unsigned player_index = 0);

    // The stable second handle described above. Same guarantees, no claimed role.
    static regenny::DatabaseMgrRecord* stable_weapon(unsigned player_index = 0);
    static std::string stable_weapon_name(unsigned player_index = 0);

    // ---- WHAT THE GAME COULD HAND THEM ---------------------------------------------------------
    //
    // The weapon category itself, and every weapon defined in it. This is what a wheel enumerates:
    // it is the database's list, so it includes weapons the player has not picked up.
    static regenny::DatabaseMgrCategory* weapon_category();
    static size_t weapon_count();
    static std::vector<std::string> weapon_names();
    static regenny::DatabaseMgrRecord* find_weapon(std::string_view name);

    // Is this record one of the game's weapons? The predicate the sweep above is built on, exposed
    // because any consumer holding a record from elsewhere wants to ask the same question.
    static bool is_weapon(const regenny::DatabaseMgrRecord* record);

    // Index of a record within the weapon category, so a wheel can address slots by number without
    // holding pointers across a level load. nullopt when the record is not a weapon.
    static std::optional<size_t> weapon_index(const regenny::DatabaseMgrRecord* record);
    static std::optional<size_t> current_weapon_index(unsigned player_index = 0);

    // ---- CAN THE SHOT BE PLACED? ---------------------------------------------------------------
    //
    // True when the held weapon's muzzle ("flash") socket resolves to a usable world transform --
    // the question FireRedirect asks before overriding the ray start, exposed so a consumer does
    // not have to fire a shot to discover the answer.
    //
    // WHAT THIS DOES NOT TELL YOU, recorded because it was added believing the opposite: it is NOT
    // "can this weapon shoot". "Shotgun_Clip" is a record the chooser selects and reports as a
    // weapon, which fires nothing -- no impact effects, no recoil -- and it answers TRUE here, as
    // does every other weapon tried. The muzzle resolving and the gun working are separate facts,
    // and a consumer wanting the second has to observe a shot (see the fixture's empirical gate).
    static bool muzzle_resolvable(unsigned player_index = 0);

    // ---- CHANGING IT ---------------------------------------------------------------------------
    //
    // Selection is NOT done by writing the field, and this class deliberately does not offer a
    // "switch weapon" call. Two reasons, both structural:
    //
    //   * The chooser is listener-driven -- WeaponChooser_Init registers fourteen handlers -- so the
    //     record in +512 is a RESULT. Writing it would leave the model, ammo display and animation
    //     state describing the old weapon.
    //
    //   * The engine's own route is the slot key, and a key press spans FRAMES: down on one, up on a
    //     later one. sdk::Input::send_key must run on the game thread and does one edge, so a tap
    //     belongs to a frame-driven consumer, not to a static call that would have to sleep.
    //
    // What belongs here is the part that is knowledge rather than timing: which key selects a slot.
    static std::optional<uint8_t> slot_virtual_key(unsigned slot);

    // Slots the default bindings cover. Above this, slot_virtual_key() is nullopt rather than a
    // guessed key code.
    static constexpr unsigned kMaxBoundSlot = 9;

    // Offsets, public because a consumer probing a build this SDK has not seen wants to check them.
    static constexpr uintptr_t kChooserSlot = 244;      // player slot -> chooser
    static constexpr uintptr_t kCurrentWeapon = 512;    // chooser -> record, FOLLOWS the player
    static constexpr uintptr_t kStableWeapon = 420;     // chooser -> record, did not follow
    static constexpr uintptr_t kRecordOwnerCategory = 0x10;

    static constexpr std::string_view kWeaponCategory = "Arsenal/Weapons";
};

}  // namespace sdk
