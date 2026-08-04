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

constexpr uintptr_t kPresentParams = 0x20DBD4;

// The engine's OWN belief about how big the screen is. The back buffer alone is not enough: with
// only the buffer enlarged, the engine kept laying out a 2560x1440 view in the corner of a
// 4320x2224 surface and the rest stayed black. The main view's render target, the viewport and the
// HUD all size themselves from here, never from D3DPRESENT_PARAMETERS.
//
// r_InitRender is where it is decided. It hands the requested mode to the renderer, gets back a
// record of the mode ACTUALLY used, and from that record's +132/+136 writes the ScreenWidth and
// ScreenHeight console variables and copies the whole thing into g_RMode. Correcting it on the way
// out of that function is the one point where every consumer is still downstream -- and it is why
// reading g_RMode during device creation showed a stale 1024x768: it had not been written yet.
constexpr uintptr_t kInitRender = 0x069595;
constexpr const char* kInitRenderHookName = "r_InitRender";
constexpr uintptr_t kRMode = 0x2F7540;
constexpr size_t kRModeWidth = 132;
constexpr size_t kRModeHeight = 136;
constexpr uintptr_t kConVarSetFloat = 0x010CF6;
constexpr uintptr_t kConVarTable = 0x2ED4A0;
constexpr const char* kPresentParamsHookName = "Renderer_SetPresentationParams";


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

float load_scale() {
    const uint32_t bits = g_scale_bits.load(std::memory_order_relaxed);
    float f = 0.0f;
    static_assert(sizeof(f) == sizeof(bits));
    std::memcpy(&f, &bits, sizeof(f));
    return f;
}

// Renderer_SetPresentationParams(renderer, width, height, windowed). THE one place the back buffer
// size is decided -- it writes BackBufferWidth/Height straight into the D3DPRESENT_PARAMETERS the
// engine hands to CreateDevice.
//
// A BIGGER BACK BUFFER IS NOT A BIGGER WINDOW. Windowed presentation here uses D3DSWAPEFFECT_COPY
// (params[92] = 3, confirmed live), and COPY permits a back buffer larger than the client area:
// Present stretches it down. So the desktop keeps showing 2560x1440 while everything the engine
// draws -- scene, post chain, HUD, all of it -- happens at the larger size, with every internal
// invariant untouched. That is the opposite of the offscreen-target attempt, which kept the engine
// small and tried to enlarge one stage inside it.
//
// Applied at DEVICE CREATION, never through SetRenderMode. The earlier attempt drove a mode change
// after startup, and the override was still armed while the engine tried to restore its previous
// mode -- so the restore was sabotaged too and the renderer came down with
// LT_UNABLETORESTOREVIDEO. Injection happens before the renderer builds its device, so there is
// nothing to change: the first device can simply be the right size.
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
        bool expected = false;
        if (s.used.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
            s.flags.store(flags, std::memory_order_relaxed);
            s.width.store(w, std::memory_order_relaxed);
            s.height.store(h, std::memory_order_relaxed);
            s.binds.store(1, std::memory_order_relaxed);
            g_distinct.fetch_add(1, std::memory_order_relaxed);
            LOGX("[scenetarget] binds %ux%u flags 0x%X (%s)", w, h, flags,
                 (flags & 0x800u) != 0 ? "virtual -- draws into the back buffer" : "offscreen");
            return;
        }
    }
}

// __thiscall(this, handle, a3, a4, a5). Read-only: this is how we see what the engine actually
// binds, which is the only reason the 0x800 behaviour was ever found.
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

// __cdecl(const void* requested_mode) -> 0 on success. Everything that asks the engine how big the
// screen is reads what this function publishes.
int __cdecl init_render_detour(const void* mode) {
    auto* hook = Hooks::get().find(kInitRenderHookName);
    if (hook == nullptr) {
        return 48;
    }
    const int r = hook->original<int(__cdecl*)(const void*)>()(mode);

    const float scale = load_scale();
    const uint32_t rw = g_rec_w.load(std::memory_order_relaxed);
    const uint32_t rh = g_rec_h.load(std::memory_order_relaxed);
    const auto* exe = sdk::Modules::get().exe();
    if (r == 0 && scale > 0.0f && rw != 0 && rh != 0 && exe != nullptr && exe->base != 0) {
        const auto w = static_cast<uint32_t>((static_cast<float>(rw) * scale) * 2.0f) & ~1u;
        const auto h = static_cast<uint32_t>(static_cast<float>(rh) * scale) & ~1u;

        auto* const mw = reinterpret_cast<uint32_t*>(exe->base + kRMode + kRModeWidth);
        auto* const mh = reinterpret_cast<uint32_t*>(exe->base + kRMode + kRModeHeight);
        LOGX("[scenetarget] engine believed %ux%u -> %ux%u", *mw, *mh, w, h);
        *mw = w;
        *mh = h;

        // ---- NOT THE CONSOLE VARIABLES ---------------------------------------------------------
        //
        // Writing ScreenWidth/ScreenHeight looked like the same numbers by another route. It is not:
        // the game PERSISTS them. Setting them to a supersampled size meant the next launch asked
        // for a resolution no adapter enumerates, the mode match fell through to its default, and
        // the game came up at 640x480 -- with the mod uninstalled and nothing to explain it. A mod
        // that changes a setting the user cannot see and that outlives the mod is a trap.
        //
        // g_RMode above is enough. It is what the renderer reads while it is running, and it dies
        // with the process.
    }
    return r;
}

