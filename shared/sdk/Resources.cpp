#include "Resources.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

#include <utility/Seh.hpp>

#include "Modules.hpp"

namespace sdk {

namespace {

// The resource manager singleton, from the lazy initialiser the console commands call before enumerating.
constexpr uintptr_t kManagerOffset = 0x2F24F0;
// The hash table is a MEMBER at +0x2C, not the manager itself. The constructor proves it arithmetically:
// it initialises something at `this + 44` and the next field it touches is at 1068 == 44 + 128*8.
constexpr uintptr_t kTableWithinManager = 0x2C;
constexpr uintptr_t kRecNext = 0x04;

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

// The one walk every public entry point shares. `visit` returns false to stop early.
template <typename Fn>
bool walk(Fn&& visit, Resources::Stats* stats) {
    const uintptr_t table = Resources::table_address();
    if (table == 0) {
        return false;
    }
    size_t total = 0;
    for (size_t b = 0; b < Resources::kBucketCount; ++b) {
        const uintptr_t bucket = table + b * 8;
        uint32_t self_link = 0, head = 0;
        if (!seh_copy(&self_link, bucket, sizeof(self_link)) ||
            !seh_copy(&head, bucket + 4, sizeof(head))) {
            continue;
        }
        // THE ENGINE'S OWN EMPTINESS TEST is on the FIRST dword, while the first node comes from the
        // SECOND. The two are different fields and conflating them is what produced a non-terminating walk.
        if (self_link == bucket || head == 0 || head == bucket) {
            continue;
        }
        if (stats != nullptr) {
            ++stats->buckets_used;
        }
        uintptr_t node = head;
        size_t chain = 0;
        while (node != 0 && node != bucket && chain < Resources::kMaxChain &&
               total < Resources::kMaxRecords) {
            const auto rec = Resources::read(node);
            if (!rec.has_value()) {
                break;
            }
            ++total;
            ++chain;
            if (stats != nullptr) {
                if (!rec->name.empty()) {
                    ++stats->named;
                }
                if (rec->loaded) {
                    ++stats->loaded;
                }
                if (rec->auto_prefetched) {
                    ++stats->auto_prefetched;
                }
            }
            if (!visit(*rec)) {
                if (stats != nullptr) {
                    stats->total = total;
                }
                return true;
            }
            uint32_t next = 0;
            if (!seh_copy(&next, node + kRecNext, sizeof(next))) {
                break;
            }
            node = next;
        }
        if (stats != nullptr) {
            if (chain > stats->longest_chain) {
                stats->longest_chain = chain;
            }
            if (chain >= Resources::kMaxChain || total >= Resources::kMaxRecords) {
                stats->hit_cap = true;
            }
        }
    }
    if (stats != nullptr) {
        stats->total = total;
    }
    return true;
}

}  // namespace

uintptr_t Resources::manager_address() {
    const auto* exe = Modules::get().exe();
    if (exe == nullptr || exe->base == 0) {
        return 0;
    }
    return exe->base + kManagerOffset;
}

uintptr_t Resources::table_address() {
    const uintptr_t mgr = manager_address();
    return mgr == 0 ? 0 : mgr + kTableWithinManager;
}

std::optional<Resources::Stats> Resources::stats() {
    Stats s{};
    // Distinct ADDRESSES, sorted after the walk rather than hashed during it: an address is unique by
    // construction, so this counts nodes without relying on any record field being an identity.
    std::vector<uintptr_t> seen;
    seen.reserve(4096);
    if (!walk(
            [&](const Record& rec) {
                seen.push_back(rec.address);
                return true;
            },
            &s)) {
        return std::nullopt;
    }
    std::sort(seen.begin(), seen.end());
    seen.erase(std::unique(seen.begin(), seen.end()), seen.end());
    s.distinct_addresses = seen.size();
    return s;
}

std::vector<Resources::Record> Resources::all(size_t limit) {
    std::vector<Record> out;
    walk(
        [&](const Record& rec) {
            out.push_back(rec);
            return limit == 0 || out.size() < limit;
        },
        nullptr);
    return out;
}

std::optional<Resources::Record> Resources::find(std::string_view name) {
    std::optional<Record> found;
    walk(
        [&](const Record& rec) {
            if (rec.name == name) {
                found = rec;
                return false;
            }
            return true;
        },
        nullptr);
    return found;
}

std::vector<Resources::Record> Resources::search(std::string_view needle, size_t limit) {
    std::vector<Record> out;
    if (needle.empty()) {
        return out;
    }
    walk(
        [&](const Record& rec) {
            if (rec.name.find(needle) != std::string::npos) {
                out.push_back(rec);
            }
            return limit == 0 || out.size() < limit;
        },
        nullptr);
    return out;
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
