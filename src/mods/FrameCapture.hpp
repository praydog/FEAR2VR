#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "../Mod.hpp"

// ---- READING THE RENDERED FRAME BACK ---------------------------------------
//
// Two things this project has needed for a long time, and they are the same
// mechanism.
//
// 1. **A visual oracle that is not a lie.** Desktop captures here return stale
//    or black frames while the engine reports a live world and a climbing pass
//    count -- proven with a control that had visibly moved the world moments
//    earlier and then changed nothing on screen. Every conclusion drawn from a
//    screenshot in this project has had to be thrown away. Reading the BACK
//    BUFFER inside the present hook cannot be stale: it is the frame the engine
//    is about to show, sampled on the render thread in phase with it.
//
// 2. **The fallback stereo route.** Sharing a surface with an OpenXR swapchain
//    wants D3D9Ex, which is now measured as expensive -- 512 static managed
//    textures per level load would each need a COM proxy. The alternative is to
//    copy the finished frame out and upload it on the OpenXR side, which needs
//    no engine-wide surgery at all. Whether that is viable is a TIMING question,
//    and this is the thing that times it.
//
// The copy is `GetRenderTargetData` into a SYSTEMMEM surface -- the only legal
// way to get render-target pixels onto the CPU in D3D9. It is a GPU-to-CPU
// readback and therefore a synchronisation point, which is exactly why the cost
// has to be measured rather than assumed.
//
// ---- WHAT IT COSTS AND WHEN IT RUNS ---------------------------------------
//
// OFF by default, and bounded when on: a readback every frame would stall the
// pipeline, and this project has already wedged the payload once by putting
// unbounded work in a per-frame hook. `request_capture()` arms exactly one, and
// the staging surface is created once and reused so the measurement times the
// COPY rather than an allocation.
class FrameCapture final : public Mod {
public:
    static FrameCapture& get();

    std::string_view get_name() const override { return "FrameCapture"; }
    std::optional<std::string> on_initialize() override;
    void on_frame() override {}
    void on_shutdown() override;

    // Arm ONE capture, serviced on the next present. Returns false if a capture is
    // already pending -- a caller that fires twice should know its first is still
    // in flight rather than silently losing one.
    // ---- CAPTURING AT A REDUCED RESOLUTION ---------------------------------
    //
    // The readback costs ~10 ms at 2560x1440, which is an entire 90 Hz frame budget,
    // and it scales with PIXELS. So the question for a copy-based stereo path is not
    // "is it affordable" but "at what resolution", and answering that needs the
    // engine's own downscale in the path rather than arithmetic on a single sample.
    //
    // `divisor` of 1 reads the back buffer directly; 2 halves each axis (a quarter of
    // the pixels), and so on. The downscale is a StretchRect on the GPU into a
    // DEFAULT-pool render target -- which is what a real VR path would do anyway,
    // since an eye texture is rarely the desktop's resolution.
    //
    // This is the consumer knob: a mod submitting frames to a headset picks the
    // resolution its budget allows, and the timings below say what that budget buys.
    void set_divisor(uint32_t divisor);
    uint32_t divisor() const { return m_divisor.load(std::memory_order_relaxed); }

    // Milliseconds for the GPU downscale, when a divisor above 1 puts one in the path.
    double last_stretch_ms() const;

    // ---- CONTINUOUS, PIPELINED CAPTURE -------------------------------------
    //
    // THE optimisation the timing curve points at. A one-shot capture pays ~2 ms of
    // pure GPU synchronisation -- waiting for the frame it just asked about -- and
    // that cost is fixed regardless of resolution, so no amount of downscaling
    // removes it.
    //
    // Double buffering does: issue the readback for frame N into one staging surface
    // and lock the one issued for frame N-1, which the GPU finished during the frame
    // that has since elapsed. Nothing waits. The cost is one frame of AGE in the
    // captured image, which for a headset submission is the ordinary tradeoff.
    //
    // This is the mode a VR presenter would actually run in, which is why it is a
    // mode rather than a benchmark: `set_continuous(true)` and read the newest
    // completed frame each present.
    void set_continuous(bool enabled);
    bool continuous() const { return m_continuous.load(std::memory_order_relaxed); }

