#include "ResourceWatch.hpp"

#include <d3d9.h>

#include "../Hooks.hpp"
#include <cinttypes>

#include "Log.hpp"
#include "sdk/Render.hpp"

namespace {

// COM methods on x86 are __stdcall with `this` as the FIRST STACK ARGUMENT, so each of these is
// expressible as a plain function pointer and the trampoline can be called directly. That is why
// these are inline hooks with real signatures rather than mid hooks: nothing arrives in a register
// a C++ declaration cannot name.
//
// (This used to contrast them with "the engine's own __userpurge entries". That contrast was false:
// the engine functions in question -- Weapon_FireServer and Weapon_HandleClientFireMessage -- are
// ordinary __thiscall, and IDA's register parameters were deferred callee-saves. The COM case is
// simply the same situation arrived at from a different direction.)
using CreateTexture_t = HRESULT(__stdcall*)(void*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                            IDirect3DTexture9**, HANDLE*);
using CreateVolume_t = HRESULT(__stdcall*)(void*, UINT, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                           IDirect3DVolumeTexture9**, HANDLE*);
using CreateCube_t = HRESULT(__stdcall*)(void*, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                         IDirect3DCubeTexture9**, HANDLE*);
using CreateVB_t = HRESULT(__stdcall*)(void*, UINT, DWORD, DWORD, D3DPOOL, IDirect3DVertexBuffer9**,
                                       HANDLE*);
using CreateIB_t = HRESULT(__stdcall*)(void*, UINT, DWORD, D3DFORMAT, D3DPOOL,
                                       IDirect3DIndexBuffer9**, HANDLE*);

constexpr const char* kTexHook = "d3d9_create_texture";
constexpr const char* kVolHook = "d3d9_create_volume_texture";
constexpr const char* kCubeHook = "d3d9_create_cube_texture";
constexpr const char* kVbHook = "d3d9_create_vertex_buffer";
constexpr const char* kIbHook = "d3d9_create_index_buffer";

// The device does not exist at framework init, and a stereo path provokes resets. Retry on a slow
// cadence rather than every frame -- resolving the vtable is a guarded read per attempt, and there
// is no reason to keep paying for it once the hooks are in.
constexpr uint32_t kRetryFrames = 120;

// A retired hook is still FOUND by the registry, so every detour has to tolerate being entered
// while its trampoline is gone. Returning the failure code is wrong -- the caller would see a
// resource it asked for refused during our teardown -- so a missing trampoline is treated as
// "cannot forward", which only happens between disable() and unmap.
template <typename Fn>
Fn original_of(const char* name) {
    auto* hook = Hooks::get().find(name);
    if (hook == nullptr || !*hook) {
        return nullptr;
    }
    return hook->original<Fn>();
}

HRESULT __stdcall create_texture(void* self, UINT w, UINT h, UINT levels, DWORD usage,
                                 D3DFORMAT fmt, D3DPOOL pool, IDirect3DTexture9** out,
                                 HANDLE* shared) {
    // Record BEFORE forwarding. The original can fail, and a REFUSED managed allocation is still
    // the engine having asked for one -- which is exactly what the D3D9Ex question is about.
    ResourceWatch::get().note_create(ResourceWatch::Kind::Texture, static_cast<uint32_t>(pool));
    ResourceWatch::get().note_texture(static_cast<uint32_t>(pool), usage, static_cast<uint32_t>(fmt),
                                      w > h ? w : h);
    auto fn = original_of<CreateTexture_t>(kTexHook);
    return fn != nullptr ? fn(self, w, h, levels, usage, fmt, pool, out, shared) : D3DERR_INVALIDCALL;
}

HRESULT __stdcall create_volume_texture(void* self, UINT w, UINT h, UINT d, UINT levels, DWORD usage,
                                        D3DFORMAT fmt, D3DPOOL pool, IDirect3DVolumeTexture9** out,
                                        HANDLE* shared) {
    ResourceWatch::get().note_create(ResourceWatch::Kind::VolumeTexture, static_cast<uint32_t>(pool));
    auto fn = original_of<CreateVolume_t>(kVolHook);
    return fn != nullptr ? fn(self, w, h, d, levels, usage, fmt, pool, out, shared) : D3DERR_INVALIDCALL;
}

HRESULT __stdcall create_cube_texture(void* self, UINT edge, UINT levels, DWORD usage, D3DFORMAT fmt,
                                      D3DPOOL pool, IDirect3DCubeTexture9** out, HANDLE* shared) {
    ResourceWatch::get().note_create(ResourceWatch::Kind::CubeTexture, static_cast<uint32_t>(pool));
    auto fn = original_of<CreateCube_t>(kCubeHook);
    return fn != nullptr ? fn(self, edge, levels, usage, fmt, pool, out, shared) : D3DERR_INVALIDCALL;
}

HRESULT __stdcall create_vertex_buffer(void* self, UINT len, DWORD usage, DWORD fvf, D3DPOOL pool,
                                       IDirect3DVertexBuffer9** out, HANDLE* shared) {
    ResourceWatch::get().note_create(ResourceWatch::Kind::VertexBuffer, static_cast<uint32_t>(pool));
    auto fn = original_of<CreateVB_t>(kVbHook);
    return fn != nullptr ? fn(self, len, usage, fvf, pool, out, shared) : D3DERR_INVALIDCALL;
}

HRESULT __stdcall create_index_buffer(void* self, UINT len, DWORD usage, D3DFORMAT fmt, D3DPOOL pool,
                                      IDirect3DIndexBuffer9** out, HANDLE* shared) {
    ResourceWatch::get().note_create(ResourceWatch::Kind::IndexBuffer, static_cast<uint32_t>(pool));
    auto fn = original_of<CreateIB_t>(kIbHook);
    return fn != nullptr ? fn(self, len, usage, fmt, pool, out, shared) : D3DERR_INVALIDCALL;
}

} // namespace

