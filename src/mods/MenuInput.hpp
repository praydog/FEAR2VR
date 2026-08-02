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
    void on_shutdown() override {}

    // Flash key codes, which match the Windows virtual keys for everything a menu needs.
    static constexpr uint32_t kUp = 38;
    static constexpr uint32_t kDown = 40;
    static constexpr uint32_t kLeft = 37;
    static constexpr uint32_t kRight = 39;
    static constexpr uint32_t kEnter = 13;
    static constexpr uint32_t kEscape = 27;

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
};