char __fastcall set_presentation_params_detour(void* self, void* /*edx*/, int width, int height,
                                               unsigned char windowed) {
    const float scale = load_scale();
    const uint32_t rw = g_rec_w.load(std::memory_order_relaxed);
    const uint32_t rh = g_rec_h.load(std::memory_order_relaxed);

    LOGX("[scenetarget] present params %dx%d windowed=%d", width, height, windowed);

    if (scale > 0.0f && rw != 0 && rh != 0) {
        if (windowed != 0) {
            // Both eyes side by side in one buffer, so double the width and not the height.
            const int w = static_cast<int>((static_cast<float>(rw) * scale) * 2.0f) & ~1;
            const int h = static_cast<int>(static_cast<float>(rh) * scale) & ~1;
            LOGX("[scenetarget] back buffer %dx%d -> %dx%d (window unchanged)", width, height, w, h);

            width = w;
            height = h;
            g_overrides.fetch_add(1, std::memory_order_relaxed);
        } else {
            // Fullscreen uses DISCARD and must name a real display mode, so an off-display size
            // would fail device creation outright and take the renderer with it.
            LOGX("[scenetarget] FULLSCREEN device -- refusing to resize (DISCARD needs a real mode)");
        }
    }

    auto* hook = Hooks::get().find(kPresentParamsHookName);
    if (hook == nullptr) {
        return 0;
    }
    return hook->original<char(__fastcall*)(void*, void*, int, int, unsigned char)>()(
        self, nullptr, width, height, windowed);
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

    uint32_t w = GetPrivateProfileIntA("render", "per_eye_width", 0, path);
    uint32_t h = GetPrivateProfileIntA("render", "per_eye_height", 0, path);

    // ---- WHAT THE HEADSET ASKED FOR, WITHOUT BEING TOLD ----------------------------------------
    //
    // xr64.exe writes the runtime's recommended per-eye size to LOCALAPPDATA as soon as it starts,
    // and the launcher starts it before the game -- so by the time this runs the number is already
    // on disk. Asking the host directly is not an option: this decision happens seconds after
    // injection, before the renderer builds the target, and the mod cannot wait on a socket for it.
    //
    // A size in the local file still wins, because someone who wrote one down meant it.
    if (w == 0 || h == 0) {
        char shared[MAX_PATH]{};
        if (GetEnvironmentVariableA("LOCALAPPDATA", shared, MAX_PATH) != 0) {
            strcat_s(shared, MAX_PATH, "\\fear2vr\\runtime.ini");
            w = GetPrivateProfileIntA("render", "per_eye_width", 0, shared);
            h = GetPrivateProfileIntA("render", "per_eye_height", 0, shared);
            if (w != 0 && h != 0) {
                LOGX("[scenetarget] headset asks for %ux%u per eye (from %s)", w, h, shared);
            }
        }
    }

    // Permille rather than a float: GetPrivateProfileInt cannot read one, and a second parser for a
    // single number is not worth writing.
    //
    // DEFAULT OFF. Shipping this on produced a back buffer with THREE panels -- a raw pre-post scene
    // across the left half, and the engine's post-processed pair squeezed into the right -- so the
    // host's 50% slice handed one raw image to the left eye and two small ones to the right. The
    // composite below blits the scene target BEFORE the engine's post chain has run, and the engine
    // then draws its own result over part of it. Until that is understood, this stays off.
    const uint32_t permille = GetPrivateProfileIntA("render", "supersample_permille", 0, path);
    if (permille == 0) {
        LOGX("[scenetarget] supersampling off (set supersample_permille in %s) -- back buffer as-is",
             path);
        return;
    }
    if (w == 0 || h == 0) {
        LOGX("[scenetarget] no per-eye size known -- is xr64.exe running? back buffer as-is");
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
    if (!Hooks::get().install(kPresentParamsHookName,
                              reinterpret_cast<void*>(exe->base + kPresentParams),
                              reinterpret_cast<void*>(&set_presentation_params_detour))) {
        return "could not hook Renderer_SetPresentationParams";
    }
    if (!Hooks::get().install(kInitRenderHookName, reinterpret_cast<void*>(exe->base + kInitRender),
                              reinterpret_cast<void*>(&init_render_detour))) {
        return "could not hook r_InitRender";
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


bool SceneTarget::main_view_size(int32_t& w, int32_t& h) {
    // Nothing to override any more. Enlarging the BACK BUFFER keeps the engine's own main-view test
    // -- target size equals back buffer size -- true by construction, which is most of why it is the
    // better lever than enlarging one target inside a small engine.
    (void)w;
    (void)h;
    return false;
}
