#include "OpenXR.hpp"

#include "xr/Fear2XrApi.h"
#include "xr/RuntimeLoader.hpp"

#include <windows.h>

#include <cstddef>
#include <cstring>
#include <fstream>
#include <sstream>

namespace sdk {

namespace {

constexpr int32_t kSuccess = 0;  // XR_SUCCESS

// XR_TYPE_EXTENSION_PROPERTIES. Established by ASKING THE RUNTIME rather than trusting a remembered
// enum: 0 and 1 were rejected with XR_ERROR_VALIDATION_FAILURE, 2 accepted. An earlier version
// guessed 3 -- XR_TYPE_INSTANCE_CREATE_INFO -- and the validation failure looked like a broken
// runtime.
constexpr uint32_t kTypeExtensionProperties = 2;

constexpr uint64_t make_version(uint64_t major, uint64_t minor, uint64_t patch) {
    return (major << 48) | (minor << 32) | patch;
}

std::string directory_of(const std::string& path) {
    const size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? std::string{} : path.substr(0, slash);
}



// ---- THE LOADER<->RUNTIME NEGOTIATION ABI ------------------------------------------------------
//
// Declared here rather than vendoring the OpenXR SDK: this is the entire surface needed to reach a
// runtime, it is fixed by the loader specification, and adding a fetched dependency to the build for
// six structs would be a poor trade. Field order and the constants below are the spec's.
// XrExtensionProperties. The name array is XR_MAX_EXTENSION_NAME_SIZE = 128.
struct ExtensionProperties {
    uint32_t type;  // XR_TYPE_EXTENSION_PROPERTIES = 3
    void* next;
    char extensionName[128];
    uint32_t extensionVersion;
};

using PFN_EnumerateExtensions = int32_t(__stdcall*)(const char*, uint32_t, uint32_t*,
                                                    ExtensionProperties*);

// ---- INSTANCE AND SYSTEM STRUCTURES ------------------------------------------------------------
//
// Declared rather than vendored, same as the negotiation ABI above, and asserted rather than
// trusted. Layout is where a 32-bit consumer of this API gets hurt: XrVersion and XrFlags64 are
// 64-bit and force 8-byte alignment inside otherwise 32-bit structs, so a hand-written definition
// that "looks right" can still be four bytes out. The static_asserts below are the cheap version of
// the lesson the handle size already taught this project.
constexpr uint32_t kTypeInstanceCreateInfo = 3;
constexpr uint32_t kTypeSystemGetInfo = 4;

struct ApplicationInfo {
    char applicationName[128];  // XR_MAX_APPLICATION_NAME_SIZE
    uint32_t applicationVersion;
    char engineName[128];  // XR_MAX_ENGINE_NAME_SIZE
    uint32_t engineVersion;
    uint64_t apiVersion;
};

struct InstanceCreateInfo {
    uint32_t type;
    const void* next;
    uint64_t createFlags;
    ApplicationInfo applicationInfo;
    uint32_t enabledApiLayerCount;
    const char* const* enabledApiLayerNames;
    uint32_t enabledExtensionCount;
    const char* const* enabledExtensionNames;
};

struct SystemGetInfo {
    uint32_t type;
    const void* next;
    uint32_t formFactor;
};

static_assert(sizeof(ApplicationInfo) == 272, "XrApplicationInfo layout");
static_assert(offsetof(InstanceCreateInfo, applicationInfo) == 16, "createFlags is 64-bit");
static_assert(sizeof(InstanceCreateInfo) == 304, "XrInstanceCreateInfo layout");
static_assert(sizeof(SystemGetInfo) == 12, "XrSystemGetInfo layout");

using PFN_CreateInstance = int32_t(__stdcall*)(const InstanceCreateInfo*, uint64_t*);
using PFN_DestroyInstance = int32_t(__stdcall*)(uint64_t);
using PFN_GetSystem = int32_t(__stdcall*)(uint64_t, const SystemGetInfo*, uint64_t*);

static_assert(sizeof(OpenXR::XrHandle) == 8, "OpenXR handles are 64-bit on every platform");

// The manifest is small and its shape is fixed, so the one field we need is lifted directly rather
// than by pulling in a JSON parser. Deliberately narrow: anything unexpected fails the discovery
// instead of guessing at a path.
int guarded_call_get_proc(OpenXR::PFN_GetInstanceProcAddr fn, OpenXR::XrHandle instance,
                          const char* name,
                          void** out, int32_t* result) {
    __try {
        *result = fn(instance, name, out);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *out = nullptr;
        return 1;
    }
}

int guarded_enumerate(PFN_EnumerateExtensions fn, uint32_t capacity, uint32_t* count,
                      ExtensionProperties* props, int32_t* result) {
    __try {
        *result = fn(nullptr, capacity, count, props);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
}

int guarded_create_instance(PFN_CreateInstance fn, const InstanceCreateInfo* info, uint64_t* out,
                            int32_t* result) {
    __try {
        *result = fn(info, out);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
}

int guarded_destroy_instance(PFN_DestroyInstance fn, uint64_t instance, int32_t* result) {
    __try {
        *result = fn(instance);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
}

int guarded_get_system(PFN_GetSystem fn, uint64_t instance, const SystemGetInfo* info,
                       uint64_t* out, int32_t* result) {
    __try {
        *result = fn(instance, info, out);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
}

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

    if (guarded_call_get_proc(m_get_proc, instance, name, out, &result) != 0) {
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

    if (rr != kSuccess || fn == nullptr) {
        m_error = "runtime does not resolve xrEnumerateInstanceExtensionProperties (XrResult " +
                  std::to_string(rr) + ")";
        return false;
    }

    auto enumerate = reinterpret_cast<PFN_EnumerateExtensions>(fn);
    uint32_t count = 0;

    int32_t er = 0;

    if (guarded_enumerate(enumerate, 0, &count, nullptr, &er) != 0) {
        m_faulted = true;
        m_crashed = true;
        m_error = "runtime faulted counting extensions";
        return false;
    }

    if (er != kSuccess) {
        m_error = "extension count query failed (XrResult " + std::to_string(er) + ")";
        return false;
    }

    if (count == 0) {
        return true;
    }

    std::vector<ExtensionProperties> props(count);

    for (auto& p : props) {
        p.type = kTypeExtensionProperties;
    }

    uint32_t written = 0;

    if (guarded_enumerate(enumerate, count, &written, props.data(), &er) != 0) {
        m_faulted = true;
        m_crashed = true;
        m_error = "runtime faulted enumerating extensions";
        return false;
    }

    if (er != kSuccess) {
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

    if (resolve("xrCreateInstance", &fn) != kSuccess || fn == nullptr) {
        m_error = "runtime does not resolve xrCreateInstance";
        return false;
    }

    InstanceCreateInfo info{};
    info.type = kTypeInstanceCreateInfo;
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

    if (guarded_create_instance(reinterpret_cast<PFN_CreateInstance>(fn), &info, &handle,
                                &result) != 0) {
        m_faulted = true;
        m_crashed = true;
        m_error = "runtime faulted creating an instance";
        return false;
    }

    m_last_result = result;

    if (result != kSuccess || handle == 0) {
        m_error = "xrCreateInstance failed (XrResult " + std::to_string(result) + ")";
        return false;
    }

    m_instance = handle;
    persist_handle("xr_instance", handle);
    return true;
}

bool OpenXR::destroy_instance() {
    if (m_instance == 0) {
        return true;
    }

    void* fn = nullptr;

    // Instance-level functions are resolved THROUGH the instance, not a null handle.
    if (resolve("xrDestroyInstance", &fn, m_instance) != kSuccess || fn == nullptr) {
        m_error = "runtime does not resolve xrDestroyInstance";
        return false;
    }

    int32_t result = 0;
    const int faulted =
        guarded_destroy_instance(reinterpret_cast<PFN_DestroyInstance>(fn), m_instance, &result);

    m_instance = 0;
    persist_handle("xr_instance", 0);

    if (faulted != 0) {
        m_faulted = true;
        m_crashed = true;
        m_error = "runtime faulted destroying the instance";
        return false;
    }

    m_last_result = result;
    return result == kSuccess;
}

int32_t OpenXR::get_system(XrHandle& out_system, uint32_t form_factor) {
    out_system = 0;

    if (m_instance == 0) {
        m_error = "no instance";
        return -1;  // XR_ERROR_VALIDATION_FAILURE
    }

    void* fn = nullptr;
    const int32_t rr = resolve("xrGetSystem", &fn, m_instance);

    if (rr != kSuccess || fn == nullptr) {
        m_error = "runtime does not resolve xrGetSystem";
        return rr == kSuccess ? -1 : rr;
    }

    SystemGetInfo info{};
    info.type = kTypeSystemGetInfo;
    info.formFactor = form_factor;

    uint64_t id = 0;
    int32_t result = 0;

    if (guarded_get_system(reinterpret_cast<PFN_GetSystem>(fn), m_instance, &info, &id, &result) !=
        0) {
        m_faulted = true;
        m_crashed = true;
        m_error = "runtime faulted in xrGetSystem";
        return -1;
    }

    m_last_result = result;
    out_system = id;
    return result;
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
