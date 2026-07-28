#pragma once

#include <cstdint>

namespace sdk {

// Engine-side CClientShell (CNetHandler subclass in the LithTech sources).
// Reached through CClientMgr::client_shell() (m_pClientShellDE at +0x1434).
//
// No regenny::CClientShell struct yet -- CClientShell::Update (dump
// 0x40CC5E) touches at least a byte field @+0x69, a uint16_t[4] handle
// table @+0x60 (0xFFFF-sentineled, resolved through what looks like a
// generic engine handle-manager indirection), and the resolved uint32_t[4]
// @+0x6C, but none of these were pinned down to a confident semantic this
// pass (see reversing/MAPPING_WORKFLOW.md: don't force a struct/field name
// without confident evidence). Deliberately out of scope, not forgotten --
// next slice if this class needs more than update_fn()/get().
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
