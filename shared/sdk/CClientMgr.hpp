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
// fear2.genny) is ground truth for the two fields mapped so far
// (client_shell, updating) -- only these two; the ~5200+ byte object is
// otherwise genuinely uninvestigated (see fear2.genny's CClientMgr comment).
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

private:
    char m_data[sizeof(regenny::CClientMgr)];
};

} // namespace sdk
