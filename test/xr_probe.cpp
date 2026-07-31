// ---- OPENXR BRING-UP, OUT OF PROCESS -----------------------------------------------------------
//
// Everything this project knows about reaching an OpenXR runtime, exercised in a throwaway 32-bit
// process instead of inside the game. That separation is not tidiness, it is a hard requirement:
//
//   LOADING A RUNTIME IS VERY NEARLY A ONE-WAY DOOR. Oculus, Virtual Desktop and PimaxXR each spawn
//   their own threads on load and none of them support FreeLibrary. Once one is resident, the mod
//   DLL cannot be unloaded cleanly -- observed directly, as a link failure: fear2vr.dll stayed
//   locked after `injector --unload` and the next build could not overwrite it. The project's whole
//   iteration loop is inject / test / unload / rebuild, so anything that pins the DLL costs a game
//   restart per edit.
//
//   LOADING A RUNTIME IS ALSO NOT PASSIVE. It starts that vendor's services -- PimaxXR brought up
//   PiPlatformService, Oculus woke OVRServer -- and can raise a Windows firewall prompt that blocks
//   unrelated launches until a human answers it. It touches the machine, so it is opt-in here and
//   restricted to the ACTIVE runtime.
//
// So: discovery runs by default and is completely side-effect free (a registry read and a small
// file parse). Actually loading the runtime requires --load, which the automated test never passes.
//
// Usage:
//   xr-probe            discovery only; this is what ctest runs
//   xr-probe --load     also load, negotiate, and enumerate the ACTIVE runtime

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "sdk/OpenXR.hpp"

namespace {

int g_checks = 0;
int g_failures = 0;

void check(bool ok, const char* what) {
    ++g_checks;

    if (!ok) {
        ++g_failures;
        std::printf("[FAIL] %s\n", what);
    }
}

}  // namespace

