#include "AgentRuntime.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

#include <windows.h>

#include "Framework.hpp"
#include "Log.hpp"
#include "ipc/CommandServer.hpp"

namespace runtime {

void run_supervisor(void* self_raw, int32_t ipc_port) {
    HMODULE self = static_cast<HMODULE>(self_raw);

    // Resolve our own path once; the log derives from it. Grow the buffer until
    // the path fits -- a truncated path would silently misplace the log.
    std::wstring self_str(MAX_PATH, L'\0');
    for (;;) {
        DWORD n = GetModuleFileNameW(self, self_str.data(), static_cast<DWORD>(self_str.size()));
        if (n == 0 || (n == self_str.size() && GetLastError() == ERROR_INSUFFICIENT_BUFFER)) {
            if (n == 0 || self_str.size() > 64 * 1024) {
                self_str = L"fear2vr"; // give up: log still lands somewhere sane
                break;
            }
            self_str.resize(self_str.size() * 2);
            continue;
        }
        self_str.resize(n);
        break;
    }
    const std::filesystem::path self_path{self_str};

    std::string log_path = self_path.string();
    {
        const size_t dot = log_path.find_last_of('.');
        if (dot != std::string::npos && log_path.find_last_of("\\/") < dot) {
            log_path.erase(dot);
        }
        log_path += ".log";
    }
    logger::init(log_path.c_str());
    LOGX("[main] supervisor thread start (module %s)", self_path.string().c_str());

    // The framework object is leaked on all paths: never destruct it in-process
    // (its teardown already ran; destructing later would only risk touching
    // unmapped dependencies).
    auto* framework = new Framework(self, ipc_port);
    bool ipc_up = framework->initialize();

    if (!ipc_up) {
        // Without IPC no /unload can ever arrive; leave the module resident but
        // inert (hooks were never installed -- initialize() installs nothing
        // before starting IPC).
        LOGX("[main] framework init failed; going dormant (module stays mapped)");
        ExitThread(0);
    }

    // Poll for a hot-reload/unload request from the IPC channel.
    while (!cmdsrv::unload_requested()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    LOGX("[main] unload requested; retiring");
    const bool safe_to_unmap = framework->shutdown();

    if (safe_to_unmap) {
        LOGX("[main] unmapping (clean unload)");
        // Never returns; the thread exits as the image unmaps.
        FreeLibraryAndExitThread(self, 0);
    }

    // Dormant fallback: a straggler could not be proven clear. The module stays
    // mapped; the injector loads the next build under a fresh filename.
    LOGX("[main] dormant: module stays mapped; load next build under a new name");
    ExitThread(0);
}

} // namespace runtime
