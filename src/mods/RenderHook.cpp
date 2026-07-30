#include "RenderHook.hpp"

#include <windows.h>

#include <atomic>
#include <cinttypes>

#include "sdk/Render.hpp"
#include "sdk/SceneCamera.hpp"

#include "Hooks.hpp"
#include "Log.hpp"

namespace {

constexpr const char* kHookName = "LTRenderer_PresentAndSync";

std::atomic<uint64_t> g_frames{0};
std::atomic<uint64_t> g_state_not_one{0};
std::atomic<uint32_t> g_last_state{0};
std::atomic<uint32_t> g_samples{0};
std::atomic<double> g_mean_ms{0.0};
std::atomic<int64_t> g_last_qpc{0};
std::atomic<int64_t> g_qpc_freq{0};

std::atomic<RenderHook::PresentCallback> g_callbacks[RenderHook::kMaxCallbacks]{};
std::atomic<uint32_t> g_callback_count{0};

// __thiscall(this) in the original; the detour form for x86 is __fastcall with the edx placeholder
// (AGENT.MD rule 1). Returns int, which the engine's own tail call produces.
int __fastcall present_detour(void* self, void* /*edx*/) {
    // SAME-PHASE READ. The gate above this function requires the renderer state to equal 1, and this is the
    // only place that claim can be checked against the frame it describes -- an IPC-thread read lands in a
    // different phase and answers a different question.
    if (const auto st = sdk::SceneCamera::state()) {
        g_last_state.store(*st, std::memory_order_relaxed);
        if (*st != 1) {
            g_state_not_one.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // Frame interval from QPC rather than the engine clock: the engine clock is scaled and pausable (see
    // Engine::clock_state), so it measures GAME time, while frame pacing is a wall-clock question.
    int64_t freq = g_qpc_freq.load(std::memory_order_relaxed);
    if (freq == 0) {
        LARGE_INTEGER f{};
        if (QueryPerformanceFrequency(&f)) {
            freq = f.QuadPart;
            g_qpc_freq.store(freq, std::memory_order_relaxed);
        }
    }
    if (freq > 0) {
        LARGE_INTEGER now{};
        if (QueryPerformanceCounter(&now)) {
            const int64_t prev = g_last_qpc.exchange(now.QuadPart, std::memory_order_relaxed);
            if (prev != 0 && now.QuadPart > prev) {
                const double ms = static_cast<double>(now.QuadPart - prev) * 1000.0 / static_cast<double>(freq);
                // Running mean, so nothing accumulates without bound and no buffer is needed in a hot path.
                const uint32_t n = g_samples.fetch_add(1, std::memory_order_relaxed) + 1;
                const double old_mean = g_mean_ms.load(std::memory_order_relaxed);
                g_mean_ms.store(old_mean + (ms - old_mean) / static_cast<double>(n),
                                std::memory_order_relaxed);
            }
        }
    }

    g_frames.fetch_add(1, std::memory_order_relaxed);

    // Consumers run BEFORE the engine presents -- that is the whole point of the boundary. Read the count once
    // so a registration racing with a frame cannot make this loop read past what was published.
    const uint32_t n = g_callback_count.load(std::memory_order_acquire);
    for (uint32_t i = 0; i < n && i < RenderHook::kMaxCallbacks; ++i) {
        if (auto cb = g_callbacks[i].load(std::memory_order_relaxed)) {
            cb();
        }
    }

    auto* hook = Hooks::get().find(kHookName);
    if (hook == nullptr) {
        return 0;
    }
    // original<Fn*>(), NOT call<>(). AGENT.MD rule 7 says so and this is why it is a rule: safetyhook's call<>
    // uses the compiler's DEFAULT convention (__cdecl on x86) and takes a scoped_lock on the hook.
    //
    // Both halves are fatal here. The original is __thiscall, so __cdecl put `this` on the stack instead of
    // ECX and the original dereferenced garbage -- that crashed FEAR2 on the first presented frame. And a
    // mutex in a per-frame detour is the blocking this file's own header forbids, on the one code path where
    // retire() may be holding the other side.
    return hook->original<int(__fastcall*)(void*, void*)>()(self, nullptr);
}

}  // namespace

RenderHook& RenderHook::get() {
    static RenderHook s_instance;
    return s_instance;
}

std::optional<std::string> RenderHook::on_initialize() {
    const uintptr_t target = sdk::Render::engine_present_fn();
    if (target == 0) {
        LOGX("[renderhook] LTRenderer_PresentAndSync not found -- no frame boundary");
        return std::string{"LTRenderer_PresentAndSync pattern did not resolve"};
    }
    if (!Hooks::get().install(kHookName, reinterpret_cast<void*>(target),
                              reinterpret_cast<void*>(&present_detour))) {
        return std::string{"failed to hook LTRenderer_PresentAndSync"};
    }
    LOGX("[renderhook] frame boundary hooked at 0x%08" PRIXPTR, target);
    return std::nullopt;
}

bool RenderHook::add_present_callback(PresentCallback cb) {
    if (cb == nullptr) {
        return false;
    }
    const uint32_t n = g_callback_count.load(std::memory_order_relaxed);
    if (n >= kMaxCallbacks) {
        return false;
    }
    g_callbacks[n].store(cb, std::memory_order_relaxed);
    // Release AFTER the slot is written, so the detour never sees a count covering an empty slot.
    g_callback_count.store(n + 1, std::memory_order_release);
    return true;
}

RenderHook::Stats RenderHook::stats() const {
    Stats out;
    out.target = sdk::Render::engine_present_fn();
    out.hooked = Hooks::get().find(kHookName) != nullptr;
    out.frames = g_frames.load(std::memory_order_relaxed);
    out.mean_interval_ms = g_mean_ms.load(std::memory_order_relaxed);
    out.samples = g_samples.load(std::memory_order_relaxed);
    out.state_at_present = g_last_state.load(std::memory_order_relaxed);
    out.state_not_one = g_state_not_one.load(std::memory_order_relaxed);
    out.callbacks = g_callback_count.load(std::memory_order_relaxed);
    return out;
}
