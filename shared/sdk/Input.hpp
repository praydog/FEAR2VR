#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

// FEAR 2's INPUT SUBSYSTEM, and the flag that decides whether the game simulates at all.
//
// WHAT THIS BUILD DOES NOT USE. The reference LithTech tree reads input through DirectInput 8 --
// input.cpp calls DirectInput8Create, CreateDevice, SetDataFormat, Acquire, GetDeviceState and
// GetDeviceData, behind an InputMgr struct of function pointers. FEAR 2 replaced that layer entirely:
//
//   * dinput8.dll is NOT LOADED in the live process, nor is xinput or any HID module.
//   * FEAR2.exe's user32 imports contain no RegisterRawInputDevices, no GetRawInputData, and -- the
//     surprise -- no GetCursorPos. They are static imports; there is no dynamic resolution table.
//   * LTClient_WndProc handles no mouse message at all. WM_KEYDOWN, WM_KEYUP, WM_CHAR, WM_PAINT,
//     WM_ERASEBKGND, WM_ACTIVATEAPP, WM_SYSCOMMAND and WM_WTSSESSION_CHANGE, and nothing else.
//
// So the mouse is not read by polling the cursor and not read from window messages by the engine's own
// window procedure. Its position is PUSHED IN: LTInputDevice_Mouse_SetPositionFromClient(dev, x, y)
// takes a CLIENT-space point, stores it at the device's +0x28/+0x2C, and converts it to screen space
// with ClientToScreen. The axis getter then returns that position MINUS THE WINDOW CENTRE, and
// LTInput_CenterCursor warps the cursor back to the centre with SetCursorPos. That is the classic
// centre-relative mouse-look loop, with the sampling step outside the engine's window procedure.
//
// THE TWO INPUT PATHS, selected by LTInput_IsBufferedKeyInputActive():
//
//   buffered OFF (the normal case) -- CClientMgr::Update calls g_pILTInput's slot 3, which polls the
//       device array. THE WINDOW-MESSAGE KEY QUEUE IS NOT DRAINED. Writing into it achieves nothing.
//   buffered ON -- the device poll is skipped and the queue IS drained: each queued virtual key is
//       passed to sub_4111C1 and both queues are then cleared.
//
// That distinction is the whole reason key_queue_is_drained() exists. A mod that injects synthetic keys
// into the queue while buffered input is off will see them accumulate and never be consumed, and the
// only way to know is to ask which path the engine is on this frame.
//
// THE SIMULATION GATE, which is why this header carries focus state at all. LTClient_IsClientActive()
// reads g_ClientGlob_bClientActive, and CClientShell::Update checks it THREE times:
//
//   * an idle/background flag it passes to its first callee
//   * `if (active) sub_40B380(g_pClientMgr)`                     -- SKIPPED when inactive
//   * `if (active) LTClient_InterpolateObjectTransforms(this)`   -- SKIPPED when inactive
//
// while the IClientShell Pre/Update/Post callbacks run unconditionally. CClientMgr::Update likewise
// gates the ILTInput poll on `active && !buffered`. THAT IS THE MECHANISM behind a three-way liveness
// split measured earlier in this project: a frame hook on CClientShell::Update kept firing at ~170 Hz
// while the engine clock and the render path were both frozen. The main loop pumps; simulation and
// rendering are switched off by one flag.
//
// For a VR mod the consequence is direct and the reason simulation_is_gated() is exposed: a headset
// does not care whether the desktop window has focus, but this engine stops simulating when it loses
// it. client_active_address() hands over the flag's address deliberately -- forcing it is a far smaller
// intervention than hooking the three call sites, though LTClient_WndProc will clear it again on the
// next WM_ACTIVATEAPP that arrives while minimized or with a lost D3D device.
//
// WHAT SETS THAT FLAG. LTClient_WndProc's WM_ACTIVATEAPP handler, which matches the reference's
// structure closely enough to borrow its names (m_bLostFocus, m_bClientActive, m_bRendererShutdown) but
// differs in one ground-truth detail: the reference clears m_bClientActive on ANY deactivation, while
// FEAR 2 clears it only when IsIconic() holds or the D3D device's TestCooperativeLevel (vtable slot 3)
// reports failure. On that same branch it tears the renderer down, which is the best available
// explanation for the scene renderer's state field reading 0 rather than any of its documented 1..4.
// The client shell is then told: LTEVENT_LOSTFOCUS = 6 on deactivate, LTEVENT_GAINEDFOCUS = 7 on
// activate, both confirmed against the reference's ltcodes.h.
namespace sdk {

class Input {
public:
    // ---- FOCUS AND THE SIMULATION GATE ------------------------------------------------

