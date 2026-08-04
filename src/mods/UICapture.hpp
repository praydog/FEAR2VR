#pragma once

#include <d3d9.h>

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "../Mod.hpp"

// ---- THE UI, ON ITS OWN SURFACE -------------------------------------------
//
// Redirects the HUD into a render target this mod owns, so it can be composited
// as its own quad in the headset instead of being baked across both eyes of the
// world image.
//
// WHY IT IS THIS SMALL. Everything hard was answered by measurement first
// (`RenderTimeline`, and ENGINE_NOTES "The HUD has its own render-target
// bracket"): the engine closes the main view's target and opens a NEW bracket,
// on the same target, containing the ten 2D passes and NOTHING else. So there is
// no need to identify Scaleform internals, bracket by time, or filter draws --
// binding a different surface for the duration of one bracket isolates the UI
// exactly.
//
// WHICH BRACKET IS LEARNED, NOT ASSUMED. `RenderTimeline::hud_bracket()` reports
// the ordinal that took 2D passes on the PREVIOUS frame. It is 2 in normal play
// and something else at a menu or during a load, which is why it is measured
// every frame rather than written down here.
//
// HOW THE SWAP WORKS. On that bracket's begin -- after the engine's own call, so
// its target is bound and its viewport derived -- save the current colour surface,
// bind ours, and clear it to TRANSPARENT black. On the bracket's end, before the
// engine tears it down, put the original back. The engine draws the HUD in
// between and never knows.
//
// ALPHA IS THE POINT. The surface is A8R8G8B8 and starts fully transparent, so
// what accumulates is the UI over nothing -- which is exactly what a composition
// layer needs. Whether Scaleform's blend modes leave usable destination alpha is
// a question only measurement answers; `alpha_coverage()` reports it rather than
// assuming it.
//
// DEFAULT-POOL DISCIPLINE. `CreateRenderTarget` is implicitly D3DPOOL_DEFAULT, and
// D3D9 refuses to reset a device while one is alive -- so holding it through an
// alt-tab makes the device UNRESETTABLE for the rest of the session. FrameCapture
// records having caused exactly that. The surface is therefore dropped the moment
// `TestCooperativeLevel` stops saying D3D_OK, and recreated lazily.
//
// OFF BY DEFAULT: while armed the game's own back buffer no longer receives the
// HUD, which is visible on the desktop window.
class UICapture final : public Mod {
public:
    static UICapture& get();

    std::string_view get_name() const override { return "UICapture"; }
    std::optional<std::string> on_initialize() override;
    void on_frame() override {}
    void on_shutdown() override;

    void set_enabled(bool on) { m_enabled.store(on, std::memory_order_release); }

    // BISECTION. The swap does three things and each could be the one that breaks the frame, so
    // each is separately disableable rather than reasoned about: 1 = bind and restore only,
    // 2 = + clear to transparent, 3 = + force the alpha write mask. Default 3.
    void set_mode(int mode) { m_mode.store(mode, std::memory_order_release); }
    int mode() const { return m_mode.load(std::memory_order_relaxed); }

    // Copy the borrowed target's contents out and back around the swap. OFF by default because it
    // is measured NOT to help: the back buffer is already black when the bracket opens, so the copy
    // preserves nothing. Kept because it is the mechanism a mirror fix would use once the composite
    // is understood.
    void set_preserve(bool on) { m_preserve.store(on, std::memory_order_release); }
    bool preserve() const { return m_preserve.load(std::memory_order_relaxed); }

    bool enabled() const { return m_enabled.load(std::memory_order_relaxed); }

    // Write the captured surface to a BMP. Serviced on the render thread at the next
    // frame boundary, because reading a render target means a device call.
    // `backbuffer` takes the real post-HUD back buffer instead of our redirected target.
    bool request_shot(const std::string& path, bool backbuffer = false);

    bool have_surface() const { return m_surface.load(std::memory_order_relaxed) != nullptr; }
    int32_t width() const { return m_width.load(std::memory_order_relaxed); }
    int32_t height() const { return m_height.load(std::memory_order_relaxed); }
    uint64_t swaps() const { return m_swaps.load(std::memory_order_relaxed); }
    uint64_t restores() const { return m_restores.load(std::memory_order_relaxed); }
    uint64_t failures() const { return m_failures.load(std::memory_order_relaxed); }
    uint32_t device_lost_events() const { return m_device_lost.load(std::memory_order_relaxed); }

    // Fraction of the last read-back surface with non-zero alpha, x1000. The measurement that
    // says whether the isolated UI is usable as a transparency layer at all: near zero means the
    // HUD drew nothing, and 1000 means it filled the frame opaquely (also a failure).
    int32_t alpha_coverage() const { return m_alpha_coverage.load(std::memory_order_relaxed); }

    // The colour write mask as the engine left it at the end of the UI bracket. 0xF includes
    // alpha; 0x7 is RGB only and means the mask is re-set inside the bracket.
    uint32_t colorwrite_at_end() const { return m_colorwrite_at_end.load(std::memory_order_relaxed); }

