#include "CClientShell.hpp"

#include <windows.h>

#include "Memory.hpp"

#include "interfaces/IClientShell.hpp"
#include <utility/Module.hpp>
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
    return sdk::mem::guarded([&] {
        *out = reinterpret_cast<GetShellTimeFn>(fn)();
    });
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
    r.ok = sdk::mem::guarded([&] {
        const auto* s = reinterpret_cast<const regenny::CClientShell*>(shell);
        for (int i = 0; i < 4; ++i) {
            r.ids[i] = s->local_client_ids[i];
        }
        r.frame_interval = s->frame_interval;
    });
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
    r.ok = sdk::mem::guarded([&] {
        const auto* s = reinterpret_cast<const regenny::CClientShell*>(shell);
        r.handle = s->local_player_handles[index];
        r.object = s->local_player_objects[index];
    });
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
    // KEEP THE PROMISE THIS FUNCTION MAKES. Its contract says the handle and the pointer are the
    // same object, so verify it rather than assuming: resolve the handle through the manager's table
    // and refuse the pair if it disagrees with the stored pointer.
    //
    // That matters because the shell re-resolves the pointer once per frame inside Update, so a
    // caller on another thread can land mid-refresh. Failing closed is the only safe default -- a
    // torn pair is exactly what a consumer cannot do anything sensible with, and an optional checker
    // callers must remember is not a contract.
    //
    // Use local_player_raw_pair_agrees() if you specifically want to OBSERVE the disagreement.
    // The manager is REQUIRED, not optional. Equality is part of this function's contract, so a
    // state where it cannot be verified is not a state where the pair may be handed out -- treating
    // "unverifiable" as "valid" would leave exactly the hole the check exists to close.
    auto* mgr = CClientMgr::get();
    if (mgr == nullptr || mgr->object_from_handle(r.handle) != r.object) {
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

std::optional<bool> CClientShell::local_player_raw_pair_agrees(unsigned index) {
    // Deliberately does NOT go through local_player(), which now refuses a disagreeing pair -- this
    // has to read the raw slot to be able to report one.
    if (index >= 4) {
        return std::nullopt;
    }
    CClientShell* shell = get();
    auto* mgr = CClientMgr::get();
    if (shell == nullptr || mgr == nullptr) {
        return std::nullopt;
    }
    const LocalPlayerRaw r = seh_local_player(shell, index);
    // 0xFFFF is the resolver's own empty sentinel, not zero -- see the schema note on
    // local_player_handles.
    if (!r.ok || r.handle == 0xFFFF || r.object == nullptr) {
        return std::nullopt;
    }
    return mgr->object_from_handle(r.handle) == r.object;
}

namespace {

// IClientShell vtable slots, established from the binary rather than from the reference SDK's
// declaration order -- see the header for the anchor.
constexpr size_t kSlotImplementationName = 1;
constexpr size_t kSlotPreUpdate = 2;
constexpr size_t kSlotPostUpdate = 3;
constexpr size_t kSlotUpdate = 4;

uintptr_t seh_vtable_slot(const void* iface, size_t slot) {
    const auto vt = sdk::mem::read_ptr(reinterpret_cast<uintptr_t>(iface));
    if (!vt.has_value() || *vt == 0) {
        return 0;
    }
    return sdk::mem::read_ptr(*vt + slot * sizeof(void*)).value_or(0);
}

// Slot 1 is `return "CGameClientShell";` -- a pure return of a constant, so calling it has no side
// effects and needs no particular thread.
const char* seh_call_impl_name(uintptr_t fn, const void* iface) {
    const char* out = nullptr;
    sdk::mem::guarded([&] {
        out = reinterpret_cast<const char*(__thiscall*)(const void*)>(fn)(iface);
    });
    return out;
}

int64_t seh_copy_name(const char* src, char* dst, size_t cap) {
    int64_t n = -1;
    sdk::mem::guarded([&] {
        size_t i = 0;
        for (; i + 1 < cap && src[i] != '\0'; ++i) {
            dst[i] = src[i];
        }
        dst[i] = '\0';
        n = static_cast<int64_t>(i);
    });
    return n;
}

// Only addresses inside gameclient.dll are handed out: an implementation slot pointing anywhere
// else means the layout assumption is wrong, and hooking it would be worse than refusing.
bool in_game_client(uintptr_t fn) {
    const auto* gc = Modules::get().game_client();
    if (gc == nullptr || gc->base == 0 || fn == 0) {
        return false;
    }
    const auto size = utility::get_module_size(gc->handle);
    if (!size.has_value()) {
        return false;
    }
    return fn >= gc->base && fn < gc->base + *size;
}

uintptr_t resolve_checked(size_t slot) {
    if (!GameClientShell::available()) {
        return 0;
    }
    auto* iface = interfaces::IClientShell::get();
    if (iface == nullptr) {
        return 0;
    }
    const auto fn = seh_vtable_slot(iface, slot);
    return in_game_client(fn) ? fn : 0;
}

}  // namespace

uintptr_t GameClientShell::vtable_entry_address(size_t slot) {
    // Refuse anything past the mapped slots: this mapping never measured the table's extent, so an
    // address computed beyond entry 4 would be fabricated. See the header.
    if (slot > kMaxMappedSlot || !available()) {
        return 0;
    }
    auto* iface = interfaces::IClientShell::get();
    if (iface == nullptr) {
        return 0;
    }
    const auto vt = sdk::mem::read_ptr(reinterpret_cast<uintptr_t>(iface));
    if (!vt.has_value() || *vt == 0) {
        return 0;
    }
    // THE ADDRESS OF THE ENTRY, not the function stored in it. That distinction is the whole purpose of
    // this accessor: pre_update_fn() hands out the function for introspection, while this hands out the
    // .rdata slot a hook repoints. Returning the function here would give a consumer intending to hook a
    // pointer it would then write THROUGH -- overwriting the method's first four code bytes instead of
    // swapping a table entry.
    return *vt + slot * sizeof(void*);
}

uintptr_t GameClientShell::pre_update_vtable_entry() { return vtable_entry_address(kSlotPreUpdate); }

std::optional<std::string> GameClientShell::implementation_name() {
    // Delegates: interfaces::slot1_constant_string() reads slot 1's constant-return body without
    // calling it, and is shared by every interface rather than reimplemented here. The semantic name
    // stays HERE, where slot 1 was individually verified to be _InterfaceImplementation returning
    // "CGameClientShell"; the generic helper only reports the shape it found.
    return interfaces::slot1_constant_string(interfaces::IClientShell::get());
}

bool GameClientShell::available() {
    const auto name = implementation_name();
    return name.has_value() && *name == "CGameClientShell";
}

uintptr_t GameClientShell::pre_update_fn() { return resolve_checked(kSlotPreUpdate); }

// Prologue bytes as read from this build. Each is only as long as it needs to be to separate the
// three slots from one another.
bool GameClientShell::slots_match_mapped_shapes() {
    static constexpr uint8_t kPreUpdate[] = {0xC3, 0xCC};                          // retn + int3 pad
    static constexpr uint8_t kPostUpdate[] = {0x81, 0xEC, 0x28, 0x01, 0x00, 0x00};  // sub esp, 128h
    static constexpr uint8_t kUpdate[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xC0};      // ebp frame, align 64

    const struct {
        uintptr_t fn;
        const uint8_t* want;
        size_t len;
    } slots[] = {
        {pre_update_fn(), kPreUpdate, sizeof(kPreUpdate)},
        {post_update_fn(), kPostUpdate, sizeof(kPostUpdate)},
        {update_fn(), kUpdate, sizeof(kUpdate)},
    };

    for (const auto& s : slots) {
        if (s.fn == 0) {
            return false;
        }
        uint8_t buf[8]{};
        if (!sdk::mem::copy(buf, s.fn, s.len) || memcmp(buf, s.want, s.len) != 0) {
            return false;
        }
    }
    return true;
}

bool GameClientShell::pre_update_entry_returns_immediately() {
    const auto fn = pre_update_fn();
    if (fn == 0) {
        return false;
    }
    const auto b = sdk::mem::read<uint8_t>(fn);
    const bool empty = b.has_value() && *b == 0xC3u;  // entry returns at once
    return empty;
}
uintptr_t GameClientShell::update_fn() { return resolve_checked(kSlotUpdate); }
uintptr_t GameClientShell::post_update_fn() { return resolve_checked(kSlotPostUpdate); }

} // namespace sdk
