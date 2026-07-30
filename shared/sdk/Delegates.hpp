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

    // ---- WHICH SLOT ACTUALLY RUNS, FROM THE FUNCTION THAT DISPATCHES IT --------------------------
    //
    // The header above describes a delegate vtable as {handler, handler, Detach} without saying which handler
    // is invoked. Delegate_Notify (gameclient 0x1007BFB0) settles it, and it corroborates this whole layout
    // from the CONSUMING side rather than the teardown side:
    //
    //     _DWORD* p = subject[1];                    // the head's `next`
    //     while (p != subject) {                     // CIRCULAR -- terminates by returning to the head
    //         next = p[1];
    //         (*(*(p - 1) + 4))(p - 1, a2, a3);      // p - 1 is the NODE BASE; +4 is SLOT 1
    //         p = next;
    //     }
    //
    // Three things that were assumed are now read off dispatching code: a subject head really is a {prev,next}
    // pair, a node base really is link - 4 (the adjustment read_node_from_link exists for), and SLOT 1 is the
    // method that runs. Slot 0 is therefore not the notification entry point, whatever else it is.
    //
    // Every CPlayerStats setter calls it with (new_value, old_value), so a handler receives two dwords.
    static constexpr size_t kHandlerSlot = 1;

    // Runtime address of the shared notify function, or 0 when gameclient is not mapped. The counterpart to
    // detach_fn(), and useful to a consumer that wants to hook notification centrally rather than per class.
    static uintptr_t notify_fn();

    // The function that runs when this node's subject notifies -- slot 1 of its vtable, bounds-checked.
    // nullopt when the node is unreadable or its vtable is not a delegate's.
    //
    // This is the address a consumer hooks to observe one specific listener, which is the practical reason to
    // enumerate nodes at all.
    static std::optional<uintptr_t> handler_of(uintptr_t node);

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

    //
    // FINDING AN OBJECT'S EVENT CHANNELS.
    //
    // Objects here do not hold one delegate list -- they hold an ARRAY of them, one per event. The player
    // object opens with twenty-one heads at eight-byte stride across +0x00..+0xA0, carrying between one and
    // eleven listeners each and twenty-two distinct listener objects between them. Its very first dword is
    // therefore the first channel's `prev`, not a vtable, which is worth knowing before reading any object of
    // this shape as if it began with one.
    //
    // The predicate below is what makes a blind scan safe. A pair of dwords that happens to look like a link
    // is common; a pair whose chain leads to nodes whose vtables ALL carry the shared detach method in slot 2
    // is not. That is the same self-validation is_delegate_vtable provides, applied to a whole list.
    //

    // Does `head` head a list of genuine delegates? False for an unreadable pair, an empty list, or a chain
    // containing anything that is not a delegate. Deliberately false for EMPTY: a scan looking for an object's
    // channels wants the ones in use, and an empty pair is indistinguishable from two unrelated zero fields.
    static bool is_delegate_list(uintptr_t head);

    struct Channel {
        uintptr_t offset{};  // byte offset within the object
        uintptr_t head{};    // absolute address of the link pair
        size_t listeners{};  // how many delegates are attached
    };

    // Every delegate channel in `object`'s first `span` bytes, at four-byte granularity so a channel array at
    // an odd alignment is still found. Each hit is validated by is_delegate_list, so a false positive requires
    // a chain of fake nodes with the real detach method in their vtables.
    static std::vector<Channel> find_channels(uintptr_t object, size_t span = 0x100);

    // Which of `object`'s channels `owner` is attached to, as byte offsets. The direct answer to "what does
    // this listener react to on that object".
    static std::vector<uintptr_t> channels_listened_to(uintptr_t object, uintptr_t owner,
                                                       size_t span = 0x100);

    //
    // THE DUAL QUESTION: WHAT DOES THIS OBJECT SUBSCRIBE TO?
    //
    // Everything above looks at a SUBJECT -- who listens to it, which channels it publishes. The opposite
    // question is what an object LISTENS TO, and answering it needs the nodes the object owns rather than the
    // lists it heads. Those nodes are embedded in the object: a class that subscribes to twelve events carries
    // twelve nodes in a contiguous array.
    //
    // FOUND THE HARD WAY. A scan of the player for sub-object pointers reported the movement controller
    // fourteen times, at +236 and then at a 20-byte stride from +3740. Fourteen controllers is absurd, so the
    // stride was the finding -- those were owner back-pointers from an embedded node array, and the object
    // count was inflated by them.
    //
    // THAT SCAN THEN GOT THE PHASE WRONG, which is why this function exists rather than a new class. Matching
    // "a field equal to the object, and a data pointer eight bytes later" finds real nodes at the wrong base:
    // the match is `owner` at +0x0C, and the pointer eight bytes on is THE NEXT NODE'S VTABLE, not this one's.
    // Reading the layout out of Delegate_Detach, as the top of this file does, is what gets it right; deriving
    // it from a stride does not.
    //
    // So the rule here is the validated one: a node's own vtable must carry the shared detach method. A
    // coincidence has to fake that, and 329 vtables share it, so the check is cheap and decisive.
    //

    // Every delegate node OWNED BY `object` within its first `extent` bytes -- i.e. every event it subscribes
    // to. Four-byte granularity, since the array's phase differs per class. Only nodes whose own vtable passes
    // is_delegate_vtable are returned, so a result needs no further filtering.
    //
    // `extent` should be the class size where known; a constructor's highest touched offset is a lower bound.
    static std::vector<Listener> owned_nodes(uintptr_t object, size_t extent);

    // Is that array contiguous at the 20-byte stride? A real subscription array is; a scattered set of matches
    // means the scan found unrelated fields and a caller should distrust it. Exposed because it is the
    // difference between "found the array" and "found some matches".
    static bool nodes_are_contiguous(const std::vector<Listener>& nodes);

    // The subjects an object is currently attached to, deduplicated -- the objects publishing the events it
    // reacts to. A node with a null subject is detached and contributes nothing, which is exactly the
    // distinction kNodeSubject exists to make.
    static std::vector<uintptr_t> subscribed_subjects(uintptr_t object, size_t extent);
};

}  // namespace sdk
