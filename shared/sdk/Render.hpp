#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <windows.h>

#include <d3d9.h>

// The engine's Direct3D 9 side.
//
// WHERE RENDERING LIVES, because the file layout does not say: there is NO separate render
// DLL. FEAR2.exe loads d3d9.dll and d3dx9_40.dll directly (checked against the live module
// list), so the renderer is in the executable -- unlike the game logic, which is in
// Game\gameclient.dll. A mod hooking rendering scans the exe.
//
// EVERY STRUCT HERE IS TYPED BY THE ENGINE, not by us. The three records this class
// exposes are the exact addresses FEAR 2 hands to D3D9 entry points:
//
//   GetAdapterDisplayMode(0, adapter_info + 0x04)  -> D3DDISPLAYMODE
//   GetDeviceCaps(renderer + 0x0C)                 -> D3DCAPS9
//   CreateDevice(..., renderer + 0x158, ...)       -> D3DPRESENT_PARAMETERS
//
// So the whole of each struct is really there and can be returned whole, rather than a
// few leading fields we happened to check.
namespace sdk {

class Render {
public:
    // ---- THE FACTORY ----------------------------------------------------------------
    //
    // The engine's IDirect3D9. It is the first field of a small adapter record
    // (g_D3DAdapterInfo, exe+0x3304AC) initialised by D3DAdapterInfo_InitD3D9, which
    // calls Direct3DCreate9(32), stores the result, asks for the current display mode and
    // enumerates every adapter mode.
    //
    // nullptr before the engine has initialised D3D, or when the read faulted.
    static IDirect3D9* d3d9();

    // Address of the adapter record, for logging and for reaching past what is mapped.
    static uintptr_t adapter_info_address();

    // The DESKTOP mode, as GetAdapterDisplayMode reported it for adapter 0 at init -- not
    // the mode the game presents at. For that use present_params(), which live reports a
    // different Format on the same machine (21 vs 22). Live: 5120x1440 @ 240Hz, Format 22.
    static std::optional<D3DDISPLAYMODE> display_mode();

    // ---- THE DEVICE -----------------------------------------------------------------
    //
    // The engine's IDirect3DDevice9 -- the first field of the renderer object (g_Renderer,
    // exe+0x32E118), where Renderer_CreateDevice stores what CreateDevice hands back.
    //
    // The layout was confirmed by two independent structures reading correctly at their
    // offsets, not by the pointer looking plausible: a D3DCAPS9 at +0x0C whose leading
    // fields are DeviceType 1 (HAL) and AdapterOrdinal 0, and a D3DPRESENT_PARAMETERS at
    // +0x158 reporting the resolution the game is running.
    //
    // nullptr before the renderer is created, or when the read faulted.
    static IDirect3DDevice9* device();

    // Address of the renderer object.
    static uintptr_t renderer_address();

    // The caps the engine cached at creation. DeviceType is the one to check casually --
    // D3DDEVTYPE_HAL means real hardware, and the engine logs "Couldn't find any HAL
    // devices, Using reference rasterizer" when it falls back -- but the whole struct is
    // here, so MaxTextureWidth, VertexShaderVersion and the rest are available without
    // calling into the device.
    static std::optional<D3DCAPS9> device_caps();

    // WHAT THE GAME IS PRESENTING: the swap chain, not the desktop. This is the pair of
    // numbers a VR mod needs before it can size anything, plus Windowed, SwapEffect,
    // AutoDepthStencilFormat and PresentationInterval, which decide how a stereo path can
    // be attached at all.
    //
    // Live: 5120x1440, BackBufferFormat 21 (A8R8G8B8), BackBufferCount 1.
    static std::optional<D3DPRESENT_PARAMETERS> present_params();

    // ---- WHO IS IN FRONT OF D3D9 ----------------------------------------------------
    //
    // WHICH MODULE IMPLEMENTS AN INTERFACE'S METHODS -- the question a mod must answer
    // before patching a vtable.
    //
    // ASK ABOUT A METHOD, NOT THE VTABLE POINTER. An earlier version of this API reported
    // the module owning the vtable itself, which is unreliable: a real D3D9 device's
    // vtable is HEAP-allocated by d3d9.dll, belongs to no module image, and that test
    // returns nothing for it. Where the METHODS point is well defined in both cases.
    //
    // MEASURED LIVE, and the two interfaces differ, which is why this is exposed at all:
    //
    //   d3d9()    vtable inside gameoverlayrenderer.dll's image,
    //             ALL 17 methods in gameoverlayrenderer.dll   -> Steam's overlay PROXY
    //
    //   device()  vtable on the heap (d3d9.dll's own allocation),
    //             ALL 61 sampled methods in d3d9.dll          -> the REAL runtime
    //
    // The overlay wraps the factory to intercept CreateDevice, then hands back the genuine
    // device. So patching the DEVICE vtable patches the runtime, while reasoning about the
    // FACTORY means talking to the overlay.
    //
    // `method_slot` defaults to 2 (Release), which every COM interface has. nullopt when
    // the interface is null, a read faulted, or the method address is in no module.
    //
    // Takes IUnknown* rather than void* so a caller cannot pass something that is not a
    // COM interface at all. Every D3D9 interface is single-inheritance COM, so the
    // IUnknown* and the concrete pointer share one vtable slot 0 -- the conversion does
    // not move the pointer and slot indices stay meaningful.
    static std::optional<std::string> interface_impl_owner(IUnknown* iface,
                                                           size_t method_slot = 2);
};

}  // namespace sdk
