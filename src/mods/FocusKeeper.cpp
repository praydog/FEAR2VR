#include "FocusKeeper.hpp"

#include <atomic>
#include <cinttypes>

#include "sdk/Engine.hpp"
#include "sdk/Input.hpp"

#include "Hooks.hpp"
#include "Log.hpp"
#include "sdk/Modules.hpp"

namespace {

constexpr const char* kHookName = "ILTTimer::SetPaused";

// ---- THE OTHER HALF OF LOSING FOCUS -----------------------------------------------------------
//
// Defeating the alt-tab PAUSE is not enough. WinMain's frame loop (FEAR2.exe 0x466F8D) begins:
//
//     cmp  g_ClientGlob_bLostFocus, 0     ; 0x4671C4
//     jz   short skip                     ; 0x4671CA   74 08
//     push 5                              ; 0x4671CC
//     call Sleep                          ; 0x4671CE
//   skip:
//     call CClientMgr::Update
//
// so an unfocused client sleeps 5 ms EVERY iteration. The engine calls timeBeginPeriod(1) at
// startup, so that is a real ~5 ms rather than a rounded scheduler tick, and on top of a ~4-5 ms
// frame it pins the game near 100 fps. It is the only branch in the binary that makes a timing
// decision from a focus flag.
//
// This is not merely a measurement nuisance. A WEARER IS NEVER LOOKING AT THE DESKTOP WINDOW, so
// for the use this mod exists to serve, the throttled path is the normal one.
//
// Patching the BRANCH rather than writing the flag, deliberately. LTClient_WndProc treats
// g_ClientGlob_bLostFocus as a transition latch: on WM_ACTIVATEAPP it takes the restore path only
// `if (lostFocus)`, and that path is what sets bClientActive and calls LTRender_InitRender. A mod
// that forces the flag to zero therefore makes a later minimise/restore skip the renderer restore
// entirely. Changing 0x74 to 0xEB alters timing and nothing else.
constexpr uintptr_t kUnfocusedSleepBranch = 0x0671CA;  // VA 0x4671CA at imagebase 0x400000
constexpr uint8_t kJzShort = 0x74;
constexpr uint8_t kJmpShort = 0xEB;

std::atomic<bool> g_rate_patched{false};

// Same length replacing same length -- a two-byte instruction becomes a different two-byte
// instruction -- so the main thread cannot decode a torn or mis-sized instruction mid-write.
bool write_branch_byte(uint8_t want, uint8_t expect) {
    const auto* exe = sdk::Modules::get().exe();
    if (exe == nullptr || exe->base == 0) {
        return false;
    }
    auto* const at = reinterpret_cast<uint8_t*>(exe->base + kUnfocusedSleepBranch);
    DWORD old_protect = 0;
    if (!VirtualProtect(at, 1, PAGE_EXECUTE_READWRITE, &old_protect)) {
        return false;
    }
    const bool ok = (*at == expect);
    if (ok) {
        *at = want;
    }
    DWORD ignored = 0;
    VirtualProtect(at, 1, old_protect, &ignored);
    if (!ok) {
        LOGX("[focus] frame-loop branch is 0x%02X, expected 0x%02X -- refusing to patch a byte that "
             "is not the instruction this was written for", *at, expect);
    }
    return ok;
}

// ON BY DEFAULT. This is not a debug convenience: a headset does not care whether the desktop window has
// focus, and a VR mod whose world freezes the moment the user looks at another monitor is broken. The desktop
// game's alt-tab pause is the behaviour that is wrong here, not the override.
std::atomic<bool> g_enabled{true};
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
    // original<Fn*>() with the REAL convention -- see RenderHook.cpp for what call<> costs. ILTTimer::SetPaused
    // is __stdcall(handle, bool) and ends in `retn 8`, so calling it as __cdecl leaks 8 bytes of stack on every
    // pass-through. It survived testing only because the measured path refuses and returns early; the pause
    // MENU takes this branch, and it would have drifted the stack once per pause.
    return hook->original<int(__stdcall*)(void*, char)>()(timer, paused);
}


// CLEARS A PAUSE THAT IS ALREADY IN EFFECT, which refusing future requests cannot do.
//
// Measured the hard way: switching the refusal on while alt-tabbed reported 0 pause requests and a clock still
// frozen, because the pause had been set BEFORE the detour existed. A hook only sees calls that happen after
// it; the byte was already 1. Needed at initialize() too -- the payload is routinely injected while the window
// is NOT focused, since that is exactly when a tester is reading output in another window.
//
// Only acts when the engine's own latch says focus was lost. Doing this with the window focused and the game
// paused would unpause the pause MENU, which is a behaviour change nobody asked for.
void clear_existing_focus_pause() {
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
    const int rc = hook->original<int(__stdcall*)(void*, char)>()(
        reinterpret_cast<void*>(addrs->owner), 0);
    LOGX("[focuskeeper] cleared an existing focus pause on timer 0x%08" PRIXPTR " (rc %d)", addrs->owner, rc);
}

}  // namespace

FocusKeeper& FocusKeeper::get() {
    static FocusKeeper s_instance;
    return s_instance;
}

std::optional<std::string> FocusKeeper::on_initialize() {
    if (write_branch_byte(kJmpShort, kJzShort)) {
        g_rate_patched.store(true, std::memory_order_release);
        LOGX("[focus] unfocused frame cap removed (the 5 ms sleep per loop while alt-tabbed)");
    }

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
    LOGX("[focuskeeper] hooked ILTTimer::SetPaused at 0x%08" PRIXPTR " -- refusing focus-loss pauses", fn);
    // Also clear a pause that is ALREADY in effect: the mod is commonly injected while the window is not
    // focused (that is exactly when a tester is reading output), and a detour cannot see a call that already
    // happened. Same one-shot clear keep_running() performs, for the same measured reason.
    clear_existing_focus_pause();
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
    clear_existing_focus_pause();
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

void FocusKeeper::on_shutdown() {
    // Put the byte back. An injected mod that can be unloaded does not get to leave the executable
    // modified behind it -- the next inject would then find an instruction it did not expect and
    // correctly refuse to patch it.
    if (g_rate_patched.exchange(false, std::memory_order_acq_rel)) {
        write_branch_byte(kJzShort, kJmpShort);
    }
}
