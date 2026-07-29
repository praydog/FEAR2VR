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

    // ZERO IS THE COMMON CASE, not an error: measured across 34 live assets the range is
    // 0..19, and most of them -- every shell casing, debris chunk, medkit, armour pickup
    // and collectible in the sample -- have none, because a prop has nothing to attach.
    // Only characters and weapons carry sockets. Do not treat an empty table or a failed
    // find_socket() as a fault.
    size_t socket_count() const { return m_socket_count; }

    // nullopt when the index is out of range or the read faulted.
    std::optional<Socket> socket(size_t index) const;

    // CASE-INSENSITIVE, because the engine's own lookup is: it compares with
    // String_EqualsI. That is not pedantry -- the live data mixes `flash` and `Flash`
    // across assets, so a case-sensitive search would miss half the weapons.
    std::optional<size_t> find_socket(const char* name) const;

    // ---- EYE GEOMETRY, FROM ASSET DATA ------------------------------------------
    //
    // A stereo renderer needs to know where a character's eyes are and how far apart. That is
    // available WITHOUT touching the bone cache at all: socket offsets are asset data, so unlike
    // socket_world_transform() they are never stale, never need evaluation, and are identical on
    // every instance of the model.
    //
    // THE CATCH IS THAT TWO SOCKET POSITIONS ARE ONLY COMPARABLE IF THEY SHARE A NODE. Each offset
    // is relative to its own node_index, so subtracting positions taken from different bones is
    // meaningless -- it mixes two coordinate frames. These helpers check that and refuse rather
    // than returning a plausible number, which is the whole reason they exist instead of leaving
    // callers to subtract socket().position themselves.

    struct EyeGeometry {
        // Offsets from the shared node, in asset space.
        regenny::LTVector left;
        regenny::LTVector right;
        // The node both hang off -- feed it to node_name().
        size_t node_index;
        // Distance between them, in engine units.
        //
        // DO NOT ASSUME IT IS NON-ZERO. Measured across the 30 rigged characters live, this ranges
        // from 0.0 to 5.54 -- at least one rig places both eye sockets at the SAME point, so a
        // consumer using this as a stereo baseline must handle zero rather than dividing by it.
        float separation;
        // Midpoint, which is where a stereo pair should be centred.
        regenny::LTVector center;
    };

    // nullopt when either socket is absent (most models -- only characters are rigged with them),
    // when they hang off DIFFERENT nodes, or when a read faulted. Socket names are matched
    // case-insensitively, as the engine matches them.
    //
    // THE SHARED-NODE CHECK HAS NEVER FAILED LIVE: all 30 models carrying both sockets hang them
    // off one node, and the camera socket too. It is still checked, because the refusal is the
    // difference between a meaningless number and no number.
    //
    // AND THE RIGS ARE NOT ANATOMICALLY TIDY -- three plausible assumptions died on measurement.
    // The eyes are NOT mirror-symmetric about the node origin (|left.x + right.x| reaches 6.01),
    // they are NOT level with each other (only 8 of 30 match in y and z within 0.01), and `left`
    // is not reliably the -x side (22 of 30). So this returns the art as it is and asserts nothing
    // about its shape; a consumer wanting a symmetric stereo pair must impose that itself.
    std::optional<EyeGeometry> eye_geometry() const;

    // The `camera` socket's offset from the same node the eyes use, so a caller can relate the
    // engine's view attach point to the eyes -- the offset a VR camera has to undo.
    //
    // nullopt when there is no camera socket, no eye geometry, or the camera hangs off a different
    // node than the eyes.
    std::optional<regenny::LTVector> camera_to_eye_center() const;

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
        // WHICH SPACE the position/rotation are in, and this is not cosmetic: the model
        // carries two caches and they DO NOT AGREE on space. Measured over every clean
        // slot, split by the mode selector at +0x156:
        //
        //   selector == 0   297 slots, ALL near the model origin      -> model space
        //   selector != 0    46 slots, ALL near the object position   -> WORLD space
        //
        // Zero exceptions either way, and one world-space bone sat 0.00 units from its
        // object. A caller that composes with the object's transform unconditionally
        // double-applies it on the second group -- which is exactly the bug that
        // produced a 5449-unit socket offset before this flag existed.
        bool world_space;
    };

    // nullopt when the index is out of range, the cache is absent, or the read
    // faulted. A STALE entry is still returned -- flagged -- because a caller deciding
    // "skip this frame" needs to know the difference between stale and missing.
    std::optional<NodeTransform> node_transform(size_t index) const;

    // ---- SOCKET TRANSFORMS: where a named attach point actually IS ---------------
    //
    // This is the primitive a VR mod is ultimately after: give it "camera" or
    // "LeftHand" and get back a position and orientation it can use. It composes the
    // socket's offset with its bone's current transform, the way the engine's own
    // ILTModel::GetSocketTransform does.
    //
    // THE MATH IS THE ENGINE'S, TRANSCRIBED RATHER THAN REDERIVED. LTTransform_Compose
    // (dump 0x4292C7) is:
    //     out.rotation = LTRotation_Multiply(parent.rotation, child.rotation)
    //     out.position = parent.position + rotate(parent.rotation, child.position) * parent.scale
    //     out.scale    = parent.scale * child.scale
    // and the quaternion product and the rotate-by-quaternion are copied term for term
    // out of LTRotation_Multiply (0x424C4F) and LTRotation_RotateVector (0x404C7F).
    // Hand-rolling either would have meant guessing a sign convention; the engine's
    // own expressions cannot disagree with the engine.
    struct SocketTransform {
        regenny::LTVector position;
        regenny::LTRotation rotation;
        float scale;
        // Propagated from the underlying node transform. A stale socket transform is
        // built on a stale bone, so it inherits the warning wholesale -- see
        // node_transform() for what the flag means and how it was measured.
        bool stale;
    };

    // In the MODEL's own space: socket offset composed with its bone's transform.
    std::optional<SocketTransform> socket_transform(size_t socket_index) const;

    // In WORLD space: the above, composed once more with the owning object's own
    // position/rotation/scale. This is what a mod attaches something to.
    // NOTE A STALE RESULT IS NOT AN ERROR and is returned as-is: `stale` means the engine has
    // never evaluated this model's bone cache, so the numbers are whatever the allocation held
    // and CAN be non-finite -- measured at 7 of 702 live transforms, all stale. Use
    // socket_world_transform_is_usable() unless you specifically want the raw read.
    std::optional<SocketTransform> socket_world_transform(size_t socket_index) const;

    // IS THAT TRANSFORM SAFE TO USE? The question every consumer of the above actually has --
    // attaching a hand, a weapon, a camera or a tracked controller to a socket needs to know
    // whether the pose can be applied, not merely whether a struct came back.
    //
    // Three things must hold, and they are checked HERE rather than at each call site because
    // getting them wrong is silent: a NaN position propagates into a matrix and the object
    // vanishes rather than erroring.
    //
    //   * not stale     -- the engine's bone cache has actually been evaluated for this model
    //   * finite        -- position components are neither NaN nor infinity
    //   * unit rotation -- the composed quaternion still has length 1
    //
    // MEASURED, so the staleness test is not defensive padding: of 702 live socket transforms,
    // 7 had non-finite positions and EVERY ONE of them was stale -- zero clean transforms were
    // non-finite. A model whose cache was never evaluated holds whatever its allocation held, so
    // `stale` is the flag that separates "the engine has posed this" from "these are old bytes".
    //
    // nullopt when the transform cannot be composed at all; false when it composed into
    // something unusable.
    std::optional<bool> socket_world_transform_is_usable(size_t socket_index) const;

    // ---- ASK THE ENGINE INSTEAD -------------------------------------------------------
    //
    // Everything above reads the bone cache and composes. THIS calls the engine's own
    // ILTModel::GetSocketTransform (vtable slot 3) and returns what the game itself would use --
    // the strongest available check on the composition, and the honest answer for a consumer that
    // does not want to depend on this SDK's arithmetic at all.
    //
    // THE SIGNATURE IS PINNED BY THE DISASSEMBLY, not guessed: `retn 10h` gives four dword
    // arguments, and `mov ecx, [esp+arg_0]` shows the FIRST is the OBJECT -- the entry loads it
    // into ECX, tests `[ecx+0x10] == 1` for OT_MODEL, and re-dispatches as a thiscall on it. The
    // interface `this` is never touched. So it is `(object, handle, out, flag)`, returning 0 on
    // success and 60 (LT_INVALIDPARAMS) when the gate rejects the object. The out buffer is eight floats
    // -- position, quaternion xyz, quaternion w, scale -- which the engine's own failure path
    // reveals by zeroing all eight and writing 1.0 into the last two.
    //
    // `handle` follows the engine's combined numbering: sockets below socket_count, nodes above
    // it -- the same rule socket_handle_transform() implements.
    //
    // CALLING THIS CAN MUTATE ENGINE STATE. LTModelObject_GetNodeTransform evaluates the skeleton
    // when the node is dirty and clears the flag, so on a STALE socket this is not a read: it asks
    // the engine to pose the model, which belongs on the game thread. On a CLEAN socket the dirty
    // check short-circuits and the call is pure. Callers off the game thread should restrict
    // themselves to sockets socket_world_transform_is_usable() already accepts.
    //
    // nullopt when the interface is unresolved, the slot guard fails, the object is absent, or the
    // engine reports failure.
    // `world_space` is the engine's own trailing argument, forwarded to
    // LTModelObject_GetNodeTransform, and MEASUREMENT settled what it selects:
    //
    //   1 -> WORLD space. Matches socket_world_transform() on 181 of 181 clean sockets.
    //   0 -> MODEL space. Matches socket_transform() on 131 of 181.
    //
    // The 131-versus-181 split is not noise, it is the whole story: the other 50 models keep
    // their bone cache ALREADY in world space, so for them model and world coincide and flag 0
    // matches both. 131 + 50 = 181, exactly.
    //
    // That is what makes this the strongest check in the SDK on socket_world_transform(): the
    // engine agrees on every clean socket, including the world-space branch that composition
    // deliberately skips.
    //
    // Defaults to world space, because that is what an attachment wants and what the primary
    // accessor returns.
    std::optional<SocketTransform> engine_socket_transform(size_t handle,
                                                           int world_space = 1) const;

    // DOES ILTModel VTABLE SLOT 3 STILL HOLD GetSocketTransform? A slot index is a claim like any
    // other, and getting it wrong calls an arbitrary function rather than failing. This compares
    // the slot against the function's known module offset, so the call above can refuse rather
    // than guess. Exposed so a consumer can check once at startup instead of per call.
    // THE ONE CALL AN ATTACHMENT WANTS: a usable world pose for this socket, evaluating the
    // skeleton if the engine has not already.
    //
    // socket_world_transform() reads the cache and is honest about staleness, which leaves a
    // consumer holding 181 usable poses out of 702 and no way forward. This closes that: when the
    // cache is clean it returns the composed pose (a pure read, and the engine has been shown to
    // agree with it on every one); when it is stale it asks the engine, which EVALUATES the
    // skeleton and returns a real pose rather than old bytes.
    //
    // MUST BE CALLED ON THE GAME THREAD whenever the cache might be dirty, because evaluation
    // mutates engine state -- it poses the model and clears the dirty flag. From a mod's
    // on_frame() that is automatic. Off-thread callers should stick to
    // socket_world_transform_is_usable() and accept the smaller population.
    //
    // THE EVALUATING BRANCH IS NOT EXERCISED BY THE TEST SUITE, and the reason is structural rather
    // than an omission: the suite drives everything over IPC from its own thread, where forcing an
    // evaluation races the engine's update. So what IS verified is the clean path -- which the
    // engine itself agrees with on every clean socket -- and the engine call mechanism, verified on
    // that same population. The dirty path is the engine's own per-frame code being asked to do
    // what it already does, but a consumer should know it carries less evidence than the rest of
    // this header.
    //
    // An attempt to verify it by walking every dirty skeleton from the frame hook wedged the
    // payload; see reversing/MAPPING_WORKFLOW.md. A bounded few-per-frame refresh is the shape that
    // would work.
    //
    // nullopt when the socket cannot be resolved or the engine reports failure; the result is
    // never stale by construction.
    std::optional<SocketTransform> socket_pose(size_t handle) const;

    static bool engine_socket_transform_available();

    // Diagnostics for the call above: the engine's own return code from the most recent attempt,
    // and the byte ILTModel_GetSocketTransform gates on before doing any work (it requires 1).
    // Exposed because "the engine refused" is not actionable without the reason.
    static int64_t last_engine_rc();
    static int64_t engine_iface_gate_byte();

    // The same, BY NAME -- which is how a consumer actually asks. Every caller of the
    // index form was going to write find_socket() immediately above it, so this saves the
    // dance and the chance of pairing an index with the wrong skeleton.
    //
    // Case-insensitive, like find_socket(). nullopt when no socket has that name.
    //
    // WHAT THE PLAYER ACTUALLY DEFINES, measured on `fp_playerm05.mdl` (65 nodes, 19
    // sockets), because it decides which of these a mod can use right now:
    //
    //   LeftHand / RightHand -> L_Hand / R_Hand    bones CLEAN -- usable immediately
    //   Light                -> L_Hand             CLEAN (the flashlight mount)
    //   back, Torso_Forward  -> torso bones        CLEAN
    //   camera               -> Eye_Cam (node 64)  bone DIRTY at rest
    //   eyes, head, nose, chin, cameraDEAD -> Head bone DIRTY at rest
    //
    // So the HANDS are readable from any thread at any time, while the VIEW bones are
    // recomputed by the engine in its render path and read stale outside it -- exactly
    // the split node_transform()'s `stale` flag exists to report. Do not read that as a
    // defect: it is why the flag is part of the API.
    std::optional<SocketTransform> socket_world_transform(const char* name) const;

    // ---- THE ENGINE'S UNIFIED SOCKET HANDLE ---------------------------------------
    //
    // ILTModel_GetSocketTransform takes ONE index that means two things: below
    // socket_count it is a socket, at or above it is the node `handle - socket_count`.
    // Every engine-facing "socket handle" lives in this space -- notably
    // Attachment::socket_handle -- so a consumer holding such a value MUST resolve it this
    // way and not by guessing which table it indexes. Both tables will accept a small
    // number and both will return something plausible, which is exactly how this project
    // spent several passes with the field named wrong.
    enum class HandleKind { Socket, Node };
    struct ResolvedHandle {
        HandleKind kind;
        size_t index;  // into sockets, or into nodes, per `kind`
    };

    // Split a handle into the table it addresses and the index within it. nullopt when
    // the handle is past the end of both tables.
    std::optional<ResolvedHandle> resolve_socket_handle(size_t handle) const;

    // The handle's pose in WORLD space -- what the engine computes for an attachment.
    // Dispatches to the socket or the node path exactly as ILTModel_GetSocketTransform
    // does, so a caller reproducing engine placement uses this and nothing else.
    std::optional<SocketTransform> socket_handle_transform(size_t handle) const;

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
    // Which space the active node cache is in; the two caches differ, so this decides
    // whether a world transform still needs the object composed onto it.
    bool m_world_space{};
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


