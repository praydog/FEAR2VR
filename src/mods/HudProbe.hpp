#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "Mod.hpp"

// ---- READING THE RECT THE HUD IS ACTUALLY CLAMPED INTO ----------------------------------------
//
// HUD_ClampElementPos (gameclient.dll 0x1008EF70) clamps every HUD element against one of two
// rects, chosen by `this[581]`:
//
//     mode 1  -> this[616..619]
//     mode 2  -> PlayerMgr_GetLocalPlayer(...) + 252, then [49..52]
//
// Live, it runs in MODE 2. An earlier probe logged the mode-1 rect regardless and reported
// (0,0)-(640,480) as though it were the ceiling; it was a field the engine was not consulting, and
// every conclusion from it was void. Acting on it also crashed the game, by calling
// PlayerMgr_GetLocalPlayer with a guessed calling convention.
//
// So this reads the mode-2 rect the only safe way: through the SDK's already-tested
// PlayerMgr::local_player(), whose Player::holder IS that `object + 252`. No engine call, no hook,
// no guessed prototype -- four field reads off a pointer the SDK already validates.
class HudProbe final : public Mod {
public:
    static HudProbe& get();

    std::string_view get_name() const override { return "HudProbe"; }
    void on_frame() override;

    struct State {
        bool have;
        uintptr_t holder;
        int32_t rect[4];
        uint32_t screen_w;
        uint32_t screen_h;
    };
    State state() const;

    // The engine's own opt-in UI scale override (flt_6E34F4). Negative restores stock behaviour.
    void set_scale(float v);
    float scale() const;

private:
    HudProbe() = default;
};
