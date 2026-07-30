#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "regenny/regenny/LTObject.hpp"
#include "regenny/regenny/LTRotation.hpp"
#include "regenny/regenny/LTVector.hpp"

// Consumer-facing LTObject access.
//
// Everything a mod asks about an arbitrary object -- what kind is it, where is it,
// will it be drawn -- without the caller reaching into generated layout structs. The
// point is not convenience: reading `obj->flags2` directly means every call site
// silently depends on the schema, and there is no guard, so one stale pointer takes
// the game down. Here the reads are SEH-guarded and the offsets live in one place.
namespace sdk {

// The engine's object type, in the engine's own numbering (the value stored in
// LTObject::type, which equals the index of the CClientMgr bucket holding it).
// OT_LIGHT exists in the numbering but is UNCREATABLE -- the creation dispatcher has
// no case for it -- so no live object ever reports Light.
enum class ObjectKind : uint8_t {
    Normal = 0,
    Model = 1,
    WorldModel = 2,
    Sprite = 3,
    Light = 4,
    // NOT THE VIEW CAMERA -- read this before building on it. FEAR 2 carries 474 live
    // OT_CAMERA objects, and measured against the player NONE of them tracks him: the
    // nearest sits 84.8 units away, their positions are INTEGER and grid-aligned (several
    // share a z plane exactly), and they spread out to 16110 units. They are bulk static
    // level furniture.
    //
    // Nor does the class carry a projection: OT_CAMERA adds exactly ONE field to
    // OT_WORLDMODEL (a uint16 creation parameter), there is no FOV anywhere on it, and
    // FEAR2.exe contains no "fov" string at all. The view's projection is not an
    // engine-side camera-object property -- it lives in gameclient.dll.
    //
    // For "where is the player looking", use CClientShell::local_player() and the
    // skeleton's `camera` socket (bone `Eye_Cam`), which IS the view anchor.
    Camera = 5,
    ParticleSystem = 6,
};

// Flag bits worth naming. The engine has FOUR flag words (flags, flags2, user_flags,
// and a client byte on the spatial record); these are bits of the first one.
namespace object_flags {

// PROVEN, from the engine's own render gate rather than from the reference SDK's
// constant: LTObjectOwner_UpdateSpatialRecord collects an object for drawing only
// when `(flags & 1) && !(flags2 & 0x700)`, and SetFlags special-cases a change in
// this bit. Live it is the most common bit (1924 objects across five types) and NO
// camera has it, which is what you would expect of a visibility flag.
constexpr uint32_t kVisible = 1u << 0;

// Live it is set on ALL 474 cameras and on ZERO of the other 3109 objects, making it
// the cleanest single-bit discriminator in the word. Recorded as an observation with
// that population behind it, NOT as a proven "is a camera" flag -- nothing read so
// far tests it, so it may be a property cameras happen to share.
constexpr uint32_t kCameraOnly = 1u << 11;

}  // namespace object_flags

// A copied-out snapshot of the fields a mod usually wants together. Copied rather
// than returned by reference for the usual reason: the object can go away, and a
// caller holding a pointer into it across a frame is the expected mistake.
struct ObjectInfo {
    ObjectKind kind;
    uint16_t handle;  // 0xFFFF when the object has none (live: 335 of 3583)
    uint32_t flags;
    uint32_t user_flags;
    uint16_t flags2;
    uint16_t flags3;
    regenny::LTVector position;
    regenny::LTRotation rotation;
    float scale;
    // The object's SECOND identity, distinct from the handle: a unique index, live
    // 3248 distinct values in [0..3885] with no duplicates, and 0xFFFFFFFF on the
    // other 335. It is NOT a copy of the handle -- the two are unequal on all 3248
    // objects carrying both.
    //
    // Useful to a mod as a stable per-object key: unlike a pointer it survives being
    // logged, and unlike the handle it is unique across the whole object set rather
    // than per-table. Reuse after free is NOT ruled out, so it identifies an object
    // for the frame you read it, not forever.
    uint32_t slot_index;
};

// nullopt only when `obj` is null or the read faulted.
std::optional<ObjectInfo> object_info(const regenny::LTObject* obj);

// THE ENGINE'S OWN RENDER GATE, reproduced: `(flags & 1) && !(flags2 & 0x700)`.
//
// This is the question a mod actually has -- "will this be drawn?" -- and it is NOT
// the same as testing kVisible. The engine requires the flags2 clause too, so an
// object can carry kVisible and still be skipped. Anything toggling visibility should
// check here rather than assume one bit decides it.
//
// Note the direction of what is proven: the gate decides whether the object gets
// COLLECTED into the visibility structures. An object that passes can still fail to
// appear for reasons this cannot see (its volume reaching no sector, for instance --
// live, 40 objects pass the gate and hold no spatial entries).
std::optional<bool> is_renderable(const regenny::LTObject* obj);

// ---- THE ENGINE'S OWN ROTATION SETTER, FOR HOOKING ------------------------------------------------
//
// LTObject_SetRotation (FEAR2.exe 0x00420290). THE write path for an object's rotation, and the one that
// reaches the renderer -- found by a data breakpoint on the live camera object's rotation, which caught
// LTRotation_Copy and led here through the only callers that target an object's +0x20.
//
// Why this and not a field write: overriding the camera holder's fields does not reach the rendered view.
// Measured with the source field pinned to 0.00 degrees of drift, the applied pose and the camera object both
// sat 58.82 degrees from the override's intent, and forcing the applied pose too made the object WORSE
// (109.47). The object is not a copy of either -- it is written here, by the engine, through this function.
//
//     __thiscall, `this` = the LTObject
//     ONE 4-byte stack argument, a pointer to the quaternion (the single exit is `retn 4`)
//     copies it into this+0x20 via LTRotation_Copy, then fires a virtual notify with code 4
//
// So an x86 detour is __fastcall(this, edx_dummy, float* quat), and a consumer overriding a view replaces the
// QUATERNION THE CALLER PASSED rather than the field afterwards -- the same lesson as ApplyLookDelta, where
// writing the destination lost a race with the code below the call.
//
// Only three callers, so this is a narrow interception rather than a general-purpose API hook. 0 on a miss.
uintptr_t set_rotation_fn();

// LTObject_SetPosRot (FEAR2.exe 0x004202B6). The MOVE-AND-TURN path, and the likelier one for a camera: it
// sets position and rotation together, recomputes the world AABB, re-adds the object to the world tree and
// notifies with code 6.
//
// LTObject_SetRotation was ruled out by measurement rather than by reading: hooked live it fired 50,475 times
// in eight seconds and NOT ONCE on the player's camera object, so the camera does not travel that path.
//
//     __thiscall, `this` = the LTObject
//     ONE 4-byte stack argument (single exit is `retn 4`), pointing at SEVEN floats:
//         [0..2] position, [3..6] rotation quaternion (the copy source is arg + 0x0C)
//
// So a detour is __fastcall(this, edx_dummy, float* posrot), and a view override replaces posrot[3..6].
uintptr_t set_pos_rot_fn();

// THE ENGINE'S OTHER "RENDERABLE" PREDICATE, and the two are NOT the same test:
//
//   is_renderable()    (flags & 1) && !(flags2 & 0x700)          -- will it be DRAWN
//   is_tree_eligible() !(flags & 0x200) && (flags & 0x10C30)     -- will it be INDEXED
//
// The second is LTObject_IsRenderable (0x4200A0), and despite the engine's name for it, what
// it actually decides is SPATIAL INDEX MEMBERSHIP: it is the gate on every call to
// LTWorldTree_AddObject -- from SetPos, SetPosRot, SetDims_Notify and SetFlags. Different
// mask, different flag word clause, different purpose. Reproducing one and assuming it
// answers for the other would be wrong in both directions.
//
// WHY A CONSUMER WANTS IT: paired with WorldBSP::is_linked() it explains spatial staleness.
// is_tree_eligible() is whether the object SHOULD be in the tree; is_linked() is whether it
// IS. LTObject_SetPos writes an object's AABB unconditionally but relinks only when this
// predicate holds, so an object that moves while ineligible keeps its old node -- which is
// why 370 of 2142 indexed entries are stale while zero AABBs are.
//
// So: before trusting WorldBSP::objects_near() for something that moves, ask whether it is
// eligible. If it is not, the index will not have followed it.
std::optional<bool> is_tree_eligible(const regenny::LTObject* obj);

// Whether this is a SERVER object -- the engine's own concept, and its own test.
//
// This is not a name I chose. CLTClient::IsServerObject (dump 0x40991C) is, in its
// entirety, `*out = obj->handle != 0xFFFF`. So "has an engine handle" and "came from
// the server" are the SAME predicate as far as the engine is concerned, and the 335
// handle-less objects are exactly the client-created ones.
//
// That settles by engine code what an earlier pass could only infer from a
// population: the objects without a handle are all 278 particle systems plus a
// scattering of models and sprites, which LOOKED like client-side effects. The
// engine agrees.
//
// TWO CONSEQUENCES, both worth knowing:
//
//   * A false answer means the object cannot be passed to ANY ILT* entry point --
//     those take an HOBJECT and the handle table has no slot for it. A mod walking
//     the object lists meets these constantly and should skip them rather than
//     discover it one failed call at a time.
//   * A false answer also means the object is client-side: a local effect, not a
//     replicated entity. Anything that should act on "real" game entities wants this
//     filter regardless of the API question.
//
// slot_index agrees completely (present on exactly the same 3248 objects), so a
// caller need not check both.
std::optional<bool> is_server_object(const regenny::LTObject* obj);

// The object's COLOUR AND ALPHA -- the engine's per-object tint and transparency.
//
// Read straight out of the field CLTClient::GetObjectColor hands out. The byte order is
// the engine's: B, G, R, A ascending, so the packed dword reads 0xAARRGGBB. That order
// is NOT what the reference SDK declares (it lists red first), which is why `packed` is
// exposed alongside the components -- a caller passing the dword to something else
// should know what layout it is in.
//
// WHY A VR MOD CARES: alpha is how the engine already hides and fades things, so it is
// the least invasive way to make the player's own body or a blocking prop disappear --
// no geometry edits, no render hooks. Live 121 objects are genuinely translucent and 9
// worldmodels sit at alpha 0, so the mechanism is in active use rather than theoretical.
//
// The default is 0xFFFFFFFF: white, fully opaque. 3265 of 3583 objects are exactly that.
struct ObjectColor {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t a;
    // The whole field as the engine stores and returns it, 0xAARRGGBB.
    uint32_t packed;
};

// nullopt when `obj` is null or the read faulted.
//
// NOT PAIRED WITH A SETTER YET, deliberately. The write path is mapped -- SetObjectColor
// and SetObjectAlpha write the field and then notify the owner through its vtable, while
// SetObjectRGB writes and does NOT notify -- but this SDK is read-only so far, and
// adding the first mutator is a decision worth making on purpose rather than in passing.
std::optional<ObjectColor> object_color(const regenny::LTObject* obj);

// The object's COLLISION HALF-EXTENTS, as the engine reports them.
//
// ILTPhysics::GetObjectDims hands out exactly this field, and the engine derives an
// object's AABB as `position +/- dims`. Live all three components are non-negative on
// 3583/3583 objects -- which a half-extent must be -- with 1902 objects at all-zero
// (never sized) and a maximum of 13375 belonging to the level's own worldmodel.
//
// A VR mod wants this for the player's collision volume: how tall the body is, and how
// much room it needs. Note ALL-ZERO IS A REAL STATE, not a failure: sphere-culled
// objects never run SetDims, so they legitimately report nothing.
std::optional<regenny::LTVector> object_dims(const regenny::LTObject* obj);

// What this object is STANDING ON, and the surface it is standing on.
//
// The engine keeps a pair: the object beneath (usually the level's worldmodel) and a
// world node describing the actual surface. ILTPhysics::GetStandingOn reads both, gates
// on the lower object's type, and reports the surface height as `pos.y + dims.y`.
//
// For a VR mod this is ground contact -- whether the player is supported, by what, and
// how high the floor is. Live only one object in the sampled scene stands on anything,
// and it stands on the level geometry with a floor plane of normal (0,1,0).
struct StandingOn {
    // The object beneath. Never null in a returned value -- nullopt is returned instead
    // when nothing is being stood on, so a caller cannot mistake one for the other.
    const regenny::LTObject* object;
    // The surface height directly under this object, computed the engine's own way.
    float surface_height;
    // Whether a surface NODE was recorded too. The engine takes the plane from it; when
    // false it falls back to a flat (0,1,0) assumption, exactly as this does.
    bool has_node;
};

// nullopt when the object is null, stands on nothing, or the read faulted.
std::optional<StandingOn> standing_on(const regenny::LTObject* obj);

// ---- ATTACHMENTS -------------------------------------------------------------
//
// What is riding on this object, and where. For a VR mod this is the question behind
// "where is the weapon" and "what is on the player's back": FEAR 2 hangs a list of
// attachment records off every object that has children mounted on it (live, 139 of
// 3583 objects do, holding 362 records between them).
//
// One record per attached object, in the engine's own list order.
struct Attachment {
    // The attached object, already resolved through the engine's handle table using
    // the engine's own rule (slot tag must be live). NULLPTR IS A NORMAL RESULT: live,
    // 327 of 362 records resolve and 35 do not, because a record can name a handle
    // whose slot is not live. The engine's own walker checks this before touching the
    // child, and so should a caller.
    const regenny::LTObject* child;

