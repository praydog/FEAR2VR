#include "Object.hpp"

#include <windows.h>
#include <cmath>

#include <utility/Seh.hpp>

#include "CClientMgr.hpp"
#include "regenny/regenny/LTAttachment.hpp"
// The cull rule reads per-type fields, so it needs the concrete object classes.
#include "regenny/regenny/LTModelObject.hpp"
#include "regenny/regenny/LTSpriteObject.hpp"
#include "regenny/regenny/LTParticleSystemObject.hpp"
#include "regenny/regenny/LTSpatialRecord.hpp"

namespace sdk {

namespace {

// POD mirror of ObjectInfo so the SEH guard never shares a frame with a type that
// unwinds. The `ok` flag rather than a bool return keeps the copy in one place.
struct InfoRaw {
    uint8_t kind;
    uint16_t handle;
    uint32_t flags;
    uint32_t user_flags;
    uint16_t flags2;
    uint16_t flags3;
    float pos[3];
    float rot[4];
    float scale;
    uint32_t slot_index;
    bool ok;
};

InfoRaw seh_read_info(const regenny::LTObject* obj) {
    InfoRaw r{};
    KANANLIB_SEH_TRY {
        r.kind = static_cast<uint8_t>(obj->type);
        r.handle = obj->handle;
        r.flags = obj->flags;
        r.user_flags = obj->user_flags;
        r.flags2 = obj->flags2;
        r.flags3 = obj->flags3;
        r.pos[0] = obj->position.x;
        r.pos[1] = obj->position.y;
        r.pos[2] = obj->position.z;
        r.rot[0] = obj->rotation.x;
        r.rot[1] = obj->rotation.y;
        r.rot[2] = obj->rotation.z;
        r.rot[3] = obj->rotation.w;
        r.scale = obj->scale;
        r.slot_index = obj->slot_index;
        r.ok = true;
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        r.ok = false;
    }
    return r;
}

// Returns 1 / 0 for the gate, or -1 on fault.
int seh_renderable(const regenny::LTObject* obj) {
    int result = -1;
    KANANLIB_SEH_TRY {
        // The engine's clause, verbatim: visible AND none of the flags2 suppressor
        // bits. Both halves matter -- see the header.
        const bool vis = (obj->flags & object_flags::kVisible) != 0;
        const bool suppressed = (obj->flags2 & 0x700) != 0;
        result = (vis && !suppressed) ? 1 : 0;
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        result = -1;
    }
    return result;
}

}  // namespace

std::optional<ObjectInfo> object_info(const regenny::LTObject* obj) {
    if (obj == nullptr) {
        return std::nullopt;
    }
    const InfoRaw r = seh_read_info(obj);
    if (!r.ok) {
        return std::nullopt;
    }
    ObjectInfo out{};
    out.kind = static_cast<ObjectKind>(r.kind);
    out.handle = r.handle;
    out.flags = r.flags;
    out.user_flags = r.user_flags;
    out.flags2 = r.flags2;
    out.flags3 = r.flags3;
    out.position.x = r.pos[0];
    out.position.y = r.pos[1];
    out.position.z = r.pos[2];
    out.rotation.x = r.rot[0];
    out.rotation.y = r.rot[1];
    out.rotation.z = r.rot[2];
    out.rotation.w = r.rot[3];
    out.scale = r.scale;
    out.slot_index = r.slot_index;
    return out;
}

std::optional<bool> is_renderable(const regenny::LTObject* obj) {
    if (obj == nullptr) {
        return std::nullopt;
    }
    const int r = seh_renderable(obj);
    if (r < 0) {
        return std::nullopt;
    }
    return r != 0;
}

std::optional<bool> is_server_object(const regenny::LTObject* obj) {
    if (obj == nullptr) {
        return std::nullopt;
    }
    const InfoRaw r = seh_read_info(obj);
    if (!r.ok) {
        return std::nullopt;
    }
    // The engine's own test, verbatim: CLTClient::IsServerObject (0x40991C) performs
    // exactly this comparison and nothing else, so this is not an approximation of
    // the engine's notion of a server object -- it IS it.
    return r.handle != 0xFFFF;
}

}  // namespace sdk