    // Frames captured in continuous mode, and the lock cost in that mode -- the number
    // that says whether the pipelining actually removed the stall.
    uint64_t continuous_frames() const { return m_cont_frames.load(std::memory_order_relaxed); }
    double continuous_lock_ms() const;

    // ---- IS THE PICTURE ACTUALLY DIFFERENT? ----------------------------------------------------
    //
    // A cheap signature of the last captured frame: a sampled grid reduced to one 64-bit value,
    // plus the mean luminance. Both are computed during the readback that already happens, so this
    // costs nothing extra.
    //
    // The question it exists to answer is the one a stereo path cannot answer for itself. This
    // project can show structurally that two eyes are configured -- asymmetric frustum centres,
    // a second draw group -- and none of that proves the two eyes RENDER DIFFERENTLY. Measured
    // here by capturing each eye and comparing: same eye twice differs by a mean of 1.3 (animation
    // and flicker), the two eyes by 15.3, with no overlap between the distributions.
    //
    // A consumer bringing up a headset wants exactly this check at startup: render left, render
    // right, and refuse to claim stereo if the signatures match.
    uint64_t last_signature() const { return m_signature.load(std::memory_order_relaxed); }

    // ---- THE TWO HALVES, WHICH IS WHAT A SIDE-BY-SIDE PAIR IS MADE OF --------------------------
    //
    // Mean luminance of the left and right halves of the captured frame, accumulated in the same
    // readback walk as everything else. A stereo bring-up compares them constantly: a side-by-side
    // submission is only a PAIR if the halves carry the same scene from two viewpoints, and the
    // cheapest thing that distinguishes "two viewpoints" from "one view and some garbage" is that
    // the halves are of comparable brightness while not being identical.
    //
    // This exists because every diagnosis of the split path so far has needed a host-side image
    // library to answer a question the mod could answer for itself. Live, with the split active and
    // the right half known-bad, the two differ by roughly a third; mono, they differ by the scene.
    double last_left_luma() const;
    double last_right_luma() const;
    double last_mean_luma() const;

    // ---- WHERE IN THE FRAME THE PICTURE IS TAKEN -----------------------------------------------
    //
    // By default the readback happens at PRESENT, which is the finished frame. That is the right
    // answer for a screenshot and the wrong one for diagnosing a render path, because everything
    // between a draw and the present has already happened to the pixels.
    //
    // AfterSecondEye reads the back buffer INSIDE the pass hook, immediately after the second eye's
    // draw returns. Comparing the two answers "did this stage produce the picture, or did something
    // later change it" -- which no amount of staring at the final frame can settle.
    enum class Stage : uint32_t {
        Present = 0,
        AfterSecondEye = 1,
    };
    void set_stage(Stage s) { m_stage.store(static_cast<uint32_t>(s), std::memory_order_release); }
    Stage stage() const { return static_cast<Stage>(m_stage.load(std::memory_order_acquire)); }

    // ---- KEEPING THE PAIR ON THE GPU ----------------------------------------------------------
    //
    // Everything above ends in SYSTEM MEMORY, which costs ~10 ms at 2560x1440 and 3.6-17% of frame
    // rate -- fine for a screenshot or a diagnostic, useless for submission. A compositor wants a
    // TEXTURE, and the copy that produces one never leaves the GPU.
    //
    // The mirror is a private render target the same size as the back buffer, filled with
    // StretchRect at the chosen stage. That is the surface a headset submission hands over, and the
    // reason this can be built and measured without any hardware: the copy either happens at GPU
    // speed and holds the right picture, or it does not.
    //
    // Armed separately from the readback because they answer different questions and a consumer
    // submitting frames does not want a 10 ms stall attached.
    void set_gpu_mirror(bool enabled);

    // ---- HANDING FRAMES TO THE 64-BIT HOST -----------------------------------------------------
    //
    // Publish every pipelined readback into the shared section FramePublisher owns, which is how
    // the pixels reach the process that can actually submit them to a headset. Turning this on also
    // turns on the continuous path, because a one-shot capture is not a video feed.
    void set_publishing(bool enabled);
    bool publishing() const { return m_publishing.load(std::memory_order_acquire); }

