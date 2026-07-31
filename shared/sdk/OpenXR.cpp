#include "OpenXR.hpp"

#include <windows.h>

#include <cstring>
#include <fstream>
#include <sstream>

namespace sdk {

namespace {

// ---- THE LOADER<->RUNTIME NEGOTIATION ABI ------------------------------------------------------
//
// Declared here rather than vendoring the OpenXR SDK: this is the entire surface needed to reach a
// runtime, it is fixed by the loader specification, and adding a fetched dependency to the build for
// six structs would be a poor trade. Field order and the constants below are the spec's.
constexpr uint32_t kStructLoaderInfo = 1;      // XR_LOADER_INTERFACE_STRUCT_LOADER_INFO
constexpr uint32_t kStructRuntimeRequest = 3;  // XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST
constexpr uint32_t kLoaderInfoVersion = 1;     // XR_LOADER_INFO_STRUCT_VERSION
constexpr uint32_t kRuntimeInfoVersion = 1;    // XR_RUNTIME_INFO_STRUCT_VERSION
constexpr uint32_t kCurrentLoaderRuntimeVersion = 1;
constexpr int32_t kSuccess = 0;  // XR_SUCCESS

// XR_TYPE_EXTENSION_PROPERTIES. Established by ASKING THE RUNTIME rather than by trusting a
// remembered enum: values 0 and 1 were rejected with XR_ERROR_VALIDATION_FAILURE and 2 was accepted.
// The first version of this file guessed 3 -- which is XR_TYPE_INSTANCE_CREATE_INFO -- and produced
// a validation failure that looked exactly like a broken runtime.
constexpr uint32_t kTypeExtensionProperties = 2;

// XR_MAKE_VERSION(major, minor, patch)
constexpr uint64_t make_version(uint64_t major, uint64_t minor, uint64_t patch) {
    return (major << 48) | (minor << 32) | patch;
}

struct NegotiateLoaderInfo {
    uint32_t structType;
    uint32_t structVersion;
    size_t structSize;
    uint32_t minInterfaceVersion;
    uint32_t maxInterfaceVersion;
    uint64_t minApiVersion;
    uint64_t maxApiVersion;
};

struct NegotiateRuntimeRequest {
    uint32_t structType;
    uint32_t structVersion;
    size_t structSize;
    uint32_t runtimeInterfaceVersion;
    uint64_t runtimeApiVersion;
    OpenXR::PFN_GetInstanceProcAddr getInstanceProcAddr;
};

using PFN_Negotiate = int32_t(__stdcall*)(const NegotiateLoaderInfo*, NegotiateRuntimeRequest*);

// XrExtensionProperties. The name array is XR_MAX_EXTENSION_NAME_SIZE = 128.
struct ExtensionProperties {
    uint32_t type;  // XR_TYPE_EXTENSION_PROPERTIES = 3
    void* next;
    char extensionName[128];
    uint32_t extensionVersion;
};

using PFN_EnumerateExtensions = int32_t(__stdcall*)(const char*, uint32_t, uint32_t*,
                                                    ExtensionProperties*);

static_assert(sizeof(OpenXR::XrHandle) == 8, "OpenXR handles are 64-bit on every platform");

// The manifest is small and its shape is fixed, so the one field we need is lifted directly rather
// than by pulling in a JSON parser. Deliberately narrow: anything unexpected fails the discovery
// instead of guessing at a path.
std::string extract_library_path(const std::string& json) {
    const size_t key = json.find("\"library_path\"");

    if (key == std::string::npos) {
        return {};
    }

    const size_t colon = json.find(':', key);

    if (colon == std::string::npos) {
        return {};
    }

    const size_t open = json.find('"', colon);

    if (open == std::string::npos) {
        return {};
    }

    std::string out;

    for (size_t i = open + 1; i < json.size(); ++i) {
        const char c = json[i];

        if (c == '"') {
            return out;
        }

        // JSON escapes backslashes, and Windows paths are full of them.
        if (c == '\\' && i + 1 < json.size()) {
            ++i;
            out.push_back(json[i]);
            continue;
        }

        out.push_back(c);
    }

    return {};
}

std::string directory_of(const std::string& path) {
    const size_t slash = path.find_last_of("\\/");
    return slash == std::string::npos ? std::string{} : path.substr(0, slash);
}

// ---- SURVIVING A BROKEN RUNTIME ----------------------------------------------------------------
//
// Separate functions with no C++ objects in scope, because __try cannot coexist with unwinding. The
// game's crash reporter uses SetUnhandledExceptionFilter, which only runs when nothing CLAIMS the
// exception -- so claiming it here is what keeps the process alive.
int guarded_load(const char* path, HMODULE* out) {
    __try {
        *out = ::LoadLibraryExA(path, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *out = nullptr;
        return 1;
    }
}

// EVERY call into the runtime, not just the negotiation. The first version of this class guarded
// the load and then called the runtime's own xrGetInstanceProcAddr unguarded -- which jumped into
// unmapped memory and took the game down exactly as before. A pointer a foreign library handed us
// is foreign code too.
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

int guarded_negotiate(PFN_Negotiate fn, const NegotiateLoaderInfo* info,
                      NegotiateRuntimeRequest* request, int32_t* result) {
    __try {
        *result = fn(info, request);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
}

}  // namespace

std::vector<std::string> OpenXR::available_runtimes() {
    std::vector<std::string> out;
    HKEY key = nullptr;

    if (::RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Khronos\\OpenXR\\1\\AvailableRuntimes", 0,
                        KEY_READ, &key) != ERROR_SUCCESS) {
        return out;
    }

    char name[MAX_PATH * 2]{};
    DWORD index = 0;

    for (;;) {
        DWORD len = sizeof(name);
        DWORD type = 0;

        if (::RegEnumValueA(key, index++, name, &len, nullptr, &type, nullptr, nullptr) !=
            ERROR_SUCCESS) {
            break;
        }

        out.emplace_back(name);
    }

    ::RegCloseKey(key);
    return out;
}

bool OpenXR::select_manifest(const std::string& manifest) {
    unload();
    m_discovered = false;
    m_error.clear();
    m_manifest = manifest;

    std::ifstream f(m_manifest, std::ios::binary);

    if (!f) {
        m_error = "manifest unreadable: " + m_manifest;
        return false;
    }

    std::ostringstream ss;
    ss << f.rdbuf();
    const std::string lib = extract_library_path(ss.str());

    if (lib.empty()) {
        m_error = "manifest names no library_path: " + m_manifest;
        return false;
    }

    const bool absolute = lib.size() > 2 && (lib[1] == ':' || (lib[0] == '\\' && lib[1] == '\\'));
    m_library = absolute ? lib : directory_of(m_manifest) + "\\" + lib;
    m_discovered = true;
    return true;
}

OpenXR& OpenXR::get() {
    static OpenXR instance{};
    return instance;
}

bool OpenXR::discover() {
    m_discovered = false;
    m_manifest.clear();
    m_library.clear();
    m_error.clear();

    // A 32-bit process reads HKLM\SOFTWARE\Khronos through the WOW6432Node view automatically, so
    // this resolves the 32-BIT active runtime without asking for it -- which is exactly what we
    // want, and why a 64-bit tool checking the same key would report a different (useless) answer.
    char value[MAX_PATH * 2]{};
    DWORD size = sizeof(value);
    DWORD type = 0;
    LSTATUS st = ::RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Khronos\\OpenXR\\1", "ActiveRuntime",
                                RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND, &type, value,
                                &size);

