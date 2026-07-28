#include <cstdint>

#include <windows.h>

#include "AgentRuntime.hpp"

// Thin entrypoint for fear2vr.dll. ALL runtime lifecycle (framework init, hook
// teardown, quiescence, unmap) lives in fear2-core's runtime::run_supervisor --
// this DLL is just DllMain + module wiring.
namespace {
constexpr int32_t kIpcPort = 8798;

HMODULE g_self = nullptr;

DWORD WINAPI supervisor_thread(LPVOID) {
    runtime::run_supervisor(g_self, kIpcPort); // does not return (ends its own thread)
    return 0;
}
} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = module;
        DisableThreadLibraryCalls(module);
        CreateThread(nullptr, 0, supervisor_thread, nullptr, 0, nullptr);
    }
    return TRUE;
}
