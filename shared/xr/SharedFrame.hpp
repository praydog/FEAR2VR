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
    uint32_t reserved;
};

static_assert(sizeof(SharedFrameHeader) == 64,
              "the header must be byte-identical in both bitnesses");

constexpr const char* kSharedFrameName = "Local\\fear2vr_frame";

}  // namespace xr
