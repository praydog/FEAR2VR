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

size_t CClientMgr::start_shell_list_count() const {
    constexpr size_t kMaxWalk = 10000; // fail closed on a corrupted/non-terminating list rather than hang
    size_t count = 0;
    KANANLIB_SEH_TRY {
        const auto* head = &regenny()->start_shell_list;
        const void* cur = head->next;
        while (cur != static_cast<const void*>(head) && count < kMaxWalk) {
            ++count;
            cur = reinterpret_cast<const regenny::CClientMgrListLink*>(cur)->next;
        }
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return count;
}

} // namespace sdk
