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
// SDK CLASS CONVENTION (AGENTS.md 5a): regenny::CClientMgr (reversing/
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
    // is exactly what AGENTS.md 5a's testing corollary forbids. Ask "did it
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

    // ---- schema size agreement (the engine's own allocation sizes) -------
    //
    // Each bank records the element_size the engine asks its pool for, i.e. the
    // concrete class's size as the ENGINE understands it. Our schema declares a
    // size for each of those classes independently. Comparing the two is a
    // self-check with no host-recorded baseline on either side: one number comes
    // from live memory, the other from the generated headers.
    //
    // What it does and does not catch is worth being precise about. It pins each
    // class's TOTAL size, so a field added past the end, a wrong array length or
    // a mis-sized member shows up immediately. It does NOT catch a field
    // attributed to the wrong class within a correct total -- the transform pair
    // sat on LTCameraObject for several passes while both sizes were right. Only
    // the destructor chain caught that.
    struct SchemaSizeCheck {
        size_t types_checked;   // types the engine allocates from a bank
        size_t size_matches;    // element_size == sizeof(our class for that type)
        bool light_has_no_bank; // OT_LIGHT is uncreatable, so it must have none
    };

    // nullopt only when the manager is unreachable; a MISMATCH is reported in the
    // counts rather than hidden as an error, so the caller can name which type.
    std::optional<SchemaSizeCheck> check_schema_sizes() const;

    // ---- OT_MODEL's embedded list (type 1) -------------------------------
    //
    // Every model owns a list whose head is at LTModelObject::list_head with its
    // own count beside it, and the constructor links the object's own embedded
    // record into it before anything else can. So these must hold for every live
    // model, none of them recorded from a previous run:
    //   * the stored list_count equals the number of members actually walked,
    //   * the object's own record is one of those members,
    //   * asset_dup equals record.asset -- two independent routes to one pointer,
    //   * EVERY member's asset equals the owner's. That last one is what makes
    //     "the members are LTModelRecords" a claim with evidence rather than an
    //     assumption about a bare link address: a wrong record layout would put
    //     something other than the owner's asset at member+0x20.
    // The cached_rotation being unit-length is checked too: it is what proves
    // those 16 bytes are a quaternion rather than trailing padding.
    struct ModelListCheck {
        size_t sampled;            // models examined
        size_t count_matches_walk; // list_count == walked members
        size_t embedded_linked;    // the object's own record is a member
        size_t asset_dup_agrees;   // asset_dup == record.asset
        size_t asset_present;      // record.asset is non-null (a mandatory resource)
        size_t rotation_unit;      // cached_rotation is unit length
        size_t max_members;        // largest list seen (one embedded + extras)
        size_t members_total;      // members walked across every model
        size_t member_asset_ok;    // members whose asset equals their owner's
    };

    // ---- OT_MODEL material names (an owned std::string array) ------------
    //
    // LTModelObject::material_names is an array of MSVC std::string, length in
    // material_count. Both facts come from the engine's own teardown:
    // 0x429771(base, count) runs `~string(p); p += 28` that many times, so the
    // base, the length AND the 28-byte stride are the engine's arithmetic rather
    // than an assumption about which STL this build used.
    //
    // Live these hold MATERIAL PATHS -- "weapons\_global\shellcasings\...\
    // assault_rifle_casing1.mat" -- which makes this the first field in the
    // mapping that states in plain text what an object IS. Worth having for
    // recon: identifying an object by path beats inferring it from geometry.
    //
    // Every check here is a std::string against ITSELF, which is the strongest
    // form available (see TESTING.MD): the byte at [size] must be the
    // terminator, size must not exceed capacity, and capacity must be at least
    // the small-buffer size. No baseline, no sibling structure. A wrong stride
    // or a wrong field offset breaks all three at once.
    struct MaterialCheck {
        size_t models;             // models with a non-null array and a sane count
        size_t strings_total;      // strings visited
        size_t terminated;         // byte at [size] is NUL
        size_t size_le_capacity;   // size <= capacity
        size_t capacity_sane;      // capacity >= 15 (the small-buffer minimum)
        size_t nonempty_printable; // non-empty and printable ASCII throughout
        size_t max_count;          // largest material_count seen
    };

    // Walks up to `max` type-1 objects. nullopt on fault or a walk that did not
    // terminate. Reads string bodies, so it reaches through two pointer levels --
    // SEH-guarded, and every length is bounded before use.
    std::optional<MaterialCheck> check_model_materials(size_t max) const;

    // ---- LTModelAsset (the shared per-.mdl resource) ---------------------
    //
    // 34 distinct assets serve 215 models live. The class is 0xA0 because
    // LTModelAsset_FindOrLoad allocates exactly that (`LTMem_Alloc(0xA0)`) and the
    // ctor's last write ends there -- NOT because of the 0xC8 minimum spacing
    // between live assets, which is a general-heap artefact and only an upper
    // bound. That mistake is recorded in fear2.genny so it is not re-made.
    //
    // Every check below is the asset against ITSELF or against the models that
    // point at it, never against a recorded value:
    //   * self_ref holds the asset's own address,
    //   * radius_dup equals radius, and filename_dup equals filename,
    //   * filename decodes as printable NUL-terminated ASCII,
    //   * refcount is at least the number of models referencing it.
    // The last one is deliberately an INEQUALITY. refcount == 2*users + 1 fits 27
    // of 34, but six assets fall BELOW that, which kills the "one cache ref plus
    // two per model" reading rather than merely complicating it. The tight
    // relation is reported so the shortfall stays visible.
    struct AssetCheck {
        size_t assets;          // distinct assets reached from live models
        size_t self_ref_ok;     // self_ref == the asset's own address
        size_t radius_dup_ok;   // radius_from_file == radius, and radius > 0
        size_t name_at_blob;    // filename == string_blob (the name is copied to its front)
        size_t name_readable;   // filename is printable and NUL-terminated
        size_t refcount_ge;     // refcount >= models referencing it
        size_t refcount_exact;  // refcount == 2*users + 1 (reported, not asserted)
        // The blob is one allocation the loader carves everything out of, so every
        // pointer it derives has to land inside it. These are containment checks
        // against the asset's OWN recorded size -- no external reference at all.
        size_t blob_size_sane;  // string_blob_size is non-zero and plausible
        size_t arrays_in_blob;  // entry_array_a AND _b lie within the blob
        size_t write_order_ok;  // filename <= entry_array_a <= entry_array_b
        size_t count_matches;   // entry_count == (entry_array_b - entry_array_a) / 4
        size_t count_dup_ok;    // entry_count_dup == entry_count
    };

    // ---- the animation-name lookup table (asset+0x6C) --------------------
    //
    // LTModelAsset::anim_names is a std::vector of {name_hash, value} pairs that
    // LTModelAsset_FindAnimByName BINARY SEARCHES. That makes ascending order a
    // FUNCTIONAL REQUIREMENT, not an incidental property of the data: if it ever
    // stopped holding, the engine's own name lookups would start missing entries
    // silently, and nothing else would complain. It is exactly the kind of
    // invariant worth a test, because the failure mode is quiet.
    //
    // The element size is the engine's own: its size helper is
    // `(last - first) >> 3` and its indexer `first + 8 * i`.
    struct AnimTableCheck {
        size_t assets;          // distinct assets examined
        size_t table_sane;      // first/last present and a plausible count
        size_t hashes_ascending;// entries sorted by hash, as the binary search needs
        size_t entries_total;   // entries across all tables (reported)
        size_t max_entries;     // largest table seen (reported)
    };

    std::optional<AnimTableCheck> check_anim_tables(size_t max) const;

    // ---- skeleton nodes (names + hashes on the shared asset) -------------
    //
    // LTModelAsset::node_names is an array of node_count char* into the asset's
    // string_blob, and node_hashes is a parallel array of case-insensitive name
    // hashes. LTModelAsset_ReadNodeTree writes both in one recursive pass:
    // `names[i] = ptr; hashes[i] = String_HashI(ptr)`.
    //
    // Live these are bone names -- Pelvis, Torso, Neck, Head, L_Shoulder, L_Hand,
    // L_Thumb0, Face_Jaw. Worth having mapped well before any VR work needs to
    // locate a head or hand bone: name lookup is how the engine itself does it.
    //
    // The hash is NOT recomputed here, on purpose. Doing so would mean either
    // hardcoding the 256-byte translation table or reading it from a fixed module
    // offset, and this project resolves engine data by pattern, never by absolute
    // address. Instead the check exploits the ONE property a hash of a name must
    // have: the same name must always produce the same value. Across 34 assets
    // many node names repeat (11 assets carry a "Head"), so this compares the
    // engine's stored hashes against each other with no external input at all.
    struct NodeCheck {
        size_t assets;           // assets with a usable node array
        size_t nodes_total;      // node entries visited
        size_t names_in_blob;    // name pointer lies inside that asset's string_blob
        size_t names_printable;  // name decodes as printable NUL-terminated ASCII
        size_t distinct_names;   // unique names, compared case-insensitively
        size_t repeated_names;   // names that occur on more than one node/asset
        size_t hash_consistent;  // nodes whose hash matches the first seen for that name
        size_t hash_collisions;  // pairs of DIFFERENT names sharing one hash
        size_t count_dup_ok;     // node_count_dup == node_count
        // The array is a TREE in topological order, which is the property that
        // makes a single forward pass enough to accumulate world transforms. Each
        // of these is checked per asset, against the asset's own numbers:
        size_t records_in_blob;  // the whole node array lies inside string_blob
        size_t root_is_255;      // node[0].parent_index == 255 (the reader's sentinel)
        size_t index_self_ok;    // own_index == the array index, on every node
        size_t topological_ok;   // every non-root parent_index < its own index
        size_t child_sum_ok;     // sum(child_count) == node_count - 1: one root, no cycles
        size_t rot_a_unit;       // rotation_a is unit length
        size_t rot_b_unit;       // rotation_b is unit length
        size_t pos_finite;       // both position vectors are finite and in range
        // CONTIGUOUS CHILDREN. LTModelObject_GetNodeChildIndex resolves a child as
        // `nodes[i + nodes[i].first_child_offset + ordinal]`, so a node's children
        // are a solid block. That makes first_child_offset and child_count
        // cross-checkable against parent_index -- three fields that must agree, none
        // of them recorded from a previous run:
        size_t child_block_in_range;  // [i+off, i+off+count) stays inside the array
        size_t child_parents_ok;      // every node in that block names i as parent
        size_t child_links_seen;       // child links actually examined (non-vacuity)
    };

    // Walks up to `max` type-1 objects, visiting each distinct asset's nodes once.
    // nullopt on fault or a walk that did not terminate.
    std::optional<NodeCheck> check_model_nodes(size_t max) const;

    // Walks up to `max` type-1 objects, collecting distinct assets. nullopt on
    // fault or a walk that did not terminate.
    std::optional<AssetCheck> check_model_assets(size_t max) const;

    // Walks up to `max` type-1 objects. nullopt on fault or a walk that did not
    // terminate.
    std::optional<ModelListCheck> check_model_lists(size_t max) const;

    // ---- cached transforms (WorldModel state, inherited by Camera) -------
    //
    // The cached 3x4 world transform and its rigid inverse belong to the
    // WORLDMODEL class (type 2, 0x13C) and are INHERITED by Camera (type 5,
    // which adds only a uint16). The hierarchy is proven by the destructor
    // chain -- see fear2.genny's LTWorldModelObject. An earlier version of this
    // check only walked type 5, which is why the mis-attribution went unnoticed:
    // it never looked at the 1473 objects that carry the same fields.
    //
    // A MAPPING SELF-CHECK, not a feature. It recomputes what the mapping claims
    // and reports whether it holds:
    //   * det(3x3) == 1, i.e. the matrix is a proper rotation. HARD invariant on
    //     both types.
    //   * the 3x3 equals R(the object's own quaternion), and the second block is
    //     the rigid inverse of the first. Both hold on 474/474 cameras but only
    //     1464/1473 and 1450/1473 worldmodels, because a worldmodel is moving
    //     level geometry whose cached transform can lag its quaternion between
    //     updates. Those two are therefore REPORTED per type, not asserted.
    struct TransformCheck {
        size_t sampled;        // objects examined
        size_t rotation_match; // 3x3 == R(quaternion)
        size_t inverse_ok;     // block2 == rigid inverse of block1
        size_t det_ok;         // det(3x3) == 1
    };

    // `type` must be 2 (WorldModel) or 5 (Camera); anything else is nullopt,
    // because no other type has these fields. nullopt also on fault or a walk
    // that did not terminate.
    std::optional<TransformCheck> check_transforms(size_t type, size_t max) const;

    // ---- bounding geometry (every object type) --------------------------
    //
    // LTObject caches a world AABB, a radius and a half-extents vector, and
    // SetDims (dump 0x420358) derives the first two from the third:
    //     aabb_min = position - dims
    //     aabb_max = position + dims
    //     radius   = |dims| + 0.1
    // Another MAPPING SELF-CHECK, in the same spirit as check_type5_transforms:
    // these are identities the engine's own writer establishes, so they must
    // hold for every live object. If one stops holding, an offset moved.
    //
    // Why this matters beyond bookkeeping: `dims`, `radius` and the AABB are
    // the culling inputs, so a wrong offset here would silently mis-cull
    // geometry rather than crash.
    //
    // The radius is TWO-STATE, and this distinction is load-bearing: the base
    // constructor zeroes both dims and radius, so an object SetDims never ran
    // on sits at (dims == 0, radius == 0) -- NOT at radius 0.1. Over the WHOLE
    // object set 2126 are sized and 1457 pristine, totalling all 3583; a call
    // with `max_per_type` 512 samples ~2215 of them and splits ~1164/1051. The
    // absolute counts therefore depend on the CAP YOU PASS -- an earlier version
    // of this comment quoted the full-set numbers without saying so, which made a
    // capped call look like it had lost objects. What does not depend on the cap,
    // and is the actual invariant, is that the two states PARTITION the sample.
    // The counts are reported separately rather than merged so a caller can
    // assert both branches are actually exercised; collapsing them into one
    // "ok" tally, or widening the tolerance until the pristine objects slipped
    // under it, would turn a real two-state invariant into a vacuous one.
    struct GeometryCheck {
        size_t sampled;         // objects examined across all buckets
        size_t aabb_min_ok;     // aabb_min == position - dims
        size_t aabb_max_ok;     // aabb_max == position + dims
        size_t radius_sized;    // SetDims ran: radius == |dims| + 0.1
        size_t radius_pristine; // never sized: dims == 0 AND radius == 0
        size_t dims_nonneg;     // all three components >= 0
    };

    // Walks up to `max_per_type` objects of EVERY type. nullopt when any walk
    // faulted or failed to terminate. radius_sized + radius_pristine must equal
    // sampled; any shortfall is an object in neither state, i.e. a broken map.
    std::optional<GeometryCheck> check_object_geometry(size_t max_per_type) const;

    // ---- world tree (spatial index) --------------------------------------
    //
    // Objects are bucketed into a quadtree over X/Z for culling; each links in
    // at LTObject.world_tree_link and each node is a regenny::LTWorldTreeNode.
    // See fear2.genny for how the node layout was recovered and verified.
    //
    // A third MAPPING SELF-CHECK. It reaches the tree the way the engine does --
    // from an object, out through its link to the owning node, then up the
    // parent chain -- so it exercises world_tree_link, parent_offset and
    // occupied_count together rather than trusting any one offset.
    //
    // Identifying the node among the list elements needs the exe image range:
    // every element except the node's own head is `some LTObject + 0xC4`, so a
    // candidate is an object exactly when the vtable slot 0xC4 back lies inside
    // FEAR2.exe. That is why this walk depends on Modules being initialized.
    struct WorldTreeCheck {
        size_t objects_seen;     // objects examined
        size_t linked;           // world_tree_link is threaded into a node
        size_t unlinked;         // self-pointing, i.e. not in the tree
        size_t node_found;       // owning node located from the object's list
        size_t root_reached;     // parent chain terminated (parent_offset == 0)
        size_t counts_monotonic; // occupied_count never decreases toward the root
        size_t root_mismatches;  // linked objects reaching a DIFFERENT root than the first
        uintptr_t root;          // the root the first linked object reached
        size_t max_depth;        // deepest parent chain observed
        // CROSS-ROUTE CHECK. The world-tree root is reachable two completely
        // independent ways: climbed from an object's world_tree_link (what this
        // walk does), or read straight out of LTWorldClientBSP.world_tree_root.
        // An earlier pass only had the first. Agreement is evidence for
        // parent_offset, world_tree_link AND that header field at once, and it
        // needs no recorded baseline -- the engine supplies both sides.
        uintptr_t bsp_root;      // LTWorldClientBSP.world_tree_root, 0 if unavailable

        // WHICH FAILURE, when there was one. The walk used to collapse "the image range is unknown", "a
        // read faulted" and "the list did not terminate" into a single nullopt, and the test that consumed
        // it said so out loud: "null == faulted or no exe range". A level where this fires then leaves you
        // guessing between three unrelated causes. It is one bool and one index to say which.
        bool completed;          // false when a list spine faulted
        size_t object_faults;    // objects whose OWN contents faulted and were stepped over
        uintptr_t first_fault;   // the first such object, so it can be looked at rather than guessed about
        bool hit_cap;            // the walk stopped on its iteration cap rather than reaching the head
        size_t faulted_list;     // which object list index stopped it, when !completed
        size_t lists_walked;     // how many completed before that
        bool root_matches_bsp;   // the climbed root equals it
    };

    // Walks up to `max_objects` objects per type. nullopt on fault or a walk
    // that failed to terminate. A healthy tree gives linked + unlinked ==
    // objects_seen, node_found == root_reached == counts_monotonic == linked,
    // and root_mismatches == 0.
    std::optional<WorldTreeCheck> check_world_tree(size_t max_objects) const;

    // ---- per-type cull volumes -------------------------------------------
    //
    // Object vtable slot 2 hands the engine a culling volume and dispatches on
    // its return: 0 = none, 1 = sphere, 2 = AABB. This checks the two typed
    // sources that feed it, which is where the newly mapped offsets live:
    //   * OT_MODEL derives its cull radius as vis_radius * LTObject.scale -- the
    //     reader that proved `scale` is a scale at all.
    //   * OT_PARTICLESYSTEM stores its volume KIND in the object, and the
    //     provider returns that byte directly, so the field's legal values are
    //     fixed by the interface rather than by observation.
    //
    // Nothing here calls the game's virtuals -- it recomputes from fields on the
    // IPC thread. Calling slot 2 would run engine code off the engine thread.
    struct CullVolumeCheck {
        size_t models;               // OT_MODEL objects examined
        size_t model_vis_radius_pos; // vis_radius > 0
        size_t model_radius_ok;      // vis_radius * scale is finite and > 0
        // The asset link. `asset` is a SHARED per-asset record (34 distinct
        // across 215 objects live) and vis_radius is a CACHE of its radius, so
        // comparing them checks the pointer AND both float offsets at once. A
        // stale cache would be a genuine engine bug, not a mapping error -- which
        // is exactly why it is worth watching.
        size_t model_asset_nonnull;  // asset != nullptr
        size_t model_asset_radius_eq; // asset->radius == vis_radius
        size_t particles;            // OT_PARTICLESYSTEM objects examined
        size_t particle_type_ok;     // cull_volume_type is 1 or 2
        size_t particle_sphere;      // cull_volume_type == 1
        size_t particle_aabb;        // cull_volume_type == 2
        // OT_SPRITE's selector is a KIND byte, not a volume enum: the provider
        // hard-codes kinds 3/4/7/9 as AABB-shaped and everything else as a
        // sphere. So unlike the particle case, the legal values are not fixed by
        // the interface -- only the split is. Each shape gets its own sanity
        // check because they read different fields.
        size_t sprites;              // OT_SPRITE objects examined
        size_t sprite_aabb;          // kind in {3,4,7,9}
        size_t sprite_sphere;        // any other kind
        size_t sprite_aabb_ordered;  // of the AABB kinds, aabb_min <= aabb_max componentwise
        size_t sprite_radius_ok;     // of the sphere kinds, radius finite and > 0
    };

    // nullopt on fault or a walk that failed to terminate.
    std::optional<CullVolumeCheck> check_cull_volumes(size_t max_per_type) const;

    // ---- attachments, owned objects, slot index ---------------------------
    //
    // LTObject carries a parent/child attachment graph (child_list at +0x74,
    // parent_link at +0x7C, parent at +0x88), a `self` back-pointer that exists
    // so a bare link can recover its object, a list of GAME-side objects it owns
    // (owned_list), and a unique per-object slot_index with a -1 sentinel.
    //
    // A note on what is and is not a strong guard here. The attachment
    // population is SCENE-DEPENDENT: at the main menu exactly one object has a
    // parent, so "parented objects exist" cannot be asserted -- it would fail
    // spuriously in a scene with none. The checks that stay meaningful are the
    // TOTAL ones:
    //   * self == own address, on every object;
    //   * the biconditional (parent == null) == (parent_link self-pointing), on
    //     every object -- this is what actually pins both offsets together;
    //   * children_reached == parented, a cross-count identity between two
    //     independently-walked quantities -- but ONLY over a COMPLETE walk. A
    //     truncated sample breaks it for a mundane reason: a sampled child's
    //     parent can fall outside the sample, so the child is counted as
    //     parented while nothing walks the list it sits in. `listed` reports
    //     every element traversed, so a caller can tell coverage from sampling
    //     (complete iff listed == objects) and only assert the identity when it
    //     actually applies.
    // The identity is weak when the attachment population is small, and that
    // weakness is the honest state of the evidence rather than something to
    // paper over.
    struct AttachmentCheck {
        size_t objects;          // objects SAMPLED
        size_t listed;           // objects TRAVERSED (> objects means truncated)
        size_t self_ptr_ok;      // self == own address
        size_t parentless;       // parent == nullptr
        size_t parented;         // parent != nullptr
        size_t link_consistent;  // (parent == nullptr) == (parent_link self-pointing)
        size_t children_reached; // entries found by walking every child_list
        size_t child_parent_ok;  // each such child names the walker as its parent
        size_t owned_nonempty;   // objects with a non-empty owned_list
        size_t owned_entries;    // total owned_list entries
        size_t index_none;       // slot_index == 0xFFFFFFFF
        size_t index_set;        // slot_index is a real index
        // shared_ref: a refcounted record the base destructor releases. Checked
        // against ITSELF -- the record stores its own address twice and a
        // positive count -- so no baseline and no sibling structure is involved.
        size_t shared_refs;          // objects with a non-null shared_ref
        size_t shared_ref_count_ok;  // refcount is positive and sane
        size_t shared_ref_self_ok;   // both self-pointer copies equal the record's address
    };

    // nullopt on fault or a walk that failed to terminate.
    std::optional<AttachmentCheck> check_attachments(size_t max_per_type) const;

    // ---- spatial records (the cull volume the engine actually stored) ------
    //
    // Each LTObject points at an LTSpatialRecord whose head holds the culling
    // volume its vtable slot 2 produced. That makes this the strongest available
    // check on the geometry mapping, because the engine wrote those bytes
    // INDEPENDENTLY of the fields they are compared against: one comparison ties
    // LTObject's position / aabb_min / aabb_max / scale, LTModelObject's
    // vis_radius, LTSpriteObject's kind / aabb / radius and
    // LTParticleSystemObject's cull_volume_type / offsets to a separate copy of
    // the same values. A single moved offset anywhere in that set breaks it.
    //
    // Two legitimate reasons a record's volume is all zeros, and both are
    // COUNTED rather than tolerated:
    //   * OT_NORMAL -- its provider is `return 0`, so no volume is ever stored;
    //   * flags3 bit 0x80 -- the shared WORLDMODEL/CAMERA provider tests that
    //     byte as SIGNED and bails, which live is exactly the level-geometry
    //     object.
    // `unexplained` must be zero: anything neither matching nor gated is a real
    // mapping failure, not noise.
    struct SpatialRecordCheck {
        size_t objects;        // objects examined
        size_t backpointer_ok; // record->object == the owning object
        size_t volume_matched; // head equals the volume recomputed from typed fields
        size_t volume_gated;   // legitimately absent (OT_NORMAL, or flags3 & 0x80)
        size_t unexplained;    // neither -- MUST be 0
        // The record's ENTRY LIST -- each entry is one object<->hit association,
        // and every entry sits in two lists at once with different linkage. What
        // is checked here:
        //   * entry_count is a MAINTAINED count, not a hint: it must equal the
        //     walked length of entry_list on every record.
        //   * each entry names its own record.
        //   * the hit-side doubly-linked pointers are mutually consistent.
        // A wrong record offset makes the count disagree with the walk; a wrong
        // ENTRY offset makes the hit-side links stop pointing back. The two
        // failure modes are distinguishable, which is why both are counted.
        size_t entries;             // entries reached across all records
        size_t count_matches_walk;  // records where entry_count == walked length
        size_t entry_record_ok;     // records whose every entry names it
        size_t hit_links_ok;        // records whose entries' hit links all check out
        // The far end of the association: each entry's hit_head addresses a
        // LTVisSector's entry_list slot, so the sector is reachable through the
        // object graph with NO global pointer -- which is why this can be checked
        // in-process at all. Counted PER ENTRY rather than per sector because a
        // per-sector tally would need a set, and a POD SEH walk has no room for
        // one. Every entry pointing at a malformed sector is a hit either way.
        size_t entry_sector_aabb_ok;   // the sector's AABB is ordered
        size_t entry_sector_planes_ok; // all its plane normals are unit length (or it has none)
        // The VISIBILITY GATE. LTObjectOwner_UpdateSpatialRecord only collects
        // associations when `(flags & 1) && !(flags2 & 0x700)`; otherwise it
        // calls Release and the list stays empty. So the gate is NECESSARY for
        // entries to exist -- asserted below as gated_violations == 0 -- but not
        // sufficient, because a gated object's volume can still miss every
        // sector. Same asymmetry as renderable-implies-linked, and asserted the
        // same way: only the direction the engine guarantees.
        //
        // This is what explains OT_CAMERA holding no associations at all despite
        // getting an AABB volume: no camera has flags bit 0.
        size_t gate_open;         // (flags & 1) && !(flags2 & 0x700)
        size_t records_with_entries;
        size_t gated_violations;  // entries > 0 while the gate is CLOSED -- MUST be 0
    };

    // nullopt on fault or a walk that failed to terminate.
    std::optional<SpatialRecordCheck> check_spatial_records(size_t max_per_type) const;

    // ---- renderability vs world-tree membership ---------------------------
    //
    // LTObject_IsRenderable is `!(flags & 0x200) && (flags & 0x10C30)`, and
    // LTObject_SetFlags adds to / removes from the world tree exactly when that
    // result flips. So the predicate and the tree link are two views of one
    // mechanism, and comparing them checks `flags`, `world_tree_link` and the
    // mask decode together.
    //
    // Read the asymmetry carefully, because it decides what may be asserted:
    //   * renderable IMPLIES linked -- 1758/1758 live, zero counter-examples.
    //     Mechanism-backed, so `renderable_not_linked` is asserted to be 0.
    //   * linked does NOT imply renderable -- 384 live objects are linked while
    //     the predicate is false, because removal is less prompt than insertion.
    //     That count is REPORTED, never asserted; forcing a biconditional here
    //     would be inventing an invariant the engine does not maintain.
    // Everything else in the flags bit survey is a per-scene rate and stays in
    // the schema comment rather than in a test.
    struct RenderFlagCheck {
        size_t objects;
        size_t renderable;           // predicate true
        size_t linked;               // threaded into the world tree
        size_t renderable_not_linked; // MUST be 0 -- the mechanism-backed direction
        size_t linked_not_renderable; // reported only; 384 live, legitimately non-zero
        size_t suppressed;           // flags & 0x200
        size_t suppressed_linked;    // MUST be 0 -- suppressor implies not linked
    };

    // nullopt on fault or a walk that failed to terminate.
    std::optional<RenderFlagCheck> check_render_flags(size_t max_per_type) const;

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

    // ---- object handles (HOBJECT <-> object) -----------------------------
    //
    // CONSUMER API, and the one every other engine call needs. The engine's
    // HOBJECT is an INDEX into CClientMgr's handle table, not a pointer -- which
    // is why no module global holds an object address, and why a mod that gets a
    // handle back from any ILT* method needs this to do anything with it.
    //
    // CClientMgr_RegisterObjectHandle writes {tag=1, object} at index `handle`
    // when an object is created, so the table is authoritative and sparse: live
    // 7719 slots hold 3248 objects.

    // The engine's "no handle" sentinel. 335 of 3583 live objects carry it, and
    // they are exactly the objects whose slot_index is also unset.
    static constexpr uint16_t kNoHandle = 0xFFFF;

    // Object for `handle`, or nullptr for kNoHandle, an out-of-range index, a free
    // slot, or a faulted read. SEH-guarded: a stale handle returns nullptr rather
    // than a wild pointer, which is the whole point of going through the table
    // instead of caching object addresses.
    const regenny::LTObject* object_from_handle(uint16_t handle) const;

    // The handle an object carries, or nullopt when it has none. Cheap -- it reads
    // the object's own field rather than searching the table.
    std::optional<uint16_t> handle_of(const regenny::LTObject* obj) const;

    // Slots in the table. Grows to fit the highest handle ever registered, so it
    // is an upper bound on handles, not a live object count.
    std::optional<size_t> handle_table_size() const;

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
    // PRIVATE ON PURPOSE (AGENTS.md 5a): if callers could read this, a
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
