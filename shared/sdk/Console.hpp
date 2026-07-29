#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

//
// THE ENGINE'S CONSOLE COMMAND REGISTRY -- every command the running game will accept, including the ones
// registered by the game DLLs rather than the engine.
//
// WHY A CONSUMER WANTS THIS, AND WHY IT IS THE LIVE LIST THAT MATTERS. The executable's own table holds 34
// commands. The list the console actually dispatches against holds 118 on this build: the engine's 34 plus
// 84 registered at runtime from gameclient.dll and friends. The game-registered ones are the interesting
// half, and for a VR mod specifically they include the three that move a player's head:
//
//     GetPlayerPos, GetPlayerOrientation, SetPlayerOrientation
//
// alongside Teleport, PlayerLeash, RestartRender, RebindShaders, RebindMaterials, the OrbitalSS camera-marker
// commands, and an entire embedded command-script vocabulary (MSG, DELAY, LOOP, REPEAT, RAND, IF, SET, ADD,
// SUB, INT, OBJ, WHEN, SHOWVAR). None of those appear in the engine's table, and the engine's own
// ListCommands command cannot see them either -- it walks the static count, so it prints 34 of 118.
//
// That asymmetry is the whole reason this class walks the list instead of the table: a consumer asking "can
// this build do X" gets the wrong answer from the table for 84 of the 118 available commands.
//
// WHAT A COMMAND GIVES YOU. A name and a handler address, with the handler's owning module resolved. The
// handler is NOT called for you -- see the note on CommandHandler below, which is a warning, not a hedge.
//
// HOW THE CONTAINER WAS ESTABLISHED, since this project has mis-measured four containers by letting a scan's
// own cap set the boundary:
//   - The engine PUBLISHES the static count. `ListCommands` compares against a global, the initialiser
//     writes 34 into it as a literal, and an independent pattern scan of the table found 34 entries. Three
//     derivations, no cap involved.
//   - The live list is a circular intrusive list whose head is a field of the source descriptor, so it
//     terminates by returning to the head rather than by exhausting a limit. The caps below exist only so a
//     corrupted list cannot spin the game thread, and `Stats::hit_cap` reports if one is ever reached --
//     because a count equal to a self-imposed bound is not a measurement.
//   - Each node carries a BACK-POINTER to the object that owns it, written by the registrar. That makes the
//     walk self-checking: a node whose owner pointer does not point at the object the link sits inside is
//     rejected rather than reported. `Stats::inconsistent_nodes` counts those.
//

namespace sdk {

class Console {
public:
    // A descriptor entry in the engine's static tables: {const char* name, void* target, uint32 tag}. The
    // command table and the variable table (see EngineVars) share this shape, which is why one registrar
    // walks both.
    static constexpr size_t kDescriptorEntrySize = 12;

    // A live registry object, from the registrar's own LTMem_Alloc(0x18):
    //     +0x00 const char* name
    //     +0x04 void*       target        (handler for a command)
    //     +0x08 link.prev
    //     +0x0C link.next
    //     +0x10 link.owner  -- the object base, which is what makes the walk self-checking
    //     +0x14 uint32      flags         (copied from the descriptor's tag, uninterpreted)
    static constexpr size_t kLiveObjectSize = 0x18;

    // OBSERVED handler signature, from the handlers that use their arguments: ListResourcesOfType reads
    // argv[0] only when argc is non-zero, and Mem/quit/RestartRender ignore both.
    //
    // CALLING ONE OF THESE IS THE CONSUMER'S DECISION AND ITS RISK. A console handler runs engine work --
    // reloading resources, restarting the renderer, printing through the engine's log -- and it expects to be
    // on the thread the console normally dispatches from. Calling it from an injected thread is a real
    // possibility, not a supported one, and calling it with a wrong argument shape corrupts the game's stack
    // exactly as any other mis-shaped call does. This class hands over the address and the shape it was
    // observed to have; it deliberately does not wrap it, because a wrapper would imply a guarantee about
    // threading that no amount of reading disassembly can provide.
    using CommandHandler = void(__cdecl*)(int argc, char** argv);

