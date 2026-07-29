#include "Object.hpp"

#include <windows.h>

#include <utility/Seh.hpp>

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

std::optional<bool> is_engine_addressable(const regenny::LTObject* obj) {
    if (obj == nullptr) {
        return std::nullopt;
    }
    const InfoRaw r = seh_read_info(obj);
    if (!r.ok) {
        return std::nullopt;
    }
    // The handle is the one the ILT* entry points take, so it decides. slot_index is
    // read too and must agree -- see the header -- but a caller asking this question
    // wants the answer about the API it is going to call.
    return r.handle != 0xFFFF;
}

}  // namespace sdk
