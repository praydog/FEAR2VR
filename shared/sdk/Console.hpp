    //
    // WHAT SLOT 69 RETURNS IS AN LTConVar*, AND THERE ARE TWO TABLES OF THOSE.
    //
    // No variable getter lives here: sdk::Engine::console_var reads these records, and the LTConVar layout was
    // already mapped in fear2.genny before this pass re-derived it (and got +0x08 wrong -- it is the second
    // half of the intrusive link at +0x04, whose next points back at a bucket, not an "owning source").
    //
    // But the two routes are NOT interchangeable, and nearly deleting one as a duplicate of the other was the
    // mistake worth recording. Two 128-bucket tables of identical shape exist:
    //
    //     console source        535 entries   ScreenWidth, ApplyWorldOffset      <- what slot 69 searches
    //     CClientMgr + 0xC0     192 entries   HDR_Blur, Spawn_Debug              <- what GetSConValueFloat does
    //
    // Their populations overlap (47 names) and NEITHER contains the other, proven both ways: ScreenWidth is
    // absent from CClientMgr's and HDR_Blur is absent from the source. One finder serves both --
    // ConVarTable_FindInBucket is __thiscall and the table base arrives in ECX, which flows untouched through
    // two intermediate functions the decompiler types as taking it on the stack. That is why the base looked
    // "ignored" and why one table looked like the whole story.
    //
    // sdk::Engine::console_var searches both; console_source_vars() and console_vars() reach them separately.
    // What remains genuinely new here is the API: slot 69 is the engine's PUBLIC lookup, it hands back that
    // record type, and the record's address is what makes a variable WRITABLE since its first field is the
    // float the engine reads. Slot 71 is nothing but `*(float*)record`.
    //
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

    // Set on entries created through ILTClient::RegisterConsoleProgram; clear on the engine's built-in
    // descriptors. See Command::registered_at_runtime for the measurement.
    static constexpr uint32_t kFlagRuntimeRegistered = 1;

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

        // WHERE THIS COMMAND CAME FROM, and it is a measured partition rather than a reading of the bit's
        // name. RegisterConsoleProgram passes a literal 1 as the entry's flags, while the engine's static
        // descriptors carry a tag of 0. Live that predicts the split exactly: 34 entries with flags 0, all
        // inside the exe -- precisely the published static count -- and 84 with flags 1.
        //
        // The 84 are not all the game's: 77 have handlers in game DLLs and SEVEN are in the exe, which is
        // the engine registering through its own public API rather than through the table. So this bit says
        // HOW an entry arrived, and from_exe says WHO implements it; they are different questions.
        bool registered_at_runtime() const { return (flags & kFlagRuntimeRegistered) != 0; }

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

    // ---- WHO REGISTERED IT, WHICH THE LIVE LIST CANNOT KNOW -------------------------------------
    //
    // A live entry records its name, handler and flags. It does NOT record the function that created it, and
    // that function is what identifies a SUBSYSTEM: CMoveMgr was found because one function registers exactly
    // the five programs the reference's CMoveMgr::Init registers.
    //
    // So this is a static table, swept from gameclient's registration sites: 79 registrations across 11
    // registrars. It is gameclient only -- the exe's own commands and any gameserver ones are not in it, and
    // registrar_of returns nothing for those rather than guessing.
    //
    // THE SWEEP TOOK TWO ATTEMPTS AND THE FIRST WAS WRONG IN AN INSTRUCTIVE WAY. A registration is
    // `push handler; push name; call reg`, and the first version identified the two pushes by asking which
    // operand looked like a string. IDA's get_strlit_contents answered "yes" for a HANDLER address -- the code
    // bytes at 0x100F6680 read as the one-character string "Q" -- so that site was discarded and CPlayerStats
    // appeared to register four programs rather than five. Position, not content, tells the two pushes apart.
    // Counted 71 that way.
    //
    // THE SECOND ATTEMPT WAS ALSO SHORT, BY THREE, and the residue named them. Requiring the two pushes to be
    // ADJACENT misses the shape the compiler actually emits sometimes:
    //
    //     push offset handler
    //     mov  edx, [eax+134h]      <- the slot load sits BETWEEN the pushes
    //     push offset name
    //     call edx
    //
    // GetPlayerOrientation and LOOPID are registered that way, and SpawnLaserSight has its handler pushed
    // further back still. Counted 76 that way, and 79 once the arguments are collected across intervening
    // instructions up to the call. Three counts of one set, each looking complete.
    struct Registrar {
        uintptr_t offset;   // gameclient-relative
        const char* name;   // the function's role where established, else nullptr
        size_t count;       // how many programs it registers
    };

    // The ten registering functions, most prolific first.
    static const Registrar* registrars(size_t& count);

    // Which function registered this command, or nullptr when the command is not one of gameclient's 76.
    static const Registrar* registrar_of(std::string_view name);

    // Every command a given registrar creates, by gameclient-relative offset. Empty for an unknown offset.
    static std::vector<std::string> commands_registered_by(uintptr_t registrar_offset);

    // ---- COMMANDS THAT DO NOTHING --------------------------------------------------------------
    //
    // gameclient's retn-only functions were all folded onto one address by /OPT:ICF -- see CClientShell.hpp,
    // which found it from vtable slots. FIVE REGISTERED CONSOLE PROGRAMS have that address as their handler:
    // AIDebug, ForceSpectatorClipMode, PrintCollisionProperties, RebindFX and ReportMemory. Their bodies were
    // compiled out of this retail build and the empty remains merged.
    //
    // That matters to a consumer in a way a name never would: these commands EXIST, resolve, and accept
    // arguments, and calling them has no effect whatsoever. Anything driving the game through the console needs
    // to know which of its levers are disconnected.
    static constexpr uintptr_t kEmptyStubOffset = 0x0F6680;  // gameclient-relative

    // Runtime address of the folded empty stub, 0 when gameclient is not mapped.
    static uintptr_t empty_stub();

    // Does this command resolve to the folded stub -- i.e. is it a no-op? Read from the LIVE handler, so it
    // also answers for commands outside the static table. nullopt when the command does not exist.
    static std::optional<bool> is_noop(std::string_view name);

    // Every live command whose handler is the folded stub. The five above when only gameclient is mapped.
    static std::vector<std::string> noop_commands();

    // ---- WHAT THE SWEEP DID NOT FIND, MEASURED RATHER THAN ASSUMED -----------------------------
    //
    // Live, 77 commands have handlers in gameclient and all 77 are now attributed: the table's 79 entries
    // cover them plus two that are not currently registered. The residues below are what remains measurable,
    // and unattributed_commands() returning anything means the sweep has gone stale against a new build.
    //
    // The other direction is a different fact: NextSpawnPoint and PrevSpawnPoint are in the table and NOT live,
    // because the function registering them has not run in this session. So the table is what the code CAN
    // register and the live list is what it HAS, and a consumer must not read either as the other.
    //
    // Both residues are exposed so they stay measured. Three counts were needed to get this right -- 71, then
    // 76, then 79 -- and each intermediate looked complete, which is why these are functions and not a comment.

    // Live commands implemented in gameclient with no registrar in the static table.
    static std::vector<std::string> unattributed_commands();

    // Table entries that are not currently registered.
    static std::vector<std::string> unregistered_table_commands();

    struct Stats {
        size_t total{};                // nodes with a readable name
        size_t from_exe{};             // handler inside FEAR2.exe
        size_t from_modules{};         // handler in some other module -- the game DLLs
        size_t distinct_names{};       // fewer than total means the registry holds duplicates
        size_t unreadable_names{};     // nodes walked whose name pointer did not yield a name
        size_t inconsistent_nodes{};   // link.owner did not point at the object containing the link
        size_t nodes_walked{};         // including the ones rejected above
        size_t builtin{};              // flags == 0: the engine's static descriptors
        size_t runtime{};              // flags == 1: registered through RegisterConsoleProgram
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

    //
    // THE ENGINE'S OWN CONSOLE API, which is what a mod actually wants: not just to READ the registry but to
    // set a variable by name and to add its own command.
    //
    // These are ILTClient methods, and the slots are not positional guesses -- gameclient.dll calls exactly
    // these three, and each implementation in the exe was read to confirm what it does:
    //
    //   slot 69 (vtbl+276)  ConVarTable_Find(&source, name)              -- find a variable by name
    //   slot 73 (vtbl+292)  set(&source, name, float)                    -- set one, returns 60 on a null name
    //   slot 77 (vtbl+308)  CLTClient_RegisterConsoleProgram(name, fn)   -- add a command
    //
    // Slot 77 closes the loop on the count asymmetry: its body calls the SAME LTConsole_CreateEntry that
    // builds the live objects this class walks, with flags 1. So the 118-versus-34 gap is a proven call
    // path, not an inference from two numbers. It refuses a duplicate name with error 62 and reports
    // LT_INVALIDPARAMS (60) for a null one.
    //
    // A CALLING-CONVENTION FINDING THAT MATTERS TO A CALLER. The game invokes these through the vtable as
    // __thiscall, passing the interface in ECX -- but all three implementations take their arguments from
    // the STACK and never read the incoming ECX (every ecx touch is a push/pop stack idiom, and slot 69
    // ends in `retn 4`). They ignore the instance entirely, because they operate on a global source table
    // rather than on interface state. So the signatures below are __stdcall and need no `this`.
    //
    // THREADING IS STILL THE CALLER'S PROBLEM. Reading a variable is a table lookup; SETTING one and
    // REGISTERING a command both mutate engine-owned structures -- the registrar allocates through
    // LTMem_Alloc and links into a list the console walks. Doing that from an injected thread while the
    // game thread reads the same list is a race this SDK cannot make safe, which is why these are handed
    // over as typed pointers rather than wrapped in tidy methods.
    //
    static constexpr size_t kSlotFindVariable = 69;
    static constexpr size_t kSlotSetVariableFloat = 73;
    static constexpr size_t kSlotRegisterProgram = 77;

    using FindVariableFn = void*(__stdcall*)(const char* name);
    using SetVariableFloatFn = int(__stdcall*)(const char* name, float value);
    using RegisterProgramFn = int(__stdcall*)(const char* name, CommandHandler handler);

    // Documented error codes, from the implementations' own early-outs.
    static constexpr int kInvalidParams = 60;
    static constexpr int kAlreadyExists = 62;

    // The live ILTClient instance, via the interface registry. 0 when it has not been resolved yet.
    static uintptr_t client_interface();

    // One slot of the live ILTClient vtable, or 0. Exposed because a consumer wanting a method this class
    // does not name should read it the same way rather than recomputing the chase.
    static uintptr_t client_vtable_slot(size_t slot);

    // The three above, typed. nullopt when the interface is unresolved or the slot does not point into the
    // executable -- a slot leading elsewhere would mean the layout assumption is wrong, and handing that
    // back as a callable is worse than refusing.
    static std::optional<FindVariableFn> find_variable_fn();
    static std::optional<SetVariableFloatFn> set_variable_float_fn();
    static std::optional<RegisterProgramFn> register_program_fn();

    //
    // WHAT SLOT 69 RETURNS IS AN LTConVar*, WHICH THIS PROJECT ALREADY HAD MAPPED.
    //
    // No variable getter lives here, deliberately. sdk::Engine::console_var / console_vars already read these
    // records -- 192 of them live -- by walking the 128-bucket LTConVarTable at CClientMgr+0xC0, which is
    // how the engine's own GetSConValueFloat reaches them. That is the route to use.
    //
    // I MAPPED THE RECORD A SECOND TIME BEFORE NOTICING, and got a field wrong that the existing mapping had
    // right: the dword at +0x08 is the second half of the intrusive link at +0x04, and its `next` points back
    // at a bucket inside CClientMgr. I read it as an "owning source" pointer because it landed in the exe's
    // data. See the LTConVar class in fear2.genny for the fields, all confirmed there first.
    //
    // WHAT IS ACTUALLY NEW HERE is the connection: slot 69 is the engine's PUBLIC lookup and it hands back
    // exactly that record type, so a caller holding an ILTClient can obtain an LTConVar* by name without
    // walking the table -- and the record's address is what makes a variable WRITABLE, since its first field
    // is the float the engine reads. Slot 71 is nothing but `*(float*)record`.
    //
};

}  // namespace sdk
