#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "Mod.hpp"

// ---- UPGRADING THE ENGINE'S D3D9 DEVICE TO D3D9Ex ------------------------------------------------
//
// WHY. Getting a frame to the headset currently costs a GetRenderTargetData readback -- 38 MB every
// frame, which blocks until the GPU has drained all prior work. Measured with a live A/B: 14.3 fps at
// full size, 70.0 fps at a quarter of the bytes, 119.3 with the UI capture off as well. On real
// hardware the same build gives 50-60, so the copy is expensive everywhere and merely catastrophic
// under a software runtime, where the compositor's own GPU work lengthens every drain.
//
// A SHARED SURFACE removes the copy rather than shrinking it: D3D9Ex can create a render target with
// a shared handle, and the 64-bit host opens that same surface through ID3D11Device::
// OpenSharedResource. The frame never leaves the GPU, there is no PCIe transfer, and nothing waits on
// a drain. That is the only option that also helps on hardware, which is what decided it -- 72 fps at
// native is out of reach while every frame is copied through system memory.
//
// WHY IT HAS TO HAPPEN HERE. Sharing needs an Ex device, and the engine asks for a plain one --
// measured, not assumed: QueryInterface for IDirect3DDevice9Ex on the live device returns
// E_NOINTERFACE (0x80004002). A device cannot be upgraded after creation, so the only seam is the
// factory: return an IDirect3D9Ex where the engine asked for an IDirect3D9. Ex derives from it, so
// every call the engine makes still resolves, and devices created from an Ex factory are Ex devices.
//
// WHAT CHANGES UNDERNEATH THE ENGINE, and must be handled rather than hoped about:
//   - TestCooperativeLevel NEVER returns D3DERR_DEVICELOST on an Ex device. It returns S_OK,
//     S_PRESENT_OCCLUDED or S_PRESENT_MODE_CHANGED -- both SUCCESS codes -- and reports real loss as
//     D3DERR_DEVICEREMOVED/HUNG through Present instead. Any `!= D3D_OK` test therefore starts
//     firing on a window that is merely occluded. FrameCapture has exactly that test.
//   - D3DPOOL_MANAGED is REJECTED by Ex devices. If the engine allocates managed resources this
//     upgrade cannot stand, and that is the one failure that would force a revert.
//
// The gate at the entry point already holds the engine while hooks install, so this lands before the
// single Direct3DCreate9 call in the exe rather than racing it.
class D3D9ExUpgrade : public Mod {
public:
    static D3D9ExUpgrade& get();

    std::string_view get_name() const override { return "D3D9ExUpgrade"; }
    std::optional<std::string> on_initialize() override;

    struct State {
        bool hooked = false;      // the factory call is intercepted
        bool upgraded = false;    // an Ex factory was actually handed back
        uint32_t attempts = 0;    // times the engine asked for a factory
        uint32_t failures = 0;    // times Direct3DCreate9Ex refused and we fell back
        int32_t last_hr = 0;      // Direct3DCreate9Ex's last result
    };

    State state() const;
};
