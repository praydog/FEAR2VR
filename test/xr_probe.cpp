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

#include <windows.h>

#include <cstdio>
#include <cstdlib>
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
    // UNBUFFERED, because this program talks to third-party runtimes that can and do take the
    // process down. Block-buffered stdout loses exactly the lines that say where it died -- which
    // it did, once, and the truncated output looked like a hang.
    ::setvbuf(stdout, nullptr, _IONBF, 0);

    // HOW the process dies is the question. xrCreateSession with a graphics binding took this
    // program down with exit code 5 and no catchable exception, which leaves two possibilities: the
    // runtime called exit(), or something killed us outright. atexit distinguishes them, and a
    // vectored handler catches a fault our __except would otherwise swallow silently.
    ::atexit([] { std::printf("[probe] *** exit() was called -- the runtime terminated us\n"); });
    ::AddVectoredExceptionHandler(1, [](PEXCEPTION_POINTERS ex) -> LONG {
        const auto code = static_cast<unsigned>(ex->ExceptionRecord->ExceptionCode);

        // DBG_PRINTEXCEPTION_C carries the runtime's own OutputDebugString text, which is usually a
        // far better explanation of a refusal than any XrResult.
        if (code == 0x40010006u && ex->ExceptionRecord->NumberParameters >= 2) {
            const auto* msg =
                reinterpret_cast<const char*>(ex->ExceptionRecord->ExceptionInformation[1]);
            if (msg != nullptr) {
                std::printf("[probe] runtime says: %.*s",
                            static_cast<int>(ex->ExceptionRecord->ExceptionInformation[0]), msg);
            }
            return EXCEPTION_CONTINUE_EXECUTION;
        }

        char mod[MAX_PATH]{};
        HMODULE h = nullptr;
        ::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                 GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             static_cast<LPCSTR>(ex->ExceptionRecord->ExceptionAddress), &h);
        if (h != nullptr) {
            ::GetModuleFileNameA(h, mod, sizeof(mod));
        }

        // The address is not in any module, so name the REGION instead: VirtualQuery says whether
        // it is private commit (a manual map or JIT), an image, or freed memory, which is the
        // difference between "someone allocated and ran code here" and "this pointer is garbage".
        MEMORY_BASIC_INFORMATION mbi{};
        if (::VirtualQuery(ex->ExceptionRecord->ExceptionAddress, &mbi, sizeof(mbi)) != 0) {
            std::printf("[probe] region: base %p size 0x%zX state 0x%X type 0x%X protect 0x%X\n",
                        mbi.BaseAddress, mbi.RegionSize, static_cast<unsigned>(mbi.State),
                        static_cast<unsigned>(mbi.Type), static_cast<unsigned>(mbi.Protect));
        }

        if (ex->ExceptionRecord->NumberParameters >= 2) {
            std::printf("[probe] access: %s at 0x%p\n",
                        ex->ExceptionRecord->ExceptionInformation[0] == 0   ? "read"
                        : ex->ExceptionRecord->ExceptionInformation[0] == 1 ? "write"
                                                                            : "execute",
                        reinterpret_cast<void*>(ex->ExceptionRecord->ExceptionInformation[1]));
        }

        std::printf("[probe] *** exception 0x%08X at %p on tid %lu  (%s+0x%X)\n", code,
                    ex->ExceptionRecord->ExceptionAddress, ::GetCurrentThreadId(),
                    mod[0] != 0 ? mod : "no module",
                    h != nullptr ? static_cast<unsigned>(
                                       reinterpret_cast<uintptr_t>(ex->ExceptionRecord->ExceptionAddress) -
                                       reinterpret_cast<uintptr_t>(h))
                                 : 0u);
        return EXCEPTION_CONTINUE_SEARCH;
    });

    std::printf("[probe] main thread is %lu\n", ::GetCurrentThreadId());
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
        // NOT an API-version check. Through openxr_loader.dll nothing reports one until an
        // instance exists, and asserting it here failed for a reason that had nothing to do with
        // the runtime being wrong -- the identity check below is the one that carries meaning.
        check(xr.interface_version() >= 1 || xr.using_proxy(),
              "and a usable entry point is established, by whichever route");
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

        if (made) {
            std::printf("[probe] runtime identifies as '%s' version %llu.%llu.%llu\n",
                        xr.runtime_name().c_str(),
                        static_cast<unsigned long long>(xr.runtime_version() >> 48),
                        static_cast<unsigned long long>((xr.runtime_version() >> 32) & 0xFFFF),
                        static_cast<unsigned long long>(xr.runtime_version() & 0xFFFFFFFF));
            check(!xr.runtime_name().empty(),
                  "and the runtime says who it is -- which is how a fatal-runtime quirk is matched, "
                  "since through the loader the library on disk is the same file for all of them");
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

            // ---- A SESSION, WHICH NEEDS THE HARDWARE ----------------------------------
            if (sr == 0) {
                uint64_t luid = 0;
                uint32_t level = 0;

                if (xr.graphics_requirements(luid, level)) {
                    std::printf("[probe] graphics requirements: adapter LUID 0x%llX, min feature "
                                "level 0x%X\n", static_cast<unsigned long long>(luid), level);
                } else {
                    std::printf("[probe] graphics requirements FAILED: %s\n",
                                xr.last_error().c_str());
                }

                check(luid != 0,
                      "the runtime names the adapter it wants -- a device on any other one is "
                      "rejected or presents to nothing");

                const bool have_device = xr.ensure_d3d11_device();
                std::printf("[probe] d3d11 device: %s\n",
                            have_device ? "created on the runtime's adapter"
                                        : xr.last_error().c_str());
                check(have_device, "and a D3D11 device is created on exactly that adapter");

                // CONTROL FIRST: a session with no graphics binding must be REFUSED, not fatal.
                // If this returns cleanly, the call, the handles and the struct layout are sound
                // and anything that goes wrong next belongs to the binding.
                const int32_t no_gfx = xr.probe_session_without_graphics();
                std::printf("[probe] xrCreateSession without graphics -> XrResult %d %s\n", no_gfx,
                            no_gfx == -2 ? "(FAULTED)" : "(refused cleanly, as it must be)");
                check(no_gfx != -2 && no_gfx != 0,
                      "a session with no graphics binding is refused rather than fatal, which is "
                      "what makes the next result attributable to the binding");

                // NOT forced. On this machine the 32-bit Oculus runtime dies inside its own
                // session setup, so the SDK refuses -- and the refusal is the correct behaviour to
                // assert here. tools/xr64 is the 64-bit control that proves the sequence itself is
                // right: same machine, same headset, XR_SUCCESS.
                const bool made_session = xr.create_session();

                if (!made_session && xr.session_unsupported_here()) {
                    std::printf("[probe] session refused (as designed): %s\n",
                                xr.last_error().c_str());
                    check(xr.session() == 0,
                          "a runtime known to fault in xrCreateSession is refused rather than "
                          "called, because the fault happens on ITS thread where no guard of ours "
                          "can catch it");
                }

                std::printf("[probe] xrCreateSession -> %s (XrResult %d) handle 0x%llX\n",
                            made_session ? "ok" : "FAILED", xr.last_xr_result(),
                            static_cast<unsigned long long>(xr.session()));

                if (!made_session) {
                    std::printf("[probe] %s\n", xr.last_error().c_str());
                }

                check(made_session || xr.session_unsupported_here(),
                      "a session is created, or refused for a measured reason -- never attempted "
                      "against a runtime that would take the process down");

                if (made_session) {
                    // The runtime walks IDLE -> READY on its own schedule and only says so through
                    // the event queue. Waiting for it is not politeness, it is the protocol:
                    // xrBeginSession before READY returns XR_ERROR_SESSION_NOT_READY.
                    auto state = sdk::OpenXR::SessionState::Unknown;

                    for (int i = 0; i < 200; ++i) {
                        xr.poll_events();
                        state = xr.session_state();

                        if (state == sdk::OpenXR::SessionState::Ready ||
                            state == sdk::OpenXR::SessionState::Exiting ||
                            state == sdk::OpenXR::SessionState::LossPending) {
                            break;
                        }

                        ::Sleep(25);
                    }

                    std::printf("[probe] session state after wait: %s\n",
                                sdk::OpenXR::state_name(state));
                    check(state != sdk::OpenXR::SessionState::Unknown,
                          "and the runtime reports a session state, so the event queue is being "
                          "drained correctly");

                    if (state == sdk::OpenXR::SessionState::Ready) {
                        const bool began = xr.begin_session();
                        std::printf("[probe] xrBeginSession -> %s (XrResult %d)\n",
                                    began ? "RUNNING" : "failed", xr.last_xr_result());
                        check(began, "and the session begins, so the headset is now ours to draw to");

                        for (int i = 0; i < 40 && xr.session_state() != sdk::OpenXR::SessionState::Focused;
                             ++i) {
                            xr.poll_events();
                            ::Sleep(25);
                        }

                        std::printf("[probe] session state while running: %s\n",
                                    sdk::OpenXR::state_name(xr.session_state()));
                        xr.end_session();
                    }

                    xr.destroy_session();
                }
            }

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
