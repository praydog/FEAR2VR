#pragma once

#include <cstdint>

namespace sdk {

// Engine-side CClientShell (CNetHandler subclass in the LithTech sources).
// Reached through CClientMgr::client_shell() (m_pClientShellDE at +0x1434).
class CClientShell {
public:
    // Live instance via the client manager. nullptr when no shell exists
    // (before engine init / after world teardown).
    static CClientShell* get();

    // Runtime address of CClientShell::Update (dump 0x40CC5E) -- called every
    // frame by CClientMgr::Update once a shell exists; our frame hook anchor.
    static uintptr_t update_fn();
};

} // namespace sdk