    struct FocusState {
        // g_ClientGlob_bClientActive. Do NOT read this as "the engine is running": measured at 0 while
        // the main loop was pumping CClientShell::Update at ~170 Hz. It gates simulation, not the loop.
        bool client_active{};
        // g_ClientGlob_bLostFocus -- a latch, so the WndProc acts once per transition.
        bool lost_focus{};
        bool minimized{};          // set by WM_SYSCOMMAND / SC_MINIMIZE
        bool renderer_shutdown{};  // suppresses the re-init on the way back
        bool render_initted{};     // g_bRenderInitted, the WM_ERASEBKGND and teardown gate
    };

    // nullopt only when the exe is not mapped or a read faulted.
    static std::optional<FocusState> focus();

    // Whether the engine is currently skipping the two simulation steps and the device poll. Exactly
    // !client_active, named for the question a consumer actually has.
    static std::optional<bool> simulation_is_gated();

    // The gate's address, for a mod that intends to hold simulation on while the desktop window is not
    // active. 0 when the exe is not mapped.
    static uintptr_t client_active_address();

    // ---- DEVICES ----------------------------------------------------------------------
    //
    // CLTInput owns a fixed array of six device slots and polls each one by calling its vtable slot 2.
    // Two are populated in an ordinary session; the remaining four stay null, so a joystick or
    // gamepad plugged in later would appear here.

    static constexpr size_t kDeviceSlots = 6;

    enum class DeviceKind {
        Unknown,   // populated slot whose vtable is neither known table
        Keyboard,  // vtable exe+0x27807C
        Mouse,     // vtable exe+0x278050
    };

    struct Device {
        size_t slot{};
        uintptr_t address{};
        uintptr_t vtable{};
        DeviceKind kind{DeviceKind::Unknown};
    };

    // Populated slots only, in slot order.
    static std::vector<Device> devices();
    static std::optional<Device> device(DeviceKind kind);

    // ---- KEYBOARD ---------------------------------------------------------------------
    //
    // Three banks of 256 bytes: incoming, current, previous. The poll shifts current into previous and
    // incoming into current, so the previous bank makes edge detection possible without the caller
    // keeping its own copy -- which is what key_just_pressed exists for.

    static constexpr size_t kKeyStateCount = 256;

    static std::optional<bool> key_is_down(uint8_t vk);
    static std::optional<bool> key_was_down(uint8_t vk);
    static std::optional<bool> key_just_pressed(uint8_t vk);
    static std::optional<bool> key_just_released(uint8_t vk);

    // Every virtual key currently held, for a caller that wants to survey rather than ask.
    static std::optional<std::vector<uint8_t>> keys_down();

    // ---- MOUSE ------------------------------------------------------------------------

    struct MouseState {
        std::array<bool, 3> buttons{};       // left, right, middle -- current bank
        std::array<bool, 3> prev_buttons{};  // previous bank, for edges
        std::array<float, 2> axis{};         // current shifted axis pair (wheel/extra axes)
        int32_t screen_x{};                  // position in SCREEN space, as the engine stores it
        int32_t screen_y{};

        // The engine's own look delta: position minus the window centre, recomputed here the same way
        // its axis getter does it (GetClientRect + ClientToScreen on g_hMainWnd).
        //
        // FALSE WHEN THE WINDOW IS ICONIC, and that case is why this flag is not merely "the read
        // succeeded". A minimized window's client rect measures 160x28 at (-32000, -32000), so the
        // subtraction yields values like 34480 -- meaningless, and indistinguishable from a large mouse
        // movement. REJECTING A DEGENERATE RECT IS NOT ENOUGH: the first version of this check tested
        // `width > 0`, which 160 satisfies, and it still reported valid and still produced 34480. The
        // engine's own getter has the identical hazard and is simply never reached in that state,
        // because the device poll is gated off along with the simulation. A consumer reading this SDK
        // has no such protection.
        std::array<float, 2> look_delta{};
        bool look_delta_valid{};
    };

