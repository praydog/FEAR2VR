#include "FireRedirect.hpp"

#include <cmath>

#include <safetyhook.hpp>

#include "../Hooks.hpp"
#include "Log.hpp"
#include "sdk/Memory.hpp"
#include "sdk/Modules.hpp"

namespace {
// Weapon_TraceShot (sub_1014F4D0) prologue, gameserver.dll.
//
// THE THIRD TARGET, and the two rejected ones are the useful part of this
// comment because each was rejected by measurement, not by reading:
//
//   Weapon_FireServer entry          6 writes, impacts moved +1.23 deg when
//                                    asked for +40. Its VectorsPerRound loop
//                                    rewrites descriptor+0 with the per-pellet
//                                    spread direction after we run.
//   Weapon_FireHitscanVector entry   6 writes, impacts moved +0.06 deg when
//                                    asked for +25. It calls sub_1014D350 with
//                                    the descriptor before doing any work,
//                                    which refills the direction from the
//                                    weapon's own state.
//
// Both wrote successfully. Neither moved a bullet. A hook that fires on the
// right function at the wrong instant looks identical to a working one from
// the write counter alone, which is why `fr_writes` was never sufficient
// evidence and the impact bearing always had to be measured.
//
// This is the function the hitscan path actually does its work in -- the ELSE
// of `if (sub_100B94A0(desc))`, that predicate being an AABB containment test
// for a muzzle-inside-target contact case rather than the trace it looked like.
//
//   mov  eax, [esp+arg_4]
//   mov  ecx, dword_10333D1C
//   push esi
//   mov  esi, [esp+4+arg_28]
//   push esi / push eax
//   call sub_1011D810
constexpr const char* kFirePattern =
    "8B 44 24 08 8B 0D ? ? ? ? 56 8B 74 24 30 56 50 E8 ? ? ? ? 8B 0D ? ? ? ?";

// __stdcall(a1..a11) with the descriptor as a8: at the entry instruction the
// return address is at esp+0 and a8 is seven slots past a1.
constexpr uintptr_t kDescriptorStackOffset = 4 + 7 * 4;

// Fields within the descriptor, established in MAPPING_WORKFLOW.MD.
constexpr uintptr_t kDescriptorDirection = 0x00;
constexpr uintptr_t kDescriptorOrigin = 0x0C;

// gameserver.dll appears at session start, so a miss during initialize() is
// expected rather than fatal. Retry on a slow cadence -- a scan per frame would
// be wasted work for the entire time the player sits at a menu.
constexpr uint32_t kRescanFrames = 240;

void store3(std::atomic<float> (&dst)[3], float x, float y, float z) {
    dst[0].store(x, std::memory_order_relaxed);
    dst[1].store(y, std::memory_order_relaxed);
    dst[2].store(z, std::memory_order_relaxed);
}

std::array<float, 3> load3(const std::atomic<float> (&src)[3]) {
    return {src[0].load(std::memory_order_relaxed), src[1].load(std::memory_order_relaxed),
            src[2].load(std::memory_order_relaxed)};
}
} // namespace

FireRedirect& FireRedirect::get() {
    static FireRedirect instance;
    return instance;
}

std::array<float, 3> FireRedirect::last_engine_dir() const {
    return load3(m_engine_dir);
}

std::array<float, 3> FireRedirect::last_written_dir() const {
    return load3(m_written_dir);
}

std::array<float, 3> FireRedirect::last_origin() const {
    return load3(m_origin);
}

bool FireRedirect::set_direction(float x, float y, float z) {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) {
        return false;
    }
    // A direction that is not unit length is a symptom, not an inconvenience:
    // normalizing here would convert a caller's scaling bug into a subtly wrong
    // aim that nobody can see. Refuse it and let the caller be wrong loudly.
    const float len = std::sqrt(x * x + y * y + z * z);
    if (!(len > 0.999f && len < 1.001f)) {
        return false;
    }
    store3(m_dir, x, y, z);
    m_armed.store(true, std::memory_order_release);
    return true;
}

void FireRedirect::clear_direction() {
    m_armed.store(false, std::memory_order_release);
}

void FireRedirect::on_fire(SafetyHookContext& ctx) {
    auto& self = FireRedirect::get();
    self.m_calls.fetch_add(1, std::memory_order_relaxed);

    const uintptr_t desc = sdk::mem::read<uintptr_t>(ctx.esp + kDescriptorStackOffset).value_or(0);
    if (desc == 0) {
        return;
    }

    // Record what the ENGINE was about to fire before touching anything. Without
    // this a passing test proves only that our value is present, not that it
    // replaced a different one.
    const auto engine_dir = sdk::mem::read<std::array<float, 3>>(desc + kDescriptorDirection);
    const auto origin = sdk::mem::read<std::array<float, 3>>(desc + kDescriptorOrigin);
    if (!engine_dir.has_value() || !origin.has_value()) {
        return; // descriptor not readable -- our layout belief is wrong, do nothing
    }
    store3(self.m_engine_dir, (*engine_dir)[0], (*engine_dir)[1], (*engine_dir)[2]);
    store3(self.m_origin, (*origin)[0], (*origin)[1], (*origin)[2]);

    if (!self.m_armed.load(std::memory_order_acquire)) {
        return;
    }

    const auto dir = load3(self.m_dir);
    if (!sdk::mem::write<std::array<float, 3>>(desc + kDescriptorDirection, dir)) {
        return;
    }
    store3(self.m_written_dir, dir[0], dir[1], dir[2]);
    self.m_writes.fetch_add(1, std::memory_order_relaxed);
}

std::optional<std::string> FireRedirect::on_initialize() {
    // Not an error when it misses: the module is lazy. on_frame() retries.
    const uintptr_t target = sdk::Modules::get().scan_game_server(kFirePattern, "Weapon_TraceShot");
    if (target == 0) {
        m_retry_countdown.store(kRescanFrames, std::memory_order_relaxed);
        return std::nullopt;
    }
    m_target.store(target, std::memory_order_relaxed);
    m_hooked.store(Hooks::get().install_mid("weapon_trace_shot", reinterpret_cast<void*>(target), &FireRedirect::on_fire),
                   std::memory_order_relaxed);
    return std::nullopt;
}

void FireRedirect::on_frame() {
    if (m_hooked.load(std::memory_order_relaxed)) {
        return;
    }
    // The module arrives when a session starts, which is long after our own
    // initialize(). Poll for it, slowly.
    auto remaining = m_retry_countdown.load(std::memory_order_relaxed);
    if (remaining > 0) {
        m_retry_countdown.store(remaining - 1, std::memory_order_relaxed);
        return;
    }
    m_retry_countdown.store(kRescanFrames, std::memory_order_relaxed);

    const uintptr_t target = sdk::Modules::get().scan_game_server(kFirePattern, "Weapon_TraceShot");
    if (target == 0) {
        return;
    }
    m_target.store(target, std::memory_order_relaxed);
    m_hooked.store(Hooks::get().install_mid("weapon_trace_shot", reinterpret_cast<void*>(target), &FireRedirect::on_fire),
                   std::memory_order_relaxed);
}

void FireRedirect::on_shutdown() {
    // Disarm before hook retirement so a shot in flight during teardown gets the
    // engine's own direction rather than a stale one of ours.
    clear_direction();
}
