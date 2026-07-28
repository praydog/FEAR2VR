#pragma once

#include <cstdint>

namespace sdk {

class CClientShell;

// Engine-side client manager. Lives in FEAR2.exe; created during WinMain
// (dump 0x466F8D). The global instance pointer g_pClientMgr lives at
// FEAR2_dump.exe 0x6ECCA0 (data), referenced from e.g. CClientShell::Update's
// prologue (`mov ecx,[g_pClientMgr]`).
class CClientMgr {
public:
    // Live instance (*g_pClientMgr). nullptr before the engine has initialized.
    static CClientMgr* get();

    // Runtime address of CClientMgr::Update (dump 0x40B665) -- the per-frame
    // engine tick driving the shell update, render, and the FPS limiter.
    static uintptr_t update_fn();

    // Address of the g_pClientMgr slot itself (diagnostics/tests).
    static uintptr_t instance_slot();

    // m_pClientShellDE. Evidence: CClientMgr::Update (dump 0x40B665) reads the
    // shell pointer from *(this + 0x1434) and calls CClientShell::Update on it.
    static constexpr uint32_t k_client_shell_offset = 0x1434;
    CClientShell* client_shell();
};

} // namespace sdk
