#include "CClientShell.hpp"

#include "CClientMgr.hpp"
#include "Modules.hpp"

namespace sdk {

// CClientShell::Update -- FEAR2_dump.exe 0x40CC5E:
//   55 8B EC | 81 EC 04 02 00 00 | 53 56 57 | 8B F9 | 8B 0D [g_pClientMgr] |
//   E8 [rel32] | 33 DB | 39 1D [abs32]
// NOTE: the mov ecx,[imm32] operand at +0x10 is &g_pClientMgr
// (see CClientMgr::instance_slot).
static constexpr const char* kUpdate =
    "55 8B EC 81 EC 04 02 00 00 53 56 57 8B F9 8B 0D ? ? ? ? E8 ? ? ? ? 33 DB 39 1D";

uintptr_t CClientShell::update_fn() {
    static const uintptr_t s_fn = Modules::get().scan_exe(kUpdate, "CClientShell::Update");
    return s_fn;
}

CClientShell* CClientShell::get() {
    CClientMgr* mgr = CClientMgr::get();
    return mgr != nullptr ? mgr->client_shell() : nullptr;
}

} // namespace sdk
