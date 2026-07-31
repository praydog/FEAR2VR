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
// A VR weapon wheel needs three things: what the player is holding, what they are carrying, and
// what the game could hand them. All of it hangs off the WEAPON CHOOSER -- the subsystem at player
// slot +244, named by its console variables ('KeepCurrentAmmo', 'ChooserAutoSwitchTime',
// 'ChooserAutoSwitchFreq') and confirmed by its first ~350 bytes being exactly the fourteen
// listener records WeaponChooser_Init (gameclient 0x101361D0) builds.
//
// THE CHOOSER'S LAYOUT, from CClientWeaponMgr_ChangeWeapon (gameclient 0x10136850):
//
//     +400   CClientWeapon** array base, indexed by weapon index
//     +408   current weapon INDEX (uint16; 0xFFFF = none)
//     +412   current CClientWeapon*  -- its +668 is the weapon RECORD
//     +512   LAST weapon record (quick-switch slot)
//
// ---- A CORRECTION, KEPT BECAUSE THE MISTAKE WAS CONVINCING -------------------------------------
//
// An earlier pass exposed +512 as `current_weapon()`. It was wrong, and every piece of evidence
// offered for it was real:
//
//   * +512 holds a record from the weapon category. True.
//   * +512 CHANGES when the player presses a weapon key. Also true.
//   * A fixture check asserting "the held weapon follows a slot key" passed. It did -- against +512.
//
// What was missing is that none of those distinguish "the weapon in hand" from "the weapon you were
// holding a moment ago", and the test could not catch it because it read the same field it was
// validating. The disproof needed an INDEPENDENT route to the same quantity:
//
//     chooser+412 -> CClientWeapon -> +668  ==  "Shotgun_Clip"     (actually held)
//     chooser+512                           ==  "Submachinegun"    (held before that)
//
// The mechanism then confirms it: every call to the +512 setter is gated by Weapon_CanLastWeapon
// (0x10134DF0), which reads the database attribute "CanLastWeapon". +512 is the QUICK-SWITCH slot.
//
// +668 is not a fourth guess either -- it is the field the FIRE PATH reads to decide what is being
// fired, so the weapon the engine shoots and the weapon this reports are the same value.
class WeaponMgr {
public:
    // The weapon chooser subsystem for a player slot, or 0 when there is no player (menu, loading).
    static uintptr_t chooser(unsigned player_index = 0);

    // ---- WHAT THE PLAYER IS HOLDING ------------------------------------------------------------
    //
    // The live CClientWeapon, and the database record it carries. nullptr when there is no player,
    // no weapon is equipped, or the record is not from the weapon category -- the category is
    // re-checked on every read rather than trusted from when the offset was found.
    static uintptr_t current_weapon_object(unsigned player_index = 0);
    static regenny::DatabaseMgrRecord* current_weapon(unsigned player_index = 0);
    static std::string current_weapon_name(unsigned player_index = 0);

    // The chooser's own index for the held weapon, or nullopt when nothing is equipped (0xFFFF).
    // This indexes the CARRIED array below, NOT the database category.
    static std::optional<unsigned> current_slot(unsigned player_index = 0);

    // ---- WHAT THEY ARE CARRYING ----------------------------------------------------------------
    //
    // THE LOADOUT: the weapons the player actually holds, from CPlayerStats' own vector
    // (schema: weapon_slots_begin/end). This is what a VR weapon wheel should show, and it is a
    // different set from both of the other two populations, which is exactly why it needed
    // separating:
    //
    //     loadout   4   Assault Rifle, Submachinegun, Shotgun_Clip, FlameThrower
    //     arsenal  31   every CClientWeapon the client instantiated -- includes Turret_Helicopter
    //                   and the MP weapons, which the player is plainly not carrying
    //     catalogue 56  every record in the database category
    //
    // `slot` is 1-BASED to match the keys the player presses: slot 1 is the '1' key, which
    // CClientWeaponMgr_HandleCommand maps to command 30 and thence to element 0 of this vector.
    // Verified three for three live -- '1'/'2'/'3' selected Assault Rifle/Submachinegun/Shotgun_Clip.
    static size_t loadout_count(unsigned player_index = 0);
    static regenny::DatabaseMgrRecord* loadout_weapon(unsigned slot, unsigned player_index = 0);
    static std::vector<std::string> loadout_names(unsigned player_index = 0);

