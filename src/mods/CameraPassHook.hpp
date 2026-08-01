#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../Mod.hpp"

#include "sdk/regenny/regenny/LTNodeTransform.hpp"

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

    // ONLY DISPLACE THE MAIN VIEW, which is on by default and is a correctness setting rather than a
    // preference.
    //
    // The renderer issues two perspective passes per frame and they are indistinguishable by their arguments
    // -- same FOV, same camera, same {0,0,1,1} rect. Only the bound TARGET differs: 640x360 for one and
    // 2560x1440 for the other. Displacing the quarter-resolution pass moves whatever it feeds (it is drawn
    // from the same viewpoint every frame, so it is a screen-space input rather than a view) and gains
    // nothing.
    //
    // A pass counts as the main view when its target matches the swap chain's back buffer. Turn this off to
    // displace every pass, which is worth having as a switch precisely because the classification is a claim
    // about this build.
    void set_main_view_only(bool on);

    // AN ASYMMETRIC FRUSTUM PER EYE, which is the difference between side-by-side and a headset.
    //
    // The pass entry hardcodes the projection centre to (0,0), so this is applied AFTER the setup call
    // returns and before the draw: write the centre and let the engine's own builder recompose the
    // projection, the view-projection and the world-to-screen matrix together. Patching one of the three
    // would leave the others describing a different camera.
    //
    // `centre` is in half-view-plane units and is applied with OPPOSITE sign per eye, the way a real pair of
    // lenses is offset. Zero disables it, which is also the default.
    void set_frustum_centre(float centre_x, float centre_y);

    // ---- BOTH EYES IN ONE FRAME ------------------------------------------------------
    //
    // set_eye() renders ONE eye. This renders two, by repeating the pass group inside the target the engine
    // already opened -- which is the sequence the engine itself performs six times per cube map:
    //
    //     DrawScene(left)  ->  EndPass  ->  SetupPassPerspective(right)  ->  DrawScene(right)
    //
    // and the engine's own EndPass then closes the second pass, so the renderer's state machine ends where it
    // started (4 -> 3 -> 4 -> 3). Nothing about the frame or the render target is touched.
    //
    // COSTS A SECOND FULL SCENE DRAW, which is what stereo costs. Off by default for that reason as well as
    // the visual one.
    void set_stereo(bool on, float half_ipd, bool split_viewport);

    // Drives a pass setup WITHOUT passing through this mod's own override -- straight down the trampoline.
    // Public because rendering a second view means calling the engine's setup again from outside the engine,
    // and a caller doing that must not have its transform silently displaced a second time.
    //
    // GAME THREAD ONLY, and only between a BeginRenderTarget and its EndRenderTarget: it moves the renderer's
    // state machine. Returns false when the hook is not installed.
    // Name the world point that gets projected every main-view pass. Off until a point is set.
    // Start a fresh height-excursion window. A consumer measuring an oscillation resets, waits,
    // and reads -- the accumulation happens on the render thread in between.
    void reset_height_excursion();

    void set_probe_point(float x, float y, float z);
    void clear_probe_point();

    bool replay_setup(const regenny::LTNodeTransform& camera, const float fov[2], const float rect[4],
                      float depth_min, float depth_max);

    // Override the field of view the pass is set up with, in RADIANS, or a non-positive value to leave the
    // game's own value alone. Clamped by the engine to 179 degrees (SceneCamera::kMaxFovRadians), so a caller
    // asking for more silently gets that -- predicted_half_view_plane() will say so.
    void set_fov_override(float fov_x, float fov_y);

    // ---- MOVING THE EYE IN THE WORLD -----------------------------------------------------------
    //
    // A WORLD-SPACE delta added to the camera position of every eye, applied before the per-eye IPD
    // shift. World space, not camera-local like the IPD offset, because both of its users are about
    // where the head IS rather than which way it points:
    //
    //   ROOMSCALE -- the wearer's own movement inside their play space, which must not rotate with
    //   the head. Offsetting in the camera's frame would make leaning forward while looking down
    //   move the eye downward, which is not what the body did.
    //
    //   PINNING THE EYE HEIGHT -- the engine moves the camera vertically as the view pitches: 54 cm
    //   DOWN at -80 degrees, and a non-monotonic +9 / +7 / -0.3 cm going up. That is an artefact of
    //   the original FPS camera and it is nonsense in a headset, where the wearer's head has not
    //   moved at all. Cancelling it needs a world-space correction, since the error is vertical in
    //   the WORLD however the head happens to be oriented.
    // The camera the ENGINE handed the pass, captured before this mod touches it. The feedback-free
    // reference: a consumer correcting the camera cannot measure the error against the camera it has
    // already corrected, or it is measuring its own output.
    std::optional<std::array<float, 3>> pristine_camera_position() const;

    void set_position_offset(float x, float y, float z);
    std::array<float, 3> position_offset() const;

    // ---- WHAT THE PASSES IN A FRAME ACTUALLY ARE -------------------------------------
    //
    // The renderer issues MORE THAN ONE perspective pass per frame -- measured at roughly 390 setups per
    // second against ~300 presented frames. A stereo path that displaces every one of them is displacing
    // things that are not the world view, so "which pass is the main view" has to be answerable before an eye
    // offset can be applied selectively.
    //
    // The census is delimited by the frame boundary (RenderHook's present callback), so a "frame" here is
    // exactly the engine's own, not a guess based on timing.
    struct PassInfo {
        std::array<float, 2> fov{};
        std::array<float, 4> rect{};      // the NORMALISED rect the engine asked for
        std::array<int32_t, 4> viewport{};// the PIXEL viewport it derived
        std::array<float, 3> camera_position{};
        float depth_min{};
        float depth_max{};
    };

    static constexpr size_t kMaxPassesPerFrame = 8;

    // The passes of the last COMPLETED frame, in issue order. Empty until a frame has closed.
    //
    // Last completed rather than in-progress on purpose: a caller reading mid-frame would see a partial list
    // and could not tell that from a frame that genuinely had fewer passes.
    std::vector<PassInfo> passes_last_frame() const;

    // How many passes that frame had, and the largest seen since injection -- the two numbers that say
    // whether "one pass per frame" is ever safe to assume. It is not.
    uint32_t passes_in_last_frame() const;
    uint32_t max_passes_in_a_frame() const;

    struct Observed {
        bool hooked{};
        uintptr_t target{};
        uint64_t passes{};          // setup calls seen
        uint64_t overridden{};      // calls whose camera we replaced
        uint64_t rejected{};        // calls where the offset could not be applied (bad pose)
        uint64_t skipped_aux{};     // passes left alone because their target is not the back buffer
        bool main_view_only{};
        std::array<int32_t, 2> target_size{};  // the target bound when the last pass was set up
        std::array<float, 2> frustum_centre{}; // the requested per-eye centre offset
        std::array<float, 2> centre_applied{}; // what the record actually held after the last rebuild
        uint64_t rebuilds{};                   // matrix rebuilds performed
        // The record's own shear identity, evaluated IN PHASE inside the detour. Checked from the IPC thread
        // it is never determinable: the last pass of a frame is the ortho HUD pass, which builds differently.
        uint64_t centre_checked{};
        uint64_t centre_inconsistent{};

        // The ARGUMENTS the engine passed, captured in the detour. In-phase with the frame they configure,
        // unlike anything read from the record afterwards.
        std::array<float, 3> camera_position{};
        std::array<float, 4> camera_rotation{};
        std::array<float, 2> fov{};
        std::array<float, 4> rect{};
        float depth_min{};
        float depth_max{};

        // The PIXEL viewport the engine derived, read in-phase right after the setup call. A rect that was
        // substituted but had no effect shows up here as a full-screen viewport.
        std::array<int32_t, 4> viewport{};

        Eye eye{Eye::Off};
        bool stereo{};              // both eyes per frame
        uint64_t second_eye_draws{};// scenes drawn for the second eye
        uint64_t draw_calls{};      // DrawScene calls seen
        float half_ipd{};
        bool split_viewport{};
        std::array<float, 2> fov_override{};

        // A WORLD POINT PROJECTED IN-PHASE, i.e. while a PERSPECTIVE pass is configured. Reading the record
        // from another thread lands on the frame's last pass -- the full-screen ortho HUD pass -- where a
        // world point projects to roughly itself and says nothing about where the view is looking. Measured
        // here, a stationary world point sliding across the screen is direct evidence the rendered view
        // turned, with no screenshot involved.
        // ---- CAMERA HEIGHT EXCURSION, ACCUMULATED IN PHASE -------------------------
        //
        // WHY THIS EXISTS: the published `camera_position` is a snapshot of the LAST pass, and the
        // pass runs on the render thread's schedule. Sampling it from the IPC thread to measure an
        // OSCILLATION -- head bob, for instance -- aliases it: reads faster than the pass rate
        // return the same value repeatedly, and two different walks produced byte-identical
        // "measurements" that way. Amplitude has to be accumulated where the samples are.
        //
        // Tracks the min and max of the camera's Y across every main-view pass since the last
        // reset, so peak-to-peak is a real excursion over the window rather than the difference
        // between two arbitrary snapshots.
        float height_min{};
        float height_max{};
        uint32_t height_samples{};

        bool probe_projected{};
        std::array<float, 3> probe_point{};
        std::array<float, 2> probe_pixel{};
    };

    Observed observed() const;

private:
    CameraPassHook() = default;
};
