#include "Modules.hpp"

#include <cstring>

namespace sdk {
namespace {

Module g_modules[] = {
    // Order matters only for reporting; names are GetModuleHandleA basenames.
    {"FEAR2.exe", nullptr, 0, 0, true},
    {"gameclient.dll", nullptr, 0, 0, true},
    {"gameserver.dll", nullptr, 0, 0, false}, // loaded on demand (session start)
    {"gamedatabase.dll", nullptr, 0, 0, true},
    {"ltmemory.dll", nullptr, 0, 0, true},
};

bool g_initialized = false;

// Read the PE header chain; defensive against torn/corrupt headers.
size_t image_size_of(HMODULE module) {
    if (module == nullptr) {
        return 0;
    }
    __try {
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
            return 0;
        }
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(
            reinterpret_cast<const uint8_t*>(module) + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) {
            return 0;
        }
        return nt->OptionalHeader.SizeOfImage;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

} // namespace

size_t module_size(HMODULE module) {
    return image_size_of(module);
}

bool initialize() {
    bool all_required = true;
    for (auto& m : g_modules) {
        // GetModuleHandleA: modules are already loaded inside FEAR2.exe's
        // process (we are injected into it); this never loads anything.
        m.handle = GetModuleHandleA(m.name);
        if (m.handle == nullptr) {
            if (m.required) {
                all_required = false;
            }
            continue;
        }
        m.base = reinterpret_cast<uintptr_t>(m.handle);
        m.size = image_size_of(m.handle);
        if (m.size == 0 && m.required) {
            all_required = false;
        }
    }
    g_initialized = all_required;
    return all_required;
}

bool is_initialized() {
    return g_initialized;
}

Module* exe() { return &g_modules[0]; }
Module* game_client() { return &g_modules[1]; }
Module* game_server() { return &g_modules[2]; }
Module* game_database() { return &g_modules[3]; }
Module* lt_memory() { return &g_modules[4]; }

} // namespace sdk
