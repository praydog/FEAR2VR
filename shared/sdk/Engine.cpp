#include "Engine.hpp"

#include <windows.h>

#include "Memory.hpp"

#include "Log.hpp"
#include "Modules.hpp"
#include "CClientMgr.hpp"
#include "regenny/regenny/LTConVar.hpp"
#include "interfaces/Registry.hpp"
#include <utility/Module.hpp>
#include <algorithm>

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

bool Engine::write_console_var(const char* name, float value) {
    const auto v = console_var(name);
    if (!v.has_value() || v->address == 0) {
        return false;
    }
    // The record's first field IS the float the engine reads, so this is the whole write.
    return mem::write<float>(v->address, value);
}

const std::vector<Engine::CameraTunable>& Engine::camera_tunables() {
    // Live defaults from this build. Where the reference's CPlayerCamera::Init differs the reference value is
    // named in the comment, because the difference is FEAR 2's own retuning and worth not losing.
    static const std::vector<CameraTunable> s_tunables = {
        {"FovY", 65.0f, 65.0f, "vertical field of view in degrees; reference default was 70"},
        {"FovAspectRatioScale", 1.0f, 1.0f, "scales the horizontal FOV derived from the aspect ratio"},
        {"HeadBob", 1.0f, 0.0f, "master head-bob switch; ~40 HeadBob* parameters hang off it"},
        {"HeadBobSpeedScale", 1.0f, 0.0f, "how fast the bob cycles with movement"},
        {"DisableCameraShake", 0.0f, 1.0f, "one switch for every shake source"},
        {"CamDamage", 1.0f, 0.0f, "camera kick on taking damage (Cam*Damage* set the magnitudes)"},
        {"CameraSwayXFreq", 13.0f, 0.0f, "idle view sway, horizontal frequency"},
        {"CameraSwayYFreq", 5.0f, 0.0f, "idle view sway, vertical frequency"},
        {"CameraSwayXSpeed", 3.0f, 0.0f, "horizontal sway amplitude; reference default was 12"},
        {"CameraSwayYSpeed", 1.0f, 0.0f, "vertical sway amplitude; reference default was 1.5"},
        {"CameraSmoothingEnabled", 0.0f, 0.0f, "positional smoothing; already off in this build"},
        {"WeaponLagEnabled", 0.0f, 0.0f, "weapon trails the view; already off in this build"},
        {"WeaponLagFactor", 0.33f, 0.0f, "how far the weapon lags when enabled"},
        {"CameraClipDist", 30.0f, 30.0f, "near clip distance -- lowering it risks geometry entering the eye"},
        {"PitchClamp", 2.0f, 2.0f, "look-pitch limit; a headset supplies its own pitch"},
        {"YawClamp", 6.0f, 6.0f, "look-yaw limit"},
        {"CameraMovementMult", 0.3f, 0.0f, "how much movement drives camera offset"},
    };
    return s_tunables;
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


namespace {

// The seven standalone tunables, in the initialiser's order. Gameclient-relative.
constexpr struct {
    const char* name;
    uintptr_t offset;
} kStandaloneTunables[] = {
    {"HeadBobDebugMode", 0x1FFACC},
    {"HeadBobSpeedScale", 0x1FFAD4},
    {"HeadBobTransitionTime", 0x1FFADC},
    {"WeaponLagEnabled", 0x1FFAE4},
    {"WeaponLagFactor", 0x1FFAEC},
    {"WeaponLagReversed", 0x1FFAF4},
    {"NoHeadBobWeaponScale", 0x1FFCE0},
};

// One block per parameter; twelve {record, owner} pairs each at stride 8.
constexpr uintptr_t kHeadBobBlocks[] = {0x1FFB00, 0x1FFB60, 0x1FFBC0, 0x1FFC20, 0x1FFC80};
constexpr const char* kBobChannelNames[] = {"CameraOffset", "CameraRotation", "WeaponOffset",
                                            "WeaponRotation"};
constexpr const char* kBobAxisNames[] = {"X", "Y", "Z"};
constexpr const char* kBobParamNames[] = {"WaveMin", "WaveMax", "Amp", "AmpOffset", "Flags"};

std::optional<Engine::CachedVar> read_cache_slot(std::string name, uintptr_t offset) {
    const auto* gc = Modules::get().game_client();
    if (gc == nullptr || gc->base == 0) {
        return std::nullopt;
    }
    Engine::CachedVar v;
    v.name = std::move(name);
    v.cache_offset = offset;
    v.record = mem::read_ptr(gc->base + offset).value_or(0);
    v.owner = mem::read_ptr(gc->base + offset + sizeof(uintptr_t)).value_or(0);
    return v;
}

}  // namespace

std::string Engine::head_bob_var_name(BobChannel channel, unsigned axis, BobParam param) {
    const auto ci = static_cast<unsigned>(channel);
    const auto pi = static_cast<unsigned>(param);
    if (ci >= kHeadBobChannels || axis >= kHeadBobAxes || pi >= kHeadBobParams) {
        return {};
    }
    return std::string("HeadBob") + kBobChannelNames[ci] + kBobAxisNames[axis] + kBobParamNames[pi];
}

std::optional<Engine::CachedVar> Engine::head_bob_var(BobChannel channel, unsigned axis, BobParam param) {
    const auto ci = static_cast<unsigned>(channel);
    const auto pi = static_cast<unsigned>(param);
    if (ci >= kHeadBobChannels || axis >= kHeadBobAxes || pi >= kHeadBobParams) {
        return std::nullopt;
    }
    // index within a block is channel * 3 + axis; the block selects the parameter
    const auto offset = kHeadBobBlocks[pi] + (ci * kHeadBobAxes + axis) * 2 * sizeof(uintptr_t);
    return read_cache_slot(head_bob_var_name(channel, axis, param), offset);
}

std::vector<Engine::CachedVar> Engine::camera_tunable_cache() {
    std::vector<CachedVar> out;
    const auto* gc = Modules::get().game_client();
    if (gc == nullptr || gc->base == 0) {
        return out;
    }
    out.reserve(kCameraTunableCount);
    for (const auto& t : kStandaloneTunables) {
        if (auto v = read_cache_slot(t.name, t.offset); v.has_value()) {
            out.push_back(std::move(*v));
        }
    }
    for (unsigned p = 0; p < kHeadBobParams; ++p) {
        for (unsigned ch = 0; ch < kHeadBobChannels; ++ch) {
            for (unsigned ax = 0; ax < kHeadBobAxes; ++ax) {
                if (auto v = head_bob_var(static_cast<BobChannel>(ch), ax, static_cast<BobParam>(p));
                    v.has_value()) {
                    out.push_back(std::move(*v));
                }
            }
        }
    }
    return out;
}

std::optional<Engine::CachedVar> Engine::camera_tunable(std::string_view name) {
    if (name.empty()) {
        return std::nullopt;
    }
    for (auto& v : camera_tunable_cache()) {
        if (v.name == name) {
            return v;
        }
    }
    return std::nullopt;
}

std::optional<float> Engine::read_cached(const CachedVar& var) {
    if (var.record == 0) {
        return std::nullopt;
    }
    // The record's FIRST field is the float the engine reads -- the same field write_console_var stores into.
    return mem::read<float>(var.record);
}

bool Engine::write_cached(const CachedVar& var, float value) {
    if (var.record == 0) {
        return false;
    }
    return mem::write<float>(var.record, value);
}

std::pair<size_t, size_t> Engine::camera_tunable_agreement() {
    size_t agreeing = 0, populated = 0;
    for (const auto& v : camera_tunable_cache()) {
        if (v.record == 0) {
            continue;
        }
        ++populated;
        // The name the cache table spells must resolve, through the console tables, to the SAME record. A wrong
        // grid ordering would still produce valid names landing on the wrong records, which this catches.
        const auto found = console_var(v.name.c_str());
        if (found.has_value() && found->address == v.record) {
            ++agreeing;
        }
    }
    return {agreeing, populated};
}


uintptr_t Engine::cached_var_owner() {
    return reinterpret_cast<uintptr_t>(interfaces::Registry::get().resolve("ILTClient.Default"));
}

std::vector<Engine::CachedVar> Engine::cached_console_vars(size_t limit) {
    std::vector<CachedVar> out;
    const auto* gc = Modules::get().game_client();
    if (gc == nullptr || gc->base == 0 || gc->handle == nullptr) {
        return out;
    }
    const auto owner = cached_var_owner();
    if (owner == 0) {
        return out;  // without the owner there is no pattern to match, so scan nothing
    }
    // Section bounds from the PE headers rather than a literal: a hardcoded range would silently stop covering
    // the data if anything about the layout differed.
    const auto sections = utility::get_module_sections(gc->handle);
    if (!sections.has_value()) {
        return out;
    }
    for (const auto& sec : *sections) {
        if (sec.name != ".data") {
            continue;
        }
        const auto lo = sec.virtual_address;
        const auto hi = lo + sec.virtual_size;
        for (uintptr_t at = lo; at + 2 * sizeof(uintptr_t) <= hi && out.size() < limit;
             at += sizeof(uintptr_t)) {
            if (mem::read_ptr(at + sizeof(uintptr_t)).value_or(0) != owner) {
                continue;
            }
            const auto record = mem::read_ptr(at).value_or(0);
            if (record == 0) {
                continue;
            }
            // A record's name must READ AS AN IDENTIFIER. That is what separates a console-variable cache from
            // any other pair whose second word happens to hold the interface.
            const auto name_ptr = mem::read_ptr(record + offsetof(regenny::LTConVar, name)).value_or(0);
            if (name_ptr == 0) {
                continue;
            }
            const auto name = mem::read_name(name_ptr, 96, 3);
            if (!name.has_value()) {
                continue;
            }
            CachedVar v;
            v.name = *name;
            v.cache_offset = at - gc->base;
            v.record = record;
            v.owner = owner;
            out.push_back(std::move(v));
        }
    }
    return out;
}

std::optional<Engine::CachedVar> Engine::find_cached_var(std::string_view name) {
    if (name.empty()) {
        return std::nullopt;
    }
    for (auto& v : cached_console_vars()) {
        if (v.name == name) {
            return v;
        }
    }
    return std::nullopt;
}

}  // namespace sdk