int main(int argc, char** argv) {
    bool want_load = false;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--load") == 0) {
            want_load = true;
        }
    }

    auto& xr = sdk::OpenXR::get();

    // ---- THE HANDLE SIZE ------------------------------------------------------------------------
    //
    // Asserted first because getting it wrong does not fail cleanly. OpenXR's XR_DEFINE_HANDLE only
    // makes handles pointers when the pointer size IS 8; on 32-bit they are uint64_t. Declaring one
    // as void* costs four bytes of argument, and under __stdcall the callee then pops sixteen where
    // the caller pushed twelve. The stack unbalances and the next return goes somewhere arbitrary --
    // which is exactly how this project first killed the game from OpenXR code.
    check(sizeof(sdk::OpenXR::XrHandle) == 8, "an OpenXR handle is 64 bits even in a 32-bit process");
    check(sizeof(void*) == 4, "this probe is 32-bit, which is the only bitness FEAR2 can use");

    // ---- DISCOVERY ------------------------------------------------------------------------------
    const auto available = sdk::OpenXR::available_runtimes();
    std::printf("[probe] %zu runtime(s) registered for 32-bit\n", available.size());

    for (const auto& r : available) {
        std::printf("[probe]   %s\n", r.c_str());
    }

    const bool discovered = xr.discover();
    std::printf("[probe] active runtime: %s\n",
                discovered ? xr.manifest_path().c_str() : "(none discovered)");

    if (discovered) {
        std::printf("[probe] library:        %s\n", xr.library_path().c_str());
    } else {
        std::printf("[probe] discovery error: %s\n", xr.last_error().c_str());
    }

    // A machine with no VR software at all is a legitimate configuration, and the SDK's contract
    // there is "say so cleanly", not "succeed". So the assertion is conditional on there being
    // something registered -- which keeps this test honest on a bare machine.
    if (!available.empty()) {
        check(discovered, "an active runtime is discovered when runtimes are registered");
        check(!xr.library_path().empty(), "and its manifest resolves to a library path");
        // Relative library paths must resolve against the MANIFEST, not the working directory --
        // the Oculus manifest says ".\LibOVRRTImpl32_1.dll" and resolving that against a game's
        // folder would silently look in the wrong place.
        check(xr.library_path().find(':') != std::string::npos,
              "resolved to an absolute path, so it does not depend on the working directory");
    }

    check(!xr.loaded(), "discovery alone loads nothing -- it must stay free of side effects");

    if (!want_load) {
        std::printf("[probe] %s (%d checks, discovery only -- pass --load to go further)\n",
                    g_failures == 0 ? "PASS" : "FAIL", g_checks);
        return g_failures == 0 ? 0 : 1;
    }

    // ---- LOAD AND NEGOTIATE ---------------------------------------------------------------------
    const bool loaded = xr.load();
    std::printf("[probe] load: %s  crashed=%d  error=%s\n", loaded ? "ok" : "failed",
                static_cast<int>(xr.crashed()), xr.last_error().c_str());

    if (loaded) {
        std::printf("[probe] negotiated interface %u, OpenXR API %u.%u\n", xr.interface_version(),
                    xr.api_major(), xr.api_minor());
        check(xr.interface_version() >= 1, "the runtime agreed a loader interface version");
        check(xr.api_major() == 1, "and reports an OpenXR 1.x API version");
        check(xr.get_instance_proc_addr() != nullptr, "and handed over an entry point");

        // The functions a runtime must serve with a NULL instance -- and only these two. Resolving
        // them is the real test of the calling convention: with the handle mis-sized they returned
        // -1, -2 and -7 from three different runtimes and then crashed the caller.
        static const char* const kGlobals[] = {"xrEnumerateInstanceExtensionProperties",
                                               "xrCreateInstance"};

        for (const char* name : kGlobals) {
            void* fn = nullptr;
            const int32_t r = xr.resolve(name, &fn);
            std::printf("[probe] resolve %-40s XrResult %d  %s\n", name, r,
                        fn != nullptr ? "ok" : "null");
            check(r == 0 && fn != nullptr, "a runtime resolves the globals it is required to serve");
        }

        // And the converse, which is just as much a conformance statement: asking for anything else
        // through a null instance MUST be refused. XR_ERROR_HANDLE_INVALID (-12) here is the
        // runtime behaving correctly -- an early version of this probe read that as a failure.
        {
            void* fn = nullptr;
            const int32_t r = xr.resolve("xrGetInstanceProcAddr", &fn);
            std::printf("[probe] resolve %-40s XrResult %d (refusal expected)\n",
                        "xrGetInstanceProcAddr", r);
            check(r != 0 && fn == nullptr,
                  "and refuses a non-global through a null instance, per the loader spec");
        }

        std::vector<std::string> exts;

        if (xr.enumerate_extensions(exts)) {
            std::printf("[probe] %zu extension(s)\n", exts.size());

            for (const auto& e : exts) {
                std::printf("[probe]   %s\n", e.c_str());
            }

            check(!exts.empty(), "a working runtime offers at least one extension");
            // The path a D3D9 game reaches a compositor by: render to a shared surface, hand it over
            // as D3D11. Without this extension the plan needs rethinking, so it is worth naming.
            std::printf("[probe] XR_KHR_D3D11_enable: %s\n",
                        xr.supports_extension("XR_KHR_D3D11_enable") ? "yes" : "NO");
        } else {
            std::printf("[probe] enumeration failed: %s\n", xr.last_error().c_str());
            check(false, "extensions enumerate once the runtime is loaded");
        }
        // ---- AN INSTANCE ------------------------------------------------------------------
        //
        // Creation does NOT need a headset, so this is reachable on any machine with a runtime, and
        // it is where a layout or calling-convention error shows up -- in a throwaway process
        // rather than in front of someone wearing hardware.
        std::vector<std::string> want;

        if (xr.supports_extension("XR_KHR_D3D11_enable")) {
            // The extension a D3D9 game's surface reaches a compositor through. Asking for it here
            // proves the runtime will actually ENABLE it, which "it appears in the list" does not.
            want.emplace_back("XR_KHR_D3D11_enable");
        }

        const bool made = xr.create_instance("FEAR2VR probe", want);
        std::printf("[probe] xrCreateInstance -> %s (XrResult %d) handle 0x%llX\n",
                    made ? "ok" : "FAILED", xr.last_xr_result(),
                    static_cast<unsigned long long>(xr.instance()));

        if (!made) {
            std::printf("[probe] %s\n", xr.last_error().c_str());
        }

        check(made && xr.instance() != 0,
              "an OpenXR instance is created, which is the first thing beyond enumeration and the "
              "point where a wrong struct layout would surface");

        if (made) {
            // ---- THE SYSTEM, WHICH DOES NEED HARDWARE --------------------------------------
            //
            // The FAILURE is the informative outcome here. XR_ERROR_FORM_FACTOR_UNAVAILABLE means
            // "runtime fine, nothing plugged in" -- completely different from a validation error,
            // and a consumer must tell them apart to decide whether to offer VR at all. So this
            // asserts that the call is ANSWERED definitively, not that a headset exists.
            sdk::OpenXR::XrHandle system = 0;
            const int32_t sr = xr.get_system(system);
            std::printf("[probe] xrGetSystem -> XrResult %d, system 0x%llX  (%s)\n", sr,
                        static_cast<unsigned long long>(system),
                        sr == 0 ? "A HEADSET IS PRESENT"
                                : "no headset -- expected on a bare machine");

            // WHICH refusal is it? "Supported but nothing plugged in" and "this runtime does not
            // do that at all" are different situations, and the numbers alone do not say which is
            // which. Asking for a form factor a PC runtime cannot possibly serve -- handheld --
            // separates them: if the two codes differ, the HMD answer means the headset is merely
            // absent, and a consumer can offer to wait for one.
            sdk::OpenXR::XrHandle handheld_system = 0;
            const int32_t hr2 = xr.get_system(handheld_system, 2);  // XR_FORM_FACTOR_HANDHELD_DISPLAY
            std::printf("[probe] xrGetSystem(handheld) -> XrResult %d\n", hr2);

            if (sr != 0) {
                check(hr2 != sr,
                      "the runtime distinguishes an ABSENT head-mounted display from a form factor "
                      "it does not support, so 'no headset' is a waitable condition rather than a "
                      "permanent refusal");
            }

            check(sr == 0 ? system != 0 : system == 0,
                  "xrGetSystem either yields a system id or none at all, never a success with "
                  "nothing behind it");
            check(sr != -1,
                  "and answers with a real runtime verdict rather than XR_ERROR_VALIDATION_FAILURE, "
                  "which is what a malformed XrSystemGetInfo would produce");

            check(xr.destroy_instance(), "and the instance is destroyed cleanly");
            check(xr.instance() == 0, "leaving no handle behind");
        }
    } else {
        // A runtime that faults is a broken install, not a broken caller -- but the process being
        // alive to report it is the property that matters, and it is asserted rather than assumed.
        check(true, "a failed load returns rather than taking the process down");
    }

    std::printf("[probe] %s (%d checks, %d failure(s))\n", g_failures == 0 ? "PASS" : "FAIL",
                g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
