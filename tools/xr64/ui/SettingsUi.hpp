#pragma once

// Owns the whole in-headset settings panel: the Rml::Context, the document, its own OpenXR
// swapchain and composition-layer quad, and the HTTP link to the mod. main.cpp's contact with any
// of this is exactly the four calls below -- everything else lives in SettingsUi.cpp so that file,
// not main.cpp, is where the next change to the panel happens.
//
// WHY ITS OWN SWAPCHAIN, not main.cpp's existing `ui_swapchain`: that swapchain's whole lifecycle
// (create/resize/upload) is driven by the mod's shared-memory HUD frames -- it does not exist until
// the mod publishes a HUD frame, is sized to WHATEVER the mod published, and every frame's pixels
// come from a raw byte copy out of shared memory (see the "UPLOAD ONLY WHEN THERE IS SOMETHING NEW"
// block in main.cpp). None of that has anything to do with "is the settings menu open" -- a wearer
// must be able to open Settings even if the mod is not running yet -- so sharing the swapchain
// would mean fighting that upload for ownership of the same texture on every frame either is
// active. A second small swapchain, composited as a second quad, costs one XrSwapchain and is
// unconditionally correct instead.

#include <cstdint>

#include <d3d11.h>
#include <openxr/openxr.h>

namespace xrui {

class SettingsUi {
public:
    SettingsUi();
    ~SettingsUi();

    // One-time setup, called once after the D3D11 device, the OpenXR session and its action set
    // are attached. Everything here is BORROWED, never owned:
    //   - `swapchain_format` is main.cpp's already-resolved `screen_format` -- reused so this
    //   - `trigger_action`/`aim_space`/`hand_path` are actions, spaces and
    //     paths main.cpp already created, suggested bindings for, and attached -- read-only here,
    //     so there is exactly one owner of controller input in the process. `hand_path` is needed
    //     alongside `aim_space` because reading a per-hand float action (the trigger) requires an
    //     XrActionStateGetInfo::subactionPath, not a space.
    //   - `hud_distance_m` points at main.cpp's own `ui_distance_m` local (its `--ui-distance`
    //     value). That parameter positions the MOD's HUD quad and has no HTTP route on the mod
    //     side -- it is host process state, so the settings panel edits it directly rather than
    //     round-tripping through a socket to itself.
    bool init(ID3D11Device* device, ID3D11DeviceContext* context, XrInstance instance, XrSession session,
              XrSpace view_space, int64_t swapchain_format, XrAction trigger_action,
              const XrSpace aim_space[2], const XrPath hand_path[2], float* hud_distance_m);

    // One call per frame. Advances the RmlUi clock, polls the menu button (open/close) and, while
    // open, the aim ray of whichever controller is pointing at the panel plus its trigger (click),
    // applies any settings the mod reported since the last poll, updates the Rml context, and --
    // only while visible -- renders into this panel's own swapchain image. Returns the quad layer
    // to append to this frame's layer list, or nullptr while hidden, so main.cpp only grows its
    // layer array when there is something to show.
    //
    // `should_render` is this frame's XrFrameState::shouldRender. False means the runtime has told
    // the app not to produce pixels, so the swapchain acquire/wait/render/release is skipped and
    // the return is nullptr -- but everything above it still runs, because a menu press or a
    // settings change arriving on a hidden frame must not be thrown away. Deliberately has no
    // default argument: which frames render is the caller's decision and belongs at the call site.
    const XrCompositionLayerQuad* update(XrTime predicted_display_time, bool should_render);

    void shutdown();

    bool visible() const;

    // Driven by main.cpp's Menu latch: a short tap must still reach the game as pause, so the
    // decision cannot live inside the panel.
    void toggle();

    // Pointer/click injection entry points. update()'s own aim-ray hit test drives these, but they
    // are public so a caller with a different idea of "where the pointer is" (a different input
    // scheme, a test) is not forced through OpenXR action state to reach the same context.
    // `x`/`y` are panel-local pixel coordinates, origin top-left, matching Rml::Context's own
    // window-coordinate convention.
    void onPointerMove(int x, int y);
    void onPointerButton(bool down);

    // Public only so ui/SettingsUi.cpp's file-local helper classes can name the type (it stays
    // opaque outside that translation unit -- forward-declared, never defined here). `m_impl`
    // itself, the only thing that actually matters for encapsulation, stays private below.
public:
    struct Impl;

private:
    Impl* m_impl = nullptr;
};

} // namespace xrui