namespace sdk {

namespace {

// POD mirror of one record, copied out under the guard. The walk hands back the NEXT
// pointer too, so the loop itself never dereferences engine memory outside a guard.
struct AttachRaw {
    uint16_t child_handle;
    uint16_t parent_handle;
    uint32_t socket_handle;
    const void* next;
    float pos[3];
    float rot[4];
    bool ok;
};

AttachRaw seh_read_record(const void* rec) {
    AttachRaw r{};
    KANANLIB_SEH_TRY {
        const auto* a = static_cast<const regenny::LTAttachment*>(rec);
        r.child_handle = a->child_handle;
        r.parent_handle = a->parent_handle;
        r.socket_handle = a->socket_handle;
        r.next = a->next;
        r.pos[0] = a->offset_position.x;
        r.pos[1] = a->offset_position.y;
        r.pos[2] = a->offset_position.z;
        r.rot[0] = a->offset_rotation.x;
        r.rot[1] = a->offset_rotation.y;
        r.rot[2] = a->offset_rotation.z;
        r.rot[3] = a->offset_rotation.w;
        r.ok = true;
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        r.ok = false;
    }
    return r;
}

const void* seh_list_head(const regenny::LTObject* obj, bool* is_model) {
    const void* head = nullptr;
    KANANLIB_SEH_TRY {
        head = obj->attachments;
        *is_model = obj->type == regenny::LTObjectType::OT_MODEL;
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    return head;
}

// THE ENGINE'S sentinel, from its own comparison rather than from our sample:
// CLTCommonShared_GetAttachmentTransform tests `handle == -1`. An earlier version of this
// constant was 127, taken from 335/335 non-model records holding that value -- but those
// records are never read, because the engine short-circuits on `type != OT_MODEL` first.
// Live NO record holds -1, so this branch is unexercised in the sampled state; it is here
// because the engine has it, not because we measured it.
constexpr uint32_t kNoSocketHandle = 0xFFFFFFFFu;

// Live lists are 1..16 long. This bound exists for a TORN list, not a long one.
constexpr size_t kMaxRecords = 64;

} // namespace

std::vector<Attachment> attachments(const regenny::LTObject* obj) {
    std::vector<Attachment> out;
    if (obj == nullptr) {
        return out;
    }
    bool is_model = false;
    // Fetched ONCE outside the loop: it cannot change under us mid-walk, and calling
    // it per record would be a pointless re-resolve per attachment.
    CClientMgr* mgr = CClientMgr::get();
    const void* cur = seh_list_head(obj, &is_model);
    // Cycle guard over the record addresses. A vector scan is right here: lists are
    // short, so a set would cost more than it saves.
    const void* seen[kMaxRecords] = {};
    size_t n = 0;
    while (cur != nullptr && n < kMaxRecords) {
        for (size_t i = 0; i < n; ++i) {
            if (seen[i] == cur) {
                return out;  // torn or cyclic: hand back what is certain
            }
        }
        seen[n] = cur;
        const AttachRaw r = seh_read_record(cur);
        if (!r.ok) {
            return out;
        }
        Attachment a{};
        a.child_handle = r.child_handle;
        // Resolution needs the live manager; without it a caller still gets the handles
        // and socket handles, which is more useful than an empty answer.
        a.child = mgr != nullptr ? mgr->object_from_handle(r.child_handle) : nullptr;
        // Both conditions are the ENGINE's, copied from
        // CLTCommonShared_GetAttachmentTransform: a non-model parent never yields a handle
        // (the type test short-circuits before the field is read), and -1 means "none".
        if (is_model && r.socket_handle != kNoSocketHandle) {
            a.socket_handle = static_cast<size_t>(r.socket_handle);
        }
        a.offset_position.x = r.pos[0];
        a.offset_position.y = r.pos[1];
        a.offset_position.z = r.pos[2];
        a.offset_rotation.x = r.rot[0];
        a.offset_rotation.y = r.rot[1];
        a.offset_rotation.z = r.rot[2];
        a.offset_rotation.w = r.rot[3];
        out.push_back(a);
        ++n;
        cur = r.next;
    }
    return out;
}

} // namespace sdk

namespace sdk {

namespace {

struct StandRaw {
    float dims[3];
    const void* standing_on;
    const void* node;
    float surface_height;
    bool dims_ok;
    bool ok;
};

// Both the object's own dims and, if it stands on something, that object's position and
// dims -- in ONE guard, because the surface height is computed from two of its fields
// and reading them separately could straddle a move.
StandRaw seh_stand(const regenny::LTObject* obj) {
    StandRaw r{};
    KANANLIB_SEH_TRY {
        r.dims[0] = obj->dims.x;
        r.dims[1] = obj->dims.y;
        r.dims[2] = obj->dims.z;
        r.dims_ok = true;
        const auto* under = obj->standing_on;
        if (under != nullptr) {
            r.standing_on = under;
            r.node = obj->standing_on_node;
            // The engine's own expression for the surface height.
            r.surface_height = under->position.y + under->dims.y;
        }
        r.ok = true;
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        r.ok = false;
    }
    return r;
}

}  // namespace

std::optional<regenny::LTVector> object_dims(const regenny::LTObject* obj) {
    if (obj == nullptr) {
        return std::nullopt;
    }
    const StandRaw r = seh_stand(obj);
    if (!r.ok || !r.dims_ok) {
        return std::nullopt;
    }
    regenny::LTVector out{};
    out.x = r.dims[0];
    out.y = r.dims[1];
    out.z = r.dims[2];
    return out;
}

std::optional<StandingOn> standing_on(const regenny::LTObject* obj) {
    if (obj == nullptr) {
        return std::nullopt;
    }
    const StandRaw r = seh_stand(obj);
    if (!r.ok || r.standing_on == nullptr) {
        return std::nullopt;
    }
    StandingOn out{};
    out.object = static_cast<const regenny::LTObject*>(r.standing_on);
    out.surface_height = r.surface_height;
    out.has_node = r.node != nullptr;
    return out;
}

}  // namespace sdk

namespace sdk {

namespace {

struct ColorRaw {
    uint8_t b, g, r, a;
    bool ok;
};

// All four bytes in ONE guarded read. Reading them separately would let a fade advance
// between components and hand back a colour the object never had.
ColorRaw seh_color(const regenny::LTObject* obj) {
    ColorRaw c{};
    KANANLIB_SEH_TRY {
        c.b = obj->color_b;
        c.g = obj->color_g;
        c.r = obj->color_r;
        c.a = obj->color_a;
        c.ok = true;
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        c.ok = false;
    }
    return c;
}

}  // namespace

std::optional<ObjectColor> object_color(const regenny::LTObject* obj) {
    if (obj == nullptr) {
        return std::nullopt;
    }
    const ColorRaw c = seh_color(obj);
    if (!c.ok) {
        return std::nullopt;
    }
    ObjectColor out{};
    out.r = c.r;
    out.g = c.g;
    out.b = c.b;
    out.a = c.a;
    // Repacked in the engine's own layout so a caller can hand it straight back to
    // anything that expects what GetObjectColor returns.
    out.packed = (static_cast<uint32_t>(c.a) << 24) | (static_cast<uint32_t>(c.r) << 16) |
                 (static_cast<uint32_t>(c.g) << 8) | static_cast<uint32_t>(c.b);
    return out;
}

}  // namespace sdk

namespace sdk {

namespace {

// One 3x4 row-major matrix, copied out under the guard.
struct Mat34Raw {
    float m[12];
    bool ok;
};

// The two matrices sit past LTObject's end, on LTWorldModelObject. Reading them requires
// the object to BE one, which the caller establishes by type before we get here.
Mat34Raw seh_read_brush_matrix(const regenny::LTObject* obj, bool inverse) {
    Mat34Raw r{};
    KANANLIB_SEH_TRY {
        const auto* base = reinterpret_cast<const uint8_t*>(obj);
        const auto* src = reinterpret_cast<const float*>(base + (inverse ? 0x10C : 0xDC));
        for (size_t i = 0; i < 12; ++i) {
            r.m[i] = src[i];
        }
        r.ok = true;
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        return r;
    }
    return r;
}

// Apply a row-major 3x4: out_r = dot(row_r.xyz, p) + row_r.w.
regenny::LTVector apply_mat34(const float (&m)[12], const regenny::LTVector& p) {
    regenny::LTVector out{};
    out.x = m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3];
    out.y = m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7];
    out.z = m[8] * p.x + m[9] * p.y + m[10] * p.z + m[11];
    return out;
}

// Both entry points share the same gate: the matrices only exist on the world-model
// class and on camera, which derives from it.
std::optional<regenny::LTVector> brush_apply(const regenny::LTObject* obj,
                                             const regenny::LTVector& p, bool inverse) {
    if (obj == nullptr) {
        return std::nullopt;
    }
    const auto info = object_info(obj);
    if (!info.has_value()) {
        return std::nullopt;
    }
    if (info->kind != ObjectKind::WorldModel && info->kind != ObjectKind::Camera) {
        return std::nullopt;
    }
    const auto r = seh_read_brush_matrix(obj, inverse);
    if (!r.ok) {
        return std::nullopt;
    }
    return apply_mat34(r.m, p);
}

}  // namespace

