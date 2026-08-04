#include "HudScreenDims.hpp"

#include <windows.h>

#include "Hooks.hpp"
#include "Log.hpp"
#include "SceneTarget.hpp"
#include "sdk/Modules.hpp"

namespace {

// gameclient.dll 0x101FC170, imagebase 0x10000000 -- confirmed against the IDB's global list. An
// earlier guess of 0x1CF170 landed inside a string ("Coul") and got a junk address hooked, which is
// why try_install() now range-checks what it resolves before patching anything.
constexpr uintptr_t kILTClientRva = 0x1FC170;
constexpr uint32_t kFieldOffset = 0x2C;
constexpr const char* kHookName = "ILTClient_GetScreenDims";

// The size the interface lays out correctly at. Proven: at 2560x1440 the HUD is complete and
// correctly proportioned, and that is the configuration this project has been shipping as the
// fallback all along.
constexpr int32_t kUIWidth = 2560;
constexpr int32_t kUIHeight = 1440;

// Where the callee leaves the answer, relative to its out-parameter.
constexpr uint32_t kOutWidth = 132;
constexpr uint32_t kOutHeight = 136;

std::atomic<bool> g_hooked{false};
std::atomic<uint64_t> g_calls{0};
std::atomic<uint64_t> g_rewrites{0};
std::atomic<int32_t> g_last_w{0};
std::atomic<int32_t> g_last_h{0};
std::atomic<uintptr_t> g_fn{0};

bool read_ptr(uintptr_t at, uintptr_t* out) {
    __try {
        *out = *reinterpret_cast<const uintptr_t*>(at);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Read and rewrite are separate SEH scopes so a failure to read never leaves a half-written pair.
bool read_dims(const void* out, int32_t* w, int32_t* h) {
    __try {
        const auto* b = static_cast<const uint8_t*>(out);
        *w = *reinterpret_cast<const int32_t*>(b + kOutWidth);
        *h = *reinterpret_cast<const int32_t*>(b + kOutHeight);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool write_dims(void* out, int32_t w, int32_t h) {
    __try {
        auto* b = static_cast<uint8_t*>(out);
        *reinterpret_cast<int32_t*>(b + kOutWidth) = w;
        *reinterpret_cast<int32_t*>(b + kOutHeight) = h;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// CONVENTION VERIFIED FROM THE DISASSEMBLY, NOT THE DECOMPILER:
//
//     sub_407806(out):  push edi / mov edi,[esp+4+arg_0] / test edi,edi / jz ...
//                       mov esi, offset g_RMode / rep movsd (0x25 dwords) / xor eax,eax / retn
//
// It ends in a PLAIN `retn` -- caller-cleanup -- so this is __cdecl(void* out), NOT __thiscall. The
// caller loading ECX with g_pILTClient is incidental; the callee never reads it. Declaring this
// __fastcall would make the detour callee-clean an argument the caller also cleans, imbalancing the
// stack on every single call.
//
// The body is a 37-dword copy of the global g_RMode, which independently corroborates the +132/+136
// pair: they are dwords 33 and 34 of that structure. Returns 0 on success, 0x3C when out is null.
int __cdecl get_screen_dims_detour(void* out) {
    auto* hook = Hooks::get().find(kHookName);
    if (hook == nullptr) {
        return 0x3C;
    }
    const int r = hook->original<int(__cdecl*)(void*)>()(out);

    g_calls.fetch_add(1, std::memory_order_relaxed);
    if (r != 0 || out == nullptr || SceneTarget::get().overrides() == 0) {
        return r;
    }

    int32_t w = 0;
    int32_t h = 0;
    if (!read_dims(out, &w, &h)) {
        return r;
    }
    g_last_w.store(w, std::memory_order_relaxed);
    g_last_h.store(h, std::memory_order_relaxed);

    // Only when the engine is reporting the supersampled size. Anything else is a query we have no
    // business answering.
    if (w > kUIWidth || h > kUIHeight) {
        if (write_dims(out, kUIWidth, kUIHeight)) {
            g_rewrites.fetch_add(1, std::memory_order_relaxed);
        }
    }
    return r;
}

} // namespace

HudScreenDims& HudScreenDims::get() {
    static HudScreenDims instance;
    return instance;
}

void HudScreenDims::try_install() {
    if (g_hooked.load(std::memory_order_acquire)) {
        return;
    }
    const auto* gc = sdk::Modules::get().game_client();
    if (gc == nullptr || gc->base == 0) {
        return;
    }

    uintptr_t iface = 0;
    if (!read_ptr(gc->base + kILTClientRva, &iface) || iface == 0) {
        return;
    }
    uintptr_t fn = 0;
    if (!read_ptr(iface + kFieldOffset, &fn) || fn == 0) {
        return;
    }

    // The resolved value MUST be code inside the engine executable. Without this, a wrong global
    // address yields a plausible-looking pointer and safetyhook happily patches whatever it names.
    const auto* exe = sdk::Modules::get().exe();
    if (exe == nullptr || exe->base == 0 || fn < exe->base || fn >= exe->base + exe->size) {
        LOGX("[huddims] REFUSING to hook 0x%08X: outside FEAR2.exe (iface 0x%08X)",
             static_cast<unsigned>(fn), static_cast<unsigned>(iface));
        g_hooked.store(true, std::memory_order_release); // do not spin on it every frame
        return;
    }

    if (Hooks::get().install(kHookName, reinterpret_cast<void*>(fn),
                             reinterpret_cast<void*>(&get_screen_dims_detour))) {
        g_fn.store(fn, std::memory_order_relaxed);
        g_hooked.store(true, std::memory_order_release);
        LOGX("[huddims] hooked ILTClient+0x2C at 0x%08X (iface 0x%08X)", static_cast<unsigned>(fn),
             static_cast<unsigned>(iface));
    }
}

void HudScreenDims::on_frame() {
    try_install();
}

HudScreenDims::State HudScreenDims::state() const {
    State s{};
    s.hooked = g_hooked.load(std::memory_order_acquire);
    s.fn = g_fn.load(std::memory_order_relaxed);
    s.calls = g_calls.load(std::memory_order_relaxed);
    s.rewrites = g_rewrites.load(std::memory_order_relaxed);
    s.last_w = g_last_w.load(std::memory_order_relaxed);
    s.last_h = g_last_h.load(std::memory_order_relaxed);
    return s;
}
