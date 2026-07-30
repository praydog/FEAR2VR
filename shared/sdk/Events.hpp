#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

//
// THE GAME CALLING ITS FLASH UI -- Scaleform ActionScript invocation, by name and with a TYPED payload.
//
// THIS WAS FIRST DOCUMENTED HERE AS "the game's named event bus", which was directionally right and
// mechanically wrong. The sender's own error string names what it is:
//
//     "[UI - ActionScript]: Error - Invoke called for Monolith.I%sEvents.%s without a path to the
//      implementation object."
//
// So the CATEGORY fills that %s to form an ActionScript interface name -- Monolith.IPlayerEvents -- and each
// "event" is a method call into the Flash HUD. Not a C++ observer bus at all.
//
// WHAT THE SENDER DOES, which is what makes the payload readable:
//   * `source` is a STRING, not an object: strncpy_s copies it and the guard dereferences it as a char. It is
//     the dot-separated PATH to the implementation object on the AS side, and it lives at a1 + 0x10 of
//     whatever object the dispatcher was handed.
//   * it builds "<path>.<EventName>", falling back to "<path>.Default" when no name is supplied
//   * it marshals the format string and the argument block into a local value array
//   * it invokes SLOT 14 of the target -- the GFx movie/value object -- with that name and array
//
// Each event still has a one-line dispatcher, 87 of them, so each remains a precise hook point. Harvested from
// the binary the payload alphabet is d = int, f = float, s = string, b = bool:
//
//     HealthChanged        "d"     one int
//     FlashlightMaxCharge  "f"     one float, EIGHT bytes on the stack -- see payload_stack_bytes
//     AmmoCountChanged     "sdd"   a string and two ints
//     ShowCrosshair        "b"     one bool
//
// THIS IS A SEPARATE MECHANISM FROM THE DELEGATE CHANNELS in sdk::Delegates, and the two should not be
// confused. A delegate channel is an intrusive list of C++ objects on a subject; this is a name-and-format
// call into Flash. The player object carries 21 of the former; the events below are the latter.
//
// WHY A CONSUMER WANTS IT. These names are the game's own vocabulary for the state a mod cares about --
// health, armour, ammo, weapon selection, slow-mo, player alive/dead -- and hooking a dispatcher intercepts
// the game TELLING THE HUD about a change, which is both a notification and the point at which a stereo
// consumer can suppress or redirect a HUD element.
//
// THE CATALOGUE IS CURATED, NOT COMPLETE: the player and HUD events are here because those are the ones a mod
// reaches for. The rest (achievements, menus, matchmaking, CTF, control points, PDA) exist in the binary and
// are deliberately not transcribed, because an entry nobody checks is a claim nobody maintains.
//
// AND IT VERIFIES ITSELF AGAINST THE BINARY. Every dispatcher pushes its own event-name string, so
// verify() reads the function's bytes, follows each immediate that looks like a pointer, and requires one of
// them to spell the event's name. A build that moved a function or renamed an event fails that check rather
// than handing a consumer a stale hook address.
//

namespace sdk {

class Events {
public:
    enum class Category {
        Player,
        Menu,
    };

    struct Event {
        const char* name;
        Category category;
        const char* payload;  // "" when the event carries none
        uintptr_t offset;     // within gameclient.dll
    };

    // The curated set. Stable order, so an index into it is usable as an id.
    static const std::vector<Event>& all();

    // By exact name. Case-sensitive: these are the literals the binary contains.
    static std::optional<Event> find(std::string_view name);

    // Runtime address of an event's dispatcher, or 0 when gameclient.dll is not mapped or the name is
    // unknown. This is the hook point.
    static uintptr_t dispatcher(std::string_view name);

    //
    // HELPERS. The payload format is the part a consumer has to act on, so parsing it belongs here rather
    // than in every caller.
    //

    // How many arguments the payload describes. 0 for an empty or null format.
    static size_t payload_arg_count(std::string_view payload);

