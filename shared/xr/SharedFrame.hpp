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
// 3 adds the `flat` flag. The host VALIDATES this: a mismatched pair would otherwise read the
// pixels at the wrong offset and show garbage rather than refusing.
constexpr uint32_t kSharedFrameVersion = 4u;  // 4 adds the rendered pose

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

    // PRESENT THIS FLAT -- as a quad, not as a projection layer.
    //
    // A projection layer asserts "this image was rendered from the pose you just gave me", and the
    // compositor reprojects it against head motion on that promise. While a MENU is up the game's
    // camera stops following the head, so the promise is false and the reprojection turns a frozen
    // world into one that swings when the wearer looks around -- reported from the headset as
    // nauseating, which is exactly what it is.
    //
    // A quad claims nothing: a flat rectangle at a fixed place. The host already has that path for
    // the pre-tracking case; this lets the GAME ask for it, because only the game knows a menu is
    // up. Set whenever the front end or the pause menu is showing.
    uint32_t flat;

    // ---- THE POSE THIS IMAGE WAS ACTUALLY RENDERED FROM ----------------------------------------
    //
    // `host_sequence` above is an INDEX, and the host resolves it against its own record of what it
    // sent. That is a promise about what the game was GIVEN, not about what it DREW, and everything
    // between the two is invisible to the compositor: the conversion into the engine's basis, the
    // conjugation into the body's frame, the engine's own `outer * inner` composition, and its
    // pitch clamp. Any of those altering the rotation leaves the compositor warping from a pose the
    // image was never rendered with -- a fixed discrepancy that only becomes visible MOTION error
    // when the head turns, and stays invisible on the desktop because the game rendered itself
    // consistently.
    //
    // So the writer states it outright, in the runtime's own space: the head orientation recovered
    // from the camera the frame was drawn with. An index can be stale, recycled or double-counted;
    // a value cannot be any of those things.
    //
    // `rendered_valid` is zero when the writer could not recover it, and the reader must fall back
    // to the sequence lookup rather than trusting zeros.
    uint32_t rendered_valid;
    float rendered_orientation[4];  // x, y, z, w in the host's LOCAL space
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

    // Bumped whenever the RUNTIME recentres -- OpenXR reports this as
    // XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING. Without it, a recentre performed in the
    // headset silently moves the origin of every position we publish, and the game keeps measuring
    // roomscale against an origin that no longer means anything: the wearer ends up standing
    // beside their character with nothing to explain it.
    uint32_t recenter_serial;
};

static_assert(sizeof(HostState) == 64, "HostState must be byte-identical in both bitnesses");

// ---- THE CONTROLLERS ----------------------------------------------------------------------------
//
// A THIRD BLOCK rather than more fields on HostState, because HostState is asserted byte-identical
// at 64 bytes and verified in both bitnesses; growing it would move kPayloadOffset and re-open a
// layout that is known good. This one carries its own sequence, so a torn hand read cannot be
// mistaken for a torn head read either.
//
// AIM AND GRIP ARE BOTH CARRIED, and that is not redundancy. `aim` is the pointing ray a weapon
// follows; `grip` is where the hand physically is. On a Touch controller they differ by roughly a
// 45 degree pitch, so driving a gun from grip points it at the floor. Mirrors vr::HandState.
//
// Everything here is in the HOST's convention -- OpenXR LOCAL space, right-handed, -Z forward,
// metres. The Z flip and the unit scale belong to the consumer, exactly as they do for the head.
struct HandPose {
    float orientation[4];  // x, y, z, w
    float position[3];     // metres
    uint32_t valid;        // orientation AND position both reported valid
};

static_assert(sizeof(HandPose) == 32, "HandPose must be byte-identical in both bitnesses");

struct HandInput {
    HandPose aim;
    HandPose grip;

    float trigger;  // [0,1]
    float squeeze;  // [0,1]
    float stick[2];  // [-1,1] per axis

    uint32_t buttons;  // kHandButton* below, matching vr::VRRuntime's mask exactly
    uint32_t active;   // the runtime is tracking this controller at all
    uint32_t tracked;  // the pose is tracked rather than merely inferred
    uint32_t reserved;
};

static_assert(sizeof(HandInput) == 96, "HandInput must be byte-identical in both bitnesses");

// Same numbering as vr::VRRuntime::kButton*, so neither side translates.
constexpr uint32_t kHandButtonA = 1u << 0;
constexpr uint32_t kHandButtonB = 1u << 1;
constexpr uint32_t kHandButtonX = 1u << 2;
constexpr uint32_t kHandButtonY = 1u << 3;
constexpr uint32_t kHandButtonThumbstick = 1u << 4;
constexpr uint32_t kHandButtonMenu = 1u << 5;

constexpr uint32_t kHandLeft = 0u;
constexpr uint32_t kHandRight = 1u;

struct alignas(64) HandsState {
    int64_t write_qpc;

    uint32_t sequence;  // odd while writing, same discipline as the other two blocks
    uint32_t frames;

