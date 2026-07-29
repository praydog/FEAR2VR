#pragma once

#include <cstdint>
#include <optional>
#include <string>

// The engine's Direct3D 9 side, as much of it as is mapped.
//
// WHERE RENDERING LIVES, because the answer is not obvious from the file layout: there is
// NO separate render DLL. FEAR2.exe loads d3d9.dll and d3dx9_40.dll directly (checked
// against the live module list), so the renderer is in the executable -- unlike the game
// logic, which is in Game\gameclient.dll. A mod hooking rendering scans the exe.
namespace sdk {

class Render {
public:
    // The engine's IDirect3D9 FACTORY, returned as an opaque pointer because this SDK
    // does not include d3d9.h -- cast it yourself if you have the headers.
    //
    // It lives in a small engine-side adapter record (g_D3DAdapterInfo, exe+0x3304AC)
    // whose first field it is. That record is initialised by D3DAdapterInfo_InitD3D9,
    // which calls Direct3DCreate9(32), stores the result here, then asks it for the
    // current display mode and enumerates every adapter mode.
    //
    // nullptr before the engine has initialised D3D, or when the read faulted.
    static void* d3d9();

    // WHICH MODULE OWNS THE FACTORY'S VTABLE, which is not a curiosity -- it tells a mod
    // whether something is already sitting in front of D3D9.
    //
    // Live, with the Steam overlay active, this reports "gameoverlayrenderer.dll" and NOT
    // "d3d9.dll": the overlay hands the game a PROXY IDirect3D9. A mod that assumes the
    // interface it holds is Microsoft's own -- for instance by comparing a vtable against
    // d3d9.dll's address range, or by patching a vtable slot expecting the runtime's code
    // behind it -- is wrong on any machine with the overlay enabled, which is most of them.
    //
    // Returns the module basename, or nullopt when the factory is null, its vtable does
    // not fall inside any loaded module (which would mean it is not what we think it is),
    // or the read faulted.
    static std::optional<std::string> d3d9_vtable_owner();

    // The display mode the engine recorded at init, straight from the adapter record's
    // second field (a D3DDISPLAYMODE at +0x04).
    //
    // This is what GetAdapterDisplayMode reported for adapter 0 when D3D came up, i.e.
    // the DESKTOP mode, not necessarily the mode the game is presenting at. Live it reads
    // 5120x1440 @ 240Hz format 22 on an ultrawide desktop, which is what pinned the field.
    struct DisplayMode {
        uint32_t width;
        uint32_t height;
        uint32_t refresh_hz;
        // D3DFORMAT. The engine's own mode enumeration only ever accepts 21
        // (A8R8G8B8) or 22 (X8R8G8B8) for a display, and only resolutions of at least
        // 640x480 -- so those bounds come from its filter, not from our expectations.
        uint32_t format;
    };

    // nullopt when D3D is not up yet or the read faulted.
    static std::optional<DisplayMode> display_mode();

    // Address of the adapter record, for logging and for a caller that wants to poke
    // around past what is mapped. 0 when the exe is not resolved.
    static uintptr_t adapter_info_address();
};

}  // namespace sdk
