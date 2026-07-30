#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "../Mod.hpp"

// FEEDS THE ENGINE INPUT THAT DID NOT COME FROM THE OS.
//
// For a VR mod this is not a convenience: controller state arrives from a runtime, not from a window, and it
// has to reach the game as though a key or a button had been pressed. This mod is the scheduling half of that;
// sdk::Input::set_key_down is the primitive, and its comment carries the evidence for why this is the only
// path that works on this build.
//
// The short version, all measured rather than assumed:
//
//   * SendInput does NOT drive this game. It moves the cursor -- the main menu highlight follows it -- but no
//     click or key ever registers.
//   * The window-message key queue is inert. It is drained only when LTInput_IsBufferedKeyInputActive() is
//     true, and that reads FALSE both in play and at the main menu.
//   * The device array the input poll fills IS read, every frame, by everything that asks about a key.
//
// WHY THE SCHEDULING NEEDS A MOD AT ALL. CClientMgr::Update polls the device array (0x0040B75A) and then calls
// CClientShell::Update (0x0040B7AD). Our frame hook runs inside the second, so a write issued from on_frame
// lands after the poll that would have overwritten it and before anything reads the key that frame. Issued
// from the IPC thread instead, the same write races the poll and takes effect roughly at random.
//
// An EDGE needs two frames: key_just_pressed is `current && !previous`, and previous is filled by the poll
// from the state we wrote last frame. So a tap holds for N frames and then releases, and N=2 is the smallest
// value that reliably produces one edge for a consumer that samples once per frame.
class SyntheticInput final : public Mod {
public:
    static SyntheticInput& get();

    std::string_view get_name() const override { return "SyntheticInput"; }

    // Hooks the ILTInput device poll -- see the .cpp for why that, and not the frame hook.
    std::optional<std::string> on_initialize() override;

    // Deliberately empty: application happens in the poll detour, not here. Mods::on_frame runs off
    // CClientShell::Update, which does NOT execute at the main menu -- measured, frame_ticks delta 0 there
    // while the present path ran at 281/s. Scheduling input on a hook that stops at the menu would make this
    // mod silently inert in exactly the state a caller most often wants to drive.
    void on_frame() override {}

    void on_shutdown() override;

    // HOLD `vk` FOR `frames` FRAMES, THEN RELEASE. Returns false when the queue is full.
    //
    // Frame-counted rather than millisecond-counted on purpose: the consumer samples per frame, so a duration
    // in milliseconds would mean a different number of samples at 30fps than at 300, and the edge is what
    // matters rather than the wall time.
    bool tap(uint32_t vk, uint32_t frames = 2);

    // Hold or release without a schedule, for a consumer driving a continuous control (a trigger held while a
    // VR user holds it) rather than pressing a button. A held key is re-asserted EVERY frame, because the
    // engine's poll clears it otherwise.
    void hold(uint32_t vk, bool down);

    // Releases everything: every held key and every pending tap. Called on shutdown so uninjecting never
    // leaves a key stuck down in the engine's device array.
    void release_all();

    struct State {
        uint32_t active_taps{};   // taps still counting down
        uint32_t held_keys{};     // keys held indefinitely
        uint64_t writes{};        // key-state writes issued
        uint64_t taps_completed{};
        bool keyboard_resolved{}; // whether the device the writes target is reachable at all
    };

    State state() const;

    // MOUSE BUTTONS ARE CODES ABOVE THE VK RANGE: kMouseButton + 0/1/2 for left/right/middle. Virtual keys
    // stop at 0xFF, so these cannot collide, and one queue schedules both kinds.
    static constexpr uint32_t kMouseButton = 0x100;

    static constexpr size_t kMaxKeys = 16;

private:
    SyntheticInput() = default;
};