    // Zero until the runtime has bound an interaction profile. Poses read as invalid before that,
    // but the two are worth distinguishing: "no controller bound" is a setup problem and "bound but
    // not tracked" is the wearer having put it down.
    uint32_t profile_bound;
    uint32_t reserved[3];

    HandInput hand[2];  // kHandLeft, kHandRight
};

static_assert(sizeof(HandsState) == 256, "HandsState must be byte-identical in both bitnesses");

// ---- THE UI LAYER, WHICH IS A SECOND IMAGE ------------------------------------------------------
//
// The HUD is captured on its own transparent surface (src/mods/UICapture.hpp) and travels
// separately from the world, so the host can put it on a QUAD in front of the wearer instead of
// leaving it smeared across both eyes of a stereo image. Same discipline as the frame block: fixed
// width, asserted size, odd/even sequence, and its own slots so a reader is never inside a buffer
// the writer is filling.
//
// SMALLER THAN THE WORLD, on purpose. The UI is flat, sparse and read at a fixed apparent size on a
// quad, so it does not need the back buffer's resolution -- and the cost here is a per-frame GPU
// downscale plus a readback that is paid on the game's render thread.
constexpr uint32_t kUiMaxBytes = 1920u * 1080u * 4u;

// Two is enough where the frame needs three: the UI is a quarter of the bytes, so the host is never
// inside one for anything like a frame period.
constexpr uint32_t kUiSlots = 2u;

struct alignas(64) UiFrameHeader {
    int64_t write_qpc;

    volatile uint32_t sequence;  // odd while the writer is inside the pixels
    // Whether the game is publishing a UI layer AT ALL. Distinct from a stale sequence: the mod is
    // off by default, and "nothing has been published" must not read as "the last frame, forever".
    uint32_t present;

    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t bytes;

    uint32_t bgra;
    uint32_t slot;
    uint32_t frames_written;

    // THE COLOUR IS ALREADY MULTIPLIED BY THE ALPHA, because of how the layer is produced: the
    // surface is cleared to transparent black and only the UI draws into it, so what lands there is
    // the contribution, not the tint. The host must therefore NOT set
    // XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT -- doing so double-darkens every edge.
    uint32_t premultiplied;

    // ALPHA IS NOT IN THE PIXELS -- the host must derive it as max(r, g, b) while it copies.
    //
    // The engine's UI shaders emit zero alpha and forcing the write mask does not change that (the
    // mask was measured at 0xF for the whole bracket). It does not matter, because the surface is
    // cleared to transparent black and only the UI draws into it: RGB is already the premultiplied
    // contribution and its brightness IS its coverage.
    //
    // Derived on the HOST rather than by the game, purely for where the cost lands. It is one pass
    // over ~1 megapixel; the host is already touching every pixel to upload it, while the game
    // would be paying it on its render thread inside the frame.
    uint32_t derive_alpha;

    uint32_t reserved[3];
};

static_assert(sizeof(UiFrameHeader) == 64, "UiFrameHeader must be byte-identical in both bitnesses");

// Layout of the mapping:
//   [SharedFrameHeader][HostState][HandsState][UiFrameHeader][frame slots x3][ui slots x2]
//
// The UI header sits with the other headers rather than beside its pixels, so every fixed-size
// block stays in one contiguous run and only the payloads are resolution-sized.
constexpr uint32_t kUiStateOffset =
    static_cast<uint32_t>(sizeof(SharedFrameHeader) + sizeof(HostState) + sizeof(HandsState));

constexpr uint32_t kPayloadOffset = kUiStateOffset + static_cast<uint32_t>(sizeof(UiFrameHeader));

// Where each block sits, named rather than recomputed at every call site -- the head block's offset
// was open-coded as sizeof(header) in three places before the hands existed.
constexpr uint32_t kHostStateOffset = static_cast<uint32_t>(sizeof(SharedFrameHeader));
constexpr uint32_t kHandsStateOffset =
    static_cast<uint32_t>(sizeof(SharedFrameHeader) + sizeof(HostState));

// Where a given buffer starts. Slots are padded to the maximum so the offset never depends on the
// resolution in flight -- a reader must be able to find a slot without knowing what is in it.
constexpr uint32_t slot_offset(uint32_t slot) {
    return kPayloadOffset + (slot % kFrameSlots) * kSharedFrameMaxBytes;
}

constexpr uint32_t kUiPayloadOffset = kPayloadOffset + kFrameSlots * kSharedFrameMaxBytes;

constexpr uint32_t ui_slot_offset(uint32_t slot) {
    return kUiPayloadOffset + (slot % kUiSlots) * kUiMaxBytes;
}

// THE WHOLE MAPPING. Named because both sides must agree on it: the writer creates the section this
// big and the reader maps exactly this much, and a disagreement is an access violation in whichever
// process guessed low.
constexpr uint32_t kSharedFrameTotalBytes = kUiPayloadOffset + kUiSlots * kUiMaxBytes;

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
