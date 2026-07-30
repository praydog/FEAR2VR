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
constexpr const char* kPoseHookName = "PlayerCamera::UpdateViewPose";

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
std::atomic<uint64_t> g_pose_calls{0};
// SAME-PHASE AGREEMENT, sampled on the engine thread.
//
// Whether the applied pose equals the camera object's transform is not decidable from the IPC thread: this
// function REWRITES the pose every frame, so an out-of-band reader always lands mid-update and sees them
// stably different. Read immediately after the original returns, both sides are in the same phase and the
// question has an answer -- which is precisely what owning the writer buys.
//
// Sampled every 64th call to keep the hot path cheap; the counters are diagnostics, never decisions.
std::atomic<uint64_t> g_pose_agree_equal{0};
std::atomic<uint64_t> g_pose_agree_differ{0};
std::atomic<uint64_t> g_pose_agree_other{0};
std::atomic<uintptr_t> g_pose_last_this{0};
std::atomic<bool> g_pose_installed{false};
std::atomic<uintptr_t> g_pose_target{0};

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

// The per-frame view writer. NO stack arguments -- the original's single exit is a plain `retn` -- so this is
// __fastcall(this, edx_dummy) and the cleanup is zero on both sides.
//
// This is the hot one: it runs from the camera's update path rather than from input, so it executes whether or
// not the player is doing anything. Two relaxed stores and an increment is the entire budget here.
int __fastcall update_view_pose_detour(uintptr_t self, uintptr_t /*edx*/) {
    g_pose_calls.fetch_add(1, std::memory_order_relaxed);
    g_pose_last_this.store(self, std::memory_order_relaxed);

    auto* hook = Hooks::get().find(kPoseHookName);
    if (hook == nullptr) {
        return 0;
    }
    const int result = hook->original<int(__fastcall*)(uintptr_t, uintptr_t)>()(self, 0);

    // AFTER the original, so the pose it just wrote and the camera object are in the same phase.
    if ((g_pose_calls.load(std::memory_order_relaxed) & 63u) == 0u) {
        switch (sdk::PlayerMgr::applied_pose_agreement(0)) {
        case sdk::PlayerMgr::PoseAgreement::Equal:
            g_pose_agree_equal.fetch_add(1, std::memory_order_relaxed);
            break;
        case sdk::PlayerMgr::PoseAgreement::Differ:
            g_pose_agree_differ.fetch_add(1, std::memory_order_relaxed);
            break;
        default:
            g_pose_agree_other.fetch_add(1, std::memory_order_relaxed);
            break;
        }
    }
    return result;
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

    // THE PER-FRAME HALF. Reported independently: a miss on this one leaves the look-input hook working, and
    // saying which of the two is unavailable is more useful than one combined failure.
    const auto pose_target = sdk::PlayerMgr::update_view_pose_fn();
    g_pose_target.store(pose_target, std::memory_order_relaxed);
    if (pose_target == 0) {
        LOGX("[viewhook] UpdateViewPose target unresolved; per-frame hook NOT installed");
        return std::string{"PlayerCamera::UpdateViewPose pattern did not resolve"};
    }
    if (!Hooks::get().install(kPoseHookName, reinterpret_cast<void*>(pose_target),
                              reinterpret_cast<void*>(&update_view_pose_detour))) {
        LOGX("[viewhook] UpdateViewPose install FAILED at 0x%08" PRIXPTR, pose_target);
        return std::string{"failed to install the UpdateViewPose hook"};
    }
    g_pose_installed.store(true, std::memory_order_relaxed);
    LOGX("[viewhook] UpdateViewPose installed at 0x%08" PRIXPTR, pose_target);
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
    out.pose_calls = g_pose_calls.load(std::memory_order_relaxed);
    out.pose_last_this = g_pose_last_this.load(std::memory_order_relaxed);
    out.pose_installed = g_pose_installed.load(std::memory_order_relaxed);
    out.pose_target = g_pose_target.load(std::memory_order_relaxed);
    out.pose_agree_equal = g_pose_agree_equal.load(std::memory_order_relaxed);
    out.pose_agree_differ = g_pose_agree_differ.load(std::memory_order_relaxed);
    out.pose_agree_other = g_pose_agree_other.load(std::memory_order_relaxed);
    return out;
}
