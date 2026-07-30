#pragma once

#include <cstddef>
#include <cstdint> // generated regenny headers use uint32_t/etc. without including it themselves -- must precede them
#include <optional>
#include <string>
#include <string_view>

// Primitives shim MUST precede any generated header using strptr/wstrptr
// (fear2.genny prelude aliases with no C++ definition of their own).
#include "regenny/Primitives.hpp"
#include "regenny/regenny/DatabaseMgr.hpp"
#include "regenny/regenny/DatabaseMgrCategory.hpp"
#include "regenny/regenny/DatabaseMgrEntry.hpp"
#include "regenny/regenny/DatabaseMgrRecord.hpp"
#include "regenny/regenny/DatabaseMgrSubRecord.hpp"

namespace sdk {

// The engine's attribute/database manager, owned by gamedatabase.dll.
// Evidence: gamedatabase.dll.i64 -- exported accessor
//   ?LTGetIDatabaseMgr@@YAPAVIDatabaseMgr@@XZ
// returns the address of a static CDatabaseMgr object (singleton with lazy
// vtable init inside the dll image).
//
// SDK CLASS CONVENTION (applies to every future mapped type -- CClientMgr,
// CClientShell, etc.): the `regenny::X` type generated from fear2.genny is
// GROUND TRUTH for layout (live-memory verified, offsets/sizes from the type
// system, never hand-guessed). The `sdk::X` class wrapping it:
//   - is sized identically to `regenny::X` (`m_data[sizeof(regenny::X)]`) so
//     it can be reinterpret_cast'd directly onto a live engine instance,
//   - exposes NO raw fields itself -- only `regenny()` to reach the mapped
//     struct for anything not yet worth a named method,
//   - grows real behavior (entry_count(), entry(i), future lookups) as
//     member functions that operate ON TOP of regenny()'s fields, so
//     "complex logic" lives in ONE place instead of being re-derived at
//     every call site (Framework.cpp's diagnostics, tests, future mods).
// Whenever fear2.genny's schema changes, regenerate (sdk:generate) and this
// class's methods keep compiling against the new ground truth or fail loud.
class DatabaseMgr {
public:
    // The IDatabaseMgr singleton, nullptr if gamedatabase.dll or the export
    // is unavailable.
    static DatabaseMgr* get();

    regenny::DatabaseMgr* regenny() const {
        return (regenny::DatabaseMgr*)this;
    }

    // Number of live array entries: (array_end - array_begin) / sizeof(entry).
    // Computed independently from the pointer SPAN, deliberately NOT from
    // regenny()->unk_14 -- unk_14 matches this value in every snapshot
    // observed so far, but its role as a genuinely stored count (vs.
    // coincidence) is UNVERIFIED; see fear2.genny's DatabaseMgr comments.
    // Returns 0 if the array bounds are null or inverted (fail-closed).
    size_t entry_count() const;

    // Bounds-checked entry accessor; nullptr if index >= entry_count().
    // Uses regenny::DatabaseMgrEntry* pointer arithmetic (compiler-scaled by
    // sizeof(DatabaseMgrEntry) -- no manual stride math anywhere).
    regenny::DatabaseMgrEntry* entry(size_t index) const;

    // Safe (SEH-guarded, length-bounded, sanitized) read of a sub-record's
    // path_data string via REAL struct field access (record->path_data,
    // typed strptr = char*) -- exercises the actual mapped-field dereference
    // path any real mod feature would use. `record` may be null (e.g. from
    // entry(i)->record_a on an out-of-range/garbled entry); any failure
    // (null, unreadable, faulting access) returns an empty string rather
    // than crash the caller -- this IS the "does our mapping crash the
    // game" proof, not a side concern.
    static std::string read_path(const regenny::DatabaseMgrSubRecord* record);

    // Number of categories in a loaded database (HDATABASE), and a
    // bounds-checked accessor into its inline trailing category array.
    // CONFIRMED against IDatabaseMgr's own vtable (GameDatabase.dll+0x62DA
    // GetNumCategories, +0x62E8 GetCategoryByIndex; see fear2.genny's
    // DatabaseMgrSubRecord comment) -- these mirror that vtable's exact
    // field/bounds semantics via direct struct reads (no vtable call: no
    // refcount/mutation risk, see AGENT.MD 5a testing corollary).
    static size_t category_count(const regenny::DatabaseMgrSubRecord* database);
    static regenny::DatabaseMgrCategory* category(const regenny::DatabaseMgrSubRecord* database, size_t index);

    // Number of records in a category, and a bounds-checked accessor.
    // CONFIRMED against IDatabaseMgr's own vtable (GameDatabase.dll+0x634F
    // GetNumRecords, +0x635D GetRecordByIndex; see fear2.genny's
    // DatabaseMgrCategory comment).
    static size_t record_count(const regenny::DatabaseMgrCategory* category);
    static regenny::DatabaseMgrRecord* record(const regenny::DatabaseMgrCategory* category, size_t index);

