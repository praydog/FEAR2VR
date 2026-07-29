#include "Engine.hpp"

#include <windows.h>

#include "Memory.hpp"

#include "Log.hpp"
#include "Modules.hpp"
#include "CClientMgr.hpp"
#include "regenny/regenny/LTConVar.hpp"

namespace sdk {

// cis_GetEngineHook -- FEAR2_dump.exe 0x46AA1E (__stdcall, `retn 8` verified):
//   68 ["hwnd"] | FF 74 24 08 | E8 [rel32] | 85 C0 | 59 59 | 75 10 |
//   8B 44 24 08 | 8B 0D [hWnd] | 89 08 | 33 C0 | EB 30
// NOTE: the mov ecx,[imm32] operand at +0x1A is &hWnd (main window global).
static constexpr const char* kGetEngineHook =
    "68 ? ? ? ? FF 74 24 08 E8 ? ? ? ? 85 C0 59 59 75 10 8B 44 24 08 8B 0D ? ? ? ? 89 08 33 C0 EB 30";

// ClientTime_GetSeconds -- FEAR2_dump.exe 0x406DED, and
// ClientTime_GetMilliseconds -- 0x406DFC. Both are 15-byte leaves:
//   A1 [g_pClientMgr] | 8B 80 F4 13 00 00 | <load> | C3
// The tail is what distinguishes them: `fld qword ptr [eax+38h]` (DD 40 38) for the
// double, `mov eax, [eax+30h]` (8B 40 30) for the millisecond count. The global's
// address is wildcarded; the +0x13F4 displacement is not, because that is the field
// identifying which manager member is being followed.
static constexpr const char* kClientTimeSeconds =
    "A1 ? ? ? ? 8B 80 F4 13 00 00 DD 40 38 C3";
static constexpr const char* kClientTimeMillis =
    "A1 ? ? ? ? 8B 80 F4 13 00 00 8B 40 30 C3";

// No arguments, so __cdecl and __stdcall are indistinguishable here.
using GetTimeSecondsFn = double(*)();
using GetTimeMillisFn = uint32_t(*)();

// CClientMgr_GetGlobalForce -- FEAR2_dump.exe 0x405C39, __stdcall(float* out),
// `retn 4`:
//   A1 [g_pClientMgr] | 8B 4C 24 04 | 05 40 14 00 00 |
//   D9 00 D9 19 | D9 40 04 D9 59 04 | D9 40 08 33 C0 D9 59 08 | C2 04 00
// Three fld/fstp pairs copying mgr+0x1440..+0x1448 into the caller's buffer, with
// `xor eax, eax` for the LT_OK return wedged between the last load and store.
static constexpr const char* kGetGlobalForce =
    "A1 ? ? ? ? 8B 4C 24 04 05 40 14 00 00 D9 00 D9 19 D9 40 04 D9 59 04 D9 40 08 33 C0 "
    "D9 59 08 C2 04 00";

using GetGlobalForceFn = int(__stdcall*)(float* out);

using GetEngineHookFn = int(__stdcall*)(const char* name, void** out_data);

namespace {

// Own function scope: __try cannot share a function with static-local
// initialization (MSVC C2712), hence no lambda here.
uintptr_t resolve_hwnd_slot() {
    constexpr uint32_t kGetEngineHook_HWndOperand = 0x1A;
    const uintptr_t fn = Engine::get_engine_hook_fn();
    if (fn == 0) {
        return 0;
    }
    const auto slot = sdk::mem::read_ptr(fn + kGetEngineHook_HWndOperand);
    if (!slot.has_value()) {
        LOGX("[sdk] crashed reading &hWnd operand");
        return 0;
    }
    return *slot;
}

int call_engine_hook(uintptr_t fn, const char* name, void** out) {
    int rc = -1;
    sdk::mem::guarded([&] {
        rc = reinterpret_cast<GetEngineHookFn>(fn)(name, out);
    });
    return rc;
}

// Both engine calls inside ONE guard, so the pair is read from one instant. Reading
// them under separate guards would let a frame boundary fall between and produce a
// seconds/milliseconds mismatch that never existed.
bool call_client_time(uintptr_t fn_s, uintptr_t fn_ms, double* out_s, uint32_t* out_ms) {
    return sdk::mem::guarded([&] {
        *out_s = reinterpret_cast<GetTimeSecondsFn>(fn_s)();
        *out_ms = reinterpret_cast<GetTimeMillisFn>(fn_ms)();
    });
}

// The engine writes THREE floats through the pointer we hand it, so the buffer is
// sized here and not by the callee's word. A short buffer would be a stack overwrite
// the guard could not catch, because the write would be perfectly legal.
bool call_global_force(uintptr_t fn, float out[3]) {
    return sdk::mem::guarded([&] {
        reinterpret_cast<GetGlobalForceFn>(fn)(out);
    });
}

} // namespace

uintptr_t Engine::get_engine_hook_fn() {
    static const uintptr_t s_fn = Modules::get().scan_exe(kGetEngineHook, "cis_GetEngineHook");
    return s_fn;
}

int Engine::get_engine_hook(const char* name, void** out) {
    const uintptr_t fn = get_engine_hook_fn();
    if (fn == 0 || name == nullptr || out == nullptr) {
        return -1; // not LT_ERROR; "our side could not make the call"
    }
    return call_engine_hook(fn, name, out);
}

uintptr_t Engine::main_hwnd_slot() {
    static const uintptr_t s_slot = resolve_hwnd_slot();
    return s_slot;
}

void* Engine::main_hwnd() {
    const uintptr_t slot = main_hwnd_slot();
    if (slot == 0) {
        return nullptr;
    }
    return reinterpret_cast<void*>(sdk::mem::read_ptr(slot).value_or(0));
}

std::optional<Engine::ClientTime> Engine::client_time() {
    static const uintptr_t s_sec =
        Modules::get().scan_exe(kClientTimeSeconds, "ClientTime_GetSeconds");
    static const uintptr_t s_ms =
        Modules::get().scan_exe(kClientTimeMillis, "ClientTime_GetMilliseconds");
    if (s_sec == 0 || s_ms == 0) {
        return std::nullopt;
    }
    ClientTime out{};
    if (!call_client_time(s_sec, s_ms, &out.seconds, &out.milliseconds)) {
        return std::nullopt;
    }
    return out;
}

std::optional<Engine::ForceVector> Engine::global_force() {
    static const uintptr_t s_fn =
        Modules::get().scan_exe(kGetGlobalForce, "CClientMgr_GetGlobalForce");
    if (s_fn == 0) {
        return std::nullopt;
    }
    float v[3] = {0.0f, 0.0f, 0.0f};
    if (!call_global_force(s_fn, v)) {
        return std::nullopt;
    }
    return ForceVector{v[0], v[1], v[2]};
}

} // namespace sdk

