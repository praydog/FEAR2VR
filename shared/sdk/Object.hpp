#pragma once

#include <cstdint>
#include <optional>

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

}  // namespace sdk
