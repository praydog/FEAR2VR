#include "TurnController.hpp"

#include <atomic>
#include <cmath>

#include "sdk/PlayerMgr.hpp"

#include "SyntheticInput.hpp"

namespace {

constexpr float kPi = 3.14159265358979f;

// Convergence tolerance, in radians. 0.5 degrees: below the angular resolution a player can
// perceive on a snap turn, and comfortably above the jitter the aim shows at rest.
constexpr float kTolerance = 0.5f * kPi / 180.0f;

// Measured gain: ~0.144 degrees of yaw per unit of dx, at the sensitivity this session ran with.
// It is a STARTING ESTIMATE, not a constant -- the loop measures the result and re-corrects, which
// is the whole reason this class exists. Written as radians per unit so the arithmetic below has no
// stray conversions.
constexpr float kRadiansPerUnit = 0.144f * kPi / 180.0f;

// Deliberate under-correction. Applying the full estimate makes the loop oscillate when the real
// gain is higher than the estimate; 70% converges from either side.
constexpr float kDamping = 0.7f;

// A single delta is clamped: the engine's own curve compresses large ones, so a huge dx buys less
// than it looks like it should and costs the loop an iteration to discover.
constexpr int32_t kMaxStep = 400;

// Enough iterations to converge from a half-turn at the damping above, and few enough that a
// mis-estimated gain gives up instead of driving the player round forever.
constexpr uint32_t kMaxCorrections = 24;

std::atomic<bool> g_active{false};
std::atomic<float> g_target{0.0f};
std::atomic<float> g_error{0.0f};
std::atomic<uint32_t> g_corrections{0};
std::atomic<uint64_t> g_completed{0};
std::atomic<uint64_t> g_abandoned{0};
std::atomic<bool> g_converged{false};
// Consecutive in-tolerance observations before declaring the turn done. TWO, not one, and the
// reason is measurable: a correction queued on frame N is delivered by the input poll and only
// shows up in the heading on N+1, so a loop that stops the instant it first reads a small error
// stops with one delta still in flight. Measured that way, four turns each finished ~2.2 degrees
// past their target while the loop reported an error of 0.2.
constexpr int kSettledObservations = 2;
std::atomic<int> g_settled{0};

// Frames to wait after issuing a correction, so the next evaluation sees its effect.
constexpr int kSettleFrames = 3;
std::atomic<int> g_cooldown{0};

float wrap(float a) {
    while (a > kPi) {
        a -= 2.0f * kPi;
    }
    while (a < -kPi) {
        a += 2.0f * kPi;
    }
    return a;
}

} // namespace

void TurnController::on_frame() {
    if (!g_active.load(std::memory_order_relaxed)) {
        return;
    }
    // LET THE LAST CORRECTION LAND BEFORE JUDGING IT. A delta queued on frame N is delivered by
    // the input poll and only shows in the heading on N+1 or later, so re-evaluating immediately
    // corrects against a STALE error. Measured without this: every turn hit the 24-iteration cap
    // with the residual oscillating between -1.4 and +2.0 degrees.
    if (g_cooldown.load(std::memory_order_relaxed) > 0) {
        g_cooldown.fetch_sub(1, std::memory_order_relaxed);
        return;
    }
    // DO NOT DRIVE A CORPSE. When the player dies the aim stops responding while input keeps being
    // accepted, so the loop burns its whole iteration budget against a heading that cannot change --
    // measured doing exactly that: 25 corrections with the yaw frozen to the decimal, after the
    // player was killed mid-session. Any closed loop against game state needs a liveness gate, or it
    // mistakes "cannot move" for "has not moved yet".
    const auto stats = sdk::PlayerMgr::player_stats(0);
    if (stats.has_value() && !stats->alive()) {
        g_active.store(false, std::memory_order_relaxed);
        g_abandoned.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const auto yaw = sdk::PlayerMgr::aim_yaw(0);
    if (!yaw.has_value()) {
        // No player this frame -- a level change or a death. Abandon rather than keep issuing
        // input at whatever comes back.
        g_active.store(false, std::memory_order_relaxed);
        g_abandoned.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const float err = wrap(*yaw - g_target.load(std::memory_order_relaxed));
    g_error.store(err, std::memory_order_relaxed);

    if (std::fabs(err) < kTolerance) {
        // In tolerance -- but only finished once it STAYS there with nothing in flight.
        if (g_settled.fetch_add(1, std::memory_order_relaxed) + 1 >= kSettledObservations) {
            g_active.store(false, std::memory_order_relaxed);
            g_converged.store(true, std::memory_order_relaxed);
            g_completed.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }
    g_settled.store(0, std::memory_order_relaxed);
    const uint32_t n = g_corrections.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n > kMaxCorrections) {
        g_active.store(false, std::memory_order_relaxed);
        g_converged.store(false, std::memory_order_relaxed);
        g_abandoned.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    float step = -err / kRadiansPerUnit * kDamping;
    if (step > static_cast<float>(kMaxStep)) {
        step = static_cast<float>(kMaxStep);
    } else if (step < -static_cast<float>(kMaxStep)) {
        step = -static_cast<float>(kMaxStep);
    }
    auto dx = static_cast<int32_t>(step);
    if (dx == 0) {
        // Never stall: a sub-unit correction still has a direction, and rounding it to zero would
        // spin the loop until the cap with the error unchanged.
        dx = err > 0.0f ? -1 : 1;
    }
    SyntheticInput::get().queue_look(dx, 0);
    g_cooldown.store(kSettleFrames, std::memory_order_relaxed);
}

void TurnController::on_shutdown() {
    g_active.store(false, std::memory_order_relaxed);
}

void TurnController::turn_to(float yaw_radians) {
    g_target.store(wrap(yaw_radians), std::memory_order_relaxed);
    g_corrections.store(0, std::memory_order_relaxed);
    g_settled.store(0, std::memory_order_relaxed);
    g_cooldown.store(0, std::memory_order_relaxed);
    g_converged.store(false, std::memory_order_relaxed);
    g_active.store(true, std::memory_order_relaxed);
}

bool TurnController::turn_by(float delta_radians) {
    const auto yaw = sdk::PlayerMgr::aim_yaw(0);
    if (!yaw.has_value()) {
        return false;
    }
    turn_to(*yaw + delta_radians);
    return true;
}

bool TurnController::recentre() {
    const auto d = sdk::PlayerMgr::aim_vs_view(0);
    if (!d.has_value()) {
        return false;
    }
    // The heading the VIEW is facing. Turning the aim to it makes the body face where the head is
    // looking, which is what puts the composed head pose back at straight-ahead.
    turn_to(std::atan2(d->view_forward[0], d->view_forward[2]));
    return true;
}

void TurnController::cancel() {
    g_active.store(false, std::memory_order_relaxed);
}

TurnController::Observed TurnController::observed() const {
    Observed out{};
    out.active = g_active.load(std::memory_order_relaxed);
    out.target = g_target.load(std::memory_order_relaxed);
    out.error = g_error.load(std::memory_order_relaxed);
    out.corrections = g_corrections.load(std::memory_order_relaxed);
    out.completed = g_completed.load(std::memory_order_relaxed);
    out.abandoned = g_abandoned.load(std::memory_order_relaxed);
    out.converged = g_converged.load(std::memory_order_relaxed);
    return out;
}
