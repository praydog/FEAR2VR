#include "WeaponAgreement.hpp"

#include <atomic>
#include <cmath>

#include "sdk/Model.hpp"
#include "sdk/Object.hpp"
#include "sdk/CClientShell.hpp"

#include "Log.hpp"

namespace {

std::atomic<bool> g_running{false};
std::atomic<uint64_t> g_frames{0};
std::atomic<bool> g_valid{false};
std::atomic<float> g_disagreement{-1.0f};
std::atomic<float> g_step{-1.0f};
std::atomic<float> g_worst{0.0f};
std::atomic<uint64_t> g_still{0};

// Previous frame's weapon position, for the travel measurement.
float g_prev[3]{};
bool g_have_prev = false;

// A frame is "still" when the weapon barely moved; only those frames can hold the composition to a tight
// bound, because only they have no motion to hide behind.
constexpr float kStillStep = 0.05f;

void measure() {
    if (!g_running.load(std::memory_order_relaxed)) {
        return;
    }
    const auto player = sdk::CClientShell::local_player(0);
    if (!player.has_value()) {
        g_valid.store(false, std::memory_order_relaxed);
        g_have_prev = false;
        return;
    }
    const auto sk = sdk::ModelSkeleton::from_object(player->object);
    if (!sk.has_value()) {
        g_valid.store(false, std::memory_order_relaxed);
        g_have_prev = false;
        return;
    }
    const auto hand = sk->socket_world_transform("RightHand");
    const auto flash = sdk::attached_socket(player->object, "flash");
    if (!hand.has_value() || !flash.has_value()) {
        g_valid.store(false, std::memory_order_relaxed);
        g_have_prev = false;
        return;
    }
    const auto wi = sdk::object_info(flash->object);
    if (!wi.has_value()) {
        g_valid.store(false, std::memory_order_relaxed);
        g_have_prev = false;
        return;
    }

    const float wx = wi->position.x, wy = wi->position.y, wz = wi->position.z;
    const float dx = wx - hand->position.x;
    const float dy = wy - hand->position.y;
    const float dz = wz - hand->position.z;
    const float dis = std::sqrt(dx * dx + dy * dy + dz * dz);

    float step = -1.0f;
    if (g_have_prev) {
        const float sx = wx - g_prev[0], sy = wy - g_prev[1], sz = wz - g_prev[2];
        step = std::sqrt(sx * sx + sy * sy + sz * sz);
    }
    g_prev[0] = wx; g_prev[1] = wy; g_prev[2] = wz;
    g_have_prev = true;

    g_disagreement.store(dis, std::memory_order_relaxed);
    g_step.store(step, std::memory_order_relaxed);
    g_valid.store(true, std::memory_order_relaxed);
    g_frames.fetch_add(1, std::memory_order_relaxed);

    // The bound that means something: what the disagreement is when the weapon is NOT moving.
    if (step >= 0.0f && step <= kStillStep) {
        g_still.fetch_add(1, std::memory_order_relaxed);
        float w = g_worst.load(std::memory_order_relaxed);
        while (dis > w && !g_worst.compare_exchange_weak(w, dis, std::memory_order_relaxed)) {
        }
    }
}

} // namespace

std::optional<std::string> WeaponAgreement::on_initialize() {
    g_running.store(true, std::memory_order_relaxed);
    return std::nullopt;
}

void WeaponAgreement::on_frame() {
    measure();
}

void WeaponAgreement::on_shutdown() {
    g_running.store(false, std::memory_order_relaxed);
}

WeaponAgreement::Observed WeaponAgreement::observed() const {
    Observed out{};
    out.running = g_running.load(std::memory_order_relaxed);
    out.frames = g_frames.load(std::memory_order_relaxed);
    out.valid = g_valid.load(std::memory_order_relaxed);
    out.disagreement = g_disagreement.load(std::memory_order_relaxed);
    out.step = g_step.load(std::memory_order_relaxed);
    out.worst = g_worst.load(std::memory_order_relaxed);
    out.still_frames = g_still.load(std::memory_order_relaxed);
    return out;
}
