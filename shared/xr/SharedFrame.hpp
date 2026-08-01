#pragma once

#include <stdint.h>

// ---- THE GAME'S PIXELS, CROSSING TO THE 64-BIT HOST ---------------------------------------------
//
// One file mapping. The 32-bit mod writes a frame into it; the 64-bit host uploads that frame into
// an OpenXR swapchain image and submits it. Measured cost of the boundary itself: 200 nanoseconds to
// notify, ~25 GB/s to copy (tools/ipcbench).
//
// THIS HEADER IS COMPILED INTO BOTH BITNESSES, which is the entire reason it looks like this. Every
// field is fixed-width, the layout is asserted, and there is not a `size_t`, a `bool`, a pointer or
// an enum anywhere -- each of those differs or can differ between x86 and x64, and a mismatch would
// not fail, it would silently misread every frame.
//
// SYNCHRONISATION is a sequence number, not a lock: the writer makes it ODD before touching the
// pixels and EVEN when the frame is complete, so a reader that sees an even value it has not read
// before has a whole frame. A reader never blocks the game, which matters because the game's writer
// runs on its render thread.

namespace xr {

constexpr uint32_t kSharedFrameMagic = 0x32524546u;  // 'FER2'
constexpr uint32_t kSharedFrameVersion = 1u;

// Sized for the game's own back buffer at 2560x1440 BGRA. Deliberately NOT sized for a supersampled
// future: the section is committed memory in both processes, and the resolution question is settled
// by measurement later rather than by reserving for it now.
constexpr uint32_t kSharedFrameMaxBytes = 2560u * 1440u * 4u;

// ---- WHY THERE IS MORE THAN ONE PIXEL BUFFER ----------------------------------------------------
//
// A sequence number protects the HEADER. It does not protect fourteen megabytes of pixels that the
// reader is still uploading: the host's copy out of the section takes ~2.5 ms and the game
// republishes every ~15 ms, so with a single buffer roughly one frame in six is caught mid-write
// and uploaded TORN -- part of the previous frame, part of the next. Reported from the headset as
// "70% of frames are correct" with the rest looking stale or mismatched, which is exactly what a
// torn frame looks like when the two halves came from different head poses.
//
// Three buffers, not two: two is sufficient only while the reader always finishes inside one frame
// period, and the reader is a separate process that can be descheduled. The third costs 14 MB and
// removes the assumption.
constexpr uint32_t kFrameSlots = 3u;

// How the pixels should be read.
constexpr uint32_t kLayoutMono = 0u;          // one image, shown to both eyes
constexpr uint32_t kLayoutSideBySide = 1u;    // left half is the left eye, right half the right
struct alignas(64) SharedFrameHeader {
    // THE 64-BIT FIELDS COME FIRST, and that ordering is load-bearing rather than stylistic. With
    // them in the middle the compiler inserted four bytes of padding to align them, which pushed
    // the struct to 128 bytes -- caught here by the assert, but it would otherwise have been a
    // silent disagreement between a 32-bit writer and a 64-bit reader.
    int64_t write_qpc;  // when the writer finished this frame
    int64_t qpc_freq;   // so a reader converts without assuming its own frequency matches

    uint32_t magic;
    uint32_t version;

    // Odd while the writer is inside the pixels; even when a whole frame is present.
    volatile uint32_t sequence;
    uint32_t layout;

    uint32_t width;
    uint32_t height;
    uint32_t pitch;  // bytes per row as the writer produced them, which is NOT width * 4
    uint32_t bytes;  // pitch * height

    // D3D9 back buffers are BGRA in memory, so the host picks a BGRA swapchain format and the
    // upload is a straight copy. Published rather than assumed: getting it wrong swaps red and blue
    // and looks like a colour-grading bug rather than a format bug.
    uint32_t bgra;
    uint32_t writer_pid;
    uint32_t frames_written;

    // WHICH BUFFER holds the completed frame. The writer fills a different one, so a reader may
    // take as long as it likes over this slot without the pixels moving underneath it.
    uint32_t slot;

    // WHICH HostState THE GAME RENDERED WITH. The host keeps a short history of the poses it
    // published and submits this frame using the one the game actually used -- not the newest.
    // Declaring the current pose for an image rendered from an older one is the difference between
    // reprojection CORRECTING head motion and reprojection doing nothing while the image swims.
    uint32_t host_sequence;
};

// 128 now that the slot index is carried: alignas(64) rounds up, and the exact size matters far
// less than both bitnesses agreeing on it -- which is what the assert is really for.
static_assert(sizeof(SharedFrameHeader) == 128,
              "the header must be byte-identical in both bitnesses");

// ---- THE OTHER DIRECTION: WHAT THE HEADSET IS DOING ---------------------------------------------
//
// The host knows where the wearer's head is; the game needs it to point its camera. Same mapping,
// written by the host and read by the game, and the same discipline -- fixed width, asserted, and a
// sequence rather than a lock so neither side can stall the other.
//
// WHY THE GAME MUST HAVE THIS BEFORE ANY PROJECTION LAYER: a projection layer tells the compositor
// "this image was rendered from the pose you gave me". Until the game's camera actually follows the
// head that statement is false, and the compositor's reprojection turns the lie into a world that
// swings when the wearer looks around.
struct alignas(64) HostState {
    int64_t write_qpc;

    uint32_t sequence;  // odd while writing
    uint32_t valid;     // the runtime reported an ORIENTATION_VALID pose

    float orientation[4];  // x, y, z, w -- head pose in the host's LOCAL space
    float position[3];     // metres
    uint32_t frames;

    // The SYMMETRIC half-angles the game should render with, in radians. A headset's own frustum is
    // asymmetric and this engine offers no asymmetric projection, so the game renders the smallest
    // symmetric frustum that CONTAINS the headset's, and the host declares exactly that to the
    // compositor. The corners are then over-rendered and cropped, which costs pixels and nothing
    // else -- the alternative is a wrong frustum, which costs correctness.
    float fov_x;
    float fov_y;
    float ipd_m;  // full interpupillary distance the host measured, in metres
    uint32_t reserved;
};

static_assert(sizeof(HostState) == 64, "HostState must be byte-identical in both bitnesses");

// Layout of the mapping: [SharedFrameHeader][HostState][pixels]
constexpr uint32_t kPayloadOffset =
    static_cast<uint32_t>(sizeof(SharedFrameHeader) + sizeof(HostState));

// Where a given buffer starts. Slots are padded to the maximum so the offset never depends on the
// resolution in flight -- a reader must be able to find a slot without knowing what is in it.
constexpr uint32_t slot_offset(uint32_t slot) {
    return kPayloadOffset + (slot % kFrameSlots) * kSharedFrameMaxBytes;
}

constexpr const char* kSharedFrameName = "Local\\fear2vr_frame";

// ---- LETTING THE RUNTIME PACE THE GAME ----------------------------------------------------------
//
// Signalled by the host once per xrWaitFrame, waited on by the game inside its own update. This is
// how a normal VR title works -- xrWaitFrame IS the frame clock, and the application runs at
// whatever cadence the compositor asks for -- and it is not available to the game directly here
// because the runtime lives in another process.
//
// WHY IT MATTERS MORE THAN IT SOUNDS: unpaced, the game ran at 140-150 fps into a 90 Hz compositor
// and juddered, while the SAME build alt-tabbed to ~72 fps looked perfect. A faster game made the
// picture worse, because frames and poses were being produced on two unrelated clocks and the beat
// between them is visible. Pacing removes the beat rather than compensating for it.
constexpr const char* kFrameTickEventName = "Local\\fear2vr_frame_tick";

}  // namespace xr
