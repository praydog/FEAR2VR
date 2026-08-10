#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace sdk {

// ---- ACTIONS, NOT KEYS -------------------------------------------------------------------------
//
// The VR mod used to drive gameplay by pressing hardcoded virtual keys -- 'W', VK_SPACE, 'R', 'E',
// VK_SHIFT and so on. That works on a default profile and ONLY on a default profile: the moment a
// player rebinds jump, the controller's jump button presses a key that no longer jumps. It is not
// shippable, and `WeaponMgr::slot_virtual_key` already says as much in its own comment -- "a
// consumer that must be correct under rebinding has to read the profile rather than trust a
// mapping".
//
// So the mod now names the ACTION and this class answers "what input does that action, for THIS
// player, right now".
//
// THE COMMAND IDS ARE THE ENGINE'S OWN, and they are verified rather than copied. The game
// serialises its bindings as a flat table of {input object name, command id} pairs, so the numbering
// can be read straight out of a live profile instead of trusted from the FEAR source drop (which
// AGENTS.md rightly calls a false friend). Every value below was confirmed against a real profile:
//
//     Key W -> 0    Key S -> 1     Key A -> 3     Key D -> 4      Mouse Axis X -> 12
//     Key C -> 14   Key Space -> 15   Mouse Button1 -> 17   Key V -> 19   Key Shift -> 29
//     Key 1 -> 30   Key Z -> 70    Mouse Button2 -> 71   Key X -> 73
//     Key E -> 87   Key R -> 88    Key Control -> 106    Key F -> 114
//
// Where those overlap the FEAR header they agree exactly (FORWARD 0, DUCK 14, JUMP 15, FIRING 17,
// WEAPON_BASE 30), which is the corroboration worth having: two independent sources, one of them
// this build's own data.
enum class Action : uint32_t {
    Forward = 0,
    Backward = 1,
    StrafeLeft = 3,
    StrafeRight = 4,
    Pitch = 11,  // axis; no key form
    Yaw = 12,    // axis; no key form
    Menu = 13,
    Crouch = 14,
    Jump = 15,
    Fire = 17,
    Melee = 19,  // ALT_FIRING in the FEAR header; FEAR2 binds it to V, which is melee
    Sprint = 29,
    WeaponSlotBase = 30,  // slots 1..4 are 30..33, 5..8 are 40..43 -- NOT contiguous
    LeanLeft = 70,
    Aim = 71,
    LeanRight = 73,
    Objectives = 79,
    Grenade = 81,
    Use = 87,
    Reload = 88,
    Reflex = 106,  // slow-mo
    Flashlight = 114,
};

class Actions {
public:
    // The input bound to `action`, encoded the way SyntheticInput expects: a Windows virtual key
    // below 0x100, or 0x100 + button index for the mouse.
    //
    // nullopt means the action has no bound input this consumer can press -- an axis (Pitch/Yaw), a
    // mouse wheel, or genuinely unbound. Callers must treat that as "cannot do this", NOT as a
    // reason to fall back to a guessed key: a guess is the bug this class exists to remove.
    static std::optional<uint32_t> input_for(Action action);

    // Weapon slots are 1-based and their command ids are NOT contiguous (1..4 -> 30..33,
    // 5..8 -> 40..43), so the arithmetic lives here rather than at each call site.
    static std::optional<uint32_t> weapon_slot_input(unsigned slot);

    // A single binding as the game stores it.
    struct Binding {
        uint32_t command{};
        std::string object;  // the engine's own name, e.g. "Key Space", "Mouse Button1"
        uint32_t input{};    // decoded to the SyntheticInput encoding; 0 when undecodable
    };

    // Everything parsed, for diagnostics. Empty when no profile could be read.
    static std::vector<Binding> all();

    // Where the bindings came from, for /sdk diagnostics and for a bug report that needs to say
    // which file was actually consulted. Empty when none was found.
    static std::string source_path();

    // Re-read on the next query. The game rewrites the profile when the player changes a binding,
    // so a long session would otherwise keep pressing the old key.
    static void invalidate();

    // True when a real profile was parsed. False means every input_for() is nullopt, which a
    // consumer should surface rather than paper over.
    static bool loaded();
};

}  // namespace sdk