    // Is every character one this bus uses? The alphabet is the MARSHALLER's switch, not the set of letters
    // that happen to appear in the catalogue: b, d, f, s and w. An earlier version omitted 'w' and would have
    // rejected a format the game supports.
    static bool payload_is_well_formed(std::string_view payload);

    //
    // THE WIRE FORMAT, read off the marshaller rather than inferred from stack sizes.
    //
    // Each argument is preceded by its OWN TYPE TAG, and the list ends with a terminator. The marshaller
    // REFUSES the call if a tag does not match the format letter, so these are not decoration:
    //
    //     b   tag 0x12345678   one dword, non-zero test   -> GFx value type 2 (bool)
    //     d   tag 0x12345678   one dword, widened         -> type 3 (number)
    //     f   tag 0x12345679   a DOUBLE, eight bytes      -> type 3 (number)
    //     s   tag 0x1234567A   one pointer                -> type 4 (string)
    //     w   tag 0x1234567C   one pointer                -> type 5 (wide string)
    //     end tag 0x1234567D
    //
    // AN EARLIER PASS CALLED THESE "a marker before the payload and a second marker after". That reading fits
    // a one-argument event perfectly and is wrong for every other: at n = 1 a per-argument tag and a bracket
    // are indistinguishable, and both events measured then had one argument.
    static constexpr uint32_t kTagInt = 0x12345678;  // also bool
    static constexpr uint32_t kTagFloat = 0x12345679;
    static constexpr uint32_t kTagString = 0x1234567A;
    static constexpr uint32_t kTagWideString = 0x1234567C;
    static constexpr uint32_t kTagEnd = 0x1234567D;

    // Each marshalled output slot is 16 bytes: the GFx value type at +0 and the value at +8.
    static constexpr size_t kValueSlotBytes = 16;

    // The tag a caller must push before an argument of this letter, or 0 for an unknown letter. A consumer
    // building its own invoke needs this, and the marshaller rejects a mismatch outright.
    static uint32_t tag_for(char letter);

    // The GFx value type a letter marshals to, or 0 for an unknown letter.
    static uint32_t value_type_for(char letter);

    // Total bytes the payload occupies as pushed arguments.
    //
    // A FLOAT IS EIGHT BYTES HERE, NOT FOUR, and an earlier version of this returned four for every letter.
    // The call sites settle it: these dispatchers are reached through a VARIADIC sender, so a float is
    // promoted to double exactly as C varargs requires. Measured on two events whose frames are visible in
    // the disassembly --
    //
    //     HealthChanged     "d"   push int      -> add esp, 14h = 20 = 4 + 4 + 4 + 4 + 4
    //     SlowMoMaxChanged  "f"   sub esp, 8    -> add esp, 18h = 24 = 4 + 4 + 4 + 8 + 4
    //
    // -- and both totals reconcile only with d = 4 and f = 8. A consumer reading a hooked "f" event four
    // bytes wide gets the low half of a double, which is not a small error but a meaningless one.
    //
    // nullopt for a malformed format.
    static std::optional<size_t> payload_stack_bytes(std::string_view payload);

    // THE WHOLE CALL FRAME a hook will see, in bytes: source, target, then for EACH argument a type tag
    // followed by its value, then the terminator.
    //
    // VERIFIED AGAINST FOUR OBSERVED CLEANUPS with three distinct format shapes:
    //     HealthChanged     "d"     add esp, 14h = 20
    //     SlowMoMaxChanged  "f"     add esp, 18h = 24
    //     AmmoCountChanged  "sdd"   add esp, 24h = 36
    //     SetChainPrompt    "ddf"   add esp, 28h = 40
    //
    // An earlier formula charged ONE tag for the whole payload rather than one per argument. It reproduced 20
    // and 24 exactly and was wrong by 8 and 12 on the other two -- an error invisible at one argument, which
    // is all that had been measured.
    //
    // This is the number that appears as the `add esp, N` after the call, so a consumer can check its hook
    // against the disassembly it is patching. nullopt for a malformed format.
    static constexpr size_t kTagBytes = 4;
    static constexpr size_t kTerminatorBytes = 4;
    static constexpr size_t kHeaderBytes = 8;  // source + target
    static std::optional<size_t> frame_bytes(std::string_view payload);

