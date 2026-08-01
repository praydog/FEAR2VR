// ---- THE 64-BIT CONTROL ------------------------------------------------------------------------
//
// The 32-bit Oculus runtime crashes inside its own RuntimeIPC init during xrCreateSession -- it gets
// as far as initialising its D3D11 compositor client and creating a texture swap chain, then jumps
// to a fixed address in reserved memory on one of its own worker threads. Identical through the
// real openxr_loader.dll and through direct negotiation, identical address every run.
//
// That is either "Meta's 32-bit path has rotted" or "this program is doing something wrong". The
// only way to tell them apart is to do the SAME SEQUENCE in a 64-bit process on the same machine,
// the same headset, the same runtime version. This is that control, and nothing else -- it is not
// part of the mod and it is not built by build.bat.
//
//   cmake -B build64 -A x64 -S tools/xr64 && cmake --build build64 --config Release
//   build64/Release/xr64.exe

#define XR_USE_GRAPHICS_API_D3D11
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>

#include <d3d11.h>
#include <dxgi1_2.h>

#include <cstdio>
#include <vector>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

namespace {

const char* result_name(XrInstance instance, XrResult r) {
    static char buf[XR_MAX_RESULT_STRING_SIZE];

    if (instance != XR_NULL_HANDLE && XR_SUCCEEDED(xrResultToString(instance, r, buf))) {
        return buf;
    }

    std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(r));
    return buf;
}

const char* state_name(XrSessionState s) {
    switch (s) {
    case XR_SESSION_STATE_IDLE: return "IDLE";
    case XR_SESSION_STATE_READY: return "READY";
    case XR_SESSION_STATE_SYNCHRONIZED: return "SYNCHRONIZED";
    case XR_SESSION_STATE_VISIBLE: return "VISIBLE";
    case XR_SESSION_STATE_FOCUSED: return "FOCUSED";
    case XR_SESSION_STATE_STOPPING: return "STOPPING";
    case XR_SESSION_STATE_LOSS_PENDING: return "LOSS_PENDING";
    case XR_SESSION_STATE_EXITING: return "EXITING";
    default: return "unknown";
    }
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);

    uint32_t ext_count = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &ext_count, nullptr);
    std::vector<XrExtensionProperties> exts(ext_count, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, ext_count, &ext_count, exts.data());
    std::printf("[xr64] %u extension(s)\n", ext_count);

    const char* enabled[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};

    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
    std::snprintf(ici.applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE, "FEAR2VR x64");
    std::snprintf(ici.applicationInfo.engineName, XR_MAX_ENGINE_NAME_SIZE, "control");
    ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 34);
    ici.enabledExtensionCount = 1;
    ici.enabledExtensionNames = enabled;

    XrInstance instance = XR_NULL_HANDLE;
    XrResult r = xrCreateInstance(&ici, &instance);
    std::printf("[xr64] xrCreateInstance -> %s\n", result_name(XR_NULL_HANDLE, r));

    if (XR_FAILED(r)) {
        return 1;
    }

    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    r = xrGetSystem(instance, &sgi, &system);
    std::printf("[xr64] xrGetSystem -> %s (system %llu)\n", result_name(instance, r),
                static_cast<unsigned long long>(system));

    if (XR_FAILED(r)) {
        return 1;
    }

    PFN_xrGetD3D11GraphicsRequirementsKHR get_reqs = nullptr;
    xrGetInstanceProcAddr(instance, "xrGetD3D11GraphicsRequirementsKHR",
                          reinterpret_cast<PFN_xrVoidFunction*>(&get_reqs));

    XrGraphicsRequirementsD3D11KHR reqs{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    r = get_reqs(instance, system, &reqs);
    std::printf("[xr64] graphics requirements -> %s, LUID 0x%llX, level 0x%X\n",
                result_name(instance, r),
                (static_cast<unsigned long long>(static_cast<uint32_t>(reqs.adapterLuid.HighPart))
                 << 32) |
                    static_cast<uint32_t>(reqs.adapterLuid.LowPart),
                static_cast<unsigned>(reqs.minFeatureLevel));

    IDXGIFactory1* factory = nullptr;
    CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory));
    IDXGIAdapter1* chosen = nullptr;

    for (UINT i = 0; factory != nullptr; ++i) {
        IDXGIAdapter1* a = nullptr;

        if (factory->EnumAdapters1(i, &a) != S_OK) {
            break;
        }

        DXGI_ADAPTER_DESC1 d{};
        a->GetDesc1(&d);

        if (d.AdapterLuid.LowPart == reqs.adapterLuid.LowPart &&
            d.AdapterLuid.HighPart == reqs.adapterLuid.HighPart) {
            chosen = a;
            break;
        }

        a->Release();
    }

    if (factory != nullptr) {
        factory->Release();
    }

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL got{};
    const D3D_FEATURE_LEVEL want = reqs.minFeatureLevel;
    const HRESULT hr = D3D11CreateDevice(chosen, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                         D3D11_CREATE_DEVICE_BGRA_SUPPORT, &want, 1,
                                         D3D11_SDK_VERSION, &device, &got, &ctx);
    std::printf("[xr64] D3D11CreateDevice -> 0x%08X, device %p\n", static_cast<unsigned>(hr),
                static_cast<void*>(device));

    if (chosen != nullptr) {
        chosen->Release();
    }

    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    binding.device = device;

    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &binding;
    sci.systemId = system;

    XrSession session = XR_NULL_HANDLE;
    r = xrCreateSession(instance, &sci, &session);
    std::printf("[xr64] xrCreateSession -> %s (session %p)\n", result_name(instance, r),
                reinterpret_cast<void*>(session));

    if (XR_FAILED(r)) {
        return 1;
    }

    // The runtime walks IDLE -> READY on its own schedule and says so only through the event queue.
    XrSessionState state = XR_SESSION_STATE_UNKNOWN;

    for (int i = 0; i < 200; ++i) {
        XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};

        while (xrPollEvent(instance, &ev) == XR_SUCCESS) {
            if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                state = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev)->state;
                std::printf("[xr64] session state -> %s\n", state_name(state));
            }

            ev = XrEventDataBuffer{XR_TYPE_EVENT_DATA_BUFFER};
        }

        if (state == XR_SESSION_STATE_READY) {
            break;
        }

        Sleep(25);
    }

    if (state == XR_SESSION_STATE_READY) {
        XrSessionBeginInfo sbi{XR_TYPE_SESSION_BEGIN_INFO};
        sbi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        r = xrBeginSession(session, &sbi);
        std::printf("[xr64] xrBeginSession -> %s\n", result_name(instance, r));

        for (int i = 0; i < 80 && state != XR_SESSION_STATE_FOCUSED; ++i) {
            XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};

            while (xrPollEvent(instance, &ev) == XR_SUCCESS) {
                if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                    state = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev)->state;
                    std::printf("[xr64] session state -> %s\n", state_name(state));
                }

                ev = XrEventDataBuffer{XR_TYPE_EVENT_DATA_BUFFER};
            }

            Sleep(25);
        }

        xrEndSession(session);
    }

    std::printf("[xr64] final state %s\n", state_name(state));
    xrDestroySession(session);
    xrDestroyInstance(instance);

    if (ctx != nullptr) {
        ctx->Release();
    }

    if (device != nullptr) {
        device->Release();
    }

    std::printf("[xr64] DONE -- a 64-bit session %s\n",
                state != XR_SESSION_STATE_UNKNOWN ? "WORKS on this machine" : "did not start");
    return 0;
}
