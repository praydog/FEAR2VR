#include "CClientMgr.hpp"

#include <windows.h>

#include <cmath>

#include <utility/Seh.hpp>

#include "regenny/regenny/CClientMgrCounterNode.hpp"
#include "regenny/regenny/LTCameraObject.hpp"
#include "regenny/regenny/LTMemoryPool.hpp"

#include "CClientShell.hpp"
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

// POD-only SEH helper. Walks type-5 objects and checks the two relationships
// the LTCameraObject mapping asserts. Every offset comes from the generated
// schema -- no literal appears here.
//
// Returns the number sampled, or -1 on fault / non-termination.
int64_t seh_check_type5(const regenny::CClientMgrListLink* head, size_t max,
                        size_t* rot_ok, size_t* inv_ok, size_t* det_ok, size_t cap) {
    int64_t result = -1;
    KANANLIB_SEH_TRY {
        const regenny::CClientMgrListLink* cur = head->next;
        size_t n = 0, sampled = 0;
        while (cur != head && n < cap) {
            if (sampled < max) {
                const auto* obj = reinterpret_cast<const regenny::LTCameraObject*>(
                    reinterpret_cast<uintptr_t>(cur) - offsetof(regenny::LTObject, list_link));
                const float qx = obj->base.rotation.x, qy = obj->base.rotation.y;
                const float qz = obj->base.rotation.z, qw = obj->base.rotation.w;
                const float R[3][3] = {
                    {1 - 2 * (qy * qy + qz * qz), 2 * (qx * qy - qw * qz), 2 * (qx * qz + qw * qy)},
                    {2 * (qx * qy + qw * qz), 1 - 2 * (qx * qx + qz * qz), 2 * (qy * qz - qw * qx)},
                    {2 * (qx * qz - qw * qy), 2 * (qy * qz + qw * qx), 1 - 2 * (qx * qx + qy * qy)},
                };
                const float* M = obj->world_transform.m;
                const float* I = obj->inverse_transform.m;

                float dr = 0.0f, di = 0.0f;
                for (int r = 0; r < 3; ++r) {
                    for (int c = 0; c < 3; ++c) {
                        const float a = M[r * 4 + c] - R[r][c];
                        dr = (a < 0 ? -a : a) > dr ? (a < 0 ? -a : a) : dr;
                        const float b = I[r * 4 + c] - M[c * 4 + r];
                        di = (b < 0 ? -b : b) > di ? (b < 0 ? -b : b) : di;
                    }
                }
                // t2 == -R1^T * t1
                float dt = 0.0f;
                for (int r = 0; r < 3; ++r) {
                    const float e = -(M[0 * 4 + r] * M[0 * 4 + 3] + M[1 * 4 + r] * M[1 * 4 + 3] +
                                      M[2 * 4 + r] * M[2 * 4 + 3]);
                    const float b = I[r * 4 + 3] - e;
                    dt = (b < 0 ? -b : b) > dt ? (b < 0 ? -b : b) : dt;
                }
                const float det = M[0] * (M[5] * M[10] - M[6] * M[9]) -
                                  M[1] * (M[4] * M[10] - M[6] * M[8]) +
                                  M[2] * (M[4] * M[9] - M[5] * M[8]);
                if (dr < 0.002f) {
                    ++*rot_ok;
                }
                if (di < 0.002f && dt < 0.05f) {
                    ++*inv_ok;
                }
                const float dd = det - 1.0f;
                if ((dd < 0 ? -dd : dd) < 0.01f) {
                    ++*det_ok;
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

// Walks one bucket, checking the identities SetDims establishes plus the sign
// constraint on dims. Offsets all come from the generated schema.
//
// The radius is classified into one of two states rather than tested against a
// single formula -- see GeometryCheck for why that distinction must not be
// collapsed or absorbed into a wider tolerance.
//
// Returns the number sampled, or -1 on fault / non-termination.
int64_t seh_check_geometry(const regenny::CClientMgrListLink* head, size_t max, size_t* mn_ok,
                           size_t* mx_ok, size_t* r_sized, size_t* r_pristine, size_t* nonneg,
                           size_t cap) {
    int64_t result = -1;
    KANANLIB_SEH_TRY {
        const regenny::CClientMgrListLink* cur = head->next;
        size_t n = 0, sampled = 0;
        while (cur != head && n < cap) {
            if (sampled < max) {
                const auto* o = reinterpret_cast<const regenny::LTObject*>(
                    reinterpret_cast<uintptr_t>(cur) - offsetof(regenny::LTObject, list_link));

                const float px = o->position.x, py = o->position.y, pz = o->position.z;
                const float dx = o->dims.x, dy = o->dims.y, dz = o->dims.z;

                if (approx_eq(o->aabb_min.x, px - dx) && approx_eq(o->aabb_min.y, py - dy) &&
                    approx_eq(o->aabb_min.z, pz - dz)) {
                    ++*mn_ok;
                }
                if (approx_eq(o->aabb_max.x, px + dx) && approx_eq(o->aabb_max.y, py + dy) &&
                    approx_eq(o->aabb_max.z, pz + dz)) {
                    ++*mx_ok;
                }
                // SetDims adds the double at 0x66FBB8, which is exactly 0.1.
                // An object it never ran on keeps the constructor's zeroes, so
                // radius 0 with dims 0 is correct-but-unsized, NOT a violation.
                const float len = static_cast<float>(sqrt(static_cast<double>(dx) * dx +
                                                          static_cast<double>(dy) * dy +
                                                          static_cast<double>(dz) * dz));
                if (dx == 0.0f && dy == 0.0f && dz == 0.0f && o->radius == 0.0f) {
                    ++*r_pristine;
                } else if (approx_eq(o->radius, len + 0.1f)) {
                    ++*r_sized;
                }
                if (dx >= 0.0f && dy >= 0.0f && dz >= 0.0f) {
                    ++*nonneg;
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

} // namespace

std::optional<CClientMgr::TransformCheck> CClientMgr::check_type5_transforms(size_t max) const {
    // Bank index 4 serves type 5; go through the type, not the index.
    constexpr auto kType = static_cast<ObjectType>(5);
    if (static_cast<size_t>(kType) >= object_list_count()) {
        return std::nullopt;
    }
    TransformCheck out{};
    const int64_t n = seh_check_type5(&regenny()->object_lists[static_cast<size_t>(kType)], max,
                                     &out.rotation_match, &out.inverse_ok, &out.det_ok,
                                     max_object_walk);
    if (n < 0) {
        return std::nullopt;
    }
    out.sampled = static_cast<size_t>(n);
    return out;
}

std::optional<CClientMgr::GeometryCheck> CClientMgr::check_object_geometry(
    size_t max_per_type) const {
    GeometryCheck out{};
    for (size_t t = 0; t < object_list_count(); ++t) {
        const int64_t n = seh_check_geometry(&regenny()->object_lists[t], max_per_type,
                                            &out.aabb_min_ok, &out.aabb_max_ok, &out.radius_sized,
                                            &out.radius_pristine, &out.dims_nonneg,
                                            max_object_walk);
        if (n < 0) {
            return std::nullopt; // a faulted bucket invalidates the whole report
        }
        out.sampled += static_cast<size_t>(n);
    }
    return out;
}

} // namespace sdk