    if (st != ERROR_SUCCESS) {
        m_error = "no ActiveRuntime registered for 32-bit (status " + std::to_string(st) + ")";
        return false;
    }

    char expanded[MAX_PATH * 2]{};

    if (::ExpandEnvironmentStringsA(value, expanded, sizeof(expanded)) == 0) {
        m_error = "could not expand the runtime manifest path";
        return false;
    }

    m_manifest = expanded;

    std::ifstream f(m_manifest, std::ios::binary);

    if (!f) {
        m_error = "runtime manifest is registered but unreadable: " + m_manifest;
        return false;
    }

    std::ostringstream ss;
    ss << f.rdbuf();
    const std::string lib = extract_library_path(ss.str());

    if (lib.empty()) {
        m_error = "manifest names no library_path: " + m_manifest;
        return false;
    }

    // Relative paths are resolved against the MANIFEST's directory, per the loader spec -- the
    // Oculus manifest uses ".\LibOVRRTImpl32_1.dll" and resolving it against the working directory
    // would silently look in the game's folder.
    const bool absolute = lib.size() > 2 && (lib[1] == ':' || (lib[0] == '\\' && lib[1] == '\\'));
    m_library = absolute ? lib : directory_of(m_manifest) + "\\" + lib;
    m_discovered = true;
    return true;
}

bool OpenXR::load() {
    if (loaded()) {
        return true;
    }

    if (!m_discovered && !discover()) {
        return false;
    }

    m_error.clear();
    m_crashed = false;
    m_faulted = false;

    // LOAD_WITH_ALTERED_SEARCH_PATH so the runtime finds its own siblings: LibOVRRTImpl32_1.dll
    // pulls in further Oculus libraries from its directory, which is not on our search path.
    HMODULE mod = nullptr;

    if (guarded_load(m_library.c_str(), &mod) != 0) {
        m_crashed = true;
        m_error = "runtime faulted while loading: " + m_library;
        return false;
    }

    if (mod == nullptr) {
        m_error = "LoadLibrary failed (" + std::to_string(::GetLastError()) + ") for " + m_library;
        return false;
    }

    auto negotiate =
        reinterpret_cast<PFN_Negotiate>(::GetProcAddress(mod, "xrNegotiateLoaderRuntimeInterface"));

    if (negotiate == nullptr) {
        ::FreeLibrary(mod);
        m_error = "runtime exports no xrNegotiateLoaderRuntimeInterface: " + m_library;
        return false;
    }

    NegotiateLoaderInfo info{};
    info.structType = kStructLoaderInfo;
    info.structVersion = kLoaderInfoVersion;
    info.structSize = sizeof(info);
    info.minInterfaceVersion = kCurrentLoaderRuntimeVersion;
    info.maxInterfaceVersion = kCurrentLoaderRuntimeVersion;
    info.minApiVersion = make_version(1, 0, 0);
    // Accept anything in 1.x: a runtime newer than us is not a reason to refuse to talk.
    info.maxApiVersion = make_version(1, 0xFFF, 0xFFFFFFFF);

    NegotiateRuntimeRequest request{};
    request.structType = kStructRuntimeRequest;
    request.structVersion = kRuntimeInfoVersion;
    request.structSize = sizeof(request);

    int32_t result = 0;

    if (guarded_negotiate(negotiate, &info, &request, &result) != 0) {
        // Do NOT FreeLibrary here: the runtime faulted partway through its own setup and its state
        // is unknown, so unloading it is at least as likely to fault again as to help.
        m_crashed = true;
        m_error = "runtime faulted during negotiation: " + m_library;
        return false;
    }

    if (result != kSuccess || request.getInstanceProcAddr == nullptr) {
        ::FreeLibrary(mod);
        m_error = "runtime refused negotiation (XrResult " + std::to_string(result) + ")";
        return false;
    }

    m_module = mod;
    m_get_proc = request.getInstanceProcAddr;
    m_interface_version = request.runtimeInterfaceVersion;
    m_api_version = request.runtimeApiVersion;
    return true;
}

void OpenXR::unload() {
    if (m_module != nullptr) {
        ::FreeLibrary(static_cast<HMODULE>(m_module));
        m_module = nullptr;
    }

    m_get_proc = nullptr;
    m_interface_version = 0;
    m_api_version = 0;
    m_faulted = false;
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

bool OpenXR::supports_extension(const char* name) const {
    if (name == nullptr) {
        return false;
    }

    for (const auto& e : m_extensions) {
        if (e == name) {
            return true;
        }
    }

    return false;
}

}  // namespace sdk
