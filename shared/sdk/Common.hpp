#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

// ILTCommon (CLTCommonClient) -- 19 slots, and the two that matter most for VR are the attachment
// transforms: a weapon or a hand has to be placed on a model NODE or SOCKET, and this interface is how the
// engine itself answers that question. Reading its answer is far cheaper than recomputing the skeleton.
//
// HOW THE MAP WAS ESTABLISHED, and why NONE of it came from the reference's ordering:
//
// FEAR 2 REORDERED THIS INTERFACE. Slot 2 is the reference's 16th method, slot 10 its 1st, slot 14 its
// 12th. That is the opposite of ILTPhysics, where the order was preserved with deletions and the drift was
// monotonic. So the reference supplied a vocabulary here, never a position -- every name below rests on a
// string the function references or on what it demonstrably does.
//
// THE SERVER TWIN IS THE STRUCTURAL CHECK. CLTCommonServer is also 19 slots and aligns slot for slot, with
// ELEVEN of them sharing the identical function address (the CLTCommonShared implementations, plus the
// shared abstract destructor at slot 0 -- which a first count of this by hand missed, and the measurement
// caught). Eight are overridden per side: 1-6, 13 and 18. One table's layout could be a coincidence; two
// independently resolved tables agreeing slot by slot is not.
//
// THE NODE/SOCKET PAIR was the one genuinely ambiguous case, and it resolved without guessing. Slots 7 and
// 9 forward to a single implementation that differs only by a bool, and the flag selects which ILTModel
// method gets called: GetNodeTransform when set, GetSocketTransform when clear. They are distinguished by
// what they CALL rather than by where they sit -- which is exactly the evidence position could not give.
//
// TWO SLOTS HAVE NO REFERENCE COUNTERPART AT ALL: 15 (GetPolyOcclusionIndex, named from its own string) and
// 16, which calls ISteamApps' second method -- BIsLowViolence in that interface's published order -- so it
// is the low-violence build check. That last name rests on the Steam SDK's interface layout rather than on
// anything in this binary, and is flagged accordingly.
//
// SLOT 13 IS DELIBERATELY NOT NAMED "CreateMessage". It allocates from a pool, initialises four dwords and
// a vtable, and hands the object back through an out-parameter, which matches the reference's CreateMessage
// in shape. But the allocated class publishes no name, references no strings, and its vtable is referenced
// only by its own constructor -- so there is nothing to confirm it with. Since this interface is reordered,
// reference correspondence alone is not evidence. It is CreatePooledObject until something proves more.
namespace sdk {

class Common {
public:
    enum class Slot : size_t {
        Destructor = 0,
        InterfaceImplementation = 1,
        SetObjectFilenames = 2,
        GetObjectFlags = 3,
        SetObjectFlags = 4,
        GetAttachmentObjects = 5,
        GetAttachments = 6,
        GetAttachedModelNodeTransform = 7,
        GetAttachmentTransform = 8,
        GetAttachedModelSocketTransform = 9,
        GetObjectType = 10,
        Parse = 11,
        ParseAlt = 12,
        CreatePooledObject = 13,
        GetPolyTextureFlags = 14,
        GetPolyOcclusionIndex = 15,  // no reference counterpart
        IsLowViolence = 16,          // no reference counterpart; ISteamApps-backed
        StubReturnTrue = 17,         // ICF-folded stub shared with unrelated classes
        GetILTModel = 18,
    };

    static constexpr size_t kSlotCount = 19;

    // Resolved through the interface registry, which re-reads the holder slot every call: an interface
    // pointer is null before the database fills it and can go null again on module unload.
    static uintptr_t instance();
    static uintptr_t vtable();

    // The class name asked of the binary through the name getter, not looked up in a table. A consumer
    // handed an interface pointer should confirm this before calling through it.
    static std::optional<std::string> class_name();

    // One slot's address, bounds-checked into the exe. Handed over for the calls this header does not
    // wrap -- notably the object-flag mutators, which change engine state.
    static std::optional<uintptr_t> slot_address(Slot slot);

    // ---- QUERIES ----------------------------------------------------------------------
    //
    // Each checks the engine's LTRESULT rather than discarding it, so a caller cannot mistake "the engine
    // refused" for a zero answer. All refuse a null handle instead of passing it through.

    // Object type as the engine reports it -- the OT_* value. Pairs with the object-type mapping already
    // in sdk::Object, so the two can be cross-checked against each other.
    static std::optional<uint32_t> object_type(uintptr_t object);



    // ---- THE ATTACHMENT TRANSFORMS: ADDRESSES, NOT WRAPPERS ---------------------------
    //
    // Slots 6, 7, 8 and 9 are the VR-relevant group -- enumerate an object's attachments, then ask for a
    // NODE or SOCKET transform on the attached model, which is how the engine itself places a weapon or a
    // hand.
    //
    // THEY ARE NOT WRAPPED HERE, and the reason is worth stating rather than hiding. Their argument shapes
    // are only PARTLY established: the shared implementation takes (attachment, name, out) by thiscall,
    // and it forwards a transform through a chain of its own vtable calls whose out-parameter POSITION I
    // have not pinned. Calling an engine function with a wrong argument shape is not a wrong answer, it is
    // a corrupted stack in the game's own thread -- so this SDK hands over the verified slot addresses and
    // says exactly what is known, instead of a signature that looks authoritative.
    //
    // WHAT IS ESTABLISHED: slot 7 resolves the transform through ILTModel::GetNodeTransform, slot 9
    // through ILTModel::GetSocketTransform, and both first call this interface's own slot 5
    // (GetAttachmentObjects) and slot 8 (GetAttachmentTransform), then combine the two. So a consumer
    // driving them should expect the attachment to be valid and the model to be loaded.

    // Whether this build is running in low-violence mode, via ISteamApps. Included because a mod that
    // changes what the player sees should know when the engine has already censored it.
    static std::optional<bool> is_low_violence();

    static constexpr int32_t kLtOk = 0;
};

}  // namespace sdk
