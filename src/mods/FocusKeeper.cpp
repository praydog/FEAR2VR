#include "FocusKeeper.hpp"

#include <atomic>
#include <cinttypes>

#include "sdk/Engine.hpp"
#include "sdk/Input.hpp"

#include "Hooks.hpp"
#include "Log.hpp"

namespace {

constexpr const char* kHookName = "ILTTimer::SetPaused";

std::atomic<bool> g_enabled{false};
std::atomic<uint64_t> g_requests{0};
std::atomic<uint64_t> g_suppressed{0};
std::atomic<uint64_t> g_passed{0};
std::atomic<uintptr_t> g_fn{0};

// __stdcall(HLTTIMER, bool) -> LT error code. 0 is LT_OK, which is what the engine's own implementation
// returns after writing the byte, so returning it without writing is indistinguishable to the caller.
using SetPausedFn = int(__stdcall*)(void*, char);

int __stdcall set_paused_detour(void* timer, char paused) {
    auto* hook = Hooks::get().find(kHookName);

    if (paused != 0) {
        g_requests.fetch_add(1, std::memory_order_relaxed);

        // THE DISCRIMINATOR. A pause requested while the window is focused is the pause menu and is none of our
        // business; one requested while the engine's own latch says focus was lost is the alt-tab pause, and
        // that is the only one refused. Reading the engine's latch rather than asking the OS keeps this
        // consistent with whatever the WndProc last decided.
        const auto focus = sdk::Input::focus();
        const bool unfocused = focus.has_value() && focus->lost_focus;

        if (g_enabled.load(std::memory_order_relaxed) && unfocused) {
            g_suppressed.fetch_add(1, std::memory_order_relaxed);
            return 0;  // LT_OK, byte untouched -- the timer keeps its scale and time keeps moving
        }
        g_passed.fetch_add(1, std::memory_order_relaxed);
    }

    if (hook == nullptr) {
        return 0;
    }
    return hook->call<int, void*, char>(timer, paused);
}

}  // namespace

FocusKeeper& FocusKeeper::get() {
    static FocusKeeper s_instance;
    return s_instance;
}

std::optional<std::string> FocusKeeper::on_initialize() {
    const uintptr_t fn = sdk::Engine::timer_set_paused_fn();
    g_fn.store(fn, std::memory_order_relaxed);
    if (fn == 0) {
        LOGX("[focuskeeper] ILTTimer::SetPaused not found -- alt-tab pause cannot be refused");
        return std::string{"ILTTimer::SetPaused pattern did not resolve"};
    }
    if (!Hooks::get().install(kHookName, reinterpret_cast<void*>(fn),
                              reinterpret_cast<void*>(&set_paused_detour))) {
        return std::string{"failed to hook ILTTimer::SetPaused"};
    }
    LOGX("[focuskeeper] hooked ILTTimer::SetPaused at 0x%08" PRIXPTR " (disabled until asked)", fn);
    return std::nullopt;
}

void FocusKeeper::keep_running(bool on) {
    if (on) {
        g_requests.store(0, std::memory_order_relaxed);
        g_suppressed.store(0, std::memory_order_relaxed);
        g_passed.store(0, std::memory_order_relaxed);
    }
    g_enabled.store(on, std::memory_order_relaxed);
    LOGX("[focuskeeper] refusing focus-loss pauses: %s", on ? "ON" : "OFF");

    if (!on) {
        return;
    }

    // CLEARING A PAUSE THAT IS ALREADY IN EFFECT, which refusing future requests cannot do.
    //
    // Measured the hard way: enabling while alt-tabbed reported 0 pause requests and a clock still frozen,
    // because the pause had been set BEFORE the hook was asked to refuse anything. A detour only sees calls
    // that happen after it; the byte was already 1.
    //
    // Only when the engine's own latch says focus was lost. Enabling with the window focused and the game
    // paused is the pause MENU, and unpausing that would be a behaviour change nobody asked for.
    const auto focus = sdk::Input::focus();
    if (!focus.has_value() || !focus->lost_focus) {
        return;
    }
    const auto clock = sdk::Engine::clock_state();
    if (!clock.has_value() || !clock->paused) {
        return;
    }
    const auto addrs = sdk::Engine::client_time_addresses();
    auto* hook = Hooks::get().find(kHookName);
    if (!addrs.has_value() || hook == nullptr) {
        return;
    }
    // Through the engine's OWN API rather than by poking the byte: the handle it takes IS the timer node, and
    // going through the trampoline means whatever else that function does stays done.
    const int rc = hook->call<int, void*, char>(reinterpret_cast<void*>(addrs->owner), 0);
    LOGX("[focuskeeper] cleared an existing focus pause on timer 0x%08" PRIXPTR " (rc %d)", addrs->owner, rc);
}

FocusKeeper::State FocusKeeper::state() const {
    State out;
    out.enabled = g_enabled.load(std::memory_order_relaxed);
    out.set_paused_fn = g_fn.load(std::memory_order_relaxed);
    out.hook_installed = Hooks::get().find(kHookName) != nullptr;
    out.pause_requests = g_requests.load(std::memory_order_relaxed);
    out.suppressed = g_suppressed.load(std::memory_order_relaxed);
    out.passed_through = g_passed.load(std::memory_order_relaxed);
    if (const auto f = sdk::Input::focus()) {
        out.lost_focus = f->lost_focus;
    }
    if (const auto gated = sdk::Input::simulation_is_gated()) {
        out.window_active = !*gated;
    }
    return out;
}
