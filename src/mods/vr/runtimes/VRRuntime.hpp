#pragma once

#include <array>
#include <cstdint>
#include <shared_mutex>
#include <string_view>

// ---- THE RUNTIME ABSTRACTION ---------------------------------------------------------------
//
// Modelled on ue4poc's `VRRuntime`, trimmed to what this engine and this project actually use.
// The shape that matters is inherited wholesale: a base with sensible no-op defaults, so a new
// backend overrides only what it genuinely does, and consumers never branch on which one is live.
//
// WHAT IS DELIBERATELY NOT HERE. ue4poc keeps swapchains, `begin_frame` and `end_frame` in the
// derived OpenXR class rather than the base, and that turns out to be the single most useful
// property of the design: a runtime that never presents anything needs no swapchain, no session
// and no compositor. It fills poses and returns SUCCESS. That is what makes a simulated backend
// ~200 lines instead of a project.
//
// Also dropped: the `on_pre_render_{game,render,rhi}_thread` hooks (no LithTech analogue -- this
// engine has one render path, not UE's three) and `handle_pause_select` (an input policy, not a
// runtime concern).
//
// ---- COORDINATE SPACE, AND WHY IT IS OPENXR'S AND NOT THE ENGINE'S -------------------------
//
// Everything below is expressed in the RUNTIME's convention: right-handed, +X right, +Y up,
// -Z forward, metres. NOT the engine's. LithTech is left-handed with +Z forward and its own unit
// scale, and the conversion belongs to the consumer that touches the engine.
//
// That split is the whole point of the abstraction. If poses arrived pre-converted, the simulated
// backend would be defining engine semantics, and swapping in real OpenXR later would mean
// re-deriving them. Keeping runtime space here means a real backend is a drop-in and the
// conversion has exactly one implementation to get right -- and one place to test.
namespace vr {

// A tracked pose. `valid` says the numbers are meaningful at all; `tracked` says they came from
// real tracking rather than being held, extrapolated or synthesised -- the same distinction
// OpenXR draws with XR_SPACE_LOCATION_*_VALID_BIT versus *_TRACKED_BIT, and worth keeping because
// a consumer may reasonably want to ignore an untracked pose while still accepting a valid one.
struct Pose {
    std::array<float, 3> position{0.0f, 0.0f, 0.0f};
    std::array<float, 4> orientation{0.0f, 0.0f, 0.0f, 1.0f};  // x, y, z, w
    bool valid{false};
    bool tracked{false};
};

// One controller. AIM and GRIP are separate poses because they are separate things and conflating
// them is a classic VR bug: `aim` is the pointing ray a weapon should follow, `grip` is where the
// hand actually is. On a Touch controller they differ by roughly a 45 degree pitch, so using grip
// for the weapon makes every gun point at the floor.
struct HandState {
    Pose aim{};
    Pose grip{};

    float trigger{0.0f};        // [0,1]
    float squeeze{0.0f};        // [0,1]
    std::array<float, 2> thumbstick{0.0f, 0.0f};  // [-1,1] per axis

    // Bitmask rather than named bools: the set is runtime- and profile-dependent, and a consumer
    // asking "is A down" should go through a named accessor, not a struct field that only exists
    // on some controllers.
    uint32_t buttons{0};

    bool active{false};
};

struct VRRuntime {
    enum class Error : int64_t {
        UNSPECIFIED = -1,
        SUCCESS = 0,
        // Backend-specific codes continue from here.
    };

    enum class Type : uint8_t {
        NONE,
        SIMULATED,
        OPENXR,
    };

    enum class Eye : uint8_t {
        LEFT,
        RIGHT,
    };

    enum class Hand : uint8_t {
        LEFT,
        RIGHT,
    };

    static constexpr uint32_t kButtonA = 1u << 0;
    static constexpr uint32_t kButtonB = 1u << 1;
    static constexpr uint32_t kButtonX = 1u << 2;
    static constexpr uint32_t kButtonY = 1u << 3;
    static constexpr uint32_t kButtonThumbstick = 1u << 4;
    static constexpr uint32_t kButtonMenu = 1u << 5;

    virtual ~VRRuntime() = default;

    virtual std::string_view name() const { return "NONE"; }
    virtual Type type() const { return Type::NONE; }
    virtual bool ready() const { return m_loaded; }
    virtual void destroy() { m_loaded = false; }

    // ---- FRAME LIFECYCLE ------------------------------------------------------------------
    //
    // `synchronize_frame` is where a real backend blocks in xrWaitFrame; the simulated one uses it
    // to advance its own counter. Consumers call it once per game frame either way.
    virtual Error synchronize_frame() { return Error::SUCCESS; }
    virtual Error update_poses() { return Error::SUCCESS; }
    virtual Error update_input() { return Error::SUCCESS; }

    // ---- WHAT THE CONSUMER READS -----------------------------------------------------------
    //
    // Snapshots, not references. Poses are written by whichever thread drives the runtime and read
    // by the game thread and the IPC thread, so handing out a reference into shared state would be
    // a data race dressed as an accessor.
    Pose head() const {
        std::shared_lock lock{m_pose_mtx};
        return m_head;
    }

    HandState hand(Hand which) const {
        std::shared_lock lock{m_pose_mtx};
        return m_hands[static_cast<size_t>(which)];
    }

    uint64_t frame_count() const { return m_frame_count.load(std::memory_order_relaxed); }

    bool is_simulated() const { return type() == Type::SIMULATED; }
    bool is_openxr() const { return type() == Type::OPENXR; }

protected:
    mutable std::shared_mutex m_pose_mtx{};
    Pose m_head{};
    std::array<HandState, 2> m_hands{};

    std::atomic<uint64_t> m_frame_count{0};
    bool m_loaded{false};
};

} // namespace vr
