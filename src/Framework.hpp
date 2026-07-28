#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

#include "ipc/CommandServer.hpp"

class Framework;
// The one global framework instance (ue4poc convention). Set exactly once by
// the supervisor at injection; NEVER reset (module unmaps whole instead).
extern std::unique_ptr<Framework> g_framework;

// Top-level ownership object (the uevr/reFramework g_framework convention):
// constructed on the supervisor thread at injection, assigned to the
// g_framework global. Owns SDK warm-up, the frame hook, the Mods fan-out, and
// the IPC command server.
//
// The singleton is deliberately NEVER reset in-process: graceful uninject
// leaves it allocated and FreeLibraryAndExitThread unmaps the image whole.
// (Its members own nothing that needs destruction after Framework::shutdown.)
class Framework {
public:
    Framework(void* self_module, int32_t ipc_port);

    // Resolve modules/SDK, install the frame hook, init mods, start IPC.
    bool initialize();

    // Graceful teardown (order is load-bearing; see .cpp):
    //   1. latch shutting-down
    //   2. cmdsrv::stop()      -- no handler in-flight after the join
    //   3. Mods on_shutdown()  -- mods yield the frame path
    //   4. Hooks::retire()     -- original bytes restored globally
    //   5. quiescence proof    -- no other thread's EIP inside our image
    // Returns true iff the module may be unmapped.
    bool shutdown();

    bool is_shutting_down() const { return m_shutting_down.load(); }
    uint64_t frame_ticks() const { return m_frame_ticks.load(); }

    // Called by the frame detour every CClientShell::Update tick.
    void note_frame_tick() { m_frame_ticks.fetch_add(1, std::memory_order_relaxed); }

    void* self_module() const { return m_self; }
    static Framework* get() { return g_framework.get(); }

private:
    void* m_self;
    int32_t m_ipc_port;
    std::atomic<bool> m_initialized{false};
    std::atomic<bool> m_shutting_down{false};
    std::atomic<uint64_t> m_frame_ticks{0};
    bool m_sdk_ready{false};

    Framework(const Framework&) = delete;
    Framework& operator=(const Framework&) = delete;
};

// The one global framework instance (ue4poc convention). Set exactly once by
// the supervisor at injection; NEVER reset (module unmaps whole instead).
extern std::unique_ptr<Framework> g_framework;
