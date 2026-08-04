#include "HudProbe.hpp"

#include <windows.h>

#include <atomic>

#include <d3d9.h>

#include "Log.hpp"
#include "sdk/PlayerMgr.hpp"
#include "sdk/Render.hpp"

namespace {

// Player::holder is the engine's `object + 252` -- the same pointer HUD_ClampElementPos walks to in
// its mode-2 branch. The rect sits at DWORD indices 49..52 of it.
constexpr size_t kRectByteOffset = 49 * sizeof(int32_t);

std::atomic<bool> g_have{false};
std::atomic<uintptr_t> g_holder{0};
std::atomic<int32_t> g_rect[4]{};
std::atomic<uint32_t> g_screen_w{0};
std::atomic<uint32_t> g_screen_h{0};
std::atomic<uint32_t> g_tick{0};

// SEH-guarded because the holder is engine memory that can be torn down between the SDK handing it
// over and this read -- AGENTS.md rule on caller-provided dereferences. POD-only scope, no locals
// with destructors, or MSVC rejects the __try.
bool read_rect(uintptr_t holder, int32_t out[4]) {
    __try {
        const auto* r = reinterpret_cast<const int32_t*>(holder + kRectByteOffset);
        out[0] = r[0];
        out[1] = r[1];
        out[2] = r[2];
        out[3] = r[3];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

}  // namespace

HudProbe& HudProbe::get() {
    static HudProbe s_instance;
    return s_instance;
}

void HudProbe::on_frame() {
    // Once a second at 60fps. This is a measurement, not a feature, and the rect only changes when
    // the engine relays out the interface.
    if ((g_tick.fetch_add(1, std::memory_order_relaxed) % 60u) != 0) {
        return;
    }
    const auto player = sdk::PlayerMgr::local_player();
    if (!player.has_value() || player->holder == 0) {
        return;
    }
    int32_t rect[4]{};
    if (!read_rect(player->holder, rect)) {
        return;
    }

    uint32_t sw = 0;
    uint32_t sh = 0;
    if (const auto pp = sdk::Render::present_params()) {
        sw = pp->BackBufferWidth;
        sh = pp->BackBufferHeight;
    }

    const bool changed = !g_have.load(std::memory_order_acquire) ||
                         g_rect[0].load(std::memory_order_relaxed) != rect[0] ||
                         g_rect[1].load(std::memory_order_relaxed) != rect[1] ||
                         g_rect[2].load(std::memory_order_relaxed) != rect[2] ||
                         g_rect[3].load(std::memory_order_relaxed) != rect[3];

    for (int i = 0; i < 4; ++i) {
        g_rect[i].store(rect[i], std::memory_order_relaxed);
    }
    g_holder.store(player->holder, std::memory_order_relaxed);
    g_screen_w.store(sw, std::memory_order_relaxed);
    g_screen_h.store(sh, std::memory_order_relaxed);
    g_have.store(true, std::memory_order_release);

    if (changed) {
        LOGX("[hudprobe] mode-2 rect (%d,%d)-(%d,%d) = %dx%d, back buffer %ux%u, holder 0x%08X",
             rect[0], rect[1], rect[2], rect[3], rect[2] - rect[0], rect[3] - rect[1], sw, sh,
             static_cast<unsigned>(player->holder));
    }
}

HudProbe::State HudProbe::state() const {
    State s{};
    s.have = g_have.load(std::memory_order_acquire);
    s.holder = g_holder.load(std::memory_order_relaxed);
    for (int i = 0; i < 4; ++i) {
        s.rect[i] = g_rect[i].load(std::memory_order_relaxed);
    }
    s.screen_w = g_screen_w.load(std::memory_order_relaxed);
    s.screen_h = g_screen_h.load(std::memory_order_relaxed);
    return s;
}
