#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "regenny/regenny/LTObject.hpp"
#include "regenny/regenny/LTRotation.hpp"
#include "regenny/regenny/LTVector.hpp"

// Consumer-facing model and skeleton access.
//
// Everything here answers a question a mod actually asks -- "what model is this
// object?", "where is its head bone?", "what is that bone's pose?" -- rather than
// a question the test suite asks. The CClientMgr::check_* family validates that
// the mapping underneath is still correct; this is what you build on top of it.
//
// Two rules shape the interface:
//
//  * Nothing returns a raw engine pointer. The skeleton lives in an allocation
//    owned by a SHARED asset that can be released when the last model referencing
//    it goes away, so handing out `const char*` into it would be a use-after-free
//    waiting for a level transition. Names and lists are copied out.
//
//  * Every read is SEH-guarded and bounds-checked against the engine's own counts,
//    so a stale view returns nullopt rather than faulting. That matters because a
//    view is only valid for as long as the object lives, and a mod holding one
//    across a load screen is the expected mistake, not an exotic one.
namespace sdk {

// A read-only view of one model object's skeleton.
//
// Construction is two pointer reads, so build it per use rather than caching it.
// The node data belongs to the shared LTModelAsset, which means every model using
// the same .mdl sees the same skeleton -- node indices are stable per ASSET, not
// per object, and are safe to remember for as long as the asset stays loaded.
class ModelSkeleton {
public:
    // nullopt when `obj` is not an OT_MODEL, has no asset, or the asset carries no
    // node array. Does not fault on a dangling object.
    static std::optional<ModelSkeleton> from_object(const regenny::LTObject* obj);

    size_t node_count() const { return m_count; }

    // Copied out, because the engine's storage may be freed with the asset.
    std::optional<std::string> node_name(size_t index) const;

    // Case-insensitive, matching how the engine matches names itself (its own
    // lookup hash folds case through a translation table). Returns the node index,
    // which is what every other method here takes.
    //
    // Linear over the name array. That is deliberate: skeletons here run 2..84
    // nodes, so a scan is a few hundred byte comparisons, and it needs no engine
    // data beyond what this view already holds. The engine's hash array is used by
    // the test suite to prove the mapping, not by this lookup.
    std::optional<size_t> find_node(std::string_view name) const;

    // nullopt for the root (which stores 255, the engine's "no parent" sentinel)
    // and for an out-of-range index.
    std::optional<size_t> parent_of(size_t index) const;

    // A node's children are a CONTIGUOUS run, so this is a range rather than a
    // list: indices [first, first + count).
    struct Children {
        size_t first;
        size_t count;
    };
    std::optional<Children> children_of(size_t index) const;

    // Each node stores TWO (position, rotation) pairs. Which is which is NOT
    // established -- the obvious reading, that the second is the first's rigid
    // inverse, was tested and fails on most nodes (see fear2.genny's LTModelNode).
    // They are exposed raw and named non-committally so a caller can experiment
    // without inheriting a guess dressed up as an API.
    struct Pose {
        regenny::LTVector position;
        regenny::LTRotation rotation;
    };
    std::optional<Pose> pose_a(size_t index) const;
    std::optional<Pose> pose_b(size_t index) const;

    // Indices from `index` up to and including the root, nearest first. Useful for
    // composing a world transform, since the array is in topological order and a
    // parent always precedes its children. Bounded by node_count, so a corrupt
    // parent chain returns nullopt instead of looping.
    std::optional<std::vector<size_t>> path_to_root(size_t index) const;

    // ---- the bone matrix palette (see fear2.genny's node_matrices) ---------
    //
    // CONFIRMED what this is, from its two consumers: the renderer walks a mesh's
    // bone index list, writes `palette + 48*bone` into a shader constant slot for
    // each, then uploads 48*count bytes -- capped at 24 bones, gated on the constant
    // registers being bound. It is the SKINNING PALETTE handed to the vertex shader.
    // Stride and length come from the allocator (48 * node_count), so indexing here
    // is bounds-checked against node_count and against the parent allocation.
    //
    // THE CATCH, and it decides how a mod must use this: the palette is PER-FRAME
    // RENDER STATE. It is filled during the draw, for the models actually being
    // drawn. Read from an idle game it is mostly zeros -- live, 169 of 215 models
    // read entirely zero, the player's own viewmodel included. So:
    //   * reading it off the render path gives blank or stale data, not an error, and
    //   * is_populated() below is the difference between a real matrix and a slot
    //     nobody has written this frame.
    // A caller that wants reliable bone transforms should read this from a render
    // hook, or get them from the skeleton's own poses instead.
    struct BoneMatrix {
        float m[12];  // three rows of four
    };

