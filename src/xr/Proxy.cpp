// The resident OpenXR proxy. See shared/xr/Fear2XrApi.h for why it exists.
//
// Kept deliberately small: this module can never be unloaded once the runtime is in it, so its code
// is frozen for the life of the game process. Anything that might need iterating belongs in the mod.

#include <windows.h>

#include <map>
#include <mutex>
#include <string>

#include "xr/Fear2XrApi.h"
#include "xr/RuntimeLoader.hpp"

namespace {

Fear2XrApi g_api{};
std::mutex g_lock;

// The runtime lives HERE, in the module that never leaves. Everything else about OpenXR -- the
// instance, the session, the policy -- belongs to the mod, which is free to be rebuilt.
xr::RuntimeLoader& runtime() {
    static xr::RuntimeLoader loader;
    return loader;
}

std::map<std::string, uint64_t> g_handles;
std::string g_library;
std::string g_error;

// Self-pin. Without this the mod's FreeLibrary would take the proxy with it -- and the OpenXR
// runtime's threads with THAT -- which is the exact failure the proxy exists to prevent. Pinning is
// the documented way to say "this module is here for the life of the process".
void pin_self() {
    static bool pinned = false;

    if (pinned) {
        return;
    }

    HMODULE self = nullptr;

    if (::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                             reinterpret_cast<LPCSTR>(&pin_self), &self) != 0) {
        pinned = true;
    }
}

int api_ensure_runtime() {
    static std::mutex once;
    std::lock_guard<std::mutex> guard(once);

    if (runtime().loaded()) {
        return 1;
    }

    // The load happens HERE, inside the resident module, which is the whole point: the runtime's
    // threads are created against this module's lifetime, not the mod's. sdk::OpenXR guards the
    // load, the negotiation and every call, so a broken runtime leaves the process alive with
    // `runtime_crashed()` set.
    if (!runtime().load()) {
        return 0;
    }

    g_api.get_instance_proc_addr = runtime().get_proc();
    return 1;
}

int api_runtime_loaded() {
    return runtime().loaded() ? 1 : 0;
}

int api_runtime_crashed() {
    return runtime().crashed() ? 1 : 0;
}

const char* api_runtime_library() {
    // Returned through a static: the caller is a separate module and must not receive a pointer
    // into a temporary.
    std::lock_guard<std::mutex> guard(g_lock);
    g_library = runtime().library_path();
    return g_library.c_str();
}

const char* api_last_error() {
    std::lock_guard<std::mutex> guard(g_lock);
    g_error = runtime().last_error();
    return g_error.c_str();
}

uint32_t api_api_major() {
    return runtime().api_major();
}

uint32_t api_api_minor() {
    return runtime().api_minor();
}

uint32_t api_interface_version() {
    return runtime().interface_version();
}

void api_store_handle(const char* key, uint64_t value) {
    if (key == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> guard(g_lock);
    g_handles[key] = value;
}

uint64_t api_load_handle(const char* key) {
    if (key == nullptr) {
        return 0;
    }

    std::lock_guard<std::mutex> guard(g_lock);
    const auto it = g_handles.find(key);
    return it == g_handles.end() ? 0 : it->second;
}

void api_clear_handles() {
    std::lock_guard<std::mutex> guard(g_lock);
    g_handles.clear();
}

}  // namespace

extern "C" __declspec(dllexport) const Fear2XrApi* __cdecl fear2xr_get_api(uint32_t version) {
    if (version != FEAR2XR_API_VERSION) {
        return nullptr;
    }

    pin_self();

    static bool initialised = false;

    if (!initialised) {
        initialised = true;

        g_api.struct_size = sizeof(Fear2XrApi);
        g_api.version = FEAR2XR_API_VERSION;
        g_api.ensure_runtime = &api_ensure_runtime;
        g_api.runtime_loaded = &api_runtime_loaded;
        g_api.runtime_crashed = &api_runtime_crashed;
        g_api.runtime_library = &api_runtime_library;
        g_api.last_error = &api_last_error;
        g_api.api_major = &api_api_major;
        g_api.api_minor = &api_api_minor;
        g_api.interface_version = &api_interface_version;
        g_api.store_handle = &api_store_handle;
        g_api.load_handle = &api_load_handle;
        g_api.clear_handles = &api_clear_handles;
    }

    return &g_api;
}

BOOL APIENTRY DllMain(HMODULE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        // Nothing here on purpose. Loading an OpenXR runtime from DllMain would run a vendor's
        // initialisation under the loader lock, and those runtimes start threads.
    }

    return TRUE;
}