std::optional<regenny::LTVector> world_to_brush(const regenny::LTObject* obj,
                                                const regenny::LTVector& world_point) {
    return brush_apply(obj, world_point, /*inverse=*/true);
}

std::optional<regenny::LTVector> brush_to_world(const regenny::LTObject* obj,
                                                const regenny::LTVector& brush_point) {
    return brush_apply(obj, brush_point, /*inverse=*/false);
}

}  // namespace sdk

namespace sdk {

namespace {

// The matrix offsets are NOT repeated here: seh_read_brush_matrix above already owns them,
// and two copies of an offset is exactly how one of them goes stale.

bool brush_capable(const regenny::LTObject* obj, ObjectInfo* info_out) {
    if (obj == nullptr) {
        return false;
    }
    const auto info = object_info(obj);
    if (!info.has_value()) {
        return false;
    }
    if (info->kind != ObjectKind::WorldModel && info->kind != ObjectKind::Camera) {
        return false;
    }
    if (info_out != nullptr) {
        *info_out = *info;
    }
    return true;
}

float abs_f(float v) {
    return v < 0.0f ? -v : v;
}

}  // namespace

Matrix34 rotation_matrix(const regenny::LTRotation& q) {
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    Matrix34 out{};
    out.m[0] = 1.0f - 2.0f * (y * y + z * z);
    out.m[1] = 2.0f * (x * y - w * z);
    out.m[2] = 2.0f * (x * z + w * y);
    out.m[4] = 2.0f * (x * y + w * z);
    out.m[5] = 1.0f - 2.0f * (x * x + z * z);
    out.m[6] = 2.0f * (y * z - w * x);
    out.m[8] = 2.0f * (x * z - w * y);
    out.m[9] = 2.0f * (y * z + w * x);
    out.m[10] = 1.0f - 2.0f * (x * x + y * y);
    return out;
}

std::optional<Matrix34> brush_transform(const regenny::LTObject* obj) {
    if (!brush_capable(obj, nullptr)) {
        return std::nullopt;
    }
    const auto r = seh_read_brush_matrix(obj, /*inverse=*/false);
    if (!r.ok) {
        return std::nullopt;
    }
    Matrix34 out{};
    for (size_t i = 0; i < 12; ++i) {
        out.m[i] = r.m[i];
    }
    return out;
}

std::optional<Matrix34> brush_inverse_transform(const regenny::LTObject* obj) {
    if (!brush_capable(obj, nullptr)) {
        return std::nullopt;
    }
    const auto r = seh_read_brush_matrix(obj, /*inverse=*/true);
    if (!r.ok) {
        return std::nullopt;
    }
    Matrix34 out{};
    for (size_t i = 0; i < 12; ++i) {
        out.m[i] = r.m[i];
    }
    return out;
}

std::optional<TransformQuality> brush_transform_quality(const regenny::LTObject* obj) {
    ObjectInfo info{};
    if (!brush_capable(obj, &info)) {
        return std::nullopt;
    }
    const auto fwd = brush_transform(obj);
    const auto inv = brush_inverse_transform(obj);
    if (!fwd.has_value() || !inv.has_value()) {
        return std::nullopt;
    }
    const Matrix34 R = rotation_matrix(info.rotation);
    const float* M = fwd->m;
    const float* I = inv->m;

    TransformQuality q{};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            const float dr = abs_f(M[r * 4 + c] - R.m[r * 4 + c]);
            if (dr > q.rotation_error) {
                q.rotation_error = dr;
            }
            // transpose IS inverse for a rotation, so compare the inverse against it
            const float di = abs_f(I[r * 4 + c] - M[c * 4 + r]);
            if (di > q.inverse_error) {
                q.inverse_error = di;
            }
        }
    }
    // A true inverse's translation is -R^T * t.
    for (int r = 0; r < 3; ++r) {
        const float expect = -(M[0 * 4 + r] * M[0 * 4 + 3] + M[1 * 4 + r] * M[1 * 4 + 3] +
                               M[2 * 4 + r] * M[2 * 4 + 3]);
        const float dt = abs_f(I[r * 4 + 3] - expect);
        if (dt > q.translation_error) {
            q.translation_error = dt;
        }
    }
    q.determinant = M[0] * (M[5] * M[10] - M[6] * M[9]) -
                    M[1] * (M[4] * M[10] - M[6] * M[8]) +
                    M[2] * (M[4] * M[9] - M[5] * M[8]);

    q.rotation_matches = q.rotation_error < 0.002f;
    q.inverse_exact = q.inverse_error < 0.002f && q.translation_error < 0.05f;
    q.determinant_unit = abs_f(q.determinant - 1.0f) < 0.01f;
    return q;
}

}  // namespace sdk