    struct Command {
        uintptr_t object{};   // live object base, 0 for an entry read from the static table
        uintptr_t link{};     // the intrusive link inside it, 0 likewise
        std::string name;
        uintptr_t handler{};
        uint32_t flags{};

        // Which module implements the handler, resolved from the address. This is the field that separates
        // the engine's own commands from the game's, and it is asked of the OS rather than inferred from an
        // address range, so it also names modules this SDK does not track.
        std::string module;

        // Convenience for the common case, since "is this the engine or the game" is the usual question.
        bool from_exe{};

        // The handler, typed. Reading the warning on CommandHandler is the price of using it.
        CommandHandler as_handler() const { return reinterpret_cast<CommandHandler>(handler); }
    };

    // The source descriptor the engine builds at startup: {vars_table, vars_count, cmds_table, cmds_count,
    // link.prev, link.next}. Both of the engine's built-in tables hang off this one structure, and the
    // command list's head is a field inside it.
    static uintptr_t source_block();

    // The engine's own static command table and the count it publishes for it.
    static uintptr_t static_table();
    static std::optional<size_t> static_count();

    // Head of the circular list the console dispatches against. A walk ends by returning here.
    static uintptr_t list_head();

    // Caps, so a corrupted list cannot spin the game thread. The live figure is 118, far below these; if a
    // walk ever reaches one, Stats::hit_cap says so.
    static constexpr size_t kMaxCommands = 4096;

    // THE LIVE REGISTRY -- engine and game commands together, in reverse registration order (most recently
    // registered first), which is the order the list itself holds. Pass a non-zero limit to stop early.
    static std::vector<Command> all(size_t limit = 0);

    // The engine's 34, read from the static descriptor table instead of the list. Useful for exactly one
    // question -- "did the game register this, or was it always here" -- and for cross-checking the walk.
    static std::vector<Command> static_commands();

    // Exact match. Case-SENSITIVE, because the names in this registry are not uniformly cased (LISTCMDS and
    // ListCommands are different commands from different modules) and folding case would silently pick one.
    static std::optional<Command> find(std::string_view name);

    // Case-insensitive match, which is how a person types a command. Returns the FIRST match in list order
    // and reports nothing about ambiguity -- use find() when you mean a specific one.
    static std::optional<Command> find_insensitive(std::string_view name);

    // The handler for a name, typed, or nullopt when the command is absent from this build. Absent is a
    // legitimate answer: 84 of the 118 come from game DLLs, so the set depends on what is loaded.
    static std::optional<CommandHandler> handler_of(std::string_view name);

    struct Stats {
        size_t total{};                // nodes with a readable name
        size_t from_exe{};             // handler inside FEAR2.exe
        size_t from_modules{};         // handler in some other module -- the game DLLs
        size_t distinct_names{};       // fewer than total means the registry holds duplicates
        size_t unreadable_names{};     // nodes walked whose name pointer did not yield a name
        size_t inconsistent_nodes{};   // link.owner did not point at the object containing the link
        size_t nodes_walked{};         // including the ones rejected above
        bool hit_cap{};                // true means these are lower bounds, not counts
    };

    static std::optional<Stats> stats();

    //
    // HELPERS, exposed because a consumer holding a Command needs the same questions answered that this
    // class needed internally.
    //

    // Is this address inside the executable? The cheap version of the module question, for a caller that
    // only wants engine-versus-game and does not want to ask the OS.
    static bool address_in_exe(uintptr_t address);

    // Validate one live registry node without walking anything: checks the object's link geometry and that
    // its owner back-pointer agrees. This is what the walk uses to reject a bad node, and a consumer that
    // cached a Command across a level load can use it to find out whether the object is still there.
    static bool node_is_consistent(uintptr_t object);

    // Read one live object into a Command, or nullopt when it does not read as one. The name is validated
    // rather than trusted, so a wrong address yields nullopt instead of a Command holding binary.
    static std::optional<Command> read_object(uintptr_t object);
};

}  // namespace sdk
