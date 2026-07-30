#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sdk {

// THE GAME'S UI COMMAND TABLE -- every action the Scaleform front end can trigger, callable directly.
//
// gameclient.dll builds an array of {const char* name, handler, uint8 flag} at startup (the stores are in
// UiPanel_Menu_Dispatch's init path) and dispatches menu clicks through it by name: "Menu.ContinueGame",
// "Menu.StartCheckpoint", "Menu.StartMission", "Menu.StartDLCMission" and the rest.
//
// ---- WHY THIS EXISTS ---------------------------------------------------------------------------------
//
// A development loop that survives a crash needs to put the game back into a playable state without a human.
// Every synthetic-input route was measured and none of them drives this main menu: SendInput moves the cursor
// and the highlight follows, but no click or key ever activates an item; the window-message key queue is not
// drained; and input written into the device array -- visible in the engine's own MouseState as a clean press
// and release -- is ignored by the menu too. The full evidence is in reversing/MAPPING_WORKFLOW.MD.
//
// The menu's own dispatch table has none of those problems, because it is where the click was going anyway.
//
// It is also the general lever: a mod can trigger front-end behaviour it has not reversed, by name.
//
// ---- WHAT THE HANDLERS DO ------------------------------------------------------------------------------
//
// The two that matter for reloading both funnel into one dispatcher, which is worth knowing because the
// console command that looks equivalent is NOT:
//
//   Menu.ContinueGame     -> ... -> LoadDispatcher(1, 0, 0, 0)
//   Menu.StartCheckpoint  -> ... -> LoadDispatcher(8, 0, 0, 0)   -- resolves the "Checkpoint" save, checks it
//                                                                   exists, then transitions
//   console "LoadCheckpoint" ->     LoadDispatcher(9, 0, 0, 0)   -- but ONLY from inside a loaded world; at
//                                                                   the menu it prints "You can only reload a
//                                                                   checkpoint from within the world" and
//                                                                   returns, which is exactly what it did.
//
// ---- CALLING CONVENTION AND THREAD --------------------------------------------------------------------
//
// __stdcall(int, int, int). The handlers examined ignore all three arguments -- they are the UI's panel and
// event parameters, and the load actions do not read them -- so 0, 0, 0 is what invoke() passes.
//
// MUST BE CALLED ON THE GAME THREAD. These reach into the player manager and the UI, neither of which is
// synchronised for a foreign caller. src/mods/ConsoleRunner queues by name and executes from the frame
// boundary; use it rather than calling a handler off the IPC thread.
class UiCommands {
public:
    using Handler = int(__stdcall*)(int, int, int);

    struct Command {
        std::string name;
        uintptr_t handler{};
        uint8_t flag{};
        uintptr_t entry{};  // the table row, for cross-checking against a disassembler

        Handler as_handler() const { return reinterpret_cast<Handler>(handler); }
    };

    // Address of the first row, or 0 when gameclient.dll is absent or the anchor string is not in this build.
    //
    // Resolved by finding a POINTER to a known command name rather than by a byte pattern: the table is filled
    // by a long run of `mov` stores, so a signature over it would encode link-time offsets, while the name
    // strings are stable content.
    static uintptr_t table_address();

    // Every row, in table order. Empty when the table did not resolve.
    static std::vector<Command> all();

    // Case-insensitive, because these are typed by a person as often as by code.
    static std::optional<Command> find(std::string_view name);

    // CALLS the handler. Returns false when the command is unknown or the table did not resolve.
    //
    // GAME THREAD ONLY -- see the class comment. Nothing here can enforce that, so it is stated rather than
    // pretended: a wrapper that took a lock would be inventing a guarantee the engine does not offer.
    static bool invoke(std::string_view name);

    // Guard rails for the walk: the live table is small, and a corrupted read must not spin.
    static constexpr size_t kMaxCommands = 256;
    static constexpr size_t kEntryStride = 12;
};

}  // namespace sdk
