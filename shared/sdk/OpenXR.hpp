#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sdk {

// ---- REACHING OPENXR FROM A 32-BIT PROCESS -----------------------------------------------------
//
// The last unknown between a verified stereo frame and a headset. Three facts settle how it has to
// be done here, and each was measured rather than assumed (PE machine type, registry, filesystem):
//
//   * The Meta XR Simulator -- which would allow bring-up with no hardware -- is **x64 only**
//     (SIMULATOR.dll, machine 0x8664). It cannot serve this process at any price.
//   * The only `openxr_loader.dll` on this machine is x64 as well. There is no 32-bit loader to
//     link against, and fetching one is a build-time dependency this project does not need.
//   * The registered 32-bit runtime EXISTS: HKLM\Software\Khronos\OpenXR\1\ActiveRuntime (the
//     WOW6432Node view a 32-bit process sees) names oculus_openxr_32.json, whose library is
//     LibOVRRTImpl32_1.dll, machine 0x014c.
//
// So this class does what the loader would do, which is a documented and small job: read the active
// runtime's manifest, load its library, and call `xrNegotiateLoaderRuntimeInterface` to exchange
// versions and receive the runtime's `xrGetInstanceProcAddr`. Everything else in OpenXR hangs off
// that one pointer.
//
// WHAT WORKS WITHOUT A HEADSET, and therefore what this class can promise unattended: discovery,
// loading, negotiation, and extension enumeration. Creating a SESSION needs a device; that is the
// consumer's problem and this class deliberately stops short of it rather than pretending.
//
// THREAD AFFINITY: discovery and loading touch the registry and the loader lock. Call them from a
// mod's initialise or a control thread, never from a frame hook.
class OpenXR {
public:
    static OpenXR& get();

    // The runtime's entry point, typed. Public because a consumer driving OpenXR needs to CALL it:
    // every other OpenXR function is reached through this one.
    //
    // THE HANDLE IS 64 BITS EVEN HERE. OpenXR's XR_DEFINE_HANDLE only makes handles pointers when
    // the pointer size IS 8; on 32-bit they are uint64_t, exactly like Vulkan's non-dispatchable
    // handles. Declaring XrInstance as void* costs four bytes of argument, which under __stdcall
    // means the callee pops 16 while the caller pushed 12 -- the stack unbalances by four and the
    // next return goes somewhere arbitrary. That is not a theory: it read `name` from the wrong
    // slot (XrResult -1/-2/-7 from three different runtimes) and then jumped into unmapped memory
    // and killed the game.
    using XrHandle = uint64_t;
    using PFN_GetInstanceProcAddr = int32_t(__stdcall*)(XrHandle, const char*, void**);

    // ---- DISCOVERY -----------------------------------------------------------------------------
    //
    // Reads the active runtime manifest and resolves its library path. Cheap, no DLL is loaded, and
    // it answers "is there anything to talk to on this machine" -- which a mod wants before it
    // advertises a VR mode.
    bool discover();

    // Every runtime registered for THIS bitness, active or not, as manifest paths. A machine can
    // have several -- this one has three -- and the active one is not necessarily the one that
    // works. A consumer offering a "which runtime" choice, or falling back after a failure, needs
    // the list rather than just the winner.
    static std::vector<std::string> available_runtimes();

    // Point at a specific manifest instead of the registered active one. Resolves its library the
    // same way discover() does, so load() follows immediately.
    bool select_manifest(const std::string& manifest);
    bool discovered() const { return m_discovered; }
    const std::string& manifest_path() const { return m_manifest; }
    const std::string& library_path() const { return m_library; }

    // ---- LOAD AND NEGOTIATE --------------------------------------------------------------------
    //
    // LoadLibrary + xrNegotiateLoaderRuntimeInterface. On success the runtime has agreed an
    // interface version with us and handed over its entry point.
    // A THIRD-PARTY RUNTIME CAN KILL THE HOST, and one here does: the Oculus 32-bit runtime faults
    // inside its own negotiation on a machine with no headset service (null `this`, write to +0xF).
    // A game must not die because a runtime is unhappy, so the load and the negotiation are both
    // guarded -- a faulting runtime is reported as a failed load, with `crashed()` set, and the
    // process keeps running. This is the difference between a mod that degrades to flatscreen and
    // one that takes the game down on machines its author never tested.
    // ---- THE ROUTE A MOD SHOULD ACTUALLY USE ---------------------------------------------------
    //
    // Load the runtime inside the RESIDENT PROXY (fear2xr.dll) instead of this module, and route
    // every call through it. Costs nothing at the call site -- `resolve`, `enumerate_extensions`
    // and everything built on them work identically -- and keeps this DLL unloadable, because the
    // runtime's threads end up in the proxy's module range rather than ours.
    //
    // A mod running inside the game MUST prefer this over load(). Direct load() is for out-of-
    // process tools like xr-probe, where nothing needs to unload.
    //
    // `proxy_path` with no directory separator is resolved next to THIS module, not against the
    // working directory the game happened to start in.
    // Attach WITHOUT starting anything. Free: it loads the proxy module and reads its API, but no
    // OpenXR runtime and no vendor service. This is how a mod asks "is the proxy here" and how the
    // persistent handle store is reached, neither of which should wake a headset.
    bool attach_proxy(const char* proxy_path = "fear2xr.dll");

