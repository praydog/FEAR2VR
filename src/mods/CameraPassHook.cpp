#include "CameraPassHook.hpp"

#include "VR.hpp"

#include <atomic>
#include <cinttypes>
#include <cstring>

#include "sdk/Render.hpp"
#include "sdk/SceneCamera.hpp"

#include "Hooks.hpp"
#include "SceneTarget.hpp"
#include "FrameCapture.hpp"
#include "RenderHook.hpp"
#include "Log.hpp"

namespace {

constexpr const char* kHookName = "CLTRenderer::SetupPassPerspective";

std::atomic<uint64_t> g_passes{0};
std::atomic<float> g_h_min{0.0f};
std::atomic<float> g_h_max{0.0f};
std::atomic<uint32_t> g_h_samples{0};

std::atomic<bool> g_probe_on{false};
std::atomic<float> g_probe_pt[3]{{0.0f}, {0.0f}, {0.0f}};
std::atomic<float> g_probe_px{0.0f};
std::atomic<float> g_probe_py{0.0f};
std::atomic<bool> g_probe_ok{false};
std::atomic<uint64_t> g_overridden{0};
std::atomic<uint64_t> g_rejected{0};
std::atomic<uint8_t> g_eye{static_cast<uint8_t>(CameraPassHook::Eye::Off)};
std::atomic<float> g_half_ipd{0.0f};
std::atomic<float> g_pos_off_x{0.0f};
std::atomic<float> g_pos_off_y{0.0f};
std::atomic<float> g_pos_off_z{0.0f};
std::atomic<bool> g_split{false};
std::atomic<float> g_fov_x{0.0f};
std::atomic<float> g_fov_y{0.0f};
std::atomic<uintptr_t> g_target{0};
std::atomic<bool> g_stereo{false};
std::atomic<bool> g_main_only{true};
std::atomic<uint64_t> g_skipped_aux{0};
std::atomic<int32_t> g_target_size[2]{};
std::atomic<float> g_centre_x{0.0f};
std::atomic<float> g_centre_y{0.0f};
std::atomic<float> g_centre_applied[2]{};
std::atomic<uint64_t> g_rebuilds{0};
std::atomic<uint64_t> g_centre_checked{0};
std::atomic<uint64_t> g_centre_inconsistent{0};
std::atomic<uint64_t> g_second_draws{0};
std::atomic<uint64_t> g_draw_calls{0};

// ---- WHICH POSE THIS FRAME IS ACTUALLY BEING DRAWN FROM ----------------------------------------
//
// Latched where the PRISTINE camera is taken, because that transform already carries the head
// rotation the engine thread wrote -- build_eye only adds IPD and the position offset. So this is
// the sequence the view matrix genuinely corresponds to.
//
// FrameCapture stamps the published frame from VR::last_host_sequence() at capture-issue instead,
// which is LATER and on the render thread while the engine thread keeps ingesting. If the two
// disagree, the projection layer claims a pose the image was not rendered from and the compositor
// corrects by the difference -- judder proportional to head speed, with every pose counter on the
// host still reading a clean hit.
std::atomic<uint32_t> g_view_seq{0};

// THE ROTATION THE MAIN VIEW IS BEING DRAWN WITH -- separate from the g_rot reporting copy, which
// the setup detour writes for EVERY pass. In a place that draws a render-to-texture (a monitor or
// camera feed) g_rot may hold the RTT's own camera, and comparing that against the head reports a
// mismatch of tens of degrees that never happened. Written only where we actually override the
// main view, so it is always an eye.
std::atomic<float> g_view_rot[4]{};
std::atomic<uint32_t> g_view_tid{0};
std::atomic<uintptr_t> g_draw_target{0};
std::atomic<uintptr_t> g_endpass_fn{0};

constexpr const char* kDrawHookName = "CLTRenderer::DrawScene";

// THE PRISTINE SETUP, captured before any override is applied. The second eye must be derived from what the
// ENGINE asked for, not from the displaced pose we handed it -- deriving eye B from eye A would separate the
// views by two IPDs and put the centre in the wrong place.
//
// Written only from the setup detour and read only from the draw detour, both on the render thread and in that
// order within a frame, so a plain struct behind a validity flag is sufficient.
struct PristineSetup {
    regenny::LTNodeTransform camera{};
    float fov[2]{};
    float rect[4]{};
    float depth_min{};
    float depth_max{};
};
PristineSetup g_pristine{};
std::atomic<bool> g_pristine_valid{false};
// Whether the setup we kept was the MAIN VIEW. The second eye is only drawn for that one -- replaying a
// quarter-resolution auxiliary pass would draw a second copy of something that is not a view.
std::atomic<bool> g_pristine_main{false};

// ---- THE MAIN VIEW'S OWN COPY -------------------------------------------------------------------
//
// g_pristine belongs to whichever pass set up LAST, and that is not necessarily the pass being
// drawn. Where the frame contains a second view -- a monitor, a camera feed -- an auxiliary setup
// landing between the main view's setup and its draw replaces the transform the second eye is
// built from. The replay then either uses the wrong camera or is skipped entirely, leaving the
// right half of the split holding the PREVIOUS frame. Once per frame, intermittently, and only in
// places that draw a second view.
PristineSetup g_pristine_main_copy{};
std::atomic<bool> g_pristine_main_valid{false};
std::atomic<uint64_t> g_pristine_clobbered{0};

// The engine's own view matrix against the camera we handed it -- the only non-circular check of
// whether the frame was rendered from the pose we say it was.
std::atomic<uint64_t> g_view_check_samples{0};
std::atomic<uint64_t> g_view_mismatch_frames{0};
// WHICH EYE the engine disagreed on. The left is the engine's own pass that we override in place;
// the right is our replay, set up by us after closing the engine's. If an auxiliary pass in the
// same frame is disturbing one of them, they will not disagree at the same rate -- and that says
// whether the corruption is in the engine's pass or in ours.
std::atomic<uint64_t> g_view_mismatch_left{0};
std::atomic<uint64_t> g_view_mismatch_right{0};
// And whether an auxiliary pass had already run this frame when the disagreement happened.
std::atomic<uint32_t> g_aux_passes_this_frame{0};
std::atomic<uint64_t> g_mismatch_after_aux{0};
// HOW MANY PASSES CALLED THEMSELVES THE MAIN VIEW this frame. Exactly ONE: the engine's own left
// eye. The replayed right eye goes straight down the trampoline on purpose -- routing it through
// this detour would displace the transform twice -- so it is never seen here. I guessed two and the
// census said one, which is the answer being correct rather than the check being broken.
//
// More than one means the auxiliary pass was misclassified,
// which would write ITS camera into g_view_rot and make the camera's rotation delta disagree with
// the head's about an AXIS while still matching in MAGNITUDE. That is precisely the shape of the
// error being measured, so it has to be excluded before the measurement means anything.
uint32_t g_main_this_frame = 0;  // render thread only
std::atomic<uint64_t> g_frames_two_main{0};
std::atomic<uint64_t> g_frames_other_main{0};
std::atomic<uint32_t> g_main_worst{0};
std::atomic<float> g_view_mismatch_worst{0.0f};
std::atomic<uint64_t> g_second_eye_skipped{0};

// Last arguments seen, published for diagnostics. Plain floats behind an atomic sequence would be tidier, but
// these are read for reporting rather than for control, and a torn pair here misleads nobody.
std::atomic<float> g_pos[3]{};
std::atomic<float> g_rot[4]{};
std::atomic<float> g_fov_seen[2]{};
std::atomic<float> g_rect_seen[4]{};
std::atomic<float> g_depth_min{0.0f};
std::atomic<float> g_depth_max{0.0f};

// THE VIEWPORT THE ENGINE DERIVED, read from the record immediately after the setup call returns. In phase
// with the pass it describes, unlike a read from the IPC thread -- which lands on whichever pass ran last and
// is usually the full-screen ortho HUD pass.
//
// This is the observable that settles whether a substituted rect actually took effect. Without it the only
// evidence was a screenshot, and a dark corridor looks a lot like a clipped viewport.
std::atomic<int32_t> g_vp[4]{};

// ---- THE PER-FRAME CENSUS ----------------------------------------------------------------------------
//
// Written by the setup detour and rotated by the present callback, both on the render thread, so the only
// synchronisation needed is against a reader on the IPC thread: `published_count` is released last and
// acquired first, and the buffers are double-buffered so a reader never walks the list being filled.
CameraPassHook::PassInfo g_frame_passes[2][CameraPassHook::kMaxPassesPerFrame]{};
std::atomic<uint32_t> g_frame_slot{0};       // which buffer the CURRENT frame is filling
uint32_t g_filling_count = 0;                 // render-thread only
std::atomic<uint32_t> g_published_count{0};
std::atomic<uint32_t> g_published_slot{0};
std::atomic<uint32_t> g_max_passes{0};

// Appends this pass to the frame being censused. Render thread only.
void record_pass(const regenny::LTNodeTransform* camera, const float* fov, const float* rect,
                 float depth_min, float depth_max) {
    if (g_filling_count >= CameraPassHook::kMaxPassesPerFrame) {
        return;  // a frame with more passes than this is itself the finding; the count still rises
    }
    auto& p = g_frame_passes[g_frame_slot.load(std::memory_order_relaxed)][g_filling_count];
    p = {};
    if (fov != nullptr) {
        p.fov = {fov[0], fov[1]};
    }
    if (rect != nullptr) {
        p.rect = {rect[0], rect[1], rect[2], rect[3]};
    }
    if (camera != nullptr) {
        p.camera_position = {camera->position.x, camera->position.y, camera->position.z};
    }
    p.depth_min = depth_min;
    p.depth_max = depth_max;
    for (size_t i = 0; i < 4; ++i) {
        p.viewport[i] = g_vp[i].load(std::memory_order_relaxed);
    }
    ++g_filling_count;
}

// Closes the census at the frame boundary and swaps buffers. Registered as a RenderHook present callback, so
// "a frame" is the engine's own rather than a timing guess.
void close_frame_census() {
    const uint32_t n = g_filling_count;
    if (n > g_max_passes.load(std::memory_order_relaxed)) {
        g_max_passes.store(n, std::memory_order_relaxed);
    }
    const uint32_t slot = g_frame_slot.load(std::memory_order_relaxed);
    g_published_slot.store(slot, std::memory_order_relaxed);
    g_published_count.store(n, std::memory_order_release);
    g_frame_slot.store(slot ^ 1u, std::memory_order_relaxed);
    g_filling_count = 0;
    // Per-FRAME, so "an auxiliary pass had already run when the engine disagreed" means this frame
    // rather than at any point since injection. Reset at the boundary, which is what this callback
    // already is.
    g_aux_passes_this_frame.store(0, std::memory_order_relaxed);
    if (g_main_this_frame == 1) {
        g_frames_two_main.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_frames_other_main.fetch_add(1, std::memory_order_relaxed);
    }
    g_main_this_frame = 0;
}

// Applies the per-eye projection centre AFTER the engine has built its matrices, then has the engine rebuild
// them. The pass entry cannot express an off-centre frustum, so this is the only route to one.
//
// Opposite sign per eye, matching how a real pair of lenses sits either side of the panel centre.
void apply_frustum_centre(CameraPassHook::Eye eye) {
    const float cx = g_centre_x.load(std::memory_order_relaxed);
    const float cy = g_centre_y.load(std::memory_order_relaxed);
    if (cx == 0.0f && cy == 0.0f) {
        return;
    }
    const float sign = eye == CameraPassHook::Eye::Left ? -1.0f : 1.0f;
    if (sdk::SceneCamera::set_projection_centre(sign * cx, cy)) {
        g_rebuilds.fetch_add(1, std::memory_order_relaxed);
        if (const auto got = sdk::SceneCamera::projection_centre()) {
            g_centre_applied[0].store((*got)[0], std::memory_order_relaxed);
            g_centre_applied[1].store((*got)[1], std::memory_order_relaxed);
        }
        // IN PHASE, and it has to be. The record is a PERSPECTIVE one only while a perspective pass is
        // configured; by the time the IPC thread reads it the last pass of the frame is the full-screen ortho
        // HUD pass, and the shear identity does not apply to that. Evaluated from the IPC thread the check
        // simply answered "not determinable" every time.
        if (const auto ok = sdk::SceneCamera::projection_centre_is_consistent()) {
            g_centre_checked.fetch_add(1, std::memory_order_relaxed);
            if (!*ok) {
                g_centre_inconsistent.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
}

// Reads the pixel viewport the engine just derived from the rect it was handed.
// Project the caller's world point through the record THAT THIS PASS JUST CONFIGURED. Only meaningful while
// a perspective pass is live, which is why it runs here rather than off an IPC read.
// Accumulate the camera height for THIS pass. In phase by construction: it runs inside the pass
// entry, so every pass contributes exactly one sample and nothing is aliased away.
void accumulate_height(bool is_main_view, const regenny::LTNodeTransform* camera) {
    if (!is_main_view || camera == nullptr) {
        return;
    }
    const float y = camera->position.y;
    if (g_h_samples.fetch_add(1, std::memory_order_relaxed) == 0) {
        g_h_min.store(y, std::memory_order_relaxed);
        g_h_max.store(y, std::memory_order_relaxed);
        return;
    }
    float lo = g_h_min.load(std::memory_order_relaxed);
    while (y < lo && !g_h_min.compare_exchange_weak(lo, y, std::memory_order_relaxed)) {
    }
    float hi = g_h_max.load(std::memory_order_relaxed);
    while (y > hi && !g_h_max.compare_exchange_weak(hi, y, std::memory_order_relaxed)) {
    }
}

void project_probe(bool is_main_view) {
    if (!is_main_view || !g_probe_on.load(std::memory_order_relaxed)) {
        return;
    }
    const auto scam = sdk::SceneCamera::snapshot();
    if (!scam || !scam->is_perspective_projection()) {
        g_probe_ok.store(false, std::memory_order_relaxed);
        return;
    }
    const auto p = scam->project_point(g_probe_pt[0].load(std::memory_order_relaxed),
                                       g_probe_pt[1].load(std::memory_order_relaxed),
                                       g_probe_pt[2].load(std::memory_order_relaxed));
    if (!p) {
        g_probe_ok.store(false, std::memory_order_relaxed);
        return;
    }
    g_probe_px.store(p->x, std::memory_order_relaxed);
    g_probe_py.store(p->y, std::memory_order_relaxed);
    g_probe_ok.store(true, std::memory_order_relaxed);
}

void capture_viewport() {
    if (const auto snap = sdk::SceneCamera::snapshot()) {
        g_vp[0].store(snap->viewport_left, std::memory_order_relaxed);
        g_vp[1].store(snap->viewport_top, std::memory_order_relaxed);
        g_vp[2].store(snap->viewport_right, std::memory_order_relaxed);
        g_vp[3].store(snap->viewport_bottom, std::memory_order_relaxed);
    }
}

// __stdcall, five stack arguments, `retn 14h`. NOT __thiscall despite being vtable slot 15: the wrapper loads
// ecx with g_SceneRenderer itself and never reads an incoming this.
using SetupFn = char(__stdcall*)(const regenny::LTNodeTransform*, const float*, const float*, float, float);

char __stdcall setup_pass_detour(const regenny::LTNodeTransform* camera, const float* fov, const float* rect,
                                 float depth_min, float depth_max) {
    g_passes.fetch_add(1, std::memory_order_relaxed);

    // CAPTURE FIRST, and capture the ARGUMENTS. These are in phase with the frame they configure; the record
    // read from the IPC thread afterwards is not, which is the distinction this project keeps relearning.
    if (camera != nullptr) {
        g_pos[0].store(camera->position.x, std::memory_order_relaxed);
        g_pos[1].store(camera->position.y, std::memory_order_relaxed);
        g_pos[2].store(camera->position.z, std::memory_order_relaxed);
        g_rot[0].store(camera->rotation.x, std::memory_order_relaxed);
        g_rot[1].store(camera->rotation.y, std::memory_order_relaxed);
        g_rot[2].store(camera->rotation.z, std::memory_order_relaxed);
        g_rot[3].store(camera->rotation.w, std::memory_order_relaxed);
    }
    if (fov != nullptr) {
        g_fov_seen[0].store(fov[0], std::memory_order_relaxed);
        g_fov_seen[1].store(fov[1], std::memory_order_relaxed);
    }
    if (rect != nullptr) {
        for (size_t i = 0; i < 4; ++i) {
            g_rect_seen[i].store(rect[i], std::memory_order_relaxed);
        }
    }
    g_depth_min.store(depth_min, std::memory_order_relaxed);
    g_depth_max.store(depth_max, std::memory_order_relaxed);

    // WHICH TARGET IS BOUND, read before deciding anything. This is the only thing that distinguishes the
    // main view from the quarter-resolution auxiliary pass -- their arguments are identical.
    bool is_main_view = true;
    if (const auto ts = sdk::SceneCamera::current_target_size()) {
        g_target_size[0].store((*ts)[0], std::memory_order_relaxed);
        g_target_size[1].store((*ts)[1], std::memory_order_relaxed);
        int32_t mw = 0;
        int32_t mh = 0;
        if (SceneTarget::main_view_size(mw, mh)) {
            // Supersampling moved the scene off the back buffer, so the back buffer's size no
            // longer describes it.
            is_main_view = (*ts)[0] == mw && (*ts)[1] == mh;
        } else if (const auto pp = sdk::Render::present_params()) {
            is_main_view = (*ts)[0] == static_cast<int32_t>(pp->BackBufferWidth) &&
                           (*ts)[1] == static_cast<int32_t>(pp->BackBufferHeight);
        }
    }

    // ---- LATCH THE POSE HERE, NOT AT THE TOP OF THE UPDATE -------------------------------------
    //
    // The pristine copy below is what both eyes are built from, so correcting it here corrects the
    // whole frame -- and here is as late as this side of the pipeline gets: the game logic has run,
    // the draw is about to be issued.
    //
    // Applied to the camera BEFORE the pristine copy is taken, because the eye offset is computed
    // from the rotation and would otherwise displace the eyes along the stale axis.
    regenny::LTNodeTransform latched{};
    if (is_main_view && camera != nullptr) {
        if (const auto d = VR::get().late_latch_head()) {
            latched = *camera;
            const auto& q = latched.rotation;
            // camera = head * aim, so replacing the head is a LEFT multiply by the delta.
            const float x = (*d)[3] * q.x + (*d)[0] * q.w + (*d)[1] * q.z - (*d)[2] * q.y;
            const float y = (*d)[3] * q.y - (*d)[0] * q.z + (*d)[1] * q.w + (*d)[2] * q.x;
            const float z = (*d)[3] * q.z + (*d)[0] * q.y - (*d)[1] * q.x + (*d)[2] * q.w;
            const float w = (*d)[3] * q.w - (*d)[0] * q.x - (*d)[1] * q.y - (*d)[2] * q.z;
            latched.rotation.x = x;
            latched.rotation.y = y;
            latched.rotation.z = z;
            latched.rotation.w = w;
            camera = &latched;

            // THE FRAME'S POSE IS DECIDED HERE, so this is where it is committed.
            //
            // `camera` now holds the head pose that will build this pass's matrices, and this pass
            // runs once per rendered frame (cp_frames_2main measured 1:1 with frames). Committing
            // at HeadTracking's composition instead was wrong: that is a WRITER into the camera
            // holder, running at its own cadence rather than the renderer's, so frames between two
            // compositions were all stamped with the older pose -- late by a frame or two and
            // jittery rather than uniformly behind, which is what it felt like.
            //
            // The value stays the exact quaternion the host sent. Only the instant comes from here.
            VR::commit_wire_head_pose();
        }
    }

    if (!is_main_view) {
        g_aux_passes_this_frame.fetch_add(1, std::memory_order_relaxed);
    } else {
        ++g_main_this_frame;
        if (g_main_this_frame > g_main_worst.load(std::memory_order_relaxed)) {
            g_main_worst.store(g_main_this_frame, std::memory_order_relaxed);
        }
    }

    // THE PRISTINE COPY for a second eye, taken before any override touches it.
    if (camera != nullptr && fov != nullptr && rect != nullptr) {
        g_pristine.camera = *camera;
        g_pristine.fov[0] = fov[0];
        g_pristine.fov[1] = fov[1];
        memcpy(g_pristine.rect, rect, sizeof(g_pristine.rect));
        g_pristine.depth_min = depth_min;
        g_pristine.depth_max = depth_max;
        g_pristine_main.store(is_main_view, std::memory_order_relaxed);
        g_pristine_valid.store(true, std::memory_order_release);
        if (is_main_view) {
            g_pristine_main_copy = g_pristine;
            g_pristine_main_valid.store(true, std::memory_order_release);
        }
    }

    auto* hook = Hooks::get().find(kHookName);
    if (hook == nullptr) {
        return 0;
    }
    const auto original = hook->original<SetupFn>();

    // In stereo the first pass IS the left eye; set_eye's single-eye mode is the other way in.
    auto eye = static_cast<CameraPassHook::Eye>(g_eye.load(std::memory_order_relaxed));
    if (g_stereo.load(std::memory_order_relaxed)) {
        eye = CameraPassHook::Eye::Left;
    }
    if (!is_main_view && g_main_only.load(std::memory_order_relaxed)) {
        g_skipped_aux.fetch_add(1, std::memory_order_relaxed);
        eye = CameraPassHook::Eye::Off;
    }

    if (eye == CameraPassHook::Eye::Off || camera == nullptr) {
        const char r = original(camera, fov, rect, depth_min, depth_max);
        capture_viewport();
        project_probe(is_main_view);
    accumulate_height(is_main_view, camera);
        accumulate_height(is_main_view, camera);
        record_pass(camera, fov, rect, depth_min, depth_max);
        return r;
    }

    // SUBSTITUTED COPIES, never a write through the engine's pointers. The caller owns that storage and may
    // reuse it after this returns; mutating it would be a side effect outside the frame we were asked about.
    const float sign = eye == CameraPassHook::Eye::Left ? -1.0f : 1.0f;
    auto shifted = sdk::SceneCamera::offset_transform_local(
        *camera, sign * g_half_ipd.load(std::memory_order_relaxed), 0.0f, 0.0f);

    if (shifted.has_value()) {
        // WORLD space, so it survives however the head is turned -- see set_position_offset().
        shifted->position.x += g_pos_off_x.load(std::memory_order_relaxed);
        shifted->position.y += g_pos_off_y.load(std::memory_order_relaxed);
        shifted->position.z += g_pos_off_z.load(std::memory_order_relaxed);
    }

    if (!shifted.has_value()) {
        // A pose whose rotation is not usable as one. Passing the original through is the only safe answer --
        // rendering an eye from a garbage transform is worse than rendering it monocular.
        g_rejected.fetch_add(1, std::memory_order_relaxed);
        return original(camera, fov, rect, depth_min, depth_max);
    }

    float fov_local[2] = {fov != nullptr ? fov[0] : 0.0f, fov != nullptr ? fov[1] : 0.0f};
    const float ox = g_fov_x.load(std::memory_order_relaxed);
    const float oy = g_fov_y.load(std::memory_order_relaxed);
    if (ox > 0.0f) {
        fov_local[0] = ox;
    }
    if (oy > 0.0f) {
        fov_local[1] = oy;
    }

    float rect_local[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    if (rect != nullptr) {
        memcpy(rect_local, rect, sizeof(rect_local));
    }
    if (g_split.load(std::memory_order_relaxed)) {
        // Halve the horizontal extent of whatever the game asked for, rather than assuming full width: the
        // engine already uses sub-rects, and clobbering one would break a pass we did not intend to touch.
        const float left = rect_local[0];
        const float right = rect_local[2];
        const float mid = left + (right - left) * 0.5f;
        if (eye == CameraPassHook::Eye::Left) {
            rect_local[2] = mid;
        } else {
            rect_local[0] = mid;
        }
    }

    g_overridden.fetch_add(1, std::memory_order_relaxed);
    if (is_main_view) {
        // Position offsets above do not touch rotation, so this is the pristine view rotation.
        g_view_rot[0].store(shifted->rotation.x, std::memory_order_relaxed);
        g_view_rot[1].store(shifted->rotation.y, std::memory_order_relaxed);
        g_view_rot[2].store(shifted->rotation.z, std::memory_order_relaxed);
        g_view_rot[3].store(shifted->rotation.w, std::memory_order_relaxed);
    }
    const char r = original(&shifted.value(), fov_local, rect_local, depth_min, depth_max);
    apply_frustum_centre(eye);
    capture_viewport();

    // ---- WHAT DID THE ENGINE ACTUALLY TAKE? ----------------------------------------------------
    //
    // Every other check in this project compares our INPUTS to each other -- the pose we ingested
    // against the camera we built from it -- which is circular and answers 0.000 by construction.
    // This reads the engine's OWN view matrix back out of its record, inside the pass, and asks
    // whether it is the one we just handed over.
    //
    // In the pass because the record is only a perspective one while a perspective pass is
    // configured; from anywhere else it describes whatever ran last. That has caught two
    // diagnostics in this project already.
    //
    // A divergence means the engine rendered from a camera other than ours -- a script, a shake, a
    // recompute from player state -- which is a game-to-render discrepancy that no amount of
    // checking our own bookkeeping could ever see.
    if (is_main_view) {
        if (const auto snap = sdk::SceneCamera::snapshot()) {
            if (const auto want = sdk::SceneCamera::view_matrix_from_pose(shifted.value())) {
                const auto got_rot = sdk::SceneCamera::rotation_from_matrix(snap->view);
                const auto want_rot = sdk::SceneCamera::rotation_from_matrix(*want);
                if (got_rot.has_value() && want_rot.has_value()) {
                    float d = got_rot->x * want_rot->x + got_rot->y * want_rot->y +
                              got_rot->z * want_rot->z + got_rot->w * want_rot->w;
                    d = d < 0.0f ? -d : d;
                    d = d > 1.0f ? 1.0f : d;
                    const float deg = 2.0f * acosf(d) * 57.2957795f;
                    g_view_check_samples.fetch_add(1, std::memory_order_relaxed);
                    // 0.05 was BELOW THE ARITHMETIC. Matrix to quaternion and back does not
                    // round-trip exactly, and the worst disagreement ever measured was 0.079 deg
                    // -- reported as "the engine rendered from a different camera on 2% of
                    // passes", which was precision, not a camera. A real substitution would be
                    // degrees, so the threshold belongs where a real one would land.
                    if (deg > 0.5f) {
                        g_view_mismatch_frames.fetch_add(1, std::memory_order_relaxed);
                        if (eye == CameraPassHook::Eye::Left) {
                            g_view_mismatch_left.fetch_add(1, std::memory_order_relaxed);
                        } else {
                            g_view_mismatch_right.fetch_add(1, std::memory_order_relaxed);
                        }
                        if (g_aux_passes_this_frame.load(std::memory_order_relaxed) > 0) {
                            g_mismatch_after_aux.fetch_add(1, std::memory_order_relaxed);
                        }
                    }
                    float worst = g_view_mismatch_worst.load(std::memory_order_relaxed);
                    while (deg > worst && !g_view_mismatch_worst.compare_exchange_weak(
                                              worst, deg, std::memory_order_relaxed)) {
                    }
                }
            }
        }
    }
    project_probe(is_main_view);
    record_pass(&shifted.value(), fov_local, rect_local, depth_min, depth_max);
    return r;
}

// Applies the eye displacement and viewport split to a pristine setup. Shared by the setup detour and the
// second-eye replay so the two eyes are constructed by identical code rather than mirrored by hand.
bool build_eye(const PristineSetup& in, CameraPassHook::Eye eye, regenny::LTNodeTransform& cam_out,
               float rect_out[4]) {
    const float sign = eye == CameraPassHook::Eye::Left ? -1.0f : 1.0f;
    auto shifted = sdk::SceneCamera::offset_transform_local(
        in.camera, sign * g_half_ipd.load(std::memory_order_relaxed), 0.0f, 0.0f);
    if (!shifted.has_value()) {
        return false;
    }

    shifted->position.x += g_pos_off_x.load(std::memory_order_relaxed);
    shifted->position.y += g_pos_off_y.load(std::memory_order_relaxed);
    shifted->position.z += g_pos_off_z.load(std::memory_order_relaxed);
    cam_out = *shifted;
    memcpy(rect_out, in.rect, sizeof(in.rect[0]) * 4);
    if (g_split.load(std::memory_order_relaxed)) {
        const float mid = rect_out[0] + (rect_out[2] - rect_out[0]) * 0.5f;
        if (eye == CameraPassHook::Eye::Left) {
            rect_out[2] = mid;
        } else {
            rect_out[0] = mid;
        }
    }
    return true;
}

// __stdcall(a1, a2), retn 8.
using DrawFn = char(__stdcall*)(void*, void*);
using EndPassFn = char(*)();

char __stdcall draw_scene_detour(void* a1, void* a2) {
    g_draw_calls.fetch_add(1, std::memory_order_relaxed);
    g_view_seq.store(VR::get().last_host_sequence(), std::memory_order_release);
    g_view_tid.store(::GetCurrentThreadId(), std::memory_order_relaxed);

    auto* draw_hook = Hooks::get().find(kDrawHookName);
    if (draw_hook == nullptr) {
        return 0;
    }
    const auto draw_original = draw_hook->original<DrawFn>();

    // The engine's own draw, which in stereo has already been set up as the LEFT eye.
    const char first = draw_original(a1, a2);

    if (!g_stereo.load(std::memory_order_relaxed) ||
        !g_pristine_valid.load(std::memory_order_acquire)) {
        return first;
    }
    // WHETHER THIS DRAW IS THE MAIN VIEW IS A PROPERTY OF THIS DRAW, asked now rather than
    // inherited from whichever setup happened to run last. The stale flag is what an intervening
    // auxiliary pass corrupts.
    bool draw_is_main = g_pristine_main.load(std::memory_order_relaxed);
    if (const auto ts = sdk::SceneCamera::current_target_size()) {
        if (const auto pp = sdk::Render::present_params()) {
            draw_is_main = (*ts)[0] == static_cast<int32_t>(pp->BackBufferWidth) &&
                           (*ts)[1] == static_cast<int32_t>(pp->BackBufferHeight);
            int32_t dw = 0;
            int32_t dh = 0;
            if (SceneTarget::main_view_size(dw, dh)) {
                draw_is_main = (*ts)[0] == dw && (*ts)[1] == dh;
            }
        }
    }
    if (draw_is_main && !g_pristine_main.load(std::memory_order_relaxed)) {
        // The main view is being drawn but the pristine copy belongs to something else: an
        // auxiliary setup landed in between. THIS is the frame that would have gone out mono.
        g_pristine_clobbered.fetch_add(1, std::memory_order_relaxed);
    }
    if (g_main_only.load(std::memory_order_relaxed) && !draw_is_main) {
        g_second_eye_skipped.fetch_add(1, std::memory_order_relaxed);
        return first;  // the pass just drawn was the auxiliary one; it gets no second eye
    }
    if (!g_pristine_main_valid.load(std::memory_order_acquire)) {
        g_second_eye_skipped.fetch_add(1, std::memory_order_relaxed);
        return first;
    }

    auto* setup_hook = Hooks::get().find(kHookName);
    const auto end_pass = reinterpret_cast<EndPassFn>(g_endpass_fn.load(std::memory_order_relaxed));
    if (setup_hook == nullptr || end_pass == nullptr) {
        return first;
    }

    regenny::LTNodeTransform cam{};
    float rect[4]{};
    if (!build_eye(g_pristine_main_copy, CameraPassHook::Eye::Right, cam, rect)) {
        return first;
    }

    // THE SECOND EYE. Close the pass the engine opened, configure another from the pristine setup, and draw
    // again. The engine's own EndPass then closes THIS one, so the state machine ends where it began:
    // 4 -> (EndPass) 3 -> (SetupPass) 4 -> (engine's EndPass) 3.
    //
    // Straight down the trampolines: going through our own detours would displace the transform twice and
    // recurse into this function.
    if (end_pass() == 0) {
        return first;  // not in a configured pass -- leave the frame exactly as the engine had it
    }
    float fov[2] = {g_pristine_main_copy.fov[0], g_pristine_main_copy.fov[1]};
    const float ox = g_fov_x.load(std::memory_order_relaxed);
    const float oy = g_fov_y.load(std::memory_order_relaxed);
    if (ox > 0.0f) { fov[0] = ox; }
    if (oy > 0.0f) { fov[1] = oy; }

    if (setup_hook->original<SetupFn>()(&cam, fov, rect, g_pristine_main_copy.depth_min, g_pristine_main_copy.depth_max) == 0) {
        return first;
    }
    apply_frustum_centre(CameraPassHook::Eye::Right);

    // RECORD THE SECOND EYE TOO. The replay goes straight down the trampoline, so until now it
    // never reached capture_viewport() or record_pass() -- the census reported TWO passes in a
    // frame where three setups had happened, and the one pass a stereo bug would live in was the
    // invisible one. A diagnostic that omits the interesting case is how the split defect stayed
    // unexplained for two sessions.
    capture_viewport();
    record_pass(&cam, fov, rect, g_pristine_main_copy.depth_min, g_pristine_main_copy.depth_max);

    draw_original(a1, a2);
    g_second_draws.fetch_add(1, std::memory_order_relaxed);

    // A capture aimed at THIS stage reads the back buffer here, before anything downstream of the
    // scene draw can touch it. That is the difference between "the right eye drew wrong" and
    // "something later overwrote it", which the finished frame cannot distinguish.
    if (FrameCapture::get().stage() == FrameCapture::Stage::AfterSecondEye) {
        FrameCapture::get().service_now();
    }
    return first;
}

}  // namespace

CameraPassHook& CameraPassHook::get() {
    static CameraPassHook s_instance;
    return s_instance;
}

std::optional<std::string> CameraPassHook::on_initialize() {
    const uintptr_t target = sdk::SceneCamera::renderer_fn(sdk::SceneCamera::RendererSlot::SetupPassPerspective);
    g_target.store(target, std::memory_order_relaxed);
    if (target == 0) {
        LOGX("[camerapass] SetupPassPerspective did not resolve -- no stereo intervention point");
        return std::string{"CLTRenderer slot 15 did not resolve"};
    }
    if (!Hooks::get().install(kHookName, reinterpret_cast<void*>(target),
                              reinterpret_cast<void*>(&setup_pass_detour))) {
        return std::string{"failed to hook CLTRenderer::SetupPassPerspective"};
    }
    LOGX("[camerapass] perspective pass hooked at 0x%08" PRIXPTR " (observing)", target);

    // DrawScene and EndPass, which together with the setup make a second eye possible. A missing one is not
    // fatal -- single-eye override still works -- so this degrades rather than failing initialize().
    // The frame boundary delimits the census. RenderHook already owns that hook, so this uses its extension
    // point rather than installing a second one on the same function.
    if (!RenderHook::get().add_present_callback(&close_frame_census)) {
        LOGX("[camerapass] could not register the frame-boundary callback -- census unavailable");
    }

    const uintptr_t draw = sdk::SceneCamera::renderer_fn(sdk::SceneCamera::RendererSlot::DrawScene);
    const uintptr_t endp = sdk::SceneCamera::renderer_fn(sdk::SceneCamera::RendererSlot::EndPass);
    g_draw_target.store(draw, std::memory_order_relaxed);
    g_endpass_fn.store(endp, std::memory_order_relaxed);
    if (draw != 0 && endp != 0) {
        if (Hooks::get().install(kDrawHookName, reinterpret_cast<void*>(draw),
                                 reinterpret_cast<void*>(&draw_scene_detour))) {
            LOGX("[camerapass] DrawScene hooked at 0x%08" PRIXPTR ", EndPass at 0x%08" PRIXPTR
                 " -- both eyes available", draw, endp);
        } else {
            LOGX("[camerapass] DrawScene hook FAILED -- single-eye override only");
        }
    } else {
        LOGX("[camerapass] DrawScene/EndPass unresolved -- single-eye override only");
    }
    return std::nullopt;
}

void CameraPassHook::on_shutdown() {
    // Stop overriding before the hooks retire, so the last frames the engine renders on its way out are its
    // own. Hook removal is Hooks::retire()'s job; this is about what the player sees.
    g_eye.store(static_cast<uint8_t>(Eye::Off), std::memory_order_relaxed);
    g_stereo.store(false, std::memory_order_relaxed);
}

void CameraPassHook::set_eye(Eye eye, float half_ipd, bool split_viewport) {
    g_half_ipd.store(half_ipd, std::memory_order_relaxed);
    g_split.store(split_viewport, std::memory_order_relaxed);
    g_eye.store(static_cast<uint8_t>(eye), std::memory_order_release);
    LOGX("[camerapass] eye=%u half_ipd=%.3f split=%d", static_cast<unsigned>(eye), half_ipd,
         split_viewport ? 1 : 0);
}

void CameraPassHook::set_stereo(bool on, float half_ipd, bool split_viewport) {
    g_half_ipd.store(half_ipd, std::memory_order_relaxed);
    g_split.store(split_viewport, std::memory_order_relaxed);
    // Keep the STORED eye consistent with what the pass detour actually renders. The detour
    // already forces Eye::Left while stereo is on, so this changes no pixels -- it only stops
    // observed().eye reporting Off during stereo, which is what a consumer polling the state sees.
    //
    // Recorded because it was briefly mistaken for a fix: the split-mode defect below is NOT this.
    g_eye.store(static_cast<uint8_t>(on ? Eye::Left : Eye::Off), std::memory_order_relaxed);
    g_stereo.store(on, std::memory_order_release);
    LOGX("[camerapass] stereo %s half_ipd=%.3f split=%d", on ? "ON" : "OFF", half_ipd, split_viewport ? 1 : 0);
}

bool CameraPassHook::replay_setup(const regenny::LTNodeTransform& camera, const float fov[2],
                                  const float rect[4], float depth_min, float depth_max) {
    auto* hook = Hooks::get().find(kHookName);
    if (hook == nullptr) {
        return false;
    }
    return hook->original<SetupFn>()(&camera, fov, rect, depth_min, depth_max) != 0;
}

void CameraPassHook::set_frustum_centre(float centre_x, float centre_y) {
    g_centre_x.store(centre_x, std::memory_order_relaxed);
    g_centre_y.store(centre_y, std::memory_order_relaxed);
    LOGX("[camerapass] frustum centre %.4f, %.4f", centre_x, centre_y);
}

void CameraPassHook::set_main_view_only(bool on) {
    g_main_only.store(on, std::memory_order_relaxed);
    LOGX("[camerapass] main-view-only %s", on ? "ON" : "OFF");
}

void CameraPassHook::set_fov_override(float fov_x, float fov_y) {
    g_fov_x.store(fov_x, std::memory_order_relaxed);
    g_fov_y.store(fov_y, std::memory_order_relaxed);
}

std::vector<CameraPassHook::PassInfo> CameraPassHook::passes_last_frame() const {
    std::vector<PassInfo> out;
    const uint32_t n = g_published_count.load(std::memory_order_acquire);
    const uint32_t slot = g_published_slot.load(std::memory_order_relaxed);
    for (uint32_t i = 0; i < n && i < kMaxPassesPerFrame; ++i) {
        out.push_back(g_frame_passes[slot][i]);
    }
    return out;
}

uint32_t CameraPassHook::passes_in_last_frame() const {
    return g_published_count.load(std::memory_order_acquire);
}

uint32_t CameraPassHook::max_passes_in_a_frame() const {
    return g_max_passes.load(std::memory_order_relaxed);
}

void CameraPassHook::reset_height_excursion() {
    g_h_samples.store(0, std::memory_order_relaxed);
}

void CameraPassHook::set_probe_point(float x, float y, float z) {
    g_probe_pt[0].store(x, std::memory_order_relaxed);
    g_probe_pt[1].store(y, std::memory_order_relaxed);
    g_probe_pt[2].store(z, std::memory_order_relaxed);
    g_probe_on.store(true, std::memory_order_relaxed);
}

void CameraPassHook::clear_probe_point() {
    g_probe_on.store(false, std::memory_order_relaxed);
    g_probe_ok.store(false, std::memory_order_relaxed);
}

CameraPassHook::Observed CameraPassHook::observed() const {
    Observed out;
    out.target = g_target.load(std::memory_order_relaxed);
    out.hooked = Hooks::get().find(kHookName) != nullptr;
    out.passes = g_passes.load(std::memory_order_relaxed);
    out.overridden = g_overridden.load(std::memory_order_relaxed);
    out.rejected = g_rejected.load(std::memory_order_relaxed);
    for (size_t i = 0; i < 3; ++i) {
        out.camera_position[i] = g_pos[i].load(std::memory_order_relaxed);
    }
    for (size_t i = 0; i < 4; ++i) {
        out.camera_rotation[i] = g_rot[i].load(std::memory_order_relaxed);
        out.rect[i] = g_rect_seen[i].load(std::memory_order_relaxed);
    }
    for (size_t i = 0; i < 2; ++i) {
        out.fov[i] = g_fov_seen[i].load(std::memory_order_relaxed);
    }
    for (size_t i = 0; i < 4; ++i) {
        out.viewport[i] = g_vp[i].load(std::memory_order_relaxed);
    }
    out.depth_min = g_depth_min.load(std::memory_order_relaxed);
    out.depth_max = g_depth_max.load(std::memory_order_relaxed);
    out.eye = static_cast<Eye>(g_eye.load(std::memory_order_acquire));
    out.half_ipd = g_half_ipd.load(std::memory_order_relaxed);
    out.split_viewport = g_split.load(std::memory_order_relaxed);
    out.fov_override = {g_fov_x.load(std::memory_order_relaxed), g_fov_y.load(std::memory_order_relaxed)};
    out.stereo = g_stereo.load(std::memory_order_acquire);
    out.main_view_only = g_main_only.load(std::memory_order_relaxed);
    out.skipped_aux = g_skipped_aux.load(std::memory_order_relaxed);
    out.target_size = {g_target_size[0].load(std::memory_order_relaxed),
                       g_target_size[1].load(std::memory_order_relaxed)};
    out.frustum_centre = {g_centre_x.load(std::memory_order_relaxed),
                          g_centre_y.load(std::memory_order_relaxed)};
    out.centre_applied = {g_centre_applied[0].load(std::memory_order_relaxed),
                          g_centre_applied[1].load(std::memory_order_relaxed)};
    out.rebuilds = g_rebuilds.load(std::memory_order_relaxed);
    out.centre_checked = g_centre_checked.load(std::memory_order_relaxed);
    out.centre_inconsistent = g_centre_inconsistent.load(std::memory_order_relaxed);
    out.second_eye_draws = g_second_draws.load(std::memory_order_relaxed);
    out.draw_calls = g_draw_calls.load(std::memory_order_relaxed);
    out.height_min = g_h_min.load(std::memory_order_relaxed);
    out.height_max = g_h_max.load(std::memory_order_relaxed);
    out.height_samples = g_h_samples.load(std::memory_order_relaxed);
    out.probe_projected = g_probe_ok.load(std::memory_order_relaxed);
    for (size_t i = 0; i < 3; ++i) {
        out.probe_point[i] = g_probe_pt[i].load(std::memory_order_relaxed);
    }
    out.probe_pixel = {g_probe_px.load(std::memory_order_relaxed), g_probe_py.load(std::memory_order_relaxed)};
    return out;
}

void CameraPassHook::set_position_offset(float x, float y, float z) {
    g_pos_off_x.store(x, std::memory_order_relaxed);
    g_pos_off_y.store(y, std::memory_order_relaxed);
    g_pos_off_z.store(z, std::memory_order_relaxed);
}

std::array<float, 3> CameraPassHook::position_offset() const {
    return {g_pos_off_x.load(std::memory_order_relaxed), g_pos_off_y.load(std::memory_order_relaxed),
            g_pos_off_z.load(std::memory_order_relaxed)};
}

std::optional<std::array<float, 3>> CameraPassHook::pristine_camera_position() const {
    if (!g_pristine_valid.load(std::memory_order_acquire)) {
        return std::nullopt;
    }

    return std::array<float, 3>{g_pristine.camera.position.x, g_pristine.camera.position.y,
                                g_pristine.camera.position.z};
}

uint32_t CameraPassHook::last_view_seq() {
    return g_view_seq.load(std::memory_order_acquire);
}

uint32_t CameraPassHook::last_view_tid() {
    return g_view_tid.load(std::memory_order_relaxed);
}

void CameraPassHook::camera_rotation_now(float out[4]) {
    for (int i = 0; i < 4; ++i) {
        out[i] = g_view_rot[i].load(std::memory_order_relaxed);
    }
}

uint64_t CameraPassHook::pristine_clobbered() {
    return g_pristine_clobbered.load(std::memory_order_relaxed);
}

uint64_t CameraPassHook::second_eye_skipped() {
    return g_second_eye_skipped.load(std::memory_order_relaxed);
}

uint64_t CameraPassHook::view_check_samples() {
    return g_view_check_samples.load(std::memory_order_relaxed);
}

uint64_t CameraPassHook::view_mismatch_frames() {
    return g_view_mismatch_frames.load(std::memory_order_relaxed);
}

float CameraPassHook::view_mismatch_worst() {
    return g_view_mismatch_worst.load(std::memory_order_relaxed);
}

uint64_t CameraPassHook::view_mismatch_left() {
    return g_view_mismatch_left.load(std::memory_order_relaxed);
}

uint64_t CameraPassHook::view_mismatch_right() {
    return g_view_mismatch_right.load(std::memory_order_relaxed);
}

uint64_t CameraPassHook::mismatch_after_aux() {
    return g_mismatch_after_aux.load(std::memory_order_relaxed);
}

void CameraPassHook::reset_frame_aux() {
    g_aux_passes_this_frame.store(0, std::memory_order_relaxed);
}

uint64_t CameraPassHook::frames_two_main() {
    return g_frames_two_main.load(std::memory_order_relaxed);
}

uint64_t CameraPassHook::frames_other_main() {
    return g_frames_other_main.load(std::memory_order_relaxed);
}

uint32_t CameraPassHook::main_worst() {
    return g_main_worst.load(std::memory_order_relaxed);
}
