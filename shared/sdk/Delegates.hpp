#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

//
// WHO IS LISTENING TO THIS OBJECT -- the game's observer pattern, which is how gameclient.dll wires almost
// everything together.
//
// THE SCALE IS THE REASON THIS IS ITS OWN CLASS: 329 vtable slots in gameclient hold the one shared detach
// method, and they land at slots 2, 5, 8, 11, 14 and so on -- every third. So a delegate vtable is exactly
// three slots, {handler, handler, Detach}, and the game contains 329 delegate implementations. Anything that
// wants to know what reacts to a player, a weapon or a camera is asking about these lists.
//
// A NODE IS TWENTY BYTES, and the layout is the detach method's own doing rather than a guess (see
// Delegate_Detach, gameclient 0x100EE9F0):
//
//     +0x00  vtable     three slots; slot 2 is always Delegate_Detach
//     +0x04  link.prev  self-pointing when detached
//     +0x08  link.next
//     +0x0C  owner      the object whose handler runs -- what a caller actually wants
//     +0x10  subject    the list this node is threaded into; non-null exactly when registered
//
// Detach guards on the subject, unlinks the node, RE-SELF-LINKS it, then zeroes the subject and the owner. That
// is what makes sdk::mem::LinkState::Empty mean "not registered" by construction.
//
// WALKING FROM A SUBJECT. A subject is a {prev, next} link pair; its `next` gives the first node's LINK
// address, and a node's base is link - 4. Measured on the player object: the list at player+0x70 holds four
// listeners with four distinct vtables, one of them the player camera.
//
// THE WALK VALIDATES ITSELF, which is unusual enough to be worth using. Because all 329 delegate vtables carry
// the same function in slot 2, a node found by pointer-chasing can be CHECKED to be a delegate rather than
// assumed -- see is_delegate_vtable. A walk that strays off a real list produces nodes that fail that test
// instead of plausible-looking owners, which is the failure mode this project has been bitten by repeatedly.
//

namespace sdk {

class Delegates {
public:
    static constexpr size_t kNodeSize = 20;
    static constexpr uintptr_t kNodeVtable = 0x00;
    static constexpr uintptr_t kNodeLink = 0x04;
    static constexpr uintptr_t kNodeOwner = 0x0C;
    static constexpr uintptr_t kNodeSubject = 0x10;

    // Slot 2 of every delegate vtable. The offset a validator checks.
    static constexpr size_t kDetachSlot = 2;

    struct Listener {
        uintptr_t node{};     // the node's base address
        uintptr_t vtable{};   // its three-slot vtable
        uintptr_t owner{};    // the object whose handler runs -- the answer to "who is listening"
        uintptr_t subject{};  // the list it is threaded into
        bool vtable_valid{};  // slot 2 is the shared detach method, so this really is a delegate
    };

    // Runtime address of the shared detach method, or 0 when gameclient.dll is not mapped. Exposed because it
    // is the anchor every other check here rests on, and a consumer verifying its own pointer-chase wants it.
    static uintptr_t detach_fn();

    // Does this vtable belong to a delegate? Reads slot 2 and compares against detach_fn(). This is the whole
    // validator, and it is cheap enough to apply to every node a walk produces.
    static bool is_delegate_vtable(uintptr_t vtable);

    // Read one node by its BASE address. nullopt when the read faults; a node whose vtable fails the validator
    // is still returned, with vtable_valid false, because "found something that is not a delegate" is a more
    // useful answer to a caller debugging a pointer than nothing at all.
    static std::optional<Listener> read_node(uintptr_t node);

    // Read one node from its LINK address, which is what a list walk holds. Equivalent to read_node(link - 4)
    // and named separately because getting that adjustment wrong is the easy mistake.
    static std::optional<Listener> read_node_from_link(uintptr_t link);

    // Everything attached to `subject_head`, a {prev, next} link pair. Bounded, and the bound is reported
    // rather than hidden: a result of exactly `limit` entries should be treated as a lower bound.
    //
    // Empty when the head is unreadable or the list is empty -- those are not distinguished, because a caller
    // can do nothing different about either. Use sdk::mem::classify_link when the difference matters.
    static constexpr size_t kMaxListeners = 64;
    static std::vector<Listener> listeners(uintptr_t subject_head, size_t limit = kMaxListeners);

    // Is `owner` attached to `subject_head`? The question a consumer usually has, without building a vector.
    static bool is_listening(uintptr_t subject_head, uintptr_t owner);
};

}  // namespace sdk
