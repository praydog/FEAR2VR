#include "WeaponMgr.hpp"

#include "Object.hpp"
#include "regenny/regenny/CClientWeapon.hpp"

#include "DatabaseMgr.hpp"
#include "Memory.hpp"
#include "CClientShell.hpp"
#include "Modules.hpp"
#include "regenny/regenny/CPlayerStats.hpp"
#include "Model.hpp"
#include "PlayerMgr.hpp"

namespace sdk {

namespace {

// The weapon category, resolved fresh rather than cached: a level load rebuilds the database and a
// cached pointer would be a use-after-free that reads as plausible data.
const regenny::DatabaseMgrSubRecord* database_root() {
    auto* db = DatabaseMgr::get();

    if (db == nullptr || db->entry_count() < 1) {
        return nullptr;
    }

    auto* e = db->entry(0);

    if (e == nullptr) {
        return nullptr;
    }

    return e->record_a;
}

// The record a chooser field holds, but ONLY if it really is a weapon. The category is re-checked
// on every read: the offset was found once, and a build where it means something else should return
// nothing rather than a confident wrong answer.
regenny::DatabaseMgrRecord* weapon_at(uintptr_t holder, uintptr_t offset) {
    if (holder == 0) {
        return nullptr;
    }

    const auto value = mem::read_ptr(holder + offset);

    if (!value.has_value() || *value == 0) {
        return nullptr;
    }

    auto* record = reinterpret_cast<regenny::DatabaseMgrRecord*>(*value);

    return WeaponMgr::is_weapon(record) ? record : nullptr;
}

}  // namespace

uintptr_t WeaponMgr::chooser(unsigned player_index) {
    const auto player = PlayerMgr::slot(player_index);

    if (!player.has_value()) {
        return 0;
    }

    return mem::read_ptr(*player + kChooserSlot).value_or(0);
}

regenny::DatabaseMgrCategory* WeaponMgr::weapon_category() {
    const auto* root = database_root();

    if (root == nullptr) {
        return nullptr;
    }

    return DatabaseMgr::find_category(root, kWeaponCategory);
}

bool WeaponMgr::is_weapon(const regenny::DatabaseMgrRecord* record) {
    if (record == nullptr) {
        return false;
    }

    const auto* cat = weapon_category();

    if (cat == nullptr) {
        return false;
    }

    // The record's OWN back-pointer, not a name comparison: this is the test that separated real
    // weapons from the "GhostAttack1"/"book_chunk_red2" false positives a looser sweep produced.
    const auto owner = mem::read_ptr(reinterpret_cast<uintptr_t>(record) + kRecordOwnerCategory);

    return owner.has_value() && *owner == reinterpret_cast<uintptr_t>(cat);
}

uintptr_t WeaponMgr::current_weapon_object(unsigned player_index) {
    const auto ch = chooser(player_index);

    if (ch == 0) {
        return 0;
    }

    return mem::read_ptr(ch + kCurrentWeaponObject).value_or(0);
}

regenny::DatabaseMgrRecord* WeaponMgr::current_weapon(unsigned player_index) {
    // THROUGH THE OBJECT, not through the chooser's own record fields. +668 on the live
    // CClientWeapon is the field the fire path reads, so this and the weapon the engine actually
    // shoots cannot disagree.
    return weapon_at(current_weapon_object(player_index), kWeaponObjectRecord);
}

std::string WeaponMgr::current_weapon_name(unsigned player_index) {
    auto* r = current_weapon(player_index);

    return r == nullptr ? std::string() : DatabaseMgr::record_name(r);
}

std::optional<unsigned> WeaponMgr::current_slot(unsigned player_index) {
    const auto ch = chooser(player_index);

    if (ch == 0) {
        return std::nullopt;
    }

    const auto raw = mem::read<uint16_t>(ch + kCurrentSlot);

    if (!raw.has_value() || *raw == kNoSlot) {
        return std::nullopt;
    }

    return static_cast<unsigned>(*raw);
}

std::optional<int32_t> WeaponMgr::magazine_rounds(unsigned player_index) {
    const auto obj = current_weapon_object(player_index);

    if (obj == 0) {
        return std::nullopt;
    }

    const auto v = mem::read<int32_t>(obj + kWeaponObjectMagazine);

    if (!v.has_value() || *v < 0) {
        return std::nullopt;
    }

    return *v;
}

std::optional<int32_t> WeaponMgr::spare_rounds(unsigned player_index) {
    const auto mag = magazine_rounds(player_index);
    const auto name = current_ammo_name(player_index);

    if (!mag.has_value() || name.empty()) {
        return std::nullopt;
    }

    const auto pool = PlayerMgr::ammo_count(player_index, name);

    if (!pool.has_value()) {
        return std::nullopt;
    }

    return *pool > *mag ? *pool - *mag : 0;
}

regenny::DatabaseMgrRecord* WeaponMgr::current_ammo(unsigned player_index) {
    const auto obj = current_weapon_object(player_index);

    if (obj == 0) {
        return nullptr;
    }

    const auto value = mem::read_ptr(obj + kWeaponObjectAmmo);

    if (!value.has_value() || *value == 0) {
        return nullptr;
    }

    // NOT is_weapon(): this is an AMMO record, from a sibling category. Validated by its name
    // resolving rather than by a category it does not belong to.
    auto* rec = reinterpret_cast<regenny::DatabaseMgrRecord*>(*value);

    return DatabaseMgr::record_name(rec).empty() ? nullptr : rec;
}

std::string WeaponMgr::current_ammo_name(unsigned player_index) {
    auto* r = current_ammo(player_index);

    return r == nullptr ? std::string() : DatabaseMgr::record_name(r);
}

regenny::DatabaseMgrRecord* WeaponMgr::pending_weapon(unsigned player_index) {
    return weapon_at(chooser(player_index), kPendingWeapon);
}

std::string WeaponMgr::pending_weapon_name(unsigned player_index) {
    auto* r = pending_weapon(player_index);

    return r == nullptr ? std::string() : DatabaseMgr::record_name(r);
}

bool WeaponMgr::equipped(unsigned player_index) {
    // BOTH halves, because the completion callback clears them together and a consumer reading only
    // one would see a half-torn state as valid.
    return current_slot(player_index).has_value() && current_weapon_object(player_index) != 0;
}

bool WeaponMgr::switching(unsigned player_index) {
    if (chooser(player_index) == 0) {
        return false;  // no player is not "switching"
    }

    return pending_weapon(player_index) != nullptr || !equipped(player_index);
}

regenny::DatabaseMgrRecord* WeaponMgr::last_weapon(unsigned player_index) {
    return weapon_at(chooser(player_index), kLastWeapon);
}

std::string WeaponMgr::last_weapon_name(unsigned player_index) {
    auto* r = last_weapon(player_index);

    return r == nullptr ? std::string() : DatabaseMgr::record_name(r);
}

uintptr_t WeaponMgr::arsenal_array(unsigned player_index) {
    const auto ch = chooser(player_index);

    if (ch == 0) {
        return 0;
    }

    return mem::read_ptr(ch + kArsenalArray).value_or(0);
}

std::optional<size_t> WeaponMgr::arsenal_count() {
    // The engine's OWN count: Arsenal_GetWeaponIndex (gameclient 0x10175B50) derives it from this
    // begin/end pair, so it is the same number the engine bounds its own lookups with.
    const auto* gc = Modules::get().game_client();

    if (gc == nullptr || gc->base == 0) {
        return std::nullopt;
    }

    const auto arsenal = mem::read_ptr(gc->base + kArsenalGlobalOffset);

    if (!arsenal.has_value() || *arsenal == 0) {
        return std::nullopt;
    }

    const auto begin = mem::read_ptr(*arsenal + kArsenalVectorBegin);
    const auto end = mem::read_ptr(*arsenal + kArsenalVectorEnd);

    if (!begin.has_value() || !end.has_value() || *end < *begin) {
        return std::nullopt;
    }

    return static_cast<size_t>((*end - *begin) / sizeof(void*));
}

uintptr_t WeaponMgr::arsenal_object(unsigned index, unsigned player_index) {
    const auto n = arsenal_count();

    if (!n.has_value() || index >= *n) {
        return 0;
    }

    const auto base = arsenal_array(player_index);

    if (base == 0) {
        return 0;
    }

    return mem::read_ptr(base + index * sizeof(void*)).value_or(0);
}

regenny::DatabaseMgrRecord* WeaponMgr::arsenal_weapon(unsigned index, unsigned player_index) {
    return weapon_at(arsenal_object(index, player_index), kWeaponObjectRecord);
}

std::vector<std::string> WeaponMgr::arsenal_names(unsigned player_index) {
    std::vector<std::string> out;
    const auto n = arsenal_count();

    if (!n.has_value()) {
        return out;
    }

    out.reserve(*n);

    for (unsigned i = 0; i < *n; ++i) {
        auto* rec = arsenal_weapon(i, player_index);
        out.push_back(rec == nullptr ? std::string() : DatabaseMgr::record_name(rec));
    }

    return out;
}

std::optional<bool> WeaponMgr::current_slot_indexes_current_weapon(unsigned player_index) {
    const auto slot = current_slot(player_index);
    const auto obj = current_weapon_object(player_index);

    if (!slot.has_value() || obj == 0) {
        return std::nullopt;  // nothing equipped is a state, not a disagreement
    }

    return arsenal_object(*slot, player_index) == obj;
}

// ---- THE LOADOUT, THROUGH THE SCHEMA -----------------------------------------------------------
//
// CPlayerStats' own vector. The offsets are the generated header's, so the compiler derives them
// and this cannot drift from reversing/fear2.genny.
namespace {

// Through PlayerMgr's own named-subsystem lookup rather than a second copy of the slot offset --
// one place owns "where the stats live", and this is a consumer of it.
uintptr_t player_stats_address(unsigned player_index) {
    const auto sub = PlayerMgr::subsystem_by_name(player_index, "player stats");

    return sub.has_value() ? sub->object : 0;
}

}  // namespace

size_t WeaponMgr::loadout_count(unsigned player_index) {
    const auto stats = player_stats_address(player_index);

    if (stats == 0) {
        return 0;
    }

    const auto begin = mem::read_ptr(stats + offsetof(regenny::CPlayerStats, weapon_slots_begin));
    const auto end = mem::read_ptr(stats + offsetof(regenny::CPlayerStats, weapon_slots_end));

    if (!begin.has_value() || !end.has_value() || *begin == 0 || *end < *begin) {
        return 0;
    }

    return static_cast<size_t>((*end - *begin) / sizeof(void*));
}

regenny::DatabaseMgrRecord* WeaponMgr::loadout_weapon(unsigned slot, unsigned player_index) {
    // 1-BASED: slot 1 is the '1' key. The engine's own indexing is 0-based (command - 30).
    if (slot < 1 || slot > loadout_count(player_index)) {
        return nullptr;
    }

    const auto stats = player_stats_address(player_index);
    const auto begin =
        stats == 0 ? std::nullopt
                   : mem::read_ptr(stats + offsetof(regenny::CPlayerStats, weapon_slots_begin));

    if (!begin.has_value() || *begin == 0) {
        return nullptr;
    }

    const auto entry = mem::read_ptr(*begin + (slot - 1) * sizeof(void*));

    if (!entry.has_value() || *entry == 0) {
        return nullptr;
    }

    auto* rec = reinterpret_cast<regenny::DatabaseMgrRecord*>(*entry);

    return is_weapon(rec) ? rec : nullptr;
}

std::vector<std::string> WeaponMgr::loadout_names(unsigned player_index) {
    std::vector<std::string> out;
    const size_t n = loadout_count(player_index);
    out.reserve(n);

    for (unsigned slot = 1; slot <= n; ++slot) {
        auto* rec = loadout_weapon(slot, player_index);
        out.push_back(rec == nullptr ? std::string() : DatabaseMgr::record_name(rec));
    }

    return out;
}

std::optional<unsigned> WeaponMgr::loadout_slot_of(std::string_view name, unsigned player_index) {
    const size_t n = loadout_count(player_index);

    for (unsigned slot = 1; slot <= n; ++slot) {
        auto* rec = loadout_weapon(slot, player_index);

        if (rec != nullptr && DatabaseMgr::record_name(rec) == name) {
            return slot;
        }
    }

    return std::nullopt;
}

std::optional<uint8_t> WeaponMgr::key_for_weapon(std::string_view name, unsigned player_index) {
    const auto slot = loadout_slot_of(name, player_index);

    return slot.has_value() ? slot_virtual_key(*slot) : std::nullopt;
}

size_t WeaponMgr::weapon_count() {
    const auto* cat = weapon_category();

    return cat == nullptr ? 0 : DatabaseMgr::record_count(cat);
}

std::vector<std::string> WeaponMgr::weapon_names() {
    std::vector<std::string> out;
    const auto* cat = weapon_category();

    if (cat == nullptr) {
        return out;
    }

    const size_t n = DatabaseMgr::record_count(cat);
    out.reserve(n);

    for (size_t i = 0; i < n; ++i) {
        auto* rec = DatabaseMgr::record(cat, i);

        if (rec == nullptr) {
            continue;
        }

        auto name = DatabaseMgr::record_name(rec);

        if (!name.empty()) {
            out.push_back(std::move(name));
        }
    }

    return out;
}

regenny::DatabaseMgrRecord* WeaponMgr::find_weapon(std::string_view name) {
    const auto* cat = weapon_category();

    if (cat == nullptr) {
        return nullptr;
    }

    return DatabaseMgr::find_record(cat, name);
}

std::optional<size_t> WeaponMgr::weapon_index(const regenny::DatabaseMgrRecord* record) {
    if (!is_weapon(record)) {
        return std::nullopt;
    }

    const auto* cat = weapon_category();
    const size_t n = DatabaseMgr::record_count(cat);

    for (size_t i = 0; i < n; ++i) {
        if (DatabaseMgr::record(cat, i) == record) {
            return i;
        }
    }

    return std::nullopt;
}

std::optional<size_t> WeaponMgr::current_weapon_index(unsigned player_index) {
    return weapon_index(current_weapon(player_index));
}

uintptr_t WeaponMgr::current_weapon_model(unsigned player_index) {
    const auto weapon = current_weapon_object(player_index);

    if (weapon == 0) {
        return 0;
    }

    // Through the schema, so the offset is derived by the compiler rather than restated here. The
    // read is still guarded: `weapon` is an engine pointer and the class is only a lower bound on
    // the real layout.
    const auto* w = reinterpret_cast<const regenny::CClientWeapon*>(weapon);
    uintptr_t model = 0;

    if (!mem::copy(&model, reinterpret_cast<uintptr_t>(&w->model_object), sizeof(model)) ||
        model == 0) {
        return 0;
    }

    // Read as an object before handing it out. The field is only trusted so far: a weapon mid-swap
    // can carry a pointer whose object has already gone, and the caller writes through this.
    if (!object_info(reinterpret_cast<const regenny::LTObject*>(model)).has_value()) {
        return 0;
    }

    return model;
}

std::optional<WeaponMgr::Muzzle> WeaponMgr::muzzle(unsigned player_index) {
    const auto model = current_weapon_model(player_index);

    if (model == 0) {
        return std::nullopt;
    }

    const auto* obj = reinterpret_cast<const regenny::LTObject*>(model);
    const auto skeleton = ModelSkeleton::from_object(obj);

    if (!skeleton.has_value()) {
        return std::nullopt;
    }

    const auto index = skeleton->find_socket("flash");

    if (!index.has_value()) {
        return std::nullopt;
    }

    const auto transform = skeleton->socket_world_transform(*index);
    const auto info = object_info(obj);

    if (!transform.has_value() || !info.has_value()) {
        return std::nullopt;
    }

    Muzzle out{};
    out.position = {transform->position.x, transform->position.y, transform->position.z};
    out.stale = transform->stale;

    const auto fwd = forward_of(info->rotation);
    out.forward = {fwd.x, fwd.y, fwd.z};

    return out;
}

bool WeaponMgr::muzzle_resolvable(unsigned player_index) {
    const auto player = CClientShell::local_player(player_index);

    if (!player.has_value() || player->object == nullptr) {
        return false;
    }

    // attached_socket() already refuses a stale skeleton rather than handing back a transform that
    // looks usable -- see Model.hpp. So "has a value" IS the answer, not a starting point.
    return attached_socket(player->object, "flash").has_value();
}

std::optional<uint8_t> WeaponMgr::slot_virtual_key(unsigned slot) {
    // The number-row keys, which is what the default bindings use and what the discriminator that
    // established kCurrentWeapon actually pressed. A REBOUND game breaks this, and there is no
    // apology for that: the binding lives in the player's profile, not in the engine, so a consumer
    // that must be correct under rebinding has to read the profile rather than trust a mapping.
    if (slot < 1 || slot > kMaxBoundSlot) {
        return std::nullopt;
    }

    return static_cast<uint8_t>('0' + slot);
}

}  // namespace sdk
