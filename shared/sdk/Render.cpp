#include "Render.hpp"

#include <windows.h>

#include <utility/Seh.hpp>

#include "Modules.hpp"

namespace sdk {

namespace {

// g_D3DAdapterInfo. Found from the ONE call to Direct3DCreate9 in the whole exe:
// D3DAdapterInfo_InitD3D9 takes an IDirect3D9** and every caller passes this address.
constexpr uintptr_t kAdapterInfoOffset = 0x3304AC;

// Field offsets inside that record, both pinned by the init function's own use:
// it stores the factory at +0x00 and passes +0x04 to GetAdapterDisplayMode.
constexpr size_t kFactory = 0x00;
constexpr size_t kDisplayMode = 0x04;

uintptr_t record_address() {
    const auto* exe = Modules::get().exe();
    if (exe == nullptr || exe->base == 0) {
        return 0;
    }
    return exe->base + kAdapterInfoOffset;
}

struct PtrRead {
    uintptr_t value;
    bool ok;
};

PtrRead seh_read_ptr(uintptr_t at) {
    PtrRead r{};
    KANANLIB_SEH_TRY {
        r.value = *reinterpret_cast<const uintptr_t*>(at);
        r.ok = true;
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        return r;
    }
    return r;
}

struct ModeRead {
    uint32_t v[4];
    bool ok;
};

ModeRead seh_read_mode(uintptr_t at) {
    ModeRead r{};
    KANANLIB_SEH_TRY {
        const auto* src = reinterpret_cast<const uint32_t*>(at);
        for (size_t i = 0; i < 4; ++i) {
            r.v[i] = src[i];
        }
        r.ok = true;
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        return r;
    }
    return r;
}

}  // namespace

uintptr_t Render::adapter_info_address() {
    return record_address();
}

void* Render::d3d9() {
    const auto rec = record_address();
    if (rec == 0) {
        return nullptr;
    }
    const auto p = seh_read_ptr(rec + kFactory);
    if (!p.ok) {
        return nullptr;
    }
    return reinterpret_cast<void*>(p.value);
}

std::optional<std::string> Render::d3d9_vtable_owner() {
    auto* factory = d3d9();
    if (factory == nullptr) {
        return std::nullopt;
    }
    // The vtable pointer is the object's first word. Read it under the guard: a
    // half-initialised or already-released interface would fault here rather than
    // hand back a plausible module name.
    const auto vt = seh_read_ptr(reinterpret_cast<uintptr_t>(factory));
    if (!vt.ok || vt.value == 0) {
        return std::nullopt;
    }
    // Deliberately NOT matched against sdk::Modules: the answer we care about is a
    // module that is not in our list at all (the Steam overlay), so ask the OS which
    // module owns the address instead of testing our five known ranges.
    HMODULE owner = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(vt.value), &owner) == 0 ||
        owner == nullptr) {
        return std::nullopt;
    }
    char path[MAX_PATH]{};
    if (GetModuleFileNameA(owner, path, sizeof(path)) == 0) {
        return std::nullopt;
    }
    std::string full{path};
    const auto slash = full.find_last_of("\\/");
    return slash == std::string::npos ? full : full.substr(slash + 1);
}

std::optional<Render::DisplayMode> Render::display_mode() {
    const auto rec = record_address();
    if (rec == 0) {
        return std::nullopt;
    }
    // The mode is only meaningful once the factory exists -- the record is zeroed
    // static storage before that, and a caller reading 0x0 would take it for a real
    // answer rather than "not up yet".
    if (d3d9() == nullptr) {
        return std::nullopt;
    }
    const auto m = seh_read_mode(rec + kDisplayMode);
    if (!m.ok) {
        return std::nullopt;
    }
    DisplayMode out{};
    out.width = m.v[0];
    out.height = m.v[1];
    out.refresh_hz = m.v[2];
    out.format = m.v[3];
    return out;
}

}  // namespace sdk
