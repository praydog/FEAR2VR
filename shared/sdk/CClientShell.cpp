#include "CClientShell.hpp"

#include <windows.h>

#include <utility/Seh.hpp>

#include "CClientMgr.hpp"
#include "regenny/regenny/CClientShell.hpp"
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

// The two clock accessors, dump 0x407490 (game) and 0x406BDC (real). Identical
// 24-byte leaves apart from the field they load:
//   A1 [g_pClientMgr] | 05 34 14 00 00 | 83 38 00 | 74 06 | 8B 00 |
//   DD 40 <08|10> | C3 | D9 EE | C3
// i.e. load the global, add 0x1434, return 0.0 (fldz) if the slot is null, else
// follow it and fld the double. The +0x1434 displacement and the trailing fldz are
// both left concrete: they are what makes the match a shell-clock getter rather than
// any other double reader.
static constexpr const char* kGetGameTime =
    "A1 ? ? ? ? 05 34 14 00 00 83 38 00 74 06 8B 00 DD 40 08 C3 D9 EE C3";
static constexpr const char* kGetRealTime =
    "A1 ? ? ? ? 05 34 14 00 00 83 38 00 74 06 8B 00 DD 40 10 C3 D9 EE C3";

using GetShellTimeFn = double(*)();

namespace {

bool call_shell_time(uintptr_t fn, double* out) {
    bool ok = false;
    KANANLIB_SEH_TRY {
        *out = reinterpret_cast<GetShellTimeFn>(fn)();
        ok = true;
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return ok;
}

} // namespace

CClientShell* CClientShell::get() {
    CClientMgr* mgr = CClientMgr::get();
    return mgr != nullptr ? mgr->client_shell() : nullptr;
}

std::optional<double> CClientShell::game_time_seconds() {
    static const uintptr_t s_fn =
        Modules::get().scan_exe(kGetGameTime, "ClientShell_GetGameTime");
    // The shell check is OURS, not the engine's: its accessor answers 0.0 for a
    // missing shell, which a caller cannot tell from a genuine zero.
    if (s_fn == 0 || get() == nullptr) {
        return std::nullopt;
    }
    double out = 0.0;
    if (!call_shell_time(s_fn, &out)) {
        return std::nullopt;
    }
    return out;
}

std::optional<double> CClientShell::real_time_seconds() {
    static const uintptr_t s_fn =
        Modules::get().scan_exe(kGetRealTime, "ClientShell_GetRealTime");
    if (s_fn == 0 || get() == nullptr) {
        return std::nullopt;
    }
    double out = 0.0;
    if (!call_shell_time(s_fn, &out)) {
        return std::nullopt;
    }
    return out;
}

} // namespace sdk

namespace {

// One guarded read of the whole 4-byte table, so a caller counting slots and a caller
// reading one slot see the same instant.
struct LocalIdsRaw {
    uint8_t ids[4];
    float frame_interval;
    bool ok;
};

LocalIdsRaw seh_local_ids(sdk::CClientShell* shell) {
    LocalIdsRaw r{};
    KANANLIB_SEH_TRY {
        const auto* s = reinterpret_cast<const regenny::CClientShell*>(shell);
        for (int i = 0; i < 4; ++i) {
            r.ids[i] = s->local_client_ids[i];
        }
        r.frame_interval = s->frame_interval;
        r.ok = true;
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        r.ok = false;
    }
    return r;
}

} // namespace

namespace sdk {

std::optional<uint8_t> CClientShell::local_client_id(unsigned index) {
    // The engine's own bound: GetLocalClientID rejects >= 4 outright.
    if (index >= 4) {
        return std::nullopt;
    }
    CClientShell* shell = get();
    if (shell == nullptr) {
        return std::nullopt;
    }
    const LocalIdsRaw r = seh_local_ids(shell);
    if (!r.ok || r.ids[index] == 0xFF) {
        return std::nullopt;
    }
    return r.ids[index];
}

std::optional<unsigned> CClientShell::local_client_count() {
    CClientShell* shell = get();
    if (shell == nullptr) {
        return std::nullopt;
    }
    const LocalIdsRaw r = seh_local_ids(shell);
    if (!r.ok) {
        return std::nullopt;
    }
    unsigned n = 0;
    for (int i = 0; i < 4; ++i) {
        if (r.ids[i] != 0xFF) {
            ++n;
        }
    }
    return n;
}

std::optional<float> CClientShell::frame_interval_seconds() {
    CClientShell* shell = get();
    if (shell == nullptr) {
        return std::nullopt;
    }
    const LocalIdsRaw r = seh_local_ids(shell);
    if (!r.ok) {
        return std::nullopt;
    }
    return r.frame_interval;
}

} // namespace sdk

namespace {

// The handle and the resolved pointer for one slot, read together under one guard so a
// caller never sees a handle from one instant paired with a pointer from another.
struct LocalPlayerRaw {
    const void* object;
    uint16_t handle;
    bool ok;
};

LocalPlayerRaw seh_local_player(sdk::CClientShell* shell, unsigned index) {
    LocalPlayerRaw r{};
    KANANLIB_SEH_TRY {
        const auto* s = reinterpret_cast<const regenny::CClientShell*>(shell);
        r.handle = s->local_player_handles[index];
        r.object = s->local_player_objects[index];
        r.ok = true;
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        r.ok = false;
    }
    return r;
}

} // namespace

namespace sdk {

std::optional<CClientShell::LocalPlayer> CClientShell::local_player(unsigned index) {
    // Four slots, the same bound the engine uses for local client ids.
    if (index >= 4) {
        return std::nullopt;
    }
    CClientShell* shell = get();
    if (shell == nullptr) {
        return std::nullopt;
    }
    const LocalPlayerRaw r = seh_local_player(shell, index);
    // An empty slot is BOTH sentinels at once; requiring both guards against reading a
    // half-updated pair rather than trusting either alone.
    if (!r.ok || r.handle == 0xFFFF || r.object == nullptr) {
        return std::nullopt;
    }
    LocalPlayer out{};
    out.object = static_cast<const regenny::LTObject*>(r.object);
    out.handle = r.handle;
    return out;
}

std::optional<unsigned> CClientShell::local_player_count() {
    if (get() == nullptr) {
        return std::nullopt;
    }
    unsigned n = 0;
    for (unsigned i = 0; i < 4; ++i) {
        if (local_player(i).has_value()) {
            ++n;
        }
    }
    return n;
}

} // namespace sdk
