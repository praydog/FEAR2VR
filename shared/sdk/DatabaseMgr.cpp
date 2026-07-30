#include "DatabaseMgr.hpp"
#include <cstring>
#include <unordered_set>
#include <unordered_map>

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


namespace {

// The engine's search, generalised: binary search an array of `count` entries of `stride` bytes whose key is a
// uint32 at `key_offset`, all read through the guard.
std::optional<uintptr_t> binary_search_by_hash(uintptr_t base, size_t count, size_t stride,
                                               size_t key_offset, uint32_t want) {
    if (base == 0 || count == 0) {
        return std::nullopt;
    }
    size_t lo = 0;
    size_t hi = count;
    while (lo < hi) {
        const auto mid = lo + (hi - lo) / 2;
        const auto key = mem::read<uint32_t>(base + stride * mid + key_offset);
        if (!key.has_value()) {
            return std::nullopt;
        }
        if (*key == want) {
            return base + stride * mid;
        }
        if (*key < want) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return std::nullopt;
}

}  // namespace

regenny::DatabaseMgrCategory* DatabaseMgr::find_category(const regenny::DatabaseMgrSubRecord* database,
                                                        std::string_view name) {
    const auto want = hash_name(name);
    if (!want.has_value() || database == nullptr) {
        return nullptr;
    }
    const auto count = category_count(database);
    const auto* first = category(database, 0);
    if (count == 0 || first == nullptr) {
        return nullptr;
    }
    const auto hit = binary_search_by_hash(reinterpret_cast<uintptr_t>(first), count,
                                           sizeof(regenny::DatabaseMgrCategory), 0x10, *want);
    if (!hit.has_value()) {
        return nullptr;
    }
    auto* cat = reinterpret_cast<regenny::DatabaseMgrCategory*>(*hit);
    // THE ENGINE STOPS HERE. We do not: a 32-bit hash can collide, and returning the wrong category silently is
    // worse than returning nothing.
    if (category_name(cat) != name) {
        return nullptr;
    }
    return cat;
}

regenny::DatabaseMgrRecord* DatabaseMgr::find_record(const regenny::DatabaseMgrCategory* category_ptr,
                                                    std::string_view name) {
    const auto want = hash_name(name);
    if (!want.has_value() || category_ptr == nullptr) {
        return nullptr;
    }
    const auto count = record_count(category_ptr);
    const auto* first = record(category_ptr, 0);
    if (count == 0 || first == nullptr) {
        return nullptr;
    }
    const auto hit = binary_search_by_hash(reinterpret_cast<uintptr_t>(first), count,
                                           sizeof(regenny::DatabaseMgrRecord), 0x14, *want);
    if (!hit.has_value()) {
        return nullptr;
    }
    auto* rec = reinterpret_cast<regenny::DatabaseMgrRecord*>(*hit);
    if (record_name(rec) != name) {
        return nullptr;
    }
    return rec;
}

regenny::DatabaseMgrRecord* DatabaseMgr::find_record(const regenny::DatabaseMgrSubRecord* database,
                                                    std::string_view category_name_in,
                                                    std::string_view record_name_in) {
    auto* cat = find_category(database, category_name_in);
    if (cat == nullptr) {
        return nullptr;
    }
    return find_record(cat, record_name_in);
}

bool DatabaseMgr::categories_sorted_by_hash(const regenny::DatabaseMgrSubRecord* database) {
    const auto n = category_count(database);
    uint32_t prev = 0;
    bool first = true;
    for (size_t i = 0; i < n; ++i) {
        const auto* cat = category(database, i);
        if (cat == nullptr) {
            return false;
        }
        const auto h = mem::read<uint32_t>(reinterpret_cast<uintptr_t>(cat) + 0x10);
        if (!h.has_value()) {
            return false;
        }
        if (!first && *h < prev) {
            return false;
        }
        prev = *h;
        first = false;
    }
    return n > 0;
}

bool DatabaseMgr::records_sorted_by_hash(const regenny::DatabaseMgrCategory* category_ptr) {
    const auto n = record_count(category_ptr);
    uint32_t prev = 0;
    bool first = true;
    for (size_t i = 0; i < n; ++i) {
        const auto* rec = record(category_ptr, i);
        if (rec == nullptr) {
            return false;
        }
        const auto h = mem::read<uint32_t>(reinterpret_cast<uintptr_t>(rec) + 0x14);
        if (!h.has_value()) {
            return false;
        }
        if (!first && *h < prev) {
            return false;
        }
        prev = *h;
        first = false;
    }
    return true;  // an empty category is trivially sorted
}

DatabaseMgr::CollisionReport DatabaseMgr::hash_collisions(const regenny::DatabaseMgrSubRecord* database) {
    CollisionReport out;
    const auto ncat = category_count(database);
    for (size_t i = 0; i < ncat; ++i) {
        const auto* cat = category(database, i);
        const auto nrec = record_count(cat);
        // Within a category the array is sorted, so equal hashes are ADJACENT -- no map needed, and the check
        // costs one pass. Across categories a shared hash is harmless: lookup is scoped to one category.
        std::string prev_name;
        uint32_t prev_hash = 0;
        bool have_prev = false;
        for (size_t j = 0; j < nrec; ++j) {
            const auto* rec = record(cat, j);
            if (rec == nullptr) {
                continue;
            }
            const auto h = mem::read<uint32_t>(reinterpret_cast<uintptr_t>(rec) + 0x14);
            if (!h.has_value()) {
                continue;
            }
            auto name = record_name(rec);
            ++out.names;
            if (have_prev && *h == prev_hash) {
                if (name == prev_name) {
                    ++out.duplicates;
                } else {
                    ++out.collisions;
                }
            }
            prev_hash = *h;
            prev_name = std::move(name);
            have_prev = true;
        }
    }
    return out;
}


size_t DatabaseMgr::distinct_name_count(const regenny::DatabaseMgrCategory* category_ptr) {
    const auto n = record_count(category_ptr);
    if (n == 0) {
        return 0;
    }
    // The array is sorted by hash, so equal names are adjacent -- counting transitions is enough and costs one
    // pass instead of a set. (Two DIFFERENT names sharing a hash would break that, which is exactly why
    // hash_collisions is measured: it is zero on this data.)
    size_t distinct = 0;
    std::string prev;
    bool have_prev = false;
    for (size_t i = 0; i < n; ++i) {
        auto name = record_name(record(category_ptr, i));
        if (!have_prev || name != prev) {
            ++distinct;
        }
        prev = std::move(name);
        have_prev = true;
    }
    return distinct;
}

bool DatabaseMgr::name_is_unique_key(const regenny::DatabaseMgrCategory* category_ptr) {
    const auto n = record_count(category_ptr);
    if (n == 0) {
        return true;  // vacuously
    }
    return distinct_name_count(category_ptr) == n;
}


size_t DatabaseMgr::struct_dword_count(uint8_t type) {
    switch (type) {
    case kType8Bytes:
        return 2;
    case kType12Bytes:
        return 3;
    case kType16Bytes:
        return 4;
    default:
        return 0;
    }
}

size_t DatabaseMgr::attribute_count(const regenny::DatabaseMgrRecord* record_ptr) {
    if (record_ptr == nullptr) {
        return 0;
    }
    const auto n = mem::read<uint32_t>(reinterpret_cast<uintptr_t>(record_ptr) + kRecordAttributeCount);
    if (!n.has_value() || *n > 4096) {
        return 0;  // thousands of attributes on one record would be a misread, not data
    }
    return *n;
}

std::optional<DatabaseMgr::Attribute> DatabaseMgr::attribute_at(const regenny::DatabaseMgrRecord* record_ptr,
                                                               size_t index) {
    if (record_ptr == nullptr || index >= attribute_count(record_ptr)) {
        return std::nullopt;
    }
    const auto base = reinterpret_cast<uintptr_t>(record_ptr);
    const auto array = mem::read_ptr(base + kRecordAttributeArray);
    const auto blob = mem::read_ptr(base + kRecordValueBlob);
    if (!array.has_value() || *array == 0 || !blob.has_value()) {
        return std::nullopt;
    }
    const auto desc = *array + kAttributeDescriptorSize * index;
    const auto hash = mem::read<uint32_t>(desc);
    const auto raw_type = mem::read<uint8_t>(desc + 4);
    const auto count = mem::read<uint8_t>(desc + 5);
    const auto vindex = mem::read<uint16_t>(desc + 6);
    if (!hash.has_value() || !raw_type.has_value() || !count.has_value() || !vindex.has_value()) {
        return std::nullopt;
    }
    Attribute out;
    out.descriptor = desc;
    out.name_hash = *hash;
    out.raw_type = *raw_type;
    out.type = static_cast<uint8_t>(*raw_type & kTypeMask);
    out.num_values = *count;
    out.value_index = *vindex;
    out.blob = *blob;
    return out;
}

std::optional<DatabaseMgr::Attribute> DatabaseMgr::find_attribute(const regenny::DatabaseMgrRecord* record_ptr,
                                                                 std::string_view name) {
    const auto want = hash_name(name);
    if (!want.has_value() || record_ptr == nullptr) {
        return std::nullopt;
    }
    const auto n = attribute_count(record_ptr);
    const auto array = mem::read_ptr(reinterpret_cast<uintptr_t>(record_ptr) + kRecordAttributeArray);
    if (n == 0 || !array.has_value() || *array == 0) {
        return std::nullopt;
    }
    const auto hit = binary_search_by_hash(*array, n, kAttributeDescriptorSize, 0, *want);
    if (!hit.has_value()) {
        return std::nullopt;
    }
    return attribute_at(record_ptr, (*hit - *array) / kAttributeDescriptorSize);
}

bool DatabaseMgr::has_attribute(const regenny::DatabaseMgrRecord* record_ptr, std::string_view name) {
    return find_attribute(record_ptr, name).has_value();
}

bool DatabaseMgr::attributes_sorted_by_hash(const regenny::DatabaseMgrRecord* record_ptr) {
    const auto n = attribute_count(record_ptr);
    uint32_t prev = 0;
    bool have_prev = false;
    for (size_t i = 0; i < n; ++i) {
        const auto a = attribute_at(record_ptr, i);
        if (!a.has_value()) {
            return false;
        }
        if (have_prev && a->name_hash < prev) {
            return false;
        }
        prev = a->name_hash;
        have_prev = true;
    }
    return true;
}

std::optional<bool> DatabaseMgr::attribute_bool(const Attribute& attribute, size_t i) {
    if (!attribute.is_bit()) {
        return std::nullopt;
    }
    const auto at = attribute.element_address(i);
    if (!at.has_value()) {
        return std::nullopt;
    }
    const auto word = mem::read<uint32_t>(*at);
    if (!word.has_value()) {
        return std::nullopt;
    }
    return ((*word >> attribute.element_bit(i)) & 1u) != 0;
}

std::optional<float> DatabaseMgr::attribute_float(const Attribute& attribute, size_t i) {
    if (attribute.type != kTypeFloat) {
        return std::nullopt;
    }
    const auto at = attribute.element_address(i);
    if (!at.has_value()) {
        return std::nullopt;
    }
    return mem::read<float>(*at);
}

regenny::DatabaseMgrRecord* DatabaseMgr::attribute_record(const Attribute& attribute, size_t i) {
    if (!attribute.is_record_link()) {
        return nullptr;
    }
    const auto at = attribute.element_address(i);
    if (!at.has_value()) {
        return nullptr;
    }
    const auto p = mem::read_ptr(*at);
    if (!p.has_value() || *p == 0) {
        return nullptr;  // the fixup leaves an out-of-range link null
    }
    return reinterpret_cast<regenny::DatabaseMgrRecord*>(*p);
}

std::vector<uint32_t> DatabaseMgr::attribute_struct(const Attribute& attribute, size_t i) {
    std::vector<uint32_t> out;
    const auto n = struct_dword_count(attribute.type);
    if (n == 0) {
        return out;
    }
    const auto at = attribute.element_address(i);
    if (!at.has_value()) {
        return out;
    }
    const auto ptr = mem::read_ptr(*at);
    if (!ptr.has_value() || *ptr == 0) {
        return out;
    }
    for (size_t k = 0; k < n; ++k) {
        const auto d = mem::read<uint32_t>(*ptr + 4 * k);
        if (!d.has_value()) {
            return {};
        }
        out.push_back(*d);
    }
    return out;
}

std::optional<uint32_t> DatabaseMgr::attribute_raw_dword(const Attribute& attribute, size_t i) {
    if (attribute.type != kTypeDwordA && attribute.type != kTypeDwordB && attribute.type != kTypeDwordC) {
        return std::nullopt;
    }
    const auto at = attribute.element_address(i);
    if (!at.has_value()) {
        return std::nullopt;
    }
    return mem::read<uint32_t>(*at);
}

DatabaseMgr::TypeSample DatabaseMgr::sample_type(const regenny::DatabaseMgrSubRecord* database, uint8_t type,
                                                size_t limit) {
    TypeSample out;
    out.type = type;
    const auto ncat = category_count(database);
    for (size_t i = 0; i < ncat && out.sampled < limit; ++i) {
        const auto* cat = category(database, i);
        const auto nrec = record_count(cat);
        for (size_t j = 0; j < nrec && out.sampled < limit; ++j) {
            const auto* rec = record(cat, j);
            const auto na = attribute_count(rec);
            for (size_t k = 0; k < na && out.sampled < limit; ++k) {
                const auto a = attribute_at(rec, k);
                if (!a.has_value() || a->type != type) {
                    continue;
                }
                const auto raw = attribute_raw_dword(*a, 0);
                if (!raw.has_value()) {
                    continue;
                }
                ++out.sampled;
                // Is it plausibly a pointer, and does it dereference to text? read_name applies the guard and
                // the printability test, so an unmapped address just yields nothing.
                if (*raw > 0x10000) {
                    ++out.pointer_like;
                    const auto text = mem::read_name(*raw, 64, 2);
                    if (text.has_value() && !text->empty()) {
                        ++out.ascii_like;
                    }
                    // POSITIVE test for UTF-16 rather than inferring it from the absence of ASCII: read pairs
                    // and require a printable low byte with a zero high byte, for at least two characters
                    // followed by a null pair.
                    size_t wide_chars = 0;
                    bool wide = true;
                    for (size_t w = 0; w < 32; ++w) {
                        const auto unit = mem::read<uint16_t>(*raw + 2 * w);
                        if (!unit.has_value()) {
                            wide = false;
                            break;
                        }
                        if (*unit == 0) {
                            break;
                        }
                        if (*unit < 0x20 || *unit > 0x7E) {
                            wide = false;
                            break;
                        }
                        ++wide_chars;
                    }
                    if (wide && wide_chars >= 2) {
                        ++out.utf16_like;
                    }
                    // AND THE SHAPE FOUND BY LOOKING: a zero dword header followed by text. read_name at
                    // offset 0 stops on that header, which is why an ASCII test starting there reports nothing.
                    const auto header = mem::read<uint32_t>(*raw);
                    const auto tail = mem::read_name(*raw + 4, 64, 2);
                    if (header.has_value() && *header == 0 && tail.has_value() && !tail->empty()) {
                        ++out.ascii_at_4;
                    }
                }
            }
        }
    }
    return out;
}


namespace {

std::unordered_map<uint32_t, std::string>& name_index_storage() {
    static std::unordered_map<uint32_t, std::string> s_map;
    return s_map;
}

DatabaseMgr::NameIndex& name_index_state() {
    static DatabaseMgr::NameIndex s_state;
    return s_state;
}

}  // namespace

const DatabaseMgr::NameIndex& DatabaseMgr::build_name_index() {
    auto& state = name_index_state();
    if (state.distinct_hashes != 0) {
        return state;  // module data does not change; one scan is enough
    }
    auto& map = name_index_storage();
    const auto* gc = Modules::get().game_client();
    if (gc == nullptr || gc->base == 0) {
        return state;
    }
    // Walk the module's DATA sections only. Code contains printable byte runs that are not strings, and this is
    // exactly the "points into the module is not enough" lesson applied to a different question.
    for (uintptr_t addr = gc->base; addr < gc->base + gc->size;) {
        const auto sec = Modules::section_of(addr);
        if (!sec.has_value()) {
            ++addr;
            continue;
        }
        if (sec->kind != Modules::SectionKind::Data) {
            addr = sec->end;
            continue;
        }
        // Scan this section for NUL-terminated printable runs.
        std::string current;
        for (uintptr_t p = sec->start; p < sec->end; ++p) {
            const auto ch = mem::read<uint8_t>(p);
            if (!ch.has_value()) {
                current.clear();
                continue;
            }
            if (*ch >= 0x20 && *ch <= 0x7E) {
                if (current.size() < 96) {
                    current.push_back(static_cast<char>(*ch));
                } else {
                    current.clear();
                }
                continue;
            }
            if (*ch == 0 && current.size() >= 3) {
                // INDEX THE RUN AND ITS FIRST FEW SUFFIXES. A printable run is not necessarily a string: a
                // float or integer stored just before a literal can contribute printable bytes that get glued
                // onto its front. "WaterAffectsSpeed" is preceded by the float 280.0f, whose exponent byte is
                // 0x43 = 'C', so the run reads "CWaterAffectsSpeed" and the real name never appears.
                //
                // Requiring a NUL before the run would reject the glued form AND lose the real name with it, so
                // instead the first few offsets are indexed as well. Three is enough for one glued dword's worth
                // of printable bytes and keeps the map small; deeper suffixes would trade precision for reach.
                for (size_t skip = 0; skip <= 3 && current.size() - skip >= 3; ++skip) {
                    const std::string candidate = current.substr(skip);
                    const auto hash = hash_name(candidate);
                    if (hash.has_value()) {
                        if (skip == 0) {
                            ++state.strings_scanned;
                        }
                        map.emplace(*hash, candidate);
                    }
                }
            }
            current.clear();
        }
        addr = sec->end;
    }
    state.distinct_hashes = map.size();
    return state;
}

std::optional<std::string> DatabaseMgr::name_for_hash(uint32_t hash) {
    build_name_index();
    const auto& map = name_index_storage();
    const auto it = map.find(hash);
    if (it == map.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::optional<std::string> DatabaseMgr::attribute_name(const Attribute& attribute) {
    return name_for_hash(attribute.name_hash);
}

DatabaseMgr::NameCoverage DatabaseMgr::name_coverage(const regenny::DatabaseMgrSubRecord* database,
                                                    size_t record_limit) {
    NameCoverage out;
    build_name_index();
    std::unordered_set<uint32_t> seen;
    const auto ncat = category_count(database);
    for (size_t i = 0; i < ncat; ++i) {
        const auto* cat = category(database, i);
        const auto nrec = record_count(cat);
        for (size_t j = 0; j < nrec; ++j) {
            if (record_limit != 0 && out.records_scanned >= record_limit) {
                break;
            }
            const auto* rec = record(cat, j);
            ++out.records_scanned;
            const auto na = attribute_count(rec);
            for (size_t k = 0; k < na; ++k) {
                const auto a = attribute_at(rec, k);
                if (!a.has_value()) {
                    continue;
                }
                if (!seen.insert(a->name_hash).second) {
                    continue;
                }
                ++out.distinct_attribute_hashes;
                if (name_for_hash(a->name_hash).has_value()) {
                    ++out.resolved;
                }
            }
        }
    }
    return out;
}

DatabaseMgr::StructSample DatabaseMgr::sample_struct_type(const regenny::DatabaseMgrSubRecord* database,
                                                         uint8_t type, size_t limit) {
    StructSample out;
    out.type = type;
    const auto want = struct_dword_count(type);
    if (want == 0) {
        return out;
    }
    const auto ncat = category_count(database);
    for (size_t i = 0; i < ncat && out.sampled < limit; ++i) {
        const auto* cat = category(database, i);
        const auto nrec = record_count(cat);
        for (size_t j = 0; j < nrec && out.sampled < limit; ++j) {
            const auto* rec = record(cat, j);
            const auto na = attribute_count(rec);
            for (size_t k = 0; k < na && out.sampled < limit; ++k) {
                const auto a = attribute_at(rec, k);
                if (!a.has_value() || a->type != type) {
                    continue;
                }
                const auto dwords = attribute_struct(*a, 0);
                if (dwords.size() != want) {
                    continue;
                }
                ++out.sampled;
                bool floats = true;
                bool denorm = false;
                bool small_int = true;
                for (const auto d : dwords) {
                    float f = 0.0f;
                    std::memcpy(&f, &d, sizeof(f));
                    const auto mag = f < 0.0f ? -f : f;
                    if (!(f == f) || (mag != 0.0f && (mag < 1e-30f || mag > 1e12f))) {
                        floats = false;
                    }
                    if (mag != 0.0f && mag < 1e-30f) {
                        denorm = true;
                    }
                    if (d > 100000u) {
                        small_int = false;
                    }
                }
                if (floats) {
                    ++out.all_float_like;
                }
                if (denorm) {
                    ++out.any_denormal;
                }
                if (small_int) {
                    ++out.all_small_int;
                }
            }
        }
    }
    return out;
}


std::optional<std::string> DatabaseMgr::attribute_text(const Attribute& attribute, size_t i) {
    if (attribute.type != kTypeDwordB && attribute.type != kTypeDwordC) {
        return std::nullopt;
    }
    const auto raw = attribute_raw_dword(attribute, i);
    if (!raw.has_value() || *raw <= 0x10000) {
        return std::nullopt;
    }
    return mem::read_name(*raw + 4, 128, 1);
}

DatabaseMgr::StringHeaderSample DatabaseMgr::sample_string_header(const regenny::DatabaseMgrSubRecord* database,
                                                                uint8_t type, size_t limit) {
    StringHeaderSample out;
    out.type = type;
    if (type != kTypeDwordB && type != kTypeDwordC) {
        return out;
    }
    const auto ncat = category_count(database);
    for (size_t i = 0; i < ncat && out.sampled < limit; ++i) {
        const auto* cat = category(database, i);
        const auto nrec = record_count(cat);
        for (size_t j = 0; j < nrec && out.sampled < limit; ++j) {
            const auto* rec = record(cat, j);
            const auto na = attribute_count(rec);
            for (size_t k = 0; k < na && out.sampled < limit; ++k) {
                const auto a = attribute_at(rec, k);
                if (!a.has_value() || a->type != type) {
                    continue;
                }
                const auto raw = attribute_raw_dword(*a, 0);
                if (!raw.has_value() || *raw <= 0x10000) {
                    continue;
                }
                const auto header = mem::read<uint32_t>(*raw);
                if (!header.has_value()) {
                    continue;
                }
                ++out.sampled;
                if (*header == 0) {
                    ++out.header_zero;
                }
                const auto text = mem::read_name(*raw + 4, 128, 1);
                if (!text.has_value() || text->empty()) {
                    continue;
                }
                ++out.text_readable;
                if (text->rfind("IDS_", 0) == 0) {
                    ++out.text_is_ids_key;
                }
                const auto want = hash_name(*text);
                if (want.has_value() && *want == *header) {
                    ++out.header_is_text_hash;
                }
            }
        }
    }
    return out;
}


namespace {

std::string hex32(uint32_t v) {
    char buf[16]{};
    snprintf(buf, sizeof(buf), "0x%08X", v);
    return buf;
}

std::string render_float(float f) {
    char buf[32]{};
    snprintf(buf, sizeof(buf), "%g", static_cast<double>(f));
    return buf;
}

}  // namespace

std::vector<DatabaseMgr::DescribedAttribute> DatabaseMgr::describe_record(
    const regenny::DatabaseMgrRecord* record_ptr, size_t max_elements) {
    std::vector<DescribedAttribute> out;
    const auto n = attribute_count(record_ptr);
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        const auto a = attribute_at(record_ptr, i);
        if (!a.has_value()) {
            continue;
        }
        DescribedAttribute d;
        d.name_hash = a->name_hash;
        d.name = attribute_name(*a);
        d.type = a->type;
        d.num_values = a->num_values;

        const size_t shown = a->num_values < max_elements ? a->num_values : max_elements;
        d.value.truncated = a->num_values > shown;
        std::string rendered;
        for (size_t e = 0; e < shown; ++e) {
            if (!rendered.empty()) {
                rendered += ", ";
            }
            switch (a->type) {
            case kTypeBool: {
                const auto b = attribute_bool(*a, e);
                rendered += b.has_value() ? (*b ? "true" : "false") : "?";
                break;
            }
            case kTypeFloat: {
                const auto f = attribute_float(*a, e);
                rendered += f.has_value() ? render_float(*f) : "?";
                break;
            }
            case kTypeString:
            case kTypeLocalizedKey: {
                const auto t = attribute_text(*a, e);
                rendered += t.has_value() ? ("\"" + *t + "\"") : "?";
                break;
            }
            case kTypeRecordLink:
            case kTypeRecordLinkAlt: {
                auto* linked = attribute_record(*a, e);
                if (linked == nullptr) {
                    rendered += "<null link>";
                } else {
                    const auto nm = record_name(linked);
                    rendered += "->" + (nm.empty() ? hex32(static_cast<uint32_t>(
                                                          reinterpret_cast<uintptr_t>(linked)))
                                                   : nm);
                }
                break;
            }
            case kType8Bytes:
            case kType12Bytes:
            case kType16Bytes: {
                const auto dwords = attribute_struct(*a, e);
                if (dwords.empty()) {
                    rendered += "?";
                    break;
                }
                // Types 7 and 8 measured as floats; type 6 is UNDECIDED, so it renders as hex rather than
                // committing to a reading the measurement did not support.
                // All three struct types are established as float vectors, type 6 by Client/CameraClamping's
                // exact round angles rather than by the sampling, which could not discriminate.
                const bool as_float = true;
                rendered += "(";
                for (size_t k = 0; k < dwords.size(); ++k) {
                    if (k != 0) {
                        rendered += " ";
                    }
                    if (as_float) {
                        float f = 0.0f;
                        std::memcpy(&f, &dwords[k], sizeof(f));
                        rendered += render_float(f);
                    } else {
                        rendered += hex32(dwords[k]);
                    }
                }
                rendered += ")";
                break;
            }
            default: {
                // Type 3 (integer by elimination, signedness unestablished) and anything unseen: raw hex.
                const auto raw = attribute_raw_dword(*a, e);
                rendered += raw.has_value() ? hex32(*raw) : "?";
                break;
            }
            }
        }
        d.value.text = std::move(rendered);
        out.push_back(std::move(d));
    }
    return out;
}

std::vector<std::string> DatabaseMgr::describe_record_lines(const regenny::DatabaseMgrRecord* record_ptr,
                                                           size_t max_elements) {
    std::vector<std::string> out;
    for (const auto& d : describe_record(record_ptr, max_elements)) {
        std::string line = d.named() ? *d.name : ("#" + hex32(d.name_hash));
        line += " t" + std::to_string(d.type);
        if (d.num_values != 1) {
            line += "[" + std::to_string(d.num_values) + "]";
        }
        line += " = " + d.value.text;
        if (d.value.truncated) {
            line += ", ...";
        }
        out.push_back(std::move(line));
    }
    return out;
}

DatabaseMgr::DescribeCoverage DatabaseMgr::describe_coverage(const regenny::DatabaseMgrRecord* record_ptr) {
    DescribeCoverage out;
    for (const auto& d : describe_record(record_ptr, 1)) {
        ++out.attributes;
        if (d.named()) {
            ++out.named;
        }
        if (!d.value.text.empty() && d.value.text != "?") {
            ++out.valued;
        }
    }
    return out;
}


std::vector<float> DatabaseMgr::attribute_floats(const Attribute& attribute, size_t i) {
    std::vector<float> out;
    for (const auto d : attribute_struct(attribute, i)) {
        float f = 0.0f;
        std::memcpy(&f, &d, sizeof(f));
        out.push_back(f);
    }
    return out;
}

std::optional<DatabaseMgr::FloatPair> DatabaseMgr::attribute_float_pair(const Attribute& attribute, size_t i) {
    if (attribute.type != kTypeFloatPair) {
        return std::nullopt;
    }
    const auto f = attribute_floats(attribute, i);
    if (f.size() != 2) {
        return std::nullopt;
    }
    return FloatPair{f[0], f[1]};
}

} // namespace sdk