    // Safe (SEH-guarded, length-bounded, sanitized) name reads -- same
    // guarantee as read_path(), generalized via a pointer-to-member so the
    // struct-pointer dereference (obj->*field) itself stays inside the SEH
    // guard, not just the resulting char* walk.
    static std::string category_name(const regenny::DatabaseMgrCategory* category);
    static std::string record_name(const regenny::DatabaseMgrRecord* record);

    // ---- String_HashI, AND THE TWO DATABASE FIELDS IT PRODUCES ----------------------------------
    //
    // THE FUNCTION WAS ALREADY MAPPED, and this header's first version claimed to have found it. It is
    // String_HashI, established by earlier passes in the EXE at 0x004051C0 and confirmed there on two
    // independent name populations -- 191 of 191 skeleton node names and 42 of 42 animation names.
    //
    //     hash = 0;  for each char c:  hash = g_HashCharTable[c] + 919 * hash;
    //
    // GAMECLIENT CARRIES ITS OWN COPY at 0x1002F4F0 with its own table at +0x1C9810, and the tables are
    // BYTE-IDENTICAL to the exe's (both sum to 2766, same fold values). Same maths, different shape: the exe's
    // is __cdecl returning the value, this one is __thiscall writing through a pointer. Two implementations of
    // one algorithm, one per module -- which is why the gameclient IDB had no String_HashI to find.
    //
    // The table is a CASE-FOLDING alphabet map, not a permutation: 'A'..'Z' and 'a'..'z' both map to 1..26,
    // digits to 27..36, '_' to 38, '/' to 52, '\\' to 42, '.' to 55. Hence a case-insensitive hash, checked
    // rather than assumed: hash("GunLead") == hash("gunlead").
    //
    // WHAT IS ACTUALLY NEW HERE is the population, not the function. fear2.genny carried
    // DatabaseMgrCategory+0x10 and DatabaseMgrRecord+0x14 as "plausible name hash, not otherwise confirmed" for
    // several passes, because nothing had connected them to a known hash. They agree with String_HashI(name) for
    // 359 of 359 categories and 28652 of 28652 records, none skipped -- a third and fourth name population for
    // the same function, and 29011 samples more than the first two combined.
    //
    // The route to it was an open item: CMoveMgr_Init appeared to contain two loops over 71 items. 71 is 0x47,
    // the character 'G', and both strings being hashed inline -- "GunLead" and "GamePad" -- begin with it. They
    // are DATABASE ATTRIBUTE names, which is why they allocate no console variable.
    //
    // WHY A CONSUMER WANTS IT: every database lookup by name goes through this, so a precomputed hash finds a
    // category or record without walking strings.
    static constexpr uint32_t kHashMultiplier = 919;
    static constexpr uintptr_t kFoldTableOffset = 0x1C9810;  // gameclient's g_HashCharTable

    // Runtime address of the fold table, 0 when gameclient is not mapped.
    static uintptr_t fold_table();

    // String_HashI over a name. nullopt when the fold table cannot be read -- it is module data, so this is
    // a real possibility rather than a formality. Case-insensitive by construction, not by lowercasing first.
    static std::optional<uint32_t> hash_name(std::string_view name);

    // Does this category's stored value at +0x10 equal the hash of its own name? The question fear2.genny left
    // open. nullopt when either side cannot be read.
    static std::optional<bool> category_hash_matches(const regenny::DatabaseMgrCategory* category);

    // Same for a record's +0x14.
    static std::optional<bool> record_hash_matches(const regenny::DatabaseMgrRecord* record);

    struct HashAgreement {
        size_t compared{};   // how many had both a readable name and a readable value
        size_t agreeing{};   // how many matched
        size_t skipped{};    // unreadable either side

        // The only honest verdict: agreement across a population, not a single sample.
        bool unanimous() const { return compared > 0 && agreeing == compared; }
    };

    // Walk every category of a database and compare each stored value against the hash of its name. This is the
    // measurement that turns "plausible" into established, or refutes it.
    static HashAgreement category_hash_agreement(const regenny::DatabaseMgrSubRecord* database);

    // The same over every record of every category.
    static HashAgreement record_hash_agreement(const regenny::DatabaseMgrSubRecord* database);

