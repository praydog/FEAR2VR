#pragma once

// NOT generated -- hand-written, permanent. sdk:generate() emits C++ for
// fear2.genny's STRUCT types but does not define the .genny prelude's tagged
// PRIMITIVE aliases (strptr/wstrptr) as real C++ types; generated fields typed
// `strptr`/`wstrptr` (e.g. DatabaseMgrSubRecord::string_data/path_data)
// otherwise fail to compile. Include this BEFORE any generated regenny header
// that might reference them.
//
// Matches fear2.genny's own declarations exactly:
//   type strptr  4 [[utf8*]]   -- char* to a (possibly non-single) C string
//   type wstrptr 4 [[utf16*]]  -- wchar_t* equivalent
// ([[code]]-tagged fields generate as plain `void*` already, no shim needed.)
using strptr = char*;
using wstrptr = wchar_t*;
