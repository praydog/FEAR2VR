#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "../Mod.hpp"

// KEEPS THE WORLD RUNNING WHILE THE DESKTOP WINDOW IS NOT ACTIVE.
//
// ---- WHAT ACTUALLY FREEZES, MEASURED RATHER THAN ASSUMED --------------------------------------------------
//
// Alt-tabbed, with the payload injected and reporting:
//
//     client_active      True     <- the engine still considers itself ACTIVE
//     lost_focus         True     <- main loop takes Sleep(5), so it runs ~118/s instead of ~300/s
//     eng_clock_paused   TRUE     <- scale 0.0, millisecond accumulator dead flat over 2 seconds
//     UpdateViewPose     0 calls  <- nothing drives the view, because nothing drives time
//     frame hook         236 ticks in 2s -- still running the whole time
//
// So the world does not stop because the engine thinks it is inactive. It stops because THE GAME PAUSES THE
// ENGINE'S TIMER. `ILTTimer::SetPaused` writes a pause byte on the timer node; the tick then multiplies its
// delta by a scale of zero and the clock stands still. A hardware write watch on that live byte named the
// store, and the stack above it named gameclient.dll frames -- the policy is the game DLL's.
//
// ---- WHY THIS HOOKS AN API INSTEAD OF WRITING A FLAG ------------------------------------------------------
//
// The first attempt forced `g_ClientGlob_bClientActive` true from the per-frame hook. That was measured to be
// both useless and dangerous: 2473 re-asserts across 18 seconds, the flag never once observed cleared, the
// engine clock advancing by 0.000 -- and then the process HUNG, because holding it true runs the DirectInput
// poll and the render path against a window that does not have focus, every iteration.
//
// The lesson is the one already recorded for the camera rotation in reversing/ENGINE_NOTES.md: when a
// value is recomputed or rewritten by an owner, fighting the store loses. Intercept the CALL that decides.
// So this refuses the pause request at `ILTTimer::SetPaused`, which is one function, the engine's own API, and
// the exact point where the decision becomes a memory write.
//
// ---- WHAT IS DELIBERATELY LEFT ALONE ---------------------------------------------------------------------
//
// `lost_focus` is NOT cleared, though clearing it would remove the main loop's Sleep(5) and restore the full
// ~300/s. Two reasons: the same flag gates cursor centring, so clearing it warps the mouse into whatever
// window the user is actually working in; and ~118 iterations a second is far more than enough for a world to
// tick. Losing frame rate while alt-tabbed is not a problem worth creating a cursor-stealing bug to solve.
//
// A pause requested while the window IS focused -- the pause menu -- is passed through untouched. Suppressing
// that would be a behaviour change nobody asked for, and the discriminator is free: the engine already
// publishes its own focus latch.
class FocusKeeper final : public Mod {
public:
    static FocusKeeper& get();

    std::string_view get_name() const override { return "FocusKeeper"; }

    std::optional<std::string> on_initialize() override;
    void on_frame() override {}
    void on_shutdown() override {}

    // Start or stop refusing focus-loss pauses. Idempotent. Off by default: this changes engine behaviour, and
    // AGENTS.md's rule is that a mutation is opt-in and visible.
    void keep_running(bool on);

    struct State {
        bool enabled{};
        bool hook_installed{};
        uintptr_t set_paused_fn{};
        uint64_t pause_requests{};    // every SetPaused(true) that reached the detour
        uint64_t suppressed{};        // those refused because the window was not focused
        uint64_t passed_through{};    // those honoured, i.e. a real in-game pause
        bool window_active{};         // the engine's own flag, unmodified
        bool lost_focus{};            // the latch this decision reads
    };

    State state() const;

private:
    FocusKeeper() = default;
};
