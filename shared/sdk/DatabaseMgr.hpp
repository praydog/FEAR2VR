#pragma once

#include <cstddef>
#include <cstdint> // generated regenny headers use uint32_t/etc. without including it themselves -- must precede them
#include <optional>
#include <vector>
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

    // ---- A RECORD'S ATTRIBUTES, FROM THE ENGINE'S OWN DECODER ------------------------------------
    //
    // The third level of the same design. DatabaseMgr_FindAttributeByHash binary-searches a record's descriptor
    // array on an attribute-name hash, and DatabaseMgr_DecodeAttributeValue turns a descriptor into a value
    // location. Between them they settle three fields fear2.genny carried as guesses:
    //
    //     record +0x04   uint32   ATTRIBUTE COUNT     (was "plausible attribute count")
    //     record +0x08   ptr      DESCRIPTOR ARRAY    (was "plausible attribute-descriptor array")
    //     record +0x0C   ptr      VALUE BLOB          (was "unverified")
    //
    // AND THE DESCRIPTOR IS THE genny's HashEntry, whose four trailing bytes were all "meaning unverified":
    //
    //     +0  uint32  name_hash    the search key -- String_HashI of the attribute name
    //     +4  uint8   type         ONLY THE LOW 6 BITS are the type; the decoder masks & 0x3F
    //     +5  uint8   passed through to the decoded value's +6, meaning still unestablished
    //     +6  uint16  value_index  index into the value blob, IN DWORDS
    //
    // The genny's own observations line up exactly: it recorded +4 taking values 01,02,04,06,07,09 (a small type
    // enum), +6 "small incrementing-looking values" and +7 "constant 0x00" -- which is a little-endian 16-bit
    // index whose high byte is zero because records have few attributes.
    //
    // TYPE 1 IS A PACKED BIT, which is the part that would silently corrupt a naive reader:
    //
    //     type 1 : value = blob + 4 * (value_index >> 5),  bit = value_index & 0x1F
    //     else   : value = blob + 4 * value_index,         whole dword
    //
    // So booleans are stored 32 to a dword and the 16-bit field is a BIT index for them, not a dword index.
    // Reading a type-1 attribute as a dword yields up to 31 unrelated neighbours' values.
    static constexpr uintptr_t kRecordAttributeCount = 0x04;
    static constexpr uintptr_t kRecordAttributeArray = 0x08;
    static constexpr uintptr_t kRecordValueBlob = 0x0C;
    static constexpr size_t kAttributeDescriptorSize = 8;

    // ---- EVERY ATTRIBUTE IS AN ARRAY, WHICH THE FIRST VERSION OF THIS MISSED --------------------
    //
    // DatabaseMgr_DecodeAttributeValue decodes ELEMENT ZERO, and reading only it made attributes look scalar.
    // The engine's nine typed getters all take a value index and bounds-check it against descriptor+5 -- the
    // byte recorded here as "meaning unestablished". IT IS THE ELEMENT COUNT, and the reference names the same
    // thing GetNumValues(HATTRIBUTE).
    //
    //     element i of any type :  blob dword index = value_index + i
    //     type 1 (bit)          :  dword = blob + 4 * ((value_index + i) >> 5),  bit = (value_index + i) & 0x1F
    //
    // ---- THE TYPE TAGS ----
    //
    // Live the low 6 bits take values 1..9 and never 0. All nine getters were read; the STORAGE of each is
    // established from its own code, while the C type is only claimed where evidence outside this binary exists.
    //
    //   1  BOOL, a packed bit. Two routes: the bit arithmetic, and the reference reading WaterAffectsSpeed
    //      with GetBool -- which live carries type 1.
    //   2  FLOAT, read straight from the blob dword. YawClamp and YawBias carry it and CMoveMgr reads floats.
    //   3  NOT A POINTER. Sampled 400: only 40 land in committed memory and none dereference to text, so the
    //      dword is a value. Integer-like, and left at that.
    //   4, 5  BOTH POINT TO A {uint32 header, char text[]} STRUCTURE, measured rather than assumed:
    //        type 4  400/400 are pointers, and 293 read as text from offset 0 while the other 107 have a ZERO
    //                header with text at +4. 293 + 107 = 400 exactly, so it is one layout whose header is
    //                sometimes zero and sometimes not.
    //        type 5  33/33 have the zero header with text at +4, and the one sampled reads "IDS_PLAYER_N..." --
    //                a localization key, in Profile/Multiplayer.
    //      WHAT SEPARATES 4 FROM 5 IS NOT ESTABLISHED. The layout is shared; only the tag and the population
    //      differ. A wide-string reading of 5 was TESTED AND REFUTED: a positive UTF-16 check (printable low
    //      bytes, zero high bytes) matched 0 of 33, which is why the check exists instead of inferring "wide"
    //      from the absence of ASCII.
    //   6  A POINTER to 8 bytes, copied as two dwords.
    //   7  A POINTER to 12 bytes. The size and shape of an LTVector, though nothing proves the components are
    //      floats.
    //   8  A POINTER to 16 bytes -- an LTVector4 or an LTRotation by size.
    //   9  A RECORD LINK, and this CORRECTS the previous pass. It was recorded as "[INFERENCE] a nested-structure
    //      reference"; it is a link to another RECORD. DatabaseMgr_FixupRecordLinks rewrites each element in
    //      place at load time from a packed {uint16 category_index, uint16 record_index} into a real
    //      DatabaseMgrRecord*, so a reader gets a usable pointer. The old wording was directionally right and
    //      wrong in the mechanism.
    //  10  Also a record link -- the fixup handles 9 AND 10 -- but no attribute in the shipped database uses it,
    //      so it is defined and unexercised.
    //
    // > That fixup confirms the category and record layouts a THIRD time (stride 0x14 / 0x18, num_records at
    // > +0x08, records at +0x0C), independently of the index accessors and the by-hash searches. And it explains
    // > the _Structures pool: links are how records reference each other, and the sub-records they point at live
    // > there -- which is why names repeat (the name is the struct TYPE) and identity comes from the link.
    static constexpr uint8_t kTypeMask = 0x3F;
    static constexpr uint8_t kTypeBool = 1;        // packed bit
    static constexpr uint8_t kTypeFloat = 2;
    static constexpr uint8_t kTypeDwordA = 3;      // storage-identical trio, C type unestablished
    static constexpr uint8_t kTypeDwordB = 4;
    static constexpr uint8_t kTypeDwordC = 5;
    static constexpr uint8_t kType8Bytes = 6;
    static constexpr uint8_t kType12Bytes = 7;     // LTVector by size
    static constexpr uint8_t kType16Bytes = 8;     // LTVector4 / LTRotation by size
    static constexpr uint8_t kTypeRecordLink = 9;
    static constexpr uint8_t kTypeRecordLinkAlt = 10;  // defined, unused in the shipped data

    // Kept for the historical name; kTypeBool says why it is special.
    static constexpr uint8_t kTypeBit = kTypeBool;

    // How many dwords the pointed-at struct holds, for types 6/7/8; 0 for every other type.
    static size_t struct_dword_count(uint8_t type);

    struct Attribute {
        uintptr_t descriptor{};  // the 8-byte descriptor
        uint32_t name_hash{};
        uint8_t type{};          // masked to the low 6 bits
        uint8_t raw_type{};      // unmasked, so the high 2 bits are not discarded
        uint8_t num_values{};    // descriptor+5 -- the ELEMENT COUNT; the reference's GetNumValues
        uint16_t value_index{};
        uintptr_t blob{};        // the record's value blob, needed to address elements

        bool is_bit() const { return type == kTypeBool; }
        bool is_record_link() const { return type == kTypeRecordLink || type == kTypeRecordLinkAlt; }
        bool is_struct() const { return struct_dword_count(type) != 0; }

        // The blob address of element `i`, ignoring bit packing. nullopt when i is out of range -- the same
        // bound every engine getter enforces.
        std::optional<uintptr_t> element_address(size_t i) const {
            if (i >= num_values || blob == 0) {
                return std::nullopt;
            }
            if (is_bit()) {
                return blob + 4 * ((static_cast<uintptr_t>(value_index) + i) >> 5);
            }
            return blob + 4 * (static_cast<uintptr_t>(value_index) + i);
        }

        // Which bit within that dword, for a bit attribute.
        uint8_t element_bit(size_t i) const {
            return static_cast<uint8_t>((static_cast<uintptr_t>(value_index) + i) & 0x1F);
        }
    };

    // How many attributes a record declares.
    static size_t attribute_count(const regenny::DatabaseMgrRecord* record);

    // One attribute descriptor by index. nullopt when out of range or a read faults.
    static std::optional<Attribute> attribute_at(const regenny::DatabaseMgrRecord* record, size_t index);

    // One attribute by NAME, via the same binary search the engine uses. nullopt when absent.
    //
    // NOTE THE ASYMMETRY WITH find_record: a descriptor stores only the HASH, never the name, so there is
    // nothing to string-compare and a collision cannot be rejected here the way it can one level up.
    static std::optional<Attribute> find_attribute(const regenny::DatabaseMgrRecord* record,
                                                   std::string_view name);

    // Does the record declare this attribute? The cheap question.
    static bool has_attribute(const regenny::DatabaseMgrRecord* record, std::string_view name);

    // Is the descriptor array sorted ascending by hash, as its binary search requires?
    static bool attributes_sorted_by_hash(const regenny::DatabaseMgrRecord* record);

    // ---- TYPED READERS, each refusing every type but its own ------------------------------------
    //
    // The engine's getters return a FALLBACK on a type or index mismatch, so a caller cannot tell "absent" from
    // "happens to equal the default". These return nullopt instead, which is the difference between a library a
    // consumer can trust and one that quietly lies.

    // Element `i` of a bool attribute. nullopt unless the type really is 1.
    static std::optional<bool> attribute_bool(const Attribute& attribute, size_t i = 0);

    // Element `i` of a float attribute. nullopt unless the type really is 2.
    static std::optional<float> attribute_float(const Attribute& attribute, size_t i = 0);

    // Element `i` of a record link, already fixed up by the engine to a record pointer. nullopt unless the type
    // is a link, or when the link was out of range at load time and left null.
    static regenny::DatabaseMgrRecord* attribute_record(const Attribute& attribute, size_t i = 0);

    // The dwords of a type 6/7/8 struct element -- 2, 3 or 4 of them. Empty for any other type.
    static std::vector<uint32_t> attribute_struct(const Attribute& attribute, size_t i = 0);

    // The raw dword of a type 3/4/5 attribute, whose C type is not established. Deliberately NOT called
    // attribute_int: this hands over the bits and says nothing about what they mean.
    static std::optional<uint32_t> attribute_raw_dword(const Attribute& attribute, size_t i = 0);

    // ---- NARROWING 3, 4 AND 5 BY MEASUREMENT ---------------------------------------------------
    //
    // Those three are storage-identical, and the reference's unaccounted scalar getters are GetInt32, GetString
    // and GetWString. A string attribute's dword is a POINTER to text, an int32's is a number, so the two are
    // distinguishable at runtime without guessing: sample the values of one type across the database and ask
    // what fraction dereference to readable text.
    //
    // Reported as a ratio rather than a verdict, because the answer is evidence and the naming decision is a
    // separate step that belongs in a later pass with the numbers in hand.
    struct TypeSample {
        uint8_t type{};
        size_t sampled{};
        size_t pointer_like{};  // lands in committed memory
        size_t ascii_like{};    // dereferences to printable ASCII
        size_t utf16_like{};    // dereferences to UTF-16: printable low bytes, zero high bytes
        size_t ascii_at_4{};    // a zero dword then printable ASCII -- the localized-string shape

        // The three are not exclusive by construction, so a caller compares ratios rather than trusting one.
    };

    static TypeSample sample_type(const regenny::DatabaseMgrSubRecord* database, uint8_t type,
                                  size_t limit = 400);

    // ---- RECOVERING ATTRIBUTE NAMES, WHICH DESCRIPTORS DO NOT STORE -----------------------------
    //
    // A descriptor holds a hash and nothing else, so enumerating a record hands a consumer numbers it cannot
    // display or act on. The names live in the .gamedb file, not in any module.
    //
    // BUT THE NAMES THE CODE USES ARE IN THE CODE. gameclient references attributes by string literal --
    // "WaterAffectsSpeed", "GunLead", "YawClamp" -- and those literals sit in its .rdata. Hashing every printable
    // string in the loaded module and indexing by hash recovers exactly the subset a mod is likely to want: the
    // attributes the game itself reads by name.
    //
    // This is the same trick an earlier pass used to identify model animation names by intersecting hashes with
    // strings harvested from the asset's own blob. Here the haystack is a module's data sections instead.
    //
    // A RESOLVED NAME IS A CANDIDATE, NOT A PROOF. It round-trips by construction -- hash(name) equals the hash
    // asked about -- but a DIFFERENT name with the same hash would resolve identically. Collisions measured zero
    // across 28652 record names, so this is unlikely rather than impossible, and the distinction is the caller's
    // to respect.
    struct NameIndex {
        size_t strings_scanned{};
        size_t distinct_hashes{};

        // Empty when gameclient is not mapped.
        bool ready() const { return distinct_hashes != 0; }
    };

    // Build (or rebuild) the index over gameclient's data sections. Cached: subsequent calls return the built
    // index without rescanning, since module data does not change.
    static const NameIndex& build_name_index();

    // A name whose String_HashI equals this hash, or nullopt. See the caveat above.
    static std::optional<std::string> name_for_hash(uint32_t hash);

    // The attribute's name, when it happens to be one the code mentions.
    static std::optional<std::string> attribute_name(const Attribute& attribute);

    struct NameCoverage {
        size_t distinct_attribute_hashes{};
        size_t resolved{};
        size_t records_scanned{};
    };

    // How many of the database's distinct attribute hashes the index can name. This is the honest measure of the
    // facility's reach, and it belongs in the API rather than a comment because the answer depends on the build.
    static NameCoverage name_coverage(const regenny::DatabaseMgrSubRecord* database, size_t record_limit = 0);

    // ---- ARE THE 12- AND 16-BYTE STRUCTS FLOATS? ------------------------------------------------
    //
    // Types 7 and 8 point at 12 and 16 bytes, which are the sizes of an LTVector and an LTVector4, but size is
    // not evidence about the components. The test that settles it is the one EngineVars used for its type tags: a
    // dword that is absurd as an integer and reasonable as a float IS a float.
    struct StructSample {
        uint8_t type{};
        size_t sampled{};
        size_t all_float_like{};  // every component finite and in a sane magnitude band
        size_t any_denormal{};    // at least one component denormal -- the trap this project keeps hitting
        size_t all_small_int{};   // every component would also be a plausible small integer
    };

    // Sample type 6/7/8 attributes and report how their components read. all_small_int is reported alongside
    // all_float_like on purpose: if a population satisfies BOTH, the measurement cannot discriminate and saying
    // so is the result.
    static StructSample sample_struct_type(const regenny::DatabaseMgrSubRecord* database, uint8_t type,
                                           size_t limit = 200);

private:
    char m_data[sizeof(regenny::DatabaseMgr)];
};

} // namespace sdk
