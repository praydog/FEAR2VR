#include "Resources.hpp"

#include <cstring>

#include <utility/Seh.hpp>

#include "Modules.hpp"

namespace sdk {

namespace {

// The resource manager singleton, from the lazy initialiser the console commands call before enumerating.
// NOT the container base -- see the header: read live, this object's first dword is 0 where a bucket
// sentinel's self-link would be.
constexpr uintptr_t kManagerOffset = 0x2F24F0;

// Record offsets, each taken from a ListResourcesOfType column accessor rather than guessed.
constexpr uintptr_t kRecName = 0x0C;
constexpr uintptr_t kRecRefcount = 0x10;
constexpr uintptr_t kRecFlags = 0x16;
constexpr uintptr_t kRecData = 0x18;

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

// A resource path, one guarded byte at a time. Rejected outright on a non-printable byte: a wrong pointer
// yields binary, and returning that as a "name" would be worse than returning nothing.
bool read_name(uintptr_t at, std::string& out) {
    out.clear();
    if (at < 0x10000) {
        return false;
    }
    for (size_t i = 0; i < 260; ++i) {
        char c = 0;
        if (!seh_copy(&c, at + i, sizeof(c))) {
            return false;
        }
        if (c == '\0') {
            return out.size() >= 4;
        }
        const auto uc = static_cast<unsigned char>(c);
        if (uc < 32 || uc >= 127) {
            return false;
        }
        out.push_back(c);
    }
    return false;
}

}  // namespace

uintptr_t Resources::manager_address() {
    const auto* exe = Modules::get().exe();
    if (exe == nullptr || exe->base == 0) {
        return 0;
    }
    return exe->base + kManagerOffset;
}

std::optional<Resources::Record> Resources::read(uintptr_t record_address) {
    if (record_address < 0x10000) {
        return std::nullopt;
    }
    uint32_t name_ptr = 0, refcount = 0, data = 0;
    uint8_t flags = 0;
    if (!seh_copy(&name_ptr, record_address + kRecName, sizeof(name_ptr)) ||
        !seh_copy(&refcount, record_address + kRecRefcount, sizeof(refcount)) ||
        !seh_copy(&flags, record_address + kRecFlags, sizeof(flags)) ||
        !seh_copy(&data, record_address + kRecData, sizeof(data))) {
        return std::nullopt;
    }
    Record rec{};
    rec.address = record_address;
    rec.refcount = refcount;
    rec.flags = flags;
    rec.auto_prefetched = (flags & kFlagAutoPrefetched) != 0;
    rec.loaded = data != 0;
    (void)read_name(name_ptr, rec.name);
    return rec;
}

}  // namespace sdk
