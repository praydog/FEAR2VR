#include "Input.hpp"

#include <cstring>

#include <windows.h>

#include <utility/Seh.hpp>

#include "Modules.hpp"

namespace sdk {

namespace {

// ---- exe offsets ---------------------------------------------------------------------------------
//
// All named in the IDB. The focus flags are four consecutive dwords; the key queue is one contiguous
// 0x964-byte block whose internal layout is ARITHMETICALLY CLOSED -- every array's end is exactly the
// next array's start, and the two counters sit immediately past the last of them:
//
//   0x2E4744 downVK[100]   -> 0x2E48D4 downLParam[100] -> 0x2E4A64 downAscii[100]
//   0x2E4B2C downUnicode[100] -> 0x2E4BF4 downMods[100] -> 0x2E4CBC upVK[100]
//   0x2E4E4C upAscii[100]  -> 0x2E4F14 upUnicode[100]   -> 0x2E4FDC upMods[100]
//   0x2E50A4 nDowns (u16)  -> 0x2E50A6 nUps (u16)       -> 0x2E50A8 bufferedKeyInput (u32)
//
// There is no slack anywhere in that chain, which is what makes the array bounds facts rather than
// guesses: a 101st element of any array would overlap the next one's first.
constexpr uintptr_t kClientActive = 0x2E4734;
constexpr uintptr_t kLostFocus = 0x2E4738;
constexpr uintptr_t kMinimized = 0x2E473C;
constexpr uintptr_t kRendererShutdown = 0x2E4740;
constexpr uintptr_t kRenderInitted = 0x2F75D4;  // byte

constexpr uintptr_t kDownVK = 0x2E4744;
constexpr uintptr_t kDownLParam = 0x2E48D4;
constexpr uintptr_t kDownAscii = 0x2E4A64;
constexpr uintptr_t kDownUnicode = 0x2E4B2C;
constexpr uintptr_t kDownMods = 0x2E4BF4;
constexpr uintptr_t kUpVK = 0x2E4CBC;
constexpr uintptr_t kUpAscii = 0x2E4E4C;
constexpr uintptr_t kUpUnicode = 0x2E4F14;
constexpr uintptr_t kUpMods = 0x2E4FDC;
constexpr uintptr_t kNumDowns = 0x2E50A4;  // u16
constexpr uintptr_t kNumUps = 0x2E50A6;    // u16
constexpr uintptr_t kBufferedInput = 0x2E50A8;
constexpr uintptr_t kBufferedInputEnabled = 0x2E5498;  // byte; gates the above

constexpr uintptr_t kMainWnd = 0x333000;
constexpr uintptr_t kCLTInputDevices = 0x2F768C;  // CLTInput + 4
constexpr uintptr_t kKeyboardVtable = 0x27807C;
constexpr uintptr_t kMouseVtable = 0x278050;

// ---- device field offsets, from the poll functions' own shift loops ------------------------------
//
// Keyboard: the poll walks 256 iterations doing `cur[i + 256] = cur[i]; cur[i] = cur[i + 512]`, which
// fixes all three banks at once. Mouse: three button bytes and two axis floats shifted the same way.
constexpr uintptr_t kKbCurrent = 0x04;
constexpr uintptr_t kKbPrevious = 0x104;  // +4 + 256
constexpr uintptr_t kMouseButtons = 0x04;
constexpr uintptr_t kMousePrevButtons = 0x07;
constexpr uintptr_t kMouseAxisCurrent = 0x18;  // incoming pair lives at +0x10/+0x14
constexpr uintptr_t kMousePosX = 0x28;
constexpr uintptr_t kMousePosY = 0x2C;

uintptr_t exe_at(uintptr_t offset) {
    const auto* exe = Modules::get().exe();
    if (exe == nullptr || exe->base == 0) {
        return 0;
    }
    return exe->base + offset;
}

bool seh_copy(void* out, uintptr_t at, size_t bytes) {
    if (at == 0 || out == nullptr || bytes == 0) {
        return false;
    }
    bool ok = false;
    KANANLIB_SEH_TRY {
        std::memcpy(out, reinterpret_cast<const void*>(at), bytes);
        ok = true;
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return ok;
}

template <typename T>
std::optional<T> read_exe(uintptr_t offset) {
    T value{};
    if (!seh_copy(&value, exe_at(offset), sizeof(value))) {
        return std::nullopt;
    }
    return value;
}

// The device bank a caller asks about, resolved through the live device array rather than a cached
// pointer: the slots are heap addresses that only exist once CLTInput has built them.
std::optional<uintptr_t> device_address(Input::DeviceKind kind) {
    const auto found = Input::device(kind);
    if (!found.has_value()) {
        return std::nullopt;
    }
    return found->address;
}

// Both key queues are read the same way; only the array bases differ, and the up queue has no lParam.
std::vector<Input::KeyEvent> read_queue(uintptr_t count_off, uintptr_t vk_off, uintptr_t lparam_off,
                                       uintptr_t ascii_off, uintptr_t unicode_off, uintptr_t mods_off) {
    std::vector<Input::KeyEvent> out;
    const auto count = read_exe<uint16_t>(count_off);
    if (!count.has_value()) {
        return out;
    }
    // Clamped rather than trusted: the handlers bound the counter at 100, but a caller that has been
    // writing into the queue itself could leave it anywhere.
    const size_t n = *count > Input::kKeyQueueCapacity ? Input::kKeyQueueCapacity : *count;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        Input::KeyEvent ev{};
        const auto vk = read_exe<uint32_t>(vk_off + i * sizeof(uint32_t));
        if (!vk.has_value()) {
            break;
        }
        ev.vk = *vk;
        if (lparam_off != 0) {
            ev.lparam = read_exe<uint32_t>(lparam_off + i * sizeof(uint32_t)).value_or(0);
        }
        ev.ascii = read_exe<uint16_t>(ascii_off + i * sizeof(uint16_t)).value_or(0);
        ev.unicode = read_exe<uint16_t>(unicode_off + i * sizeof(uint16_t)).value_or(0);
        ev.mods = read_exe<uint16_t>(mods_off + i * sizeof(uint16_t)).value_or(0);
        out.push_back(ev);
    }
    return out;
}

}  // namespace

// ---- focus ---------------------------------------------------------------------------------------

std::optional<Input::FocusState> Input::focus() {
    const auto active = read_exe<uint32_t>(kClientActive);
    const auto lost = read_exe<uint32_t>(kLostFocus);
    const auto mini = read_exe<uint32_t>(kMinimized);
    const auto shut = read_exe<uint32_t>(kRendererShutdown);
    const auto inited = read_exe<uint8_t>(kRenderInitted);
    if (!active.has_value() || !lost.has_value() || !mini.has_value() || !shut.has_value() ||
        !inited.has_value()) {
        return std::nullopt;
    }
    FocusState s{};
    s.client_active = *active != 0;
    s.lost_focus = *lost != 0;
    s.minimized = *mini != 0;
    s.renderer_shutdown = *shut != 0;
    s.render_initted = *inited != 0;
    return s;
}

std::optional<bool> Input::simulation_is_gated() {
    const auto s = focus();
    if (!s.has_value()) {
        return std::nullopt;
    }
    return !s->client_active;
}

uintptr_t Input::client_active_address() {
    return exe_at(kClientActive);
}

// ---- devices -------------------------------------------------------------------------------------

std::vector<Input::Device> Input::devices() {
    std::vector<Device> out;
    const uintptr_t kb_vt = exe_at(kKeyboardVtable);
    const uintptr_t mouse_vt = exe_at(kMouseVtable);
    for (size_t i = 0; i < kDeviceSlots; ++i) {
        const auto ptr = read_exe<uint32_t>(kCLTInputDevices + i * sizeof(uint32_t));
        if (!ptr.has_value() || *ptr == 0) {
            continue;  // an empty slot is normal: four of six are unused without a gamepad
        }
        Device d{};
        d.slot = i;
        d.address = *ptr;
        uint32_t vt = 0;
        if (seh_copy(&vt, d.address, sizeof(vt))) {
            d.vtable = vt;
            if (vt == kb_vt) {
                d.kind = DeviceKind::Keyboard;
            } else if (vt == mouse_vt) {
                d.kind = DeviceKind::Mouse;
            }
        }
        out.push_back(d);
    }
    return out;
}

std::optional<Input::Device> Input::device(DeviceKind kind) {
    for (const auto& d : devices()) {
        if (d.kind == kind) {
            return d;
        }
    }
    return std::nullopt;
}

// ---- keyboard ------------------------------------------------------------------------------------

std::optional<bool> Input::key_is_down(uint8_t vk) {
    const auto dev = device_address(DeviceKind::Keyboard);
    if (!dev.has_value()) {
        return std::nullopt;
    }
    uint8_t state = 0;
    if (!seh_copy(&state, *dev + kKbCurrent + vk, sizeof(state))) {
        return std::nullopt;
    }
    // The engine's own getter tests `== 1` rather than non-zero, so this matches it exactly instead of
    // treating any other value as pressed.
    return state == 1;
}

std::optional<bool> Input::key_was_down(uint8_t vk) {
    const auto dev = device_address(DeviceKind::Keyboard);
    if (!dev.has_value()) {
        return std::nullopt;
    }
    uint8_t state = 0;
    if (!seh_copy(&state, *dev + kKbPrevious + vk, sizeof(state))) {
        return std::nullopt;
    }
    return state == 1;
}

std::optional<bool> Input::key_just_pressed(uint8_t vk) {
    const auto now = key_is_down(vk);
    const auto before = key_was_down(vk);
    if (!now.has_value() || !before.has_value()) {
        return std::nullopt;
    }
    return *now && !*before;
}

std::optional<bool> Input::key_just_released(uint8_t vk) {
    const auto now = key_is_down(vk);
    const auto before = key_was_down(vk);
    if (!now.has_value() || !before.has_value()) {
        return std::nullopt;
    }
    return !*now && *before;
}

std::optional<std::vector<uint8_t>> Input::keys_down() {
    const auto dev = device_address(DeviceKind::Keyboard);
    if (!dev.has_value()) {
        return std::nullopt;
    }
    uint8_t bank[kKeyStateCount]{};
    if (!seh_copy(bank, *dev + kKbCurrent, sizeof(bank))) {
        return std::nullopt;
    }
    std::vector<uint8_t> out;
    for (size_t i = 0; i < kKeyStateCount; ++i) {
        if (bank[i] == 1) {
            out.push_back(static_cast<uint8_t>(i));
        }
    }
    return out;
}

// ---- mouse ---------------------------------------------------------------------------------------

std::optional<Input::MouseState> Input::mouse() {
    const auto dev = device_address(DeviceKind::Mouse);
    if (!dev.has_value()) {
        return std::nullopt;
    }
    uint8_t buttons[3]{};
    uint8_t prev[3]{};
    float axis[2]{};
    int32_t px = 0;
    int32_t py = 0;
    if (!seh_copy(buttons, *dev + kMouseButtons, sizeof(buttons)) ||
        !seh_copy(prev, *dev + kMousePrevButtons, sizeof(prev)) ||
        !seh_copy(axis, *dev + kMouseAxisCurrent, sizeof(axis)) ||
        !seh_copy(&px, *dev + kMousePosX, sizeof(px)) ||
        !seh_copy(&py, *dev + kMousePosY, sizeof(py))) {
        return std::nullopt;
    }
    MouseState s{};
    for (size_t i = 0; i < 3; ++i) {
        s.buttons[i] = buttons[i] == 1;
        s.prev_buttons[i] = prev[i] == 1;
    }
    s.axis[0] = axis[0];
    s.axis[1] = axis[1];
    s.screen_x = px;
    s.screen_y = py;

    // Reproduced from the engine's own axis getter rather than reinvented: it takes the client rect,
    // converts the client origin to screen space, and subtracts (origin + half the rect) from the
    // stored position. Doing it any other way would give a delta that disagrees with what the engine
    // feeds the game.
    // Geometry through the same accessor a consumer would use, so both agree by construction. The
    // ICONIC test is the load-bearing one: a minimized window measures 160x28 at (-32000, -32000), so a
    // `width > 0` guard passes and still yields a garbage delta of ~34480.
    const auto geom = window_geometry();
    if (geom.has_value() && !geom->iconic && geom->client_width > 0 && geom->client_height > 0) {
        s.look_delta[0] = static_cast<float>(px - geom->client_width / 2 - geom->screen_x);
        s.look_delta[1] = static_cast<float>(py - geom->client_height / 2 - geom->screen_y);
        s.look_delta_valid = true;
    }
    return s;
}

// ---- the window ----------------------------------------------------------------------------------

uintptr_t Input::main_window() {
    return read_exe<uint32_t>(kMainWnd).value_or(0);
}

std::optional<Input::WindowGeometry> Input::window_geometry() {
    const uintptr_t raw = main_window();
    if (raw == 0) {
        return std::nullopt;
    }
    auto hwnd = reinterpret_cast<HWND>(raw);
    RECT rect{};
    POINT origin{};
    if (::GetClientRect(hwnd, &rect) == 0) {
        return std::nullopt;
    }
    origin.x = rect.left;
    origin.y = rect.top;
    if (::ClientToScreen(hwnd, &origin) == 0) {
        return std::nullopt;
    }
    WindowGeometry g{};
    g.client_width = rect.right - rect.left;
    g.client_height = rect.bottom - rect.top;
    g.screen_x = origin.x;
    g.screen_y = origin.y;
    g.iconic = ::IsIconic(hwnd) != 0;
    return g;
}

std::optional<bool> Input::window_is_iconic() {
    const uintptr_t hwnd = main_window();
    if (hwnd == 0) {
        return std::nullopt;
    }
    return ::IsIconic(reinterpret_cast<HWND>(hwnd)) != 0;
}

// ---- the window-message key queue ----------------------------------------------------------------

std::vector<Input::KeyEvent> Input::pending_key_downs() {
    return read_queue(kNumDowns, kDownVK, kDownLParam, kDownAscii, kDownUnicode, kDownMods);
}

std::vector<Input::KeyEvent> Input::pending_key_ups() {
    return read_queue(kNumUps, kUpVK, 0, kUpAscii, kUpUnicode, kUpMods);
}

std::optional<bool> Input::key_queue_is_drained() {
    // LTInput_IsBufferedKeyInputActive() is `enabled ? bufferedFlag : 0` -- two independent globals,
    // reproduced here so a caller sees the same answer the engine acts on.
    const auto enabled = read_exe<uint8_t>(kBufferedInputEnabled);
    const auto flag = read_exe<uint32_t>(kBufferedInput);
    if (!enabled.has_value() || !flag.has_value()) {
        return std::nullopt;
    }
    return *enabled != 0 && *flag != 0;
}

}  // namespace sdk
