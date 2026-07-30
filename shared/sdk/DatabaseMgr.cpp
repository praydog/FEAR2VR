#include "DatabaseMgr.hpp"

#include <windows.h>

#include "Memory.hpp"

#include "Modules.hpp"

namespace sdk {

using DbGetFn = void*(__cdecl*)();

namespace {

DbGetFn resolve_getter_fn() {
    const auto* mod = Modules::get().game_database();
    if (mod == nullptr || mod->handle == nullptr) {
        return nullptr;
    }
    return reinterpret_cast<DbGetFn>(
        GetProcAddress(mod->handle, "?LTGetIDatabaseMgr@@YAPAVIDatabaseMgr@@XZ"));
}

// Own function scope: __try cannot share a function with static-local
// initialization (MSVC C2712), and the caller's scope holds the static.
DatabaseMgr* call_getter(DbGetFn fn) {
    void* mgr = nullptr;
    if (!sdk::mem::guarded([&] { mgr = fn(); })) {
        return nullptr;
    }
    return reinterpret_cast<DatabaseMgr*>(mgr);
}

// Own function scope, POD-only locals/return (MSVC C2712: __try cannot share
// a function with a non-trivial return type or non-trivial local objects --
// std::string in the caller would trip it). Generalized over T via a
// pointer-to-member so the STRUCT POINTER DEREFERENCE (obj->*field) itself
// stays inside the SEH guard -- not just the resulting char* walk -- which
// matters because `obj` (record/category/sub-record) can itself be a
// garbled/out-of-range pointer, not only the string it points to. Returns
// the sanitized byte count written into `buf` (nul-terminated), or -1 on
// null/fault.
template <typename T>
int32_t seh_read_strptr_field_into(const T* obj, strptr T::* field, char* buf, size_t buf_size) {
    if (obj == nullptr || buf == nullptr || buf_size == 0) {
        return -1;
    }
    int32_t n = -1;
    sdk::mem::guarded([&] {
        const char* p = obj->*field;
        if (p != nullptr) {
            size_t i = 0;
            for (; i < buf_size - 1; ++i) {
                const char c = p[i];
                if (c == '\0') break;
                buf[i] = (c >= 32 && c < 127) ? c : '.';
            }
            buf[i] = '\0';
            n = static_cast<int32_t>(i);
        }
    });
    return n;
}

// Own function scope, POD-only return (size_t): generic bounds-checked
// "count" read at a given byte offset within `container`, SEH-guarded since
// `container` can be a garbled pointer (e.g. chained from another
// bounds-checked-but-still-live-memory accessor). Used for both
// category_count (offset of DatabaseMgrSubRecord::num_categories) and
// record_count (offset of DatabaseMgrCategory::num_records) -- avoids
// duplicating the SEH scaffolding per call site.
uint32_t seh_read_u32(const void* container, size_t byte_offset) {
    if (container == nullptr) {
        return 0;
    }
    return sdk::mem::read<uint32_t>(reinterpret_cast<uintptr_t>(container) + byte_offset).value_or(0);
}

} // namespace

DatabaseMgr* DatabaseMgr::get() {
    static DbGetFn s_get = resolve_getter_fn();
    if (s_get == nullptr) {
        return nullptr;
    }
    return call_getter(s_get);
}

size_t DatabaseMgr::entry_count() const {
    auto* r = regenny();
    if (r->array_begin == nullptr || r->array_end == nullptr) {
        return 0;
    }
    // Pointer subtraction across two arbitrary live-process pointers is UB in
    // standard C++ (only well-defined within the same array object); do the
    // span math in uintptr_t instead. Still no magic stride: divides by
    // sizeof(regenny::DatabaseMgrEntry) from the type itself.
    const auto begin = reinterpret_cast<uintptr_t>(r->array_begin);
    const auto end = reinterpret_cast<uintptr_t>(r->array_end);
    if (end < begin) {
        return 0; // inverted span -- fail closed
    }
    const uintptr_t span = end - begin;
    if (span % sizeof(regenny::DatabaseMgrEntry) != 0) {
        return 0; // misaligned span -- something's wrong, fail closed rather than truncate-divide
    }
    return static_cast<size_t>(span / sizeof(regenny::DatabaseMgrEntry));
}

regenny::DatabaseMgrEntry* DatabaseMgr::entry(size_t index) const {
    if (index >= entry_count()) {
        return nullptr;
    }
    return regenny()->array_begin + index; // compiler-scaled pointer arithmetic
}

// Thin, non-SEH wrappers: the actual guards live in seh_read_strptr_field_into
// / seh_read_u32 (own function scope, POD-only -- MSVC C2712 forbids __try
// sharing a function with a non-trivial return type/locals like the
// std::string built here).
std::string DatabaseMgr::read_path(const regenny::DatabaseMgrSubRecord* record) {
    char buf[261];
    const int32_t n = seh_read_strptr_field_into(record, &regenny::DatabaseMgrSubRecord::path_data, buf, sizeof(buf));
    return n >= 0 ? std::string(buf, static_cast<size_t>(n)) : std::string();
}

size_t DatabaseMgr::category_count(const regenny::DatabaseMgrSubRecord* database) {
    return seh_read_u32(database, offsetof(regenny::DatabaseMgrSubRecord, num_categories));
}

regenny::DatabaseMgrCategory* DatabaseMgr::category(const regenny::DatabaseMgrSubRecord* database, size_t index) {
    if (index >= category_count(database)) {
        return nullptr;
    }
    // SEH-guarded read of the array-base pointer field (database can be a
    // garbled pointer, same rationale as seh_read_strptr_field_into); the
    // index-scaled arithmetic below is pure address computation, no
    // dereference -- the caller (category_name/record_count/etc.) is the one
    // that actually reads through the returned pointer, and IS SEH-guarded.
    const uint32_t base = seh_read_u32(database, offsetof(regenny::DatabaseMgrSubRecord, categories));
    if (base == 0) {
        return nullptr;
    }
    return reinterpret_cast<regenny::DatabaseMgrCategory*>(base) + index; // compiler-scaled pointer arithmetic
}

size_t DatabaseMgr::record_count(const regenny::DatabaseMgrCategory* category) {
    return seh_read_u32(category, offsetof(regenny::DatabaseMgrCategory, num_records));
}

regenny::DatabaseMgrRecord* DatabaseMgr::record(const regenny::DatabaseMgrCategory* category, size_t index) {
    if (index >= record_count(category)) {
        return nullptr;
    }
    const uint32_t base = seh_read_u32(category, offsetof(regenny::DatabaseMgrCategory, records));
    if (base == 0) {
        return nullptr;
    }
    return reinterpret_cast<regenny::DatabaseMgrRecord*>(base) + index; // compiler-scaled pointer arithmetic
}

std::string DatabaseMgr::category_name(const regenny::DatabaseMgrCategory* category) {
    char buf[261];
    const int32_t n = seh_read_strptr_field_into(category, &regenny::DatabaseMgrCategory::name, buf, sizeof(buf));
    return n >= 0 ? std::string(buf, static_cast<size_t>(n)) : std::string();
}

std::string DatabaseMgr::record_name(const regenny::DatabaseMgrRecord* record) {
    char buf[261];
    const int32_t n = seh_read_strptr_field_into(record, &regenny::DatabaseMgrRecord::name, buf, sizeof(buf));
    return n >= 0 ? std::string(buf, static_cast<size_t>(n)) : std::string();
}


uintptr_t DatabaseMgr::fold_table() {
    const auto* gc = Modules::get().game_client();
    if (gc == nullptr || gc->base == 0) {
        return 0;
    }
    return gc->base + kFoldTableOffset;
}

std::optional<uint32_t> DatabaseMgr::hash_name(std::string_view name) {
    const auto table = fold_table();
    if (table == 0) {
        return std::nullopt;
    }
    uint32_t hash = 0;
    for (const char ch : name) {
        const auto folded = mem::read<uint8_t>(table + static_cast<uint8_t>(ch));
        if (!folded.has_value()) {
            return std::nullopt;
        }
        hash = static_cast<uint32_t>(*folded) + kHashMultiplier * hash;
    }
    return hash;
}

std::optional<bool> DatabaseMgr::category_hash_matches(const regenny::DatabaseMgrCategory* category) {
    if (category == nullptr) {
        return std::nullopt;
    }
    const auto name = category_name(category);
    if (name.empty()) {
        return std::nullopt;
    }
    const auto want = hash_name(name);
    if (!want.has_value()) {
        return std::nullopt;
    }
    const auto got = mem::read<uint32_t>(reinterpret_cast<uintptr_t>(category) + 0x10);
    if (!got.has_value()) {
        return std::nullopt;
    }
    return *got == *want;
}

std::optional<bool> DatabaseMgr::record_hash_matches(const regenny::DatabaseMgrRecord* record) {
    if (record == nullptr) {
        return std::nullopt;
    }
    const auto name = record_name(record);
    if (name.empty()) {
        return std::nullopt;
    }
    const auto want = hash_name(name);
    if (!want.has_value()) {
        return std::nullopt;
    }
    const auto got = mem::read<uint32_t>(reinterpret_cast<uintptr_t>(record) + 0x14);
    if (!got.has_value()) {
        return std::nullopt;
    }
    return *got == *want;
}

DatabaseMgr::HashAgreement DatabaseMgr::category_hash_agreement(
    const regenny::DatabaseMgrSubRecord* database) {
    HashAgreement out;
    const auto n = category_count(database);
    for (size_t i = 0; i < n; ++i) {
        const auto* cat = category(database, i);
        const auto ok = category_hash_matches(cat);
        if (!ok.has_value()) {
            ++out.skipped;
            continue;
        }
        ++out.compared;
        if (*ok) {
            ++out.agreeing;
        }
    }
    return out;
}

DatabaseMgr::HashAgreement DatabaseMgr::record_hash_agreement(
    const regenny::DatabaseMgrSubRecord* database) {
    HashAgreement out;
    const auto ncat = category_count(database);
    for (size_t i = 0; i < ncat; ++i) {
        const auto* cat = category(database, i);
        const auto nrec = record_count(cat);
        for (size_t j = 0; j < nrec; ++j) {
            const auto* rec = record(cat, j);
            const auto ok = record_hash_matches(rec);
            if (!ok.has_value()) {
                ++out.skipped;
                continue;
            }
            ++out.compared;
            if (*ok) {
                ++out.agreeing;
            }
        }
    }
    return out;
}

} // namespace sdk
