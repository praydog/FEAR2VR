#include "Render.hpp"

#include "Memory.hpp"
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
    const auto p = sdk::mem::read_ptr(rec + kFactory);
    return p.has_value() ? reinterpret_cast<IDirect3D9*>(*p) : nullptr;
}

IDirect3DDevice9* Render::device() {
    const auto rec = exe_at(kRendererOffset);
    if (rec == 0) {
        return nullptr;
    }
    const auto p = sdk::mem::read_ptr(rec + kDevice);
    return p.has_value() ? reinterpret_cast<IDirect3DDevice9*>(*p) : nullptr;
}

std::optional<D3DDISPLAYMODE> Render::display_mode() {
    const auto rec = exe_at(kAdapterInfoOffset);
    // Gate on the factory: the record is static storage, so before Direct3DCreate9 has run
    // it reads as zeros and a caller would take 0x0 for a real answer.
    if (rec == 0 || d3d9() == nullptr) {
        return std::nullopt;
    }
    D3DDISPLAYMODE out{};
    if (!sdk::mem::copy(&out, rec + kDisplayMode, sizeof(out))) {
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
    if (!sdk::mem::copy(&out, rec + kCaps, sizeof(out))) {
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
    if (!sdk::mem::copy(&out, rec + kPresentParams, sizeof(out))) {
        return std::nullopt;
    }
    return out;
}

uintptr_t Render::device_vtable() {
    auto* dev = device();
    if (dev == nullptr) {
        return 0;
    }
    const auto vt = sdk::mem::read_ptr(reinterpret_cast<uintptr_t>(dev));
    return vt.value_or(0);
}

std::optional<bool> Render::device_vtable_writable() {
    const auto vt = device_vtable();
    if (vt == 0) {
        return std::nullopt;   // no device to answer about
    }
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(vt), &mbi, sizeof(mbi)) != sizeof(mbi)) {
        return std::nullopt;   // could not find out -- distinct from "not writable"
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
    const auto vt = sdk::mem::read_ptr(reinterpret_cast<uintptr_t>(iface));
    if (!vt.has_value() || *vt == 0) {
        return std::nullopt;
    }
    const auto fn = sdk::mem::read_ptr(*vt + method_slot * sizeof(uintptr_t));
    if (!fn.has_value() || *fn == 0) {
        return std::nullopt;
    }
    return Modules::owning_module_name(*fn);
}

}  // namespace sdk
