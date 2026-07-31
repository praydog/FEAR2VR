#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

#include "../Mod.hpp"

// ---- SELECTING A WEAPON BY NAME --------------------------------------------------------------
//
// What a VR weapon wheel needs: the player points at "Shotgun" and gets a shotgun. Everything
// underneath is already mapped -- sdk::WeaponMgr::key_for_weapon() names the key, sdk::Input can
// press it -- but a caller wiring those together directly gets three things wrong, and this class
// exists to own all three rather than leave them to be rediscovered:
//
//   1. A KEY PRESS SPANS FRAMES. sdk::Input::send_key does one edge and must run on the game
//      thread. Anything that pressed and then immediately read the result would be asking before
//      the engine had a chance to answer.
//
//   2. THE SWITCH ITSELF TAKES ~0.5 SECONDS, and during it the player holds NOTHING --
//      CClientWeaponMgr_ChangeWeapon defers the install until the deselect animation finishes
//      (WeaponChooser_OnDeselectComplete clears the slot index and the object). A consumer that
//      polled once, saw no weapon, and concluded failure would be reading a normal intermediate
//      state. sdk::WeaponMgr::switching() is that state and this waits for it to clear.
//
//   3. THE PRESS CAN BE REFUSED. The engine ignores a slot key while reloading, mid-switch, or
//      when the weapon is not carried, and it says nothing about it. So this RETRIES, bounded, and
//      reports a definite failure rather than hanging or lying.
//
// The loop lives here rather than in a caller for the reason TESTING.MD gives: a control loop with
// a tolerance and a retry budget is a behaviour, and behaviours belong to a class a consumer can
// call. The fixture drives this exactly the way a VR mod would.
class WeaponWheel final : public Mod {
public:
    static WeaponWheel& get();

    std::string_view get_name() const override { return "WeaponWheel"; }
    void on_frame() override;
    void on_shutdown() override;

    enum class State : uint32_t {
        Idle = 0,      // nothing requested since the last reset
        Working = 1,   // pressing and waiting for the engine
        Succeeded = 2, // the requested weapon is in hand
        Failed = 3,    // the budget ran out, or it was never selectable
    };

    // Ask for a weapon BY NAME, as it appears in the database ("Assault Rifle", "Submachinegun").
    //
    // Returns false IMMEDIATELY, without arming, when the request cannot be honoured: the name is
    // not in the player's loadout, or its slot has no bound key. That distinction matters to a
    // wheel -- "you are not carrying that" is a different message from "it did not work".
    //
    // Requesting the weapon already in hand succeeds without pressing anything.
    bool request(std::string_view name);

    // The same, addressed by loadout slot (1-based, matching the number keys).
    bool request_slot(unsigned slot);

    // Abandon an outstanding request. Nothing to undo -- a press already delivered is the engine's
    // now -- so this only stops the retries.
    void cancel();

    State state() const { return static_cast<State>(m_state.load(std::memory_order_relaxed)); }
    bool busy() const { return state() == State::Working; }
    std::string requested() const;

    // How many key presses the last request needed. A wheel can surface a rising number as "the
    // engine keeps refusing", and it is the honest measure of how reliable the route is.
    uint32_t presses() const { return m_presses.load(std::memory_order_relaxed); }

    // Frames spent on the last request, and why it ended. Empty while idle or working.
    uint32_t frames_taken() const { return m_frames_taken.load(std::memory_order_relaxed); }
    std::string last_error() const;

    // Budget. Deliberately generous against a measured ~0.5 s switch at this game's frame rate,
    // and bounded so a refused request always terminates.
    static constexpr uint32_t kMaxPresses = 4;
    static constexpr uint32_t kFramesBetweenPresses = 45;
    static constexpr uint32_t kMaxFrames = 240;

private:
    WeaponWheel() = default;

    std::atomic<uint32_t> m_state{static_cast<uint32_t>(State::Idle)};
    std::atomic<uint32_t> m_presses{0};
    std::atomic<uint32_t> m_frames_taken{0};
    std::atomic<uint32_t> m_wait{0};
    std::atomic<uint8_t> m_key{0};

    // Written by the IPC thread under m_lock, read on the game thread. Short and uncontended.
    mutable std::atomic<bool> m_lock{false};
    std::string m_requested;
    std::string m_error;

    void set_error(std::string text);
};
