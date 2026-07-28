#include "DatabaseMgr.hpp"

#include <windows.h>

#include <utility/Seh.hpp>

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
    KANANLIB_SEH_TRY {
        mgr = fn();
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    return reinterpret_cast<DatabaseMgr*>(mgr);
}

// Own function scope, POD-only locals/return (MSVC C2712: __try cannot share
// a function with a non-trivial return type or non-trivial local objects --
// std::string in the caller would trip it). Guards the actual mapped-field
// dereference (record->path_data), proving the mapping is safe to use
// directly, not just self-consistent on paper. Returns the sanitized byte
// count written into `buf` (nul-terminated), or -1 on null/fault.
int32_t seh_read_path_into(const regenny::DatabaseMgrSubRecord* record, char* buf, size_t buf_size) {
    if (record == nullptr || buf == nullptr || buf_size == 0) {
        return -1;
    }
    int32_t n = -1;
    KANANLIB_SEH_TRY {
        const char* p = record->path_data;
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
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        n = -1;
    }
    return n;
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

// Thin, non-SEH wrapper: the actual guard lives in seh_read_path_into (own
// function scope, POD-only -- MSVC C2712 forbids __try sharing a function
// with a non-trivial return type/locals like the std::string built here).
std::string DatabaseMgr::read_path(const regenny::DatabaseMgrSubRecord* record) {
    char buf[261];
    const int32_t n = seh_read_path_into(record, buf, sizeof(buf));
    return n >= 0 ? std::string(buf, static_cast<size_t>(n)) : std::string();
}

} // namespace sdk