    // ---- LOOKUP BY NAME, THE WAY THE ENGINE DOES IT ---------------------------------------------
    //
    // gamedatabase.dll's own by-name entry points hash the name and then BINARY SEARCH on name_hash:
    // IDatabaseMgr_GetCategoryByName -> DatabaseMgr_FindCategoryByHash, and the record pair likewise. Reading
    // those two searches confirmed fear2.genny's layout from a second direction -- they were mapped from the
    // INDEX-based accessors, and the hash searches independently use the same base, count, stride and key:
    //
    //     categories   base hDatabase+0x14   count +0x0C   stride 0x14   key +0x10
    //     records      base category+0x0C    count +0x08   stride 0x18   key +0x14
    //
    // THE ENGINE DOES NOT STRING-COMPARE. It trusts the 32-bit hash, which forces two things a consumer needs:
    //
    //   * BOTH ARRAYS MUST BE SORTED ascending by name_hash, or the search silently fails to find entries that
    //     are present. That is an invariant of the data, not of the code, so it is checkable -- see
    //     categories_sorted_by_hash / records_sorted_by_hash.
    //   * A HASH COLLISION RETURNS THE WRONG ENTRY, with no error. The functions below therefore VERIFY the
    //     name after finding a candidate, which the engine does not. That makes them strictly safer than the
    //     engine's own lookup at the cost of one string compare, and it means a mismatch is reportable rather
    //     than silent.
    //
    // Mirroring the algorithm rather than doing a linear name scan matters for more than speed: if the data is
    // ever NOT sorted, a linear scan would quietly succeed where the game itself fails, and a mod built on that
    // would behave differently from the game for the same name.

    // The category with this name, or nullptr. Binary search on the hash exactly as the engine does, then a
    // string compare to reject a collision -- so nullptr means "not present or a collision", never "wrong one".
    static regenny::DatabaseMgrCategory* find_category(const regenny::DatabaseMgrSubRecord* database,
                                                       std::string_view name);

    // The record with this name inside a category, or nullptr. Same guarantees.
    static regenny::DatabaseMgrRecord* find_record(const regenny::DatabaseMgrCategory* category,
                                                   std::string_view name);

    // Convenience for the usual two-level lookup, e.g. ("AI/WeaponContext", "Default").
    static regenny::DatabaseMgrRecord* find_record(const regenny::DatabaseMgrSubRecord* database,
                                                   std::string_view category_name,
                                                   std::string_view record_name);

    // Is the category array sorted ascending by name_hash, as the binary search requires? False means the
    // engine's own by-name lookup cannot be trusted on this data.
    static bool categories_sorted_by_hash(const regenny::DatabaseMgrSubRecord* database);

    // The same for one category's records.
    static bool records_sorted_by_hash(const regenny::DatabaseMgrCategory* category);

    struct CollisionReport {
        size_t names{};        // how many names were examined
        size_t collisions{};   // distinct pairs sharing a hash with a DIFFERENT name
        size_t duplicates{};   // pairs sharing a hash AND the same name -- not a collision
    };

    // Do any two differently-named entries share a hash? With a 32-bit hash and tens of thousands of names this
    // is worth measuring rather than assuming: the engine would return the wrong entry for one of them.
    //
    // MEASURED, whole database: 28652 record names, ZERO collisions. So hash-only lookup is safe on this data --
    // but see below, because the same measurement turned up something that DOES limit name lookup.
    static CollisionReport hash_collisions(const regenny::DatabaseMgrSubRecord* database);

    // ---- WHERE NAME LOOKUP IS MEANINGFUL, AND WHERE IT IS NOT -----------------------------------
    //
    // The collision scan reported 18374 adjacent same-hash-SAME-NAME pairs, which looked alarming until it was
    // localised: EVERY ONE OF THEM IS IN A SINGLE CATEGORY, `_Structures`.
    //
    //     _Structures        18653 records, 65% of the whole database, only 279 DISTINCT names
    //     everything else     9999 records, 0 duplicate names, 0 collisions
    //
    // So `_Structures` is not a keyed category at all -- it is a pool of anonymous nested-structure instances
    // whose "name" is the structure's TYPE, repeated on average 67 times. find_record there returns whichever
    // instance the binary search lands on, which is arbitrary and almost certainly not what a caller wants.
    // Everywhere else a name identifies exactly one record.
    //
    // This is exposed rather than buried because a consumer cannot tell from the API which case it is in, and
    // the failure is silent: a plausible record comes back either way.
    static constexpr const char* kStructurePoolCategory = "_Structures";

    // Do names uniquely identify records in this category -- i.e. is find_record meaningful here? Walks the
    // category once. False for `_Structures` on the shipped data, true for every other category measured.
    static bool name_is_unique_key(const regenny::DatabaseMgrCategory* category);

    // How many DISTINCT names a category holds, which is what makes the pool obvious: 279 names across 18653
    // records. Equal to record_count for a properly keyed category.
    static size_t distinct_name_count(const regenny::DatabaseMgrCategory* category);

private:
    char m_data[sizeof(regenny::DatabaseMgr)];
};

} // namespace sdk
