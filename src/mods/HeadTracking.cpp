#include "HeadTracking.hpp"

#include <atomic>
#include <cinttypes>
#include <cmath>

#include "sdk/PlayerMgr.hpp"

#include "Hooks.hpp"
#include "Log.hpp"

namespace {

constexpr const char* kHookName = "PlayerCamera_UpdateAttachedRotation";

std::atomic<bool> g_enabled{false};
std::atomic<float> g_quat[4]{};          // x, y, z, w
std::atomic<uint64_t> g_writer_calls{0};
std::atomic<uint64_t> g_writes{0};
std::atomic<uintptr_t> g_last_holder{0};
std::atomic<uintptr_t> g_target{0};
std::atomic<float> g_readback[4]{};
std::atomic<bool> g_readback_ok{false};

// __thiscall(this) with no stack arguments -- `this` is the camera holder, the same object ClampPitch reads
// +324 from. x86 detour form is __fastcall with the edx placeholder (AGENTS.md rule 1).
void __fastcall attached_rotation_detour(void* self, void* /*edx*/) {
    g_writer_calls.fetch_add(1, std::memory_order_relaxed);
    const auto holder = reinterpret_cast<uintptr_t>(self);
    g_last_holder.store(holder, std::memory_order_relaxed);

    auto* hook = Hooks::get().find(kHookName);
    if (hook != nullptr) {
        // ORIGINAL FIRST. It writes the outer operand unconditionally -- identity in the ordinary case -- so
        // amending before it would be overwritten, which is exactly what the probe measured as Reclaimed.
        hook->original<void(__fastcall*)(void*, void*)>()(self, nullptr);
    }

    if (!g_enabled.load(std::memory_order_relaxed) || holder == 0) {
        return;
    }

    const std::array<float, 4> q{g_quat[0].load(std::memory_order_relaxed),
                                 g_quat[1].load(std::memory_order_relaxed),
                                 g_quat[2].load(std::memory_order_relaxed),
                                 g_quat[3].load(std::memory_order_relaxed)};
    if (!sdk::PlayerMgr::write_camera_outer_rotation(holder, q)) {
        return;
    }
    g_writes.fetch_add(1, std::memory_order_relaxed);

    // READ BACK IN THE SAME DETOUR. The point is not to check that a store worked -- it is that nothing
    // between here and the composition puts identity back. Sampled from the IPC thread this would land after
    // the next frame's writer had already run and would report failure on a working feature.
    if (const auto got = sdk::PlayerMgr::camera_rotation_operands_from_holder(holder)) {
        bool same = true;
        for (size_t i = 0; i < 4; ++i) {
            g_readback[i].store(got->outer[i], std::memory_order_relaxed);
            if (std::fabs(got->outer[i] - q[i]) > 1e-6f) {
                same = false;
            }
        }
        g_readback_ok.store(same, std::memory_order_relaxed);
    }
}

}  // namespace

HeadTracking& HeadTracking::get() {
    static HeadTracking s_instance;
    return s_instance;
}

std::optional<std::string> HeadTracking::on_initialize() {
    // Identity until a caller says otherwise, so enabling before setting a rotation is a no-op rather than a
    // snap to some uninitialised orientation.
    g_quat[3].store(1.0f, std::memory_order_relaxed);

    const uintptr_t target = sdk::PlayerMgr::camera_attached_rotation_fn();
    g_target.store(target, std::memory_order_relaxed);
    if (target == 0) {
        LOGX("[headtrack] PlayerCamera_UpdateAttachedRotation not found -- no composition point");
        return std::string{"PlayerCamera_UpdateAttachedRotation pattern did not resolve"};
    }
    if (!Hooks::get().install(kHookName, reinterpret_cast<void*>(target),
                              reinterpret_cast<void*>(&attached_rotation_detour))) {
        return std::string{"failed to hook PlayerCamera_UpdateAttachedRotation"};
    }
    LOGX("[headtrack] outer-operand writer hooked at 0x%08" PRIXPTR, target);
    return std::nullopt;
}

void HeadTracking::on_shutdown() {
    // Nothing to restore: the engine writes identity there every frame anyway, so stopping is enough.
    g_enabled.store(false, std::memory_order_relaxed);
}

void HeadTracking::set_head_rotation(const std::array<float, 4>& rotation) {
    for (float v : rotation) {
        if (!std::isfinite(v)) {
            return;
        }
    }
    for (size_t i = 0; i < 4; ++i) {
        g_quat[i].store(rotation[i], std::memory_order_relaxed);
    }
    g_enabled.store(true, std::memory_order_release);
}

void HeadTracking::clear() {
    g_enabled.store(false, std::memory_order_release);
    g_quat[0].store(0.0f, std::memory_order_relaxed);
    g_quat[1].store(0.0f, std::memory_order_relaxed);
    g_quat[2].store(0.0f, std::memory_order_relaxed);
    g_quat[3].store(1.0f, std::memory_order_relaxed);
}

HeadTracking::State HeadTracking::state() const {
    State out;
    out.enabled = g_enabled.load(std::memory_order_acquire);
    out.target = g_target.load(std::memory_order_relaxed);
    out.hooked = Hooks::get().find(kHookName) != nullptr;
    out.writer_calls = g_writer_calls.load(std::memory_order_relaxed);
    out.writes = g_writes.load(std::memory_order_relaxed);
    out.last_holder = g_last_holder.load(std::memory_order_relaxed);
    for (size_t i = 0; i < 4; ++i) {
        out.requested[i] = g_quat[i].load(std::memory_order_relaxed);
        out.readback[i] = g_readback[i].load(std::memory_order_relaxed);
    }
    out.readback_matches = g_readback_ok.load(std::memory_order_relaxed);
    return out;
}
