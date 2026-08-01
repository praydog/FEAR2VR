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
    // Answers without the caller having to remember to enumerate first: an empty cache is filled
    // on demand. The version that required a prior enumerate_extensions() call silently returned
    // false for everything, and the in-game path duly created an instance with NO extensions
    // enabled while reporting success.
    bool supports_extension(const char* name);

    // ---- AN INSTANCE, WHICH IS WHERE OPENXR ACTUALLY BEGINS ------------------------------------
    //
    // Everything beyond enumeration needs one. Creation does NOT need a headset -- only
    // xrGetSystem does -- so this is the deepest a machine with nothing plugged in can go, and it
    // is worth going there because a layout or convention error shows up here rather than in front
    // of a user wearing hardware.
    //
    // THE HANDLE IS PARKED IN THE PROXY when one is attached, under "xr_instance". A reload finds
    // the existing instance and adopts it instead of building a second one: instances are
    // expensive, a runtime may permit only one, and re-creating it would defeat the reason the
    // proxy exists. Pass `force` to build a fresh one anyway.
    bool create_instance(const char* app_name, const std::vector<std::string>& extensions = {},
                         bool force = false);
    XrHandle instance() const { return m_instance; }
    bool have_instance() const { return m_instance != 0; }

    // Destroys and forgets it, including the proxy's copy. A consumer should rarely want this --
    // the point of parking the handle is that it survives -- but a changed extension list needs a
    // new instance.
    bool destroy_instance();

    // ---- THE SYSTEM, WHICH DOES NEED HARDWARE --------------------------------------------------
    //
    // Returns the runtime's XrResult verbatim rather than a bool, because the FAILURE is the
    // informative part on a machine with no headset: XR_ERROR_FORM_FACTOR_UNAVAILABLE means
    // "runtime fine, nothing plugged in", which is a completely different situation from a
    // validation error and a consumer must be able to tell them apart to decide whether to offer
    // VR at all.
    int32_t get_system(XrHandle& out_system, uint32_t form_factor = kHeadMountedDisplay);
    static constexpr uint32_t kHeadMountedDisplay = 1;  // XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY

    // ---- A SESSION -----------------------------------------------------------------------------
    //
    // The first thing that genuinely requires hardware. It also requires a graphics binding, and
    // the runtime -- not the caller -- chooses the adapter: xrGetD3D11GraphicsRequirementsKHR hands
    // back a LUID, and a device created on any other adapter will be rejected or, worse, silently
    // present to nothing. On a laptop or a machine with an iGPU that is not a formality.
    //
    // The D3D11 device is created on that adapter and PARKED IN THE PROXY, because the session
    // holds a reference to it and both must outlive the mod. Its code lives in d3d11.dll, so it
    // stays valid across a reload; this DLL must never release it.
    bool graphics_requirements(uint64_t& adapter_luid, uint32_t& min_feature_level);
    bool ensure_d3d11_device();
    void* d3d11_device() const { return m_d3d11; }

    // REFUSES ON THE 32-BIT OCULUS RUNTIME, and that refusal is protecting the game.
    //
    // Measured on this machine: xrCreateSession gets as far as initialising Oculus's D3D11
    // compositor client and creating a texture swap chain, then jumps to a FIXED address in
    // reserved memory on one of its own worker threads -- where no __try of ours can catch it, so
    // the process simply dies. Identical through the real openxr_loader.dll and through direct
    // negotiation, identical address every run.
    //
    // The same sequence in a 64-bit process on the same machine, same headset, same runtime version
    // returns XR_SUCCESS (see tools/xr64). So this is Meta's 32-bit path, which they deprecated
    // years ago, and no argument this side of the call changes it.
    //
    // Leaving it callable would mean one stray request takes FEAR2 down with no crash log worth
    // reading. `force` exists so the finding can be re-tested deliberately when a runtime updates.
    bool create_session(bool force = false);

    // True when a session cannot be created here for a reason that is not going to change: the
    // process is 32-bit and the active runtime is the one known to fault. A consumer should read
    // this and route through a 64-bit host rather than trying and dying.
    bool session_unsupported_here();

    // WHO the runtime actually is, asked of the instance rather than inferred from a file path --
    // which stopped working the moment calls started going through openxr_loader.dll, where the
    // library on disk is the loader and says nothing about the runtime behind it.
    bool instance_properties(std::string& runtime_name, uint64_t& runtime_version);
    const std::string& runtime_name() const { return m_runtime_name; }

    // The RUNTIME's own version (Oculus 86.x here), which is not the OpenXR API version and must
    // not be confused with it -- an earlier version of this file stored one in the other's field.
    uint64_t runtime_version() const { return m_runtime_version; }

    // Attempt a session with NO graphics binding. Guaranteed to fail -- the spec requires one --
    // but HOW it fails is diagnostic: a clean XR_ERROR_GRAPHICS_DEVICE_INVALID proves the call
    // itself, the handles and the struct layout are all sound, and isolates a fault to the binding.
    int32_t probe_session_without_graphics();
    XrHandle session() const { return m_session; }
    bool have_session() const { return m_session != 0; }
    bool destroy_session();

    // ---- SESSION LIFECYCLE -----------------------------------------------------------------
    //
    // A session does not become usable because it was created: the runtime walks it through IDLE ->
    // READY -> SYNCHRONIZED -> VISIBLE -> FOCUSED, and it only says so through the event queue. A
    // consumer that skips this and starts submitting frames gets errors that look like bugs in its
    // rendering.
    enum class SessionState : uint32_t {
        Unknown = 0,
        Idle = 1,
        Ready = 2,
        Synchronized = 3,
        Visible = 4,
        Focused = 5,
        Stopping = 6,
        LossPending = 7,
        Exiting = 8,
    };

    // Drains the event queue and updates the cached state. Cheap; call it every frame.
    void poll_events();
    SessionState session_state() const { return m_session_state; }
    static const char* state_name(SessionState s);

    // xrBeginSession, which is legal only from READY. Returns false (without calling) otherwise, so
    // a caller can simply attempt it each frame until the runtime is ready.
    bool begin_session();
    bool session_running() const { return m_session_running; }
    bool end_session();

    // The raw XrResult of the last instance or system call, for a caller that wants the code.
    int32_t last_xr_result() const { return m_last_result; }

    // Empty when the last operation succeeded.
    const std::string& last_error() const { return m_error; }

private:
    OpenXR() = default;

    bool m_discovered{false};
    std::string m_manifest;
    std::string m_library;
    std::string m_error;
    PFN_GetInstanceProcAddr m_get_proc{nullptr};
    uint32_t m_interface_version{0};
    uint64_t m_api_version{0};
    bool m_crashed{false};
    bool m_faulted{false};
    std::vector<std::string> m_extensions;
    const void* m_proxy{nullptr};
    XrHandle m_instance{0};
    XrHandle m_system{0};
    std::string m_runtime_name;
    uint64_t m_runtime_version{0};
    XrHandle m_session{0};
    void* m_d3d11{nullptr};
    SessionState m_session_state{SessionState::Unknown};
    bool m_session_running{false};
    int32_t m_last_result{0};
};

}  // namespace sdk