    // The raw handle, kept even when the child does not resolve -- useful for logging
    // and for noticing that a specific attachment went stale.
    uint16_t child_handle;

    // WHICH ATTACH POINT the child rides, as the engine's UNIFIED SOCKET HANDLE: it
    // addresses the parent's SOCKETS first and falls through to skeleton NODES beyond
    // socket_count. nullopt when the parent is not a model or the record holds the
    // engine's sentinel -- both are the engine's own conditions, taken from
    // CLTCommonShared_GetAttachmentTransform, which tests exactly
    // `parent->type != OT_MODEL || handle == -1` before passing this to
    // ILTModel_GetSocketTransform.
    //
    // NAMED FOR THE SPACE IT LIVES IN, after two wrong names. It was `socket` (right by
    // luck, wrong by reasoning) and then `parent_node` (wrong outright): all 27 live
    // model-parent values fall inside node_count AND inside socket_count, so the range
    // proves nothing, and resolved as node indices they even yield plausible bone names.
    // What settled it was the child's actual position -- the player's handles 0 and 1 are
    // sockets `RightHand` and `LeftHand`, and each child sits exactly there.
    //
    // Resolve it the way the engine does, which is what socket_handle_transform() is for:
    //     auto skel = sdk::ModelSkeleton::from_object(parent);
    //     auto xf   = skel->socket_handle_transform(*a.socket_handle);   // world space
    // Do NOT pass it to node_name() or socket() directly: one of those is wrong for any
    // given value and both will happily return something.
    std::optional<size_t> socket_handle;

