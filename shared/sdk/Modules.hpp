#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include <windows.h>

// FEAR 2 module resolution. FEAR 2: Project Origin is a 32-bit LithTech
// Jupiter EX game whose code is spread across FIVE binaries:
//
//   FEAR2.exe           -- client engine (ILTClient implementation, renderer, D3D9)
//   Game\gameclient.dll -- client-side game logic (IClientShell implementation)
//   Game\gameserver.dll -- server-side game logic (loaded when hosting/playing)
//   gamedatabase.dll    -- attribute/database layer (LTGetIDatabaseMgr exports)
//   ltmemory.dll        -- engine memory manager
//
// (Verified against the installation layout and the live process, 2026-07. The
// renderer is statically linked inside FEAR2.exe; D3D9 is reached through a
// GetProcAddress'd Direct3DCreate9.)
namespace sdk {

// NOTE: no size helper here -- use kananlib's utility::get_module_size(HMODULE)
// (utility/Module.hpp). We deliberately do not wrap what kananlib already has.

class Modules {
public:
    struct Module {
        const char* name; // module basename, e.g. "gameclient.dll"
        HMODULE handle;
        uintptr_t base;
        size_t size;
        bool required; // required modules fail initialize() when missing
    };

    static Modules& get() {
        static Modules s_instance;
        return s_instance;
    }

    // Resolve every module handle + geometry. Returns false when a REQUIRED
    // module is missing (gameserver.dll is optional: only mapped in a session).
    bool initialize();
    bool is_initialized() const { return m_initialized; }

    // Null handle until initialize() resolved that module.
    const Module* exe() const { return &m_modules[0]; }           // FEAR2.exe
    const Module* game_client() const { return &m_modules[1]; }   // Game\gameclient.dll
    const Module* game_server() const { return &m_modules[2]; }   // Game\gameserver.dll (optional)
    const Module* game_database() const { return &m_modules[3]; } // gamedatabase.dll
    const Module* lt_memory() const { return &m_modules[4]; }     // ltmemory.dll

    // Which module owns an arbitrary address, as a basename ("d3d9.dll"), or nullopt when the OS does
    // not attribute it to one.
    //
    // DELIBERATELY NOT MATCHED AGAINST THE FIVE MODULES ABOVE: the answers that matter are modules this
    // class does not track. Both current consumers are like that -- Render asks who implements a COM
    // interface and gets d3d9.dll, Input asks who owns the window procedure and gets the Steam overlay.
    // Testing our own ranges would answer "not one of ours" and throw away the useful part.
    static std::optional<std::string> owning_module_name(uintptr_t address);

    // ---- SECTIONS, AND WHY "POINTS INTO THE MODULE" IS NOT "IS A VTABLE" --------------------
    //
    // A pass scanning the player object for sub-objects tested each candidate by reading its first dword and
    // asking whether that address lies inside gameclient. It reported 205 pointer slots over 86 objects.
    //
    // 137 OF THOSE SLOTS WERE NOT VTABLE POINTERS. Two addresses accounted for 84 of them, and their
    // "slot 0" read 0xC7F18B56 -- which is not an address, it is the x86 bytes `56 8B F1 C7`, a function
    // prologue. The dwords were FUNCTION POINTERS into .text, and a module-range test cannot tell those from
    // a vtable pointer because both point into the module. With the section test the true figures are 68
    // slots over 40 objects.
    //
    // THE DISCRIMINATOR IS THE SECTION. A vtable lives in initialised read-only data; a function lives in
    // code. In gameclient .text is 0x1000..0x1C1000 and every real vtable seen by this project sits at
    // 0x1C141C or above, so the two do not overlap at all -- the test is exact rather than heuristic.
    //
    // Read from the mapped PE headers rather than hardcoded, so it holds for every module and any build.
    enum class SectionKind {
        Code,  // IMAGE_SCN_CNT_CODE or MEM_EXECUTE -- functions live here
        Data,  // everything else: .rdata, .data, .idata ... vtables live here
    };

    struct SectionInfo {
        char name[9]{};  // PE section names are 8 bytes, not necessarily terminated
        uintptr_t start{};
        uintptr_t end{};
        SectionKind kind{SectionKind::Data};

        bool contains(uintptr_t address) const { return address >= start && address < end; }
    };

    // Which section of which loaded module contains this address? nullopt when the address is outside every
    // section of every module this class tracks. Walks the mapped headers, so it costs a few reads.
    static std::optional<SectionInfo> section_of(uintptr_t address);

    // COULD THIS DWORD BE A VTABLE POINTER? True only when it lands in a DATA section of a tracked module.
    // This is the predicate whose absence produced the 137 false positives above, and it is the one a
    // consumer identifying an opaque object should use before trusting the first dword.
    //
    // It is necessary, not sufficient: a data pointer that happens to point at a pointer array passes. Pair
    // it with Vtables::find_by_vtable or an expected address when the answer has to be exact.
    static bool looks_like_vtable_pointer(uintptr_t value);

    // The same question for an OBJECT rather than a raw dword: reads *object through a guard and applies the
    // test. nullopt when the read faults; false when the object holds something that is not a vtable.
    //
    // The player object is the case that makes this worth having: its first dword is a HEAP address, so it
    // has no vtable at all, and a consumer that assumed one would read a heap value as a table pointer.
    static std::optional<bool> object_has_vtable(uintptr_t object);

    // kananlib pattern scan over the live FEAR2.exe image; 0 on miss (+log
    // line so a broken signature is loud in fear2vr.log). Scanning a module is
    // a Modules job; each SDK class owns its own patterns and calls this.
    uintptr_t scan_exe(const char* pattern, const char* name) const;

    // THE SAME, AGAINST Game\gameclient.dll. Separate from scan_exe because the module is a different
    // resource with a different presence guarantee: the exe is always mapped in-process, so a miss there is
    // definitive, while a caller resolving a gameclient pattern before Modules::initialize() has run must be
    // able to try again. Callers latch only once this returns non-zero -- see AGENT.MD rule 5 on RETRYABLE vs
    // DEFINITIVE resolution.
    uintptr_t scan_game_client(const char* pattern, const char* name) const;

    // Scan gameserver.dll. Returns 0 when the module is not resolved yet, which
    // is the NORMAL state at a main menu -- retry once a session exists.
    uintptr_t scan_game_server(const char* pattern, const char* name) const;

private:
    Modules() = default;

    Module m_modules[5]{};
    bool m_initialized{false};
};

} // namespace sdk
