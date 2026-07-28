#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

// Primitives shim MUST precede any generated header using strptr/wstrptr.
#include "regenny/Primitives.hpp"
#include "regenny/regenny/CClientMgr.hpp"
#include "regenny/regenny/LTObject.hpp"

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

public:
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
    // CORRELATED live -- elapsed_ms == elapsed_time*1000 to the digit,
    // sampled from the same node in one pass. Its ADVANCEMENT semantics are
    // unverified: the value is known to change between observations, but
    // not at what rate, against what clock, or from what zero point. Do NOT
    // treat it as an uptime. See fear2.genny's CClientMgrCounterNode
    // comment for the full evidence and for a retracted earlier claim.
    // Anchored on the counter node CClientMgr registers for itself at
    // CClientMgr_Init. SEH-guarded: own_counter_node is a pointer that can
    // be null before Init completes.
    uint32_t counter_elapsed_ms() const;
    double counter_elapsed_time() const;

    // Bounded walk of regenny()->start_shell_list (a generic engine
    // intrusive circular list -- see fear2.genny's CClientMgrListLink
    // comment). Node PAYLOAD is not mapped (unknown fields beyond the 8-byte
    // link portion) so this only counts entries -- proves the list traversal
    // itself is safe and live, same "does our mapping crash the game" bar as
    // DatabaseMgr's array walks.
    //
    // nullopt when the walk did NOT terminate (hit an internal fail-closed
    // cap, i.e. a corrupt list or a wrong mapping) or faulted. The cap value
    // is deliberately NOT part of this API: callers (diagnostics, tests) must
    // never restate it -- that would put a magic value outside the SDK, which
    // is exactly what AGENT.MD 5a's testing corollary forbids. Ask "did it
    // terminate?" (has_value), never "is the count below <literal>?".
    std::optional<size_t> start_shell_list_count() const;

    // The CClientMgr_Init wiring invariant, expressed entirely through the
    // schema (zero literal offsets): the node this manager registered for
    // itself must be linked into its own counter list, i.e.
    //   &own_counter_node->self_link == counter_list_head.next
    // CClientMgr_Init (dump 0x40AEC6) is what establishes this
    // (`inserted = EngineList_InsertNode(this+counter_list_head);
    //   this->own_counter_node = inserted;`).
    //
    // This exists as an SDK METHOD, not a test-side computation, so the
    // invariant is checked against the generated schema's own offsetof --
    // if fear2.genny's layout ever drifts, this recomputes correctly
    // instead of silently comparing stale hardcoded numbers. False when
    // own_counter_node is null (pre-Init) or the link doesn't match.
    bool counter_node_registered() const;

    // ---- object enumeration -------------------------------------------
    //
    // regenny()->object_lists is 7 intrusive circular list heads, and every
    // object in bucket N has LTObject.type == N (PROVEN per-object: 0
    // mismatches across 3490 live objects in a single traversal; each
    // bucket also holds exactly one distinct vtable, none shared). See
    // fear2.genny's CClientMgr.object_lists comment for the full evidence.
    //
    // This is the first SDK surface reaching actual game entities and their
    // transforms (LTObject.position / .rotation, the latter validated
    // unit-length across every sampled object). NOTE: what those transforms
    // are RELATIVE TO (world origin? a parent?) is NOT established -- see
    // fear2.genny's LTObject comment. Don't assume world space.
    //
    // DELIBERATELY SNAPSHOT-BASED, NOT POINTER-RETURNING. These lists mutate
    // while we read them -- consecutive walks genuinely return different
    // counts because the game creates/destroys objects continuously. Handing
    // a caller an LTObject* would be a use-after-free waiting to happen: the
    // node can be unlinked and freed between us returning it and the caller
    // dereferencing it, and SEH only catches a hard fault, NOT stale-but-
    // still-readable memory. So the walk and the field reads happen together
    // in ONE guarded pass, and callers only ever receive copied values.

    // Bucket count, straight from the schema -- never a literal.
    static constexpr size_t object_list_count() {
        return sizeof(regenny::CClientMgr::object_lists) / sizeof(regenny::CClientMgrListLink);
    }

    // Copied-out view of one object. Plain POD: safe to hold after the walk.
    // `address` is for diagnostics/correlation only -- it is NOT valid to
    // dereference later, for exactly the reason above.
    struct ObjectSnapshot {
        uint32_t address;
        uint32_t vtable;
        uint8_t type;
        uint16_t handle;
        float position[3];
        float rotation[4];
    };

    // Objects in one type bucket. nullopt when `type` is out of range, the
    // walk faulted, or it did not terminate within an internal fail-closed
    // cap. The cap deliberately does NOT leak to callers -- ask has_value(),
    // never "is it under <literal>".
    std::optional<size_t> object_count(size_t type) const;

    // Walks bucket `type` and copies up to `max` objects into `out` in a
    // single SEH-guarded pass. Returns how many were written, or nullopt on
    // the same failure conditions as object_count(). Writing fewer than the
    // bucket's count is normal (bounded by `max`).
    std::optional<size_t> snapshot_objects(size_t type, ObjectSnapshot* out, size_t max) const;

private:
    char m_data[sizeof(regenny::CClientMgr)];
};

} // namespace sdk