    // The slot a named weapon occupies, or nullopt when the player is not carrying it. This is the
    // lookup a wheel performs: it has a name and needs the key that selects it.
    static std::optional<unsigned> loadout_slot_of(std::string_view name, unsigned player_index = 0);

    // The key press that selects a carried weapon by name -- the whole read-side answer to "how do
    // I switch to this", in one call. nullopt when it is not carried or its slot has no bound key.
    static std::optional<uint8_t> key_for_weapon(std::string_view name, unsigned player_index = 0);

    // ---- THE CLIENT'S ARSENAL OBJECT TABLE -----------------------------------------------------
    //
    // The chooser's CClientWeapon* array, indexed by WEAPON INDEX (which is what current_slot()
    // returns -- not a loadout slot). Exposed because the live object is where per-weapon state
    // lives, including the record the fire path reads.
    //
    // The count is the engine's OWN: the arsenal keeps a begin/end pair, so this is a stored count
    // rather than a scan that stops when something stops looking like a weapon.
    static uintptr_t arsenal_array(unsigned player_index = 0);
    static uintptr_t arsenal_object(unsigned index, unsigned player_index = 0);
    static regenny::DatabaseMgrRecord* arsenal_weapon(unsigned index, unsigned player_index = 0);
    static std::optional<size_t> arsenal_count();
    static std::vector<std::string> arsenal_names(unsigned player_index = 0);

    // Does the chooser's bookkeeping agree with itself? `arsenal_object(current_slot())` must BE
    // `current_weapon_object()` -- array, index and pointer are three fields written by one
    // function, so a wrong offset in any of them breaks this at once. Nothing external involved.
    static std::optional<bool> current_slot_indexes_current_weapon(unsigned player_index = 0);

    static constexpr unsigned kNoSlot = 0xFFFF;

    // ---- THE MAGAZINE, WHICH IS NOT THE RESERVE ------------------------------------------------
    //
    // Two different numbers, and the HUD shows both ("26/385"). Everything mapped until now was the
    // RESERVE -- sdk::PlayerMgr::ammo_count() and AmmoKeeper, which tops up the pool and explicitly
    // does NOT refill the magazine, so a player kept stocked by it still has to reload.
    //
    // A VR mod needs the magazine specifically: an ammo readout on the gun shows rounds LOADED, and
    // a reload gesture has to know when the magazine is empty rather than when the pool is.
    //
    // Found by differential scan of the live CClientWeapon across a burst: +184 fell 26 -> 19 for
    // seven shots. Per-weapon, as it must be -- the assault rifle read 30 while the submachinegun
    // read 41 at the same instant.
    //
    // A NEIGHBOUR THAT LOOKED LIKE CAPACITY IS NOT NAMED HERE. +200 read 31 alongside a magazine of
    // 26, which is exactly what a capacity would look like -- and it read 31 on the submachinegun
    // too, whose magazine held 41. A value that plausible is why the second weapon was checked.
    static std::optional<int32_t> magazine_rounds(unsigned player_index = 0);

    // ---- AND THE POOL ALREADY INCLUDES IT ------------------------------------------------------
    //
    // sdk::PlayerMgr::ammo_count() is a TOTAL, not a spare count. Measured on one weapon:
    //
    //     firing 10 rounds   magazine -10   pool -10     both fall together
    //     reloading          magazine +10   pool   0     the pool already counted them
    //
    // So a readout showing the pool as "spare" DOUBLE-COUNTS whatever is loaded. This is the number
    // a magazine/spare display actually wants, and the subtraction lives here rather than in every
    // consumer that would otherwise have to rediscover the semantics above.
    //
    // nullopt when either half is unavailable; clamped at zero, because a pool momentarily behind
    // the magazine during a reload is a sampling artefact and not a negative quantity of bullets.
    static std::optional<int32_t> spare_rounds(unsigned player_index = 0);

