#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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
// THE CONTAINER, AND HOW A WRONG GUESS AT IT WAS CAUGHT.
//
// It is a 128-bucket hash table of intrusive circular lists at MANAGER + 0x2C -- not at the manager, which
// is what a first version of this file assumed. THE CONSTRUCTOR SETTLES IT ARITHMETICALLY: it initialises
// something at `this + 44`, and the next field it touches is at 1068 == 44 + 128*8, exactly the table's
// size. A bucket is EMPTY when its first dword points at itself (the engine's own test), its first node
// comes from its SECOND dword, and records link through +0x04 around to the sentinel.
//
// THE WRONG GUESS LOOKED ENTIRELY RIGHT, which is why it stays written down. Taking the manager's own
// address as the table produced a walk that ran to its 65536-record cap while reporting 65534 "printable
// names" and a longest chain of exactly the per-bucket cap -- every figure an artefact of a bound I chose.
// An earlier hand walk's "7814 records" was the same artefact at a smaller guard. THE TELL WAS THAT EACH
// NUMBER EQUALLED ONE OF MY OWN LIMITS, which is why hit_cap is part of Stats rather than an internal
// detail: a caller must be able to tell a count from a ceiling.
//
// VERIFIED ON THE CORRECT BASE: 128 of 128 buckets occupied, and 128 of 128 first nodes carrying plausible
// paths -- worlds\sp\m05_outershell\..., prefabs\e04\general\window_03.inst -- with refcounts of 1, 2, 3
// and mixed load states. On the wrong base the first node's bytes read "anim": a string, not a record, and
// the check that should have been run before building anything on it.
//
// NOT EXPOSED: the "Memory" column, since its accessor returns 0 unless the data pointer is set and
// otherwise asks the loaded object, so it is derived rather than stored. Nor the manager's other
// 1200-odd bytes -- tracking lists, a vtable at +1076, several sub-objects -- which this class does not
// touch.
namespace sdk {

class Resources {
public:
    // A RECORD IS 32 BYTES, and the stride is measured rather than assumed: four consecutive blocks from a
    // bank-allocated run each carry a valid list link and IDs 81, 82, 83, 84 in sequence. A first reading
    // of the memory took the mirrored dwords at +0x20 for more fields of one record; they are the NEXT
    // record.
    static constexpr size_t kRecordSize = 0x20;

    struct Record {
        uintptr_t address{};
        std::string name;
        uint32_t refcount{};

        // NOTE THERE IS NO ID FIELD, and the reason is worth carrying: +0x1C looked like a monotonic
        // resource id on four adjacent records (81, 82, 83, 84) and is not one. Across all 3458 records it
        // holds only 131 DISTINCT values, its maximum is 0xAAAAAAAA -- the debug fill pattern, so it is
        // uninitialised on some records -- and small values recur exactly 28 times each. Structured, but
        // not an identity. A consumer wanting a stable key should hold the ADDRESS or the path.

        uint8_t flags{};
        bool auto_prefetched{};  // flags bit 1
        bool loaded{};           // the +0x18 data pointer is non-null
    };

    static constexpr uint8_t kFlagAutoPrefetched = 0x02;

    // Read one record. Guarded, and the NAME is validated rather than trusted: a path is accepted only if
    // it is printable and at least four characters, so a wrong pointer yields a record with an empty name
    // instead of binary presented as a filename.
    static std::optional<Record> read(uintptr_t record_address);

    static uintptr_t manager_address();
    static uintptr_t table_address();  // manager + 0x2C -- see the container note above

    static constexpr size_t kBucketCount = 128;
    // Caps, so a corrupted list cannot spin the game thread. Live figures sit far below these -- and if a
    // walk ever REACHES one, hit_cap says so, because a count equal to a self-imposed bound is not a
    // measurement. That is exactly how the wrong table base was caught.
    static constexpr size_t kMaxRecords = 262144;
    static constexpr size_t kMaxChain = 32768;

    struct Stats {
        size_t total{};
        size_t named{};
        size_t loaded{};
        size_t auto_prefetched{};
        size_t buckets_used{};
        size_t longest_chain{};
        bool hit_cap{};  // true means these are lower bounds, not counts

        // DISTINCT RECORD ADDRESSES SEEN, which is the sharpest available check on the traversal: an
        // address is unique by construction, so distinct_addresses < total means the walk visited a node
        // twice. That is exactly what a wrong table base produces, and it needs no field in the record to
        // cooperate -- the first version of this check used a supposed id field at +0x1C and that field
        // turned out not to be unique, so the check was measuring the wrong thing.
        size_t distinct_addresses{};
    };

    static std::optional<Stats> stats();

    // Every record, or the first `limit` when non-zero, in bucket order rather than alphabetical.
    static std::vector<Record> all(size_t limit = 0);

    // Exact-name lookup, case-SENSITIVE: the engine's own paths are consistent, and folding case would hide
    // a caller passing the wrong separator or casing.
    static std::optional<Record> find(std::string_view name);

    // Substring search -- the query a consumer actually makes, since resource paths are long and
    // hierarchical ("which materials are loaded").
    static std::vector<Record> search(std::string_view needle, size_t limit = 32);
};

}  // namespace sdk
