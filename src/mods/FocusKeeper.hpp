#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "../Mod.hpp"

// HOLDS SIMULATION ON WHILE THE DESKTOP WINDOW IS NOT ACTIVE.
//
// This engine stops simulating when it loses focus, and `g_ClientGlob_bClientActive` is the single flag that
// does it. CClientShell::Update reads it three times -- an idle flag it passes on, then two branches that SKIP
// the simulation step and the transform interpolation -- while the IClientShell Pre/Update/Post callbacks run
// unconditionally. CClientMgr::Update separately gates the ILTInput poll on `active && !buffered`.
//
// That asymmetry is why a frame hook keeps firing at ~170 Hz against a frozen world, and it is a practical
// problem for this project rather than a curiosity: every measurement taken while the tester reads output in
// another window is taken against a stopped game. Half the "inconclusive" results in this project's history are
// that.
//
// IT MUST BE RE-ASSERTED, NOT SET ONCE. LTClient_WndProc's WM_ACTIVATEAPP handler clears the flag whenever it
// arrives while minimised or with a lost device, so a one-shot write is undone by the next message. Writing it
// from the per-frame hook wins because our hook runs on the pumping thread whether or not the world ticks.
//
// FOR VR THIS IS NOT A TEST CONVENIENCE. A headset does not care whether the desktop window has focus, and a
// mod that lets the engine stop simulating on focus loss is a mod that freezes the moment the user looks at
// something else on their desktop.
//
// KNOWN HAZARD, deliberately not hidden: the same flag gates the input poll, so holding it true can let the
// engine read the keyboard and mouse while another window has focus. Whether it actually does depends on how
// the device backend handles a non-foreground window, and the honest answer is a measurement rather than a
// guess -- input_leaked() reports whether look input arrived while the window was NOT active.
class FocusKeeper final : public Mod {
public:
    static FocusKeeper& get();

    std::string_view get_name() const override { return "FocusKeeper"; }

    std::optional<std::string> on_initialize() override;

    // Re-asserts the flag when held. Runs on the engine's pumping thread, which continues while the world does
    // not -- that is precisely why this works.
    void on_frame() override;

    void on_shutdown() override {}

    // Start or stop holding. Idempotent; `false` restores nothing because there is nothing to restore -- the
    // engine's own WndProc owns the flag again the moment we stop writing it.
    void hold(bool on);

    struct State {
        bool holding{};
        bool flag_resolved{};
        uintptr_t flag_address{};
        uint64_t writes{};          // frames we re-asserted the flag
        uint64_t observed_cleared{};// frames we found it CLEARED and had to put it back
        bool window_active{};       // the engine's own view of focus, unmodified
        // Look events that arrived while the window was NOT active. Non-zero means holding the flag also let
        // input through, which a tester needs to know before trusting a session.
        uint64_t input_while_inactive{};
    };

    State state() const;

private:
    FocusKeeper() = default;
};