    // The record's own offset transform. Live these are all zero and identity
    // respectively, so they are UNEXERCISED in the sampled state -- do not assume they
    // are always neutral, and do not assume they are meaningful either.
    regenny::LTVector offset_position;
    regenny::LTRotation offset_rotation;
};

// Empty when the object has no attachments, is null, or the walk faulted -- a caller
// that needs to tell those apart does not exist yet, and inventing the distinction
// would mean inventing a failure mode.
//
// The walk is BOUNDED AND CYCLE-CHECKED even though live lists are short (1..16 long,
// no cycles over 139 lists): this reads a live mutating structure, and a torn list
// must return a short answer rather than hang the caller's frame.
std::vector<Attachment> attachments(const regenny::LTObject* obj);


// ---- WORLD MODELS: the level's own solid, moving geometry ---------------------
//
// A world model is a brush: doors, elevators, moving platforms, breakables, the level
// shell itself. They are the SECOND most numerous object type live (1476 of 3583), and
// they are what a VR player collides with, rides and teleports onto -- so "where is this
// brush, and where is a world point relative to it" is a question a mod asks constantly.
//
// THE ENGINE KEEPS BOTH DIRECTIONS. A world model carries its local->world rigid
// transform AND the inverse, side by side (LTWorldModelObject.world_transform @0xDC and
// inverse_transform @0x10C, each a row-major 3x4). That is why this API exposes two
// functions and no matrix: a caller wanting brush-local coordinates uses the engine's own
// inverse rather than inverting anything, which is both faster and immune to the
// row/column mistake that inverting by hand invites.
//
// TWO THINGS TO EXPECT, both measured through this API over 1947 live brushes (1473
// world models + 474 cameras, which inherit the pair because OT_CAMERA derives from
// OT_WORLDMODEL):
//
//  * A ROUND TRIP IS NOT BIT-EXACT. Level coordinates run to thousands of units, and a
//    float32 point through two 3x4 matrices loses about 0.1 units. 1920 of 1947 returned
//    within 0.05; raising the bound to 0.5 accounts for 15 more. If you compare positions
//    for equality, use a tolerance in that range, not zero.
//
//  * ABOUT 12 BRUSHES CARRY A PAIR THAT IS NOT AN INVERSE, worst case 49.8 units of
//    round-trip error. Both matrices are individually valid -- scale 1.0 and det exactly
//    1.0 on every one of them -- so this is not a scaled or skewed brush; the two simply
//    disagree. Ask brush_transform_quality(obj)->trustworthy() before relying on one; an
//    earlier version of this note told the caller to round-trip a probe point by hand,
//    which is what that function now does properly.
//
// Cameras are the clean case: the pair holds on 474/474.
//
// nullopt when `obj` is null, is not a world model or camera, or the read faulted. The
// TYPE CHECK IS DELIBERATE: the matrices live past LTObject's end, so calling these on an
// OT_MODEL would read a different class's fields and return a plausible wrong point.
std::optional<regenny::LTVector> world_to_brush(const regenny::LTObject* obj,
                                                const regenny::LTVector& world_point);

std::optional<regenny::LTVector> brush_to_world(const regenny::LTObject* obj,
                                                const regenny::LTVector& brush_point);

// ---- THE MATRICES THEMSELVES, and how much to trust them ----------------------
//
// world_to_brush/brush_to_world above cover the common case. These expose what they are
// built on, because a mod doing its own maths -- feeding a matrix to a renderer, chaining
// several transforms, uploading one as a shader constant -- needs the matrix, not a
// point-at-a-time helper.
//
// THIS LOGIC USED TO BE TRAPPED INSIDE A TEST. CClientMgr::check_transforms computed all
// of it and threw everything away except three counters, so a consumer could learn that
// "1450 of 1473 pairs are exact" and nothing about the object in front of it. The check
// now aggregates these functions instead of hiding its own copy.

// A rigid transform in the engine's own layout: row-major 3x4, translation in column 3,
// so row r is m[r*4 .. r*4+3].
struct Matrix34 {
    float m[12];
};

// R(q) as the top-left 3x3 with a zero translation. PURE MATH: no engine read, no thread or state
// requirement -- so it is usable on any LTRotation, including ones you built yourself, because it
// applies the ENGINE'S scale of 2/|q|^2 (LTRotation_ToMatrix3x4, 0x40FC87) rather than a hardcoded
// 2. That distinction is invisible for unit quaternions and wrong for every other, and this
// function previously claimed the general case while implementing the unit-only one.
//
// It DOES have a failure mode, despite being pure: nullopt for a non-finite or zero-norm
// quaternion, which is the only input that describes no rotation at all.
std::optional<Matrix34> rotation_matrix(const regenny::LTRotation& q);

// THE QUATERNION PRODUCT, transcribed from the game client's own (gameclient.dll 0x100016B0), which
// CPlayerCamera's load path uses to derive one stored rotation from another.
//
// It is the standard Hamilton product, verified term by term against the decompiled arithmetic rather than
// assumed: w = aw*bw - dot(av, bv), and the vector part aw*bv + bw*av + cross(av, bv).
//
// THE ORDER CONVENTION IS THE PART THAT MATTERS TO A CONSUMER, and it is established by measurement rather
// than by convention-lawyering: the suite builds R(a*b) and compares it against R(a)*R(b) and R(b)*R(a), and
// only one can match. It is R(a)*R(b) -- so with this engine's column-vector matrices, multiply_rotations(a, b)
// means "apply b FIRST, then a". A VR consumer composing a headset rotation onto the game's camera rotation
// needs that order right or the result is mirrored in a way that looks almost plausible while turning the
// wrong way.
//
// NOT NORMALISED, deliberately. The product of two unit quaternions is unit to within float error, but the
// product of non-unit ones carries |a||b| -- and sdk::rotation_matrix divides by |q|^2, so a matrix built
// from the result is still correct while a chain of further products drifts. See the note on
// SceneCamera::invert_transform, which makes the same point from the other direction.
regenny::LTRotation multiply_rotations(const regenny::LTRotation& a, const regenny::LTRotation& b);

// The world model's cached local->world transform, and the inverse the engine keeps
// beside it. Same type gate as world_to_brush: WorldModel or Camera only, since the
// fields live past LTObject's end.
std::optional<Matrix34> brush_transform(const regenny::LTObject* obj);
std::optional<Matrix34> brush_inverse_transform(const regenny::LTObject* obj);

// HOW TRUSTWORTHY THIS OBJECT'S PAIR IS, which is a real question and not a diagnostic:
// live, 12 of 1947 brushes carry a forward/inverse pair that is NOT an inverse (worst
// case 49.8 units of round-trip error) even though both matrices are individually valid
// rigid transforms -- scale 1.0 and determinant exactly 1.0 on every one of them.
//
// A mod that cares about accuracy asks this before trusting a transform, rather than
// round-tripping a probe point itself as the documentation used to suggest.
struct TransformQuality {
    // Worst absolute disagreement between the matrix's 3x3 and R(object rotation). The
    // engine recomputes the matrix rather than deriving it, so these are two independent
    // expressions of one orientation.
    float rotation_error;
    // Worst absolute disagreement between the inverse's 3x3 and the transform's transpose.
    // For a rotation, transpose IS inverse, so this measures the pair against itself.
    float inverse_error;
    // Worst disagreement between the inverse's translation and -R^T * t, the translation a
    // true inverse must carry.
    float translation_error;
    // det of the 3x3. Exactly 1.0 for a proper rigid transform; a reflection gives -1 and
    // a scaled matrix something else.
    float determinant;