    // ---- DEVICE LOSS -------------------------------------------------------------------------
    //
    // How many times this mod has seen the device leave D3D_OK and dropped its resources for it.
    // Worth exposing: a consumer that allocates its own DEFAULT-pool resources needs to know the
    // device went away, and a rising count with no recovery is a real symptom.
    uint32_t device_lost_events() const { return m_device_lost.load(std::memory_order_relaxed); }

    // Drop every device resource at the next frame, as a device loss would. Exists so the REBUILD
    // path -- the half that actually breaks -- can be exercised on demand instead of only when
    // someone alt-tabs. Returns false if no frame can service it.
    bool request_resource_drop();
    bool gpu_mirror() const { return m_mirror_on.load(std::memory_order_relaxed); }

    // The live surface. Null until the mirror has run at least once. Valid on the render thread;
    // do NOT hold it across a device reset.
    void* gpu_mirror_surface() const { return m_mirror; }
    uint64_t gpu_mirror_frames() const { return m_mirror_frames.load(std::memory_order_relaxed); }
    double last_gpu_copy_ms() const;

    // Ask for the mirror to be read back once and its half luminances reported, so a caller can
    // prove the GPU copy holds the same picture the CPU path sees. Deliberately expensive -- a
    // verification, not a submission path -- and what lets the suite assert the mirror's CONTENT
    // rather than merely that a copy returned S_OK.
    //
    // REQUESTED, not performed: this touches the device, and the device belongs to the render
    // thread. Calling it from the IPC thread returned zeros and would eventually have returned
    // something worse. Poll mirror_verified() for the result.
    void request_gpu_mirror_verify() { m_mirror_verify.store(true, std::memory_order_release); }
    bool mirror_verified() const { return m_mirror_verified.load(std::memory_order_relaxed); }
    double mirror_left_luma() const;
    double mirror_right_luma() const;

    // The BACK BUFFER's halves, sampled inside the same verification call as the mirror's. The
    // comparison a consumer wants is "does the mirror hold what the frame holds", and that is only
    // an identity if both are read at ONE instant -- the published readback above is whatever frame
    // last completed, which drifts by a frame and by however much the scene moved in it.
    double mirror_ref_left_luma() const;
    double mirror_ref_right_luma() const;

    // Perform a pending readback right now. MUST be called on the render thread; it is what the
    // present callback calls, exposed so another stage can call it at its own point in the frame.
    void service_now();

    bool request_capture();

    // Arm one capture AND write it to `path` as a BMP. Empty path captures without
    // writing, which is the timing-only case.
    bool request_capture_to(const std::string& path);

    bool pending() const { return m_pending.load(std::memory_order_relaxed); }
    uint64_t captures() const { return m_captures.load(std::memory_order_relaxed); }
    uint64_t failures() const { return m_failures.load(std::memory_order_relaxed); }

    // Milliseconds for the readback alone, measured on the render thread around
    // GetRenderTargetData. This is the number that decides whether the copy-based
    // stereo path can hold a frame budget.
    double last_copy_ms() const;
    double worst_copy_ms() const;

    // Milliseconds for the whole serviced capture including the file write, so a
    // caller can tell the copy apart from the encoding.
    double last_total_ms() const;

    // Milliseconds for the LOCK. Reported separately because GetRenderTargetData timed at
    // 0.002 ms for a 14 MB readback, which is not a copy -- D3D9 queues the transfer and the
    // stall lands on LockRect. A number that implausible is the measurement being wrong, not
    // the hardware being fast, and the two have to be visible apart for the stereo decision
    // to rest on either.
    double last_lock_ms() const;

    uint32_t width() const { return m_width.load(std::memory_order_relaxed); }
    uint32_t height() const { return m_height.load(std::memory_order_relaxed); }
    uint32_t format() const { return m_format.load(std::memory_order_relaxed); }

    // Non-black pixel count from the last capture. THE reason this class exists as
    // an oracle: "the renderer is running" and "the frame has content" are different
    // claims, and every stale-screenshot conclusion in this project came from
    // conflating them.
    uint64_t nonblack_pixels() const { return m_nonblack.load(std::memory_order_relaxed); }
    uint64_t sampled_pixels() const { return m_sampled.load(std::memory_order_relaxed); }

