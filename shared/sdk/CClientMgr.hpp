#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>

// Primitives shim MUST precede any generated header using strptr/wstrptr.
#include "regenny/Primitives.hpp"
#include "regenny/regenny/CClientMgr.hpp"
#include "regenny/regenny/LTObject.hpp"

namespace sdk {

class CClientShell;

// The object-type enum is NOT redeclared here -- it is the generated
// schema's own type, aliased. A hand-written copy in this namespace could
// silently drift from fear2.genny; this cannot.
//
// The value space is CONFIRMED: CClientMgr keeps exactly 7 type-bucketed
// object lists, every live object's `type` equals its bucket index (0
// mismatches / 3490 objects, single traversal), and each bucket holds
// exactly one distinct vtable with none shared between buckets. The NAMES
// are LithTech-SDK analogues (FEAR2 ships without RTTI, so they are not
// recovered from the binary) -- see fear2.genny's LTObjectType comment.
using ObjectType = regenny::LTObjectType;

// "OT_MODEL" etc. for diagnostics/logging. Never nullptr; returns
// "OT_INVALID" for a value outside the confirmed 0..6 range.
const char* object_type_name(ObjectType type);

// Engine-side client manager. Lives in FEAR2.exe; created during WinMain
// (dump 0x466F8D). The global instance pointer g_pClientMgr lives at
// FEAR2_dump.exe 0x6ECCA0 (data), referenced from e.g. CClientShell::Update's
// prologue (`mov ecx,[g_pClientMgr]`).
//
// SDK CLASS CONVENTION (AGENT.MD 5a): regenny::CClientMgr (reversing/
// fear2.genny) is ground truth. What is mapped, and what that buys you:
//
//   object_lists[7]        7 intrusive circular lists of LTObject, bucketed
//                          by type. THE useful surface -- every client-side
//                          entity with a position and rotation is reachable
//                          from here. See the object-enumeration section.
//   client_shell           the CClientShell this manager drives.
//   updating               re-entrancy guard around CClientShell::Update.
//   pending_shell_release  what that guard protects: a shell released during
//                          its own Update is parked here and destroyed the
//                          instant Update clears the guard.
//   counter_list_head      an engine list this manager registers itself in,
//   own_counter_node       via that node; iterated per-frame by
//                          EngineList_ForEach. Carries elapsed_ms/
//                          elapsed_time (correlated pair) and the fields
//                          CClientMgr::Update's FPS limiter reads.
//   start_shell_list       a second, distinct engine list; node payload
//                          unmapped, so only its traversal is exercised.
//   last_sample_time_ms    the ms timestamp Update differences to get its
//                          frame delta (unit confirmed via the /1000.0).
//   last_sent_bandwidth    change-detection cache of BandwidthTargetClient/8000.
//   unk_1424, unk_4E4      honestly unnamed: offset and type evidenced, role
//                          not. unk_1424 in particular had a wrong name
//                          ("frame_counter") retracted after live sampling
//                          contradicted it -- see fear2.genny.
//
// Beyond those the ~5200-byte object is still uninvestigated, including the
// whole span from 0x38 to 0x68 and the blob at 0x90. fear2.genny's CClientMgr
// comment carries the full evidence trail per field, and the IDA-side names
// established so far (CClientMgr_Init 0x40AEC6, CClientMgr_StartShell
// 0x40A90A, CClientMgr::Update 0x40B665).
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

    // regenny()->last_sample_time_ms: the millisecond timestamp Update
    // differences against to derive its frame delta. The UNIT is confirmed
    // (the engine divides that delta by exactly 1000.0 to get seconds) and it
    // tracks wall clock 1:1 live; the zero point is not established, and
    // neither is WHAT writes it -- Update only reads it, so do not treat its
    // advancement as proof that frames specifically are running (some other
    // timer path could be the writer). unk_1424 is deliberately NOT exposed
    // at all: it also advances, but what it measures is unknown and one wrong
    // name has already been retracted from it.
    uint32_t last_sample_time_ms() const;

