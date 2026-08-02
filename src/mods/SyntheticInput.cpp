#include "SyntheticInput.hpp"

#include <atomic>
#include <cinttypes>

#include "sdk/Input.hpp"

#include "Hooks.hpp"
#include "Log.hpp"

namespace {

// Fixed table, no allocation: this runs on the engine's frame path. A slot is claimed by storing a non-zero
// vk, and `frames` is what the frame hook counts down. `held` slots never count down.
struct Slot {
    std::atomic<uint32_t> vk{0};      // 0 = free (VK 0 is not a real key)
    std::atomic<uint32_t> frames{0};  // remaining frames for a tap
    std::atomic<bool> held{false};    // hold() rather than tap()
};

Slot g_slots[SyntheticInput::kMaxKeys];
std::atomic<uint64_t> g_writes{0};
std::atomic<uint64_t> g_completed{0};

Slot* find_slot(uint32_t vk) {
    for (auto& s : g_slots) {
        if (s.vk.load(std::memory_order_acquire) == vk) {
            return &s;
        }
    }
    return nullptr;
}

constexpr const char* kPollHook = "ILTInput::PollDevices";

// MOUSE BUTTONS SHARE THE KEY TABLE, encoded above the virtual-key range. VK codes stop at 0xFF, so 0x100+n
// cannot collide with one, and a single slot table then schedules both without a second mechanism.
constexpr uint32_t kMouseButtonBase = 0x100;

bool send_one(uint32_t code, bool down) {
    if (code >= kMouseButtonBase) {
        return sdk::Input::send_mouse_button(static_cast<uint8_t>(code - kMouseButtonBase), down);
    }
    return sdk::Input::send_key(static_cast<uint8_t>(code), down);
}

// Pending look delta, accumulated by callers and drained by the poll detour.
std::atomic<int32_t> g_look_dx{0};
std::atomic<int32_t> g_look_dy{0};
std::atomic<uint64_t> g_look_delivered{0};
std::atomic<int32_t> g_look_last_dx{0};
std::atomic<int32_t> g_look_last_dy{0};

// Pending wheel notches, same discipline. Accumulated rather than latched so two clicks in one
// frame both arrive: the engine turns the delta back into a notch count, so a caller that spins
// the wheel faster than the frame rate still gets every detent it asked for.
std::atomic<int32_t> g_wheel{0};
std::atomic<uint64_t> g_wheel_delivered{0};

// Applies every pending key state. Called from the poll detour AFTER the original has run, which is the only
// instant in the frame where a write survives to be read -- see SyntheticInput.hpp.
void apply_pending();

Slot* claim_slot(uint32_t vk) {
    if (auto* existing = find_slot(vk)) {
        return existing;
    }
    for (auto& s : g_slots) {
        uint32_t expected = 0;
        if (s.vk.compare_exchange_strong(expected, vk, std::memory_order_acq_rel)) {
            return &s;
        }
    }
    return nullptr;
}

// __thiscall(this) with no arguments, so __fastcall with the edx placeholder (AGENTS.md rule 1).
void __fastcall poll_detour(void* self, void* /*edx*/) {
    // APPLIED BEFORE THE ORIGINAL, deliberately. These writes go to the INCOMING bank, which is exactly what
    // the poll then shifts into current -- so the engine's own pipeline produces the press edge instead of us
    // simulating one. An earlier version wrote the CURRENT state after the original and had to hit a one-call
    // window in the frame to survive; this has no such window.
    apply_pending();

    // The look delta rides the same "after the original has run" slot as the key state: the poll
    // has just finished reading the device, so a move written now is what the NEXT read sees.
    const int32_t dx = g_look_dx.exchange(0, std::memory_order_relaxed);
    const int32_t dy = g_look_dy.exchange(0, std::memory_order_relaxed);
    if ((dx != 0 || dy != 0) && sdk::Input::send_mouse_look(dx, dy)) {
        g_look_last_dx.store(dx, std::memory_order_relaxed);
        g_look_last_dy.store(dy, std::memory_order_relaxed);
        g_look_delivered.fetch_add(1, std::memory_order_relaxed);
    }

    // The wheel is an IMPULSE, not a level: the engine converts it straight into an object value
    // rather than a held state, so it is sent once and cleared rather than re-asserted per frame.
    const int32_t notches = g_wheel.exchange(0, std::memory_order_relaxed);
    if (notches != 0 && sdk::Input::send_mouse_wheel(notches)) {
        g_wheel_delivered.fetch_add(1, std::memory_order_relaxed);
    }

    auto* hook = Hooks::get().find(kPollHook);
    if (hook != nullptr) {
        hook->original<void(__fastcall*)(void*, void*)>()(self, nullptr);
    }
}

}  // namespace

SyntheticInput& SyntheticInput::get() {
    static SyntheticInput s_instance;
    return s_instance;
}

