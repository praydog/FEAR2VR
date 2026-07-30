#include "FocusKeeper.hpp"

#include <atomic>
#include <cinttypes>

#include "sdk/Input.hpp"
#include "sdk/Memory.hpp"

#include "Log.hpp"
#include "ViewHook.hpp"

namespace {

std::atomic<bool> g_holding{false};
std::atomic<uintptr_t> g_flag{0};
std::atomic<uint64_t> g_writes{0};
std::atomic<uint64_t> g_cleared{0};
std::atomic<uint64_t> g_input_inactive{0};
std::atomic<uint64_t> g_last_look{0};

}  // namespace

FocusKeeper& FocusKeeper::get() {
    static FocusKeeper s_instance;
    return s_instance;
}

std::optional<std::string> FocusKeeper::on_initialize() {
    const auto addr = sdk::Input::client_active_address();
    g_flag.store(addr, std::memory_order_relaxed);
    if (addr == 0) {
        LOGX("[focuskeeper] client-active flag unresolved; holding unavailable");
        return std::string{"Input::client_active_address() did not resolve"};
    }
    LOGX("[focuskeeper] client-active flag at 0x%08" PRIXPTR, addr);
    return std::nullopt;
}

void FocusKeeper::on_frame() {
    const uintptr_t addr = g_flag.load(std::memory_order_relaxed);
    if (addr == 0) {
        return;
    }

    // READ FIRST, ALWAYS. The engine's own view of focus is worth reporting whether or not we are holding, and
    // it is the only way to tell "we forced it" from "it was already active".
    const auto cur = sdk::mem::read<uint8_t>(addr);
    const bool was_active = cur.has_value() && *cur != 0;

    // INPUT LEAK DETECTION, and it only means something while the window is genuinely inactive. ApplyLookDelta's
    // counter is the cheapest evidence that the engine processed a look: if it advances while the window is not
    // active, holding the flag has let input through and any session running this needs to know.
    const uint64_t look = ViewHook::get().observed().calls;
    const uint64_t prev = g_last_look.exchange(look, std::memory_order_relaxed);
    if (!was_active && look > prev) {
        g_input_inactive.fetch_add(look - prev, std::memory_order_relaxed);
    }

    if (!g_holding.load(std::memory_order_relaxed)) {
        return;
    }

    // RE-ASSERT. LTClient_WndProc clears this on a WM_ACTIVATEAPP that arrives while minimised or with a lost
    // device, so a one-shot write does not survive. Counting the frames we found it cleared measures how hard
    // the engine is fighting us, which is the difference between "held" and "held on average".
    if (!was_active) {
        g_cleared.fetch_add(1, std::memory_order_relaxed);
    }
    const uint8_t one = 1;
    if (sdk::mem::store(addr, &one, sizeof(one))) {
        g_writes.fetch_add(1, std::memory_order_relaxed);
    }
}

void FocusKeeper::hold(bool on) {
    if (on) {
        g_writes.store(0, std::memory_order_relaxed);
        g_cleared.store(0, std::memory_order_relaxed);
        g_input_inactive.store(0, std::memory_order_relaxed);
    }
    g_holding.store(on, std::memory_order_relaxed);
    LOGX("[focuskeeper] holding %s", on ? "ON" : "OFF");
}

FocusKeeper::State FocusKeeper::state() const {
    State out;
    const uintptr_t addr = g_flag.load(std::memory_order_relaxed);
    out.holding = g_holding.load(std::memory_order_relaxed);
    out.flag_address = addr;
    out.flag_resolved = addr != 0;
    out.writes = g_writes.load(std::memory_order_relaxed);
    out.observed_cleared = g_cleared.load(std::memory_order_relaxed);
    out.input_while_inactive = g_input_inactive.load(std::memory_order_relaxed);
    if (addr != 0) {
        if (const auto cur = sdk::mem::read<uint8_t>(addr)) {
            out.window_active = *cur != 0;
        }
    }
    return out;
}
