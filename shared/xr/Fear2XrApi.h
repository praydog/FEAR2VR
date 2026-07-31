#ifndef FEAR2XR_API_H
#define FEAR2XR_API_H

#include <stdint.h>

// ---- THE RESIDENT OPENXR PROXY -----------------------------------------------------------------
//
// A tiny DLL that loads the OpenXR runtime and NEVER LEAVES. The mod talks to it through this C ABI
// and stays freely unloadable.
//
// WHY THIS EXISTS. The mod's unload path is fail-closed: `prove_quiescent` suspends every other
// thread and refuses to unmap if any thread's instruction pointer is inside the mod's module, or if
// any thread cannot be inspected at all. That check is correct and worth keeping -- but an OpenXR
// runtime spawns its own threads on load, and once they exist the mod can never prove itself quiet.
// Observed as LNK1104: after loading a runtime, `injector --unload` stopped freeing fear2vr.dll and
// the next build could not overwrite it. Since the project's entire iteration loop is
// inject / test / unload / rebuild, that costs a game restart per edit.
//
// Moving the runtime behind a resident module fixes it at the cause: those threads now live in the
// proxy's address range, which the mod's quiescence check does not care about.
//
// AND IT BUYS SOMETHING BETTER. XR state OUTLIVES THE MOD. An XrInstance and XrSession cost real
// time to create and a session cannot be casually recreated, so parking their handles here means a
// mod reload does not disturb a running headset session. That turns "restart the game to test an
// XR change" into the same edit/reload loop everything else in this project enjoys.
//
// DESIGN RULE: the proxy owns what CANNOT be safely destroyed and recreated -- the runtime module,
// the handles, later the D3D11 device. Everything that can be is the mod's, where it can be
// iterated freely. The proxy is deliberately thin because its own code is frozen for the session:
// changing it requires a game restart, so it must be the part that never needs changing.
//
// THE ONE RULE A CONSUMER MUST FOLLOW: never hand the runtime a pointer into the mod that outlives
// a call. A debug-utils messenger, or any callback registered with OpenXR, would be invoked after
// the mod unloads and jump into freed memory. Callbacks belong to the proxy or to nobody.

#ifdef __cplusplus
extern "C" {
#endif

#define FEAR2XR_API_VERSION 2u

typedef struct Fear2XrApi {
    // Both checked by the client: `struct_size` catches a layout change, `version` an intentional
    // break. A mod built against a newer proxy than the resident one must refuse rather than call
    // through a shorter struct.
    uint32_t struct_size;
    uint32_t version;

    // ---- THE RUNTIME -----------------------------------------------------------------------
    //
    // LAZY, and that is a correctness property rather than an optimisation. An earlier version
    // loaded the runtime inside fear2xr_get_api, which meant merely ASKING WHETHER A PROXY EXISTS
    // started a vendor's VR service. Attaching must be free; `ensure_runtime` is the moment a
    // caller says it actually wants a headset. Returns non-zero once the runtime is up; safe and
    // cheap to call repeatedly.
    int (*ensure_runtime)(void);
    int (*runtime_loaded)(void);
    int (*runtime_crashed)(void);
    const char* (*runtime_library)(void);
    const char* (*last_error)(void);
    uint32_t (*api_major)(void);
    uint32_t (*api_minor)(void);
    uint32_t (*interface_version)(void);

    // The runtime's own entry point -- every OpenXR function is reached through it. __stdcall and a
    // 64-bit handle, because OpenXR handles are uint64_t even in a 32-bit process (XR_DEFINE_HANDLE
    // only makes them pointers when the pointer size IS 8).
    int32_t(__stdcall* get_instance_proc_addr)(uint64_t instance, const char* name, void** fn);

    // ---- STATE THAT OUTLIVES THE MOD -------------------------------------------------------
    // A deliberately dumb key/value store for handles. Dumb because the proxy must not know what an
    // XrSession is: keeping the semantics in the mod is what lets the mod be rewritten without a
    // game restart. `load_handle` returns 0 for an unknown key.
    void (*store_handle)(const char* key, uint64_t value);
    uint64_t (*load_handle)(const char* key);
    void (*clear_handles)(void);
} Fear2XrApi;

// The single export. Returns null if the caller's version is not supported.
typedef const Fear2XrApi*(__cdecl* PFN_fear2xr_get_api)(uint32_t version);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // FEAR2XR_API_H
