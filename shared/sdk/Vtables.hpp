#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// A CATALOGUE OF THE ENGINE'S CLASS VTABLES, WITH EXACT SLOT COUNTS.
//
// WHY THIS EXISTS. This project has now mistaken a scan's stopping point for a structure's end four times:
// the engine variable table twice (22, then 106, actually 107), ILTInput's vtable (12, actually 28), and
// the input device vtables (10, actually 11). Every one looked complete. A slot count is the same class of
// fact, and a consumer calling past the end of a vtable reads whatever the linker put next -- which in
// .rdata is usually another table full of perfectly valid-looking function pointers.
//
// HOW THE EXTENTS WERE FOUND, and why they are exact rather than plausible. MSVC places a class's name
// literal immediately after its vtable in this binary, so THE STRING IS THE BOUNDARY. A sweep of .rdata
// for "a run of in-image function pointers followed by an identifier-shaped string" yields 114 candidates.
//
// THAT SIGNATURE IS NECESSARY BUT NOT SUFFICIENT: a run of pointers followed by an unrelated string looks
// identical. StreamBuffer (155 "slots"), curveTo, showMenu, getTextExtent, DDS and gfxVersion are all
// coincidences of that kind -- Scaleform ActionScript identifiers and format strings that happen to sit
// after a pointer run. The candidates were therefore filtered by asking whether any slot IN the table
// references the trailing string, which is exactly what InterfaceImplementation does. 54 of 114 pass.
//
// THE FILTER HAS ITS OWN KNOWN FALSE POSITIVE, recorded rather than hidden: a GFx vtable can reach an
// ActionScript identifier from inside one of its methods, which is how `available`, `captureFocus` and
// `gfxVersion` survive. All three report a name_slot of 3, whereas the engine's own convention returns the
// name from slot 0, 1 or 2. follows_convention() exposes that distinction so a caller can weigh it; the
// SLOT COUNT is trustworthy either way, since it comes from the terminator rather than from the name.
//
// WHAT VERIFICATION BUYS. verify() catches an extent wrong in EITHER direction, which no single-sided check
// does:
//   * one slot too LONG -- the extra dword is the first four bytes of the name string, which is not an
//     address inside the exe image, so the in-image test fails.
//   * one slot too SHORT -- the trailing-string read lands on the last function pointer instead of text,
//     so the name comparison fails.
// The suite runs it over every entry, which is the only way a catalogue like this stays honest.
namespace sdk {

class Vtables {
public:
    struct Entry {
        const char* name;
        uintptr_t offset;     // exe-relative address of slot 0
        uint16_t slot_count;  // exact; see name_follows for which terminator bounds it
        uint16_t name_slot;   // which slot references the name; meaningless when !name_follows

        // TWO KINDS OF TERMINATOR, both exact and both verifiable:
        //   true  -- the class-name string sits immediately after the table, which is the boundary AND
        //            the identification. Found by sweep.
        //   false -- the class does NOT publish its name there. CLTPhysicsClient is the case that forced
        //            this: 18 slots followed by g_LTOrthoXBias, a float constant. Its bound is that the
        //            dword after the table is not an exe-range address, so the extent cannot be extended.
        //            Such an entry is added deliberately, not swept, and its NAME comes from elsewhere.
        bool name_follows;

        // Whether the name comes from slot 0, 1 or 2 -- the engine's InterfaceImplementation convention.
        // Only meaningful for swept entries.
        bool follows_convention() const { return name_follows && name_slot <= 2; }
    };

    // The whole catalogue. Sorted by name, so a caller may binary-search if it matters.
    static const Entry* all(size_t& count);

    // Exact-name lookup; nullptr when absent.
    static const Entry* find(std::string_view name);

    // Runtime address of a catalogued vtable, 0 when unknown or the exe is not mapped.
    static uintptr_t address(std::string_view name);

    // A SLOT'S FUNCTION POINTER, BOUNDS-CHECKED AGAINST THE RECORDED EXTENT. This is the point of the
    // catalogue for a consumer: asking for slot 92 of CLTRenderer (which has 0..91) returns nullopt
    // instead of the first dword of the string "CLTRenderer" reinterpreted as an address.
    static std::optional<uintptr_t> resolve(std::string_view name, size_t slot);

    // ---- A SUBSTITUTE FOR RTTI ---------------------------------------------------------
    //
    // THIS BINARY CARRIES NO RTTI. Every class identification in this project so far was done by hand:
    // read an object's vtable pointer, compare it against a table address recognised from somewhere else.
    // That is what these two do, generally and for 54 classes at once.
    //
    // It is how the input devices were identified -- keyboard and mouse were told apart by vtable, not by
    // slot order -- and a consumer handed an opaque engine pointer has the same question.

    // The entry whose vtable is exactly this address, or nullptr.
    static const Entry* find_by_vtable(uintptr_t vtable);

    // An object's vtable pointer, read through a guard. nullopt when the read faults or it is null. The
    // companion to class_name_of: this answers "which table", that answers "which class", and a consumer
    // holding an unidentified pointer usually wants both.
    static std::optional<uintptr_t> vtable_of(uintptr_t object);

    // The class name of an OBJECT: reads its vtable pointer through a guard and looks it up. nullopt when
    // the read faults or the vtable is not one of the 54 -- which is a legitimate answer, since the
    // catalogue covers only classes that publish a name, not every class in the image.
    static std::optional<std::string> class_name_of(uintptr_t object);

    struct Verification {
        size_t slots_checked{};
        bool slots_in_image{};  // every slot points inside the exe
        // For a name_follows entry: the string right after the table is the catalogued name. For one
        // without: the dword right after is NOT an exe-range address, which is what bounds it. Either way
        // an extent one slot too long or too short fails.
        bool name_matches{};

        bool ok() const { return slots_in_image && name_matches; }
    };

    // Verify one entry against live memory. nullopt when the exe is not mapped or a read faulted.
    static std::optional<Verification> verify(const Entry& entry);
};

}  // namespace sdk
