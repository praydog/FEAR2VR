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
    uint32_t socket;
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
        r.socket = a->socket;
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

// The engine's sentinel for "not mounted on a bone". 127, not -1 -- measured on
// 335/335 non-model owners.
constexpr uint32_t kNoSocket = 127;

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
        // Resolution needs the live manager; without it a caller still gets the
        // handles and sockets, which is more useful than an empty answer.
        a.child = mgr != nullptr ? mgr->object_from_handle(r.child_handle) : nullptr;
        // The socket is only an index when the OWNER is a model -- the engine's own
        // condition, not a value test, so a model that happened to store 127 would
        // still be reported as "no socket" and a non-model never yields an index.
        if (is_model && r.socket != kNoSocket) {
            a.socket = static_cast<size_t>(r.socket);
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
