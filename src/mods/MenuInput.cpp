#include "MenuInput.hpp"

#include <cstring>

#include "Log.hpp"
#include "RenderHook.hpp"
#include "sdk/Engine.hpp"
#include "sdk/Events.hpp"
#include "FramePublisher.hpp"
#include "vr/runtimes/SimulatedRuntime.hpp"
#include "xr/SharedFrame.hpp"

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

namespace {

// One axis of a stick, reduced to a direction with hysteresis. Returns -1, 0 or +1.
int32_t stick_dir(float v, int32_t current, float on, float off) {
    if (current != 0 && (v > -off && v < off)) {
        return 0;  // returned to centre -- re-armed
    }
    if (v >= on) {
        return 1;
    }
    if (v <= -on) {
        return -1;
    }
    return current;
}

}  // namespace

void MenuInput::poll_controller() {
    if (!m_controller.load(std::memory_order_acquire)) {
        return;
    }
    // ---- ONLY WHILE A MENU IS UP, WHICH IS TWO SITUATIONS ------------------------------------
    //
    // The main menu reports UiMode::Menu. The PAUSE menu does not -- it reports UiMode::InGame,
    // because it is drawn through the in-game UI system, so gating on Menu alone left the pause
    // screen exactly as unusable as the front end had been.
    //
    // The second half is the engine's own clock: paused means a menu has stopped the world, and
    // that is a state the engine maintains rather than something inferred from the UI. In play the
    // clock advances and these sticks stay locomotion, which is the behaviour that must not break.
    const auto mode = sdk::Events::ui_mode();
    bool menu_up = mode == sdk::Events::UiMode::Menu;
    if (!menu_up && mode == sdk::Events::UiMode::InGame) {
        if (const auto clock = sdk::Engine::clock_state()) {
            menu_up = clock->paused;
        }
    }
    m_menu_up.store(menu_up, std::memory_order_relaxed);
    if (!menu_up) {
        m_stick_dir = 0;
        m_stick_dir_x = 0;
        m_repeat_countdown = 0;
        return;
    }

    // ---- STRAIGHT FROM THE HOST, NOT FROM THE SIMULATED RUNTIME -----------------------------
    //
    // VR ingests the controller block in its per-frame update, and that update is fanned out from
    // CClientShell::Update -- which does not tick at the MAIN MENU. So at the one screen this mod
    // exists for, the simulated runtime holds whatever the controllers were doing when the world
    // last ran, which is nothing. Reading the shared block here bypasses the dead path entirely.
    //
    // Same seqlock discipline VR uses: odd means mid-write, and the sequence is re-read after the
    // copy so a block that moved underneath us is discarded rather than believed.
    const auto* hands = FramePublisher::get().hands_state();
    if (hands == nullptr) {
        return;
    }
    const uint32_t seq = hands->sequence;
    if ((seq & 1u) != 0u) {
        return;
    }
    const xr::HandsState snap = *hands;
    if (hands->sequence != seq) {
        return;
    }
    const auto& left = snap.hand[xr::kHandLeft];
    const auto& right = snap.hand[xr::kHandRight];
    m_hands_ok.store(true, std::memory_order_relaxed);
    m_left_active.store(left.active != 0u, std::memory_order_relaxed);
    m_stick_x.store(left.stick[0], std::memory_order_relaxed);
    m_stick_y.store(left.stick[1], std::memory_order_relaxed);

    // ---- THE STICK, VERTICALLY AND HORIZONTALLY -------------------------------------------
    //
    // OpenXR's stick y is +up, and a menu's "next item" is DOWN, so the vertical axis is negated
    // once here rather than at each use.
    const int32_t was_y = m_stick_dir;
    const int32_t was_x = m_stick_dir_x;
    if (left.active != 0u) {
        m_stick_dir = stick_dir(-left.stick[1], m_stick_dir, kStickOn, kStickOff);
        m_stick_dir_x = stick_dir(left.stick[0], m_stick_dir_x, kStickOn, kStickOff);
    } else {
        m_stick_dir = 0;
        m_stick_dir_x = 0;
    }

    const bool y_new = m_stick_dir != 0 && m_stick_dir != was_y;
    const bool x_new = m_stick_dir_x != 0 && m_stick_dir_x != was_x;
    if (m_stick_dir == 0 && m_stick_dir_x == 0) {
        m_repeat_countdown = 0;
    } else if (y_new || x_new) {
        m_repeat_countdown = kRepeatDelay;  // fire now, then wait before repeating
    } else if (m_repeat_countdown > 0) {
        --m_repeat_countdown;
    }

    const bool fire = y_new || x_new ||
                      ((m_stick_dir != 0 || m_stick_dir_x != 0) && m_repeat_countdown == 0);
    if (fire) {
        if (m_stick_dir != 0) {
            tap(m_stick_dir > 0 ? kDown : kUp);
            m_controller_keys.fetch_add(1, std::memory_order_relaxed);
        } else if (m_stick_dir_x != 0) {
            tap(m_stick_dir_x > 0 ? kRight : kLeft);
            m_controller_keys.fetch_add(1, std::memory_order_relaxed);
        }
        if (!y_new && !x_new) {
            m_repeat_countdown = kRepeatEvery;
        }
    }

    // ---- ACCEPT AND BACK -------------------------------------------------------------------
    //
    // Rising edges on the RIGHT hand, which is where they sit in game too (A jumps, B reloads), so
    // the thumb does not have to learn a second place for "yes".
    if (right.active != 0u) {
        const uint32_t now = right.buttons;
        const uint32_t pressed = now & ~m_last_buttons;
        m_last_buttons = now;
        if ((pressed & xr::kHandButtonA) != 0u) {
            tap(kEnter);
            m_controller_keys.fetch_add(1, std::memory_order_relaxed);
        }
        if ((pressed & xr::kHandButtonB) != 0u) {
            tap(kEscape);
            m_controller_keys.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void MenuInput::service() {
    poll_controller();

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
