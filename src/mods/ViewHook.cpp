#include "ViewHook.hpp"

#include <array>
#include <atomic>
#include <cmath>
#include <cinttypes>
#include <cstring>

#include "sdk/Memory.hpp"
#include "sdk/Object.hpp"
#include "sdk/Modules.hpp"
#include "sdk/PlayerMgr.hpp"

#include "../Hooks.hpp"
#include "Log.hpp"

namespace {

// THE HOOK'S NAME IS THE REGISTRY KEY and the detour's way back to the trampoline, so it is defined once.
constexpr const char* kHookName = "CPlayerCamera::ApplyLookDelta";

std::atomic<bool> g_level_aim{false};
std::atomic<uint64_t> g_level_writes{0};

// RETRACTED: LEVELLING BY REWRITING THE QUATERNION.
//
// The first attempt stripped pitch out of the aim quaternion inside the look-delta detour. It made
// things WORSE -- the reported aim pitch went from -66 degrees to exactly -80, which is the engine's
// own clamp limit. The engine keeps pitch as a SCALAR and rebuilds the quaternion from it, so
// rewriting the quaternion does not remove pitch; it just disagrees with the authority, once per
// call, until the disagreement saturates against the clamp.
//
// The engine's own look-delta entry point is the authority, so levelling now goes through that
// instead -- see ViewHook::on_frame().
constexpr const char* kPoseHookName = "PlayerCamera::UpdateViewPose";
constexpr const char* kSetRotHookName = "LTObject::SetRotation";
constexpr const char* kSetPosRotHookName = "LTObject::SetPosRot";

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
// 0 = the APPLIED pose at +244 (derived), 1 = the VIEW rotation at +324, 2 = RENDER ONLY.
//
// Mode 2 is the one a VR mod wants, and a player's report is what identified it. With the camera frozen through
// SetPosRot AND the aim fields overridden, the view was perfectly still and BULLETS DID NOT DEVIATE -- but the
// viewmodel jittered, because +244 was only partially won (1.14 degrees of residual) and the weapon follows it.
//
// Leaving the aim fields alone entirely separates the three systems:
//     camera / view  -> the camera object, written once a frame by SetPosRot   OWNED
//     bullets / aim  -> follow the view                                        follow the override
//     viewmodel      -> the aim rotation at +244                               left to the player
// which is head-tracking with the weapon still on the controller, i.e. exactly the decoupling that was listed
// as the next open question.
std::atomic<uint32_t> g_ov_target{0};
// The rotation the engine held when the override was armed. The offset is applied to THIS, never to the live
// value, or a persisting write feeds its own output back and accumulates.
std::atomic<uint32_t> g_ov_base[4]{};
std::atomic<bool> g_ov_have_base{false};
// The worst drift seen between our corrections, in degrees, and how many corrections found any.
std::atomic<uint32_t> g_ov_max_drift_bits{0};
std::atomic<uint64_t> g_ov_drift_frames{0};
// Frames where we replaced the quaternion IN FLIGHT rather than writing the field after the fact.
std::atomic<uint64_t> g_ov_inflight{0};
// Worst deviation of each DERIVED stage from what the override intends -- where the render path re-enters.
std::atomic<uint32_t> g_ov_applied_drift_bits{0};
std::atomic<uint32_t> g_ov_object_drift_bits{0};
// Times the applied pose was replaced right after ApplyLookDelta wrote it -- the render chain's entry point.
std::atomic<uint64_t> g_ov_applied_writes{0};
// The engine's own setter, and the only write path that reaches the renderer.
std::atomic<uint64_t> g_setrot_calls{0};
std::atomic<uint64_t> g_setrot_camera{0};    // calls whose object IS the player's camera
std::atomic<uint64_t> g_setrot_overridden{0};
std::atomic<bool> g_setrot_installed{false};
std::atomic<uintptr_t> g_setrot_target{0};
// The move-and-turn path, which is where a camera actually goes.
std::atomic<uint64_t> g_spr_calls{0};
std::atomic<uint64_t> g_spr_camera{0};
std::atomic<uint64_t> g_spr_overridden{0};
std::atomic<bool> g_spr_installed{false};
// The player BODY's rotation against its own baseline: large while the camera is locked means the two are
// genuinely decoupled.
std::atomic<uint32_t> g_ov_body_drift_bits{0};
std::atomic<uint32_t> g_ov_body_base[4]{};
std::atomic<bool> g_ov_body_base_set{false};

// ---- QUIESCENCE, TRACKED ON THE ENGINE THREAD -------------------------------------------------------
//
// A dozen suite checks silently required a settled world and passed for many sessions only because nobody was
// playing. The fix is NOT to suppress input -- TESTING.MD forbids narrowing the input so a failing path is
// never exercised, it would make the suite test a state that never occurs, and input is only one source of
// motion anyway (animation, physics settling and the clamp timer keep running).
//
// So quiescence is MEASURED and REPORTED. Counting consecutive frames in which nothing moved has to happen
// where the frames are: an IPC sampler cannot tell "still" from "sampled twice within one frame".
//
// "Nothing moved" means the camera rotation, the body rotation and the body position are all bit-identical to
// last frame and no look input arrived. Bit equality rather than an epsilon, deliberately -- a threshold here
// would be a tolerance invented to make a gate feel better, and the whole point is to know precisely when a
// strong-form comparison is safe.
std::atomic<uint64_t> g_still_frames{0};
std::atomic<uint32_t> g_last_cam[4]{};
std::atomic<uint32_t> g_last_body[4]{};
std::atomic<uint32_t> g_last_pos[3]{};
std::atomic<uint64_t> g_last_look{0};
std::atomic<bool> g_still_primed{false};

// Returns true when every watched value is unchanged from the previous frame.
bool sample_stillness() {
    const auto p = sdk::PlayerMgr::player(0);
    if (!p.has_value() || p->camera_object == 0 || p->object == 0) {
        return false;
    }
    std::array<float, 4> cam{}, body{};
    std::array<float, 3> pos{};
    if (!sdk::mem::copy(cam.data(), p->camera_object + 0x20, sizeof(cam)) ||
        !sdk::mem::copy(body.data(), p->object + 0x20, sizeof(body)) ||
        !sdk::mem::copy(pos.data(), p->object + 0x14, sizeof(pos))) {
        return false;
    }
    const uint64_t look = g_calls.load(std::memory_order_relaxed);

    bool same = g_still_primed.load(std::memory_order_relaxed) &&
                look == g_last_look.load(std::memory_order_relaxed);
    for (size_t i = 0; same && i < cam.size(); ++i) {
        uint32_t a = 0;
        std::memcpy(&a, &cam[i], sizeof(a));
        same = a == g_last_cam[i].load(std::memory_order_relaxed);
    }
    for (size_t i = 0; same && i < body.size(); ++i) {
        uint32_t a = 0;
        std::memcpy(&a, &body[i], sizeof(a));
        same = a == g_last_body[i].load(std::memory_order_relaxed);
    }
    for (size_t i = 0; same && i < pos.size(); ++i) {
        uint32_t a = 0;
        std::memcpy(&a, &pos[i], sizeof(a));
        same = a == g_last_pos[i].load(std::memory_order_relaxed);
    }

    for (size_t i = 0; i < cam.size(); ++i) {
        uint32_t a = 0;
        std::memcpy(&a, &cam[i], sizeof(a));
        g_last_cam[i].store(a, std::memory_order_relaxed);
    }
    for (size_t i = 0; i < body.size(); ++i) {
        uint32_t a = 0;
        std::memcpy(&a, &body[i], sizeof(a));
        g_last_body[i].store(a, std::memory_order_relaxed);
    }
    for (size_t i = 0; i < pos.size(); ++i) {
        uint32_t a = 0;
        std::memcpy(&a, &pos[i], sizeof(a));
        g_last_pos[i].store(a, std::memory_order_relaxed);
    }
    g_last_look.store(look, std::memory_order_relaxed);
    g_still_primed.store(true, std::memory_order_relaxed);
    return same;
}
// What we wrote last frame, and whether there is anything to compare yet.
std::atomic<uint32_t> g_ov_last[4]{};
std::atomic<bool> g_ov_pending{false};
std::atomic<uintptr_t> g_pose_last_this{0};
std::atomic<bool> g_pose_installed{false};
std::atomic<uintptr_t> g_pose_target{0};

// APPLY THE OVERRIDE, from whichever detour just let a writer run.
//
// One correction per frame is NOT a lock, and the live symptom named it exactly: the view rubber-banded back
// toward the held orientation instead of holding still. ApplyLookDelta fires ~740 times a second against
// UpdateViewPose's ~300, so the player's look landed two or three times between corrections, moved the view,
// and got yanked back on the next one.
//
// So every writer we own is followed by a re-assert. `decrement` is only true for the per-frame call, or the
// countdown would burn down at the rate of look input instead of the rate of frames.
// Worst angular distance between two quaternions, accumulated into a max. Sign-insensitive, since q and -q
// are the same rotation and a naive comparison would report 180 degrees for an identical pose.
void track_drift(std::atomic<uint32_t>& slot, const std::array<float, 4>& a, const std::array<float, 4>& b) {
    float dot = 0.0f;
    for (size_t i = 0; i < a.size(); ++i) {
        dot += a[i] * b[i];
    }
    dot = dot < 0.0f ? -dot : dot;
    if (dot > 1.0f) {
        dot = 1.0f;
    }
    const float deg = 2.0f * std::acos(dot) * 57.29577951f;
    const uint32_t prev = slot.load(std::memory_order_relaxed);
    float prev_f = 0.0f;
    std::memcpy(&prev_f, &prev, sizeof(prev_f));
    if (deg > prev_f) {
        uint32_t bits = 0;
        std::memcpy(&bits, &deg, sizeof(bits));
        slot.store(bits, std::memory_order_relaxed);
    }
}

// The absolute rotation the override wants this frame: yaw offset composed onto the captured baseline.
// False when no baseline has been captured yet.
bool override_rotation(std::array<float, 4>& out) {
    if (!g_ov_have_base.load(std::memory_order_relaxed)) {
        return false;
    }
    float yaw = 0.0f;
    const uint32_t yb = g_ov_yaw_bits.load(std::memory_order_relaxed);
    std::memcpy(&yaw, &yb, sizeof(yaw));
    std::array<float, 4> b{};
    for (size_t i = 0; i < b.size(); ++i) {
        const uint32_t bits = g_ov_base[i].load(std::memory_order_relaxed);
        std::memcpy(&b[i], &bits, sizeof(b[i]));
    }
    const float half = yaw * 0.5f * 0.01745329252f;
    const std::array<float, 4> a{0.0f, std::sin(half), 0.0f, std::cos(half)};
    out = {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
           a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
           a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
           a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
    return true;
}

void apply_override(bool decrement) {
    const uint32_t frames = g_ov_frames.load(std::memory_order_relaxed);
    if (frames == 0) {
        return;
    }
    if (decrement) {
        g_ov_frames.store(frames - 1, std::memory_order_relaxed);
    }
    const uint32_t mode = g_ov_target.load(std::memory_order_relaxed);
    // RENDER ONLY: the baseline still has to be captured, but no aim field is touched.
    const auto cur = mode == 1 ? sdk::PlayerMgr::view_rotation(0) : sdk::PlayerMgr::applied_rotation(0);
    if (!cur.has_value()) {
        return;
    }
    if (!g_ov_have_base.load(std::memory_order_relaxed)) {
        for (size_t i = 0; i < cur->size(); ++i) {
            uint32_t bits = 0;
            std::memcpy(&bits, &(*cur)[i], sizeof(bits));
            g_ov_base[i].store(bits, std::memory_order_relaxed);
        }
        g_ov_have_base.store(true, std::memory_order_relaxed);
    }
    // HOW FAR DID IT DRIFT BEFORE WE CORRECTED IT? This is the rubber-band amplitude, and it can only be
    // measured here: the drift happens and is corrected between frames, so HTTP sampling sees a still value
    // however hard the two writers are fighting. Compared against what we last wrote, as an angle.
    if (g_ov_pending.load(std::memory_order_relaxed)) {
        float dot = 0.0f;
        for (size_t i = 0; i < cur->size(); ++i) {
            float last = 0.0f;
            const uint32_t bits = g_ov_last[i].load(std::memory_order_relaxed);
            std::memcpy(&last, &bits, sizeof(last));
            dot += last * (*cur)[i];
        }
        // Quaternion angular distance; sign-insensitive because q and -q are the same rotation.
        dot = dot < 0.0f ? -dot : dot;
        if (dot > 1.0f) {
            dot = 1.0f;
        }
        const float deg = 2.0f * std::acos(dot) * 57.29577951f;
        uint32_t prev = g_ov_max_drift_bits.load(std::memory_order_relaxed);
        float prev_f = 0.0f;
        std::memcpy(&prev_f, &prev, sizeof(prev_f));
        if (deg > prev_f) {
            uint32_t bits = 0;
            std::memcpy(&bits, &deg, sizeof(bits));
            g_ov_max_drift_bits.store(bits, std::memory_order_relaxed);
        }
        if (deg > 0.01f) {
            g_ov_drift_frames.fetch_add(1, std::memory_order_relaxed);
        }
    }

    float yaw = 0.0f;
    const uint32_t yb = g_ov_yaw_bits.load(std::memory_order_relaxed);
    std::memcpy(&yaw, &yb, sizeof(yaw));
    std::array<float, 4> base{};
    for (size_t i = 0; i < base.size(); ++i) {
        const uint32_t bits = g_ov_base[i].load(std::memory_order_relaxed);
        std::memcpy(&base[i], &bits, sizeof(base[i]));
    }
    const float half = yaw * 0.5f * 0.01745329252f;
    const std::array<float, 4> a{0.0f, std::sin(half), 0.0f, std::cos(half)};
    const auto& b = base;
    const std::array<float, 4> out{
        a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
        a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
        a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
        a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
    if (mode == 2) {
        // Nothing to write here; the camera is owned in the SetPosRot detour and the aim is the player's.
        for (size_t i = 0; i < out.size(); ++i) {
            uint32_t bits = 0;
            std::memcpy(&bits, &out[i], sizeof(bits));
            g_ov_last[i].store(bits, std::memory_order_relaxed);
        }
        return;
    }
    const bool wrote = mode == 1 ? sdk::PlayerMgr::write_view_rotation(0, out)
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
    const int r = hook->original<int(__fastcall*)(uintptr_t, uintptr_t, float*, float, float*)>()(
        self, 0, a2, a3, a4);

    // OVERRIDE THE QUATERNION IN FLIGHT, which is why a2 matters.
    //
    // A data breakpoint on the live field found the writer: this function's CALLER copies the rotation to the
    // stack, passes a pointer to it here, and stores the result back into +144h..+150h itself --
    //
    //     fld [esi+144h] .. fld [esi+150h]      ; quaternion -> stack
    //     call CPlayerCamera_ApplyLookDelta     ; modifies the stack copy
    //     fld [esp+..] / fstp [esi+144h]        ; ... and writes it back
    //
    // So writing the FIELD from in here is pointless: the caller overwrites it microseconds later with the
    // stack value. That is the rubber-band, and it is why correcting after both hooks still left a 3.24 degree
    // worst-case excursion. Writing THROUGH a2 puts our rotation into the value the caller is about to store,
    // so the engine propagates it instead of fighting it.
    if (g_ov_frames.load(std::memory_order_relaxed) > 0 &&
        g_ov_target.load(std::memory_order_relaxed) != 2u) {
        std::array<float, 4> out{};
        if (override_rotation(out)) {
            if (a2 != nullptr) {
                std::memcpy(a2, out.data(), sizeof(out));
                g_ov_inflight.fetch_add(1, std::memory_order_relaxed);
            }
            // AND THE APPLIED POSE, which is what the RENDERER follows.
            //
            // Owning +324 completely was not enough: with it pinned to 0.00 degrees of drift, the applied pose
            // at +244 and the camera object BOTH sat 58.82 degrees away and the player still saw the view
            // rubber-band. The two derived stages agreed with each other and not with us, so the render chain
            // is +244 -> camera object -> renderer and it is NOT fed from +324.
            //
            // ApplyLookDelta writes +244 itself, from the delta-applied rotation, before returning. So the
            // moment to replace it is HERE -- immediately after its writer -- and not after UpdateViewPose,
            // where an earlier attempt was simply overwritten by this function on the next call.
            if (sdk::PlayerMgr::write_applied_rotation(0, out)) {
                g_ov_applied_writes.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    return r;
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
    // point -- writing the object directly is reclaimed within a frame. This call owns the frame countdown.
    apply_override(true);

    // SAME-PHASE AGREEMENT, sampled AFTER the original so the pose it just wrote and the camera object are in
    // one phase. This is a question the IPC thread cannot ask -- an out-of-band reader never lands in a known
    // phase because this function rewrites the pose every frame.
    //
    // RESTORED: a refactor that replaced this region with apply_override() deleted it, and the counters kept
    // being reported as zeros. The assertion "the in-detour sampler actually ran" is what caught that, which is
    // the whole reason an anti-vacuity check earns its place next to the measurement it guards.
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

    // QUIESCENCE, once per frame. Cheap: three guarded reads and a comparison.
    if (sample_stillness()) {
        g_still_frames.fetch_add(1, std::memory_order_relaxed);
    } else {
        g_still_frames.store(0, std::memory_order_relaxed);
    }

    // HOW FAR DOWNSTREAM DOES THE OVERRIDE ACTUALLY REACH?
    //
    // Zero drift at +324 only proves we win at the field we write. A player still saw the rendered view
    // rubber-band, which means whatever the renderer consumes is NOT that field -- so each derived stage is
    // measured against the rotation the override intends, at the end of the frame's view update.
    //
    //     +324  the field we write
    //     +244  the applied pose, derived
    //     camera object LTObject.rotation, derived again
    //
    // Whichever stage diverges is where input re-enters, and that is the one a VR override has to reach.
    if (g_ov_frames.load(std::memory_order_relaxed) > 0) {
        std::array<float, 4> want{};
        if (override_rotation(want)) {
            if (const auto ap = sdk::PlayerMgr::applied_rotation(0)) {
                track_drift(g_ov_applied_drift_bits, want, *ap);
            }
            if (const auto p = sdk::PlayerMgr::player(0); p.has_value()) {
                if (p->camera_object != 0) {
                    std::array<float, 4> obj{};
                    if (sdk::mem::copy(obj.data(), p->camera_object + 0x20, sizeof(obj))) {
                        track_drift(g_ov_object_drift_bits, want, obj);
                    }
                }
                // THE PLAYER'S BODY, which is the field I should have been measuring all along.
                //
                // Every instrument here was camera-side, so a render-only lock reported the aim pose frozen and
                // I concluded the body was frozen too. A player then rotated their character through a full 360
                // while the view stayed stuck. The aim pose at +244 is the CAMERA's pose -- of course it follows
                // the camera -- and the body has its own rotation on the player object.
                //
                // Measured against the body's OWN baseline, so a large number here while the camera reads zero
                // is decoupling working rather than an error.
                if (p->object != 0) {
                    std::array<float, 4> body{};
                    if (sdk::mem::copy(body.data(), p->object + 0x20, sizeof(body))) {
                        if (!g_ov_body_base_set.load(std::memory_order_relaxed)) {
                            for (size_t i = 0; i < body.size(); ++i) {
                                uint32_t bits = 0;
                                std::memcpy(&bits, &body[i], sizeof(bits));
                                g_ov_body_base[i].store(bits, std::memory_order_relaxed);
                            }
                            g_ov_body_base_set.store(true, std::memory_order_relaxed);
                        }
                        std::array<float, 4> bb{};
                        for (size_t i = 0; i < bb.size(); ++i) {
                            const uint32_t bits = g_ov_body_base[i].load(std::memory_order_relaxed);
                            std::memcpy(&bb[i], &bits, sizeof(bb[i]));
                        }
                        track_drift(g_ov_body_drift_bits, bb, body);
                    }
                }
            }
        }
    }

    return result;
}

// THE ENGINE'S ROTATION SETTER. __thiscall with ONE stack argument (the original's single exit is `retn 4`),
// so __fastcall(this, edx_dummy, quat) and the callee cleanup matches.
//
// This is where the rendered view is actually written. Overriding the camera holder's fields does not reach it:
// with the source field pinned to 0.00 degrees of drift, the camera object still sat 58.82 degrees from the
// override's intent, and forcing the applied pose too made it 109.47. The object is written HERE, and the
// quaternion arrives as an argument -- so it is replaced in flight, the same shape that fixed ApplyLookDelta.
//
// FILTERED ON THE CAMERA OBJECT. This setter moves every object in the world; touching anything but the
// player's camera would be a mod rewriting the game's physics by accident.
int __fastcall set_rotation_detour(uintptr_t self, uintptr_t /*edx*/, float* quat) {
    g_setrot_calls.fetch_add(1, std::memory_order_relaxed);

    if (quat != nullptr && g_ov_frames.load(std::memory_order_relaxed) > 0) {
        const auto p = sdk::PlayerMgr::player(0);
        if (p.has_value() && p->camera_object != 0 && self == p->camera_object) {
            g_setrot_camera.fetch_add(1, std::memory_order_relaxed);
            std::array<float, 4> out{};
            if (override_rotation(out)) {
                std::memcpy(quat, out.data(), sizeof(out));
                g_setrot_overridden.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    auto* hook = Hooks::get().find(kSetRotHookName);
    if (hook == nullptr) {
        return 0;
    }
    return hook->original<int(__fastcall*)(uintptr_t, uintptr_t, float*)>()(self, 0, quat);
}

// THE MOVE-AND-TURN PATH. Same ABI shape as the rotation setter -- __thiscall with one stack argument, single
// exit `retn 4` -- but the argument is SEVEN floats: position at [0..2] and the quaternion at [3..6]. Only the
// rotation is replaced; moving the camera's position would be a different feature and a worse bug.
int __fastcall set_pos_rot_detour(uintptr_t self, uintptr_t /*edx*/, float* posrot) {
    g_spr_calls.fetch_add(1, std::memory_order_relaxed);

    if (posrot != nullptr && g_ov_frames.load(std::memory_order_relaxed) > 0) {
        const auto p = sdk::PlayerMgr::player(0);
        if (p.has_value() && p->camera_object != 0 && self == p->camera_object) {
            g_spr_camera.fetch_add(1, std::memory_order_relaxed);
            std::array<float, 4> out{};
            if (override_rotation(out)) {
                std::memcpy(posrot + 3, out.data(), sizeof(out));
                g_spr_overridden.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    auto* hook = Hooks::get().find(kSetPosRotHookName);
    if (hook == nullptr) {
        return 0;
    }
    return hook->original<int(__fastcall*)(uintptr_t, uintptr_t, float*)>()(self, 0, posrot);
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

    // THE ENGINE'S SETTER, in FEAR2.exe rather than gameclient -- the write path that reaches the renderer.
    const auto sr_target = sdk::set_rotation_fn();
    g_setrot_target.store(sr_target, std::memory_order_relaxed);
    if (sr_target == 0) {
        LOGX("[viewhook] LTObject::SetRotation unresolved; render-path hook NOT installed");
        return std::string{"LTObject::SetRotation pattern did not resolve"};
    }
    if (!Hooks::get().install(kSetRotHookName, reinterpret_cast<void*>(sr_target),
                              reinterpret_cast<void*>(&set_rotation_detour))) {
        LOGX("[viewhook] LTObject::SetRotation install FAILED at 0x%08" PRIXPTR, sr_target);
        return std::string{"failed to install the LTObject::SetRotation hook"};
    }
    g_setrot_installed.store(true, std::memory_order_relaxed);
    LOGX("[viewhook] LTObject::SetRotation installed at 0x%08" PRIXPTR, sr_target);

    const auto spr_target = sdk::set_pos_rot_fn();
    if (spr_target == 0) {
        LOGX("[viewhook] LTObject::SetPosRot unresolved; move-and-turn hook NOT installed");
        return std::string{"LTObject::SetPosRot pattern did not resolve"};
    }
    if (!Hooks::get().install(kSetPosRotHookName, reinterpret_cast<void*>(spr_target),
                              reinterpret_cast<void*>(&set_pos_rot_detour))) {
        return std::string{"failed to install the LTObject::SetPosRot hook"};
    }
    g_spr_installed.store(true, std::memory_order_relaxed);
    LOGX("[viewhook] LTObject::SetPosRot installed at 0x%08" PRIXPTR, spr_target);
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
    const uint32_t db = g_ov_max_drift_bits.load(std::memory_order_relaxed);
    std::memcpy(&out.override_max_drift_deg, &db, sizeof(out.override_max_drift_deg));
    out.override_drift_frames = g_ov_drift_frames.load(std::memory_order_relaxed);
    out.override_inflight = g_ov_inflight.load(std::memory_order_relaxed);
    out.override_applied_writes = g_ov_applied_writes.load(std::memory_order_relaxed);
    out.setrot_calls = g_setrot_calls.load(std::memory_order_relaxed);
    out.setrot_camera = g_setrot_camera.load(std::memory_order_relaxed);
    out.setrot_overridden = g_setrot_overridden.load(std::memory_order_relaxed);
    out.setrot_installed = g_setrot_installed.load(std::memory_order_relaxed);
    out.setrot_target = g_setrot_target.load(std::memory_order_relaxed);
    out.spr_calls = g_spr_calls.load(std::memory_order_relaxed);
    out.spr_camera = g_spr_camera.load(std::memory_order_relaxed);
    out.spr_overridden = g_spr_overridden.load(std::memory_order_relaxed);
    out.spr_installed = g_spr_installed.load(std::memory_order_relaxed);
    out.still_frames = g_still_frames.load(std::memory_order_relaxed);
    const uint32_t bd = g_ov_body_drift_bits.load(std::memory_order_relaxed);
    std::memcpy(&out.override_body_drift_deg, &bd, sizeof(out.override_body_drift_deg));
    uint32_t ab = g_ov_applied_drift_bits.load(std::memory_order_relaxed);
    std::memcpy(&out.override_applied_drift_deg, &ab, sizeof(out.override_applied_drift_deg));
    uint32_t ob = g_ov_object_drift_bits.load(std::memory_order_relaxed);
    std::memcpy(&out.override_object_drift_deg, &ob, sizeof(out.override_object_drift_deg));
    return out;
}

void ViewHook::arm_override(float yaw_deg, uint32_t frames, unsigned mode) {
    g_ov_target.store(mode, std::memory_order_relaxed);
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
    g_ov_max_drift_bits.store(0, std::memory_order_relaxed);
    g_ov_drift_frames.store(0, std::memory_order_relaxed);
    g_ov_inflight.store(0, std::memory_order_relaxed);
    g_ov_applied_drift_bits.store(0, std::memory_order_relaxed);
    g_ov_object_drift_bits.store(0, std::memory_order_relaxed);
    g_ov_body_drift_bits.store(0, std::memory_order_relaxed);
    g_ov_body_base_set.store(false, std::memory_order_relaxed);
    g_ov_applied_writes.store(0, std::memory_order_relaxed);
    g_ov_pending.store(false, std::memory_order_relaxed);
    g_ov_frames.store(bounded, std::memory_order_relaxed);
    LOGX("[viewhook] override armed: %.2f deg yaw for %u frames", yaw_deg, bounded);
}

void ViewHook::set_level_aim(bool on) {
    g_level_aim.store(on, std::memory_order_relaxed);
    LOGX("[view] aim levelling %s -- the body owns yaw, the headset owns pitch", on ? "ON" : "off");
}

bool ViewHook::level_aim() const {
    return g_level_aim.load(std::memory_order_relaxed);
}

uint64_t ViewHook::level_aim_writes() const {
    return g_level_writes.load(std::memory_order_relaxed);
}

void ViewHook::on_frame() {
    if (!g_level_aim.load(std::memory_order_relaxed)) {
        return;
    }

    // NULL THE PITCH THROUGH THE ENGINE'S OWN PATH. apply_look_delta is the entry point the game
    // itself uses, so the pitch scalar, the quaternion it derives, and everything downstream all
    // move together -- which is exactly what rewriting the quaternion could not achieve.
    //
    // Once per frame, not once per call: the detour fires ~740 times a second and a correction that
    // often would be fighting the same input several times before it had been applied once.
    const auto pitch = sdk::PlayerMgr::aim_pitch(0);

    if (!pitch.has_value()) {
        return;
    }

    // A dead zone, because a delta of nearly nothing is still a write, and a write every frame
    // against an aim that is already level is pure noise in the engine's input path.
    constexpr float kLevelEpsilon = 0.0015f;  // ~0.09 degrees

    if (*pitch > -kLevelEpsilon && *pitch < kLevelEpsilon) {
        return;
    }

    // Positive is up in BOTH this reading and the delta, so nulling is simply the negation. No
    // clamp needed: driving toward zero can never leave a range that contains zero.
    if (sdk::PlayerMgr::apply_look_delta(0, -*pitch, 0.0f)) {
        g_level_writes.fetch_add(1, std::memory_order_relaxed);
    }
}
