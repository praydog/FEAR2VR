#include "SceneTarget.hpp"

#include <windows.h>

#include <d3d9.h>
#include <intrin.h>

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

// Set by anything that reshapes the device; consumed by the next frame that has one.
std::atomic<bool> g_trace_pending{false};
// What we last forced the back buffer to, so a virtual target that disagrees can be spotted.
std::atomic<uint32_t> g_last_forced_w{0};
std::atomic<uint32_t> g_last_forced_h{0};
std::atomic<uint64_t> g_transition{0};
char g_trace_why[64]{};

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
// ---- WHO CREATES THE RENDER TARGETS ------------------------------------------------------------
//
// CLTRenderer::CreateRenderTarget, FEAR2.exe 0x0060B6D1. The IDB annotation on its implementation
// (Renderer_CreateRenderTarget_Impl, 0x611129) records that this is the ONLY entry point through
// which render targets are created in this engine.
//
// CONVENTION READ FROM THE DISASSEMBLY, not from the decompiler: the epilogue is `retn 10h`, so
// four callee-cleaned stack arguments and no `this` -- __stdcall(width, height, flags, &out). ECX
// is loaded with a value that is forwarded to the implementation, not an object pointer.
//
// This exists to answer one question: who creates the 2560x1440 target on the second world load?
// The bind site could never say -- it only ever sees a handle it was handed, which is why every
// size reported the identical caller.
constexpr uintptr_t kCreateRenderTargetRva = 0x20B6D1;

// Dedupe is PER GENERATION and cleared on every transition. Keyed on (size, creator) alone and kept
// globally, a second identical creation by the same function -- exactly the "was the world target
// recreated on the second load?" question -- produces the same tuple and stays hidden forever.
std::atomic<uint64_t> g_crt_seq{0};
std::atomic<uint64_t> g_bind_seq{0};
constexpr const char* kCreateRTHookName = "CLTRenderer_CreateRenderTarget";
std::atomic<bool> g_crt_hooked{false};

char __stdcall create_render_target_detour(int width, int height, int flags, void** out) {
    auto* hook = Hooks::get().find(kCreateRTHookName);
    if (hook == nullptr) {
        return 0;
    }
    // Report only targets big enough to be the scene, and only once per distinct size, so the
    // engine's many small auxiliary targets cannot crowd out the one that matters.
    // EVERY virtual (0xFC8) creation, in order, bounded. No dedupe at all here on purpose: the
    // question is create-vs-select on the second world load, and any dedupe -- by size, by creator,
    // or per "generation" -- can hide a second identical creation. Generations were worse than
    // useless for this: they only advance on a transition, and the bad 2560x1440 creation was
    // measured to have NO SetPresentationParams or InitRender before it, so the counter would not
    // have moved between the two world loads.
    if (flags == 0xFC8 && width >= 256 && height >= 256) {
        const auto seq = g_crt_seq.fetch_add(1, std::memory_order_relaxed);
        if (seq < 64) {
            const auto ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
            const auto* gc = sdk::Modules::get().game_client();
            const auto* exe = sdk::Modules::get().exe();
            const char* where = "?";
            uintptr_t rel = ret;
            if (gc != nullptr && gc->base != 0 && ret >= gc->base && ret < gc->base + gc->size) {
                where = "gameclient.dll"; rel = ret - gc->base;
            } else if (exe != nullptr && exe->base != 0 && ret >= exe->base &&
                       ret < exe->base + exe->size) {
                where = "FEAR2.exe"; rel = ret - exe->base;
            }
            // After the original, so *out is the created handle rather than the caller's variable.
            const char r = hook->original<char(__stdcall*)(int, int, int, void**)>()(width, height,
                                                                                     flags, out);
            LOGX("[trace] #%llu CreateRenderTarget %dx%d creator=%s+0x%IX -> handle=%p ok=%d",
                 static_cast<unsigned long long>(seq), width, height, where, rel,
                 (out != nullptr) ? *out : nullptr, static_cast<int>(r));
            return r;
        }
    }
    return hook->original<char(__stdcall*)(int, int, int, void**)>()(width, height, flags, out);
}

