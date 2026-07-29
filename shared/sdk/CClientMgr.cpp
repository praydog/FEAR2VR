#include "CClientMgr.hpp"

#include <windows.h>

#include <cmath>
#include <vector>

// check_transforms aggregates the public transform primitives rather than keeping its own
// copy of the maths.
#include "Object.hpp"
#include "VisTree.hpp"

#include <utility/Seh.hpp>

#include "regenny/regenny/CClientMgrCounterNode.hpp"
#include "regenny/regenny/LTCameraObject.hpp"
#include "regenny/regenny/LTAnimNameEntry.hpp"
#include "regenny/regenny/LTObjectHandleEntry.hpp"
#include "regenny/regenny/LTMemoryPool.hpp"
#include "regenny/regenny/LTModelAsset.hpp"
#include "regenny/regenny/LTModelObject.hpp"
#include "regenny/regenny/LTModelNode.hpp"
#include "regenny/regenny/LTObjectRef.hpp"
#include "regenny/regenny/LTParticleSystemObject.hpp"
#include "regenny/regenny/LTSpatialEntry.hpp"
#include "regenny/regenny/LTSpatialRecord.hpp"
#include "regenny/regenny/LTSpriteObject.hpp"
#include "regenny/regenny/LTVisPlane.hpp"
#include "regenny/regenny/StdString.hpp"
#include "regenny/regenny/LTVisSector.hpp"
#include "regenny/regenny/LTWorldModelObject.hpp"
#include "regenny/regenny/LTWorldTreeNode.hpp"

#include "CClientShell.hpp"
#include "VisTree.hpp"
#include "Log.hpp"
#include "Modules.hpp"

