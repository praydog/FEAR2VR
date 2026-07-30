#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "../Mod.hpp"

// RUNS THE GAME'S OWN CONSOLE COMMANDS, ON THE GAME'S OWN THREAD.
//
// The point is a development loop that survives a crash without a human: relaunch, inject, and put the game
// back into a playable state by asking the game to do it. That turned out to be the only workable answer --
// every synthetic-input route was measured and none drives the main menu (see reversing/MAPPING_WORKFLOW.MD),
// while the commands the menu itself ultimately invokes are right there in the console registry.
//
// It is also the more useful artefact. A console command is how a mod triggers engine behaviour it has not
// reversed: sdk::Console maps 118 live commands with their handlers, and this makes all of them callable.
//
// ---- THREAD AFFINITY IS THE WHOLE DESIGN -----------------------------------------------------------------
//
// sdk::Console::handler_of() deliberately hands out a raw function pointer and refuses to wrap it, because
// nothing in the disassembly can promise which thread a command handler is safe on. The answer is not to guess
// but to call it where the engine calls it: on the game thread.
//
// So a request only ENQUEUES. The command is executed from a RenderHook present callback -- the frame boundary
// runs on the render thread at the main menu AND in play, which Mods::on_frame does not (measured: frame_ticks
// delta 0 at the menu while the present path ran 562 times in the same window).
//
// ---- WHAT A HANDLER EXPECTS --------------------------------------------------------------------------------
//
// void __cdecl(int argc, char** argv), with argv[0] the command name, exactly as the engine's own dispatcher
// builds it. Arguments are split on spaces; there is no quoting, because no command this is aimed at takes an
// argument containing one, and inventing a quoting dialect the engine does not have would be worse than not
// supporting it.
class ConsoleRunner final : public Mod {
public:
    static ConsoleRunner& get();

    std::string_view get_name() const override { return "ConsoleRunner"; }

    std::optional<std::string> on_initialize() override;
    void on_frame() override {}
    void on_shutdown() override {}

    // QUEUES a command line ("name arg1 arg2"). Returns false when the queue is full or the line is empty.
    //
    // Does NOT report whether the command exists -- that is resolved on the game thread when it runs, and
    // reporting it here would mean a second registry lookup whose answer could differ by the time it matters.
    // last_result() carries the outcome.
    bool queue(std::string_view command_line);

    enum class Outcome : uint8_t {
        None,        // nothing has run yet
        Ran,         // a handler was found and called
        NotFound,    // no such command in the live registry
        NoHandler,   // present in the registry with a null handler
    };

    struct State {
        uint32_t pending{};
        uint64_t executed{};
        Outcome last{Outcome::None};
        std::string last_command;
        bool callback_registered{};  // false means nothing will ever drain the queue
    };

    State state() const;

    // Drains and executes. Public because the present callback is a free function that has to reach it, and
    // because a caller with its own game-thread tick may prefer to drive it directly. MUST be called on the
    // game thread; calling it from the IPC thread is the bug this class exists to avoid.
    void run_pending_on_game_thread();

    static constexpr size_t kMaxQueued = 8;
    static constexpr size_t kMaxCommandLength = 128;

private:
    ConsoleRunner() = default;
};
