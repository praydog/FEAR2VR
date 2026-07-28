#pragma once

#include <cstddef>
#include <cstdint> // generated regenny headers use uint32_t/etc. without including it themselves -- must precede them
#include <string>

// Primitives shim MUST precede any generated header using strptr/wstrptr
// (fear2.genny prelude aliases with no C++ definition of their own).
#include "regenny/Primitives.hpp"
#include "regenny/regenny/DatabaseMgr.hpp"
#include "regenny/regenny/DatabaseMgrEntry.hpp"
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

private:
    char m_data[sizeof(regenny::DatabaseMgr)];
};

} // namespace sdk
