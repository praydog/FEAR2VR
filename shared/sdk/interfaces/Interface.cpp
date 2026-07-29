#include <windows.h>

#include <utility/Seh.hpp>

#include "Interface.hpp"

namespace sdk::interfaces {

// Both helpers call initialize() every time on purpose. It is a cheap no-op
// once latched, and crucially it RETRIES while unlatched: an interface looked
// up before Modules::initialize() has run (or before the pattern is
// resolvable) must be able to succeed on a later call rather than being stuck
// returning nullptr for the process lifetime.

void* resolve_interface(const char* name) {
    auto& reg = Registry::get();
    reg.initialize();
    return reg.resolve(name);
}

namespace {

// SEH cannot live in a function that unwinds objects (C2712), so each guarded read is its own plain
// function and the std::string work happens strictly outside them.
uintptr_t seh_read_vtable_slot(const void* iface, size_t slot) {
    uintptr_t out = 0;
    KANANLIB_SEH_TRY {
        const auto* vt = *reinterpret_cast<void* const* const*>(iface);
        if (vt != nullptr) {
            out = reinterpret_cast<uintptr_t>(vt[slot]);
        }
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        out = 0;
    }
    return out;
}

// A data pointer beginning with 0xB8 must not be read as a function body.
bool is_executable(uintptr_t addr) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof(mbi)) != sizeof(mbi)) {
        return false;
    }
    if (mbi.State != MEM_COMMIT) {
        return false;
    }
    // PAGE_GUARD must be rejected BEFORE masking it off. Touching a guard page raises
    // STATUS_GUARD_PAGE_VIOLATION, which the SEH below would swallow -- but the guard is CLEARED as a
    // side effect, which is exactly the kind of process mutation this whole approach exists to avoid.
    if ((mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }
    const DWORD prot = mbi.Protect & 0xFFu;
    return prot == PAGE_EXECUTE || prot == PAGE_EXECUTE_READ || prot == PAGE_EXECUTE_READWRITE ||
           prot == PAGE_EXECUTE_WRITECOPY;
}

// The 6-byte constant-return body plus a bounded copy of the string it names. One place, so the shape
// test and the dereference cannot drift apart.
bool seh_read_constant_return_string(uintptr_t fn, char* out, size_t cap, size_t* len) {
    bool ok = false;
    KANANLIB_SEH_TRY {
        const auto* p = reinterpret_cast<const uint8_t*>(fn);
        if (p[0] == 0xB8u && p[5] == 0xC3u) {
            const auto* str = *reinterpret_cast<const char* const*>(fn + 1);
            size_t i = 0;
            for (; i < cap - 1 && str[i] != '\0'; ++i) {
                out[i] = str[i];
            }
            // The NUL must be WITHIN the cap. Without this the function reports a 127-byte prefix of
            // whatever the immediate pointed at as though it were a verified constant string -- a
            // truncated arbitrary blob is not a weaker answer, it is a wrong one.
            if (i > 0 && str[i] == '\0') {
                out[i] = '\0';
                *len = i;
                ok = true;
            }
        }
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    return ok;
}

}  // namespace

std::optional<std::string> slot1_constant_string(void* iface) {
    if (iface == nullptr) {
        return std::nullopt;
    }
    const auto fn = seh_read_vtable_slot(iface, 1);
    if (fn == 0 || !is_executable(fn)) {
        return std::nullopt;
    }
    char buf[128]{};
    size_t len = 0;
    if (!seh_read_constant_return_string(fn, buf, sizeof(buf), &len)) {
        return std::nullopt;
    }
    return std::string(buf, len);
}

uintptr_t vtable_slot(void* iface, size_t slot, size_t known_slot_count) {
    if (iface == nullptr || known_slot_count == 0 || slot >= known_slot_count) {
        return 0;
    }
    const auto fn = seh_read_vtable_slot(iface, slot);
    if (fn == 0 || !is_executable(fn)) {
        return 0;
    }
    return fn;
}

Registry::Agreement interface_agreement(const char* name) {
    auto& reg = Registry::get();
    reg.initialize();
    return reg.agreement(name);
}

} // namespace sdk::interfaces
