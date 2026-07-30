#include "ViewHook.hpp"

#include <array>
#include <atomic>
#include <cmath>
#include <cinttypes>
#include <cstring>

#include "sdk/Memory.hpp"
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

// THE OVERRIDE. Bounded by a frame countdown rather than a flag, so the worst case of any bug here is a brief
// view disturbance that ends on its own -- the engine recomputes the pose every frame, so releasing the
// override needs no restore and cannot leave a stale value behind (which is exactly the trap the camera-object
// write probes fell into).
std::atomic<uint32_t> g_ov_frames{0};
std::atomic<uint32_t> g_ov_yaw_bits{0};
std::atomic<uint64_t> g_ov_applied{0};
std::atomic<uint64_t> g_ov_carried{0};
std::atomic<uint64_t> g_ov_rejected{0};
// Did the pose we wrote still stand at the next frame? Separates "recomputed" from "not propagated".
std::atomic<uint64_t> g_ov_pose_held{0};
// 0 = the APPLIED pose at +244 (derived), 1 = the VIEW rotation at +324 (the source candidate).
std::atomic<uint32_t> g_ov_target{0};
// The rotation the engine held when the override was armed. The offset is applied to THIS, never to the live
// value, or a persisting write feeds its own output back and accumulates.
std::atomic<uint32_t> g_ov_base[4]{};
std::atomic<bool> g_ov_have_base{false};
// What we wrote last frame, and whether there is anything to compare yet.
std::atomic<uint32_t> g_ov_last[4]{};
std::atomic<bool> g_ov_pending{false};
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
    // BEFORE the original runs, settle last frame's experiment: did the camera object come to hold the pose we
    // wrote? This is the only moment it can be asked -- by the time the original returns, the pose has already
    // been recomputed for this frame.
    if (g_ov_pending.exchange(false, std::memory_order_relaxed)) {
        // FIRST: does OUR POSE still stand? If the applied pose no longer holds what we wrote, something
        // recomputed it between our write and now -- which is a completely different failure from "the pose
        // stood but nothing propagated it". Without this the two are indistinguishable.
        const bool chk_source = g_ov_target.load(std::memory_order_relaxed) != 0;
        const auto still = chk_source ? sdk::PlayerMgr::view_rotation(0) : sdk::PlayerMgr::applied_rotation(0);
        if (still.has_value()) {
            bool held = true;
            for (size_t i = 0; i < still->size(); ++i) {
                uint32_t bits = 0;
                std::memcpy(&bits, &(*still)[i], sizeof(bits));
                if (bits != g_ov_last[i].load(std::memory_order_relaxed)) {
                    held = false;
                    break;
                }
            }
            if (held) {
                g_ov_pose_held.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (const auto p = sdk::PlayerMgr::player(0); p.has_value() && p->camera_object != 0) {
            std::array<float, 4> obj{};
            if (sdk::mem::copy(obj.data(), p->camera_object + 0x20, sizeof(obj))) {
                bool same = true;
                for (size_t i = 0; i < obj.size(); ++i) {
                    uint32_t bits = 0;
                    std::memcpy(&bits, &obj[i], sizeof(bits));
                    if (bits != g_ov_last[i].load(std::memory_order_relaxed)) {
                        same = false;
                        break;
                    }
                }
                if (same) {
                    g_ov_carried.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    }

    const int result = hook->original<int(__fastcall*)(uintptr_t, uintptr_t)>()(self, 0);

    // AFTER the original: replace the pose it just computed. Upstream of the camera object, which is the whole
    // point -- writing the object directly is reclaimed within a frame.
    const uint32_t frames = g_ov_frames.load(std::memory_order_relaxed);
    if (frames > 0) {
        g_ov_frames.store(frames - 1, std::memory_order_relaxed);
        float yaw = 0.0f;
        const uint32_t yb = g_ov_yaw_bits.load(std::memory_order_relaxed);
        std::memcpy(&yaw, &yb, sizeof(yaw));
        const bool write_source = g_ov_target.load(std::memory_order_relaxed) != 0;
        const auto cur = write_source ? sdk::PlayerMgr::view_rotation(0) : sdk::PlayerMgr::applied_rotation(0);
        // Capture the baseline once, on the first frame of this arming.
        if (!g_ov_have_base.load(std::memory_order_relaxed) && cur.has_value()) {
            for (size_t i = 0; i < cur->size(); ++i) {
                uint32_t bits = 0;
                std::memcpy(&bits, &(*cur)[i], sizeof(bits));
                g_ov_base[i].store(bits, std::memory_order_relaxed);
            }
            g_ov_have_base.store(true, std::memory_order_relaxed);
        }
        if (cur.has_value() && g_ov_have_base.load(std::memory_order_relaxed)) {
            // ABSOLUTE, FROM A BASELINE CAPTURED ONCE.
            //
            // The first version composed the offset onto whatever was in the field THAT frame. Because a write
            // to +324 persists, the next frame read our own output and composed again -- so a 40-degree offset
            // at ~300 fps accumulated into a spin, which is what a live test looked like: the camera rotating
            // wildly and dragging the player's movement direction with it.
            //
            // That was a bug in the experiment and not in the mapping, and the accumulation is itself the
            // proof: only a field the engine genuinely derives the view from could do that. So the offset is
            // now applied to a baseline captured on the first armed frame, which makes the write ABSOLUTE --
            // yaw=0 holds the view still, and any other value holds it at a fixed rotation. That is the shape a
            // head pose takes: an absolute orientation per frame, not a delta.
            const float half = yaw * 0.5f * 0.01745329252f;
            const float sy = std::sin(half);
            const float cy = std::cos(half);
            const std::array<float, 4> q{0.0f, sy, 0.0f, cy};
            const auto& a = q;
            // THE BASELINE, not the live value -- that is what makes this absolute instead of cumulative.
            std::array<float, 4> base{};
            for (size_t i = 0; i < base.size(); ++i) {
                const uint32_t bits = g_ov_base[i].load(std::memory_order_relaxed);
                std::memcpy(&base[i], &bits, sizeof(base[i]));
            }
            const auto& b = base;
            // Hamilton product q * current, x,y,z,w order.
            std::array<float, 4> out{
                a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
                a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
                a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
                a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
            const bool wrote = write_source ? sdk::PlayerMgr::write_view_rotation(0, out)
                                            : sdk::PlayerMgr::write_applied_rotation(0, out);
            if (wrote) {
                g_ov_applied.fetch_add(1, std::memory_order_relaxed);
                for (size_t i = 0; i < out.size(); ++i) {
                    uint32_t bits = 0;
                    std::memcpy(&bits, &out[i], sizeof(bits));
                    g_ov_last[i].store(bits, std::memory_order_relaxed);
                }
                g_ov_pending.store(true, std::memory_order_relaxed);
            } else {
                g_ov_rejected.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

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
    out.override_frames_left = g_ov_frames.load(std::memory_order_relaxed);
    const uint32_t yb = g_ov_yaw_bits.load(std::memory_order_relaxed);
    std::memcpy(&out.override_yaw_deg, &yb, sizeof(out.override_yaw_deg));
    out.override_applied = g_ov_applied.load(std::memory_order_relaxed);
    out.override_carried = g_ov_carried.load(std::memory_order_relaxed);
    out.override_rejected = g_ov_rejected.load(std::memory_order_relaxed);
    out.override_pose_held = g_ov_pose_held.load(std::memory_order_relaxed);
    return out;
}

void ViewHook::arm_override(float yaw_deg, uint32_t frames, bool write_source) {
    g_ov_target.store(write_source ? 1u : 0u, std::memory_order_relaxed);
    // Clamped so a typo cannot hold the view for minutes. At ~300 calls/second, 3000 frames is about ten
    // seconds, which is far more than any go/no-go needs.
    const uint32_t bounded = frames > 3000u ? 3000u : frames;
    uint32_t bits = 0;
    std::memcpy(&bits, &yaw_deg, sizeof(bits));
    g_ov_yaw_bits.store(bits, std::memory_order_relaxed);
    g_ov_applied.store(0, std::memory_order_relaxed);
    g_ov_carried.store(0, std::memory_order_relaxed);
    g_ov_rejected.store(0, std::memory_order_relaxed);
    g_ov_pose_held.store(0, std::memory_order_relaxed);
    g_ov_have_base.store(false, std::memory_order_relaxed);
    g_ov_pending.store(false, std::memory_order_relaxed);
    g_ov_frames.store(bounded, std::memory_order_relaxed);
    LOGX("[viewhook] override armed: %.2f deg yaw for %u frames", yaw_deg, bounded);
}
