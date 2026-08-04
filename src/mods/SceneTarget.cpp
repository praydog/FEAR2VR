#include "SceneTarget.hpp"

#include <windows.h>

#include <d3d9.h>

#include "Hooks.hpp"
#include "Log.hpp"
#include <cstring>

#include "sdk/Modules.hpp"
#include "sdk/Render.hpp"

namespace {

// SceneRenderer_BeginRenderTarget. Every scene render-target bind goes through it: the scene draw
// is gated on a state machine (g_SceneRenderer == 2 here, 3 after, 4 once a pass is set up), so a
// 3D pass cannot run without having passed this point.
constexpr uintptr_t kBeginRenderTarget = 0x210727;
constexpr const char* kHookName = "SceneRenderer_BeginRenderTarget";

// RTHandle_Create -- the single funnel every render target in the process passes through.
// __thiscall(handle, width, height, flags), width and height used verbatim.
constexpr uintptr_t kRTHandleCreate = 0x213549;
constexpr const char* kCreateHookName = "RTHandle_Create";

// RTHandle_Unbind. On the NON-virtual path it restores the back buffer as the render target and
// depth surface, and the HUD is drawn after that -- which makes the moment just after it the one
// correct place to put the scene on screen. Earlier and the engine would overwrite it; later and we
// would paint over the HUD.
constexpr uintptr_t kRTHandleUnbind = 0x2130D9;
constexpr const char* kUnbindHookName = "RTHandle_Unbind";

// The handle's colour texture, by field read rather than by call. RTHandle_GetColourTexture0
// (0x612E60) is exactly `[[this+0x10]+0x14]`, and reading it directly avoids the engine's
// GetRenderSurface wrapper -- which takes an out-parameter on the STACK and returns a bool, not the
// surface. Calling that one as though it returned the surface is what dereferenced the flags word
// as an object and crashed the game.
constexpr size_t kHandleColourTarget = 0x10;
constexpr size_t kColourTargetTexture = 0x14;

// g_pBackBufferColourSurface, cached by Renderer_CacheBackBufferSurfaces at device creation.
constexpr uintptr_t kBackBufferSurface = 0x32EC4C;

// Set on the main view. Its meaning is what makes supersampling impossible while it is on -- see
// the header. Also masks the low three flag bits when present, which is why removing it is exact.
constexpr uint32_t kFlagVirtual = 0x800u;
constexpr uint32_t kFlagsMaskedByVirtual = 0x7u;

// Inside the render-target handle (0x1C bytes, RTHandle_Create 0x613549).
constexpr size_t kHandleFlags = 0x00;
constexpr size_t kHandlePooled = 0x18;  // -> the pooled CRenderTarget shared by same-geometry targets
// Inside the pooled CRenderTarget (0x24 bytes, RT_Ctor 0x612CAA). Both u16 -- the engine truncates
// to __int16 here, which is the only hard dimension limit in the system.
constexpr size_t kPooledWidth = 0x04;
constexpr size_t kPooledHeight = 0x06;

struct SeenSlot {
    std::atomic<uint32_t> flags{0};
    std::atomic<uint32_t> width{0};
    std::atomic<uint32_t> height{0};
    std::atomic<uint64_t> binds{0};
    std::atomic<bool> used{false};
};

std::array<SeenSlot, SceneTarget::kMaxSeen> g_seen;
std::atomic<uint64_t> g_binds{0};
std::atomic<uint32_t> g_scale_bits{0};   // float bits; 0.0f means "leave the engine alone"
std::atomic<uint32_t> g_rec_w{0};
std::atomic<uint32_t> g_rec_h{0};
std::atomic<uint64_t> g_overrides{0};
std::atomic<uintptr_t> g_main_handle{0};  // the one handle we enlarged, so only it is composited
std::atomic<uint64_t> g_composites{0};
std::atomic<uint64_t> g_composite_failures{0};
std::atomic<uint32_t> g_forced_w{0};   // the size the main view is ACTUALLY rendering at
std::atomic<uint32_t> g_forced_h{0};

float load_scale() {
    const uint32_t bits = g_scale_bits.load(std::memory_order_relaxed);
    float f = 0.0f;
    static_assert(sizeof(f) == sizeof(bits));
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

// __thiscall(handle, a2, a3).
char __fastcall rt_handle_unbind_detour(void* self, void* /*edx*/, void* a2, char a3) {
    auto* hook = Hooks::get().find(kUnbindHookName);
    if (hook == nullptr) {
        return 0;
    }
    // ORIGINAL FIRST. On the non-virtual path it is what re-binds the back buffer, and the blit
    // below needs that to have already happened.
    const char r =
        hook->original<char(__fastcall*)(void*, void*, void*, char)>()(self, nullptr, a2, a3);

    if (reinterpret_cast<uintptr_t>(self) == g_main_handle.load(std::memory_order_acquire)) {
        // ---- THE MIRROR -----------------------------------------------------------------------
        //
        // Taking the scene off the 0x800 path means the back buffer no longer receives it -- the
        // window went black the first time, because in the virtual design the back buffer WAS the
        // destination. The desktop copy has to be produced deliberately now, and StretchRect is
        // what turns the supersampled target back into a window-sized picture.
        //
        // LINEAR, because this is a downscale by construction and a point filter would alias the
        // mirror badly. The VR path does not go through here: it reads the full-resolution surface.
        auto* dev = sdk::Render::device();
        const auto* exe = sdk::Modules::get().exe();
        if (dev != nullptr && exe != nullptr && exe->base != 0) {
            const auto h = reinterpret_cast<uintptr_t>(self);
            const auto target = *reinterpret_cast<uintptr_t*>(h + kHandleColourTarget);
            auto* const tex =
                target != 0
                    ? *reinterpret_cast<IDirect3DTexture9**>(target + kColourTargetTexture)
                    : nullptr;
            auto* const back =
                *reinterpret_cast<IDirect3DSurface9**>(exe->base + kBackBufferSurface);

            IDirect3DSurface9* src = nullptr;
            if (tex != nullptr && back != nullptr && SUCCEEDED(tex->GetSurfaceLevel(0, &src))) {
                // GetSurfaceLevel adds a reference. Releasing it is not optional at this call
                // rate -- a missed Release here is a leaked surface every single frame.
                if (SUCCEEDED(dev->StretchRect(src, nullptr, back, nullptr, D3DTEXF_LINEAR))) {
                    g_composites.fetch_add(1, std::memory_order_relaxed);
                } else {
                    g_composite_failures.fetch_add(1, std::memory_order_relaxed);
                }
                src->Release();
            } else {
                g_composite_failures.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    return r;
}

// __thiscall(handle, width, height, flags).
char __fastcall rt_handle_create_detour(void* self, void* /*edx*/, int width, int height,
                                        int flags) {
    // Every creation, bounded. Which targets exist and WHEN they are made is the thing that
    // decides whether an override can ever apply -- a target built before the mod arms is a target
    // the mod cannot change without forcing it to be rebuilt.
    static std::atomic<uint32_t> s_creates{0};
    const uint32_t n = s_creates.fetch_add(1, std::memory_order_relaxed);
    if (n < 24) {
        LOGX("[scenetarget] create #%u %dx%d flags 0x%X", n, width, height, flags);
    }

    const float scale = load_scale();
    const uint32_t rw = g_rec_w.load(std::memory_order_relaxed);
    const uint32_t rh = g_rec_h.load(std::memory_order_relaxed);

    // ONLY THE MAIN VIEW. Other virtual targets exist -- a 1024x576 one was observed in the same
    // frame -- and enlarging those would cost fill rate for pixels nobody sees. The main view is
    // identified by being the virtual target whose size IS the back buffer, which is precisely what
    // the 0x800 path means.
    if (scale > 0.0f && rw != 0 && rh != 0 && (static_cast<uint32_t>(flags) & kFlagVirtual) != 0) {
        const auto pp = sdk::Render::present_params();
        if (pp.has_value() && static_cast<uint32_t>(width) == pp->BackBufferWidth &&
            static_cast<uint32_t>(height) == pp->BackBufferHeight) {
            // Both eyes share one target, side by side, so the width is doubled and the height is
            // not. Rounded to even so the split lands on a whole pixel column.
            const auto w = static_cast<int>((static_cast<float>(rw) * scale) * 2.0f) & ~1;
            const auto hgt = static_cast<int>(static_cast<float>(rh) * scale) & ~1;
            const int f = static_cast<int>(static_cast<uint32_t>(flags) &
                                           ~(kFlagVirtual | kFlagsMaskedByVirtual));
            LOGX("[scenetarget] main view %dx%d flags 0x%X -> %dx%d flags 0x%X (offscreen)", width,
                 height, flags, w, hgt, f);
            width = w;
            height = hgt;
            flags = f;
            g_overrides.fetch_add(1, std::memory_order_relaxed);
            // `self` IS the handle being constructed. Remembering it is what lets the unbind hook
            // composite exactly this target and leave every other offscreen target alone.
            g_main_handle.store(reinterpret_cast<uintptr_t>(self), std::memory_order_release);
            g_forced_w.store(static_cast<uint32_t>(w), std::memory_order_release);
            g_forced_h.store(static_cast<uint32_t>(hgt), std::memory_order_release);
        }
    }

    auto* hook = Hooks::get().find(kCreateHookName);
    if (hook == nullptr) {
        return 0;
    }
    return hook->original<char(__fastcall*)(void*, void*, int, int, int)>()(self, nullptr, width,
                                                                           height, flags);
}
std::atomic<uint32_t> g_distinct{0};

void note(uint32_t flags, uint32_t w, uint32_t h) {
    for (auto& s : g_seen) {
        if (s.used.load(std::memory_order_acquire)) {
            if (s.flags.load(std::memory_order_relaxed) == flags &&
                s.width.load(std::memory_order_relaxed) == w &&
                s.height.load(std::memory_order_relaxed) == h) {
                s.binds.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            continue;
        }
        // First writer wins the slot. A race here would at worst duplicate one triple in the
        // report, which is not worth a lock on a per-pass path.
        bool expected = false;
        if (s.used.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            s.flags.store(flags, std::memory_order_relaxed);
            s.width.store(w, std::memory_order_relaxed);
            s.height.store(h, std::memory_order_relaxed);
            s.binds.store(1, std::memory_order_relaxed);
            g_distinct.fetch_add(1, std::memory_order_relaxed);
            LOGX("[scenetarget] %ux%u flags 0x%X (%s)", w, h, flags,
                 (flags & 0x800u) != 0 ? "VIRTUAL -- draws into the back buffer, size clamped to it"
                                       : "real offscreen surface -- size is free");
            return;
        }
    }
}

// __thiscall(this, handle, a3, a4, a5) -- `this` in ECX, four stack arguments.
char __fastcall begin_render_target_detour(void* self, void* /*edx*/, void* handle, int a3, void* a4,
                                           int a5) {
    if (handle != nullptr) {
        const auto h = reinterpret_cast<uintptr_t>(handle);
        const uint32_t flags = *reinterpret_cast<const uint32_t*>(h + kHandleFlags);
        const auto pooled = *reinterpret_cast<const uintptr_t*>(h + kHandlePooled);
        if (pooled != 0) {
            note(flags, *reinterpret_cast<const uint16_t*>(pooled + kPooledWidth),
                 *reinterpret_cast<const uint16_t*>(pooled + kPooledHeight));
            g_binds.fetch_add(1, std::memory_order_relaxed);
        }
    }

    auto* hook = Hooks::get().find(kHookName);
    if (hook == nullptr) {
        return 0;
    }
    return hook->original<char(__fastcall*)(void*, void*, void*, int, void*, int)>()(
        self, nullptr, handle, a3, a4, a5);
}

}  // namespace

namespace {

// The settings file, beside the DLL. Deliberately tiny and hand-editable: it is read before the
// renderer exists, so nothing more capable is available to read it with.
void load_settings() {
    char path[MAX_PATH]{};
    HMODULE self = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&load_settings), &self);
    if (GetModuleFileNameA(self, path, MAX_PATH) == 0) {
        return;
    }
    char* const slash = strrchr(path, '\\');
    if (slash == nullptr) {
        return;
    }
    strcpy_s(slash + 1, MAX_PATH - (slash + 1 - path), "fear2vr.ini");

    const uint32_t w = GetPrivateProfileIntA("render", "per_eye_width", 0, path);
    const uint32_t h = GetPrivateProfileIntA("render", "per_eye_height", 0, path);
    // Permille rather than a float: GetPrivateProfileInt cannot read one, and a second parser for a
    // single number is not worth writing.
    const uint32_t permille = GetPrivateProfileIntA("render", "supersample_permille", 0, path);
    if (w == 0 || h == 0 || permille == 0) {
        LOGX("[scenetarget] no supersample settings in %s -- drawing at the back buffer", path);
        return;
    }
    g_rec_w.store(w, std::memory_order_relaxed);
    g_rec_h.store(h, std::memory_order_relaxed);
    const float scale = static_cast<float>(permille) / 1000.0f;
    uint32_t bits = 0;
    std::memcpy(&bits, &scale, sizeof(bits));
    g_scale_bits.store(bits, std::memory_order_relaxed);
    LOGX("[scenetarget] settings: %ux%u per eye at %.3fx", w, h, scale);
}

}  // namespace

SceneTarget& SceneTarget::get() {
    static SceneTarget s_instance;
    return s_instance;
}

std::optional<std::string> SceneTarget::on_initialize() {
    const auto* exe = sdk::Modules::get().exe();
    if (exe == nullptr || exe->base == 0) {
        return "exe module unresolved";
    }
    if (!Hooks::get().install(kHookName, reinterpret_cast<void*>(exe->base + kBeginRenderTarget),
                              reinterpret_cast<void*>(&begin_render_target_detour))) {
        return "could not hook SceneRenderer_BeginRenderTarget";
    }
    if (!Hooks::get().install(kCreateHookName, reinterpret_cast<void*>(exe->base + kRTHandleCreate),
                              reinterpret_cast<void*>(&rt_handle_create_detour))) {
        return "could not hook RTHandle_Create";
    }
    if (!Hooks::get().install(kUnbindHookName, reinterpret_cast<void*>(exe->base + kRTHandleUnbind),
                              reinterpret_cast<void*>(&rt_handle_unbind_detour))) {
        return "could not hook RTHandle_Unbind";
    }

    // ---- ARMED AT INIT, BECAUSE THE MAIN VIEW IS BUILT ONCE ------------------------------------
    //
    // Measured: the main view's target is created during renderer startup, seconds after injection
    // and long before a world is loaded, and it is NEVER rebuilt for the rest of the session. An
    // override installed later has nothing left to change -- which is exactly what happened on the
    // first attempt, silently.
    //
    // So the size has to be known before the engine asks. It comes from the settings file rather
    // than from the host, because the host may not be running yet and this decision cannot wait.
    load_settings();
    return std::nullopt;
}

SceneTarget::Report SceneTarget::report() const {
    Report r{};
    r.hooked = Hooks::get().find(kHookName) != nullptr;
    r.binds = g_binds.load(std::memory_order_relaxed);
    r.distinct = g_distinct.load(std::memory_order_relaxed);
    for (size_t i = 0; i < kMaxSeen; ++i) {
        if (!g_seen[i].used.load(std::memory_order_acquire)) {
            continue;
        }
        r.seen[i].flags = g_seen[i].flags.load(std::memory_order_relaxed);
        r.seen[i].width = g_seen[i].width.load(std::memory_order_relaxed);
        r.seen[i].height = g_seen[i].height.load(std::memory_order_relaxed);
        r.seen[i].binds = g_seen[i].binds.load(std::memory_order_relaxed);
    }
    return r;
}

void SceneTarget::set_scale(float scale) {
    uint32_t bits = 0;
    std::memcpy(&bits, &scale, sizeof(bits));
    g_scale_bits.store(bits, std::memory_order_relaxed);
}

void SceneTarget::set_recommended(uint32_t per_eye_w, uint32_t per_eye_h) {
    g_rec_w.store(per_eye_w, std::memory_order_relaxed);
    g_rec_h.store(per_eye_h, std::memory_order_relaxed);
}

float SceneTarget::scale() const { return load_scale(); }
uint32_t SceneTarget::target_w() const { return g_rec_w.load(std::memory_order_relaxed); }
uint32_t SceneTarget::target_h() const { return g_rec_h.load(std::memory_order_relaxed); }
uint64_t SceneTarget::overrides() const { return g_overrides.load(std::memory_order_relaxed); }

uint64_t SceneTarget::composites() const { return g_composites.load(std::memory_order_relaxed); }
uint64_t SceneTarget::composite_failures() const {
    return g_composite_failures.load(std::memory_order_relaxed);
}

bool SceneTarget::main_view_size(int32_t& w, int32_t& h) {
    const uint32_t fw = g_forced_w.load(std::memory_order_acquire);
    const uint32_t fh = g_forced_h.load(std::memory_order_acquire);
    if (fw == 0 || fh == 0) {
        return false;
    }
    w = static_cast<int32_t>(fw);
    h = static_cast<int32_t>(fh);
    return true;
}
