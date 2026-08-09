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
// 5 moves the frame and UI slots to native per-eye resolution and maps each slot as its OWN VIEW
// (see kViewGranularity below) instead of one view over the whole section -- both the strides and
// every offset past the control block move, so a host and mod built against different versions of
// this file must refuse each other rather than read pixels at the wrong address.
// 6 adds the HapticsState block (game -> host). It does NOT move kPayloadOffset or the pixel
// slots: at 576 bytes the whole control block still sits far inside the 64 KiB view granularity,
// so the rounding absorbs it. What it DOES move is kUiStateOffset, because HapticsState is
// inserted ahead of the UI header -- which is precisely the mismatch below, a v5 reader taking
// haptic bytes for its UiFrameHeader.
//
// THE VERSION IS IN THE OBJECT NAMES, and a field check alone was not enough. CreateFileMapping
// with a name RETURNS THE EXISTING SECTION, so a reloaded mod re-stamped `version` in the section
// an already-connected host was reading; and the host only validated it inside SharedReader::open(),
// which returns early once it holds a mapping. The host deliberately outlives mod reloads, so a v5
// host would have gone on reading v6 haptics bytes as its UiFrameHeader -- garbage, not a refusal,
// which is precisely the failure the version field was added to prevent.
//
// Putting the number in the NAME removes the class instead of patching the path: a mod and host
// built against different versions never touch the same kernel object at all. The host reports no
// mapping, which is unambiguous and self-explanatory. The field check stays as belt and braces and
// is now re-validated per poll (tools/xr64/main.cpp), so a same-name mismatch still cannot be read.
#define FEAR2VR_SHARED_FRAME_VERSION 6
#define FEAR2VR_STRINGIFY_(x) #x
#define FEAR2VR_STRINGIFY(x) FEAR2VR_STRINGIFY_(x)

constexpr uint32_t kSharedFrameVersion = FEAR2VR_SHARED_FRAME_VERSION;  // 4 added the rendered pose

// Sized for the game's native per-eye back buffer: 4320x2224 (2160x2224 per eye, side by side),
// rounded up to a height of 2240 for headroom. Width and height are named rather than folded
// straight into the byte count because kUiMaxBytes below is stated as a fraction of THESE, not as
// an independently chosen pair that could quietly fall out of sync with them again.
constexpr uint32_t kFrameCapacityWidth = 4320u;
constexpr uint32_t kFrameCapacityHeight = 2240u;
constexpr uint32_t kSharedFrameMaxBytes = kFrameCapacityWidth * kFrameCapacityHeight * 4u;

// ---- WHY THERE IS MORE THAN ONE PIXEL BUFFER ----------------------------------------------------
//
// A sequence number protects the HEADER. It does not protect ~37 MB of pixels that the reader is
// still uploading: the host's copy out of the section takes ~2.5 ms and the game republishes every
// ~15 ms, so with a single buffer roughly one frame in six is caught mid-write and uploaded TORN --
// part of the previous frame, part of the next. Reported from the headset as "70% of frames are
// correct" with the rest looking stale or mismatched, which is exactly what a torn frame looks like
// when the two halves came from different head poses.
//
// Three buffers, not two: two is sufficient only while the reader always finishes inside one frame
// period, and the reader is a separate process that can be descheduled. The third costs ~37 MB and
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

