#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "../Mod.hpp"

// THE FRAME BOUNDARY, HOOKED. This is the foundation a stereo path is built on: the one place per frame where
// the engine has finished drawing and is about to hand the back buffer to D3D.
//
// ---- WHY HERE AND NOT THE COM VTABLE ----------------------------------------------------------------------
//
// `sdk::Render::device_vtable()` is patchable, and measured writable on this machine. It is still the worse
// choice: that table is heap storage belonging to d3d9.dll, shared by every object that uses it, and Steam's
// overlay already proxies the d3d9 FACTORY in this process. Patching it means joining a queue whose ordering
// nobody controls, and re-joining it after every device reset.
//
// `LTRenderer_PresentAndSync` is an ordinary function in the exe's .text. safetyhook brackets it like any other
// engine function, it survives device resets because it is not part of the device, and `Hooks::retire()`
// removes it on uninject with the rest.
//
// ---- HOW THE TARGET WAS FOUND -----------------------------------------------------------------------------
//
// An EXECUTE watchpoint on the live d3d9.dll Present entry (`/watch/arm?type=exec`). Present runs every frame,
// so the trap needed no player input; its caller candidates named LTRenderer_PresentAndSync directly and
// CLTRenderer_SwapBuffers above it. See AGENT.MD rule 5 -- an offset scan could not have answered this.
//
// ---- WHAT IT MEASURES, AND WHY IN-HOOK ----------------------------------------------------------------------
//
// The renderer's state word gates presentation: CLTRenderer_SwapBuffers returns early unless it equals 1. But
// that word changes DURING a frame -- sampled from the IPC thread it reads 4 with the window focused and 1
// while it is not, which says nothing about its value at the moment a frame is presented.
//
// So it is sampled INSIDE the detour, where the question has an answer. That is the same-phase rule this
// project learned the hard way and TESTING.MD records: a value the engine rewrites between two out-of-band
// reads cannot be compared across them.
class RenderHook final : public Mod {
public:
    static RenderHook& get();

    std::string_view get_name() const override { return "RenderHook"; }

    std::optional<std::string> on_initialize() override;
    void on_frame() override {}
    void on_shutdown() override {}

    // A FUNCTION TO RUN ON EVERY PRESENTED FRAME -- the extension point a stereo submit occupies.
    //
    // Runs on the render thread, inside the detour, BEFORE the engine presents. Callbacks must not block, must
    // not allocate, and must not call back into the framework's locks: a frame boundary is the worst place in
    // the process to introduce a wait.
    //
    // Fixed capacity and no removal on purpose. Mods live for the whole injection and are retired together, so
    // an unregister path would be an unused code path guarding a lifetime that cannot happen; the hook is
    // removed wholesale by Hooks::retire(). Returns false when full.
    using PresentCallback = void (*)();
    static constexpr size_t kMaxCallbacks = 8;
    bool add_present_callback(PresentCallback cb);

    struct Stats {
        bool hooked{};
        uintptr_t target{};        // LTRenderer_PresentAndSync
        uint64_t frames{};         // presents observed since injection
        double mean_interval_ms{}; // wall time between presents, averaged over the samples below
        uint32_t samples{};        // intervals contributing to the mean
        // The renderer state word AS READ INSIDE THE DETOUR. The gate above this function requires 1, so a
        // frame that reaches here should always see 1 -- and unlike an out-of-band read, this one is in phase
        // with the frame it describes.
        uint32_t state_at_present{};
        uint64_t state_not_one{};  // presents that did NOT see 1; expected 0, and a real finding if not
        uint32_t callbacks{};
    };

    Stats stats() const;

private:
    RenderHook() = default;
};
