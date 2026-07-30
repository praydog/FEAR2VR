#include "ViewHook.hpp"

#include <atomic>
#include <cinttypes>
#include <cstring>

#include "sdk/Modules.hpp"
#include "sdk/PlayerMgr.hpp"

#include "../Hooks.hpp"
#include "Log.hpp"

namespace {

// THE HOOK'S NAME IS THE REGISTRY KEY and the detour's way back to the trampoline, so it is defined once.
constexpr const char* kHookName = "CPlayerCamera::ApplyLookDelta";

// OBSERVATION STATE, written from the game thread and read from the IPC thread.
//
// Relaxed atomics: these are counters and a snapshot, never a decision. Nothing branches on them inside the
// engine's hot path, so ordering between them does not matter and a torn pair would only mislabel one
// diagnostic sample. Making them seq_cst would put a fence in the camera update for no benefit.
std::atomic<uint64_t> g_calls{0};
std::atomic<uintptr_t> g_last_this{0};
std::atomic<uint32_t> g_last_a3_bits{0};
std::atomic<bool> g_installed{false};
std::atomic<uintptr_t> g_target{0};

// The detour. `edx` is the dummy that makes an x86 __thiscall reachable as __fastcall (AGENT.MD rule 1), and
// the three stack arguments match `retn 0Ch` at both of the original's exits.
//
// STAY LEAN AND NEVER THROW. This runs inside the camera update on the engine's thread every frame; anything
// slow here is a frame-time regression, and anything that unwinds crosses back into engine code that has no
// idea what a C++ exception is.
int __fastcall apply_look_delta_detour(uintptr_t self, uintptr_t /*edx*/, float* a2, float a3, float* a4) {
    g_calls.fetch_add(1, std::memory_order_relaxed);
    g_last_this.store(self, std::memory_order_relaxed);
    uint32_t bits = 0;
    static_assert(sizeof(bits) == sizeof(a3), "float and uint32_t must be the same width to alias like this");
    std::memcpy(&bits, &a3, sizeof(bits));
    g_last_a3_bits.store(bits, std::memory_order_relaxed);

    // PASS THROUGH UNCHANGED. This pass establishes that the interception point works; changing the view here
    // would conflate "the hook fires" with "the override is correct".
    //
    // The original is reached through the registry rather than a captured pointer, because Hooks::get() owns
    // the trampoline's lifetime -- a raw copy taken at install time would outlive a retire.
    auto* hook = Hooks::get().find(kHookName);
    if (hook == nullptr) {
        // Retired underneath us mid-call. There is nothing safe to call and nothing to return but a benign
        // value; the engine treats the result as a status it mostly ignores on this path.
        return 0;
    }
    return hook->original<int(__fastcall*)(uintptr_t, uintptr_t, float*, float, float*)>()(self, 0, a2, a3, a4);
}

}  // namespace

ViewHook& ViewHook::get() {
    static ViewHook s_instance;
    return s_instance;
}

std::optional<std::string> ViewHook::on_initialize() {
    const auto target = sdk::PlayerMgr::apply_look_delta_fn();
    g_target.store(target, std::memory_order_relaxed);
    if (target == 0) {
        // NOT FATAL. A missed pattern means this one feature is unavailable, and the framework is more useful
        // up-with-a-gap than refusing to initialize -- the log line from scan_game_client already names it.
        LOGX("[viewhook] target unresolved; view hook NOT installed");
        return std::string{"CPlayerCamera::ApplyLookDelta pattern did not resolve"};
    }

    if (!Hooks::get().install(kHookName, reinterpret_cast<void*>(target),
                              reinterpret_cast<void*>(&apply_look_delta_detour))) {
        LOGX("[viewhook] install FAILED at 0x%08" PRIXPTR, target);
        return std::string{"failed to install the ApplyLookDelta hook"};
    }

    g_installed.store(true, std::memory_order_relaxed);
    LOGX("[viewhook] installed at 0x%08" PRIXPTR, target);
    return std::nullopt;
}

ViewHook::Observed ViewHook::observed() const {
    Observed out;
    out.calls = g_calls.load(std::memory_order_relaxed);
    out.last_this = g_last_this.load(std::memory_order_relaxed);
    const uint32_t bits = g_last_a3_bits.load(std::memory_order_relaxed);
    std::memcpy(&out.last_a3, &bits, sizeof(out.last_a3));
    out.installed = g_installed.load(std::memory_order_relaxed);
    out.target = g_target.load(std::memory_order_relaxed);
    return out;
}
