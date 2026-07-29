#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The engine's BUILT-IN SETTINGS TABLE: 22 entries of {name, storage, type}, each carrying a DIRECT
// address for the value. No console-variable API, no string parsing, no lookup by hash -- the engine
// keeps a static array of triplets and reads the storage directly wherever the setting is used.
//
// HOW IT WAS FOUND, because the route generalises: chasing what gates CClientMgr__Update's physics
// block led to a flag with NO WRITER anywhere in the exe -- two readers, and one lone data reference.
// A global that is read but never written, with exactly one pointer to it, is a settings slot. That
// pointer sat in the middle of this table, and the entry around it named it PausePhysics.
//
// WHY A CONSUMER WANTS IT: these are the knobs the engine itself consults every frame -- MaxFPS,
// PausePhysics, the physics update rates and step cap, sound limits, background resource loading.
// Reading one tells you what the engine will actually do; the address lets a mod change it
// deliberately, which is a far smaller intervention than hooking the code that reads it.
//
// THE TYPE TAGS WERE DECODED FROM LIVE VALUES, not assumed:
//
//     2 = int32     PhysicsClientUpdateRate 45, PhysicsMaxSteps 2, SoundMaxHardwareSounds 128
//     1 = float     MaxFinalizeTimeMS reads 1077936128 as an int and exactly 3.0 as a float
//     0 = UNKNOWN   one entry only (SoundTrack3DSoundCue), reading 0 either way, so undecidable
//
// The type-1 case is the decisive one: a value that is nonsense as an integer and exact as a float is
// a float. Type 0 is left explicitly unresolved rather than folded into one of the others.
//
// THE TAG IS TWO FIELDS. Low 16 bits are the type above; the high 16 carry flags, and 6 of the 106
// entries set bit 0. What that bit means is NOT established. It is surfaced on Entry rather than masked
// away, because an unexplained field a caller can see beats one a library silently discards.
//
// THERE IS A SECOND TABLE BESIDE THIS ONE: g_LTEngineCommandTable at exe+0x2E3280, 34 entries of
// {name, handler, 0} -- quit, ListCommands, RestartRender, RebindTextures, Exec, ForceCrash and so on.
// Console COMMANDS with function pointers rather than storage. Not exposed here: reading a variable is
// a load, whereas invoking a command is a call into the engine on whatever thread the caller happens to
// be, and that deserves its own deliberate design rather than a getter.
namespace sdk {

class EngineVars {
public:
    enum class Type : uint32_t {
        Unknown = 0,  // one entry, undecidable from its value; do NOT assume
        Float = 1,
        Int32 = 2,
    };

    struct Entry {
        std::string name;
        uintptr_t address{};  // the engine's own storage, directly readable and writable
        uint32_t type{};      // the tag's LOW 16 bits; compare against Type
        uint16_t flags{};     // its HIGH 16 bits. Values 0 and 1 occur (6 entries carry 1); what the
                              // bit MEANS is not established, so it is surfaced rather than dropped.

        bool is_int() const;
        bool is_float() const;
    };

    // Number of entries the table is known to hold. Asserted rather than trusted: the walk below stops
    // on the first entry that does not look like a triplet, so a schema change shortens the result
    // instead of running off the end.
    //
    // THIS WAS 22 AND THAT WAS WRONG TWICE OVER: the offset used started 24 entries into the table, and
    // the walk rejected any tag above 8 while 6 entries carry 0x00010002 -- flags in the high half. The
    // slice looked entirely plausible, which is why the count is asserted by the suite rather than
    // trusted here.
    static constexpr size_t kKnownEntryCount = 106;

    // Address of the table, or 0 when the exe is not mapped.
    static uintptr_t table_address();

    // Every entry, read out of the process. Stops early on a malformed entry, so a short result means
    // the table is not what this build expects -- which the caller can detect against
    // kKnownEntryCount.
    static std::vector<Entry> all();

    // One entry by exact name. nullopt when absent, which is a legitimate answer for a build whose
    // table differs.
    static std::optional<Entry> find(std::string_view name);

    // ---- READING A VALUE -------------------------------------------------------------
    //
    // Typed against the table's own tag, so asking for the wrong type fails rather than reinterpreting
    // the bytes. That matters here more than usual: an int read of a float setting yields values like
    // 1077936128, which is obviously wrong, while a float read of an int setting yields 6.3e-44, which
    // could easily be mistaken for a legitimate small number.

    static std::optional<int32_t> read_int(std::string_view name);
    static std::optional<float> read_float(std::string_view name);

    // The raw four bytes, for the Unknown-typed entry or for a caller doing its own interpretation.
    static std::optional<uint32_t> read_raw(std::string_view name);
};

}  // namespace sdk