void install_create_rt_probe() {
    if (g_crt_hooked.load(std::memory_order_acquire)) {
        return;
    }
    const auto* exe = sdk::Modules::get().exe();
    if (exe == nullptr || exe->base == 0) {
        return;
    }
    if (Hooks::get().install(kCreateRTHookName,
                             reinterpret_cast<void*>(exe->base + kCreateRenderTargetRva),
                             reinterpret_cast<void*>(&create_render_target_detour))) {
        g_crt_hooked.store(true, std::memory_order_release);
        LOGX("[trace] CreateRenderTarget probe installed at 0x%08IX",
             exe->base + kCreateRenderTargetRva);
    }
}

char __fastcall begin_render_target_detour(void* self, void* /*edx*/, void* handle, int a3, void* a4,
                                           int a5) {
    if (handle != nullptr) {
        const auto h = reinterpret_cast<uintptr_t>(handle);
        const uint32_t flags = *reinterpret_cast<const uint32_t*>(h + kHandleFlags);
        const auto pooled = *reinterpret_cast<const uintptr_t*>(h + kHandlePooled);
        if (pooled != 0) {
            const uint32_t pw = *reinterpret_cast<const uint16_t*>(pooled + kPooledWidth);
            const uint32_t ph = *reinterpret_cast<const uint16_t*>(pooled + kPooledHeight);

            // ---- WHO REBUILDS THE VIRTUAL TARGET, AND AT WHAT SIZE -----------------------------
            //
            // Measured across menu -> world -> menu -> world: the first world binds a virtual
            // (0x800) target at 4320x2224 and looks right; the SECOND binds one at 2560x1440, with
            // no SetPresentationParams or InitRender transition anywhere near it. A 2560x1440
            // extent inside a 4320x2224 back buffer is 59% of the width and 65% of the height --
            // black down the right and bottom, on screen and in the headset.
            //
            // This point is READ-ONLY on purpose. By the time a bind happens the surface behind the
            // handle is already allocated, so rewriting the dimensions here would only falsify the
            // metadata and invite out-of-bounds rendering. What is needed is the ALLOCATION site,
            // so log the caller and the handle identity and go find it.
            //
            // Blanket-forcing every 0x800 bind would be wrong anyway: legitimate ones appear at
            // other sizes (3953x2224 early on).
            // EVERY virtual bind, in order, with the real handle. The mismatch-only report below
            // cannot answer create-vs-select: a correctly sized world target could be created AND
            // bound before the bad one and never appear, because it matches the forced size. Only
            // an unfiltered sequence interleaved with the creation sequence shows whether the world
            // target was never selected or was selected and then replaced.
            // flags == 0xFC8 exactly, so this stream covers the same population as the creation
            // probe. And only when the bound target CHANGES: these rebind every frame, so logging
            // each one spends the whole budget on repeats before a world ever loads. Run-length
            // compression keeps the full chronology of SELECTIONS, which is the actual question.
            if (flags == 0xFC8) {
                static std::atomic<uintptr_t> last_handle{0};
                static std::atomic<uint32_t> last_size{0};
                const uint32_t size_key = (pw << 16) | (ph & 0xFFFFu);
                // BOTH exchanges unconditionally, THEN compare. Written as
                // `a.exchange(..) != x || b.exchange(..) != y` the || short-circuits whenever the
                // handle changed, so the size exchange never ran and last_size stayed stale --
                // making the very next identical bind look like another change.
                const uintptr_t old_handle = last_handle.exchange(h, std::memory_order_relaxed);
                const uint32_t old_size = last_size.exchange(size_key, std::memory_order_relaxed);
                const bool changed = (old_handle != h) || (old_size != size_key);
                if (changed) {
                    const auto bseq = g_bind_seq.fetch_add(1, std::memory_order_relaxed);
                    if (bseq < 64) {
                        LOGX("[trace] bind #%llu %ux%u handle=0x%08IX (creations so far: %llu)",
                             static_cast<unsigned long long>(bseq), pw, ph, h,
                             static_cast<unsigned long long>(
                                 g_crt_seq.load(std::memory_order_relaxed)));
                    }
                }
            }

            if ((flags & 0x800u) != 0 && g_overrides.load(std::memory_order_relaxed) != 0) {
                // ONE LINE PER DISTINCT (size, pooled object), NOT per bind. A flat budget of
                // eight was spent entirely on the menu's 640x480 target repeating at startup, and
                // the 2560x1440 rebuild -- the only one that matters -- never printed.
                static std::atomic<uint64_t> seen_keys[12]{};
                const uint32_t want_w = g_last_forced_w.load(std::memory_order_relaxed);
                const uint32_t want_h = g_last_forced_h.load(std::memory_order_relaxed);
                const uint64_t key = (static_cast<uint64_t>(pw) << 48) |
                                     (static_cast<uint64_t>(ph & 0xFFFFu) << 32) |
                                     static_cast<uint64_t>(pooled & 0xFFFFFFFFu);
                bool fresh = false;
                if (want_w != 0 && (pw != want_w || ph != want_h)) {
                    fresh = true;
                    for (auto& slot : seen_keys) {
                        const uint64_t had = slot.load(std::memory_order_relaxed);
                        if (had == key) {
                            fresh = false;
                            break;
                        }
                        if (had == 0) {
                            uint64_t expect = 0;
                            if (slot.compare_exchange_strong(expect, key,
                                                             std::memory_order_acq_rel)) {
                                break;
                            }
                            if (slot.load(std::memory_order_relaxed) == key) {
                                fresh = false;
                                break;
                            }
                        }
                    }
                }
                if (fresh) {
                    const auto ret = reinterpret_cast<uintptr_t>(_ReturnAddress());
                    const auto* gc = sdk::Modules::get().game_client();
                    const auto* exe = sdk::Modules::get().exe();
                    const char* where = "?";
                    uintptr_t rel = ret;
                    if (gc != nullptr && gc->base != 0 && ret >= gc->base &&
                        ret < gc->base + gc->size) {
                        where = "gameclient.dll"; rel = ret - gc->base;
                    } else if (exe != nullptr && exe->base != 0 && ret >= exe->base &&
                               ret < exe->base + exe->size) {
                        where = "FEAR2.exe"; rel = ret - exe->base;
                    }
                    // NOTE: this caller is whoever BINDS the target, which is a common consumer
                    // and not necessarily whoever sized it. `pooled` is the address to point a
                    // hardware write watch at -- see /watch/arm -- to catch the actual writer.
                    LOGX("[trace] virtual target %ux%u != forced %ux%u | handle=0x%08IX "
                         "pooled=0x%08IX watch=/watch/arm?addr=0x%08IX&size=4&type=write "
                         "bind_caller=%s+0x%IX",
                         pw, ph, want_w, want_h, h, pooled, pooled + kPooledWidth, where, rel);

                    // Each world allocates a NEW pooled object -- 0x04A62C70 at startup versus
                    // 0x3AC6D420 on the second world -- so a watch armed on the bad one is always
                    // too late for the next cycle, and its address dies with the process.
                    //
                    // Cheaper discriminator first: does the engine's OWN dimension source still
                    // hold our override at this moment? g_RMode is what init_render_detour rewrote,
                    // and 2560x1440 looks far more like a configured resolution being re-read than
                    // anything derived from a 4320x2224 buffer. If g_RMode still reads 4320x2224
                    // here then the size comes from somewhere else and only a write watch will find
                    // it; if it reads 2560x1440 then something put it back and re-asserting it is
                    // the fix.
                    const auto* exe_m = sdk::Modules::get().exe();
                    if (exe_m != nullptr && exe_m->base != 0) {
                        const auto rm_w =
                            *reinterpret_cast<const uint32_t*>(exe_m->base + kRMode + kRModeWidth);
                        const auto rm_h =
                            *reinterpret_cast<const uint32_t*>(exe_m->base + kRMode + kRModeHeight);
                        LOGX("[trace]   g_RMode says %ux%u at this bind (we forced %ux%u)", rm_w,
                             rm_h, want_w, want_h);
                    }
                }
            }

            note(flags, pw, ph);
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

std::atomic<uint32_t> g_saved_screen_w{0};
std::atomic<uint32_t> g_saved_screen_h{0};
std::atomic<bool> g_screen_saved{false};

void set_screen_cvars(uintptr_t exe_base, float w, float h) {
    using SetFloatFn = void(__cdecl*)(void*, const char*, float);
    auto* const set_float = reinterpret_cast<SetFloatFn>(exe_base + kConVarSetFloat);
    auto* const table = reinterpret_cast<void*>(exe_base + kConVarTable);
    set_float(table, "ScreenWidth", w);
    set_float(table, "ScreenHeight", h);
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
        SceneTarget::get().note_transition("InitRender");

        // SAVE BEFORE OVERWRITING, AND ONLY ONCE. This used to store *mw/*mh AFTER assigning the
        // override to them, so the "saved originals" were the supersampled values -- restoring them
        // put the override back rather than undoing it. The once-only guard matters just as much:
        // this runs again on every world load, and a second pass would capture our own override as
        // the original even with the ordering fixed.
        if (!g_screen_saved.load(std::memory_order_acquire)) {
            g_saved_screen_w.store(*mw, std::memory_order_relaxed);
            g_saved_screen_h.store(*mh, std::memory_order_relaxed);
            g_screen_saved.store(true, std::memory_order_release);
        }

        *mw = w;
        *mh = h;



        // ---- THE CONSOLE VARIABLES, WHICH THE HUD LAYS OUT FROM --------------------------------
        //
        // g_RMode alone is NOT enough. The interface sizes itself from these, so leaving them at
        // the old resolution drew the whole HUD at 640x480 scale into the corner of a 4320x2224
        // buffer -- present, but a tenth of the pixels it should cover, which is what "cut off"
        // looks like once the layer is downscaled onto a quad.
        //
        // They are also PERSISTED, which is the trap: a supersampled value saved to the config made
        // the next launch ask for a resolution no adapter enumerates, fall through to the default,
        // and come up at 640x480 with the mod not even loaded. So the originals are kept and put
        // back on unload -- the game only writes its config at exit, and by then ours are gone.
        set_screen_cvars(exe->base, static_cast<float>(w), static_cast<float>(h));
    }
    return r;
}

char __fastcall set_presentation_params_detour(void* self, void* /*edx*/, int width, int height,
                                               unsigned char windowed) {
    const float scale = load_scale();
    const uint32_t rw = g_rec_w.load(std::memory_order_relaxed);
    const uint32_t rh = g_rec_h.load(std::memory_order_relaxed);

    LOGX("[scenetarget] present params %dx%d windowed=%d", width, height, windowed);
    SceneTarget::get().note_transition("SetPresentationParams");

    // ---- ONLY ONCE THERE IS A WORLD -----------------------------------------------------------
    //
    // We inflate the BACK BUFFER and leave the window alone. In world that is invisible, because
    // the scene target is explicitly sized to the buffer. Before a world it is not: the opening
    // movies and the main menu draw at the window's 640x480 and land in the top-left corner of a
    // 4320x2224 buffer. That is the reported regression, and it started exactly when the scene was
    // forced to native.
    //
    // The constraint is that the SCENE renders native -- not the menu. So leave the buffer alone
    // until a session exists and let the movies and menu fill the window they were built for.
    // gameserver.dll is this project's existing session signal: absent at the menu, resolved late
    // when a world starts (see Modules::resolve_lazy_module and the "session started" log).
    //
    // Tried and rejected first: holding g_RMode at the forced size. It was re-asserted at
    // present-params, every frame, and inside both the create and bind detours; the menu still
    // created a 640x480 target, because the pre-world path never reads it.
    const bool in_session = sdk::Modules::get().game_server() != nullptr &&
                            sdk::Modules::get().game_server()->handle != nullptr;
    if (!in_session) {
        static std::atomic<bool> said{false};
        if (!said.exchange(true, std::memory_order_relaxed)) {
            LOGX("[scenetarget] no session yet -- leaving the back buffer at %dx%d so the movies "
                 "and menu fill the window", width, height);
        }
    }

    if (in_session && scale > 0.0f && rw != 0 && rh != 0) {
        if (windowed != 0) {
            // Both eyes side by side in one buffer, so double the width and not the height.
            const int w = static_cast<int>((static_cast<float>(rw) * scale) * 2.0f) & ~1;
            const int h = static_cast<int>(static_cast<float>(rh) * scale) & ~1;
            LOGX("[scenetarget] back buffer %dx%d -> %dx%d (window unchanged)", width, height, w, h);

            width = w;
            height = h;
            g_last_forced_w.store(static_cast<uint32_t>(w), std::memory_order_relaxed);
            g_last_forced_h.store(static_cast<uint32_t>(h), std::memory_order_relaxed);
            g_overrides.fetch_add(1, std::memory_order_relaxed);

            // ---- TELL THE GAME NOW, NOT AT r_InitRender -----------------------------------------
            //
            // The buffer is forced HERE, but g_RMode and the screen cvars were only written in
            // init_render_detour -- which runs after the main menu is already up. In between, the
            // buffer is 4320x2224 while the game still believes the screen is 640x480, so the menu
            // draws at 640x480 into the corner of a much larger buffer. Caught in the trace as
            // "g_RMode says 640x480 at this bind (we forced 4320x2224)", and visible as a tiny
            // interface in the top-left at startup -- a regression introduced by forcing the RT,
            // not by anything the menu does.
            //
            // The in-world HUD was fine because by then InitRender had run. This is the same fix,
            // applied at the moment the buffer actually changes.
            const auto* exe_now = sdk::Modules::get().exe();
            if (exe_now != nullptr && exe_now->base != 0) {
                auto* const mw = reinterpret_cast<uint32_t*>(exe_now->base + kRMode + kRModeWidth);
                auto* const mh = reinterpret_cast<uint32_t*>(exe_now->base + kRMode + kRModeHeight);
                if (!g_screen_saved.load(std::memory_order_acquire)) {
                    g_saved_screen_w.store(*mw, std::memory_order_relaxed);
                    g_saved_screen_h.store(*mh, std::memory_order_relaxed);
                    g_screen_saved.store(true, std::memory_order_release);
                }
                LOGX("[scenetarget] engine believed %ux%u -> %ux%u (at present-params)", *mw, *mh,
                     static_cast<uint32_t>(w), static_cast<uint32_t>(h));
                *mw = static_cast<uint32_t>(w);
                *mh = static_cast<uint32_t>(h);
                set_screen_cvars(exe_now->base, static_cast<float>(w), static_cast<float>(h));
            }
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
    // Read-only probe: names whoever creates each distinct scene-sized render target.
    install_create_rt_probe();

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
// The gate above defers the override until a session exists. That is only safe if the engine asks
// for presentation parameters AGAIN on world load -- if it does not, the first world would render
// at the menu's size, which breaks the one constraint that matters. Rather than assume either way,
// say so the moment it happens: a session with a buffer that is not ours is exactly the failure.
void SceneTarget::check_session_buffer(uint32_t back_w, uint32_t back_h) {
    const auto* gs = sdk::Modules::get().game_server();
    const bool in_session = gs != nullptr && gs->handle != nullptr;
    if (!in_session) {
        return;
    }
    const uint32_t w = g_last_forced_w.load(std::memory_order_relaxed);
    if (w != 0 && back_w == w) {
        return;  // the world got its native buffer
    }
    static std::atomic<bool> warned{false};
    if (!warned.exchange(true, std::memory_order_relaxed)) {
        LOGX("[scenetarget] SESSION ACTIVE BUT BUFFER IS %ux%u -- the engine never re-asked for "
             "presentation params, so the deferred override never applied. The scene is NOT "
             "native.",
             back_w, back_h);
    }
}

void SceneTarget::note_transition(const char* why) {
    const auto n = g_transition.fetch_add(1, std::memory_order_relaxed) + 1;
    strncpy_s(g_trace_why, why, _TRUNCATE);
    g_trace_pending.store(true, std::memory_order_release);
    LOGX("[trace] transition #%llu via %s", static_cast<unsigned long long>(n), why);
}

// Reports what the device ACTUALLY has, rather than what we asked for. The two symptoms both look
// like a disagreement between the buffer and what is drawn into it, and only the live values can
// show that.
void SceneTarget::trace_if_pending(void* d3d9_device) {
    if (!g_trace_pending.exchange(false, std::memory_order_acq_rel) || d3d9_device == nullptr) {
        return;
    }
    auto* dev = static_cast<IDirect3DDevice9*>(d3d9_device);
    IDirect3DSurface9* bb = nullptr;
    D3DSURFACE_DESC bd{};
    if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb != nullptr) {
        bb->GetDesc(&bd);
        bb->Release();
    }
    D3DVIEWPORT9 vp{};
    dev->GetViewport(&vp);
    IDirect3DSurface9* rt = nullptr;
    D3DSURFACE_DESC rd{};
    if (SUCCEEDED(dev->GetRenderTarget(0, &rt)) && rt != nullptr) {
        rt->GetDesc(&rd);
        rt->Release();
    }
    LOGX("[trace] after %s: backbuffer %ux%u | rendertarget %ux%u | viewport %ux%u at (%u,%u)",
         g_trace_why, bd.Width, bd.Height, rd.Width, rd.Height, vp.Width, vp.Height, vp.X, vp.Y);
}

uint64_t SceneTarget::overrides() const { return g_overrides.load(std::memory_order_relaxed); }


bool SceneTarget::main_view_size(int32_t& w, int32_t& h) {
    // Nothing to override any more. Enlarging the BACK BUFFER keeps the engine's own main-view test
    // -- target size equals back buffer size -- true by construction, which is most of why it is the
    // better lever than enlarging one target inside a small engine.
    (void)w;
    (void)h;
    return false;
}

void SceneTarget::on_shutdown() {
    // Put the resolution back before the game can save ours. Without this the mod leaves behind a
    // config the game cannot start from, long after the mod is gone -- see the note in the detour.
    if (!g_screen_saved.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    const auto* exe = sdk::Modules::get().exe();
    if (exe == nullptr || exe->base == 0) {
        return;
    }
    const auto w = g_saved_screen_w.load(std::memory_order_relaxed);
    const auto h = g_saved_screen_h.load(std::memory_order_relaxed);
    auto* const mw = reinterpret_cast<uint32_t*>(exe->base + kRMode + kRModeWidth);
    auto* const mh = reinterpret_cast<uint32_t*>(exe->base + kRMode + kRModeHeight);
    *mw = w;
    *mh = h;
    set_screen_cvars(exe->base, static_cast<float>(w), static_cast<float>(h));
    LOGX("[scenetarget] restored the engine's resolution to %ux%u", w, h);
}