namespace sdk {

namespace {

// POD mirror of everything the cull rule needs, copied out under one guard. Which of
// these fields is meaningful depends on `kind`, exactly as the rule does.
struct CullRaw {
    uint8_t kind;
    uint16_t flags3;
    float pos[3];
    float scale;
    float aabb_min[3];
    float aabb_max[3];
    // OT_MODEL
    uint16_t sphere_source;
    float sphere_center[3];
    float vis_radius;
    // OT_SPRITE
    uint8_t sprite_kind;
    float sprite_aabb_min[3];
    float sprite_aabb_max[3];
    float sprite_radius;
    // OT_PARTICLESYSTEM
    uint32_t ps_volume_type;
    float ps_sphere_offset[3];
    float ps_sphere_radius;
    float ps_min_offset[3];
    float ps_max_offset[3];
    // the engine's stored copy
    bool has_record;
    float stored[6];
    bool ok;
    bool stored_is_sphere = false;
};

CullRaw seh_read_cull(const regenny::LTObject* obj) {
    CullRaw r{};
    KANANLIB_SEH_TRY {
        r.kind = static_cast<uint8_t>(obj->type);
        r.flags3 = obj->flags3;
        r.pos[0] = obj->position.x;
        r.pos[1] = obj->position.y;
        r.pos[2] = obj->position.z;
        r.scale = obj->scale;
        r.aabb_min[0] = obj->aabb_min.x;
        r.aabb_min[1] = obj->aabb_min.y;
        r.aabb_min[2] = obj->aabb_min.z;
        r.aabb_max[0] = obj->aabb_max.x;
        r.aabb_max[1] = obj->aabb_max.y;
        r.aabb_max[2] = obj->aabb_max.z;
        if (r.kind == 1) {
            const auto* m = reinterpret_cast<const regenny::LTModelObject*>(obj);
            r.sphere_source = m->sphere_source;
            r.sphere_center[0] = m->sphere_center.x;
            r.sphere_center[1] = m->sphere_center.y;
            r.sphere_center[2] = m->sphere_center.z;
            r.vis_radius = m->vis_radius;
        } else if (r.kind == 3) {
            const auto* s = reinterpret_cast<const regenny::LTSpriteObject*>(obj);
            r.sprite_kind = s->kind;
            r.sprite_radius = s->radius;
            // THE SPRITE'S OWN AABB (+0x120/+0x12C), which is NOT LTObject's at
            // +0x48/+0x54. Using the base pair here reported 9 boxed sprites as stale
            // volumes -- the two fields have the same name and different meanings.
            r.sprite_aabb_min[0] = s->aabb_min.x;
            r.sprite_aabb_min[1] = s->aabb_min.y;
            r.sprite_aabb_min[2] = s->aabb_min.z;
            r.sprite_aabb_max[0] = s->aabb_max.x;
            r.sprite_aabb_max[1] = s->aabb_max.y;
            r.sprite_aabb_max[2] = s->aabb_max.z;
        } else if (r.kind == 6) {
            const auto* p = reinterpret_cast<const regenny::LTParticleSystemObject*>(obj);
            r.ps_volume_type = p->cull_volume_type;
            r.ps_sphere_offset[0] = p->sphere_offset.x;
            r.ps_sphere_offset[1] = p->sphere_offset.y;
            r.ps_sphere_offset[2] = p->sphere_offset.z;
            r.ps_sphere_radius = p->sphere_radius;
            r.ps_min_offset[0] = p->aabb_min_offset.x;
            r.ps_min_offset[1] = p->aabb_min_offset.y;
            r.ps_min_offset[2] = p->aabb_min_offset.z;
            r.ps_max_offset[0] = p->aabb_max_offset.x;
            r.ps_max_offset[1] = p->aabb_max_offset.y;
            r.ps_max_offset[2] = p->aabb_max_offset.z;
        }
        if (const auto* rec = obj->spatial_record; rec != nullptr) {
            r.has_record = true;
            for (size_t i = 0; i < 6; ++i) {
                r.stored[i] = rec->volume[i];
            }
            // The engine's OWN shape tag, written by whichever setter last stored a volume.
            r.stored_is_sphere = (rec->volume_flags & 0x80u) != 0;
        }
        r.ok = true;
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        return r;
    }
    return r;
}

void set_vec(regenny::LTVector* v, const float (&src)[3], const float* add) {
    v->x = src[0] + (add != nullptr ? add[0] : 0.0f);
    v->y = src[1] + (add != nullptr ? add[1] : 0.0f);
    v->z = src[2] + (add != nullptr ? add[2] : 0.0f);
}

}  // namespace

std::optional<CullVolume> computed_cull_volume(const regenny::LTObject* obj) {
    if (obj == nullptr) {
        return std::nullopt;
    }
    const auto r = seh_read_cull(obj);
    if (!r.ok) {
        return std::nullopt;
    }
    CullVolume out{};
    out.shape = CullShape::None;
    // The two suppression cases first: OT_NORMAL never carries one, and flags3 bit 0x80
    // turns it off for any type. Both are the engine's own gate, not a value test.
    if (r.kind == 0 || (r.flags3 & 0x80u) != 0) {
        return out;
    }
    switch (r.kind) {
        case 2:   // OT_WORLDMODEL
        case 5: { // OT_CAMERA derives from it
            out.shape = CullShape::Box;
            set_vec(&out.min, r.aabb_min, nullptr);
            set_vec(&out.max, r.aabb_max, nullptr);
            return out;
        }
        case 1: { // OT_MODEL -- always a sphere (the virtual returns 1 on both paths), but
                  // the radius comes from a different place on each, and the engine does NOT
                  // treat them the same way.
            out.shape = CullShape::Sphere;
            if (r.sphere_source != 0) {
                set_vec(&out.center, r.sphere_center, nullptr);
                // NO SCALE HERE. OT_MODEL_GetCullVolume reads the radius straight out of the
                // shared asset -- `**(float**)(obj + 0xEC)`, i.e. LTModelAsset.radius -- and
                // multiplies by nothing. An earlier version of this branch applied
                // `* scale`, which was invisible live because scale is 1.0 on every object,
                // and was only caught by reading the virtual rather than comparing stored
                // values. vis_radius is a per-object CACHE of that same asset field (equal on
                // 215/215), so reading it here is the same number without a second
                // dereference.
                out.radius = r.vis_radius;
            } else {
                // This path DOES scale: the virtual computes `vis_radius * scale`.
                set_vec(&out.center, r.pos, nullptr);
                out.radius = r.vis_radius * r.scale;
            }
            return out;
        }
        case 3: { // OT_SPRITE -- kind selects the shape
            const bool boxed = r.sprite_kind == 3 || r.sprite_kind == 4 ||
                               r.sprite_kind == 7 || r.sprite_kind == 9;
            if (boxed) {
                out.shape = CullShape::Box;
                set_vec(&out.min, r.sprite_aabb_min, nullptr);
                set_vec(&out.max, r.sprite_aabb_max, nullptr);
            } else {
                out.shape = CullShape::Sphere;
                set_vec(&out.center, r.pos, nullptr);
                out.radius = r.sprite_radius;
            }
            return out;
        }
        case 6: { // OT_PARTICLESYSTEM -- offsets are OBJECT-LOCAL, so add the position
            if (r.ps_volume_type == 1) {
                out.shape = CullShape::Sphere;
                set_vec(&out.center, r.ps_sphere_offset, r.pos);
                out.radius = r.ps_sphere_radius;
            } else {
                out.shape = CullShape::Box;
                set_vec(&out.min, r.ps_min_offset, r.pos);
                set_vec(&out.max, r.ps_max_offset, r.pos);
            }
            return out;
        }
        default:
            return out;  // shape stays None
    }
}

std::optional<CullVolume> cull_volume(const regenny::LTObject* obj) {
    if (obj == nullptr) {
        return std::nullopt;
    }
    const auto r = seh_read_cull(obj);
    if (!r.ok || !r.has_record) {
        return std::nullopt;
    }
    // THE RECORD CARRIES ITS OWN SHAPE TAG, and it is what decides the layout below. An
    // earlier version re-derived the shape from the object's type instead, on the stated
    // belief that no such tag existed -- see volume_flags in the schema for the two engine
    // writers that set and clear it.
    //
    // THE TWO AGREE ON EVERY LIVE OBJECT (2215/2215), so this is a corroboration and not a
    // correction: the type rule was right. It is still worth taking the shape from here,
    // because the tag is ONE BIT WRITTEN BY THE ENGINE AT STORE TIME while the type rule is a
    // reimplementation of six virtual functions -- when a future object type appears, or one
    // of those virtuals is misread, the tag is the side that cannot drift.
    //
    // The type rule is still needed for the one thing the tag cannot express: whether a volume
    // exists at all. The engine calls neither setter for a suppressed object, so the bit keeps
    // whatever it last held. Existence comes from the gate, the layout from the tag.
    const auto computed = computed_cull_volume(obj);
    if (!computed.has_value()) {
        return std::nullopt;
    }
    CullVolume out{};
    out.shape = computed->shape == CullShape::None
                    ? CullShape::None
                    : (r.stored_is_sphere ? CullShape::Sphere : CullShape::Box);
    switch (out.shape) {
        case CullShape::Sphere:
            out.center.x = r.stored[0];
            out.center.y = r.stored[1];
            out.center.z = r.stored[2];
            out.radius = r.stored[3];
            break;
        case CullShape::Box:
            out.min.x = r.stored[0];
            out.min.y = r.stored[1];
            out.min.z = r.stored[2];
            out.max.x = r.stored[3];
            out.max.y = r.stored[4];
            out.max.z = r.stored[5];
            break;
        case CullShape::None:
            // Still hand back what the engine actually left there. The engine zeroes a
            // suppressed volume, so a caller CAN verify that rather than taking our word
            // -- and a non-zero here would be a real finding.
            out.min.x = r.stored[0];
            out.min.y = r.stored[1];
            out.min.z = r.stored[2];
            out.max.x = r.stored[3];
            out.max.y = r.stored[4];
            out.max.z = r.stored[5];
            break;
    }
    return out;
}

}  // namespace sdk

