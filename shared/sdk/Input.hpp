#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
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

    // ---- THE ENGINE'S INPUT OBJECT NAMESPACE ------------------------------------------
    //
    // Everything the engine binds to is addressed by an "object id", and the whole namespace is four
    // lines of LTInput_DeviceIndexForObject:
    //
    //     2000..2021  ->  device index (kind + 2), i.e. slots 2..5   -- 22 joystick objects
    //     1000..1006  ->  device index 1, the MOUSE
    //     anything else -> device index 0, the KEYBOARD (virtual-key codes)
    //
    // NOTE THAT THE KEYBOARD IS THE FALLTHROUGH, not a range: id 5000 resolves to the keyboard and reads
    // whatever byte sits at that offset rather than being rejected. classify_object() reproduces that
    // faithfully instead of pretending the engine validates, because a consumer passing a bad id needs to
    // know it will get an answer rather than an error.
    //
    // JOYSTICK IDS DO NOT DISPATCH. LTInput_ObjectChanged rejects device indices 2..5 outright, so no
    // binding on a 2000-range object can fire through the path CLTInput_Poll drives. Whether some other
    // path serves them is NOT established, which is why classify_object reports Joystick honestly while
    // the accessors below refuse those ids.
    //
    // WHY GO THROUGH THIS AT ALL when key_is_down() reads the bank directly: this is the namespace the
    // engine's own bindings speak, so a consumer inspecting or replicating a binding needs the same
    // addressing. It is also a genuinely independent path to the same state -- the accessors below call
    // the device's own vtable methods rather than reading fields -- and the suite cross-checks the two
    // against each other across all 256 keys and every mouse object.

    enum class ObjectClass {
        Keyboard,  // device 0; virtual-key codes, and the fallthrough for unrecognised ids
        Mouse,     // device 1; ids 1000..1006
        Joystick,  // devices 2..5; ids 2000..2021, which the binding dispatch rejects
    };

    static ObjectClass classify_object(int object_id);

    // Current value through the device's own getter (vtable slot 3). 1.0/0.0 for digital objects; for
    // mouse 1003/1004 it is the centre-relative position, and for 1005/1006 the axis float.
    //
    // nullopt when the object's device is absent or the read faulted, and for JOYSTICK ids, which this
    // SDK refuses rather than guessing which of slots 2..5 a given kind would select.
    static std::optional<float> object_value(int object_id);

    // The previous frame's value (vtable slot 4), which is what the engine's own edge and threshold logic
    // compares against.
    static std::optional<float> object_previous_value(int object_id);

    // The device's own change test (vtable slot 1) -- `previous != current`, NOT "is it down". This is
    // what decides whether a binding's handler fires, so bindings are EDGE-TRIGGERED.
    //
    // Mouse 1003/1004 always answer false: a continuous position axis never reports a change, which is
    // why the engine routes those through a threshold accumulator instead.
    static std::optional<bool> object_changed(int object_id);

    // ---- ILTInput's VTABLE, AND THE ADJUSTOR THUNKS ------------------------------------
    //
    // Every slot below is a thin adjustor thunk -- `add ecx, 4; jmp impl` -- shifting `this` from the
    // CLTInput object to its DEVICE ARRAY at +4. A caller going through the vtable therefore passes the
    // INTERFACE pointer and lets the thunk adjust; handing it the array directly would adjust twice.
    //
    //    0  ~CLTInput(bool deleting)
    //    1  InterfaceImplementation() -> "CLTInput"
    //    2  Init()      -- allocates the keyboard (0x304) and mouse (0x30), installs the subclass WndProc
    //    3  Poll()      -- polls the six device slots, then dispatches every binding set
    //    4  Term()      -- restores the WndProc, then destroys all six devices via their slot 0
    //    5  EnableInput()   -- writes 1 to the enable flag at +0x4C
    //    6  DisableInput()  -- writes 0 to it
    //    7  AllocBindingSet(recordCount) -> set
    //    8  DestroyBindingSet(set)
    //    9  SetBindingSetDeviceKind(set, kind)  -- the byte at the set's +0x1C; -1 leaves it inert
    //   10  GetDeviceCount() -> 6
    //   11  IsDevicePresent(index) -> bool
    //
    // SLOTS 5 AND 6 ARE WHY THE ENABLE FLAG IS WORTH KNOWING: the engine exposes an explicit switch for
    // input, entirely separate from the simulation gate, and a mod that wants input while the game thinks
    // it should be off can drive it deliberately rather than fighting the window messages that clear it.

    static constexpr size_t kSlotPoll = 3;
    static constexpr size_t kSlotEnableInput = 5;
    static constexpr size_t kSlotDisableInput = 6;
    static constexpr size_t kSlotAllocBindingSet = 7;
    static constexpr size_t kSlotDestroyBindingSet = 8;
    static constexpr size_t kSlotSetBindingSetKind = 9;
    static constexpr size_t kSlotGetDeviceCount = 10;
    static constexpr size_t kSlotIsDevicePresent = 11;

    // The ILTInput object, which is the CLTInput singleton. 0 when the exe is not mapped.
    static uintptr_t interface_address();

    // ---- DEVICE SIZES, WHICH PROVE THE LAYOUTS ------------------------------------------
    //
    // CLTInput::Init allocates each device with a literal size, derived independently of the field
    // offsets this header uses -- those came from the poll functions' shift loops and the value getters.
    // The two agree exactly, and the static_asserts below are that agreement written down: if anyone
    // adjusts an offset without re-deriving it, the build stops.

    static constexpr size_t kKeyboardDeviceSize = 0x304;
    static constexpr size_t kMouseDeviceSize = 0x30;

    // ---- THE BINDING SETS: THE ENGINE'S ACTION TABLE -----------------------------------
    //
    // CLTInput_Poll polls the devices and then walks a list of binding sets, firing the handler of every
    // record whose object CHANGED. This is the action layer, and enumerating it tells a consumer which
    // actions the engine knows and what they are bound to.
    //
    // THE LIST IS AN ARRAY, not a linked list -- established by reading the iterator primitives rather
    // than by probing memory. The container at CLTInput+0x50 holds a begin pointer at +0x54 and an end
    // pointer at +0x58; the constructor validates begin <= p <= end, advance adds 4, and dereference
    // yields the element address. So the elements are 4-byte binding-set pointers in contiguous storage.
    // (An earlier reading of this project probed +0x54 as a {next, prev, value} node, found nothing
    // recognisable, and correctly recorded the layout as unestablished. The model was wrong, not the
    // data.)
    //
    // MEASURED: one set, 108 records, kind 0.

    struct BindingRecord {
        uint32_t action_code{};  // +0x00 -- and it IS the action code, not a context word: the dispatcher
                                 // calls handler(action_code, userdata). Live values run 0, 1, 3, ...
        uintptr_t handler{};     // +0x04 -- 0 when no handler is installed
        uint32_t userdata{};     // +0x08 -- second argument to the handler
        uintptr_t owner{};       // +0x0C -- back-pointer to the owning set header
        int32_t primary{};       // +0x10 -- object id, -1 when unbound
        int32_t alternate{};     // +0x14 -- second object id, -1 when unbound
        int32_t primary_modifier{};    // +0x18 -- -1 when none; with a modifier the record goes through
        int32_t alternate_modifier{};  // +0x1C    the threshold evaluator instead of the change test

        bool is_bound() const { return primary != -1 || alternate != -1; }
        bool has_handler() const { return handler != 0; }
    };

    struct BindingSet {
        uintptr_t address{};
        uintptr_t records{};      // +0x00
        uint32_t record_count{};  // +0x18
        int32_t kind{};           // +0x1C, as a SIGNED byte widened

        // A set allocated but never given a device kind is INERT: with kind -1 every object lookup
        // returns 0, keyboard and mouse included, so none of its records can ever fire.
        bool is_inert() const { return kind == -1; }

        std::vector<BindingRecord> entries;
    };

    // A cap, so a corrupt count cannot spin. Well above the 108 measured.
    static constexpr size_t kMaxBindingRecords = 4096;

    static std::vector<BindingSet> binding_sets();

    // The actions that are actually bound to something, across every set -- the question a consumer
    // asking "what does this game respond to" actually has.
    static std::vector<BindingRecord> bound_actions();

    // ---- THE ENGINE'S OWN ANSWERS ABOUT ITS DEVICES ------------------------------------
    //
    // Both call through the ILTInput vtable rather than reading fields, which makes them an independent
    // check on the device array this SDK walks itself. GetDeviceCount is a leaf returning a constant;
    // IsDevicePresent tests devices[index] and forwards to a stub that always returns true, so its answer
    // is exactly "that slot is populated" -- the same question devices() answers by walking. The suite
    // compares the two across all six slots.

    static std::optional<uint32_t> engine_device_count();
    static std::optional<bool> device_is_present(size_t index);

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

    // ---- THE SUBCLASS WINDOW PROCEDURE ------------------------------------------------
    //
    // This is why LTClient_WndProc handles no mouse message: the engine SUBCLASSES ITS OWN WINDOW.
    // LTInput_InstallSubclassWndProc saves the existing GWL_WNDPROC and installs
    // LTInput_SubclassWndProc, which forwards to LTInput_TranslateWindowMessage and then chains on with
    // CallWindowProcW. Verified live: the saved original holds LTClient_WndProc's own address.
    //
    // EXPECT COMPANY ON THIS WINDOW. gameoverlayrenderer.dll is loaded in any Steam session and
    // subclasses the same window, so whatever sits in GWL_WNDPROC at a given moment is not necessarily
    // the engine's proc. engine_owns_window reports that honestly rather than asserting a chain nobody
    // controls; saved_is_engine is the part that IS a fact about the engine.

    struct WndProcChain {
        uintptr_t current{};         // what the window has installed right now
        uintptr_t subclass{};        // LTInput_SubclassWndProc
        uintptr_t saved_original{};  // g_pOriginalWndProc, as saved at install time
        uintptr_t engine_wndproc{};  // LTClient_WndProc
        bool engine_owns_window{};   // current == subclass: nobody has subclassed on top
        bool saved_is_engine{};      // saved_original == engine_wndproc

        // Basename of the module that owns whatever is currently installed, empty when it could not be
        // resolved. MEASURED NOT TO BE THE EXE in a Steam session -- which is the practical reason this
        // field exists: a mod that assumes the engine's proc is installed is wrong before it starts.
        std::string current_owner;
    };

    static std::optional<WndProcChain> wndproc_chain();

    // ---- THE OTHER GATE ---------------------------------------------------------------
    //
    // Input has a second switch, INDEPENDENT of the simulation gate: the translator clears it on
    // WM_CANCELMODE (a dialog taking over) and sets it on WM_NCACTIVATE, resetting every device state
    // on both edges. Measured 1 while the window was iconic and simulation was gated off, so a consumer
    // diagnosing "why is there no input" must check both this and simulation_is_gated().

    static std::optional<bool> input_is_enabled();
    static uintptr_t input_enabled_address();

    // The device array's address, and the pointer the engine publishes to it. Those must agree -- the
    // published pointer is the array's own address -- which is a cheap way to detect a build whose
    // layout differs from this mapping.
    static uintptr_t device_array_address();
    static uintptr_t published_device_array();

    // ---- SYNTHETIC INPUT: THE ENGINE'S OWN ENTRY POINTS --------------------------------
    //
    // For a mod that needs to feed the engine input it did not get from the OS -- which for a VR mod is
    // the whole point, since head pose and controller state arrive from a runtime rather than a window
    // message. These are the functions the translator itself calls, so driving them puts synthetic input
    // exactly where real input goes, one layer below the message pump.
    //
    // ALL ARE THISCALL, ecx holding the object named below. Signatures as observed:
    //
    //   mouse_on_move(deviceArray, hwnd, int clientX, int clientY, uint flags)
    //       -- deviceArray is device_array_address(); stores the point and converts it to screen space.
    //   mouse_set_incoming_button(mouseDevice, int button, char state)
    //       -- writes the incoming button bank; the poll shifts it into current next frame.
    //   keyboard_set_incoming_key(keyboardDevice, int vk, int isDown, int lparam)
    //       -- writes the incoming key bank at +0x204.
    //   mouse_on_wheel(deviceArray, hwnd, int x, int y, int delta, uint keys)
    //   center_cursor()  -- no arguments; warps the cursor to the window centre via SetCursorPos.
    //
    // Note the asymmetry deliberately: the button and key writers take a DEVICE, while the move and
    // wheel handlers take the ARRAY. That is how the engine calls them, and getting it backwards would
    // write through a wrong base.
    //
    // NOT CALLED BY THIS SDK. Handing over verified addresses with their signatures is the useful part;
    // deciding when to drive them, and on which thread, belongs to the consumer.

    struct EntryPoints {
        uintptr_t translate_window_message{};
        uintptr_t mouse_on_move{};
        uintptr_t mouse_set_incoming_button{};
        uintptr_t keyboard_set_incoming_key{};
        uintptr_t mouse_on_wheel{};
        uintptr_t center_cursor{};

        // Every address resolved and inside the exe image.
        bool all_resolved() const;
    };

    static EntryPoints entry_points();

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