    // The ammunition TYPE the held weapon consumes, which is what turns the magazine into a pair
    // with a reserve: sdk::PlayerMgr::ammo_count(current_ammo_name()) is the other half of the HUD.
    //
    // On the WEAPON OBJECT at +672, sibling to the weapon record at +668 -- CClientWeaponMgr_ChangeWeapon
    // reads exactly that (`v21 = *(*(this+412) + 672)`) when deciding whether the ammo type changed.
    // The chooser's own +520 was tried first and reads empty: ChangeWeapon only writes it inside a
    // conditional branch, so it is not the live value.
    static regenny::DatabaseMgrRecord* current_ammo(unsigned player_index = 0);
    static std::string current_ammo_name(unsigned player_index = 0);

    static constexpr uintptr_t kWeaponObjectMagazine = 184;  // CClientWeapon -> rounds loaded
    static constexpr uintptr_t kWeaponObjectAmmo = 672;      // CClientWeapon -> ammo record

    // ---- IS A SWITCH IN FLIGHT? ----------------------------------------------------------------
    //
    // Changing weapon is NOT instantaneous, and a VR mod that ignores that gets wrong answers the
    // same way this project's own fixture did twice: a weapon selected a moment ago is mid-DEPLOY,
    // fires nothing, and reports a muzzle that has not arrived.
    //
    // The mechanism, from CClientWeaponMgr_ChangeWeapon (gameclient 0x10136850): when something is
    // already equipped, it stores the REQUESTED weapon at +432, asks the animation system to play
    // the deselect, and defers. The completion callback (WeaponChooser_OnDeselectComplete,
    // 0x10134FA0) then clears +408 to -1 and +412 to null -- so between the request and the new
    // weapon arriving, the player is holding NOTHING and a request is outstanding.
    //
    // Two states worth distinguishing, because a consumer reacts differently to each:
    //   * `pending_weapon()`  -- what was asked for, non-null only while a switch is in flight;
    //   * `equipped()`        -- whether a weapon is actually in hand right now.
    static regenny::DatabaseMgrRecord* pending_weapon(unsigned player_index = 0);
    static std::string pending_weapon_name(unsigned player_index = 0);
    static bool equipped(unsigned player_index = 0);

    // The question a wheel actually asks: "has my request landed yet?" True while the engine is
    // between weapons -- either a request is outstanding or nothing is in hand.
    static bool switching(unsigned player_index = 0);

    // ---- THE QUICK-SWITCH SLOT -----------------------------------------------------------------
    //
    // What "last weapon" would switch back to. Named for what gates it (the CanLastWeapon database
    // attribute), not for what an earlier pass hoped it was.
    static regenny::DatabaseMgrRecord* last_weapon(unsigned player_index = 0);
    static std::string last_weapon_name(unsigned player_index = 0);

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
    static constexpr uintptr_t kChooserSlot = 244;        // player slot -> chooser
    static constexpr uintptr_t kArsenalArray = 400;       // chooser -> CClientWeapon**
    static constexpr uintptr_t kCurrentSlot = 408;        // chooser -> uint16 index
    static constexpr uintptr_t kCurrentWeaponObject = 412; // chooser -> CClientWeapon*
    static constexpr uintptr_t kWeaponObjectRecord = 668;  // CClientWeapon -> record (the fire path's field)
    static constexpr uintptr_t kPendingWeapon = 432;      // chooser -> requested record, mid-switch
    static constexpr uintptr_t kLastWeapon = 512;         // chooser -> record, quick-switch slot
    static constexpr uintptr_t kRecordOwnerCategory = 0x10;

    // The arsenal's own begin/end pair, reached through a gameclient global. Weapon_CanLastWeapon
    // (0x10134DF0) reads the arsenal through this global; Arsenal_GetWeaponIndex (0x10175B50)
    // derives its bound from the vector at +392/+396.
    static constexpr uintptr_t kArsenalGlobalOffset = 0x204530;
    static constexpr uintptr_t kArsenalVectorBegin = 392;
    static constexpr uintptr_t kArsenalVectorEnd = 396;

    static constexpr std::string_view kWeaponCategory = "Arsenal/Weapons";
};

}  // namespace sdk
