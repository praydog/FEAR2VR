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

// R(q) as the top-left 3x3 with a zero translation. PURE MATH, no engine read and no
// failure mode -- useful on any LTRotation, including ones you built yourself.
Matrix34 rotation_matrix(const regenny::LTRotation& q);

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

}  // namespace sdk