namespace sdk {

// CClientMgr::Update -- FEAR2_dump.exe 0x40B665:
//   55 8B EC | 83 EC 14 | 53 56 57 | 8B F1 | E8 [rel32] | 89 45 EC | 89 55 F0 |
//   E8 [rel32] | 8B C8 | E8 [rel32] | D9 05 [abs32] | 51 | D9 1C 24 | E8 ...
static constexpr const char* kUpdate =
    "55 8B EC 83 EC 14 53 56 57 8B F1 E8 ? ? ? ? 89 45 EC 89 55 F0 E8 ? ? ? ? 8B C8 "
    "E8 ? ? ? ? D9 05 ? ? ? ? 51 D9 1C 24 E8";

namespace { // scan.hpp anchor helpers stay local to their owning TU

// &g_pClientMgr is the dword operand of `mov ecx,[imm]` inside
// CClientShell::Update's prologue (fn+0x10; dump evidence: reads 0x6ECCA0).
// Own function scope: __try cannot share a function with static-local
// initialization (MSVC C2712), hence no lambda here.
uintptr_t resolve_instance_slot() {
    constexpr uint32_t kUpdate_ClientMgrOperand = 0x10;
    const uintptr_t fn = CClientShell::update_fn();
    if (fn == 0) {
        return 0;
    }
    uintptr_t slot = 0;
    KANANLIB_SEH_TRY {
        slot = *reinterpret_cast<uintptr_t*>(fn + kUpdate_ClientMgrOperand);
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        LOGX("[sdk] crashed reading &g_pClientMgr operand");
        return 0;
    }
    return slot;
}

} // namespace

uintptr_t CClientMgr::update_fn() {
    static const uintptr_t s_fn = Modules::get().scan_exe(kUpdate, "CClientMgr::Update");
    return s_fn;
}

uintptr_t CClientMgr::instance_slot() {
    static const uintptr_t s_slot = resolve_instance_slot();
    return s_slot;
}

CClientMgr* CClientMgr::get() {
    const uintptr_t slot = instance_slot();
    if (slot == 0) {
        return nullptr;
    }
    CClientMgr* instance = nullptr;
    KANANLIB_SEH_TRY {
        instance = *reinterpret_cast<CClientMgr**>(slot);
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    return instance;
}

CClientShell* CClientMgr::client_shell() const {
    CClientShell* shell = nullptr;
    KANANLIB_SEH_TRY {
        shell = reinterpret_cast<CClientShell*>(regenny()->client_shell);
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
    return shell;
}

bool CClientMgr::is_updating() const {
    bool updating = false;
    KANANLIB_SEH_TRY {
        updating = regenny()->updating;
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return updating;
}

uint32_t CClientMgr::counter_elapsed_ms() const {
    uint32_t ms = 0;
    KANANLIB_SEH_TRY {
        const auto* node = regenny()->own_counter_node;
        if (node != nullptr) {
            ms = node->elapsed_ms;
        }
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return ms;
}

double CClientMgr::counter_elapsed_time() const {
    double t = 0.0;
    KANANLIB_SEH_TRY {
        const auto* node = regenny()->own_counter_node;
        if (node != nullptr) {
            t = node->elapsed_time;
        }
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        return 0.0;
    }
    return t;
}

// POD-only SEH helper (MSVC C2712: __try cannot share a function with a
// non-trivial return type like std::optional). Returns the entry count, or
// -1 if the walk faulted or did not terminate within the fail-closed cap.
// The cap lives HERE and nowhere else -- no caller may restate it.
static int64_t seh_walk_start_shell_list(const regenny::CClientMgr* r) {
    constexpr size_t kMaxWalk = 10000; // fail closed on a corrupted/non-terminating list rather than hang
    int64_t result = -1;
    KANANLIB_SEH_TRY {
        const auto* head = &r->start_shell_list;
        const regenny::CClientMgrListLink* cur = head->next;
        size_t count = 0;
        while (cur != head && count < kMaxWalk) {
            ++count;
            cur = cur->next;
        }
        // Terminated only if we came back around to the head; hitting the cap
        // means the list is corrupt or the mapping is wrong -- report neither
        // a count nor a guess.
        result = (cur == head) ? static_cast<int64_t>(count) : -1;
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        result = -1;
    }
    return result;
}

std::optional<size_t> CClientMgr::start_shell_list_count() const {
    const int64_t n = seh_walk_start_shell_list(regenny());
    if (n < 0) {
        return std::nullopt;
    }
    return static_cast<size_t>(n);
}

bool CClientMgr::counter_node_registered() const {
    bool ok = false;
    KANANLIB_SEH_TRY {
        auto* r = regenny();
        auto* node = r->own_counter_node;
        if (node != nullptr) {
            // &node->self_link -- the compiler computes the offset from the
            // generated schema; no literal appears here or in any caller.
            ok = (&node->self_link == r->counter_list_head.next);
        }
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return ok;
}

// SEH-guarded like is_updating() above, and for the same reason: `this` can go
// stale between get() and this call (level unload, engine teardown), so even a
// direct scalar read off the singleton can fault. Each fails closed to a value
// a caller cannot mistake for real data.

uint32_t CClientMgr::last_sample_time_ms() const {
    uint32_t v = 0;
    KANANLIB_SEH_TRY {
        v = regenny()->last_sample_time_ms;
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return v;
}

bool CClientMgr::has_pending_shell_release() const {
    bool v = false;
    KANANLIB_SEH_TRY {
        v = (regenny()->pending_shell_release != nullptr);
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return v;
}

const char* object_type_name(ObjectType type) {
    switch (type) {
    case regenny::OT_NORMAL:         return "OT_NORMAL";
    case regenny::OT_MODEL:          return "OT_MODEL";
    case regenny::OT_WORLDMODEL:     return "OT_WORLDMODEL";
    case regenny::OT_SPRITE:         return "OT_SPRITE";
    case regenny::OT_LIGHT:          return "OT_LIGHT";
    case regenny::OT_CAMERA:         return "OT_CAMERA";
    case regenny::OT_PARTICLESYSTEM: return "OT_PARTICLESYSTEM";
    }
    return "OT_INVALID";
}

namespace {

// Derive the object base from its embedded link via offsetof on the
// generated schema -- the engine's own walkers use `link - 172`, and that
// 172 is LTObject.list_link's offset, which the compiler computes here so
// no literal ever appears.
const regenny::LTObject* object_from_link(const regenny::CClientMgrListLink* link) {
    return reinterpret_cast<const regenny::LTObject*>(
        reinterpret_cast<uintptr_t>(link) - offsetof(regenny::LTObject, list_link));
}

// POD-only SEH helper (MSVC C2712). Reads one link's `next` and, when that
// is not the head, the resulting object's `type`.
//
// `out_type` is written only when a real object was reached. Returns false
// on fault. The type read happens INSIDE the guard on purpose: it is the
// first dereference of a pointer we just followed, and it is exactly what
// detects wrong container-of arithmetic (see the type check in the callers).
struct RawStep {
    const regenny::CClientMgrListLink* next;
    uint8_t type;
    bool at_end;
};

bool seh_step(const regenny::CClientMgrListLink* head,
              const regenny::CClientMgrListLink* cur,
              RawStep* out) {
    bool ok = false;
    KANANLIB_SEH_TRY {
        const regenny::CClientMgrListLink* next = cur->next;
        out->next = next;
        out->at_end = (next == head);
        out->type = out->at_end ? 0u : static_cast<uint8_t>(object_from_link(next)->type);
        ok = true;
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    return ok;
}

// POD-only SEH helper. Walks one object bucket, counting entries. Returns
// the count, or -1 if it faulted, did not terminate, or found an object whose
// type disagreed with its bucket.
//
// The type check is here so that ALL THREE read paths -- counting, iterating
// (step_from) and snapshotting -- validate identically. Without it a caller
// could get a count that the other two paths would have rejected, and
// diagnostics reporting "the walk was clean" would be claiming more than this
// function checked.
int64_t seh_count_objects(const regenny::CClientMgrListLink* head,
                          uint8_t expected_type, size_t cap) {
    int64_t result = -1;
    KANANLIB_SEH_TRY {
        const regenny::CClientMgrListLink* cur = head->next;
        size_t n = 0;
        bool invariant_ok = true;
        while (cur != head && n < cap) {
            if (static_cast<uint8_t>(object_from_link(cur)->type) != expected_type) {
                invariant_ok = false;
                break;
            }
            ++n;
            cur = cur->next;
        }
        result = (invariant_ok && cur == head) ? static_cast<int64_t>(n) : -1;
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        result = -1;
    }
    return result;
}

// POD-only SEH helper. Walks one bucket AND copies each object's fields in
// the SAME pass -- see the header on why a snapshot never hands out an
// LTObject*. Returns the number written, or -1 on fault/non-termination/
// invariant violation.
//
// SELF-CHECK: every object reached from bucket N must have type == N. That
// is the invariant proven live during mapping (0 mismatches / 3490 objects,
// single traversal). Enforcing it here is cheap and catches what SEH can't:
// wrong container-of arithmetic (a schema drift in list_link's offset would
// land us on garbage that is still *readable*), and some stale-but-readable
// races where a node was unlinked and its memory reused. A violation fails
// the whole snapshot rather than returning plausible-looking wrong data.
int64_t seh_snapshot_objects(const regenny::CClientMgrListLink* head,
                             uint8_t expected_type,
                             CClientMgr::ObjectSnapshot* out, size_t max, size_t cap) {
    int64_t result = -1;
    KANANLIB_SEH_TRY {
        const regenny::CClientMgrListLink* cur = head->next;
        size_t n = 0, written = 0;
        bool invariant_ok = true;
        while (cur != head && n < cap) {
            const auto* obj = object_from_link(cur);
            if (static_cast<uint8_t>(obj->type) != expected_type) {
                invariant_ok = false;
                break;
            }
            if (written < max) {
                auto& s = out[written];
                s.address = reinterpret_cast<uintptr_t>(obj);
                s.vtable = reinterpret_cast<uintptr_t>(obj->vtable);
                s.type = obj->type;
                s.handle = obj->handle;
                s.position[0] = obj->position.x;
                s.position[1] = obj->position.y;
                s.position[2] = obj->position.z;
                s.rotation[0] = obj->rotation.x;
                s.rotation[1] = obj->rotation.y;
                s.rotation[2] = obj->rotation.z;
                s.rotation[3] = obj->rotation.w;
                ++written;
            }
            ++n;
            cur = cur->next;
        }
        result = (invariant_ok && cur == head) ? static_cast<int64_t>(written) : -1;
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        result = -1;
    }
    return result;
}

} // namespace

// first_object/next_object share one step: advance from `cur` (the head on
// the first call) and validate what we land on.
CClientMgr::ObjectStep CClientMgr::step_from(ObjectType type,
                                            const regenny::CClientMgrListLink* cur,
                                            size_t index) const {
    if (static_cast<size_t>(type) >= object_list_count()) {
        return ObjectStep{nullptr, index, false};
    }
    const auto* head = &regenny()->object_lists[static_cast<size_t>(type)];
    RawStep raw{};
    if (!seh_step(head, cur, &raw)) {
        return ObjectStep{nullptr, index, false};
    }
    if (raw.at_end) {
        // Clean end of list. Checked BEFORE the walk bound so a bucket
        // holding exactly max_object_walk objects still terminates cleanly --
        // the bound rejects a (max_object_walk + 1)'th object, not a list
        // that legitimately ends at the limit.
        return ObjectStep{nullptr, index, true};
    }
    if (index >= max_object_walk) {
        // Another object beyond the bound: the list is not terminating. Fail
        // closed rather than return a plausible-looking truncation.
        return ObjectStep{nullptr, index, false};
    }
    if (raw.type != static_cast<uint8_t>(type)) {
        // Bucket invariant broken -- the mapping drifted, or we followed a
        // reused node. Same reasoning as the snapshot's self-check.
        return ObjectStep{nullptr, index, false};
    }
    return ObjectStep{object_from_link(raw.next), index + 1, true};
}

CClientMgr::ObjectStep CClientMgr::first_object(ObjectType type) const {
    if (static_cast<size_t>(type) >= object_list_count()) {
        return ObjectStep{nullptr, 0, false};
    }
    return step_from(type, &regenny()->object_lists[static_cast<size_t>(type)], 0);
}

CClientMgr::ObjectStep CClientMgr::next_object(ObjectType type, ObjectStep cur) const {
    if (cur.object == nullptr) {
        return ObjectStep{nullptr, cur.index, false};
    }
    return step_from(type, &cur.object->list_link, cur.index);
}

std::optional<size_t> CClientMgr::object_count(ObjectType type) const {
    if (static_cast<size_t>(type) >= object_list_count()) {
        return std::nullopt;
    }
    const int64_t n = seh_count_objects(&regenny()->object_lists[static_cast<size_t>(type)],
                                        static_cast<uint8_t>(type), max_object_walk);
    if (n < 0) {
        return std::nullopt;
    }
    return static_cast<size_t>(n);
}

namespace {

// POD-only SEH helpers for the handle table. Kept out of the methods below so the
// guarded reads never share a function with std::optional's construction.
struct HandleSlot {
    uint32_t tag;
    const void* object;
    bool valid;
};

HandleSlot seh_handle_slot(const regenny::CClientMgr* mgr, uint16_t handle) {
    HandleSlot s{};
    KANANLIB_SEH_TRY {
        const auto* first = mgr->handle_table.first;
        const auto* last = mgr->handle_table.last;
        if (first != nullptr && last > first) {
            const size_t slots = static_cast<size_t>(last - first);
            if (handle < slots) {
                s.tag = first[handle].tag;
                s.object = first[handle].object;
                s.valid = true;
            }
        }
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        s.valid = false;
    }
    return s;
}

int64_t seh_handle_of(const regenny::LTObject* obj) {
    int64_t h = -1;
    KANANLIB_SEH_TRY {
        h = static_cast<int64_t>(obj->handle);
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        h = -1;
    }
    return h;
}

int64_t seh_handle_slots(const regenny::CClientMgr* mgr) {
    int64_t n = -1;
    KANANLIB_SEH_TRY {
        const auto* first = mgr->handle_table.first;
        const auto* last = mgr->handle_table.last;
        if (first != nullptr && last >= first) {
            n = static_cast<int64_t>(last - first);
        }
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        n = -1;
    }
    return n;
}

}  // namespace

const regenny::LTObject* CClientMgr::object_from_handle(uint16_t handle) const {
    if (handle == kNoHandle || regenny() == nullptr) {
        return nullptr;
    }
    const HandleSlot s = seh_handle_slot(regenny(), handle);
    // A free slot is not tagged 1. Checking the tag rather than just non-null is
    // what keeps a recycled slot from handing back a stale object.
    if (!s.valid || s.tag != 1 || s.object == nullptr) {
        return nullptr;
    }
    return static_cast<const regenny::LTObject*>(s.object);
}

std::optional<uint16_t> CClientMgr::handle_of(const regenny::LTObject* obj) const {
    if (obj == nullptr) {
        return std::nullopt;
    }
    const int64_t h = seh_handle_of(obj);
    if (h < 0 || h == kNoHandle) {
        return std::nullopt;
    }
    return static_cast<uint16_t>(h);
}

std::optional<size_t> CClientMgr::handle_table_size() const {
    if (regenny() == nullptr) {
        return std::nullopt;
    }
    const int64_t n = seh_handle_slots(regenny());
    if (n < 0) {
        return std::nullopt;
    }
    return static_cast<size_t>(n);
}

std::optional<size_t> CClientMgr::snapshot_objects(ObjectType type, ObjectSnapshot* out, size_t max) const {
    if (static_cast<size_t>(type) >= object_list_count() || (out == nullptr && max != 0)) {
        return std::nullopt;
    }
    const int64_t n = seh_snapshot_objects(&regenny()->object_lists[static_cast<size_t>(type)],
                                           static_cast<uint8_t>(type), out, max, max_object_walk);
    if (n < 0) {
        return std::nullopt;
    }
    return static_cast<size_t>(n);
}


namespace {

// Bank index -> object type. NOT the identity: OT_LIGHT (4) has no bank, so
// the array is compacted around it. Proven by chunk membership -- each bank's
// pool chunk contains only objects of its mapped type, 0 foreign, 6/6. See
// fear2.genny's CClientMgr.object_banks comment.
//
// Sized from the schema so a bank appearing or disappearing is a compile
// error here rather than a silent mis-mapping.
constexpr uint8_t kBankToType[] = {0, 1, 2, 3, 5, 6};
static_assert(sizeof(kBankToType) / sizeof(kBankToType[0]) ==
                  sizeof(regenny::CClientMgr::object_banks) /
                      sizeof(regenny::CClientMgrObjectBank),
              "bank->type table must cover exactly the schema's bank array");

// POD-only SEH helper: reads the bank pair plus the pool's block size.
bool seh_read_bank(const regenny::CClientMgrObjectBank* bank,
                   uint32_t* element_size, uint32_t* block_size, uintptr_t* pool) {
    bool ok = false;
    KANANLIB_SEH_TRY {
        const auto* p = bank->pool;
        *pool = reinterpret_cast<uintptr_t>(p);
        *element_size = bank->element_size;
        *block_size = (p != nullptr) ? p->block_size : 0u;
        ok = (p != nullptr);
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    return ok;
}

} // namespace

std::optional<CClientMgr::ObjectBankInfo> CClientMgr::bank_at(size_t index) const {
    if (index >= object_bank_count()) {
        return std::nullopt;
    }
    uint32_t element_size = 0, block_size = 0;
    uintptr_t pool = 0;
    if (!seh_read_bank(&regenny()->object_banks[index], &element_size, &block_size, &pool)) {
        return std::nullopt;
    }
    return ObjectBankInfo{static_cast<ObjectType>(kBankToType[index]), pool, element_size, block_size};
}

std::optional<CClientMgr::ObjectBankInfo> CClientMgr::bank_for(ObjectType type) const {
    for (size_t i = 0; i < object_bank_count(); ++i) {
        if (kBankToType[i] == static_cast<uint8_t>(type)) {
            return bank_at(i);
        }
    }
    return std::nullopt; // OT_LIGHT, or an out-of-range value
}

namespace {

// POD-only SEH helper for OT_MODEL's embedded list. Every offset comes from the
// generated schema. Returns the number sampled, or -1 on fault / non-termination.
int64_t seh_check_model_lists(const regenny::CClientMgrListLink* head, size_t max,
                              size_t* count_ok, size_t* embedded_ok, size_t* dup_ok,
                              size_t* asset_ok, size_t* rot_ok, size_t* max_members,
                              size_t* members_total, size_t* member_asset_ok,
                              size_t cap) {
    int64_t result = -1;
    KANANLIB_SEH_TRY {
        const regenny::CClientMgrListLink* cur = head->next;
        size_t n = 0, sampled = 0;
        while (cur != head && n < cap) {
            if (sampled < max) {
                const auto* obj = reinterpret_cast<const regenny::LTModelObject*>(
                    reinterpret_cast<uintptr_t>(cur) - offsetof(regenny::LTObject, list_link));

                // Walk the object's own list. Every member is an LTModelRecord,
                // recovered from its `link` at offset 0 -- and the check that
                // makes that claim real is that each member's asset equals the
                // owner's. The cap is the same guard used for the outer walk: a
                // corrupt link must not spin here either.
                const auto* lh = &obj->list_head;
                const auto* want = &obj->record.link;
                const auto* owner_asset = obj->record.asset;
                const regenny::CClientMgrListLink* e = lh->next;
                size_t members = 0;
                bool found = false, closed = false;
                while (members < cap) {
                    if (e == lh) {
                        closed = true;
                        break;
                    }
                    if (e == want) {
                        found = true;
                    }
                    const auto* rec = reinterpret_cast<const regenny::LTModelRecord*>(e);
                    ++*members_total;
                    if (rec->asset != nullptr && rec->asset == owner_asset) {
                        ++*member_asset_ok;
                    }
                    ++members;
                    e = e->next;
                }
                if (closed) {
                    if (obj->list_count == members) {
                        ++*count_ok;
                    }
                    if (members > *max_members) {
                        *max_members = members;
                    }
                }
                if (found) {
                    ++*embedded_ok;
                }
                if (obj->record.asset != nullptr) {
                    ++*asset_ok;
                }
                // THREE offsets reach the model's asset: record.asset (0xEC),
                // block_120.asset (0x12C) and block_130.asset (0x130). All three
                // agreeing is the assertion; any one drifting means a sub-object
                // boundary moved.
                if (obj->block_120.asset == obj->record.asset &&
                    obj->block_130.asset == obj->record.asset) {
                    ++*dup_ok;
                }
                const auto& q = obj->cached_rotation;
                const float mag2 = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
                if (mag2 > 0.99f && mag2 < 1.01f) {
                    ++*rot_ok;
                }
                ++sampled;
            }
            ++n;
            cur = cur->next;
        }
        result = (cur == head) ? static_cast<int64_t>(sampled) : -1;
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        result = -1;
    }
    return result;
}

// POD-only SEH helper for the material-name arrays. Reaches through two pointer
// levels (model -> string array -> heap body), so every length is bounded before
// it is used. Returns the number of models sampled, or -1 on fault.
int64_t seh_check_materials(const regenny::CClientMgrListLink* head, size_t max,
                            size_t* models, size_t* strings_total, size_t* terminated,
                            size_t* size_ok, size_t* cap_ok, size_t* printable,
                            size_t* max_count, size_t cap) {
    // A material path is a filename; anything beyond this is a decode failure,
    // not a long name. Bounding before the read is what keeps a bad offset from
    // turning into a huge copy.
    constexpr uint32_t kMaxLen = 1024;
    constexpr uint32_t kSmallBuf = 16;
    int64_t result = -1;
    KANANLIB_SEH_TRY {
        const regenny::CClientMgrListLink* cur = head->next;
        size_t n = 0, sampled = 0;
        while (cur != head && n < cap) {
            if (sampled < max) {
                const auto* obj = reinterpret_cast<const regenny::LTModelObject*>(
                    reinterpret_cast<uintptr_t>(cur) - offsetof(regenny::LTObject, list_link));
                const auto* arr = obj->material_names;
                const uint32_t count = obj->material_count;
                if (arr != nullptr && count > 0 && count <= 64) {
                    ++*models;
                    if (count > *max_count) {
                        *max_count = count;
                    }
                    for (uint32_t i = 0; i < count; ++i) {
                        const auto& s = arr[i];
                        ++*strings_total;
                        if (s.capacity >= kSmallBuf - 1) {
                            ++*cap_ok;
                        }
                        if (s.size <= s.capacity) {
                            ++*size_ok;
                        }
                        if (s.size > kMaxLen || s.size > s.capacity) {
                            continue;  // do not decode a length we do not trust
                        }
                        // capacity >= 16 means the body moved to the heap and buf
                        // holds the pointer; otherwise the body IS buf.
                        const char* data = (s.capacity >= kSmallBuf)
                                               ? *reinterpret_cast<const char* const*>(s.buf)
                                               : reinterpret_cast<const char*>(s.buf);
                        if (data == nullptr) {
                            continue;
                        }
                        if (data[s.size] == '\0') {
                            ++*terminated;
                        }
                        bool ok = s.size > 0;
                        for (uint32_t k = 0; k < s.size; ++k) {
                            const unsigned char c = static_cast<unsigned char>(data[k]);
                            if (c < 0x20 || c > 0x7E) {
                                ok = false;
                                break;
                            }
                        }
                        if (ok) {
                            ++*printable;
                        }
                    }
                }
                ++sampled;
            }
            ++n;
            cur = cur->next;
        }
        result = (cur == head) ? static_cast<int64_t>(sampled) : -1;
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        result = -1;
    }
    return result;
}

// Tolerance scaled with magnitude. NOT a tuned fudge factor: world coordinates
// here reach five figures, where a fixed absolute epsilon sits below the float
// spacing and would fail on correct data. Named `approx_eq`, not `near` --
// windows.h still defines `near` as an empty macro, which silently eats the
// declaration and yields a baffling C2513.
bool approx_eq(float a, float b) {
    const float d = a > b ? a - b : b - a;
    const float m = b < 0 ? -b : b;
    return d <= 0.01f + 0.0001f * m;
}


} // namespace

std::optional<CClientMgr::TransformCheck> CClientMgr::check_transforms(size_t type,
                                                                      size_t max) const {
    // Only WorldModel (2) and the Camera (5) that derives from it carry the
    // cached transform pair. Any other type would read past its allocation.
    if (type != 2 && type != 5) {
        return std::nullopt;
    }
    if (type >= object_list_count()) {
        return std::nullopt;
    }
    // AGGREGATES sdk::brush_transform_quality RATHER THAN REIMPLEMENTING IT. This used to
    // hold its own copy of the quaternion-to-matrix conversion, the transpose comparison
    // and the determinant, inside one SEH walk, and threw all of it away except these
    // three counters. The maths now lives in Object.hpp where a consumer can reach it, and
    // this function does what a check should: sample the population and count.
    std::vector<ObjectSnapshot> snaps(max);
    const auto taken = snapshot_objects(static_cast<ObjectType>(type), snaps.data(), max);
    if (!taken.has_value()) {
        return std::nullopt;
    }
    TransformCheck out{};
    for (size_t i = 0; i < *taken; ++i) {
        const auto* obj = reinterpret_cast<const regenny::LTObject*>(snaps[i].address);
        const auto q = brush_transform_quality(obj);
        if (!q.has_value()) {
            continue;
        }
        ++out.sampled;
        if (q->rotation_matches) {
            ++out.rotation_match;
        }
        if (q->inverse_exact) {
            ++out.inverse_ok;
        }
        if (q->determinant_unit) {
            ++out.det_ok;
        }
    }
    return out;
}

std::optional<CClientMgr::SchemaSizeCheck> CClientMgr::check_schema_sizes() const {
    if (regenny() == nullptr) {
        return std::nullopt;
    }
    // The engine's element_size for a type versus our schema's sizeof for the
    // class we mapped onto it. Both sides are derived, neither is recorded.
    struct Expect {
        ObjectType type;
        size_t size;
    };
    static constexpr Expect kExpect[] = {
        {static_cast<ObjectType>(0), sizeof(regenny::LTObject)},
        {static_cast<ObjectType>(1), sizeof(regenny::LTModelObject)},
        {static_cast<ObjectType>(2), sizeof(regenny::LTWorldModelObject)},
        {static_cast<ObjectType>(3), sizeof(regenny::LTSpriteObject)},
        {static_cast<ObjectType>(5), sizeof(regenny::LTCameraObject)},
        {static_cast<ObjectType>(6), sizeof(regenny::LTParticleSystemObject)},
    };

    SchemaSizeCheck out{};
    for (const auto& e : kExpect) {
        const auto bank = bank_for(e.type);
        if (!bank.has_value()) {
            continue;  // reported as a shortfall in types_checked
        }
        ++out.types_checked;
        if (bank->element_size == e.size) {
            ++out.size_matches;
        }
    }
    // OT_LIGHT is uncreatable -- CClientMgr_CreateObjectOfType has no case 4 --
    // so the engine allocates no bank for it. Absence is the assertion here.
    out.light_has_no_bank = !bank_for(static_cast<ObjectType>(4)).has_value();
    return out;
}

std::optional<CClientMgr::ModelListCheck> CClientMgr::check_model_lists(size_t max) const {
    constexpr size_t kModelType = 1;
    if (regenny() == nullptr || kModelType >= object_list_count()) {
        return std::nullopt;
    }
    ModelListCheck out{};
    const int64_t n = seh_check_model_lists(&regenny()->object_lists[kModelType], max,
                                           &out.count_matches_walk, &out.embedded_linked,
                                           &out.asset_dup_agrees, &out.asset_present,
                                           &out.rotation_unit, &out.max_members,
                                           &out.members_total, &out.member_asset_ok,
                                           max_object_walk);
    if (n < 0) {
        return std::nullopt;
    }
    out.sampled = static_cast<size_t>(n);
    return out;
}

std::optional<CClientMgr::MaterialCheck> CClientMgr::check_model_materials(size_t max) const {
    constexpr size_t kModelType = 1;
    if (regenny() == nullptr || kModelType >= object_list_count()) {
        return std::nullopt;
    }
    MaterialCheck out{};
    const int64_t n = seh_check_materials(&regenny()->object_lists[kModelType], max, &out.models,
                                         &out.strings_total, &out.terminated, &out.size_le_capacity,
                                         &out.capacity_sane, &out.nonempty_printable,
                                         &out.max_count, max_object_walk);
    if (n < 0) {
        return std::nullopt;
    }
    return out;
}

// POD-only SEH helper for the shared model assets. Collects distinct assets in a
// small fixed table -- no allocation inside the guard -- then checks each one.
// Returns the number of distinct assets, or -1 on fault / non-termination.
static int64_t seh_check_assets(const regenny::CClientMgrListLink* head, size_t max,
                               size_t* self_ok, size_t* rad_ok, size_t* name_at_blob,
                               size_t* name_ok, size_t* rc_ge, size_t* rc_exact,
                               size_t* blob_sane, size_t* in_blob, size_t* order_ok,
                               size_t* count_ok, size_t* count_dup_ok, size_t cap) {
    // Distinct assets live in the low tens; the table is a hard ceiling, and
    // overflow simply stops collecting rather than corrupting anything.
    constexpr size_t kMaxAssets = 512;
    constexpr uint32_t kMaxPath = 260;
    const regenny::LTModelAsset* seen[kMaxAssets]{};
    uint32_t users[kMaxAssets]{};
    size_t distinct = 0;
    int64_t result = -1;
    KANANLIB_SEH_TRY {
        const regenny::CClientMgrListLink* cur = head->next;
        size_t n = 0, sampled = 0;
        while (cur != head && n < cap) {
            if (sampled < max) {
                const auto* obj = reinterpret_cast<const regenny::LTModelObject*>(
                    reinterpret_cast<uintptr_t>(cur) - offsetof(regenny::LTObject, list_link));
                const auto* a = obj->record.asset;
                if (a != nullptr) {
                    size_t i = 0;
                    for (; i < distinct; ++i) {
                        if (seen[i] == a) {
                            break;
                        }
                    }
                    if (i == distinct && distinct < kMaxAssets) {
                        seen[distinct] = a;
                        users[distinct] = 0;
                        ++distinct;
                    }
                    if (i < kMaxAssets) {
                        ++users[i];
                    }
                }
                ++sampled;
            }
            ++n;
            cur = cur->next;
        }
        if (cur != head) {
            return -1;
        }
        for (size_t i = 0; i < distinct; ++i) {
            const auto* a = seen[i];
            if (a->self_ref == a) {
                ++*self_ok;
            }
            if (a->radius > 0.0f && a->radius == a->radius_from_file) {
                ++*rad_ok;
            }
            if (reinterpret_cast<uintptr_t>(a->filename) ==
                reinterpret_cast<uintptr_t>(a->string_blob)) {
                ++*name_at_blob;
            }
            // Containment. The blob is one allocation the loader carves the name
            // table, the fixed-up pointer array and both entry arrays out of, so
            // every derived pointer must land inside it. Checked against the
            // asset's OWN size field -- nothing external.
            const auto blob = reinterpret_cast<uintptr_t>(a->string_blob);
            const auto ea = reinterpret_cast<uintptr_t>(a->node_names);
            const auto eb = reinterpret_cast<uintptr_t>(a->node_hashes);
            const auto nm = reinterpret_cast<uintptr_t>(a->filename);
            const uint32_t bsz = a->string_blob_size;
            const bool sane = blob != 0 && bsz > 0 && bsz < 0x400000;
            if (sane) {
                ++*blob_sane;
                if (ea >= blob && ea < blob + bsz && eb >= blob && eb < blob + bsz) {
                    ++*in_blob;
                }
                if (nm <= ea && ea <= eb) {
                    ++*order_ok;
                }
                if (eb >= ea && a->node_count == (eb - ea) / 4) {
                    ++*count_ok;
                }
            }
            if (a->node_count_dup == a->node_count) {
                ++*count_dup_ok;
            }
            const char* p = a->filename;
            if (p != nullptr) {
                uint32_t k = 0;
                bool good = true;
                for (; k < kMaxPath; ++k) {
                    const unsigned char c = static_cast<unsigned char>(p[k]);
                    if (c == 0) {
                        break;
                    }
                    if (c < 0x20 || c > 0x7E) {
                        good = false;
                        break;
                    }
                }
                if (good && k > 0 && k < kMaxPath) {
                    ++*name_ok;
                }
            }
            if (a->refcount >= users[i]) {
                ++*rc_ge;
            }
            if (a->refcount == 2 * users[i] + 1) {
                ++*rc_exact;
            }
        }
        result = static_cast<int64_t>(distinct);
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        result = -1;
    }
    return result;
}

// Case-insensitive compare over the printable range. Deliberately not <cctype>:
// this runs inside an SEH guard where a locale-dependent call is unwelcome, and
// the engine's own hash folds case with a plain table.
static bool name_equals_i(const char* a, const char* b, uint32_t cap) {
    for (uint32_t i = 0; i < cap; ++i) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') {
            ca = static_cast<char>(ca + 32);
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = static_cast<char>(cb + 32);
        }
        if (ca != cb) {
            return false;
        }
        if (ca == '\0') {
            return true;
        }
    }
    return false;
}

// POD-only SEH helper for the node name/hash arrays. Reaches through the asset's
// blob, so every pointer is range-checked against the asset's own recorded size
static int64_t seh_check_nodes(const regenny::CClientMgrListLink* head, size_t max,
                              size_t* nodes_total, size_t* in_blob, size_t* printable,
                              size_t* distinct, size_t* repeated, size_t* consistent,
                              size_t* collisions, size_t* count_dup_ok, size_t* records_in_blob,
                              size_t* root_255, size_t* index_self_ok, size_t* topological_ok,
                              size_t* child_sum_ok, size_t* rot_a_unit, size_t* rot_b_unit,
                              size_t* pos_finite, size_t* child_range_ok, size_t* child_parents_ok,
                              size_t* child_links_seen, size_t cap) {
    constexpr size_t kMaxAssets = 512;
    constexpr size_t kMaxNames = 4096;
    constexpr uint32_t kMaxName = 128;
    constexpr uint32_t kMaxNodes = 1024;
    const regenny::LTModelAsset* seen[kMaxAssets]{};
    const char* names[kMaxNames]{};
    uint32_t hashes[kMaxNames]{};
    uint32_t counts[kMaxNames]{};
    size_t n_assets = 0, n_names = 0;
    int64_t result = -1;
    KANANLIB_SEH_TRY {
        const regenny::CClientMgrListLink* cur = head->next;
        size_t n = 0, sampled = 0;
        while (cur != head && n < cap) {
            if (sampled < max) {
                const auto* obj = reinterpret_cast<const regenny::LTModelObject*>(
                    reinterpret_cast<uintptr_t>(cur) - offsetof(regenny::LTObject, list_link));
                const auto* a = obj->record.asset;
                bool fresh = a != nullptr;
                for (size_t i = 0; i < n_assets && fresh; ++i) {
                    if (seen[i] == a) {
                        fresh = false;
                    }
                }
                if (fresh && n_assets < kMaxAssets) {
                    seen[n_assets++] = a;
                    const auto blob = reinterpret_cast<uintptr_t>(a->string_blob);
                    const uint32_t bsz = a->string_blob_size;
                    const uint32_t nc = a->node_count;
                    if (a->node_count_dup == nc) {
                        ++*count_dup_ok;
                    }
                    // Tree shape, per asset. Every one of these is the array
                    // checked against the asset's own node_count -- no baseline.
                    const auto* recs = a->node_records;
                    const auto rp = reinterpret_cast<uintptr_t>(recs);
                    if (recs != nullptr && nc > 0 && nc <= kMaxNodes && blob != 0 && bsz > 0 &&
                        rp >= blob && rp + sizeof(regenny::LTModelNode) * nc <= blob + bsz) {
                        ++*records_in_blob;
                        if (recs[0].parent_index == 255) {
                            ++*root_255;
                        }
                        bool idx_ok = true, topo_ok = true;
                        uint32_t children = 0;
                        for (uint32_t k = 0; k < nc; ++k) {
                            const auto& nd = recs[k];
                            if (nd.own_index != static_cast<uint8_t>(k)) {
                                idx_ok = false;
                            }
                            if (k > 0 && nd.parent_index >= k) {
                                topo_ok = false;
                            }
                            children += nd.child_count;
                            const float ma = nd.rotation_a.x * nd.rotation_a.x +
                                             nd.rotation_a.y * nd.rotation_a.y +
                                             nd.rotation_a.z * nd.rotation_a.z +
                                             nd.rotation_a.w * nd.rotation_a.w;
                            const float mb = nd.rotation_b.x * nd.rotation_b.x +
                                             nd.rotation_b.y * nd.rotation_b.y +
                                             nd.rotation_b.z * nd.rotation_b.z +
                                             nd.rotation_b.w * nd.rotation_b.w;
                            if (ma > 0.99f && ma < 1.01f) {
                                ++*rot_a_unit;
                            }
                            if (mb > 0.99f && mb < 1.01f) {
                                ++*rot_b_unit;
                            }
                            const float* pv = &nd.position_a.x;
                            bool fin = true;
                            for (int c = 0; c < 3; ++c) {
                                const float v = pv[c];
                                if (!(v > -1.0e6f && v < 1.0e6f)) {
                                    fin = false;
                                }
                            }
                            const float* qv = &nd.position_b.x;
                            for (int c = 0; c < 3; ++c) {
                                const float v = qv[c];
                                if (!(v > -1.0e6f && v < 1.0e6f)) {
                                    fin = false;
                                }
                            }
                            if (fin) {
                                ++*pos_finite;
                            }
                        }
                        if (idx_ok) {
                            ++*index_self_ok;
                        }
                        if (topo_ok) {
                            ++*topological_ok;
                        }
                        if (children + 1 == nc) {
                            ++*child_sum_ok;
                        }
                        // Contiguous children: the block a node points at must be
                        // in range and must point back. Counted per LINK, not per
                        // asset, and the link count is reported so the fixture can
                        // prove the loop was not empty.
                        bool blk_range = true, blk_par = true;
                        for (uint32_t k = 0; k < nc; ++k) {
                            const auto& nd = recs[k];
                            const uint32_t c = nd.child_count;
                            if (c == 0) {
                                continue;
                            }
                            const uint32_t start = k + nd.first_child_offset;
                            if (start + c > nc) {
                                blk_range = false;
                                continue;
                            }
                            for (uint32_t j = 0; j < c; ++j) {
                                ++*child_links_seen;
                                if (recs[start + j].parent_index != static_cast<uint8_t>(k)) {
                                    blk_par = false;
                                }
                            }
                        }
                        if (blk_range) {
                            ++*child_range_ok;
                        }
                        if (blk_par) {
                            ++*child_parents_ok;
                        }
                    }
                    if (blob != 0 && bsz > 0 && bsz < 0x400000 && nc > 0 && nc <= kMaxNodes &&
                        a->node_names != nullptr && a->node_hashes != nullptr) {
                        for (uint32_t k = 0; k < nc; ++k) {
                            ++*nodes_total;
                            const char* nm = a->node_names[k];
                            const auto p = reinterpret_cast<uintptr_t>(nm);
                            if (p < blob || p >= blob + bsz) {
                                continue;  // counted in nodes_total, absent from in_blob
                            }
                            ++*in_blob;
                            uint32_t len = 0;
                            bool good = true;
                            for (; len < kMaxName; ++len) {
                                const unsigned char c = static_cast<unsigned char>(nm[len]);
                                if (c == 0) {
                                    break;
                                }
                                if (c < 0x20 || c > 0x7E) {
                                    good = false;
                                    break;
                                }
                            }
                            if (!good || len == 0 || len >= kMaxName) {
                                continue;
                            }
                            ++*printable;
                            const uint32_t h = a->node_hashes[k];
                            // Same name must always hash the same. The first sighting
                            // defines it; every later one is a real comparison.
                            size_t slot = n_names;
                            for (size_t i = 0; i < n_names; ++i) {
                                if (name_equals_i(names[i], nm, kMaxName)) {
                                    slot = i;
                                    break;
                                }
                            }
                            if (slot == n_names) {
                                if (n_names < kMaxNames) {
                                    names[n_names] = nm;
                                    hashes[n_names] = h;
                                    counts[n_names] = 1;
                                    ++n_names;
                                }
                                ++*consistent;  // first sighting is consistent by definition
                            } else {
                                ++counts[slot];
                                if (hashes[slot] == h) {
                                    ++*consistent;
                                }
                            }
                        }
                    }
                }
                ++sampled;
            }
            ++n;
            cur = cur->next;
        }
        if (cur != head) {
            return -1;
        }
        *distinct = n_names;
        for (size_t i = 0; i < n_names; ++i) {
            if (counts[i] > 1) {
                ++*repeated;
            }
            for (size_t j = i + 1; j < n_names; ++j) {
                if (hashes[i] == hashes[j]) {
                    ++*collisions;
                }
            }
        }
        result = static_cast<int64_t>(n_assets);
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        result = -1;
    }
    return result;
}

std::optional<CClientMgr::NodeCheck> CClientMgr::check_model_nodes(size_t max) const {
    constexpr size_t kModelType = 1;
    if (regenny() == nullptr || kModelType >= object_list_count()) {
        return std::nullopt;
    }
    NodeCheck out{};
    const int64_t n = seh_check_nodes(&regenny()->object_lists[kModelType], max, &out.nodes_total,
                                     &out.names_in_blob, &out.names_printable, &out.distinct_names,
                                     &out.repeated_names, &out.hash_consistent,
                                     &out.hash_collisions, &out.count_dup_ok, &out.records_in_blob,
                                     &out.root_is_255, &out.index_self_ok, &out.topological_ok,
                                     &out.child_sum_ok, &out.rot_a_unit, &out.rot_b_unit,
                                     &out.pos_finite, &out.child_block_in_range,
                                     &out.child_parents_ok, &out.child_links_seen,
                                     max_object_walk);
    if (n < 0) {
        return std::nullopt;
    }
    out.assets = static_cast<size_t>(n);
    return out;
}

std::optional<CClientMgr::AssetCheck> CClientMgr::check_model_assets(size_t max) const {
    constexpr size_t kModelType = 1;
    if (regenny() == nullptr || kModelType >= object_list_count()) {
        return std::nullopt;
    }
    AssetCheck out{};
    const int64_t n = seh_check_assets(&regenny()->object_lists[kModelType], max, &out.self_ref_ok,
                                      &out.radius_dup_ok, &out.name_at_blob, &out.name_readable,
                                      &out.refcount_ge, &out.refcount_exact, &out.blob_size_sane,
                                      &out.arrays_in_blob, &out.write_order_ok, &out.count_matches,
                                      &out.count_dup_ok, max_object_walk);
    if (n < 0) {
        return std::nullopt;
    }
    out.assets = static_cast<size_t>(n);
    return out;
}

// POD-only SEH helper for the animation-name tables. Returns distinct assets, or
// -1 on fault / non-termination.
static int64_t seh_check_anim_tables(const regenny::CClientMgrListLink* head, size_t max,
                                    size_t* sane, size_t* ascending, size_t* entries_total,
                                    size_t* max_entries, size_t cap) {
    constexpr size_t kMaxAssets = 512;
    constexpr size_t kMaxEntries = 65536;
    const regenny::LTModelAsset* seen[kMaxAssets]{};
    size_t n_assets = 0;
    int64_t result = -1;
    KANANLIB_SEH_TRY {
        const regenny::CClientMgrListLink* cur = head->next;
        size_t n = 0, sampled = 0;
        while (cur != head && n < cap) {
            if (sampled < max) {
                const auto* obj = reinterpret_cast<const regenny::LTModelObject*>(
                    reinterpret_cast<uintptr_t>(cur) - offsetof(regenny::LTObject, list_link));
                const auto* a = obj->record.asset;
                bool fresh = a != nullptr;
                for (size_t i = 0; i < n_assets && fresh; ++i) {
                    if (seen[i] == a) {
                        fresh = false;
                    }
                }
                if (fresh && n_assets < kMaxAssets) {
                    seen[n_assets++] = a;
                    const auto* first = a->anim_names.first;
                    const auto* last = a->anim_names.last;
                    if (first != nullptr && last >= first) {
                        const size_t count = static_cast<size_t>(last - first);
                        if (count > 0 && count <= kMaxEntries) {
                            ++*sane;
                            *entries_total += count;
                            if (count > *max_entries) {
                                *max_entries = count;
                            }
                            bool asc = true;
                            for (size_t k = 1; k < count; ++k) {
                                if (first[k].name_hash < first[k - 1].name_hash) {
                                    asc = false;
                                    break;
                                }
                            }
                            if (asc) {
                                ++*ascending;
                            }
                        }
                    }
                }
                ++sampled;
            }
            ++n;
            cur = cur->next;
        }
        if (cur != head) {
            return -1;
        }
        result = static_cast<int64_t>(n_assets);
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        result = -1;
    }
    return result;
}

std::optional<CClientMgr::AnimTableCheck> CClientMgr::check_anim_tables(size_t max) const {
    constexpr size_t kModelType = 1;
    if (regenny() == nullptr || kModelType >= object_list_count()) {
        return std::nullopt;
    }
    AnimTableCheck out{};
    const int64_t n = seh_check_anim_tables(&regenny()->object_lists[kModelType], max,
                                           &out.table_sane, &out.hashes_ascending,
                                           &out.entries_total, &out.max_entries, max_object_walk);
    if (n < 0) {
        return std::nullopt;
    }
    out.assets = static_cast<size_t>(n);
    return out;
}

std::optional<CClientMgr::GeometryCheck> CClientMgr::check_object_geometry(
    size_t max_per_type) const {
    // AGGREGATES the public bounds primitives instead of recomputing them. The AABB
    // identity, the two-state radius rule and the relative tolerance all live in Object.hpp
    // now, where a mod can use them; this counts.
    GeometryCheck out{};
    std::vector<ObjectSnapshot> snaps(max_per_type);
    for (size_t t = 0; t < object_list_count(); ++t) {
        const auto taken =
            snapshot_objects(static_cast<ObjectType>(t), snaps.data(), max_per_type);
        if (!taken.has_value()) {
            return std::nullopt;  // a faulted bucket invalidates the whole report
        }
        for (size_t i = 0; i < *taken; ++i) {
            const auto* obj = reinterpret_cast<const regenny::LTObject*>(snaps[i].address);
            const auto box = world_aabb(obj);
            const auto fresh = world_aabb_is_current(obj);
            const auto rad = bounding_radius(obj);
            const auto dims = object_dims(obj);
            if (!box.has_value() || !fresh.has_value() || !rad.has_value() ||
                !dims.has_value()) {
                continue;
            }
            ++out.sampled;
            // One predicate covers both halves of the AABB identity, so the two counters
            // move together -- kept separate because the struct's contract says so and a
            // caller may be comparing them across builds.
            if (*fresh) {
                ++out.aabb_min_ok;
                ++out.aabb_max_ok;
            }
            if (rad->from_dims) {
                ++out.radius_sized;
            } else if (rad->unsized) {
                ++out.radius_pristine;
            }
            if (dims->x >= 0.0f && dims->y >= 0.0f && dims->z >= 0.0f) {
                ++out.dims_nonneg;
            }
        }
    }
    return out;
}

namespace {

// Walks one bucket's objects out into the world tree. POD-only for the SEH
// guard. `img_base`/`img_size` bound FEAR2.exe so a list element can be
// classified as object-link vs node-head.
//
// Returns the number of objects examined, or -1 on fault / non-termination.
int64_t seh_check_tree(const regenny::CClientMgrListLink* head, size_t max, uintptr_t img_base,
                       size_t img_size, CClientMgr::WorldTreeCheck* out, size_t cap) {
    int64_t result = -1;
    KANANLIB_SEH_TRY {
        const regenny::CClientMgrListLink* cur = head->next;
        size_t n = 0, seen = 0;
        while (cur != head && n < cap) {
            if (seen < max) {
                const auto* o = reinterpret_cast<const regenny::LTObject*>(
                    reinterpret_cast<uintptr_t>(cur) - offsetof(regenny::LTObject, list_link));
                const auto* link = &o->world_tree_link;

                if (link->next == link) {
                    ++out->unlinked;
                } else {
                    ++out->linked;
                    // Find the node head: the one element that is not <object>+0xC4.
                    const regenny::LTWorldTreeLink* e = link->next;
                    const regenny::LTWorldTreeNode* node = nullptr;
                    for (size_t steps = 0; e != link && steps < cap; ++steps) {
                        const auto cand = reinterpret_cast<uintptr_t>(e) -
                                          offsetof(regenny::LTObject, world_tree_link);
                        const uintptr_t vt =
                            *reinterpret_cast<const uintptr_t*>(cand); // vtable slot of a candidate
                        if (vt < img_base || vt >= img_base + img_size) {
                            node = reinterpret_cast<const regenny::LTWorldTreeNode*>(e);
                            break;
                        }
                        e = e->next;
                    }
                    if (node != nullptr) {
                        ++out->node_found;
                        // Climb to the root, requiring occupied_count to never drop.
                        const regenny::LTWorldTreeNode* p = node;
                        uint32_t last = p->occupied_count;
                        bool mono = true;
                        size_t hops = 0;
                        while (p->parent_offset != 0 && hops < 64) {
                            p = reinterpret_cast<const regenny::LTWorldTreeNode*>(
                                reinterpret_cast<uintptr_t>(p) -
                                sizeof(regenny::LTWorldTreeNode) * p->parent_offset);
                            if (p->occupied_count < last) {
                                mono = false;
                            }
                            last = p->occupied_count;
                            ++hops;
                        }
                        if (hops > out->max_depth) {
                            out->max_depth = hops;
                        }
                        if (p->parent_offset == 0) {
                            ++out->root_reached;
                            const auto r = reinterpret_cast<uintptr_t>(p);
                            if (out->root == 0) {
                                out->root = r;
                            } else if (out->root != r) {
                                ++out->root_mismatches;
                            }
                        }
                        if (mono) {
                            ++out->counts_monotonic;
                        }
                    }
                }
                ++seen;
            }
            ++n;
            cur = cur->next;
        }
        result = (cur == head) ? static_cast<int64_t>(seen) : -1;
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        result = -1;
    }
    return result;
}

} // namespace

std::optional<CClientMgr::WorldTreeCheck> CClientMgr::check_world_tree(size_t max_objects) const {
    const auto* exe = Modules::get().exe();
    if (exe == nullptr || exe->base == 0 || exe->size == 0) {
        return std::nullopt; // cannot classify list elements without the image range
    }
    WorldTreeCheck out{};
    for (size_t t = 0; t < object_list_count(); ++t) {
        const int64_t n = seh_check_tree(&regenny()->object_lists[t], max_objects, exe->base,
                                        exe->size, &out, max_object_walk);
        if (n < 0) {
            return std::nullopt;
        }
        out.objects_seen += static_cast<size_t>(n);
    }
    // Compare the root we climbed to against the one the engine stores. Two
    // independent routes to the same address; neither side is a baseline of
    // ours, so this stays valid in any level.
    if (const auto* bsp = WorldBSP::get()) {
        KANANLIB_SEH_TRY {
            out.bsp_root = reinterpret_cast<uintptr_t>(bsp->world_tree_root);
        }
        KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
            out.bsp_root = 0;
        }
    }
    out.root_matches_bsp = out.bsp_root != 0 && out.bsp_root == out.root;
    return out;
}

namespace {

// Recomputes OT_MODEL's cull radius from its two inputs. POD-only for the SEH
// guard. Returns objects examined, or -1 on fault / non-termination.
int64_t seh_check_model_volumes(const regenny::CClientMgrListLink* head, size_t max,
                                size_t* vis_pos, size_t* radius_ok, size_t* asset_nonnull,
                                size_t* asset_radius_eq, size_t cap) {
    int64_t result = -1;
    KANANLIB_SEH_TRY {
        const regenny::CClientMgrListLink* cur = head->next;
        size_t n = 0, seen = 0;
        while (cur != head && n < cap) {
            if (seen < max) {
                const auto* m = reinterpret_cast<const regenny::LTModelObject*>(
                    reinterpret_cast<uintptr_t>(cur) - offsetof(regenny::LTObject, list_link));
                if (m->vis_radius > 0.0f) {
                    ++*vis_pos;
                }
                // The engine's own expression: OT_MODEL_GetCullVolume computes
                // vis_radius * scale for the sphere radius.
                const float r = m->vis_radius * m->base.scale;
                if (r > 0.0f && r == r && r < 1.0e30f) {
                    ++*radius_ok;
                }
                // The asset link. asset->radius and vis_radius are the same
                // value stored twice (215/215 live); the object's copy is a
                // cache, so a divergence means either a moved offset or a stale
                // cache -- both worth surfacing.
                if (m->record.asset != nullptr) {
                    ++*asset_nonnull;
                    if (approx_eq(m->record.asset->radius, m->vis_radius)) {
                        ++*asset_radius_eq;
                    }
                }
                ++seen;
            }
            ++n;
            cur = cur->next;
        }
        result = (cur == head) ? static_cast<int64_t>(seen) : -1;
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        result = -1;
    }
    return result;
}

// Classifies OT_SPRITE volumes. The provider hard-codes which KIND bytes are
// AABB-shaped, so the split is an interface fact while the kind values are not.
// Each shape is sanity-checked on its own fields: ordering for the AABB, a
// finite positive radius for the sphere.
int64_t seh_check_sprite_volumes(const regenny::CClientMgrListLink* head, size_t max,
                                 size_t* aabb_n, size_t* sphere_n, size_t* ordered,
                                 size_t* radius_ok, size_t cap) {
    int64_t result = -1;
    KANANLIB_SEH_TRY {
        const regenny::CClientMgrListLink* cur = head->next;
        size_t n = 0, seen = 0;
        while (cur != head && n < cap) {
            if (seen < max) {
                const auto* s = reinterpret_cast<const regenny::LTSpriteObject*>(
                    reinterpret_cast<uintptr_t>(cur) - offsetof(regenny::LTObject, list_link));
                const uint8_t k = s->kind;
                if (k == 3 || k == 4 || k == 7 || k == 9) {
                    ++*aabb_n;
                    if (s->aabb_min.x <= s->aabb_max.x && s->aabb_min.y <= s->aabb_max.y &&
                        s->aabb_min.z <= s->aabb_max.z) {
                        ++*ordered;
                    }
                } else {
                    ++*sphere_n;
                    const float r = s->radius;
                    if (r > 0.0f && r == r && r < 1.0e30f) {
                        ++*radius_ok;
                    }
                }
                ++seen;
            }
            ++n;
            cur = cur->next;
        }
        result = (cur == head) ? static_cast<int64_t>(seen) : -1;
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        result = -1;
    }
    return result;
}

// Classifies OT_PARTICLESYSTEM volume kinds. The provider returns this byte, so
// 1 and 2 are the only values the interface can mean.
int64_t seh_check_particle_volumes(const regenny::CClientMgrListLink* head, size_t max,
                                   size_t* type_ok, size_t* sphere, size_t* aabb, size_t cap) {
    int64_t result = -1;
    KANANLIB_SEH_TRY {
        const regenny::CClientMgrListLink* cur = head->next;
        size_t n = 0, seen = 0;
        while (cur != head && n < cap) {
            if (seen < max) {
                const auto* p = reinterpret_cast<const regenny::LTParticleSystemObject*>(
                    reinterpret_cast<uintptr_t>(cur) - offsetof(regenny::LTObject, list_link));
                const uint8_t k = p->cull_volume_type;
                if (k == 1 || k == 2) {
                    ++*type_ok;
                }
                if (k == 1) {
                    ++*sphere;
                } else if (k == 2) {
                    ++*aabb;
                }
                ++seen;
            }
            ++n;
            cur = cur->next;
        }
        result = (cur == head) ? static_cast<int64_t>(seen) : -1;
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        result = -1;
    }
    return result;
}

} // namespace

std::optional<CClientMgr::CullVolumeCheck> CClientMgr::check_cull_volumes(
    size_t max_per_type) const {
    constexpr size_t kModel = 1;
    constexpr size_t kParticle = 6;
    if (kParticle >= object_list_count()) {
        return std::nullopt;
    }
    CullVolumeCheck out{};
    const int64_t nm = seh_check_model_volumes(&regenny()->object_lists[kModel], max_per_type,
                                              &out.model_vis_radius_pos, &out.model_radius_ok,
                                              &out.model_asset_nonnull,
                                              &out.model_asset_radius_eq, max_object_walk);
    if (nm < 0) {
        return std::nullopt;
    }
    out.models = static_cast<size_t>(nm);

    const int64_t np = seh_check_particle_volumes(&regenny()->object_lists[kParticle],
                                                 max_per_type, &out.particle_type_ok,
                                                 &out.particle_sphere, &out.particle_aabb,
                                                 max_object_walk);
    if (np < 0) {
        return std::nullopt;
    }
    out.particles = static_cast<size_t>(np);

    constexpr size_t kSprite = 3;
    const int64_t ns = seh_check_sprite_volumes(&regenny()->object_lists[kSprite], max_per_type,
                                               &out.sprite_aabb, &out.sprite_sphere,
                                               &out.sprite_aabb_ordered, &out.sprite_radius_ok,
                                               max_object_walk);
    if (ns < 0) {
        return std::nullopt;
    }
    out.sprites = static_cast<size_t>(ns);
    return out;
}

namespace {

// Walks one bucket, checking the attachment graph and the per-object slot index.
// POD-only for the SEH guard. Returns objects examined, or -1 on fault /
// non-termination.
int64_t seh_check_attachments(const regenny::CClientMgrListLink* head, size_t max,
                              CClientMgr::AttachmentCheck* out, size_t cap) {
    int64_t result = -1;
    KANANLIB_SEH_TRY {
        const regenny::CClientMgrListLink* cur = head->next;
        size_t n = 0, seen = 0;
        while (cur != head && n < cap) {
            if (seen < max) {
                const auto* o = reinterpret_cast<const regenny::LTObject*>(
                    reinterpret_cast<uintptr_t>(cur) - offsetof(regenny::LTObject, list_link));
                const auto addr = reinterpret_cast<uintptr_t>(o);

                if (reinterpret_cast<uintptr_t>(o->self) == addr) {
                    ++out->self_ptr_ok;
                }

                const auto* plink = &o->standing_on_link;
                const bool self_linked = plink->prev == plink && plink->next == plink;
                const bool has_parent = o->standing_on != nullptr;
                if (has_parent) {
                    ++out->parented;
                } else {
                    ++out->parentless;
                }
                // The biconditional: an object stands on nothing exactly when its
                // standing_on_link is self-pointing. Written as `== !` rather than
                // `!=` because the intent is an equivalence, not a difference.
                if (has_parent == !self_linked) {
                    ++out->link_consistent;
                }

                // Walk the objects standing on ME. Each entry is a standing_on_link;
                // the owning object is recovered through `self`, which sits one field
                // past the link -- that is exactly why the engine keeps it.
                {
                    const auto* h = &o->objects_standing_on;
                    const regenny::CClientMgrListLink* c = h->next;
                    for (size_t k = 0; c != h && k < cap; ++k) {
                        ++out->children_reached;
                        const auto* child = *reinterpret_cast<const regenny::LTObject* const*>(
                            reinterpret_cast<uintptr_t>(c) + sizeof(regenny::CClientMgrListLink));
                        // Each object in the list must name ME as what it stands on.
                        if (child != nullptr &&
                            reinterpret_cast<uintptr_t>(child->standing_on) == addr) {
                            ++out->child_parent_ok;
                        }
                        c = c->next;
                    }
                }

                // Owned (game-side) objects.
                {
                    const auto* h = &o->owned_list;
                    const regenny::CClientMgrListLink* e = h->next;
                    size_t k = 0;
                    for (; e != h && k < cap; ++k) {
                        ++out->owned_entries;
                        e = e->next;
                    }
                    if (k != 0) {
                        ++out->owned_nonempty;
                    }
                }

                if (o->slot_index == 0xFFFFFFFFu) {
                    ++out->index_none;
                } else {
                    ++out->index_set;
                }

                // shared_ref, validated against itself: the record keeps its own
                // address in two places and a live refcount. Nothing external is
                // consulted, so this cannot drift with the scene.
                if (const auto* ref = o->shared_ref) {
                    ++out->shared_refs;
                    const uint32_t c = ref->refcount;
                    if (c != 0 && c < 100000u) {
                        ++out->shared_ref_count_ok;
                    }
                    const auto ra = reinterpret_cast<uintptr_t>(ref);
                    if (reinterpret_cast<uintptr_t>(ref->self_08) == ra &&
                        reinterpret_cast<uintptr_t>(ref->self_1C) == ra) {
                        ++out->shared_ref_self_ok;
                    }
                }
                ++seen;
            }
            ++n;
            cur = cur->next;
        }
        // `n` counted every element; `seen` only the sampled ones. Publishing
        // both is what lets a caller distinguish a complete walk from a sample.
        out->listed += n;
        result = (cur == head) ? static_cast<int64_t>(seen) : -1;
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        result = -1;
    }
    return result;
}

} // namespace

std::optional<CClientMgr::AttachmentCheck> CClientMgr::check_attachments(
    size_t max_per_type) const {
    AttachmentCheck out{};
    for (size_t t = 0; t < object_list_count(); ++t) {
        const int64_t n =
            seh_check_attachments(&regenny()->object_lists[t], max_per_type, &out, max_object_walk);
        if (n < 0) {
            return std::nullopt;
        }
        out.objects += static_cast<size_t>(n);
    }
    return out;
}

namespace {

// Recomputes the cull volume from the object's TYPED fields and compares it to
// the copy the engine stored on the spatial record. POD-only for the SEH guard.
//
// `type` is passed in rather than read from the object so the comparison is
// driven by the bucket the object was found in, which the schema already proves
// equals LTObject.type -- using the field would make the check partly circular.
//
// Returns objects examined, or -1 on fault / non-termination.
int64_t seh_check_records(const regenny::CClientMgrListLink* head, size_t type, size_t max,
                          CClientMgr::SpatialRecordCheck* out, size_t cap) {
    int64_t result = -1;
    KANANLIB_SEH_TRY {
        const regenny::CClientMgrListLink* cur = head->next;
        size_t n = 0, seen = 0;
        while (cur != head && n < cap) {
            if (seen < max) {
                const auto* o = reinterpret_cast<const regenny::LTObject*>(
                    reinterpret_cast<uintptr_t>(cur) - offsetof(regenny::LTObject, list_link));
                const auto* rec = o->spatial_record;
                if (rec != nullptr) {
                    if (rec->object == reinterpret_cast<const void*>(o)) {
                        ++out->backpointer_ok;
                    }
                    // THE VOLUME COMPARISON USED TO LIVE HERE, as a second copy of the
                    // per-type cull rule. It is now sdk::cull_volume /
                    // sdk::computed_cull_volume in Object.hpp, and check_spatial_records
                    // aggregates those instead -- see the snapshot loop there. Only the
                    // genuine LINKED-LIST traversal stays inside this guard, because that
                    // is what actually needs to walk engine memory step by step.

                    // Walk the record's entry list. Two independent things are
                    // being checked: that entry_count agrees with the walk (a
                    // record-side offset), and that the hit-side doubly-linked
                    // pointers point back at each other (an entry-side offset).
                    size_t walked = 0;
                    bool rec_ok = true, hit_ok = true;
                    for (const regenny::LTSpatialEntry* e = rec->entry_list;
                         e != nullptr && walked < 2048; e = e->record_next) {
                        ++walked;
                        if (e->record != reinterpret_cast<const void*>(rec)) {
                            rec_ok = false;
                        }
                        // hit_next's hit_prev must come back here, and likewise
                        // hit_prev's hit_next. An entry with no hit_prev is the
                        // head, so *hit_head must be this entry.
                        if (e->hit_next != nullptr && e->hit_next->hit_prev != e) {
                            hit_ok = false;
                        }
                        if (e->hit_prev != nullptr) {
                            if (e->hit_prev->hit_next != e) {
                                hit_ok = false;
                            }
                        } else if (e->hit_head == nullptr ||
                                   *e->hit_head != reinterpret_cast<const void*>(e)) {
                            hit_ok = false;
                        }

                        // The sector this entry associates with. hit_head points
                        // AT its entry_list slot, and entry_list is the sector's
                        // FIRST field, so the slot address IS the sector -- which
                        // is how the sector is reachable with no global pointer.
                        const auto* sec =
                            reinterpret_cast<const regenny::LTVisSector*>(e->hit_head);
                        if (sec != nullptr) {
                            if (sec->aabb_min.x <= sec->aabb_max.x &&
                                sec->aabb_min.y <= sec->aabb_max.y &&
                                sec->aabb_min.z <= sec->aabb_max.z) {
                                ++out->entry_sector_aabb_ok;
                            }
                            // Unit normals are what make the normal/distance
                            // split real; a wrong offset scatters the lengths.
                            bool units = true;
                            if (sec->plane_count != 0 && sec->planes != nullptr) {
                                for (uint8_t pi = 0; pi < sec->plane_count; ++pi) {
                                    const float nx = sec->planes[pi].normal.x;
                                    const float ny = sec->planes[pi].normal.y;
                                    const float nz = sec->planes[pi].normal.z;
                                    const float len2 = nx * nx + ny * ny + nz * nz;
                                    if (len2 < 0.98f || len2 > 1.02f) {
                                        units = false;
                                    }
                                }
                            }
                            if (units) {
                                ++out->entry_sector_planes_ok;
                            }
                        }
                    }

                    // The visibility gate. Transcribed from
                    // LTObjectOwner_UpdateSpatialRecord: it collects only when
                    // this holds, and calls Release otherwise.
                    const bool gate = (o->flags & 1u) != 0 && (o->flags2 & 0x700u) == 0;
                    if (gate) {
                        ++out->gate_open;
                    }
                    if (walked != 0) {
                        ++out->records_with_entries;
                        if (!gate) {
                            ++out->gated_violations;
                        }
                    }
                    out->entries += walked;
                    if (walked == rec->entry_count) {
                        ++out->count_matches_walk;
                    }
                    if (rec_ok) {
                        ++out->entry_record_ok;
                    }
                    if (hit_ok) {
                        ++out->hit_links_ok;
                    }
                }
                ++seen;
            }
            ++n;
            cur = cur->next;
        }
        result = (cur == head) ? static_cast<int64_t>(seen) : -1;
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        result = -1;
    }
    return result;
}

} // namespace

std::optional<CClientMgr::SpatialRecordCheck> CClientMgr::check_spatial_records(
    size_t max_per_type) const {
    SpatialRecordCheck out{};
    for (size_t t = 0; t < object_list_count(); ++t) {
        const int64_t n =
            seh_check_records(&regenny()->object_lists[t], t, max_per_type, &out, max_object_walk);
        if (n < 0) {
            return std::nullopt;
        }
        out.objects += static_cast<size_t>(n);
    }
    // THE VOLUME COMPARISON, aggregated over the public primitives rather than recomputed
    // here. Two producers of one volume: the engine wrote its copy on the spatial record,
    // and computed_cull_volume derives it from the object's typed fields by the per-type
    // rule. Where they agree, the rule is right; where they do not, the record is stale.
    std::vector<ObjectSnapshot> snaps(max_per_type);
    for (size_t t = 0; t < object_list_count(); ++t) {
        const auto taken =
            snapshot_objects(static_cast<ObjectType>(t), snaps.data(), max_per_type);
        if (!taken.has_value()) {
            continue;
        }
        for (size_t i = 0; i < *taken; ++i) {
            const auto* obj = reinterpret_cast<const regenny::LTObject*>(snaps[i].address);
            const auto computed = computed_cull_volume(obj);
            const auto current = cull_volume_is_current(obj);
            if (!computed.has_value() || !current.has_value()) {
                continue;  // no spatial record: not a volume claim either way
            }
            if (!*current) {
                ++out.unexplained;
            } else if (computed->shape == CullShape::None) {
                ++out.volume_gated;
            } else {
                ++out.volume_matched;
            }
        }
    }
    return out;
}

namespace {


} // namespace

std::optional<CClientMgr::RenderFlagCheck> CClientMgr::check_render_flags(
    size_t max_per_type) const {
    // AGGREGATES sdk::is_tree_eligible and WorldBSP::is_linked instead of recomputing the
    // engine's mask inline. The predicate was the valuable part -- it is the gate on every
    // LTWorldTree_AddObject call -- so it now lives in Object.hpp where a consumer can ask it
    // before trusting a proximity query.
    RenderFlagCheck out{};
    std::vector<ObjectSnapshot> snaps(max_per_type);
    for (size_t t = 0; t < object_list_count(); ++t) {
        const auto taken =
            snapshot_objects(static_cast<ObjectType>(t), snaps.data(), max_per_type);
        if (!taken.has_value()) {
            return std::nullopt;
        }
        for (size_t i = 0; i < *taken; ++i) {
            const auto* obj = reinterpret_cast<const regenny::LTObject*>(snaps[i].address);
            const auto eligible = is_tree_eligible(obj);
            const auto linked = WorldBSP::is_linked(obj);
            if (!eligible.has_value() || !linked.has_value()) {
                continue;
            }
            ++out.objects;
            if (*eligible) {
                ++out.renderable;
            } else {
                // The suppress clause is the half worth counting separately: an object can
                // fail the gate by carrying 0x200 or by carrying none of 0x10C30, and only
                // the first is a deliberate suppression.
                if (const auto info = object_info(obj);
                    info.has_value() && (info->flags & 0x200u) != 0) {
                    ++out.suppressed;
                    if (*linked) {
                        ++out.suppressed_linked;
                    }
                }
            }
            if (*linked) {
                ++out.linked;
            }
            if (*eligible && !*linked) {
                ++out.renderable_not_linked;
            }
            if (*linked && !*eligible) {
                ++out.linked_not_renderable;
            }
        }
    }
    return out;
}

} // namespace sdk