    // DOES THE BINARY STILL MATCH THIS ENTRY? Reads the dispatcher's first bytes, treats each 4-byte
    // immediate as a candidate pointer, and requires one to spell `name`. False when gameclient is absent,
    // the read faults, or no immediate resolves to the name.
    //
    // This is the check that keeps the catalogue honest, and it is exposed because a consumer about to hook
    // an address should be able to make it too.
    static bool verify(const Event& event);

    // Every entry verified, as a count -- so a partial mismatch is visible rather than collapsing to false.
    static size_t verified_count();

    //
    // THE UI PANELS -- the other half of this bridge, and the half a consumer composing its own invoke needs.
    //
    // The binary holds 450 dotted-identifier literals, and they fall into three families: "<Panel>.<Method>"
    // for methods the game invokes, "_global.g_*" for Flash globals it reads and writes (172 of those), and
    // the unrelated "ILT*.Default" names belonging to the C++ interface registry.
    //
    // EVERY PANEL'S METHOD STRINGS ARE REFERENCED FROM ONE FUNCTION, which is what makes the grouping a
    // measurement rather than a reading of the names: Player's 34 methods all come from one dispatcher, and
    // that dispatcher is the same function that carries the Game_Player_* binding names. So a consumer wanting
    // to observe a whole panel hooks one address instead of thirty-four.
    //
    // The counts below are the number of distinct method literals found per panel. Filenames and module names
    // ("GameDatabase.dll", "autoexec.cfg") match the same dotted shape and are excluded by their tails.
    struct UiPanel {
        const char* name;
        size_t method_count;
        uintptr_t dispatch_offset;  // the lazy initialiser, within gameclient.dll
        uintptr_t guard_offset;     // its once-flag; bit 0 set means the table below is filled
        uintptr_t table_offset;     // the binding table it returns
    };

    //
    // A PANEL'S BINDING TABLE -- the whole two-directional surface, with handler addresses.
    //
    // What I first called a "dispatcher" is a lazily-initialised accessor: it fills a static table on first
    // call and returns it. The table is 12-byte entries of {const char* name, void* handler, uint8 kind},
    // terminated by a NULL NAME, and it is the same descriptor shape as the engine's console command and
    // variable tables.
    //
    // ITS FILE IMAGE IS ZERO. The initialiser writes every field at runtime, so this must be read live and only
    // after the once-flag's bit 0 is set. Reading it early yields a table of nulls, which is why
    // panel_bindings checks the guard rather than trusting the address.
    //
    // THE KIND BYTE IS THE ROLE, and the map below is a census of ALL 623 entries across all 17 panels rather
    // than a rule read off the first few:
    //
    //     0, 1            17 each   <Panel>.OnConstruct / .OnDestruct
    //     2, 3, 5, 6      208       <Panel>.<Method>        -- Flash calls the game
    //     7               208       Game_<Panel>_<Event>    -- the game calls Flash
    //     11              1         ONE Game_* exception: Game_Player_ShowSubtitle
    //     12 .. 21        172       _global.g_*             -- a Flash variable
    //
    // AN EARLIER VERSION OF THIS MAP SAID "2, 3, 6" and "15, 16", from three panels' opening entries. It missed
    // kind 5 entirely and covered only 86 of the 172 global entries; run over the population it left 100 roles
    // Unknown. The counts above sum to 623 with none left over.
    //
    // THE GLOBAL KINDS ENCODE THE VARIABLE'S TYPE, not setter-versus-getter, and five of them match the
    // Hungarian census EXACTLY: k15 = 54 = g_n, k16 = 32 = g_b, k20 = 30 = g_an, k17 = 4 = one of g_ab/g_af and
    // k21 = 4 = the other. The remaining kinds (12, 13, 14, 18, 19) total 48 against g_s (12), g_as (26) and 10
    // unprefixed names, which is consistent but does NOT pin which kind is which -- so this class reports them
    // as GlobalSetter without claiming an element type.
    //
    // So ONE table carries both directions, which is what the "same panel from opposite directions" note two
    // passes ago was describing without a field to point at. For a consumer the split is the useful part: hook
    // a kind-7 handler to intercept the game telling the HUD, and a Flash-to-game handler to intercept the HUD
    // asking the game for something.
    //
    enum class BindingRole {
        Unknown,
        Lifecycle,    // kinds 0, 1
        FlashToGame,  // kinds 2, 3, 5, 6
        GameToFlash,  // kind 7, and kind 11 for the single Game_* exception
        GlobalSetter, // kinds 12..21 -- a _global variable, element type not pinned
    };

