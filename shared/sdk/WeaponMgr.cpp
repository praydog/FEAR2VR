#include "WeaponMgr.hpp"

#include "DatabaseMgr.hpp"
#include "Memory.hpp"
#include "CClientShell.hpp"
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

// The record a chooser field holds, but ONLY if it really is a weapon. The category is re-checked on
// every read: the offset was found once, and a build where it means something else should return
// nothing rather than a confident wrong answer.
regenny::DatabaseMgrRecord* weapon_at(unsigned player_index, uintptr_t offset) {
    const auto ch = WeaponMgr::chooser(player_index);

    if (ch == 0) {
        return nullptr;
    }

    const auto value = mem::read_ptr(ch + offset);

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

regenny::DatabaseMgrRecord* WeaponMgr::current_weapon(unsigned player_index) {
    return weapon_at(player_index, kCurrentWeapon);
}

regenny::DatabaseMgrRecord* WeaponMgr::stable_weapon(unsigned player_index) {
    return weapon_at(player_index, kStableWeapon);
}

std::string WeaponMgr::current_weapon_name(unsigned player_index) {
    auto* r = current_weapon(player_index);

    return r == nullptr ? std::string() : DatabaseMgr::record_name(r);
}

std::string WeaponMgr::stable_weapon_name(unsigned player_index) {
    auto* r = stable_weapon(player_index);

    return r == nullptr ? std::string() : DatabaseMgr::record_name(r);
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
