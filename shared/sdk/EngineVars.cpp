#include "EngineVars.hpp"

#include <cstring>

#include <utility/Seh.hpp>

#include "Modules.hpp"

namespace sdk {

namespace {

// g_LTEngineVarTable, 107 entries of {const char* name, void* storage, u32 typeAndFlags}.
//
// THE START MOVED TWICE, and both times a scan's own predicate was the boundary. First reading used
// 0x2E3734 and found 22 entries: that address is 25 entries in, and the walk stopped 60 short because it
// rejected any tag above 8 while 6 entries carry flags in the high half. Second reading used 0x2E350C
// and found 106: one entry earlier sits "IP", whose name is TWO characters, and the scan that located
// the table required three.
//
// This walk has no minimum name length -- only non-empty -- so the start offset was its only defect.
// The entry before the table holds 0xBF800000, storage data rather than a pointer, which is what makes
// 0x2E3500 the real beginning.
constexpr uintptr_t kTableOffset = 0x2E3500;
constexpr size_t kEntryStride = 12;

// A cap well above the known 22, so a corrupt table cannot spin. The walk also stops on the first
// entry that fails validation, which is the real terminator.
constexpr size_t kWalkLimit = 128;

// The longest name in this build is SoundMaxNonPlayerWeaponSoundLimit at 33 characters. A generous cap
// keeps a bad pointer from being read as an unbounded string.
constexpr size_t kMaxNameLength = 96;

uintptr_t exe_at(uintptr_t offset) {
    const auto* exe = Modules::get().exe();
    if (exe == nullptr || exe->base == 0) {
        return 0;
    }
    return exe->base + offset;
}

bool seh_copy(void* out, uintptr_t at, size_t bytes) {
    if (at == 0 || out == nullptr || bytes == 0) {
        return false;
    }
    bool ok = false;
    KANANLIB_SEH_TRY {
        std::memcpy(out, reinterpret_cast<const void*>(at), bytes);
        ok = true;
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return ok;
}

bool seh_read_string(std::string& out, uintptr_t at, size_t max) {
    out.clear();
    for (size_t i = 0; i < max; ++i) {
        char c = 0;
        if (!seh_copy(&c, at + i, sizeof(c))) {
            return false;
        }
        if (c == '\0') {
            return true;
        }
        // Setting names are plain identifiers; anything else means the pointer is not a name.
        const bool printable = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                               (c >= 'a' && c <= 'z') || c == '_';
        if (!printable) {
            return false;
        }
        out.push_back(c);
    }
    return false;
}

// Is this address inside the exe? Both the name pointer and the storage pointer must be, which is what
// stops the walk at the end of the table rather than trusting a count.
bool inside_exe(uintptr_t address) {
    const auto* exe = Modules::get().exe();
    if (exe == nullptr || exe->base == 0 || exe->size == 0) {
        return false;
    }
    return address >= exe->base && address < exe->base + exe->size;
}

}  // namespace

bool EngineVars::Entry::is_int() const {
    return type == static_cast<uint32_t>(Type::Int32);
}

bool EngineVars::Entry::is_float() const {
    return type == static_cast<uint32_t>(Type::Float);
}

uintptr_t EngineVars::table_address() {
    return exe_at(kTableOffset);
}

std::vector<EngineVars::Entry> EngineVars::all() {
    std::vector<Entry> out;
    const auto table = table_address();
    if (table == 0) {
        return out;
    }
    for (size_t i = 0; i < kWalkLimit; ++i) {
        const uintptr_t at = table + i * kEntryStride;
        uintptr_t name_ptr = 0;
        uintptr_t storage = 0;
        uint32_t type = 0;
        if (!seh_copy(&name_ptr, at, sizeof(name_ptr)) ||
            !seh_copy(&storage, at + 4, sizeof(storage)) ||
            !seh_copy(&type, at + 8, sizeof(type))) {
            break;
        }
        // THE TERMINATOR IS VALIDATION, NOT A COUNT: both pointers must land in the exe and the tag
        // must be one this build understands. That is what makes a schema change shorten the result
        // instead of walking into unrelated data.
        // The tag is TWO u16 fields: the value type in the low half, flags in the high. Masking is
        // what makes the walk reach all 106 entries instead of stopping at the first flagged one.
        if (!inside_exe(name_ptr) || !inside_exe(storage) || (type & 0xFFFFu) > 2) {
            break;
        }
        Entry entry{};
        if (!seh_read_string(entry.name, name_ptr, kMaxNameLength) || entry.name.empty()) {
            break;
        }
        entry.address = storage;
        entry.type = type & 0xFFFFu;
        entry.flags = static_cast<uint16_t>(type >> 16);
        out.push_back(std::move(entry));
    }
    return out;
}

std::optional<EngineVars::Entry> EngineVars::find(std::string_view name) {
    for (auto& entry : all()) {
        if (entry.name == name) {
            return entry;
        }
    }
    return std::nullopt;
}

std::optional<uint32_t> EngineVars::read_raw(std::string_view name) {
    const auto entry = find(name);
    if (!entry.has_value()) {
        return std::nullopt;
    }
    uint32_t value = 0;
    if (!seh_copy(&value, entry->address, sizeof(value))) {
        return std::nullopt;
    }
    return value;
}

std::optional<int32_t> EngineVars::read_int(std::string_view name) {
    const auto entry = find(name);
    if (!entry.has_value() || !entry->is_int()) {
        return std::nullopt;  // typed refusal: a float setting read as an int is nonsense
    }
    int32_t value = 0;
    if (!seh_copy(&value, entry->address, sizeof(value))) {
        return std::nullopt;
    }
    return value;
}

std::optional<float> EngineVars::read_float(std::string_view name) {
    const auto entry = find(name);
    if (!entry.has_value() || !entry->is_float()) {
        return std::nullopt;  // and an int setting read as a float yields a plausible-looking 6e-44
    }
    float value = 0.0f;
    if (!seh_copy(&value, entry->address, sizeof(value))) {
        return std::nullopt;
    }
    return value;
}

}  // namespace sdk