    // regenny()->pending_shell_release: non-null ONLY inside the window where
    // a shell release was requested while `updating` was set and Update has
    // not yet performed the deferred destruction. Sampling this off-thread
    // will essentially always see null -- like is_updating(), that is
    // expected, not a mapping failure. Exposed because it is what makes the
    // `updating` flag's purpose legible: it is a re-entrancy guard, and this
    // is the parking slot it guards (see fear2.genny's CClientMgr comment).
    bool has_pending_shell_release() const;

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
    // TWO WAYS TO READ THE OBJECTS, with different lifetime rules. Pick
    // deliberately -- the difference is the whole reason both exist.
    //
    //   for_each_object()  -- in place, no copying. Your callback receives a
    //                         live `const regenny::LTObject*`. Use this on
    //                         the engine thread (a hook body) where copying
    //                         every object per frame would be absurd.
    //   snapshot_objects() -- copies POD fields out. Use this when the
    //                         values must outlive the walk, or when the
    //                         caller is NOT the thread that mutates these
    //                         lists (diagnostics, IPC, tests).
    //
    // LIFETIME (read this before using for_each_object): the pointer handed
    // to your callback is valid ONLY for the duration of that one call, and
    // ONLY if these lists are not being mutated concurrently. The SDK does
    // NOT establish that for you -- it is a CALLER PRECONDITION. What is
    // established is the opposite: the lists demonstrably churn (consecutive
    // walks return different counts because the game creates and destroys
    // objects), and a node can be unlinked and freed at any point a mutator
    // runs. Whether calling from an engine-thread hook excludes that
    // mutation is NOT something this SDK has verified. So: never store the
    // pointer, never let it escape the callback, and if you cannot argue
    // that nothing is mutating the list, use snapshot_objects() instead.
    //
    // Note also that SEH covers only the SDK's own traversal steps. Field
    // dereferences inside your callback body are outside any guard, and SEH
    // would not catch stale-but-still-readable memory anyway.

    // Bucket count, straight from the schema -- never a literal.
    static constexpr size_t object_list_count() {
        return sizeof(regenny::CClientMgr::object_lists) / sizeof(regenny::CClientMgrListLink);
    }

    // In-place iteration. Returns the number of objects visited, or nullopt
    // if the traversal did not complete cleanly -- it faulted, exceeded an
    // internal fail-closed bound, or reached an object whose type disagreed
    // with its bucket (which would mean the mapping drifted, not that the
    // game misbehaved). That bound is deliberately not part of this API:
    // ask has_value(), never "is the count below <literal>".
    //
    // The callback runs OUTSIDE the SEH guard, deliberately: a user callable
    // cannot legally live inside `__try` (MSVC C2712 rejects non-POD locals
    // sharing the scope), and guarding it would misreport a bug in caller
    // code as a fault in our mapping. Only the link-to-link steps are
    // guarded.
    //
    // Return `bool` from the callback to stop early (false == stop); a
    // void-returning callback always visits the whole bucket.
    template <typename F>
    std::optional<size_t> for_each_object(ObjectType type, F&& fn) const {
        size_t visited = 0;
        for (ObjectStep s = first_object(type); ; s = next_object(type, s)) {
            if (!s.ok) {
                return std::nullopt;
            }
            if (s.object == nullptr) {
                return visited;
            }
            ++visited;
            if constexpr (std::is_invocable_r_v<bool, F&, const regenny::LTObject*>) {
                if (!fn(s.object)) {
                    return visited;
                }
            } else {
                fn(s.object);
            }
        }
    }

    // ---- object allocators (per-type pool banks) -----------------------
    //
    // regenny()->object_banks is six {LTMemoryPool*, element_size} pairs, one
    // per object type, filling what used to be the largest unmapped span in
    // this class. See fear2.genny's CClientMgr.object_banks comment for the
    // three-step proof of the mapping.
    //
    // THE INDEX IS NOT THE TYPE. OT_LIGHT has no bank, so the array is
    // compacted around it and callers must go through bank_for(). Indexing by
    // raw ObjectType would silently read OT_CAMERA's bank for OT_LIGHT.

    // Number of banks, straight from the schema -- never a literal.
    static constexpr size_t object_bank_count() {
        return sizeof(regenny::CClientMgr::object_banks) /
               sizeof(regenny::CClientMgrObjectBank);
    }

    struct ObjectBankInfo {
        ObjectType type;        // the object type this bank allocates
        uintptr_t pool;         // the LTMemory pool (diagnostics; not dereferenced here)
        uint32_t element_size;  // allocation size the engine requests for this type
        uint32_t block_size;    // pool's slot stride == (element_size + 8) & ~7
    };

    // Bank serving `type`, or nullopt when that type has no bank (OT_LIGHT) or
    // the read faulted. SEH-guarded: reaches through the pool pointer.
    std::optional<ObjectBankInfo> bank_for(ObjectType type) const;

