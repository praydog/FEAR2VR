#include "MenuInput.hpp"

#include <cstring>

#include "Log.hpp"
#include "RenderHook.hpp"
#include "sdk/Events.hpp"

MenuInput& MenuInput::get() {
    static MenuInput instance;
    return instance;
}

namespace {
void frame_cb() {
    MenuInput::get().service();
}
}  // namespace

std::optional<std::string> MenuInput::on_initialize() {
    // THE FRAME BOUNDARY, NOT on_frame. `Mods::on_frame` is fanned out from CClientShell::Update,
    // which does not tick at the MAIN MENU -- measured: the VR frame counter sits at +0 there while
    // the renderer is still presenting. Since the front end is the one place this mod exists to
    // drive, draining from on_frame would have worked everywhere except where it is needed.
    if (!RenderHook::get().add_present_callback(&frame_cb)) {
        return std::string{"could not attach to the frame boundary -- menu keys would never be sent"};
    }
    return std::nullopt;
}

uint32_t MenuInput::key_for(std::string_view name) {
    if (name == "up") {
        return kUp;
    }
    if (name == "down") {
        return kDown;
    }
    if (name == "left") {
        return kLeft;
    }
    if (name == "right") {
        return kRight;
    }
    if (name == "enter" || name == "select" || name == "accept") {
        return kEnter;
    }
    if (name == "escape" || name == "back" || name == "cancel") {
        return kEscape;
    }
    return 0;
}

bool MenuInput::tap(uint32_t key_code) {
    if (key_code == 0) {
        return false;
    }
    for (auto& slot : m_queue) {
        uint32_t expected = 0;
        if (slot.key.compare_exchange_strong(expected, key_code, std::memory_order_acq_rel)) {
            slot.phase.store(0, std::memory_order_release);
            return true;
        }
    }
    m_refused.fetch_add(1, std::memory_order_relaxed);
    return false;
}

void MenuInput::service() {
    const auto movie = sdk::Events::active_movie();
    m_movie_ok.store(movie.has_value(), std::memory_order_relaxed);
    if (!movie.has_value()) {
        return;
    }

    // ONE EVENT PER SLOT PER FRAME. The movie samples its key queue once per Advance, so a down and
    // an up issued in the same frame are seen as a single state with no transition between them --
    // which a menu reads as "nothing happened".
    for (auto& slot : m_queue) {
        const uint32_t key = slot.key.load(std::memory_order_acquire);
        if (key == 0) {
            continue;
        }
        const uint32_t phase = slot.phase.load(std::memory_order_relaxed);
        if (!sdk::Events::send_key(*movie, key, phase == 0)) {
            slot.key.store(0, std::memory_order_release);  // unsendable: drop rather than jam
            m_refused.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (phase == 0) {
            slot.phase.store(1, std::memory_order_release);
        } else {
            slot.key.store(0, std::memory_order_release);
            m_sent.fetch_add(1, std::memory_order_relaxed);
        }
        // Only one slot advances per frame, so two queued keys arrive as two distinct presses
        // rather than as one indistinguishable blur.
        break;
    }
}