    // Whether the surface we displace for the UI bracket IS the swap chain's back buffer.
    bool target_is_backbuffer() const { return m_target_is_backbuffer.load(std::memory_order_relaxed); }
    uint32_t last_tcl() const { return m_last_tcl.load(std::memory_order_relaxed); }

    // Non-black fraction (per mille) of the back buffer sampled IN PHASE, immediately after the
    // borrowed target is restored. -1 until probed.
    void request_probe() {
        m_probe_at_begin.store(true, std::memory_order_release);
        m_probe_at_end.store(true, std::memory_order_release);
    }
    int32_t probe_begin_lit() const { return m_probe_begin_lit.load(std::memory_order_relaxed); }
    int32_t probe_lit() const { return m_probe_lit.load(std::memory_order_relaxed); }
    uintptr_t backbuffer_ptr() const { return m_backbuffer_ptr.load(std::memory_order_relaxed); }
    uintptr_t displaced_ptr() const { return m_displaced_ptr.load(std::memory_order_relaxed); }

    // Called from the bracket callback and the frame boundary; public for the free functions
    // in the .cpp's anonymous namespace.
    void on_bracket(bool begin, int32_t width, int32_t height, uint32_t index);
    void on_pass(uint32_t ordinal, uintptr_t caller);
    void on_present();
    void free_device_resources();

    // How many UI layers have reached the shared mapping.
    uint64_t publishes() const { return m_publishes.load(std::memory_order_relaxed); }
    int32_t layer_width() const { return m_layer_w.load(std::memory_order_relaxed); }
    int32_t layer_height() const { return m_layer_h.load(std::memory_order_relaxed); }

    // Who issued each 2D pass last frame. Read from the IPC thread as a snapshot; the writer is the
    // render thread and a torn read here costs a wrong diagnostic line, not correctness.
    uint32_t pass_count() const { return m_pass_seen; }
    uintptr_t pass_caller(uint32_t i) const { return i < kMaxPassRecord ? m_pass_caller[i] : 0; }

private:
    UICapture() = default;

    void publish_layer(struct IDirect3DDevice9* dev);
    void swap_target(struct IDirect3DDevice9* dev);
    void restore_target(struct IDirect3DDevice9* dev);

    std::atomic<bool> m_enabled{false};
    std::atomic<int> m_mode{3};
    std::atomic<bool> m_preserve{false};
    std::atomic<void*> m_surface{nullptr};   // IDirect3DSurface9*, our colour target
    std::atomic<void*> m_saved{nullptr};     // the engine's surface, held only inside a bracket
    std::atomic<void*> m_scratch{nullptr};   // the borrowed target's contents, kept across the swap
    std::atomic<void*> m_scaled{nullptr};    // the layer at publish size, GPU side
    std::atomic<void*> m_stage{nullptr};     // SYSTEMMEM staging for the readback
    std::atomic<int32_t> m_layer_w{1280};
    std::atomic<int32_t> m_layer_h{720};
    std::atomic<bool> m_published{false};
    std::atomic<uint64_t> m_publishes{0};

    // Per-pass caller record for the frame in flight. Game-thread only.
    static constexpr uint32_t kMaxPassRecord = 24;
    uintptr_t m_pass_caller[kMaxPassRecord]{};
    uint32_t m_pass_seen{0};
    bool m_seen_fullscreen{false};   // an engine full-screen pass has run this frame
    bool m_cleared_this_frame{false};
    std::atomic<int32_t> m_width{0};
    std::atomic<int32_t> m_height{0};
    std::atomic<uint64_t> m_swaps{0};
    std::atomic<uint64_t> m_restores{0};
    std::atomic<uint64_t> m_failures{0};
    std::atomic<uint32_t> m_device_lost{0};
    std::atomic<int32_t> m_alpha_coverage{-1};
    D3DVIEWPORT9 m_saved_viewport{};   // render thread only, guarded by the flag below
    std::atomic<bool> m_have_saved_viewport{false};
    std::atomic<bool> m_probe_at_begin{false};
    std::atomic<bool> m_probe_at_end{false};
    std::atomic<int32_t> m_probe_begin_lit{-1};
    std::atomic<int32_t> m_probe_lit{-1};
    std::atomic<uint32_t> m_last_tcl{0};
    std::atomic<bool> m_target_is_backbuffer{false};
    std::atomic<uintptr_t> m_backbuffer_ptr{0};
    std::atomic<uintptr_t> m_displaced_ptr{0};
    std::atomic<uint32_t> m_colorwrite_at_end{0xFFFFFFFFu};
    std::atomic<uint32_t> m_saved_colorwrite{0};
    std::atomic<bool> m_have_saved_colorwrite{false};
    std::atomic<bool> m_release_requested{false};
    std::atomic<bool> m_released{false};
    std::atomic<bool> m_shot_pending{false};
    std::atomic<bool> m_shot_backbuffer{false};
    std::string m_shot_path;  // guarded by m_shot_pending's release/acquire
};