namespace {

void apply_pending() {
    for (auto& s : g_slots) {
        const uint32_t vk = s.vk.load(std::memory_order_acquire);
        if (vk == 0) {
            continue;
        }

        if (s.held.load(std::memory_order_relaxed)) {
            // RE-ASSERTED EVERY FRAME, and it has to be: the poll that runs just before this hook rewrites the
            // device array from the real keyboard, so a hold written once survives exactly one frame.
            if (send_one(vk, true)) {
                g_writes.fetch_add(1, std::memory_order_relaxed);
            }
            continue;
        }

        const uint32_t left = s.frames.load(std::memory_order_relaxed);
        if (left > 0) {
            if (send_one(vk, true)) {
                g_writes.fetch_add(1, std::memory_order_relaxed);
            }
            s.frames.store(left - 1, std::memory_order_relaxed);
            continue;
        }

        // Countdown finished: release once and free the slot. The explicit zero matters -- leaving the byte
        // set would hold the key down for as long as the poll happened not to overwrite it.
        send_one(vk, false);
        g_writes.fetch_add(1, std::memory_order_relaxed);
        g_completed.fetch_add(1, std::memory_order_relaxed);
        s.vk.store(0, std::memory_order_release);
    }
}

}  // namespace

std::optional<std::string> SyntheticInput::on_initialize() {
    const uintptr_t poll = sdk::Input::poll_fn();
    if (poll == 0) {
        LOGX("[syninput] ILTInput poll not resolved -- synthetic input unavailable");
        return std::string{"ILTInput slot 3 did not resolve"};
    }
    if (!Hooks::get().install(kPollHook, reinterpret_cast<void*>(poll),
                              reinterpret_cast<void*>(&poll_detour))) {
        return std::string{"failed to hook the ILTInput device poll"};
    }
    LOGX("[syninput] device poll hooked at 0x%08" PRIXPTR, poll);
    return std::nullopt;
}

void SyntheticInput::on_shutdown() {
    release_all();
}

bool SyntheticInput::tap(uint32_t vk, uint32_t frames) {
    if (vk == 0) {
        return false;
    }
    auto* s = claim_slot(vk);
    if (s == nullptr) {
        return false;
    }
    s->held.store(false, std::memory_order_relaxed);
    s->frames.store(frames == 0 ? 1 : frames, std::memory_order_relaxed);
    LOGX("[syninput] tap code=0x%03X for %u frame(s)", vk, frames);
    return true;
}

void SyntheticInput::hold(uint32_t vk, bool down) {
    if (vk == 0) {
        return;
    }
    if (!down) {
        if (auto* s = find_slot(vk)) {
            s->held.store(false, std::memory_order_relaxed);
            s->frames.store(0, std::memory_order_relaxed);
            send_one(vk, false);
            s->vk.store(0, std::memory_order_release);
        }
        return;
    }
    if (auto* s = claim_slot(vk)) {
        s->held.store(true, std::memory_order_relaxed);
        s->frames.store(0, std::memory_order_relaxed);
    }
}

void SyntheticInput::queue_look(int32_t dx, int32_t dy) {
    g_look_dx.fetch_add(dx, std::memory_order_relaxed);
    g_look_dy.fetch_add(dy, std::memory_order_relaxed);
}

void SyntheticInput::queue_wheel(int32_t detents) {
    g_wheel.fetch_add(detents, std::memory_order_relaxed);
}

uint64_t SyntheticInput::wheel_delivered() const {
    return g_wheel_delivered.load(std::memory_order_relaxed);
}

SyntheticInput::LookStats SyntheticInput::look_stats() const {
    LookStats out{};
    out.delivered = g_look_delivered.load(std::memory_order_relaxed);
    out.last_dx = g_look_last_dx.load(std::memory_order_relaxed);
    out.last_dy = g_look_last_dy.load(std::memory_order_relaxed);
    out.pending_dx = g_look_dx.load(std::memory_order_relaxed);
    out.pending_dy = g_look_dy.load(std::memory_order_relaxed);
    return out;
}

void SyntheticInput::release_all() {
    for (auto& s : g_slots) {
        const uint32_t vk = s.vk.exchange(0, std::memory_order_acq_rel);
        if (vk != 0) {
            s.held.store(false, std::memory_order_relaxed);
            s.frames.store(0, std::memory_order_relaxed);
            send_one(static_cast<uint8_t>(vk), false);
        }
    }
}

SyntheticInput::State SyntheticInput::state() const {
    State out;
    for (const auto& s : g_slots) {
        const uint32_t vk = s.vk.load(std::memory_order_acquire);
        if (vk == 0) {
            continue;
        }
        if (s.held.load(std::memory_order_relaxed)) {
            ++out.held_keys;
        } else {
            ++out.active_taps;
        }
    }
    out.writes = g_writes.load(std::memory_order_relaxed);
    out.taps_completed = g_completed.load(std::memory_order_relaxed);
    out.keyboard_resolved = sdk::Input::device(sdk::Input::DeviceKind::Keyboard).has_value();
    return out;
}