// ---- WHAT IS MOUNTED ON THIS OBJECT, BY THE SOCKET IT CARRIES -----------------
//
// The real question behind "where is the player's muzzle" is not "which attachment is
// the weapon" -- that would be a heuristic about game logic -- but "which thing mounted
// on me defines a socket called `flash`". That is mechanical, and it is what this does.
//
// LIVE, on the player, this resolves the shotgun: the player carries two attachments,
// `weapons\shotgun_clip\shotgun_clip.mdl` (6 nodes, 5 sockets) and an `engine\default.mdl`
// placeholder with none. Asking for "flash" finds the first and ignores the second
// without needing to know that a shotgun is a weapon and a default is not.
//
// A WEAPON ASSET'S SOCKET SET, measured on that shotgun, since these are the names to
// ask for -- and all five ride ONE bone (`offset`, node 1), so their offsets are the art:
//
//   flash      (0.16, 12.17, 38.29)   the muzzle: 38 units down the barrel
//   physics    (0.16, 12.17, 38.29)   identical to flash -- where recoil/impulse applies
//   laser      (0.00, 15.18, 34.63)   just above and behind the muzzle
//   flashlight (5.95,  7.75, 36.48)   offset to the side
//   breach     (2.00,  9.00,  5.00)   back near the receiver: the ejection port
//
// `flash` and `physics` being the SAME point is worth knowing before treating them as
// two anchors: the art gives one muzzle with two names.
struct AttachedSocket {
    // The attached child carrying the socket. Valid only while it lives -- keep the
    // handle if you need to remember it across frames.
    const regenny::LTObject* object;
    uint16_t child_handle;
    // WHICH ATTACH POINT of the PARENT the child rides, in the engine's unified socket
    // handle space -- resolve it with the parent's ModelSkeleton::resolve_socket_handle(),
    // never by indexing one table directly. nullopt when the record has no handle.
    std::optional<size_t> socket_handle;
    // The socket's pose in WORLD space, composed the same way socket_world_transform
    // does it -- including the `stale` flag, which matters here too: a weapon's bones
    // are recomputed on the same schedule as anything else.
    ModelSkeleton::SocketTransform transform;
};

// The first object attached to `parent` that defines a socket named `socket_name`,
// together with that socket's world transform. Case-insensitive, like find_socket().
//
// nullopt when nothing mounted on `parent` has such a socket -- which is the ordinary
// answer for an unarmed character, not a fault.
std::optional<AttachedSocket> attached_socket(const regenny::LTObject* parent,
                                              const char* socket_name);

}  // namespace sdk