ResourceWatch& ResourceWatch::get() {
    static ResourceWatch instance;
    return instance;
}

std::optional<std::string> ResourceWatch::on_initialize() {
    // No device at init. on_frame installs once one exists.
    return std::nullopt;
}

void ResourceWatch::note_create(Kind kind, uint32_t pool) {
    const auto k = static_cast<size_t>(kind);
    if (k >= static_cast<size_t>(Kind::kCount)) {
        return;
    }
    // Anything outside D3DPOOL's 0..3 lands in the last bucket. That would mean the argument was
    // misread rather than that D3D grew a pool, and it must be visible rather than folded into a
    // valid one.
    const size_t p = pool < kPools - 1 ? pool : kPools - 1;
    m_counts[k][p].fetch_add(1, std::memory_order_relaxed);
    m_total.fetch_add(1, std::memory_order_relaxed);
}

void ResourceWatch::note_texture(uint32_t pool, uint32_t usage, uint32_t format, uint32_t edge) {
    if (pool != 1) {  // only the managed population is in question
        return;
    }
    // D3DUSAGE_DYNAMIC (0x200) decides whether this could live in DEFAULT at all.
    if ((usage & 0x200u) != 0) {
        m_managed_dynamic.fetch_add(1, std::memory_order_relaxed);
    } else {
        m_managed_static.fetch_add(1, std::memory_order_relaxed);
    }
    if ((usage & 1u) != 0) {  // D3DUSAGE_RENDERTARGET
        m_managed_rt.fetch_add(1, std::memory_order_relaxed);
    }
    uint32_t prev = m_largest_edge.load(std::memory_order_relaxed);
    while (edge > prev && !m_largest_edge.compare_exchange_weak(prev, edge, std::memory_order_relaxed)) {
    }
    if (format < kFormatSlots) {
        m_formats[format].fetch_add(1, std::memory_order_relaxed);
    } else {
        m_format_overflow.fetch_add(1, std::memory_order_relaxed);
    }
}

