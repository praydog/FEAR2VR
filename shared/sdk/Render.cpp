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

// LTRenderer_PresentAndSync @ FEAR2_dump.exe 0x0060DAB8. The `FF 52 44` is the Present call itself
// (call dword ptr [edx+44h] -- IDirect3DDevice9 slot 17); the trailing relative jump's displacement is
// wildcarded so the signature does not encode a link-time offset.
static constexpr const char* kEnginePresent =
    "56 8B F1 8B 06 8B 10 33 C9 51 51 51 51 50 FF 52 44 8B CE 5E E9";

// CLTRenderer_SwapBuffers @ 0x0060B48D, slot 10 of g_vtbl_CLTRenderer. The two absolute operands (the
// renderer state global and the renderer object) and the relative call are wildcarded.
static constexpr const char* kSwapBuffers =
    "83 3D ? ? ? ? 01 74 03 32 C0 C3 B9 ? ? ? ? E8 ? ? ? ? B0 01 C3";

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

uintptr_t Render::engine_present_fn() {
    static const uintptr_t s_fn = Modules::get().scan_exe(kEnginePresent, "LTRenderer_PresentAndSync");
    return s_fn;
}

uintptr_t Render::renderer_swap_buffers_fn() {
    static const uintptr_t s_fn = Modules::get().scan_exe(kSwapBuffers, "CLTRenderer_SwapBuffers");
    return s_fn;
}

uintptr_t Render::gpu_fence_wait_fn() {
    // Reached from the tail of LTRenderer_PresentAndSync rather than scanned for its own bytes: the function
    // ends in `jmp <fence>`, so decoding that displacement is exact where a second signature would be another
    // thing to keep in step with the build. E9 <rel32>, relative to the END of the instruction.
    const uintptr_t present = engine_present_fn();
    if (present == 0) {
        return 0;
    }
    const uintptr_t jmp = present + 20;  // offset of the E9 within the matched pattern
    const auto opcode = sdk::mem::read<uint8_t>(jmp);
    if (!opcode.has_value() || *opcode != 0xE9) {
        return 0;
    }
    const auto rel = sdk::mem::read<int32_t>(jmp + 1);
    if (!rel.has_value()) {
        return 0;
    }
    return jmp + 5 + static_cast<uintptr_t>(static_cast<intptr_t>(*rel));
}

std::optional<uintptr_t> Render::device_method(size_t slot) {
    const auto vt = device_vtable();
    if (vt == 0) {
        return std::nullopt;
    }
    const auto entry = sdk::mem::read<uint32_t>(vt + slot * sizeof(uint32_t));
    if (!entry.has_value() || *entry == 0) {
        return std::nullopt;
    }
    return static_cast<uintptr_t>(*entry);
}

// The slot numbers live here, once, named -- see the header for why a bare index at a call site is a defect
// rather than a style preference.
std::optional<uintptr_t> Render::present_fn() { return device_method(17); }
std::optional<uintptr_t> Render::reset_fn() { return device_method(16); }
std::optional<uintptr_t> Render::begin_scene_fn() { return device_method(41); }
std::optional<uintptr_t> Render::end_scene_fn() { return device_method(42); }

std::optional<std::string> Render::present_impl_owner() {
    return interface_impl_owner(reinterpret_cast<IUnknown*>(device()), 17);
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
