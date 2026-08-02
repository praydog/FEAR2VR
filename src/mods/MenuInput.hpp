#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "../Mod.hpp"

// ---- DRIVING THE FRONT END ------------------------------------------------
//
// The main menu is Scaleform, and it ignores every input this project could
// previously produce. AGENTS.md records three measured failures -- SendInput, the
// window-message key queue, and the engine's own device array -- and the reason
// is the same for all three: the menu reads NONE of them. It reads a queue that
// `GFxMovieView::HandleEvent` fills and `Advance` drains.
//
// So this sends keys through HandleEvent itself (sdk::Events::send_key), which is
// where the engine's own input would arrive. Same principle as SyntheticInput
// driving the engine's mouse entry points rather than the OS: go one layer below
// the thing that is ignoring you, not one layer above.
//
// KEYS ARE QUEUED, NOT SENT. Scaleform is being asked to mutate its own state, so
// the call belongs on the engine's own thread; a request from the IPC thread would
// race the engine's Advance.
//
// Drained at the FRAME BOUNDARY, not from on_frame. `Mods::on_frame` is fanned out
// from CClientShell::Update, and that does not tick at the main menu -- measured,
// the VR frame counter reads +0 there while the renderer keeps presenting. Using
// it would have worked everywhere except the one screen this mod is for.
//
// A KEY IS TWO EVENTS. Down and up are separate, and a menu that only ever sees
// down will latch: the arrow repeats forever. The queue therefore holds both and
// releases on the following frame rather than back-to-back in one, because the
// movie samples its queue once per Advance and two events in a frame collapse.
class MenuInput final : public Mod {
public:
    static MenuInput& get();

    std::string_view get_name() const override { return "MenuInput"; }
    std::optional<std::string> on_initialize() override;
    void on_frame() override {}

    // Drains the queue. Runs on the frame boundary rather than on_frame -- see on_initialize().
    void service();

    // Reads the VR sticks and buttons and turns them into menu keys. Called by service().
    void poll_controller();
    void on_shutdown() override {}

    // Flash key codes, which match the Windows virtual keys for everything a menu needs.
    static constexpr uint32_t kUp = 38;
    static constexpr uint32_t kDown = 40;
    static constexpr uint32_t kLeft = 37;
    static constexpr uint32_t kRight = 39;
    static constexpr uint32_t kEnter = 13;
    static constexpr uint32_t kEscape = 27;
    // The WINDOWS virtual key, for the engine's own input rather than Flash's. They happen to
    // agree on 27, but they are different namespaces reaching different code and are named apart
    // so a later divergence cannot silently alias them.
    static constexpr uint32_t kVkEscape = 0x1B;

    // Drive the menu from the VR controllers while the front end is up. On by default: a headset
    // wearer has no keyboard, and the menu is the one screen they cannot get past without one.
    void set_controller_enabled(bool on) { m_controller.store(on, std::memory_order_release); }
    bool controller_enabled() const { return m_controller.load(std::memory_order_relaxed); }
    uint64_t controller_keys() const { return m_controller_keys.load(std::memory_order_relaxed); }
    uint64_t pause_presses() const { return m_pause_presses.load(std::memory_order_relaxed); }

    // What the mod is reading right now. Published because "the stick does nothing" has two very
    // different causes -- no controller data reaching us, or data that never crosses the threshold
    // -- and they need different fixes.
    // Whether a menu is considered up at all -- the gate that decides if sticks navigate.
    bool menu_up() const { return m_menu_up.load(std::memory_order_relaxed); }

    // Force the flat presentation on or off regardless of the menu gate. -1 is automatic.
    // Exists so the PRESENTATION can be judged separately from the DETECTION -- two things that
    // fail independently and, tested together, hide each other.
    //
    // IT EXPIRES. A debug override that outlives its test stops being a tool and becomes a bug
    // wearing the feature's clothes: this one was left forced ON, and the next report was "it stays
    // flat even when I unpause" -- a defect that did not exist. It reverts to automatic after
    // kOverrideFrames and says so in the log.
    void set_flat_override(int v);
    int flat_override() const { return m_flat_override.load(std::memory_order_relaxed); }
    bool hands_readable() const { return m_hands_ok.load(std::memory_order_relaxed); }
    bool left_active() const { return m_left_active.load(std::memory_order_relaxed); }
    float stick_x() const { return m_stick_x.load(std::memory_order_relaxed); }
    float stick_y() const { return m_stick_y.load(std::memory_order_relaxed); }

    // Queue one press-and-release. False when the queue is full.
    bool tap(uint32_t key_code);

    // Map a name a person would type. 0 when unrecognised.
    static uint32_t key_for(std::string_view name);

    uint64_t sent() const { return m_sent.load(std::memory_order_relaxed); }
    uint64_t refused() const { return m_refused.load(std::memory_order_relaxed); }
    bool movie_available() const { return m_movie_ok.load(std::memory_order_relaxed); }

private:
    MenuInput() = default;

    struct Pending {
        std::atomic<uint32_t> key{0};    // 0 = empty
        std::atomic<uint32_t> phase{0};  // 0 = send down next, 1 = send up next
    };

    static constexpr size_t kMaxPending = 8;
    std::array<Pending, kMaxPending> m_queue{};
    std::atomic<uint64_t> m_sent{0};
    std::atomic<uint64_t> m_refused{0};
    std::atomic<bool> m_movie_ok{false};
    std::atomic<bool> m_controller{true};
    std::atomic<uint64_t> m_controller_keys{0};
    std::atomic<uint64_t> m_pause_presses{0};
    std::atomic<bool> m_menu_up{false};
    std::atomic<int> m_flat_override{-1};
    std::atomic<uint32_t> m_override_frames{0};
    static constexpr uint32_t kOverrideFrames = 5000;  // roughly a minute -- long enough to look at
    std::atomic<bool> m_hands_ok{false};
    std::atomic<bool> m_left_active{false};
    std::atomic<float> m_stick_x{0.0f};
    std::atomic<float> m_stick_y{0.0f};

    // Stick and button edge state, frame-boundary thread only.
    //
    // A STICK IS NOT A BUTTON: held past the threshold it must produce ONE key, then repeat at a
    // readable rate, or a single nudge runs the whole menu past you. Frame counts rather than a
    // clock because this is already driven from the frame boundary.
    static constexpr float kStickOn = 0.6f;    // cross this to fire
    static constexpr float kStickOff = 0.35f;  // fall back inside this to re-arm -- hysteresis, or
                                               // a stick resting near the edge chatters
    static constexpr uint32_t kRepeatDelay = 28;   // frames before the first repeat
    static constexpr uint32_t kRepeatEvery = 9;    // and between repeats after that
    int32_t m_stick_dir{0};        // -1 up, 0 centred, +1 down
    int32_t m_stick_dir_x{0};
    uint32_t m_repeat_countdown{0};
    uint32_t m_last_buttons{0};
    uint32_t m_last_left_buttons{0};
};
