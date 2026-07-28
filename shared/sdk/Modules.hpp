#pragma once

#include <cstddef>
#include <cstdint>

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

    // kananlib pattern scan over the live FEAR2.exe image; 0 on miss (+log
    // line so a broken signature is loud in fear2vr.log). Scanning a module is
    // a Modules job; each SDK class owns its own patterns and calls this.
    uintptr_t scan_exe(const char* pattern, const char* name) const;

private:
    Modules() = default;

    Module m_modules[5]{};
    bool m_initialized{false};
};

} // namespace sdk
