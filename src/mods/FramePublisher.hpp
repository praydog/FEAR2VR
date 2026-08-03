#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "xr/SharedFrame.hpp"

// ---- HANDING THE GAME'S FRAME TO THE 64-BIT HOST ------------------------------------------------
//
// FEAR2 is 32-bit and the 32-bit Oculus runtime cannot create a session, so the pixels have to leave
// this process to reach the headset. This publishes them into a file mapping the host reads.
//
// Cost, measured rather than assumed: the notification is ~200 nanoseconds and the copy runs at
// ~25 GB/s, so a 2560x1440 BGRA frame costs about 0.58 ms -- against 11.1 ms at 90 Hz. The write
// happens on the RENDER THREAD, inside the lock of an already-pipelined readback whose own stall is
// 0.001 ms, so this copy is very nearly the whole per-frame price of being out of process.
//
// It is a copy that should eventually not exist. A shared GPU texture would cost nothing at any
// resolution, and the only thing preventing one is D3D9-without-Ex. Treat this as the first of two
// implementations of the same idea, not as the design.
class FramePublisher {
public:
    static FramePublisher& get();

    // Create the shared section. Idempotent, and safe to call when the host is not running -- the
    // host opens the section, never the other way round, so publishing works with nobody listening.
    bool open();
    void close();
    bool active() const { return m_base != nullptr; }

    // Copy one frame in and mark it complete. `pitch` is the source's own row stride, which is not
    // width * 4 -- a locked D3D surface is padded, and assuming otherwise shears the image.
    //
    // Returns false when the frame does not fit or the section is not open, and NEVER blocks: the
    // caller is the render thread and a reader must not be able to stall a game.
    // THE UI LAYER -- a second, smaller image with alpha, travelling beside the world so the host can
    // put it on a quad. Same sequence discipline and its own slots; see xr::UiFrameHeader.
    //
    // Passing a null `bits` RETIRES the layer rather than failing: the mod is armed and disarmed at
    // runtime, and a host left showing the last HUD forever is worse than one showing none.
    //
    // `derive_alpha` tells the host to compute alpha as max(r, g, b) as it copies, because the
    // engine's UI shaders emit none. Passing false with pixels that carry no alpha publishes an
    // invisible layer -- which is exactly what the first version of this did, caught by reading the
    // mapping back rather than by the build.
    bool publish_ui(const void* bits, uint32_t pitch, uint32_t width, uint32_t height, bool bgra,
                    bool premultiplied, bool derive_alpha);

    // PRESENT THE FRAMES FLAT -- see xr::SharedFrameHeader::flat. Set while a menu is up, so the
    // host stops claiming the image was rendered from the pose it just handed out and shows it on
    // a quad instead.
    //
    // Writes the header AT ONCE rather than waiting for the next publish: the flag is for the case
    // where the game has stopped being a 3D world, and that is exactly when publishing may stop
    // too.
    void set_flat(bool on);
    bool flat() const { return m_flat.load(std::memory_order_relaxed); }

    bool publish(const void* bits, uint32_t pitch, uint32_t width, uint32_t height, bool bgra,
                 uint32_t layout, uint32_t host_sequence);

    // ---- WHAT A CONSUMER WANTS TO KNOW ---------------------------------------------------------
    // WHAT THE HEADSET IS DOING, written by the host into the same mapping. Null until the section
    // is open. A consumer must check `valid` and the sequence itself -- this hands back the raw
    // block rather than a snapshot, because the caller is the one that knows what staleness it can
    // tolerate.
    const xr::HostState* host_state() const;

    // The controller block, immediately after the head block in the same mapping. Null before the
    // mapping is open; a caller must still check `sequence` for a torn read, exactly as with the head.
    const xr::HandsState* hands_state() const;

    // ---- THE RUNTIME'S FRAME CLOCK -------------------------------------------------------------
    //
    // Blocks until the host's next xrWaitFrame tick, or the timeout, whichever comes first. Called
    // from the game's own update so the whole loop is paced by the compositor.
    //
    // NEVER hangs: the timeout is short, and a run of timeouts stops the waiting entirely until
    // ticks resume, so a host that dies or a session that goes idle costs nothing. A frame clock
    // that can freeze the game is worse than no frame clock.
    bool wait_for_host_tick(uint32_t timeout_ms);
    uint64_t tick_waits() const { return m_tick_waits; }
    uint64_t tick_timeouts() const { return m_tick_timeouts; }
    bool pacing_live() const { return m_consecutive_timeouts < kPacingGiveUp; }

    // Drop a tick that fired while the last frame was still rendering, so a game that cannot hold
    // the compositor's rate settles on a SUBMULTIPLE of it instead of beating against it.
    void set_phase_lock(bool on) { m_phase_lock.store(on, std::memory_order_relaxed); }
    bool phase_lock() const { return m_phase_lock.load(std::memory_order_relaxed); }
    uint64_t ticks_dropped() const { return m_ticks_dropped; }
    uint32_t pace_divisor() const { return m_divisor; }

    // The host's own frame counter, straight from the mapping -- the COMPOSITOR's rate, which is
    // what the game's rate has to divide into. Not the runtime frame counter, which is the game's.
    uint32_t host_frames() const {
        const auto* h = host_state();
        return h == nullptr ? 0u : h->frames;
    }

    uint32_t frames() const;
    double last_publish_ms() const;
    double worst_publish_ms() const;
    uint32_t last_width() const { return m_last_w; }
    uint32_t last_height() const { return m_last_h; }
    const std::string& last_error() const { return m_error; }

private:
    FramePublisher() = default;

    std::atomic<bool> m_phase_lock{true};
    // The compositor periods spent per game frame. Held rather than recomputed, so a workload that
    // straddles a boundary settles instead of alternating.
    uint32_t m_divisor{1};
    uint32_t m_window_frames{0};
    uint32_t m_window_overruns{0};
    uint32_t m_clean_windows{0};
    uint32_t m_last_tick_seen{0};
    bool m_tick_primed{false};
    int64_t m_work_start_qpc{0};
    int64_t m_wait_qpc{0};
    int64_t m_period_qpc{0};
    int64_t m_window_work_qpc{0};
    // SHORT, because recovery is measured in frames and the frames are slow precisely when the
    // divisor is high: at 60 frames a window lasted 3 seconds at 1/4 rate, so climbing back from a
    // heavy area took twenty. A window has to be short enough that leaving the hard part of a level
    // feels like leaving it.
    static constexpr uint32_t kDivisorWindow = 24;
    static constexpr uint32_t kMaxDivisor = 4;
    uint64_t m_ticks_dropped{0};
    static constexpr uint32_t kPacingGiveUp = 30;  // ~0.6 s of silence at 90 Hz

    void* m_mapping{nullptr};
    void* m_tick_event{nullptr};
    uint64_t m_tick_waits{0};
    uint64_t m_tick_timeouts{0};
    uint32_t m_consecutive_timeouts{0};
    void* m_base{nullptr};
    std::string m_error;

    // Whether the next published frames should be presented FLAT. Read on the render thread inside
    // publish(), written from the frame boundary, so it is atomic rather than a plain bool.
    std::atomic<bool> m_flat{false};
    uint32_t m_last_w{0};
    uint32_t m_last_h{0};
    int64_t m_last_ticks{0};
    int64_t m_worst_ticks{0};
};
