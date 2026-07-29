#include "Object.hpp"

#include <windows.h>

#include <utility/Seh.hpp>

#include "CClientMgr.hpp"
#include "regenny/regenny/LTAttachment.hpp"

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