    static std::optional<MouseState> mouse();

    // ---- THE WINDOW, AND TWO DIFFERENT MEANINGS OF "MINIMIZED" ------------------------
    //
    // FocusState::minimized is g_ClientGlob_bMinimized, which LTClient_WndProc sets on receiving
    // WM_SYSCOMMAND / SC_MINIMIZE. The branch that actually clears the simulation gate and tears the
    // renderer down tests something else entirely: a LIVE IsIconic() call, or a failed
    // TestCooperativeLevel on the D3D device. Those disagree in practice -- measured with
    // minimized == false, lost_focus == true, client_active == false and render_initted == false, on a
    // window whose client rect was degenerate and parked at -32000, i.e. iconic by every real measure
    // while the flag said otherwise.
    //
    // So a consumer asking "is the window minimized" wants this, not the flag.
    static uintptr_t main_window();
    static std::optional<bool> window_is_iconic();

    struct WindowGeometry {
        int32_t client_width{};
        int32_t client_height{};
        int32_t screen_x{};  // client origin in screen space
        int32_t screen_y{};
        bool iconic{};
    };

    // The client area the engine renders into, which is also what the mouse axis getter measures its
    // centre from.
    //
    // AN ICONIC WINDOW REPORTS 160x28 AT (-32000, -32000) -- measured, not supposed. Both numbers are
    // nonsense for rendering, but they are NON-ZERO, which is exactly why a `width > 0` guard passes and
    // why look_delta_valid keys on iconic instead. A consumer sizing a stereo target wants the iconic
    // flag as much as the dimensions.
    static std::optional<WindowGeometry> window_geometry();

    // ---- THE WINDOW-MESSAGE KEY QUEUE -------------------------------------------------
    //
    // Filled by LTInput_QueueKeyDown / LTInput_QueueKeyUp straight out of the window procedure, as
    // parallel arrays of 100 entries with the two counters immediately after them. Both handlers
    // capture more than the reference's equivalent did: the translated ASCII and Unicode characters and
    // a modifier mask, alongside the virtual key.

    static constexpr size_t kKeyQueueCapacity = 100;

    enum Mod : uint16_t {
        ModShift = 0x01,
        ModCtrl = 0x02,
        ModAlt = 0x04,
        ModNumLock = 0x08,    // toggle state, not held state
        ModCapsLock = 0x10,   // toggle
        ModScrollLock = 0x20, // toggle
    };

    struct KeyEvent {
        uint32_t vk{};
        uint32_t lparam{};   // key-down only: the message's lParam masked to 0x7FFF
        uint16_t ascii{};    // ToAscii result, 0 when it failed
        uint16_t unicode{};  // ToUnicode result, 0 when it failed
        uint16_t mods{};     // Mod bits

        bool has(Mod m) const { return (mods & static_cast<uint16_t>(m)) != 0; }
    };

    static std::vector<KeyEvent> pending_key_downs();
    static std::vector<KeyEvent> pending_key_ups();

    // Whether the engine will CONSUME the queue this frame. false means the device poll is running
    // instead and anything written into the queue is inert -- see the two-paths note above.
    static std::optional<bool> key_queue_is_drained();

    // BEWARE when reading a key-up's ascii field. LTInput_QueueKeyUp's translation test is inverted
    // relative to the key-down handler: the down path zeroes the character when ToAscii FAILS, while
    // the up path zeroes it when ToAscii SUCCEEDS and leaves the stack value untouched when it fails.
    // The up queue's ascii is therefore either 0 or uninitialised stack, never a character. Its unicode
    // field is fine. This is a defect in the binary, not in the transcription, and it is surfaced here
    // rather than papered over because a consumer reading ascii off a key-up would otherwise get
    // plausible garbage.
    static constexpr bool kKeyUpAsciiIsUnreliable = true;
};

}  // namespace sdk