    struct PanelBinding {
        std::string name;
        uintptr_t handler{};
        uint8_t kind{};
        BindingRole role{BindingRole::Unknown};
    };

    static constexpr size_t kBindingEntryBytes = 12;

    // The role a kind byte denotes. Unknown for a kind not yet observed, which is reported rather than guessed
    // -- kinds up to 21 exist across the 17 panels and only these have been tied to a population.
    static BindingRole binding_role_for_kind(uint8_t kind);

    // Every binding of a panel, read live. Empty when the panel is unknown, gameclient is absent, or the
    // panel's once-flag is still clear -- the last being a real state, not an error.
    static std::vector<PanelBinding> panel_bindings(std::string_view panel);

    // Is the panel's table filled yet? Distinguishes "not initialised" from "no bindings", which the empty
    // vector above cannot.
    static bool panel_table_initialised(std::string_view panel);

    // ---- FLASH GLOBALS, RESOLVED TO THEIR SETTERS ------------------------------------------------
    //
    // The 172 _global.g_* bindings are the game's Flash variables, and each entry's handler IS the setter. That
    // matters more than it sounds: a consumer wanting to write one does NOT need to know the variable's GFx type
    // or which interface slot to call, because the handler already encodes both.
    //
    //     auto v = sdk::Events::find_global("_global.g_nMonolithMultiplayerHostID");
    //     if (v) {
    //         using Setter = int(__stdcall*)(void* gfx, int value);
        //         reinterpret_cast<Setter>(sdk::Modules::get().game_client()->base + v->handler)(gfx, 42);
    //     }
    //
    // DO NOT DERIVE THE GFx TYPE FROM THE KIND OR THE NAME. Measured over all 172: kind 13 carries a narrow
    // string (type 4) twice and a wide one (type 5) once, so the kind does not determine the type. The array
    // setters' leading constant does not even share the scalar enumeration -- a float array reads 2 while an int
    // array reads 0. Two passes on this project each derived a type rule from a pair of samples and each was
    // wrong; the handler is the only thing that knows.
    //
    // WHAT THE KIND DOES TELL YOU, exactly and over the whole population, is the C++ argument shape: it maps
    // one-to-one onto the Hungarian prefix, so it says whether to pass an int, a float, a bool, a string, or an
    // index plus element. That is what a caller needs in order to build the call at all.
    struct GlobalVariable {
        std::string name;
        uint8_t kind{};       // 12..21, straight from the binding table
        uint32_t gfx_slot{};  // 9 scalar / 11 array -- the slot the handler uses internally
        bool is_array{};
        uintptr_t handler{};  // gameclient-relative; THIS is what to call
    };

    // Every Flash global the UI registers, read live. Empty when gameclient is absent or no panel has
    // initialised yet.
    static std::vector<GlobalVariable> global_variables();

    // Resolves one by exact name, including the leading "_global.".
    static std::optional<GlobalVariable> find_global(std::string_view name);

    // The GFx interface slot a kind's handler uses: 9 (SetVariable) for scalar kinds 12..16, 11
    // (SetVariableArray) for array kinds 17..21, and 0 outside the observed range rather than a guess. Consumers
    // hooking the GFx interface use this to know which slot a given variable travels through.
    static uint32_t gfx_slot_for_kind(uint8_t kind);

