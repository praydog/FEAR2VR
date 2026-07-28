#pragma once

#include <cstddef>
#include <cstdint>

#include <windows.h>

// FEAR 2 module resolution. FEAR 2: Project Origin is a 32-bit LithTech
// Jupiter EX game whose code is spread across FIVE binaries:
//
//   FEAR2.exe        -- client engine (ILTClient implementation, renderer, D3D9)
//   Game\gameclient.dll -- client-side game logic (IClientShell implementation)
//   Game\gameserver.dll -- server-side game logic (loaded when hosting/playing)
//   gamedatabase.dll -- attribute/database layer (LTGetIDatabaseMgr exports)
//   ltmemory.dll     -- engine memory manager
//
// (Verified against the installation layout and live process, 2026-07. The
// renderer is statically linked inside FEAR2.exe; D3D9 is reached via a
// GetProcAddress'd Direct3DCreate9.)
namespace sdk {

struct Module {
    const char* name; // module basename, e.g. "gameclient.dll"
    HMODULE handle;
    uintptr_t base;
    size_t size;
    bool required; // required modules abort sdk::initialize() when missing
};

// Resolve every module handle + geometry. Returns false if a REQUIRED module
// is missing (gameserver.dll is optional: only present in an active session).
bool initialize();
bool is_initialized();

// Module accessors. Null handle until initialize() succeeds for that module.
Module* exe();          // FEAR2.exe
Module* game_client();  // Game\gameclient.dll
Module* game_server();  // Game\gameserver.dll (optional)
Module* game_database(); // gamedatabase.dll
Module* lt_memory();    // ltmemory.dll

// Size of any loaded module's image (PE header walk); 0 if unknown.
size_t module_size(HMODULE module);

} // namespace sdk