    bool load_via_proxy(const char* proxy_path = "fear2xr.dll");
    bool using_proxy() const { return m_proxy != nullptr; }

    // Handles parked in the proxy, which outlive this DLL. An XrInstance or XrSession costs real
    // time to create and a session cannot be casually recreated, so a mod reload should find them
    // still alive rather than build them again. Returns 0 for an unknown key.
    bool persist_handle(const char* key, XrHandle value);
    XrHandle persisted_handle(const char* key) const;

    // LOADING A RUNTIME IS NOT PASSIVE. It starts that vendor's services -- PimaxXR brings up
    // PiPlatformService, Oculus wakes OVRServer -- and can trip a Windows firewall prompt that
    // blocks unrelated things until someone answers it. Load the one runtime you intend to use.
    bool load();

    // True when the last load attempt was aborted by a fault inside the runtime rather than by a
    // refusal. Worth distinguishing: a refusal is a version mismatch, a fault is a broken install.
    bool crashed() const { return m_crashed; }

    // A runtime that has faulted is latched off: further calls fail cleanly rather than poking a
    // library that has already proved it will jump into unmapped memory. unload() clears it.
    bool faulted() const { return m_faulted; }
    bool loaded() const { return m_get_proc != nullptr; }
    void unload();

    // The runtime's xrGetInstanceProcAddr, as a raw pointer so a consumer can drive OpenXR without
    // this class having to wrap every call. Null until load() succeeds.
    void* get_instance_proc_addr() const { return reinterpret_cast<void*>(m_get_proc); }

    // What the runtime negotiated. Interface version is the loader<->runtime ABI; the API version is
    // OpenXR's own (1.0.x here), split out because a consumer checking for 1.1 features needs it.
    uint32_t interface_version() const { return m_interface_version; }
    uint64_t api_version() const { return m_api_version; }
    uint16_t api_major() const { return static_cast<uint16_t>(m_api_version >> 48); }
    uint16_t api_minor() const { return static_cast<uint16_t>((m_api_version >> 32) & 0xFFFF); }

    // ---- WHAT THE RUNTIME OFFERS ---------------------------------------------------------------
    //
    // Extension enumeration takes XR_NULL_HANDLE, so it works with no instance and no device. This
    // is the deepest a headless machine can go, and it is a real capability query: a consumer looks
    // here for XR_KHR_D3D11_enable (the path a D3D9 game's shared surface would take).
    // Resolve any OpenXR entry point by name. THE primitive a consumer needs: every function in the
    // API is reached this way, and this class has no interest in wrapping two hundred of them.
    // Returns the runtime's XrResult (0 = XR_SUCCESS) so a caller can tell "unsupported" from
    // "wrong handle", and takes the instance handle because most functions require one.
    int32_t resolve(const char* name, void** out, XrHandle instance = 0);

    bool enumerate_extensions(std::vector<std::string>& out);

    // The same list, queried once and kept. A consumer checking three capabilities should not pay
    // three round trips into the runtime, and a status publisher should pay none at all.
    const std::vector<std::string>& extensions() const { return m_extensions; }
    bool supports_extension(const char* name) const;

    // Empty when the last operation succeeded.
    const std::string& last_error() const { return m_error; }

private:
    OpenXR() = default;

    bool m_discovered{false};
    std::string m_manifest;
    std::string m_library;
    std::string m_error;
    void* m_module{nullptr};
    PFN_GetInstanceProcAddr m_get_proc{nullptr};
    uint32_t m_interface_version{0};
    uint64_t m_api_version{0};
    bool m_crashed{false};
    bool m_faulted{false};
    std::vector<std::string> m_extensions;
    const void* m_proxy{nullptr};
};

}  // namespace sdk
