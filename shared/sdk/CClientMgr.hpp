#pragma once

#include <cstddef>
#include <cstdint>

// Primitives shim MUST precede any generated header using strptr/wstrptr.
#include "regenny/Primitives.hpp"
#include "regenny/regenny/CClientMgr.hpp"

namespace sdk {

class CClientShell;

// Engine-side client manager. Lives in FEAR2.exe; created during WinMain
// (dump 0x466F8D). The global instance pointer g_pClientMgr lives at
// FEAR2_dump.exe 0x6ECCA0 (data), referenced from e.g. CClientShell::Update's
// prologue (`mov ecx,[g_pClientMgr]`).
//
// SDK CLASS CONVENTION (AGENT.MD 5a): regenny::CClientMgr (reversing/
// fear2.genny) is ground truth for the fields mapped so far (client_shell,
// updating, counter_list_head/own_counter_node, start_shell_list, plus
// two honestly-unnamed unk_* fields) -- the ~5200+ byte object is
// otherwise genuinely uninvestigated (see fear2.genny's CClientMgr comment
// for the full evidence trail, including two functions renamed from this
// pass: CClientMgr_Init at dump 0x40AEC6, CClientMgr_StartShell at dump
// 0x40A90A).
class CClientMgr {
public:
    // Live instance (*g_pClientMgr). nullptr before the engine has initialized.
    static CClientMgr* get();

    // Runtime address of CClientMgr::Update (dump 0x40B665) -- the per-frame
    // engine tick driving the shell update, render, and the FPS limiter.
    static uintptr_t update_fn();

    // Address of the g_pClientMgr slot itself (diagnostics/tests).
    static uintptr_t instance_slot();

    regenny::CClientMgr* regenny() const {
        return (regenny::CClientMgr*)this;
    }

    // m_pClientShellDE (regenny()->client_shell, void* in the .genny schema
    // -- see that field's comment for why it isn't typed CClientShell*
    // there). Typed CClientShell* at the SDK level, where it's safe: the SDK
    // class only ever reinterpret_casts a raw engine pointer, never
    // dereferences CClientShell fields directly (CClientShell has no mapped
    // fields yet). SEH-guarded: `this` can go stale between get() and this
    // call (level unload, engine teardown).
    CClientShell* client_shell() const;

    // regenny()->updating: true only for the actual duration of this
    // object's CClientShell::Update call. A diagnostic/test sampling this
    // out-of-band (e.g. over HTTP) will observe false essentially always --
    // that is expected, not a mapping failure (see fear2.genny's comment).
    bool is_updating() const;

    // regenny()->own_counter_node->elapsed_ms / ->elapsed_time: CONFIRMED
    // CORRELATED live (see fear2.genny's CClientMgrCounterNode comment --
    // elapsed_ms == elapsed_time*1000 to the digit, sampled live). NOT
    // confirmed as a free-running "uptime" counter -- re-sampled minutes
    // apart at the main menu and the value did not advance; semantics of
    // WHEN it updates are unknown. Anchored on the counter node CClientMgr
    // registers for itself at CClientMgr_Init. SEH-guarded: own_counter_node
    // is a pointer that can be null before Init completes.
    uint32_t counter_elapsed_ms() const;
    double counter_elapsed_time() const;

    // Bounded walk of regenny()->start_shell_list (a generic engine
    // intrusive circular list -- see fear2.genny's CClientMgrListLink
    // comment). Node PAYLOAD is not mapped (unknown fields beyond the 8-byte
    // link portion) so this only counts entries -- proves the list traversal
    // itself is safe and live, same "does our mapping crash the game" bar as
    // DatabaseMgr's array walks. Bounded at 10000 to fail closed on a
    // corrupted/non-terminating list rather than hang.
    size_t start_shell_list_count() const;

private:
    char m_data[sizeof(regenny::CClientMgr)];
};

} // namespace sdk
