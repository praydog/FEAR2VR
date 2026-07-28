#include "Engine.hpp"

#include <utility/Scan.hpp>

#include "Log.hpp"
#include "Modules.hpp"

namespace sdk::engine {
namespace {

Targets g_targets;
bool g_resolved = false;

// ---------------------------------------------------------------------------
// Signatures. Convention: one `?` per wildcard byte (kananlib syntax, NOT ??).
// Wildcards cover call targets (rel32) and absolute data references so the
// patterns survive any module relocation of FEAR2.exe.
// ---------------------------------------------------------------------------

// CClientMgr::Update -- FEAR2_dump.exe 0x40B665:
//   55 8B EC | 83 EC 14 | 53 56 57 | 8B F1 | E8 [rel32] | 89 45 EC | 89 55 F0 |
//   E8 [rel32] | 8B C8 | E8 [rel32] | D9 05 [abs32] | 51 | D9 1C 24 | E8 ...
constexpr const char* kClientMgrUpdate =
    "55 8B EC 83 EC 14 53 56 57 8B F1 E8 ? ? ? ? 89 45 EC 89 55 F0 E8 ? ? ? ? 8B C8 "
    "E8 ? ? ? ? D9 05 ? ? ? ? 51 D9 1C 24 E8";

// CClientShell::Update -- FEAR2_dump.exe 0x40CC5E:
//   55 8B EC | 81 EC 04 02 00 00 | 53 56 57 | 8B F9 | 8B 0D [g_pClientMgr] |
//   E8 [rel32] | 33 DB | 39 1D [abs32] ...
// NOTE: the mov ecx,[imm32] at +0x0F carries &g_pClientMgr -- resolved below.
constexpr const char* kClientShellUpdate =
    "55 8B EC 81 EC 04 02 00 00 53 56 57 8B F9 8B 0D ? ? ? ? E8 ? ? ? ? 33 DB 39 1D";

// cis_GetEngineHook -- FEAR2_dump.exe 0x46AA1E:
//   68 ["hwnd"] | FF 74 24 08 | E8 [rel32] | 85 C0 | 59 59 | 75 10 |
//   8B 44 24 08 | 8B 0D [hWnd] | 89 08 | 33 C0 | EB 30 ...
// NOTE: the mov ecx,[imm32] at +0x18 carries &hWnd (main window global).
constexpr const char* kGetEngineHook =
    "68 ? ? ? ? FF 74 24 08 E8 ? ? ? ? 85 C0 59 59 75 10 8B 44 24 08 8B 0D ? ? ? ? 89 08 33 C0 EB 30";

// Operand offsets inside the matches:
constexpr uint32_t kShellUpdate_ClientMgrOperand = 0x10; // dword: &g_pClientMgr
constexpr uint32_t kEngineHook_HWndOperand = 0x1A;       // dword: &hWnd

bool scan_one(HMODULE mod, const char* pattern, const char* name, uintptr_t& out) {
    auto result = utility::scan(mod, pattern);
    if (!result) {
        LOGX("[sdk] pattern MISS: %s", name);
        return false;
    }
    out = *result;
    LOGX("[sdk] %-22s -> 0x%08X", name, static_cast<uint32_t>(out));
    return true;
}

} // namespace

bool resolve() {
    if (g_resolved) {
        return true;
    }

    if (!sdk::exe() || sdk::exe()->handle == nullptr) {
        LOGX("[sdk] resolve: FEAR2.exe module unavailable");
        return false;
    }
    HMODULE mod = sdk::exe()->handle;

    Targets t{};
    if (!scan_one(mod, kClientMgrUpdate, "CClientMgr::Update", t.client_mgr_update)) return false;
    if (!scan_one(mod, kClientShellUpdate, "CClientShell::Update", t.client_shell_update)) return false;
    if (!scan_one(mod, kGetEngineHook, "cis_GetEngineHook", t.get_engine_hook)) return false;

    __try {
        t.p_g_pClientMgr = *reinterpret_cast<uintptr_t*>(t.client_shell_update + kShellUpdate_ClientMgrOperand);
        t.p_g_hMainWnd = *reinterpret_cast<uintptr_t*>(t.get_engine_hook + kEngineHook_HWndOperand);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOGX("[sdk] resolve: crashed reading signature operands");
        return false;
    }
    LOGX("[sdk] &g_pClientMgr        -> 0x%08X", static_cast<uint32_t>(t.p_g_pClientMgr));
    LOGX("[sdk] &hWnd                -> 0x%08X", static_cast<uint32_t>(t.p_g_hMainWnd));

    g_targets = t;
    g_resolved = true;
    return true;
}

bool is_resolved() {
    return g_resolved;
}

const Targets& targets() {
    return g_targets;
}

void* get_client_mgr() {
    if (!g_resolved || g_targets.p_g_pClientMgr == 0) {
        return nullptr;
    }
    __try {
        return *reinterpret_cast<void**>(g_targets.p_g_pClientMgr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void* get_main_hwnd() {
    if (!g_resolved || g_targets.p_g_hMainWnd == 0) {
        return nullptr;
    }
    __try {
        return *reinterpret_cast<void**>(g_targets.p_g_hMainWnd);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

GetEngineHook_fn get_engine_hook() {
    return g_resolved ? reinterpret_cast<GetEngineHook_fn>(g_targets.get_engine_hook) : nullptr;
}

bool safe_read32(const void* address, uint32_t& out) {
    __try {
        out = *reinterpret_cast<const uint32_t*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void* get_database_mgr() {
    Module* db = sdk::game_database();
    if (db == nullptr || db->handle == nullptr) {
        return nullptr;
    }
    const char* export_name = "?LTGetIDatabaseMgr@@YAPAVIDatabaseMgr@@XZ";
    auto get = reinterpret_cast<void* (__cdecl*)()>(GetProcAddress(db->handle, export_name));
    if (get == nullptr) {
        return nullptr;
    }
    __try {
        return get();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

} // namespace sdk::engine