    // nullopt for an out-of-range index, a model without the palette (it is gated on
    // a flag bit), or a faulted read.
    std::optional<BoneMatrix> bone_matrix(size_t index) const;

    // Whether a slot has actually been written: all twelve floats finite, and not
    // all zero. This is the right question for a palette entry -- an earlier version
    // of this API tested whether the 3x3 was a proper ROTATION, which is the wrong
    // question, because an unwritten slot is zeros rather than a bad rotation.
    static bool is_populated(const BoneMatrix& mat);

    // The asset backing this skeleton. Two models with the same value share node
    // indices; comparing it is how a caller can cache per-asset work.
    uintptr_t asset_id() const { return reinterpret_cast<uintptr_t>(m_asset); }

    // ---- SOCKETS ---------------------------------------------------------------
    //
    // A socket is a NAMED, oriented point hanging off a node -- the art department's
    // own attach points, which is why they matter far more than anything derivable
    // from geometry. A character asset defines `camera`, `eyes`, `socket_left_eye`,
    // `socket_right_eye` (all on Head), `LeftHand`, `RightHand`, `back`, `LeftFoot`,
    // `RightFoot`; a weapon defines `flash`, `breach`, `laser`, `flashlight`.
    //
    // For a VR mod this is the shortest path to the things that are otherwise guessed
    // at: where the view should sit, where each eye goes, and where a hand belongs.
    // Live, 186 sockets across 34 assets, every one named and every one pointing at a
    // node index inside this skeleton.
    struct Socket {
        std::string name;
        // Index into THIS skeleton -- feed it to node_name() or bone_matrix().
        size_t node_index;
        // The socket's offset from that node. Real art values, not zeroes:
        // socket_left_eye sits at (-3.013, 13.745, 2.784) from Head on the sampled
        // character, with a unit-length rotation.
        regenny::LTVector position;
        regenny::LTRotation rotation;
    };

    size_t socket_count() const { return m_socket_count; }

    // nullopt when the index is out of range or the read faulted.
    std::optional<Socket> socket(size_t index) const;

    // CASE-INSENSITIVE, because the engine's own lookup is: it compares with
    // String_EqualsI. That is not pedantry -- the live data mixes `flash` and `Flash`
    // across assets, so a case-sensitive search would miss half the weapons.
    std::optional<size_t> find_socket(const char* name) const;

    // ---- CACHED NODE TRANSFORMS -------------------------------------------------
    //
    // Where a bone actually IS, as the engine last computed it -- position and
    // rotation per node. This is a different thing from bone_matrix() above: that is
    // the skinning palette the renderer fills during a draw, so it is empty on an idle
    // frame. These caches are populated all the time (2222/2222 slots hold a unit
    // rotation live), which makes them the readable source for "where is the head".
    //
    // THE CATCH IS A FLAG, NOT A GUESS, and that is what makes this usable. The engine
    // keeps a per-node dirty byte and recomputes a node's transform before using it
    // when that byte is set (LTModelObject_MarkNodeSubtreeDirty writes it, and
    // LTModelObject_GetNodeTransform tests it). Measured live: of 2222 slots, 343 had
    // a clear flag and ALL 343 held a sane position; of the 1879 dirty ones, 187 held
    // values up to 7.8e37. So `stale` is not advisory -- reading a stale position is
    // reading uninitialised memory.
    //
    // The rotation is a different matter: it is unit-length on every slot sampled,
    // stale or not, so it is NOT a validity test and cannot be used as one.
    //
    // A model carries TWO of these caches at once and a mode selector decides which
    // the engine reads. This picks the same one the engine would; a caller does not
    // see the choice.
    struct NodeTransform {
        regenny::LTVector position;
        regenny::LTRotation rotation;
        // True when the engine would recompute before using this. The position must
        // not be trusted; the rotation was unit on every sample either way.
        bool stale;
    };