    // The last error, as the D3D HRESULT, so a failure names itself rather than
    // collapsing into a bool.
    int32_t last_hresult() const { return m_hr.load(std::memory_order_relaxed); }

private:
    FrameCapture() = default;

    static void on_present();
    void service();
    void service_continuous();
    void service_mirror();

    // Free every D3D surface this mod owns. MUST run on the render thread -- see on_shutdown().
    void release_surfaces();

    // Free the surfaces but KEEP the caller's intent, so they are rebuilt on the next frame that
    // needs them. This is what device loss requires -- see the device-lost handling in on_present.
    void free_device_resources();
    bool verify_gpu_mirror();

    std::atomic<bool> m_registered{false};
    std::atomic<bool> m_release_requested{false};
    std::atomic<bool> m_released{false};
    std::atomic<uint32_t> m_device_lost{0};
    std::atomic<bool> m_drop_requested{false};
    std::atomic<bool> m_publishing{false};

    // THE POSE EACH PIPELINED SURFACE WAS RENDERED WITH. The readback is one frame deep, so the
    // pixels handed to the host are older than the pose that is current when they are handed over.
    // Stamping them with the current pose tells the compositor the image is newer than it is, and
    // its reprojection then corrects by a whole frame of head motion in the wrong direction --
    // which is judder, and only while turning.
    uint32_t m_pipe_seq[2]{};
    std::atomic<bool> m_pending{false};
    std::atomic<uint64_t> m_captures{0};
    std::atomic<uint64_t> m_failures{0};
    std::atomic<int64_t> m_copy_ticks{0};
    std::atomic<int64_t> m_worst_ticks{0};
    std::atomic<int64_t> m_total_ticks{0};
    std::atomic<int64_t> m_lock_ticks{0};
    std::atomic<int64_t> m_stretch_ticks{0};
    std::atomic<uint32_t> m_divisor{1};
    std::atomic<bool> m_continuous{false};
    std::atomic<uint32_t> m_stage{0};
    std::atomic<bool> m_mirror_on{false};
    std::atomic<uint64_t> m_mirror_frames{0};
    std::atomic<int64_t> m_mirror_copy_ticks{0};
    std::atomic<int64_t> m_mirror_left_milli{0};
    std::atomic<int64_t> m_mirror_right_milli{0};
    std::atomic<int64_t> m_mirror_ref_left_milli{0};
    std::atomic<int64_t> m_mirror_ref_right_milli{0};
    void* m_mirror{nullptr};
    uint32_t m_mirror_w{0};
    uint32_t m_mirror_h{0};
    uint32_t m_mirror_fmt{0};
    std::atomic<bool> m_mirror_verify{false};
    std::atomic<bool> m_mirror_verified{false};
    std::atomic<uint64_t> m_signature{0};
    std::atomic<int64_t> m_mean_luma_milli{0};
    std::atomic<int64_t> m_left_luma_milli{0};
    std::atomic<int64_t> m_right_luma_milli{0};
    std::atomic<uint64_t> m_cont_frames{0};
    std::atomic<int64_t> m_cont_lock_ticks{0};
    std::atomic<uint32_t> m_width{0};
    std::atomic<uint32_t> m_height{0};
    std::atomic<uint32_t> m_format{0};
    std::atomic<uint64_t> m_nonblack{0};
    std::atomic<uint64_t> m_sampled{0};
    std::atomic<int32_t> m_hr{0};

    // Guarded by the render thread only: the present callback is the sole writer.
    void* m_staging{nullptr};
    // The intermediate render target the downscale lands in, when a divisor is set.
    void* m_scaled{nullptr};
    // Ping-pong staging for the pipelined path. Index `m_issue` receives this frame's
    // readback; the other holds the previous frame's, already complete.
    void* m_pipe[2]{nullptr, nullptr};
    uint32_t m_pipe_w{0};
    uint32_t m_pipe_h{0};
    uint32_t m_issue{0};
    bool m_pipe_primed{false};
    uint32_t m_scaled_w{0};
    uint32_t m_scaled_h{0};
    uint32_t m_staging_w{0};
    uint32_t m_staging_h{0};
    uint32_t m_staging_fmt{0};
    std::string m_path;
};