namespace sdk {

namespace {

// The engine's own coordinates reach five figures, so the tolerance must SCALE. A fixed
// 0.01 sits below float32 spacing up there and reports correct data as wrong -- which is
// precisely the bug introduced the first time this comparison was lifted out of
// check_spatial_records, where the original helper had carried a comment warning about it.
bool volume_approx_eq(float a, float b) {
    const float d = a > b ? a - b : b - a;
    const float m = b < 0.0f ? -b : b;
    return d <= 0.01f + 0.0001f * m;
}

}  // namespace

std::optional<bool> cull_volume_is_current(const regenny::LTObject* obj) {
    const auto stored = cull_volume(obj);
    const auto computed = computed_cull_volume(obj);
    if (!stored.has_value() || !computed.has_value()) {
        return std::nullopt;
    }
    switch (computed->shape) {
        case CullShape::None:
            // Suppressed: the engine leaves zeros, and the primitive hands those back even
            // for None so this stays a real comparison rather than an assumption.
            return stored->min.x == 0.0f && stored->min.y == 0.0f &&
                   stored->min.z == 0.0f && stored->max.x == 0.0f;
        case CullShape::Sphere:
            // CENTRE AND RADIUS. An earlier version compared the centre only, because the
            // radius rule on the sphere_source path was not established -- reading
            // OT_MODEL_GetCullVolume settled it (asset radius, unscaled), so the stronger
            // comparison is now warranted and this pins four floats instead of three.
            return volume_approx_eq(stored->center.x, computed->center.x) &&
                   volume_approx_eq(stored->center.y, computed->center.y) &&
                   volume_approx_eq(stored->center.z, computed->center.z) &&
                   volume_approx_eq(stored->radius, computed->radius);
        case CullShape::Box:
            return volume_approx_eq(stored->min.x, computed->min.x) &&
                   volume_approx_eq(stored->max.x, computed->max.x);
    }
    return std::nullopt;
}

}  // namespace sdk