    // The Hungarian prefix a kind denotes, or nullptr outside the observed range. This is a census over all 172
    // entries: it IS a function, every kind has exactly one prefix. It is NOT injective -- strings occupy kinds
    // 13 and 14, string arrays 18 and 19 -- and what separates each pair is unexplained and deliberately not
    // invented here (it is not the GFx type; see above).
    static const char* prefix_for_kind(uint8_t kind);

    static const std::vector<UiPanel>& ui_panels();
    static std::optional<UiPanel> find_panel(std::string_view name);

    // Runtime address of a panel's dispatcher, or 0. This is the one-address hook for a whole panel.
    static uintptr_t panel_dispatch(std::string_view name);

    // Does the dispatcher still reference one of its panel's method literals? Checked by PREFIX -- any string
    // beginning "<Panel>." will do -- because a panel has many methods and requiring a specific one would make
    // the check fail for a reason that does not matter.
    static bool verify_panel(const UiPanel& panel);
    static size_t verified_panel_count();

    // The ActionScript interface a category addresses, as the sender's own error string spells it:
    // "Monolith.I" + category + "Events". A consumer inspecting the Flash side needs this name, not the bare
    // category.
    static std::string as_interface_name(Category category);

    // The full AS method a dispatcher will invoke, given the implementation path the source string carries:
    // "<path>.<EventName>". Empty when the name is unknown; a null or empty path is what the sender itself
    // refuses, so it is refused here too.
    static std::string as_method_name(std::string_view path, std::string_view event_name);

    //
    // THE GFx OBJECT'S SLOTS, each identified by a distinct population of call sites rather than by position.
    //
    //     slot  9   SetVariable        scalar; 98 of the 172 _global accessors use it
    //     slot 11   SetVariableArray   arrays; the other 64 use it
    //     slot 14   Invoke             every event dispatcher goes through it
    //
    // WHICH SLOT A VARIABLE USES IS PREDICTED BY ITS HUNGARIAN PREFIX, and that is a measurement with no
    // exceptions across 162 classified names: g_n (54), g_b (32) and g_s (12) all take slot 9, while g_an (30),
    // g_as (26), g_ab (4) and g_af (4) -- every "array of" prefix -- all take slot 11.
    //
    // So a consumer holding a variable name knows both its type and which setter the game would use for it,
    // without having to find that variable's accessor.
    static constexpr size_t kSetVariableSlot = 9;
    static constexpr size_t kSetVariableArraySlot = 11;
    static constexpr size_t kInvokeSlot = 14;
    static constexpr const char* kDefaultMethod = "Default";

    //
    // THE GFx VALUE LAYOUT, confirmed by TWO INDEPENDENT PRODUCERS. The argument marshaller writes a type at
    // +0 and the value at +8 in 16-byte slots; the _global setters build the same shape on the stack, with the
    // same type codes (3 for a number, 2 for a bool). Neither derives from the other.
    static constexpr size_t kValueTypeOffset = 0;
    static constexpr size_t kValueDataOffset = 8;

    // The slot the game would use to set this variable: kSetVariableSlot for a scalar, kSetVariableArraySlot
    // for an array, or 0 when the name carries no recognised prefix. Accepts a bare name or a fully qualified
    // "_global.g_..." one.
    static size_t setter_slot_for_variable(std::string_view variable);

    // The payload letter matching a variable's Hungarian prefix -- 'd' for a number, 'b' for a bool, 's' for a
    // string, 'f' for a float -- or 0 for an unrecognised name. For an array prefix this is the letter of its
    // ELEMENTS, which is what the array setter marshals.
    static char type_letter_for_variable(std::string_view variable);

    // Is this an array variable? Distinguished from a scalar because they take different setter slots.
    static bool variable_is_array(std::string_view variable);

    // The fallback method name the sender uses when no event name is supplied.
};

}  // namespace sdk