uint32_t ResourceWatch::distinct_formats() const {
    uint32_t n = 0;
    for (size_t i = 0; i < kFormatSlots; ++i) {
        if (m_formats[i].load(std::memory_order_relaxed) != 0) {
            ++n;
        }
    }
    return n + (m_format_overflow.load(std::memory_order_relaxed) != 0 ? 1 : 0);
}

uint64_t ResourceWatch::count(Kind kind, size_t pool) const {
    const auto k = static_cast<size_t>(kind);
    if (k >= static_cast<size_t>(Kind::kCount) || pool >= kPools) {
        return 0;
    }
    return m_counts[k][pool].load(std::memory_order_relaxed);
}

uint64_t ResourceWatch::pool_total(size_t pool) const {
    if (pool >= kPools) {
        return 0;
    }
    uint64_t n = 0;
    for (size_t k = 0; k < static_cast<size_t>(Kind::kCount); ++k) {
        n += m_counts[k][pool].load(std::memory_order_relaxed);
    }
    return n;
}

void ResourceWatch::reset_counts() {
    for (size_t k = 0; k < static_cast<size_t>(Kind::kCount); ++k) {
        for (size_t p = 0; p < kPools; ++p) {
            m_counts[k][p].store(0, std::memory_order_relaxed);
        }
    }
    m_total.store(0, std::memory_order_relaxed);
    m_managed_dynamic.store(0, std::memory_order_relaxed);
    m_managed_static.store(0, std::memory_order_relaxed);
    m_managed_rt.store(0, std::memory_order_relaxed);
    m_largest_edge.store(0, std::memory_order_relaxed);
    m_format_overflow.store(0, std::memory_order_relaxed);
    for (size_t i = 0; i < kFormatSlots; ++i) {
        m_formats[i].store(0, std::memory_order_relaxed);
    }
}

bool ResourceWatch::install() {
    const auto tex = sdk::Render::create_texture_fn();
    const auto vol = sdk::Render::create_volume_texture_fn();
    const auto cube = sdk::Render::create_cube_texture_fn();
    const auto vb = sdk::Render::create_vertex_buffer_fn();
    const auto ib = sdk::Render::create_index_buffer_fn();

    // ALL OR NOTHING. A partial install answers the pool question with a population that silently
    // excludes whichever kind failed, and "no managed textures" would then be indistinguishable
    // from "textures were never watched".
    if (!tex.has_value() || !vol.has_value() || !cube.has_value() || !vb.has_value() ||
        !ib.has_value()) {
        return false;
    }

    const bool ok =
        Hooks::get().install(kTexHook, reinterpret_cast<void*>(*tex), &create_texture) &&
        Hooks::get().install(kVolHook, reinterpret_cast<void*>(*vol), &create_volume_texture) &&
        Hooks::get().install(kCubeHook, reinterpret_cast<void*>(*cube), &create_cube_texture) &&
        Hooks::get().install(kVbHook, reinterpret_cast<void*>(*vb), &create_vertex_buffer) &&
        Hooks::get().install(kIbHook, reinterpret_cast<void*>(*ib), &create_index_buffer);

    if (ok) {
        LOGX("[resources] watching d3d9 resource creation (tex 0x%08" PRIXPTR ", vb 0x%08" PRIXPTR ")",
             *tex, *vb);
    }
    return ok;
}

void ResourceWatch::on_frame() {
    if (m_hooked.load(std::memory_order_relaxed)) {
        return;
    }
    auto remaining = m_retry.load(std::memory_order_relaxed);
    if (remaining > 0) {
        m_retry.store(remaining - 1, std::memory_order_relaxed);
        return;
    }
    m_retry.store(kRetryFrames, std::memory_order_relaxed);
    m_hooked.store(install(), std::memory_order_relaxed);
}

void ResourceWatch::on_shutdown() {
    // Nothing to unwind. These are safetyhook inline hooks in the registry, which retires them,
    // and unlike BoneControl's node cell the engine holds no pointer of ours afterwards.
}