    // The three verdicts, with the tolerances that were measured rather than chosen --
    // see MAPPING_WORKFLOW on picking a tolerance against the type and coordinate range.
    bool rotation_matches;  // rotation_error < 0.002
    bool inverse_exact;     // inverse_error < 0.002 && translation_error < 0.05
    bool determinant_unit;  // |determinant - 1| < 0.01

    // The one a caller usually wants: all three hold, so both matrices agree with the
    // object's rotation and with each other.
    bool trustworthy() const {
        return rotation_matches && inverse_exact && determinant_unit;
    }
};

// nullopt on the same conditions as brush_transform.
std::optional<TransformQuality> brush_transform_quality(const regenny::LTObject* obj);


// ---- CULL VOLUMES: what shape the engine thinks this object occupies ------------
//
// THE BOUNDS QUESTION, and the one a VR mod asks constantly: how big is this thing, and
// where does it end? Needed for interaction ranges, grab detection, teleport targeting and
// any culling a mod does itself.
//
// THIS WAS TRAPPED INSIDE A TEST TOO. CClientMgr::check_spatial_records knew the whole
// per-type rule -- it recomputed every object's volume from typed fields to compare against
// the engine's stored copy -- and returned only "volume_matched" counters. The rule is the
// valuable part, so it lives here now and the check compares the two functions below.
//
// THE RULE IS THE ENGINE'S OWN VIRTUAL, not an inference from stored values. Object vtable
// SLOT 2 is the cull-volume producer, and its RETURN CODE is the shape tag:
//
//   0 -> no volume     1 -> sphere     2 -> box
//
//   OT_NORMAL          returns 0 outright
//   OT_MODEL           returns 1 on both its paths
//   OT_WORLDMODEL      LTObject_GetCullVolume_AABB, shared with OT_CAMERA -> 2
//   OT_SPRITE          tests its kind, returns 2 or 1
//   OT_PARTICLESYSTEM  returns cull_volume_type ITSELF, so the field IS the tag
//
// Reading those five virtuals is what promoted this from "matches what the engine stored" to
// "is what the engine computes" -- and it immediately found a discrepancy that comparing
// against stored values could not: see the OT_MODEL sphere note in the .cpp, where an extra
// `* scale` was invisible because scale is 1.0 on every live object.
//
// THE SHAPE FOLLOWS FROM THE TYPE, and for two types from a per-object field:
//
//   OT_NORMAL              never has one
//   any type, flags3 0x80  suppressed -- the engine stores zeros
//   OT_WORLDMODEL/CAMERA   BOX, from the object's own aabb_min/aabb_max
//   OT_MODEL               SPHERE; centre is sphere_center when sphere_source is set,
//                          otherwise the object's position with radius vis_radius * scale
//   OT_SPRITE              kind 3/4/7/9 -> BOX, otherwise SPHERE at position
//   OT_PARTICLESYSTEM      cull_volume_type 1 -> SPHERE, else BOX; both stored as
//                          OBJECT-LOCAL offsets, so position is added in
enum class CullShape {
    None,    // no volume: OT_NORMAL, or suppressed by flags3 bit 0x80
    Sphere,
    Box,
};

struct CullVolume {
    CullShape shape;
    // Sphere only. Meaningless when shape is Box or None.
    regenny::LTVector center;
    float radius;
    // Box only, in WORLD space -- the particle-system offsets are resolved against the
    // object's position before you see them, so every shape here is comparable directly.
    regenny::LTVector min;
    regenny::LTVector max;
};

// WHAT THE ENGINE ACTUALLY CULLS AGAINST: the copy it wrote on the object's spatial
// record. This is the authoritative one -- if a mod wants to agree with the engine's own
// visibility decisions, it reads this.
//
// nullopt when `obj` is null, has no spatial record, or the read faulted. A CullShape of
// None is a normal ANSWER, not a failure -- and in that case `min`/`max` still carry the
// six floats the engine left on the record, so a caller can confirm they are zeroed
// instead of trusting this function's classification.
std::optional<CullVolume> cull_volume(const regenny::LTObject* obj);

// THE SAME VOLUME RECOMPUTED from the object's own typed fields, by the rule above.
//
// Two reasons a caller wants this rather than the stored copy: it is correct for an object
// whose record has not been refreshed this frame, and comparing the two is how you tell
// that has happened. The engine's copy and this agree on the overwhelming majority live --
// the exact fraction is what check_spatial_records reports.
std::optional<CullVolume> computed_cull_volume(const regenny::LTObject* obj);

// IS THE ENGINE'S STORED VOLUME STILL CURRENT? Compares the two above and reports whether
// they agree, which is how a caller detects a spatial record the engine has not refreshed.
// Live 3574 of 3583 objects agree (2654 by volume, 920 suppressed-and-zeroed).
//
// THE TOLERANCE IS RELATIVE, and that is not a detail: `|d| <= 0.01 + 0.0001*|b|`. Level
// coordinates reach five figures, where float32 spacing alone exceeds a fixed 0.01 -- a
// naive absolute epsilon reports correct data as mismatched. This function exists partly so
// that tolerance lives in ONE place instead of being re-derived (wrongly) at each call
// site, which is exactly what happened when this comparison was first lifted out of
// check_spatial_records.
//
// nullopt when `obj` is null, has no spatial record, or a read faulted.
std::optional<bool> cull_volume_is_current(const regenny::LTObject* obj);


// ---- WORLD BOUNDS: the AABB and radius the engine culls with ------------------
//
// object_dims() already gives the half-extents. These give the two things the engine
// DERIVES from them and stores alongside, which is what its own culling reads:
//
//   aabb_min/aabb_max  written by LTObject_SetWorldAABB as `position -/+ dims`
//   radius             |dims| + 0.1 once SetDims has run
//
// Both were computed inside CClientMgr::check_object_geometry and thrown away. A mod wants
// them directly: an AABB is the cheapest broad-phase test there is, and the radius is what
// a distance check should compare against rather than a number of its own choosing.

struct WorldAABB {
    regenny::LTVector min;
    regenny::LTVector max;
};

// The AABB as the engine STORES it -- available on every object type, unlike
// cull_volume() which is world-model/camera only. nullopt when `obj` is null or the read
// faulted.
std::optional<WorldAABB> world_aabb(const regenny::LTObject* obj);

// IS THAT AABB CURRENT? True when it equals `position -/+ dims`, the identity
// LTObject_SetWorldAABB establishes.
//
// WORTH ASKING FOR THE SAME REASON THE SPATIAL INDEX IS: the engine writes this from
// LTObject_SetPos and friends, so an object moved by another route carries an AABB around
// its old position. That is not hypothetical here -- 370 of 2142 indexed objects already
// have a stale world-tree entry for exactly that reason (see WorldBSP::index_is_current).
//
// The comparison uses a RELATIVE tolerance: level coordinates reach five figures, where a
// fixed epsilon sits below float32 spacing and reports correct data as wrong.
std::optional<bool> world_aabb_is_current(const regenny::LTObject* obj);

// The engine's bounding radius, and WHICH OF ITS TWO STATES it is in. The distinction is
// load-bearing rather than trivia: the base constructor zeroes dims and radius together, so
// an object SetDims never ran on sits at (0, 0) -- which is correct-but-unsized, NOT a
// radius that should have been 0.1. Live 2126 objects are sized and 1457 unsized, with none
// in neither state.
struct BoundingRadius {
    float radius;    // as stored
    bool from_dims;  // radius == |dims| + 0.1, i.e. SetDims has run
    bool unsized;    // dims and radius are all zero: the constructor's state
};

// nullopt when `obj` is null or the read faulted. An object in NEITHER state returns the
// stored radius with both flags false -- that is a real answer and worth noticing, since
// live nothing is in that position.
std::optional<BoundingRadius> bounding_radius(const regenny::LTObject* obj);

}  // namespace sdk
