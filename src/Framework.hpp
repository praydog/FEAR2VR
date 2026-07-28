#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "ipc/CommandServer.hpp"

// Top-level ownership object, constructed on the supervisor thread at injection
// time. Owns: SDK resolution (shared/sdk), the Hooks registry, the in-DLL test
// registry, and the IPC command server. Mirrors REFramework/UEVR's g_framework
// layer, slimmed for the fundamentals phase (no mods/UI/D3D yet -- those land
// as Mod subclasses once the SDK supports them).
class Framework {
public:
    explicit Framework(void* self_module, int32_t ipc_port);

    // Resolve modules/SDK pointers, register tests, start IPC. Idempotent-fail:
    // returns false if anything essential failed (supervisor goes dormant).
    bool initialize();

    // Graceful teardown. Steps (order is load-bearing):
    //   1. latch shutting-down (health keeps reporting, /test returns 503)
    //   2. cmdsrv::stop()      -- joins socket thread; no handler in-flight after
    //   3. hooks().retire()    -- original bytes restored; no new detour entries
    //   4. quiescence proof    -- all other threads suspended, no EIP in module
    //   5. return safe_to_unmap
    // Hook INLINE OBJECTS ARE NEVER DESTROYED HERE (see Hooks.hpp invariants).
    bool shutdown();

    bool is_shutting_down() const { return m_shutting_down.load(); }

    void* self_module() const { return m_self; }
    int32_t ipc_port() const { return m_ipc_port; }

    static Framework* get() { return s_instance; }

private:
    static Framework* s_instance;

    void* m_self;
    int32_t m_ipc_port;
    std::atomic<bool> m_initialized{false};
    std::atomic<bool> m_shutting_down{false};
    bool m_sdk_ready{false};

    Framework(const Framework&) = delete;
    Framework& operator=(const Framework&) = delete;
};
