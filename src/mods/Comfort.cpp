#include "Comfort.hpp"

#include <atomic>
#include <mutex>
#include <vector>

#include "sdk/Engine.hpp"

#include "Log.hpp"

namespace {

// The variables this mod suppresses, and the value each takes while suppressed.
//
// SPEED SCALES RATHER THAN WAVE AMPLITUDES. The bob is described by ~60 HeadBob* variables (per-axis
// wave minima and maxima for both the camera and the weapon), and zeroing all of them would be a lot
// of writes to restore correctly. `HeadBobSpeedScale` gates the whole wave, and live it reads 1.0
// with every amplitude already 0.0 -- so one variable does what sixty would, and the restore has one
// value to put back per knob instead of sixty.
struct Knob {
    const char* name;
    float suppressed_value;
};

constexpr Knob kKnobs[] = {
    {"HeadBobSpeedScale", 0.0f},
    {"CameraSwayXSpeed", 0.0f},
    {"CameraSwayYSpeed", 0.0f},
    // Already a boolean in the engine's own terms, so this is the engine's switch, not ours.
    {"DisableCameraShake", 1.0f},
};

constexpr size_t kKnobCount = sizeof(kKnobs) / sizeof(kKnobs[0]);

std::mutex g_mux;
std::atomic<bool> g_suppressed{false};
std::atomic<uint32_t> g_found{0};
std::atomic<uint32_t> g_missing{0};
std::atomic<uint32_t> g_applied{0};
std::atomic<uint32_t> g_restored{0};

// Captured originals, parallel to kKnobs. Guarded by g_mux, which is fine: this is only touched by
// a consumer arming or releasing, never on a hot path.
bool g_captured = false;
float g_original[kKnobCount]{};
bool g_have_original[kKnobCount]{};

} // namespace

bool Comfort::set_suppressed(bool on) {
    std::lock_guard<std::mutex> lock(g_mux);

    if (on) {
        if (g_suppressed.load(std::memory_order_relaxed)) {
            return true;  // idempotent: do not re-capture over our own values
        }
        uint32_t found = 0, missing = 0, applied = 0;
        for (size_t i = 0; i < kKnobCount; ++i) {
            const auto cur = sdk::Engine::console_var(kKnobs[i].name);
            if (!cur.has_value()) {
                // Absent in this build. Reported, not fatal -- see the header.
                g_have_original[i] = false;
                ++missing;
                continue;
            }
            ++found;
            g_original[i] = cur->value;
            g_have_original[i] = true;
            if (sdk::Engine::write_console_var(kKnobs[i].name, kKnobs[i].suppressed_value)) {
                ++applied;
            }
        }
        if (found == 0) {
            // Nothing resolved at all -- the table itself is unavailable, which IS a failure.
            return false;
        }
        g_captured = true;
        g_found.store(found, std::memory_order_relaxed);
        g_missing.store(missing, std::memory_order_relaxed);
        g_applied.store(applied, std::memory_order_relaxed);
        g_suppressed.store(true, std::memory_order_relaxed);
        LOGX("[comfort] suppressed %u of %u view-motion variables (%u absent)", applied, found, missing);
        return true;
    }

    if (!g_suppressed.load(std::memory_order_relaxed)) {
        return true;
    }
    uint32_t restored = 0;
    for (size_t i = 0; i < kKnobCount; ++i) {
        if (!g_have_original[i]) {
            continue;
        }
        if (sdk::Engine::write_console_var(kKnobs[i].name, g_original[i])) {
            ++restored;
        }
    }
    g_restored.store(restored, std::memory_order_relaxed);
    g_suppressed.store(false, std::memory_order_relaxed);
    LOGX("[comfort] restored %u view-motion variables", restored);
    return true;
}

bool Comfort::suppressed() const {
    return g_suppressed.load(std::memory_order_relaxed);
}

void Comfort::on_shutdown() {
    // A CONSOLE VARIABLE OUTLIVES THIS DLL. Leaving the game with its head bob switched off and no
    // mod present is silent corruption of the player's settings -- the same class of defect as a
    // hidden model piece or a latched mouse button, both of which this project has shipped once.
    set_suppressed(false);
}

Comfort::Observed Comfort::observed() const {
    Observed out{};
    out.suppressed = g_suppressed.load(std::memory_order_relaxed);
    out.known = static_cast<uint32_t>(kKnobCount);
    out.found = g_found.load(std::memory_order_relaxed);
    out.missing = g_missing.load(std::memory_order_relaxed);
    out.applied = g_applied.load(std::memory_order_relaxed);
    out.restored = g_restored.load(std::memory_order_relaxed);
    // Read live rather than remembered: this is the number that says what the ENGINE currently
    // believes, which is the only thing a caller can act on.
    if (const auto v = sdk::Engine::console_var("HeadBobSpeedScale")) {
        out.bob_scale = v->value;
        out.bob_scale_readable = true;
    }
    return out;
}
