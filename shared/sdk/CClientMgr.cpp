#include "CClientMgr.hpp"

#include <windows.h>

#include <utility/Seh.hpp>

#include "regenny/regenny/CClientMgrCounterNode.hpp"

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
        const void* cur = head->next;
        size_t count = 0;
        while (cur != static_cast<const void*>(head) && count < kMaxWalk) {
            ++count;
            cur = reinterpret_cast<const regenny::CClientMgrListLink*>(cur)->next;
        }
        // Terminated only if we came back around to the head; hitting the cap
        // means the list is corrupt or the mapping is wrong -- report neither
        // a count nor a guess.
        result = (cur == static_cast<const void*>(head)) ? static_cast<int64_t>(count) : -1;
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
            ok = (static_cast<const void*>(&node->self_link) == r->counter_list_head.next);
        }
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return ok;
}

namespace {

constexpr size_t kMaxObjectWalk = 100000; // fail closed on a corrupt/non-terminating object list rather than hang

// POD-only SEH helper (MSVC C2712). Walks one object bucket, counting
// entries. Returns the count, or -1 if it faulted or did not terminate.
int64_t seh_count_objects(const regenny::CClientMgrListLink* head) {
    int64_t result = -1;
    KANANLIB_SEH_TRY {
        const void* cur = head->next;
        size_t n = 0;
        while (cur != static_cast<const void*>(head) && n < kMaxObjectWalk) {
            ++n;
            cur = reinterpret_cast<const regenny::CClientMgrListLink*>(cur)->next;
        }
        result = (cur == static_cast<const void*>(head)) ? static_cast<int64_t>(n) : -1;
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        result = -1;
    }
    return result;
}

// POD-only SEH helper. Walks one bucket AND copies each object's fields in
// the SAME pass -- see the header's note on why we never hand out an
// LTObject*. Returns the number written, or -1 on fault/non-termination/
// invariant violation.
//
// The link -> object base step uses offsetof on the generated schema, so no
// literal offset (the engine's own walkers use `link - 172`; that 172 is
// LTObject.list_link's offset, which the compiler derives here).
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
                             CClientMgr::ObjectSnapshot* out, size_t max) {
    int64_t result = -1;
    KANANLIB_SEH_TRY {
        const void* cur = head->next;
        size_t n = 0, written = 0;
        bool invariant_ok = true;
        while (cur != static_cast<const void*>(head) && n < kMaxObjectWalk) {
            const auto link_addr = reinterpret_cast<uintptr_t>(cur);
            const auto* obj = reinterpret_cast<const regenny::LTObject*>(
                link_addr - offsetof(regenny::LTObject, list_link));
            if (static_cast<uint8_t>(obj->type) != expected_type) {
                invariant_ok = false;
                break;
            }
            if (written < max) {
                auto& s = out[written];
                s.address = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(obj));
                s.vtable = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(obj->vtable));
                s.type = static_cast<uint8_t>(obj->type);
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
            cur = reinterpret_cast<const regenny::CClientMgrListLink*>(cur)->next;
        }
        result = (invariant_ok && cur == static_cast<const void*>(head))
                     ? static_cast<int64_t>(written)
                     : -1;
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        result = -1;
    }
    return result;
}

} // namespace

std::optional<size_t> CClientMgr::object_count(size_t type) const {
    if (type >= object_list_count()) {
        return std::nullopt;
    }
    const int64_t n = seh_count_objects(&regenny()->object_lists[type]);
    if (n < 0) {
        return std::nullopt;
    }
    return static_cast<size_t>(n);
}

std::optional<size_t> CClientMgr::snapshot_objects(size_t type, ObjectSnapshot* out, size_t max) const {
    if (type >= object_list_count() || (out == nullptr && max != 0)) {
        return std::nullopt;
    }
    const int64_t n = seh_snapshot_objects(&regenny()->object_lists[type],
                                           static_cast<uint8_t>(type), out, max);
    if (n < 0) {
        return std::nullopt;
    }
    return static_cast<size_t>(n);
}

} // namespace sdk
