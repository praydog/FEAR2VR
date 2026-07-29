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

    // Is every character one this bus uses? An unknown letter means the format was misread, and a consumer
    // reading arguments off a stack from a bad format is worse off than one that refused.
    static bool payload_is_well_formed(std::string_view payload);

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

    // THE WHOLE CALL FRAME a hook will see, in bytes: source, target, a MARKER, the payload, and a second
    // MARKER. The markers are literal sentinels the senders push around the payload (0x12345678 / 0x1234567D,
    // with the leading one varying by payload kind), which is what makes the frame layout readable at all.
    //
    // This is the number that appears as the `add esp, N` after the call, so a consumer can check its hook
    // against the disassembly it is patching. nullopt for a malformed format.
    static constexpr size_t kMarkerBytes = 4;
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

    // The ActionScript interface a category addresses, as the sender's own error string spells it:
    // "Monolith.I" + category + "Events". A consumer inspecting the Flash side needs this name, not the bare
    // category.
    static std::string as_interface_name(Category category);

    // The full AS method a dispatcher will invoke, given the implementation path the source string carries:
    // "<path>.<EventName>". Empty when the name is unknown; a null or empty path is what the sender itself
    // refuses, so it is refused here too.
    static std::string as_method_name(std::string_view path, std::string_view event_name);

    // The vtable slot the sender invokes on the target GFx object, and the fallback method name it uses when
    // no event name is supplied. Both read off the sender.
    static constexpr size_t kInvokeSlot = 14;
    static constexpr const char* kDefaultMethod = "Default";
};

}  // namespace sdk