// ---- THE OTHER OTHER DIRECTION: HAPTICS -----------------------------------------------------
//
// Game -> host, unlike HostState and HandsState. The mod decides a controller should buzz (a hit,
// a shot, a pickup); only the host holds the XrSession and can call xrApplyHapticFeedback.
//
// A RING, NOT A SLOT, and the reason is the difference between rumble and events. The first
// version of this block was one pulse per hand plus a serial the host fired on when it changed.
// That silently COALESCES: two shots landing between two host reads become one buzz, because the
// second overwrote the first before anyone looked. For a continuous rumble level last-write-wins
// is exactly right; for the discrete events this actually carries it is a dropped pulse, and a
// dropped pulse is indistinguishable from a bug in the weapon code.
//
// So: a single-producer single-consumer ring. The game fills slot[(ticket - 1) % kHapticSlots],
// stamps that slot with its ticket LAST, and only then publishes the ticket as `write_index`.
// The host keeps its own read cursor and consumes everything between the two.
//
// THE INDEX ALONE IS NOT ENOUGH, which is the subtle part. A consumer that snapshots
// `write_index` and then copies entries can still be lapped MID-COPY by a producer that wraps
// all the way round -- the snapshot was taken before the overwrite, so the backlog check cannot
// see it, and the consumer walks a slot whose payload is now half old and half new. That is a
// torn read, and it would surface as a haptic pulse with a garbage duration.
//
// Hence the per-slot `commit` stamp: the consumer reads it, copies the payload, reads it again,
// and accepts the entry only if BOTH reads equal the ticket it expected. A lap during the copy
// changes the stamp and the entry is dropped and counted instead of fired. This is the same
// discipline as the frame seqlock above, applied per ring slot rather than per block.
//
// ORDERING IS EXPLICIT, NOT `volatile`. MSVC's volatile happens to imply acquire/release under
// /volatile:ms, but that is a compiler setting and this block is read across a PROCESS boundary
// by a separately-compiled binary. The producer uses MemoryBarrier() between payload and stamp
// and InterlockedExchange to publish the index; the consumer barriers around its copy.
//
// OVERRUN IS COUNTED, NOT HIDDEN. If the game queues more than kHapticSlots between two host
// reads the oldest entries are genuinely gone -- the host detects it (write_index - read_index >
// kHapticSlots), skips to the newest full window and reports the drop.
//
// THE BACKLOG AT CONNECT IS DROPPED ON PURPOSE. The host seeds its read cursor from the first
// write_index it observes, so pulses queued before it was watching never fire. That is a real
// drop and it is the behaviour we want: a rumble for a shot fired before the compositor existed
// would arrive arbitrarily late, attached to nothing the wearer is doing.
//
// Values are OpenXR's own, so the host passes them straight through without a policy of its own:
// `duration_ns` is an XrDuration (XR_MIN_HAPTIC_DURATION, -1, asks for the shortest pulse the
// runtime can produce), `frequency_hz` of 0 is XR_FREQUENCY_UNSPECIFIED, and `amplitude` is [0,1].
constexpr uint32_t kHapticSlots = 16u;

struct HapticPulse {
    int64_t duration_ns;   // XrDuration; -1 (XR_MIN_HAPTIC_DURATION) = shortest the runtime can do
    float frequency_hz;    // 0 (XR_FREQUENCY_UNSPECIFIED) = the runtime chooses
    float amplitude;       // [0,1]
    uint32_t hand;         // kHandLeft or kHandRight -- carried per entry, not per ring
    uint32_t stop;         // non-zero: xrStopHapticFeedback instead of applying a pulse

    // The ticket this entry was written for, stamped AFTER the payload and set to ~ticket before
    // it. A consumer that sees anything other than the ticket it expected -- on either side of
    // its copy -- was lapped and must discard the entry rather than fire it.
    //
    // NOT zero for the in-progress marker: write_index wraps every 2^32 pulses, and on that one
    // ticket zero would mean both "being written" and "committed", so a torn entry would pass the
    // check. The complement can never equal the ticket.
    volatile uint32_t commit;
    uint32_t reserved;
};

static_assert(sizeof(HapticPulse) == 32, "HapticPulse must be byte-identical in both bitnesses");

struct alignas(64) HapticsState {
    int64_t write_qpc;

    // Total pulses ever queued, and the ticket of the newest. The game only ever INCREMENTS this,
    // via InterlockedExchange, and only after the slot it names carries its commit stamp. The
    // host diffs it against its own cursor with UNSIGNED subtraction, so the 2^32 wrap is free.
    volatile uint32_t write_index;
    uint32_t reserved[13];

    HapticPulse slot[kHapticSlots];
};

static_assert(sizeof(HapticsState) == 576,
              "HapticsState must be byte-identical in both bitnesses");

// ---- THE UI LAYER, WHICH IS A SECOND IMAGE ------------------------------------------------------
//
// The HUD is captured on its own transparent surface (src/mods/UICapture.hpp) and travels
// separately from the world, so the host can put it on a QUAD in front of the wearer instead of
// leaving it smeared across both eyes of a stereo image. Same discipline as the frame block: fixed
// width, asserted size, odd/even sequence, and its own slots so a reader is never inside a buffer
// the writer is filling.
//
// SMALLER THAN THE WORLD, on purpose -- and no longer a chosen pair of its own. UICapture reads the
// HUD off the same back buffer at HALF its dimensions (measured live: a 2560x1440 back buffer
// produced a 1280x720 layer), so this is derived from kFrameCapacityWidth/Height rather than stated
// independently -- an independent pair is exactly what went silently stale when the frame capacity
// above changed and this did not. The cost here is a per-frame GPU downscale plus a readback that is
// paid on the game's render thread.
constexpr uint32_t kUiMaxBytes = (kFrameCapacityWidth / 2u) * (kFrameCapacityHeight / 2u) * 4u;

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

