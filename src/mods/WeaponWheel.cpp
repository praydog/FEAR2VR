#include "WeaponWheel.hpp"

#include "Log.hpp"
#include "SyntheticInput.hpp"
#include "sdk/DatabaseMgr.hpp"
#include "sdk/WeaponMgr.hpp"

namespace {

// A spin lock guarding two short strings. The alternative -- a mutex taken on the game thread every
// frame -- is exactly what RenderHook's callback contract forbids, and the critical sections here
// are a string copy.
class Guard {
public:
    explicit Guard(std::atomic<bool>& flag) : m_flag(flag) {
        bool expected = false;
        while (!m_flag.compare_exchange_weak(expected, true, std::memory_order_acquire)) {
            expected = false;
        }
    }
    ~Guard() { m_flag.store(false, std::memory_order_release); }

private:
    std::atomic<bool>& m_flag;
};

}  // namespace

WeaponWheel& WeaponWheel::get() {
    static WeaponWheel s_instance;
    return s_instance;
}

std::string WeaponWheel::requested() const {
    Guard g(m_lock);
    return m_requested;
}

std::string WeaponWheel::last_error() const {
    Guard g(m_lock);
    return m_error;
}

void WeaponWheel::set_error(std::string text) {
    Guard g(m_lock);
    m_error = std::move(text);
}

bool WeaponWheel::request(std::string_view name) {
    if (name.empty()) {
        set_error("no weapon named");
        return false;
    }

    // REFUSE UP FRONT what cannot be honoured, rather than arming and failing slowly. A wheel wants
    // "you are not carrying that" as an immediate answer, not after four presses and 240 frames.
    const auto key = sdk::WeaponMgr::key_for_weapon(name);

    if (!key.has_value()) {
        set_error("not in the player's loadout, or its slot has no bound key");
        m_state.store(static_cast<uint32_t>(State::Failed), std::memory_order_relaxed);
        return false;
    }

    {
        Guard g(m_lock);
        m_requested.assign(name);
        m_error.clear();
    }

    m_key.store(*key, std::memory_order_relaxed);
    m_presses.store(0, std::memory_order_relaxed);
    m_frames_taken.store(0, std::memory_order_relaxed);
    // Press on the very next frame rather than after a wait.
    m_wait.store(0, std::memory_order_relaxed);
    m_state.store(static_cast<uint32_t>(State::Working), std::memory_order_release);
    LOGX("[wheel] requested '%.*s' -> key 0x%02X", static_cast<int>(name.size()), name.data(), *key);
    return true;
}

bool WeaponWheel::request_slot(unsigned slot) {
    auto* rec = sdk::WeaponMgr::loadout_weapon(slot);

    if (rec == nullptr) {
        set_error("no weapon in that loadout slot");
        m_state.store(static_cast<uint32_t>(State::Failed), std::memory_order_relaxed);
        return false;
    }

    return request(sdk::DatabaseMgr::record_name(rec));
}

void WeaponWheel::cancel() {
    if (state() == State::Working) {
        set_error("cancelled");
        m_state.store(static_cast<uint32_t>(State::Idle), std::memory_order_release);
    }
}

void WeaponWheel::on_frame() {
    if (state() != State::Working) {
        return;
    }

    const uint32_t frames = m_frames_taken.fetch_add(1, std::memory_order_relaxed) + 1;

    // ARRIVED? Check before anything else, including before the switch-in-flight test: a request for
    // the weapon already in hand must succeed without pressing a key.
    const auto want = requested();

    if (!want.empty() && sdk::WeaponMgr::current_weapon_name() == want) {
        m_state.store(static_cast<uint32_t>(State::Succeeded), std::memory_order_release);
        LOGX("[wheel] '%s' in hand after %u press(es), %u frames", want.c_str(),
             m_presses.load(std::memory_order_relaxed), frames);
        return;
    }

    if (frames >= kMaxFrames) {
        set_error("the engine never handed over the weapon within the frame budget");
        m_state.store(static_cast<uint32_t>(State::Failed), std::memory_order_release);
        LOGX("[wheel] '%s' FAILED after %u press(es), %u frames", want.c_str(),
             m_presses.load(std::memory_order_relaxed), frames);
        return;
    }

    // WAIT OUT A SWITCH RATHER THAN PRESSING INTO IT. During one the player holds nothing, and a
    // second press would be delivered to an engine already busy -- which is how a wheel ends up
    // cycling past the weapon the user asked for.
    if (sdk::WeaponMgr::switching()) {
        return;
    }

    if (m_wait.load(std::memory_order_relaxed) > 0) {
        m_wait.fetch_sub(1, std::memory_order_relaxed);
        return;
    }

    const uint32_t pressed = m_presses.load(std::memory_order_relaxed);

    if (pressed >= kMaxPresses) {
        set_error("the engine refused the slot key every time it was pressed");
        m_state.store(static_cast<uint32_t>(State::Failed), std::memory_order_release);
        return;
    }

    // Through SyntheticInput, which owns the multi-frame press and the game-thread affinity.
    if (SyntheticInput::get().tap(m_key.load(std::memory_order_relaxed))) {
        m_presses.store(pressed + 1, std::memory_order_relaxed);
        m_wait.store(kFramesBetweenPresses, std::memory_order_relaxed);
    }
}

void WeaponWheel::on_shutdown() {
    // Nothing registered and nothing held: the only state is our own, and a press already delivered
    // belongs to the engine. Stopping the retries is the whole of the teardown.
    m_state.store(static_cast<uint32_t>(State::Idle), std::memory_order_release);
}
