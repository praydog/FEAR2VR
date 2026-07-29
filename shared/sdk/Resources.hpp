#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

// ONE RESOURCE-REGISTRY RECORD, and deliberately nothing more.
//
// WHAT IS ESTABLISHED. The `ListResourcesOfType` console command writes a CSV whose header string is the
// engine naming its own columns --
//
//     "Resource, Loaded, AutoPrefetched, RefCount, Memory"
//
// -- and its row writer calls one accessor per column. Every one of those accessors is a PURE FIELD READ,
// so the column names map onto record offsets with nothing inferred:
//
//     +0x0C  const char* name      (the accessor is `return this[3]`)
//     +0x10  refcount              (`return this[4]`)
//     +0x16  flags byte, BIT 1 = auto-prefetched   (`return (byte[22] >> 1) & 1`)
//     +0x18  loaded data pointer; NON-NULL MEANS LOADED   (`return this[6] != 0`)
//
// The "Memory" column is NOT a field: its accessor returns 0 unless the data pointer is set and otherwise
// asks the loaded object, so it is derived and is not exposed here.
//
// CORROBORATED: a hand walk that stumbled through these links produced thousands of real resource paths at
// +0x0C -- worlds\sp\m05_outershell\..., materials\interface\mapicons\..., prefabs\e03\general\gurney.inst --
// with refcounts of 1, 2, 3, 5, the world file reading loaded and unresident prefabs reading not-loaded.
// A wrong name offset would have produced binary immediately.
//
// WHAT IS *NOT* ESTABLISHED, AND WHY THERE IS NO ENUMERATOR HERE.
//
// The container is reached through a lazily-initialised manager singleton, and the engine's iterator walks
// something as 128 two-dword buckets, treating a bucket as empty when its first dword points at itself and
// taking the first node from its second. I took `&unk_6F24F0` -- the singleton's address -- as that table
// base, and it is NOT: read live, its first dword is 0 rather than a self-link, and the address its second
// dword yields begins with the ASCII bytes "anim". That is a string, not a record.
//
// A walk built on that assumption did not terminate. It ran to its own 65536-record cap while reporting
// 65534 "printable names", which is exactly the shape of a plausible-looking wrong answer -- the same
// failure mode this project has hit on extents four times, arriving this time as a traversal instead.
//
// So the table's real base is an offset INSIDE the manager, or the stride is not 8, and neither is measured.
// A consumer that obtains a record pointer by other means can read it with the accessors below; one that
// wants to enumerate the registry cannot do it through this SDK yet, and is better served knowing that than
// being handed a walk that returns 65536 entries.
namespace sdk {

class Resources {
public:
    struct Record {
        uintptr_t address{};
        std::string name;
        uint32_t refcount{};
        uint8_t flags{};
        bool auto_prefetched{};  // flags bit 1
        bool loaded{};           // the +0x18 data pointer is non-null
    };

    static constexpr uint8_t kFlagAutoPrefetched = 0x02;

    // Read one record. Guarded, and the NAME is validated rather than trusted: a path is accepted only if
    // it is printable and at least four characters, so a wrong pointer yields a record with an empty name
    // instead of binary presented as a filename.
    static std::optional<Record> read(uintptr_t record_address);

    // The manager singleton's address -- the object the container lives in or under. Exposed because it is
    // the starting point for finishing the mapping, not because the layout beneath it is known.
    static uintptr_t manager_address();
};

}  // namespace sdk