// ---- EACH SLOT IS ITS OWN VIEW, NOT A SLICE OF ONE ------------------------------------------------
//
// At native per-eye resolution the whole section is over 120 MB, and a 32-bit process cannot always
// find that much CONTIGUOUS address space to map it as a single view -- a single 58 MB view has
// already failed here with MapViewOfFile ERROR_NOT_ENOUGH_MEMORY, logged as "[capture] cannot
// publish: MapViewOfFile failed (8)". Mapping each slot as its own view bounds the largest single
// reservation to one slot instead of the whole mapping, which is the actual constraint: the section
// itself (one CreateFileMapping call) is unaffected, only how much of it any one view has to cover.
//
// MapViewOfFile's offset argument must be a multiple of the system's allocation granularity (64 KiB
// on every Windows target this runs on, not the 4 KiB page size) -- so every per-slot offset below
// is rounded up to it, not just the section's total size.
constexpr uint32_t kViewGranularity = 64u * 1024u;

constexpr uint32_t align_up(uint32_t value, uint32_t granularity) {
    return (value + granularity - 1u) / granularity * granularity;
}

// Layout of the mapping:
//   [SharedFrameHeader][HostState][HandsState][HapticsState][UiFrameHeader][frames x3][ui x2]
//
// The UI header sits with the other headers rather than beside its pixels, so every fixed-size
// block stays in one contiguous run and only the payloads are resolution-sized.
//
// Each offset is stated as the sum of everything BEFORE it rather than as a literal, so inserting
// a block (HapticsState was inserted here, ahead of the UI header) moves everything after it by
// construction instead of by remembering to update a number.
constexpr uint32_t kHostStateOffset = static_cast<uint32_t>(sizeof(SharedFrameHeader));
constexpr uint32_t kHandsStateOffset =
    kHostStateOffset + static_cast<uint32_t>(sizeof(HostState));
constexpr uint32_t kHapticsStateOffset =
    kHandsStateOffset + static_cast<uint32_t>(sizeof(HandsState));
constexpr uint32_t kUiStateOffset =
    kHapticsStateOffset + static_cast<uint32_t>(sizeof(HapticsState));

// Rounded up to the granularity: this is now also a MapViewOfFile offset, since the first frame
// slot's view starts here. The control block above it (header through UiFrameHeader) is small
// enough to stay a single view mapped at offset 0, which needs no rounding.
constexpr uint32_t kPayloadOffset =
    align_up(kUiStateOffset + static_cast<uint32_t>(sizeof(UiFrameHeader)), kViewGranularity);

// The STRIDE between slots -- not kSharedFrameMaxBytes itself, which is the pixel capacity a frame
// is checked against. Padding it up to the granularity keeps every slot's own view offset
// (kPayloadOffset + N * kSlotStride) valid to hand to MapViewOfFile.
constexpr uint32_t kSlotStride = align_up(kSharedFrameMaxBytes, kViewGranularity);

// Where a given buffer starts. Slots are padded to the stride so the offset never depends on the
// resolution in flight -- a reader must be able to find a slot without knowing what is in it.
constexpr uint32_t slot_offset(uint32_t slot) {
    return kPayloadOffset + (slot % kFrameSlots) * kSlotStride;
}

constexpr uint32_t kUiPayloadOffset =
    align_up(kPayloadOffset + kFrameSlots * kSlotStride, kViewGranularity);

constexpr uint32_t kUiSlotStride = align_up(kUiMaxBytes, kViewGranularity);

constexpr uint32_t ui_slot_offset(uint32_t slot) {
    return kUiPayloadOffset + (slot % kUiSlots) * kUiSlotStride;
}

// THE WHOLE MAPPING. Named because both sides must agree on it: the writer creates the section this
// big and every view on either side must fit inside it, and a disagreement is an access violation
// in whichever process guessed low.
constexpr uint32_t kSharedFrameTotalBytes = kUiPayloadOffset + kUiSlots * kUiSlotStride;

// VERSION-SUFFIXED, so a mod and host built against different layouts cannot meet. See the note
// on kSharedFrameVersion: a bare name let a reloaded mod re-stamp the section a connected host
// was already reading.
constexpr const char* kSharedFrameName =
    "Local\\fear2vr_frame_v" FEAR2VR_STRINGIFY(FEAR2VR_SHARED_FRAME_VERSION);

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
// Suffixed for the same reason as the section, and it must move WITH it: a v5 host waking a v6
// mod's frame loop through a shared event, while unable to read its pixels, would be a worse
// failure than not meeting at all.
constexpr const char* kFrameTickEventName =
    "Local\\fear2vr_frame_tick_v" FEAR2VR_STRINGIFY(FEAR2VR_SHARED_FRAME_VERSION);

}  // namespace xr
