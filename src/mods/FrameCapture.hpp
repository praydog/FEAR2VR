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
    double last_mean_luma() const;

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

    std::atomic<bool> m_registered{false};
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
    std::atomic<uint64_t> m_signature{0};
    std::atomic<int64_t> m_mean_luma_milli{0};
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