    // nullopt when the index is out of range, the cache is absent, or the read
    // faulted. A STALE entry is still returned -- flagged -- because a caller deciding
    // "skip this frame" needs to know the difference between stale and missing.
    std::optional<NodeTransform> node_transform(size_t index) const;

private:
    // The engine's per-object allocation, reconstructed from BindAsset's own size
    // expression, so every palette read can be bounded exactly instead of trusting
    // a region pointer that a rebind may have left stale.
    uintptr_t m_alloc_base{};
    uintptr_t m_alloc_end{};
    // The object as well as the asset: node data is per-ASSET (shared), but the
    // 48-byte region is per-OBJECT, so a view needs both.
    const void* m_object{};
    const void* m_asset{};
    const void* m_records{};
    const void* m_names{};
    size_t m_count{};
    // Per-ASSET like the node data above: two models sharing an asset share sockets.
    const void* m_sockets{};
    size_t m_socket_count{};
    // The ACTIVE node-transform cache and its dirty array, already resolved through
    // the model's mode selector so callers never branch on it.
    const void* m_node_xform{};
    const void* m_node_dirty{};
    size_t m_dirty_stride{};
    size_t m_dirty_offset{};
};

// The object's .mdl path, e.g. "char\ai\rep_heavyweapons\rep_heavyweapons.mdl".
// This is the cheapest way to identify what an object IS -- far more reliable than
// inferring it from geometry -- and it is the engine's own cache key, so it matches
// case-insensitively against whatever a level references.
std::optional<std::string> model_filename(const regenny::LTObject* obj);

// The object's material paths (the .mat files it renders with).
//
// THE INDEX IS THE ENGINE'S PIECE INDEX, which is the part worth knowing: the
// vector's length equals the shared asset's own material_count (equal on 215/215
// live), and that same count is what the engine uses to size this array when it
// binds the model. So slot i here is the i'th piece as the engine numbers it, and an
// index taken from any engine-facing piece API lines up with this vector directly.
//
// Empty entries are legal and are returned as empty strings rather than skipped,
// because the POSITION carries meaning -- compacting would silently renumber the
// pieces. Live, 34 of 476 slots are empty.
std::optional<std::vector<std::string>> model_materials(const regenny::LTObject* obj);

// ---- animation state -------------------------------------------------------
//
// What the engine keeps on the model's embedded record. A mod reaches for this to
// notice that an animation CHANGED (index moved) or to time something against one
// (the fraction), without knowing where the fields live.
struct AnimState {
    // Both are indices into the asset's animation table, and that bound is how they
    // were identified: strictly less than the table's entry count on 215/215 models
    // across 34 assets whose counts range 1..1039.
    //
    // WHICH ONE IS CURRENT IS NOW SETTLED, by the engine rather than by inference:
    // ILTModel::GetCurAnim resolves a tracker and returns the field `current` mirrors.
    // An earlier version of this struct exposed the pair as `index`/`index_b` and said
    // the roles were unestablished; use `current` for "what is playing", and prefer
    // model_current_anim_name() over either if what you want is the NAME.
    uint16_t index;
    // THE CURRENT ANIMATION, as the engine reports it. Equal to `index` on 214 of 215
    // models live, so the two normally coincide; what makes them diverge is unmapped.
    uint16_t current;
    // Within [0,1] on 215/215. Deliberately not called "progress" or "blend weight":
    // 36 models hold a mid-range value while their two indices agree, which a pure
    // blend weight would not do, and a pure playback position would not explain the
    // 178 sitting at exactly 0 or 1. Read it as a normalised fraction and measure
    // what it tracks before relying on either reading.
    float fraction;
    // Two NODE indices the record carries alongside the animation ones, proven to be
    // node indices by staying inside node_count while SCALING with it (max 37 and 38
    // on an 84-node skeleton). Resolve either through ModelSkeleton::node_name() to
    // get a bone name -- live they land on Pelvis, Root_assault, Hand_attach_Jnt and
    // similar, so this is how a caller sees which bone a track is anchored to.
    //
    // Ordered: node_b >= node_a always, and the pair is either equal or exactly
    // adjacent. Their ROLE is unresolved -- a subtree span was tested and failed --
    // so they are exposed as the two indices they are and nothing more.
    uint16_t node_a;
    uint16_t node_b;
};

// nullopt when `obj` is not a model or the read faulted.
std::optional<AnimState> model_anim_state(const regenny::LTObject* obj);

// How many animations the model's asset has -- the bound `index` respects, so a
// caller can validate or range-check without a second lookup. The count comes from
// the asset's animation-name table, which the engine binary-searches by name hash.
std::optional<size_t> model_anim_count(const regenny::LTObject* obj);

// THE NAME of the animation this model is currently playing, e.g. "Ragdoll",
// "PostFire", "Alma_Stand_Searching", "Corpse_surface_faceup".
//
// This is the most legible thing the model subsystem exposes, and it is what a mod
// actually wants: reacting to "the player is reloading" beats tracking an opaque
// index that changes meaning between assets. Resolved exactly as ILTModel::GetAnimName
// does -- the current index into the asset's animation-record vector, then the record's
// name pointer -- so it agrees with whatever the engine would report.
//
// Live it answered for every model in one sample (215/215) while a raw probe of the
// same chain got 214/215 in another -- models come and go between samples, and the
// engine null-checks the record slot itself, so a nullopt is a LEGAL state rather than
// a failure. nullopt also when the object is not a model or the read faulted; those
// three are not distinguished because a caller can do nothing different about them.
std::optional<std::string> model_current_anim_name(const regenny::LTObject* obj);

// Whether a PIECE is hidden -- the engine's own per-piece visibility bit.
//
// `index` is the engine's piece index, the same numbering model_materials() uses, so
// a caller can find the piece it wants by material path and then ask about it. This is
// the mechanism a VR mod needs to hide a viewmodel's arms or a character's head
// without touching geometry.
//
// HONEST STATE OF THE EVIDENCE: the reader is unambiguous -- it is the entire body of
// ILTModel::GetPieceHideStatus, a bit test against a two-dword mask. But live, every
// bit is clear on all 215 models, so nothing in the sampled scene is hidden and the
// data cannot corroborate the reader; it is only consistent with it. Treat a `true`
// from this function as trustworthy in mechanism and untested in the field.
//
// nullopt when the object is not a model, the index is at or beyond the model's piece
// count, or the read faulted.
std::optional<bool> model_piece_hidden(const regenny::LTObject* obj, size_t index);

// The NAME of a piece, e.g. "Face", "body_shadow", "Clip_Group", "Assault_Group".
//
// This is what makes model_piece_hidden() usable: a mod wants to hide "the head", not
// "piece 4", and piece numbering differs per asset. Resolved exactly as
// ILTModel::GetPieceName does, from the asset's own piece-name array.
//
// nullopt when the object is not a model, the index is at or beyond piece_count, or
// the read faulted.
std::optional<std::string> model_piece_name(const regenny::LTObject* obj, size_t index);

// How many PIECES the model has -- the bound both piece accessors respect.
//
// THIS IS NOT model_materials().size(). Pieces are the addressable sub-objects;
// materials are what they render with, and several pieces can share one. Live the two
// counts differ on 17 of 34 assets (a grunt has 7 pieces and 3 materials), so a caller
// that used the material count as a piece bound would silently skip real pieces --
// which is exactly the bug an earlier version of this header shipped.
std::optional<size_t> model_piece_count(const regenny::LTObject* obj);

// Find a piece by NAME, case-insensitively (matching how the engine compares names
// elsewhere). nullopt when no piece matches.
std::optional<size_t> model_find_piece(const regenny::LTObject* obj, const char* name);


// How a mod finds the object it cares about.
//
// Identity by PATH is the reliable route: an object's .mdl is what it is, whereas
// position, type and node count are all shared by dozens of unrelated objects. So
// "find the assault rifle" is a substring search over model_filename, not a guess
// about which of 215 models is interesting.
struct ModelMatch {
    const regenny::LTObject* object;  // valid only while the object lives
    std::string filename;             // its .mdl path, copied
    uint16_t handle;                  // CClientMgr::kNoHandle when it has none
};

// Every live OT_MODEL whose .mdl path contains `needle`, case-insensitively, up to
// `max` results. An empty needle matches everything, which is the cheap way to
// enumerate what is loaded.
//
// Returns handles as well as pointers on purpose: a pointer is only good for this
// frame, while a handle survives and can be re-resolved through
// CClientMgr::object_from_handle, so a mod that wants to remember an object should
// keep the handle.
std::vector<ModelMatch> find_models(std::string_view needle, size_t max = 64);

}  // namespace sdk
