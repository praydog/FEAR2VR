#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "Mod.hpp"

// ---- WHAT THE SCENE IS ACTUALLY DRAWN INTO ----------------------------------------------------
//
// Engine-level supersampling means making the engine draw the 3D scene into a target bigger than
// the window, then handing THAT to the runtime and letting the desktop keep the small one as a
// mirror. Whether that is possible at all turns on a single bit.
//
// FEAR2 binds render targets two ways (Renderer_SetRenderTargetSurface, FEAR2.exe 0x6131C6):
//
//   flags & 0x800 == 0   a real offscreen surface -- SetRenderTarget(0, tex) + SetDepthStencil.
//                        Size is whatever was asked for; nothing constrains it.
//
//   flags & 0x800 != 0   VIRTUAL. Nothing is bound; the pass draws into the BACK BUFFER through a
//                        SetViewport sub-rect and is StretchRect'd out afterwards. The bind is
//                        REFUSED unless x + width <= BackBufferWidth and y + height <= Height
//                        (0x61329A / 0x6132B1) -- the only place in the whole render-target system
//                        where the back buffer constrains a size.
//
// So if the main view is a 0x800 target, no amount of asking for a bigger render target will work
// while the window stays small, and the approach has to change. If it is a real offscreen target,
// the size is free: RTHandle_Create passes width and height through verbatim with no shift, mask or
// clamp anywhere, and d3d_InitFrame derives both the viewport AND the frustum from the installed
// target's dimensions -- so a larger target supersamples correctly with no projection work at all.
//
// This mod exists to answer that question against the running game rather than by argument. It only
// observes: SceneRenderer_BeginRenderTarget is the one place every scene target passes through.
class SceneTarget final : public Mod {
public:
    static SceneTarget& get();

    std::string_view get_name() const override { return "SceneTarget"; }
    std::optional<std::string> on_initialize() override;
    void on_shutdown() override;

    // One distinct (flags, width, height) triple seen at a scene-target bind, with how often.
    struct Seen {
        uint32_t flags;
        uint32_t width;
        uint32_t height;
        uint64_t binds;
    };

    static constexpr size_t kMaxSeen = 12;

    struct Report {
        bool hooked;
        uint64_t binds;
        uint32_t distinct;
        std::array<Seen, kMaxSeen> seen;
    };
    Report report() const;

    // ---- THE OVERRIDE -------------------------------------------------------------------------
    //
    // Measured live: the main view is 2560x1440 with flags 0xFC8, and 0x800 is set -- the VIRTUAL
    // path. So the scene is drawn straight into the back buffer and the bind is refused for
    // anything larger than it. Asking for a bigger target changes nothing while that bit is on.
    //
    // Clearing it is deterministic rather than a guess: RTHandle_Create masks the incoming flags
    // with ~0x7 whenever 0x800 is present (0x613549), so the same request without 0x800 resolves to
    // exactly 0x7C8 -- a real offscreen colour texture with its own depth-stencil, sized verbatim.
    //
    // scale is a multiplier on the runtime's recommended PER-EYE size. 0 disables the override and
    // returns the engine to drawing at the back buffer.
    void set_scale(float scale);
    void set_recommended(uint32_t per_eye_w, uint32_t per_eye_h);
    float scale() const;
    uint32_t target_w() const;
    uint32_t target_h() const;
    uint64_t overrides() const;

    // ---- TRANSITION TRACING ---------------------------------------------------------------
    // Two symptoms need evidence rather than theory: no VR quad at the INITIAL main menu (but a
    // working one after returning from a world), and a scene render target that comes back the
    // wrong size on a SECOND world load -- black down the right and bottom, i.e. content drawn
    // smaller than the surface holding it.
    //
    // Every device-shaping call marks a transition; the next frame with a device in hand reports
    // the LIVE back buffer and viewport. Bounded to a handful of lines per transition so a play
    // session stays readable.
    void note_transition(const char* why);
    void trace_if_pending(void* d3d9_device);
    // The size the main view is actually rendering at, when the override is active. Everything that
    // recognised the main view by comparing against the BACK BUFFER has to ask this instead: once
    // the scene draws into its own target, the back buffer's size stops describing the scene at all
    // and the stereo path silently stops firing.
    static bool main_view_size(int32_t& w, int32_t& h);


private:
    SceneTarget() = default;
};
