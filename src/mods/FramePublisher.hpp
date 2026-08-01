#pragma once

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
    bool publish(const void* bits, uint32_t pitch, uint32_t width, uint32_t height, bool bgra,
                 uint32_t layout);

    // ---- WHAT A CONSUMER WANTS TO KNOW ---------------------------------------------------------
    // WHAT THE HEADSET IS DOING, written by the host into the same mapping. Null until the section
    // is open. A consumer must check `valid` and the sequence itself -- this hands back the raw
    // block rather than a snapshot, because the caller is the one that knows what staleness it can
    // tolerate.
    const xr::HostState* host_state() const;

    uint32_t frames() const;
    double last_publish_ms() const;
    double worst_publish_ms() const;
    uint32_t last_width() const { return m_last_w; }
    uint32_t last_height() const { return m_last_h; }
    const std::string& last_error() const { return m_error; }

private:
    FramePublisher() = default;

    void* m_mapping{nullptr};
    void* m_base{nullptr};
    std::string m_error;
    uint32_t m_last_w{0};
    uint32_t m_last_h{0};
    int64_t m_last_ticks{0};
    int64_t m_worst_ticks{0};
};
