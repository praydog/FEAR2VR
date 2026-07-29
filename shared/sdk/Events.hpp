#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

//
// THE GAME'S NAMED EVENT BUS -- how gameclient.dll tells its UI that state changed, by NAME and with a TYPED
// payload.
//
// Each event has a one-line dispatcher that calls a shared sender with four things: a category ("Player",
// "Menu"), the source, the event NAME as a string literal, and a FORMAT STRING describing the payload.
// Harvested from the binary there are 87 such dispatchers and about as many distinct events.
//
//     HealthChanged        "d"     one int
//     FlashlightMaxCharge  "f"     one float
//     AmmoCountChanged     "sdd"   a string and two ints
//     ShowCrosshair        "b"     one bool
//
// The format alphabet is d = int, f = float, s = string, b = bool. An event with no payload has none.
//
// THIS IS A SEPARATE MECHANISM FROM THE DELEGATE CHANNELS in sdk::Delegates, and the two should not be
// confused. A delegate channel is an intrusive list of C++ objects on a subject; this is a name-and-format
// bus aimed at the UI layer. The player object carries 21 of the former; the events below are the latter.
//
// WHY A CONSUMER WANTS IT. These names are the game's own vocabulary for the state a mod cares about --
// health, armour, ammo, weapon selection, slow-mo, player alive/dead -- and each dispatcher is a
// single-purpose function, so it is a precise hook point. The format string says how to read the arguments
// once hooked, which is the part that is otherwise guesswork.
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

    // Total bytes the payload occupies as pushed arguments, on the assumption every one is a 4-byte slot --
    // which is what a 32-bit cdecl call does with int, float, bool and a string POINTER alike. nullopt for a
    // malformed format.
    static std::optional<size_t> payload_stack_bytes(std::string_view payload);

    // DOES THE BINARY STILL MATCH THIS ENTRY? Reads the dispatcher's first bytes, treats each 4-byte
    // immediate as a candidate pointer, and requires one to spell `name`. False when gameclient is absent,
    // the read faults, or no immediate resolves to the name.
    //
    // This is the check that keeps the catalogue honest, and it is exposed because a consumer about to hook
    // an address should be able to make it too.
    static bool verify(const Event& event);

    // Every entry verified, as a count -- so a partial mismatch is visible rather than collapsing to false.
    static size_t verified_count();
};

}  // namespace sdk
