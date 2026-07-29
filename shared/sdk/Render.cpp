#include "Render.hpp"

#include <utility/Seh.hpp>

#include "Modules.hpp"

namespace sdk {

namespace {

// g_D3DAdapterInfo. Found from the ONE Direct3DCreate9 call in the whole exe:
// D3DAdapterInfo_InitD3D9 takes an IDirect3D9** and every caller passes this address.
constexpr uintptr_t kAdapterInfoOffset = 0x3304AC;
constexpr size_t kFactory = 0x00;      // where Direct3DCreate9's result is stored
constexpr size_t kDisplayMode = 0x04;  // passed to GetAdapterDisplayMode

// g_Renderer. Found from the only IDirect3D9::CreateDevice call in the exe
// (Renderer_CreateDevice, 0x60E013): it takes the renderer as `this`.
constexpr uintptr_t kRendererOffset = 0x32E118;
constexpr size_t kDevice = 0x00;         // where CreateDevice's out-param is stored
constexpr size_t kCaps = 0x0C;           // passed to GetDeviceCaps
constexpr size_t kPresentParams = 0x158; // passed to CreateDevice

uintptr_t exe_at(uintptr_t offset) {
    const auto* exe = Modules::get().exe();
    if (exe == nullptr || exe->base == 0) {
        return 0;
    }
    return exe->base + offset;
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

// Byte-wise copy so the guard never shares a frame with a type that unwinds. The D3D
// structs are POD, so a raw copy is exactly right for them.
bool seh_copy(uintptr_t from, void* to, size_t bytes) {
    bool ok = false;
    KANANLIB_SEH_TRY {
        const auto* src = reinterpret_cast<const unsigned char*>(from);
        auto* dst = static_cast<unsigned char*>(to);
        for (size_t i = 0; i < bytes; ++i) {
            dst[i] = src[i];
        }
        ok = true;
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return ok;
}

std::optional<std::string> module_owning(uintptr_t address) {
    // Deliberately NOT matched against sdk::Modules: the answers that matter are modules
    // we do not track (the Steam overlay, d3d9.dll), so ask the OS which one owns the
    // address rather than testing our five known ranges.
    HMODULE owner = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(address), &owner) == 0 ||
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

}  // namespace

uintptr_t Render::adapter_info_address() {
    return exe_at(kAdapterInfoOffset);
}

uintptr_t Render::renderer_address() {
    return exe_at(kRendererOffset);
}

IDirect3D9* Render::d3d9() {
    const auto rec = exe_at(kAdapterInfoOffset);
    if (rec == 0) {
        return nullptr;
    }
    const auto p = seh_read_ptr(rec + kFactory);
    return p.ok ? reinterpret_cast<IDirect3D9*>(p.value) : nullptr;
}

IDirect3DDevice9* Render::device() {
    const auto rec = exe_at(kRendererOffset);
    if (rec == 0) {
        return nullptr;
    }
    const auto p = seh_read_ptr(rec + kDevice);
    return p.ok ? reinterpret_cast<IDirect3DDevice9*>(p.value) : nullptr;
}

std::optional<D3DDISPLAYMODE> Render::display_mode() {
    const auto rec = exe_at(kAdapterInfoOffset);
    // Gate on the factory: the record is static storage, so before Direct3DCreate9 has run
    // it reads as zeros and a caller would take 0x0 for a real answer.
    if (rec == 0 || d3d9() == nullptr) {
        return std::nullopt;
    }
    D3DDISPLAYMODE out{};
    if (!seh_copy(rec + kDisplayMode, &out, sizeof(out))) {
        return std::nullopt;
    }
    return out;
}

std::optional<D3DCAPS9> Render::device_caps() {
    const auto rec = exe_at(kRendererOffset);
    if (rec == 0 || device() == nullptr) {
        return std::nullopt;
    }
    D3DCAPS9 out{};
    if (!seh_copy(rec + kCaps, &out, sizeof(out))) {
        return std::nullopt;
    }
    return out;
}

std::optional<D3DPRESENT_PARAMETERS> Render::present_params() {
    const auto rec = exe_at(kRendererOffset);
    if (rec == 0 || device() == nullptr) {
        return std::nullopt;
    }
    D3DPRESENT_PARAMETERS out{};
    if (!seh_copy(rec + kPresentParams, &out, sizeof(out))) {
        return std::nullopt;
    }
    return out;
}

uintptr_t Render::device_vtable() {
    auto* dev = device();
    if (dev == nullptr) {
        return 0;
    }
    const auto vt = seh_read_ptr(reinterpret_cast<uintptr_t>(dev));
    return vt.ok ? vt.value : 0;
}

bool Render::device_vtable_writable() {
    const auto vt = device_vtable();
    if (vt == 0) {
        return false;
    }
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(vt), &mbi, sizeof(mbi)) != sizeof(mbi)) {
        return false;
    }
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }
    const DWORD p = mbi.Protect & 0xFFu;
    return p == PAGE_READWRITE || p == PAGE_WRITECOPY || p == PAGE_EXECUTE_READWRITE ||
           p == PAGE_EXECUTE_WRITECOPY;
}

std::optional<std::string> Render::interface_impl_owner(IUnknown* iface, size_t method_slot) {
    if (iface == nullptr) {
        return std::nullopt;
    }
    // Two guarded reads: the vtable pointer, then the slot. A released or half-built
    // interface faults here rather than yielding a plausible module name.
    const auto vt = seh_read_ptr(reinterpret_cast<uintptr_t>(iface));
    if (!vt.ok || vt.value == 0) {
        return std::nullopt;
    }
    const auto fn = seh_read_ptr(vt.value + method_slot * sizeof(uintptr_t));
    if (!fn.ok || fn.value == 0) {
        return std::nullopt;
    }
    return module_owning(fn.value);
}

}  // namespace sdk
