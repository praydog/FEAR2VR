#include "OpenXR.hpp"

#include "xr/Fear2XrApi.h"
#include "xr/RuntimeLoader.hpp"

#include <windows.h>

#include <d3d11.h>
#include <dxgi1_2.h>

// THE REAL HEADERS, rather than hand-declared structures. Every value this file previously guessed
// at was eventually measured against a live runtime -- XR_TYPE_EXTENSION_PROPERTIES is 2 not 3,
// XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR is ...002 not ...001 -- and each wrong guess cost a
// debugging session that looked like a broken runtime. XR_TYPE_INSTANCE_PROPERTIES turns out to be
// 32, which nothing about the surrounding values would have suggested.
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <cstddef>
#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>

namespace sdk {

namespace {

constexpr uint64_t make_version(uint64_t major, uint64_t minor, uint64_t patch) {
    return (major << 48) | (minor << 32) | patch;
}

std::string directory_of(const std::string& path) {
    const size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? std::string{} : path.substr(0, slash);
}

// ---- CALLING INTO A RUNTIME AT ALL -------------------------------------------------------------
//
// Every entry through a runtime's own function pointer is foreign code and gets a guard. This is
// not defensive decoration: the Oculus 32-bit runtime faults inside xrCreateSession, and an earlier
// version of this file guarded the load but called xrGetInstanceProcAddr bare -- which jumped into
// unmapped memory and took the game down.
//
// (A guard only helps when the fault lands on OUR thread. The session crash happens on a runtime
// worker, which is why create_session refuses outright instead of trying.)
template <typename Fn, typename A>
int guarded_call1(Fn fn, A a, int32_t* result) {
    __try {
        *result = fn(a);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
}

template <typename Fn, typename A, typename B>
int guarded_call2(Fn fn, A a, B b, int32_t* result) {
    __try {
        *result = fn(a, b);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
}

template <typename Fn, typename A, typename B, typename C>
int guarded_call3(Fn fn, A a, B b, C c, int32_t* result) {
    __try {
        *result = fn(a, b, c);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
}

template <typename Fn, typename A, typename B, typename C, typename D>
int guarded_call4(Fn fn, A a, B b, C c, D d, int32_t* result) {
    __try {
        *result = fn(a, b, c, d);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
}

static_assert(sizeof(XrInstance) == 8, "OpenXR handles are 64-bit even in a 32-bit process");
static_assert(sizeof(OpenXR::XrHandle) == sizeof(XrInstance), "our handle alias must match");

}  // namespace

namespace {

// The direct (non-proxy) route, used out of process by xr-probe. In the game the runtime lives in
// the resident proxy instead and this stays untouched.
xr::RuntimeLoader& direct() {
    static xr::RuntimeLoader loader;
    return loader;
}

}  // namespace

std::vector<std::string> OpenXR::available_runtimes() {
    return xr::RuntimeLoader::available_runtimes();
}

bool OpenXR::select_manifest(const std::string& manifest) {
    const bool ok = direct().select_manifest(manifest);
    m_manifest = direct().manifest_path();
    m_library = direct().library_path();
    m_discovered = direct().discovered();
    m_error = direct().last_error();
    return ok;
}

bool OpenXR::discover() {
    const bool ok = direct().discover();
    m_manifest = direct().manifest_path();
    m_library = direct().library_path();
    m_discovered = direct().discovered();
    m_error = direct().last_error();
    return ok;
}

bool OpenXR::load() {
    const bool ok = direct().load();
    m_manifest = direct().manifest_path();
    m_library = direct().library_path();
    m_discovered = direct().discovered();
    m_crashed = direct().crashed();
    m_error = direct().last_error();

    if (!ok) {
        return false;
    }

    m_get_proc = reinterpret_cast<PFN_GetInstanceProcAddr>(direct().get_proc());
    m_interface_version = direct().interface_version();
    m_api_version = direct().api_version();
    return true;
}

OpenXR& OpenXR::get() {
    static OpenXR instance{};
    return instance;
}

bool OpenXR::attach_proxy(const char* proxy_path) {
    if (m_proxy != nullptr) {
        return true;
    }

    m_error.clear();
    m_crashed = false;
    m_faulted = false;

    std::string path = proxy_path == nullptr ? "fear2xr.dll" : proxy_path;

    if (path.find('\\') == std::string::npos && path.find('/') == std::string::npos) {
        // Next to THIS module. A bare name would otherwise be resolved against the game's working
        // directory, which is not where the build puts anything.
        HMODULE self = nullptr;

        if (::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                     GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                 reinterpret_cast<LPCSTR>(&directory_of), &self) != 0) {
            char buf[MAX_PATH]{};

            if (::GetModuleFileNameA(self, buf, sizeof(buf)) != 0) {
                path = directory_of(buf) + "\\" + path;
            }
        }
    }

    HMODULE mod = ::LoadLibraryA(path.c_str());

    if (mod == nullptr) {
        m_error = "proxy not loadable (" + std::to_string(::GetLastError()) + "): " + path;
        return false;
    }

    auto get_api = reinterpret_cast<PFN_fear2xr_get_api>(::GetProcAddress(mod, "fear2xr_get_api"));

    if (get_api == nullptr) {
        m_error = "proxy exports no fear2xr_get_api: " + path;
        return false;
    }

    const Fear2XrApi* api = get_api(FEAR2XR_API_VERSION);

    // A resident proxy from an EARLIER build cannot be replaced without restarting the game, so a
    // version or layout mismatch has to be reported plainly rather than papered over -- calling
    // through a struct shorter than we expect would read past its end.
    if (api == nullptr || api->struct_size != sizeof(Fear2XrApi) ||
        api->version != FEAR2XR_API_VERSION) {
        m_error = "resident proxy is a different build -- restart the game to replace it";
        return false;
    }

    m_proxy = api;
    return true;
}

bool OpenXR::load_via_proxy(const char* proxy_path) {
    if (!attach_proxy(proxy_path)) {
        return false;
    }

    const auto* api = static_cast<const Fear2XrApi*>(m_proxy);
    api->ensure_runtime();

    m_library = api->runtime_library();
    m_discovered = !m_library.empty();
    m_crashed = api->runtime_crashed() != 0;

    if (api->runtime_loaded() == 0 || api->get_instance_proc_addr == nullptr) {
        m_error = api->last_error();
        return false;
    }

    m_get_proc = api->get_instance_proc_addr;
    m_interface_version = api->interface_version();
    m_api_version = (static_cast<uint64_t>(api->api_major()) << 48) |
                    (static_cast<uint64_t>(api->api_minor()) << 32);
    return true;
}

bool OpenXR::persist_handle(const char* key, XrHandle value) {
    if (m_proxy == nullptr || key == nullptr) {
        return false;
    }

    static_cast<const Fear2XrApi*>(m_proxy)->store_handle(key, value);
    return true;
}

OpenXR::XrHandle OpenXR::persisted_handle(const char* key) const {
    if (m_proxy == nullptr || key == nullptr) {
        return 0;
    }

    return static_cast<const Fear2XrApi*>(m_proxy)->load_handle(key);
}

void OpenXR::unload() {
    // In proxy mode the runtime is NOT ours to free -- it belongs to a module that stays. Detach
    // and leave it running, which is exactly the point: the next mod load finds it ready.
    if (m_proxy == nullptr) {
        direct().unload();
    }

    m_proxy = nullptr;
    m_get_proc = nullptr;
    m_interface_version = 0;
    m_api_version = 0;
    m_faulted = false;
    m_instance = 0;
    m_extensions.clear();
}

int32_t OpenXR::resolve(const char* name, void** out, XrHandle instance) {
    if (out != nullptr) {
        *out = nullptr;
    }

    if (m_get_proc == nullptr || name == nullptr || out == nullptr || m_faulted) {
        return -1;  // XR_ERROR_VALIDATION_FAILURE
    }

    int32_t result = 0;

    if (guarded_call3(m_get_proc, instance, name, out, &result) != 0) {
        // LATCHED: a runtime that faults once is not going to behave on the next call, and poking
        // it repeatedly turns one bad install into a stream of near-misses. Everything after this
        // fails cleanly until the consumer unloads.
        m_faulted = true;
        m_crashed = true;
        m_error = std::string("runtime faulted resolving ") + name;
        return -1;
    }

    return result;
}

bool OpenXR::enumerate_extensions(std::vector<std::string>& out) {
    out.clear();

    if (!load()) {
        return false;
    }

    void* fn = nullptr;

    // XR_NULL_HANDLE: this is one of the few entry points valid with no instance, which is what
    // makes it reachable on a machine with no headset attached.
    const int32_t rr = resolve("xrEnumerateInstanceExtensionProperties", &fn);

    if (rr != XR_SUCCESS || fn == nullptr) {
        m_error = "runtime does not resolve xrEnumerateInstanceExtensionProperties (XrResult " +
                  std::to_string(rr) + ")";
        return false;
    }

    auto enumerate = reinterpret_cast<PFN_xrEnumerateInstanceExtensionProperties>(fn);
    uint32_t count = 0;

    int32_t er = 0;

    if (guarded_call4(enumerate, nullptr, 0u, &count, nullptr, &er) != 0) {
        m_faulted = true;
        m_crashed = true;
        m_error = "runtime faulted counting extensions";
        return false;
    }

    if (er != XR_SUCCESS) {
        m_error = "extension count query failed (XrResult " + std::to_string(er) + ")";
        return false;
    }

    if (count == 0) {
        return true;
    }

    std::vector<XrExtensionProperties> props(count);

    for (auto& p : props) {
        p.type = XR_TYPE_EXTENSION_PROPERTIES;
    }

    uint32_t written = 0;

    if (guarded_call4(enumerate, nullptr, count, &written, props.data(), &er) != 0) {
        m_faulted = true;
        m_crashed = true;
        m_error = "runtime faulted enumerating extensions";
        return false;
    }

    if (er != XR_SUCCESS) {
        m_error = "extension enumeration failed (XrResult " + std::to_string(er) + ")";
        return false;
    }

    out.reserve(written);

    for (uint32_t i = 0; i < written; ++i) {
        props[i].extensionName[sizeof(props[i].extensionName) - 1] = '\0';
        out.emplace_back(props[i].extensionName);
    }

    m_extensions = out;
    return true;
}

bool OpenXR::create_instance(const char* app_name, const std::vector<std::string>& extensions,
                             bool force) {
    m_error.clear();

    if (m_get_proc == nullptr) {
        m_error = "no runtime loaded";
        return false;
    }

    // ADOPT rather than rebuild. The proxy outlives this DLL, so a reload normally finds the
    // instance it made last time still alive and valid -- which is the entire point of parking it.
    if (!force) {
        if (m_instance != 0) {
            return true;
        }

        const XrHandle parked = persisted_handle("xr_instance");

        if (parked != 0) {
            m_instance = parked;
            return true;
        }
    }

    void* fn = nullptr;

    if (resolve("xrCreateInstance", &fn) != XR_SUCCESS || fn == nullptr) {
        m_error = "runtime does not resolve xrCreateInstance";
        return false;
    }

    XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
        info.applicationInfo.applicationVersion = 1;
    info.applicationInfo.engineVersion = 1;
    // 1.0 rather than the runtime's own version: an application declares what it was WRITTEN
    // against, and a 1.1 runtime is required to serve a 1.0 app.
    info.applicationInfo.apiVersion = make_version(1, 0, 34);

    const char* const name = app_name == nullptr ? "FEAR2VR" : app_name;
    ::strncpy_s(info.applicationInfo.applicationName, sizeof(info.applicationInfo.applicationName),
                name, _TRUNCATE);
    ::strncpy_s(info.applicationInfo.engineName, sizeof(info.applicationInfo.engineName),
                "LithTech Jupiter EX", _TRUNCATE);

    // The array must outlive the call, so the pointers are taken from storage that does.
    std::vector<const char*> ext_ptrs;
    ext_ptrs.reserve(extensions.size());

    for (const auto& e : extensions) {
        ext_ptrs.push_back(e.c_str());
    }

    info.enabledExtensionCount = static_cast<uint32_t>(ext_ptrs.size());
    info.enabledExtensionNames = ext_ptrs.empty() ? nullptr : ext_ptrs.data();

    uint64_t handle = 0;
    int32_t result = 0;

    if (guarded_call2(reinterpret_cast<PFN_xrCreateInstance>(fn), &info, &handle, &result) != 0) {
        m_faulted = true;
        m_crashed = true;
        m_error = "runtime faulted creating an instance";
        return false;
    }

    m_last_result = result;

    if (result != XR_SUCCESS || handle == 0) {
        m_error = "xrCreateInstance failed (XrResult " + std::to_string(result) + ")";
        return false;
    }

    m_instance = handle;
    persist_handle("xr_instance", handle);

    // Now that an instance exists the runtime will say who it is and what version it speaks; the
    // loader route cannot report either before this point.
    std::string rt_name;
    uint64_t rt_version = 0;
    instance_properties(rt_name, rt_version);
    return true;
}

bool OpenXR::destroy_instance() {
    if (m_instance == 0) {
        return true;
    }

    void* fn = nullptr;

    // Instance-level functions are resolved THROUGH the instance, not a null handle.
    if (resolve("xrDestroyInstance", &fn, m_instance) != XR_SUCCESS || fn == nullptr) {
        m_error = "runtime does not resolve xrDestroyInstance";
        return false;
    }

    int32_t result = 0;
    const int faulted =
        guarded_call1(reinterpret_cast<PFN_xrDestroyInstance>(fn), m_instance, &result);

    m_instance = 0;
    persist_handle("xr_instance", 0);

    if (faulted != 0) {
        m_faulted = true;
        m_crashed = true;
        m_error = "runtime faulted destroying the instance";
        return false;
    }

    m_last_result = result;
    return result == XR_SUCCESS;
}

int32_t OpenXR::get_system(XrHandle& out_system, uint32_t form_factor) {
    out_system = 0;

    if (m_instance == 0) {
        m_error = "no instance";
        return -1;  // XR_ERROR_VALIDATION_FAILURE
    }

    void* fn = nullptr;
    const int32_t rr = resolve("xrGetSystem", &fn, m_instance);

    if (rr != XR_SUCCESS || fn == nullptr) {
        m_error = "runtime does not resolve xrGetSystem";
        return rr == XR_SUCCESS ? -1 : rr;
    }

    XrSystemGetInfo info{XR_TYPE_SYSTEM_GET_INFO};
    info.formFactor = static_cast<XrFormFactor>(form_factor);

    uint64_t id = 0;
    int32_t result = 0;

    if (guarded_call3(reinterpret_cast<PFN_xrGetSystem>(fn), m_instance, &info, &id, &result) !=
        0) {
        m_faulted = true;
        m_crashed = true;
        m_error = "runtime faulted in xrGetSystem";
        return -1;
    }

    m_last_result = result;
    out_system = id;

    if (result == XR_SUCCESS) {
        m_system = id;
    }

    return result;
}


bool OpenXR::graphics_requirements(uint64_t& adapter_luid, uint32_t& min_feature_level) {
    adapter_luid = 0;
    min_feature_level = 0;

    if (m_instance == 0) {
        m_error = "no instance";
        return false;
    }

    if (m_system == 0) {
        XrHandle ignored = 0;

        if (get_system(ignored) != XR_SUCCESS) {
            m_error = "no system -- is a headset connected?";
            return false;
        }
    }

    void* fn = nullptr;

    // An EXTENSION entry point: it exists only because XR_KHR_D3D11_enable was enabled at instance
    // creation, so resolving it also proves the extension really took.
    if (resolve("xrGetD3D11GraphicsRequirementsKHR", &fn, m_instance) != XR_SUCCESS || fn == nullptr) {
        m_error = "xrGetD3D11GraphicsRequirementsKHR unavailable -- was XR_KHR_D3D11_enable on?";
        return false;
    }

    XrGraphicsRequirementsD3D11KHR reqs{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    int32_t result = 0;

    if (guarded_call3(reinterpret_cast<PFN_xrGetD3D11GraphicsRequirementsKHR>(fn), m_instance, m_system, &reqs,
                      &result) != 0) {
        m_faulted = true;
        m_error = "runtime faulted reporting graphics requirements";
        return false;
    }

    m_last_result = result;

    if (result != XR_SUCCESS) {
        m_error = "graphics requirements failed (XrResult " + std::to_string(result) + ")";
        return false;
    }

    adapter_luid = (static_cast<uint64_t>(static_cast<uint32_t>(reqs.adapterLuid.HighPart)) << 32) |
                   static_cast<uint64_t>(reqs.adapterLuid.LowPart);
    min_feature_level = reqs.minFeatureLevel;
    return true;
}

bool OpenXR::ensure_d3d11_device() {
    if (m_d3d11 != nullptr) {
        return true;
    }

    // Adopt one a previous mod load parked. A second device would not be interchangeable: the
    // session holds a reference to the exact one it was created with.
    if (const XrHandle parked = persisted_handle("d3d11_device"); parked != 0) {
        m_d3d11 = reinterpret_cast<void*>(static_cast<uintptr_t>(parked));
        return true;
    }

    uint64_t luid = 0;
    uint32_t min_level = 0;

    if (!graphics_requirements(luid, min_level)) {
        return false;
    }

    // THE RUNTIME CHOOSES THE ADAPTER, not us. A device on a different one is rejected or presents
    // to nothing, and on any machine with more than one GPU that is a real possibility.
    IDXGIFactory1* factory = nullptr;

    if (FAILED(::CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory)))) {
        m_error = "CreateDXGIFactory1 failed";
        return false;
    }

    IDXGIAdapter1* chosen = nullptr;

    for (UINT i = 0;; ++i) {
        IDXGIAdapter1* adapter = nullptr;

        if (factory->EnumAdapters1(i, &adapter) != S_OK) {
            break;
        }

        DXGI_ADAPTER_DESC1 desc{};

        if (SUCCEEDED(adapter->GetDesc1(&desc))) {
            const uint64_t id =
                (static_cast<uint64_t>(static_cast<uint32_t>(desc.AdapterLuid.HighPart)) << 32) |
                static_cast<uint64_t>(desc.AdapterLuid.LowPart);

            if (id == luid) {
                chosen = adapter;
                break;
            }
        }

        adapter->Release();
    }

    factory->Release();

    if (chosen == nullptr) {
        m_error = "no DXGI adapter matches the runtime's LUID";
        return false;
    }

    const D3D_FEATURE_LEVEL wanted =
        static_cast<D3D_FEATURE_LEVEL>(min_level == 0 ? D3D_FEATURE_LEVEL_11_0 : min_level);
    D3D_FEATURE_LEVEL got{};
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;

    // DRIVER_TYPE_UNKNOWN is REQUIRED when an adapter is supplied; HARDWARE with a non-null adapter
    // fails with E_INVALIDARG and reads like a driver problem.
    const HRESULT hr =
        ::D3D11CreateDevice(chosen, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                            D3D11_CREATE_DEVICE_BGRA_SUPPORT, &wanted, 1, D3D11_SDK_VERSION,
                            &device, &got, &context);
    chosen->Release();

    if (FAILED(hr) || device == nullptr) {
        m_error = "D3D11CreateDevice failed";
        return false;
    }

    if (context != nullptr) {
        context->Release();
    }

    m_d3d11 = device;
    persist_handle("d3d11_device", static_cast<XrHandle>(reinterpret_cast<uintptr_t>(device)));
    return true;
}

bool OpenXR::instance_properties(std::string& name, uint64_t& version) {
    name.clear();
    version = 0;

    if (m_instance == 0) {
        return false;
    }

    void* fn = nullptr;

    if (resolve("xrGetInstanceProperties", &fn, m_instance) != XR_SUCCESS || fn == nullptr) {
        return false;
    }

    XrInstanceProperties props{XR_TYPE_INSTANCE_PROPERTIES};
    int32_t result = 0;

    if (guarded_call2(reinterpret_cast<PFN_xrGetInstanceProperties>(fn), m_instance, &props,
                      &result) != 0 ||
        result != XR_SUCCESS) {
        return false;
    }

    props.runtimeName[XR_MAX_RUNTIME_NAME_SIZE - 1] = '\0';
    name = props.runtimeName;
    version = props.runtimeVersion;
    m_runtime_name = name;
    m_runtime_version = version;
    return true;
}

bool OpenXR::session_unsupported_here() {
    if constexpr (sizeof(void*) != 4) {
        return false;
    }

    // Matched on the RUNTIME's own name, not the library on disk: with the loader in the path the
    // library is openxr_loader.dll for every runtime alike, and an earlier version of this check
    // was silently answering "supported" and letting the fatal call through.
    if (m_runtime_name.empty()) {
        std::string probe_name;
        uint64_t probe_version = 0;
        instance_properties(probe_name, probe_version);
    }

    std::string lowered = m_runtime_name;

    for (auto& ch : lowered) {
        ch = static_cast<char>(::tolower(static_cast<unsigned char>(ch)));
    }

    return lowered.find("oculus") != std::string::npos;
}

bool OpenXR::create_session(bool force) {
    if (m_session != 0) {
        return true;
    }

    if (!force && session_unsupported_here()) {
        m_error =
            "the 32-bit Oculus runtime faults inside its own RuntimeIPC init during "
            "xrCreateSession -- verified against a 64-bit control on this machine. Route sessions "
            "through a 64-bit host instead; calling this would take the game down.";
        return false;
    }

    if (const XrHandle parked = persisted_handle("xr_session"); parked != 0) {
        m_session = parked;
        return true;
    }

    if (!ensure_d3d11_device()) {
        return false;
    }

    void* fn = nullptr;

    if (resolve("xrCreateSession", &fn, m_instance) != XR_SUCCESS || fn == nullptr) {
        m_error = "runtime does not resolve xrCreateSession";
        return false;
    }

    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    binding.device = static_cast<ID3D11Device*>(m_d3d11);

    XrSessionCreateInfo info{XR_TYPE_SESSION_CREATE_INFO};
    info.next = &binding;
    info.systemId = m_system;

    uint64_t handle = 0;
    int32_t result = 0;

    if (guarded_call3(reinterpret_cast<PFN_xrCreateSession>(fn), m_instance, &info, &handle,
                      &result) != 0) {
        m_faulted = true;
        m_error = "runtime faulted creating a session";
        return false;
    }

    m_last_result = result;

    if (result != XR_SUCCESS || handle == 0) {
        m_error = "xrCreateSession failed (XrResult " + std::to_string(result) + ")";
        return false;
    }

    m_session = handle;
    persist_handle("xr_session", handle);
    return true;
}

int32_t OpenXR::probe_session_without_graphics() {
    void* fn = nullptr;

    if (resolve("xrCreateSession", &fn, m_instance) != XR_SUCCESS || fn == nullptr) {
        return -1;
    }

    XrSessionCreateInfo info{XR_TYPE_SESSION_CREATE_INFO};
    info.systemId = m_system;

    uint64_t handle = 0;
    int32_t result = 0;

    if (guarded_call3(reinterpret_cast<PFN_xrCreateSession>(fn), m_instance, &info, &handle,
                      &result) != 0) {
        return -2;  // faulted
    }

    if (handle != 0) {
        void* d = nullptr;
        int32_t ignored = 0;

        if (resolve("xrDestroySession", &d, m_instance) == XR_SUCCESS && d != nullptr) {
            guarded_call1(reinterpret_cast<PFN_xrDestroySession>(d), handle, &ignored);
        }
    }

    return result;
}

bool OpenXR::destroy_session() {
    if (m_session == 0) {
        return true;
    }

    void* fn = nullptr;
    int32_t result = 0;

    if (resolve("xrDestroySession", &fn, m_instance) == XR_SUCCESS && fn != nullptr) {
        guarded_call1(reinterpret_cast<PFN_xrDestroySession>(fn), m_session, &result);
    }

    m_session = 0;
    m_session_running = false;
    m_session_state = SessionState::Unknown;
    persist_handle("xr_session", 0);
    return result == XR_SUCCESS;
}

const char* OpenXR::state_name(SessionState s) {
    switch (s) {
    case SessionState::Idle: return "IDLE";
    case SessionState::Ready: return "READY";
    case SessionState::Synchronized: return "SYNCHRONIZED";
    case SessionState::Visible: return "VISIBLE";
    case SessionState::Focused: return "FOCUSED";
    case SessionState::Stopping: return "STOPPING";
    case SessionState::LossPending: return "LOSS_PENDING";
    case SessionState::Exiting: return "EXITING";
    default: return "unknown";
    }
}

void OpenXR::poll_events() {
    if (m_instance == 0) {
        return;
    }

    void* fn = nullptr;

    if (resolve("xrPollEvent", &fn, m_instance) != XR_SUCCESS || fn == nullptr) {
        return;
    }

    auto poll = reinterpret_cast<PFN_xrPollEvent>(fn);

    // DRAIN, do not peek. The runtime emits several events during a transition, and a consumer that
    // takes one per frame falls further behind the harder the runtime is working.
    for (int i = 0; i < 64; ++i) {
        XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
        int32_t result = 0;

        if (guarded_call2(poll, m_instance, &ev, &result) != 0) {
            m_faulted = true;
            return;
        }

        if (result != XR_SUCCESS) {
            return;  // XR_EVENT_UNAVAILABLE
        }

        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            const auto* sc = reinterpret_cast<const XrEventDataSessionStateChanged*>(&ev);
            m_session_state = static_cast<SessionState>(sc->state);
        }
    }
}

bool OpenXR::begin_session() {
    if (m_session == 0 || m_session_running) {
        return m_session_running;
    }

    // ONLY FROM READY. Calling earlier returns XR_ERROR_SESSION_NOT_READY, which looks like a
    // rendering bug to anyone who did not know a state machine was involved.
    if (m_session_state != SessionState::Ready) {
        return false;
    }

    void* fn = nullptr;

    if (resolve("xrBeginSession", &fn, m_instance) != XR_SUCCESS || fn == nullptr) {
        m_error = "runtime does not resolve xrBeginSession";
        return false;
    }

    XrSessionBeginInfo info{XR_TYPE_SESSION_BEGIN_INFO};
    info.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;

    int32_t result = 0;

    if (guarded_call2(reinterpret_cast<PFN_xrBeginSession>(fn), m_session, &info, &result) != 0) {
        m_faulted = true;
        m_error = "runtime faulted beginning the session";
        return false;
    }

    m_last_result = result;
    m_session_running = (result == XR_SUCCESS);
    return m_session_running;
}

bool OpenXR::end_session() {
    if (m_session == 0 || !m_session_running) {
        return true;
    }

    void* fn = nullptr;
    int32_t result = 0;

    if (resolve("xrEndSession", &fn, m_instance) == XR_SUCCESS && fn != nullptr) {
        guarded_call1(reinterpret_cast<PFN_xrEndSession>(fn), m_session, &result);
    }

    m_session_running = false;
    return result == XR_SUCCESS;
}

bool OpenXR::supports_extension(const char* name) {
    if (name == nullptr) {
        return false;
    }

    if (m_extensions.empty()) {
        std::vector<std::string> ignored;
        enumerate_extensions(ignored);
    }

    for (const auto& e : m_extensions) {
        if (e == name) {
            return true;
        }
    }

    return false;
}

}  // namespace sdk