    // Bank at raw array index, or nullopt when out of range / faulted. For
    // diagnostics that want to enumerate the array as it actually is.
    std::optional<ObjectBankInfo> bank_at(size_t index) const;

    // ---- type-5 (OT_CAMERA analogue) cached transforms -----------------
    //
    // Type-5 objects are 320 bytes: an LTObject base plus a cached 3x4 world
    // transform and its exact rigid inverse (a view-matrix pair). See
    // fear2.genny's LTCameraObject for the evidence, and note the NAME is a
    // reference-SDK analogue, not proven.
    //
    // This is a MAPPING SELF-CHECK, not a feature. It recomputes the two
    // relationships the mapping asserts and reports whether they hold, so the
    // fixture can catch schema drift without re-deriving offsets host-side:
    //   * the transform's 3x3 equals the rotation matrix of the object's own
    //     quaternion, and
    //   * the second block is the rigid inverse of the first.
    // Snapshot-based for the same reason as snapshot_objects(): the lists
    // mutate, so nothing here hands out a pointer.
    struct TransformCheck {
        size_t sampled;        // objects examined
        size_t rotation_match; // 3x3 == R(quaternion)
        size_t inverse_ok;     // block2 == rigid inverse of block1
        size_t det_ok;         // det(3x3) == 1
    };

    // Walks up to `max` type-5 objects, checking both relationships. nullopt
    // when the walk faulted or did not terminate.
    std::optional<TransformCheck> check_type5_transforms(size_t max) const;

    // Copied-out view of one object. Plain POD: safe to hold after the walk.
    //
    // `address`/`vtable` are uintptr_t, not a pointer type: they are
    // deliberately NOT dereferenceable. By the time you read this struct the
    // object may be gone, so the SDK hands back an integer you can print and
    // correlate but cannot accidentally follow. (Anything genuinely
    // integral, like `handle`, stays an integer type for the opposite
    // reason -- it was never an address.)
    struct ObjectSnapshot {
        uintptr_t address;
        uintptr_t vtable;
        ObjectType type;
        uint16_t handle;
        float position[3];
        float rotation[4];
    };

    // Objects in one type bucket. nullopt when `type` is out of range, the
    // walk faulted, it did not terminate within an internal fail-closed
    // bound, or an object's type disagreed with its bucket. All three read
    // paths (this, for_each_object, snapshot_objects) validate identically,
    // so a count you get here is one the other two would also accept.
    std::optional<size_t> object_count(ObjectType type) const;

    // Walks bucket `type` and copies up to `max` objects into `out` in a
    // single SEH-guarded pass. Returns how many were written, or nullopt on
    // the same failure conditions as object_count(). Writing fewer than the
    // bucket's count is normal (bounded by `max`).
    std::optional<size_t> snapshot_objects(ObjectType type, ObjectSnapshot* out, size_t max) const;

private:
    // Fail-closed traversal bound, shared by every object walk in this class
    // (iteration, counting, snapshotting) so the value exists exactly once.
    // A corrupt list can cycle without ever returning to its head; guarded
    // reads alone would spin forever, so every walk also stops here and
    // reports failure rather than a plausible-looking partial answer.
    //
    // PRIVATE ON PURPOSE (AGENT.MD 5a): if callers could read this, a
    // diagnostic or test would eventually restate it, putting a magic bound
    // outside the SDK that silently disagrees when this changes.
    static constexpr size_t max_object_walk = 100000;

    // One traversal step. POD by design so the SEH-guarded implementation
    // stays C2712-legal. `object == nullptr && ok` means a clean end of
    // list; `!ok` means the step faulted, exceeded max_object_walk, or hit
    // an object whose type disagreed with its bucket.
    struct ObjectStep {
        const regenny::LTObject* object;
        size_t index;
        bool ok;
    };

    // Guarded single steps backing for_each_object(). Private: the only
    // public way to reach a live LTObject* is through the callback, whose
    // scope documents the lifetime.
    //
    // step_from() is the shared body: advance from `cur` (the bucket head on
    // the first call, the current object's own link afterwards), validate
    // what we land on, and carry the running index so the walk bound is
    // enforced across the whole iteration rather than per-step.
    ObjectStep step_from(ObjectType type,
                         const regenny::CClientMgrListLink* cur,
                         size_t index) const;
    ObjectStep first_object(ObjectType type) const;
    ObjectStep next_object(ObjectType type, ObjectStep cur) const;

    char m_data[sizeof(regenny::CClientMgr)];
};

} // namespace sdk