namespace sdk {

namespace {

// POD copy of one record, taken under the guard; the two strings are copied by the
// caller so the guard never holds a type with a destructor.
struct ConVarRaw {
    uintptr_t address;
    float value;
    uint32_t hash;
    const char* name_ptr;
    const char* text_ptr;
    const void* next;
    bool ok;
};

// `link` points at a record's +0x04, so the record base is link - 4. That offset is the
// engine's own: ConVarTable_FindInBucket ends with `return link - 4`.
ConVarRaw seh_convar(const void* link) {
    ConVarRaw r{};
    const auto rec = sdk::mem::read<regenny::LTConVar>(reinterpret_cast<uintptr_t>(link) - 4);
    if (!rec.has_value()) {
        return r;
    }
    r.address = reinterpret_cast<uintptr_t>(link) - 4;
    r.value = rec->value;
    r.hash = rec->name_hash;
    r.name_ptr = rec->name;
    r.text_ptr = rec->string_value;
    r.next = rec->link.next;
    r.ok = true;
    return r;
}

// Copy a NUL-terminated engine string into a caller buffer. POD only, no unwinding
// objects in scope: MSVC refuses __try in a function that must unwind one (C2712), so
// the std::string is built by the caller.
bool seh_copy_str(const char* src, char* dst, size_t cap) {
    bool ok = false;
    const bool guarded_ok = sdk::mem::guarded([&] {
        if (src != nullptr && cap > 0) {
            size_t i = 0;
            for (; i + 1 < cap; ++i) {
                const char c = src[i];
                if (c == '\0') {
                    break;
                }
                dst[i] = c;
            }
            dst[i] = '\0';
            ok = true;
        }
    });
    return guarded_ok && ok;
}

// The bucket head's next pointer, read under its own guard for the same reason.
bool seh_bucket_next(const void* head, const void** out) {
    const auto v = sdk::mem::read_ptr(
        reinterpret_cast<uintptr_t>(head) + offsetof(regenny::CClientMgrListLink, next));
    if (!v.has_value()) {
        return false;
    }
    *out = reinterpret_cast<const void*>(*v);
    return true;
}

constexpr size_t kConVarBuckets = 128;
// A bucket of 5 is the longest seen live; this cap is for a torn chain, not a long one.
constexpr size_t kMaxChain = 256;

}  // namespace

namespace {

// Walk one 128-bucket LTConVar table from its BUCKET ARRAY base. Both tables have identical shape, so this
// is shared rather than written twice.
std::vector<Engine::ConVar> walk_convar_table(uintptr_t buckets) {
    std::vector<Engine::ConVar> out;
    if (buckets == 0) {
        return out;
    }
    for (size_t b = 0; b < kConVarBuckets; ++b) {
        const void* headp = reinterpret_cast<const void*>(buckets + b * 8);
        const void* cur = nullptr;
        if (!seh_bucket_next(headp, &cur)) {
            continue;
        }
        size_t n = 0;
        while (cur != nullptr && cur != headp && n < kMaxChain) {
            const ConVarRaw r = seh_convar(cur);
            if (!r.ok) {
                break;
            }
            char nbuf[128]{};
            char tbuf[128]{};
            const bool have_name = seh_copy_str(r.name_ptr, nbuf, sizeof(nbuf));
            seh_copy_str(r.text_ptr, tbuf, sizeof(tbuf));
            Engine::ConVar v{};
            v.address = r.address;
            v.name = have_name ? nbuf : "";
            v.value = r.value;
            v.text = tbuf;
            if (!v.name.empty()) {
                out.push_back(std::move(v));
            }
            cur = r.next;
            ++n;
        }
    }
    return out;
}

// The console source descriptor's own table. Its bucket array sits 0x24 into the block, the same offset
// LTConVarTable uses on CClientMgr.
constexpr uintptr_t kConsoleSourceOffset = 0x2ED4A0;
constexpr uintptr_t kBucketsWithinTable = 0x24;

}  // namespace

std::vector<Engine::ConVar> Engine::console_vars() {
    CClientMgr* mgr = CClientMgr::get();
    if (mgr == nullptr) {
        return {};
    }
    return walk_convar_table(
        reinterpret_cast<uintptr_t>(&mgr->regenny()->console_vars.buckets[0]));
}

std::vector<Engine::ConVar> Engine::console_source_vars() {
    const auto* exe = Modules::get().exe();
    if (exe == nullptr || exe->base == 0) {
        return {};
    }
    return walk_convar_table(exe->base + kConsoleSourceOffset + kBucketsWithinTable);
}

std::vector<Engine::ConVar> Engine::all_console_vars() {
    auto out = console_source_vars();
    auto mgr_vars = console_vars();
    out.insert(out.end(), std::make_move_iterator(mgr_vars.begin()),
               std::make_move_iterator(mgr_vars.end()));
    return out;
}

std::optional<Engine::ConVar> Engine::console_var(const char* name) {
    if (name == nullptr) {
        return std::nullopt;
    }
    // A linear scan of the enumeration rather than a hash probe: reproducing the
    // engine's hash here would add a second implementation of String_HashI for no gain,
    // and the table is 192 entries.
    for (auto& v : all_console_vars()) {
        if (_stricmp(v.name.c_str(), name) == 0) {
            return v;
        }
    }
    return std::nullopt;
}

}  // namespace sdk
