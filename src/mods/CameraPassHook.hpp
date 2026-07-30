#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "../Mod.hpp"

// THE PERSPECTIVE PASS, HOOKED -- where a stereo view is substituted.
//
// `CLTRenderer_SetupPassPerspective` (vtable slot 15, FEAR2.exe 0x0060B520) is the one call that decides what
// the next DrawScene sees:
//
//     __stdcall(const LTNodeTransform* camera,   // position + quaternion
//               const float fov[2],              // ANGLES IN RADIANS, x then y
//               const float rect[4],             // NORMALISED viewport, left/top/right/bottom in 0..1
//               float depthMin, float depthMax)  -> bool, retn 14h
//
// sdk::SceneCamera documents why this argument is the right thing to change: it is forwarded to
// SetupCameraShaderParams, copied to SceneCamera+0x14 as the pose, and inverted into the view matrix -- so
// replacing it moves the view matrix, the view-projection and the world-to-screen matrix TOGETHER. Patching
// three matrices separately and hoping they agree is the alternative, and it is how the camera work earlier in
// this project went wrong five times.
//
// ---- WHAT MAKES STEREO CHEAP HERE ------------------------------------------------------------------------
//
// Two properties of this entry, both established from the disassembly rather than assumed:
//
//   * FOV IS AN ANGLE. No projection matrix is built by the caller, so a per-eye field of view is a float.
//   * THE VIEWPORT IS FRACTIONAL. {0,0,0.5,1} is the left half of the render target and {0.5,0,1,1} the
//     right, so a side-by-side pair needs no matrix work at all.
//
// And one limit worth stating before someone plans around it: the projection CENTRE OFFSET is hardcoded (0,0)
// by this entry, so the asymmetric frustum a real HMD wants is NOT reachable through it. Symmetric per-eye FOV
// plus a sub-rect is. An asymmetric frustum needs the record field written after setup, which is a separate
// piece of work and is not pretended to be done here.
//
// ---- WHAT THIS MOD DOES, AND DOES NOT, DO ----------------------------------------------------------------
//
// It observes every pass, and it can displace the camera along its OWN axes (an eye offset) and narrow the
// viewport to one half. That is one eye per frame, which is the honest description: rendering BOTH eyes needs
// the setup/draw/end group driven twice inside one target -- the engine does exactly that six times in
// Renderer_MakeCubicEnvMap -- and that is the next step, not this one.
//
// Off by default. It changes what the player sees.
class CameraPassHook final : public Mod {
public:
    static CameraPassHook& get();

    std::string_view get_name() const override { return "CameraPassHook"; }

    std::optional<std::string> on_initialize() override;
    void on_frame() override {}
    void on_shutdown() override;

    enum class Eye : uint8_t {
        Off = 0,   // observe only
        Left = 1,
        Right = 2,
    };

    // Displace the camera by `half_ipd` along its own right vector -- negative for the left eye, positive for
    // the right -- and optionally restrict the viewport to that half of the target.
    //
    // `half_ipd` is in WORLD UNITS, not metres: this engine's scale is not established here, so a caller
    // picks a number and looks. Reporting it back in observed() is what makes that iteration possible.
    void set_eye(Eye eye, float half_ipd, bool split_viewport);

    // Override the field of view the pass is set up with, in RADIANS, or a non-positive value to leave the
    // game's own value alone. Clamped by the engine to 179 degrees (SceneCamera::kMaxFovRadians), so a caller
    // asking for more silently gets that -- predicted_half_view_plane() will say so.
    void set_fov_override(float fov_x, float fov_y);

    struct Observed {
        bool hooked{};
        uintptr_t target{};
        uint64_t passes{};          // setup calls seen
        uint64_t overridden{};      // calls whose camera we replaced
        uint64_t rejected{};        // calls where the offset could not be applied (bad pose)

        // The ARGUMENTS the engine passed, captured in the detour. In-phase with the frame they configure,
        // unlike anything read from the record afterwards.
        std::array<float, 3> camera_position{};
        std::array<float, 4> camera_rotation{};
        std::array<float, 2> fov{};
        std::array<float, 4> rect{};
        float depth_min{};
        float depth_max{};

        Eye eye{Eye::Off};
        float half_ipd{};
        bool split_viewport{};
        std::array<float, 2> fov_override{};
    };

    Observed observed() const;

private:
    CameraPassHook() = default;
};