namespace sdk {

namespace {

// POD mirror of the geometry fields, copied out under one guard.
struct GeomRaw {
    float pos[3];
    float dims[3];
    float mn[3];
    float mx[3];
    float radius;
    bool ok;
};

GeomRaw seh_read_geom(const regenny::LTObject* obj) {
    GeomRaw r{};
    KANANLIB_SEH_TRY {
        r.pos[0] = obj->position.x;
        r.pos[1] = obj->position.y;
        r.pos[2] = obj->position.z;
        r.dims[0] = obj->dims.x;
        r.dims[1] = obj->dims.y;
        r.dims[2] = obj->dims.z;
        r.mn[0] = obj->aabb_min.x;
        r.mn[1] = obj->aabb_min.y;
        r.mn[2] = obj->aabb_min.z;
        r.mx[0] = obj->aabb_max.x;
        r.mx[1] = obj->aabb_max.y;
        r.mx[2] = obj->aabb_max.z;
        r.radius = obj->radius;
        r.ok = true;
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        return r;
    }
    return r;
}

// RELATIVE, for the reason the original helper's own comment gave: level coordinates reach
// five figures, where a fixed epsilon sits below float32 spacing and fails on correct data.
bool geom_approx_eq(float a, float b) {
    const float d = a > b ? a - b : b - a;
    const float m = b < 0.0f ? -b : b;
    return d <= 0.01f + 0.0001f * m;
}

}  // namespace

std::optional<WorldAABB> world_aabb(const regenny::LTObject* obj) {
    if (obj == nullptr) {
        return std::nullopt;
    }
    const auto r = seh_read_geom(obj);
    if (!r.ok) {
        return std::nullopt;
    }
    WorldAABB out{};
    out.min.x = r.mn[0];
    out.min.y = r.mn[1];
    out.min.z = r.mn[2];
    out.max.x = r.mx[0];
    out.max.y = r.mx[1];
    out.max.z = r.mx[2];
    return out;
}

std::optional<bool> world_aabb_is_current(const regenny::LTObject* obj) {
    if (obj == nullptr) {
        return std::nullopt;
    }
    const auto r = seh_read_geom(obj);
    if (!r.ok) {
        return std::nullopt;
    }
    for (size_t i = 0; i < 3; ++i) {
        if (!geom_approx_eq(r.mn[i], r.pos[i] - r.dims[i]) ||
            !geom_approx_eq(r.mx[i], r.pos[i] + r.dims[i])) {
            return false;
        }
    }
    return true;
}

std::optional<BoundingRadius> bounding_radius(const regenny::LTObject* obj) {
    if (obj == nullptr) {
        return std::nullopt;
    }
    const auto r = seh_read_geom(obj);
    if (!r.ok) {
        return std::nullopt;
    }
    BoundingRadius out{};
    out.radius = r.radius;
    const bool zero_dims = r.dims[0] == 0.0f && r.dims[1] == 0.0f && r.dims[2] == 0.0f;
    out.unsized = zero_dims && r.radius == 0.0f;
    if (!out.unsized) {
        // The 0.1 is the engine's own constant, added by SetDims -- double-precision there,
        // so the length is computed in double before the comparison too.
        const double len = std::sqrt(static_cast<double>(r.dims[0]) * r.dims[0] +
                                     static_cast<double>(r.dims[1]) * r.dims[1] +
                                     static_cast<double>(r.dims[2]) * r.dims[2]);
        out.from_dims = geom_approx_eq(r.radius, static_cast<float>(len) + 0.1f);
    }
    return out;
}

}  // namespace sdk

namespace sdk {

std::optional<bool> is_tree_eligible(const regenny::LTObject* obj) {
    // The masks are the ENGINE'S, transcribed from LTObject_IsRenderable (0x4200A0). They are
    // the one place a raw constant is right rather than a smell: they ARE the predicate under
    // reproduction, so naming them after a guess at their meaning would obscure what is
    // being reproduced.
    constexpr uint32_t kSuppress = 0x200u;
    constexpr uint32_t kAccept = 0x10C30u;
    const auto info = object_info(obj);
    if (!info.has_value()) {
        return std::nullopt;
    }
    return (info->flags & kSuppress) == 0 && (info->flags & kAccept) != 0;
}

}  // namespace sdk
