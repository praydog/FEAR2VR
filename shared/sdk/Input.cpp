#include "Input.hpp"

#include <cstring>
#include <iterator>

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
constexpr uintptr_t kInputEnabled = 0x2F76D4;        // device array + 0x48
constexpr uintptr_t kOriginalWndProc = 0x2F76FC;
constexpr uintptr_t kPublishedDeviceArray = 0x2F7700;
constexpr uintptr_t kSubclassWndProc = 0x6D08F;
constexpr uintptr_t kEngineWndProc = 0x66B82;
constexpr uintptr_t kTranslateWindowMessage = 0x6CE93;
constexpr uintptr_t kMouseOnMove = 0x6CE11;
constexpr uintptr_t kMouseSetIncomingButton = 0x6C78A;
constexpr uintptr_t kKeyboardSetIncomingKey = 0x6C73E;
constexpr uintptr_t kMouseOnWheel = 0x6CE2E;
constexpr uintptr_t kCenterCursor = 0x69199;

// The binding-set list. CLTInput+0x50 is the container the iterators validate against; +0x54 and +0x58
// are its begin and end pointers, and the elements are 4-byte binding-set pointers.
constexpr uintptr_t kBindingListBegin = 0x2F76DC;  // CLTInput + 0x54
constexpr uintptr_t kBindingListEnd = 0x2F76E0;    // CLTInput + 0x58

// Binding set header and record offsets, all confirmed live -- the record's owner field points back at
// its own set header, which is what makes the stride and base checkable rather than assumed.
constexpr uintptr_t kSetRecords = 0x00;
constexpr uintptr_t kSetRecordCount = 0x18;
constexpr uintptr_t kSetKind = 0x1C;
constexpr size_t kRecordStride = 0x20;
constexpr uintptr_t kCLTInputDevices = 0x2F768C;  // CLTInput + 4
constexpr uintptr_t kCLTInput = 0x2F7688;  // the ILTInput object; its device array is +4
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

// THE LAYOUTS FILL THE ALLOCATIONS EXACTLY, and these are that fact as a build-time check.
//
// Keyboard: 4 bytes of vtable then three 256-byte banks.
static_assert(sizeof(uint32_t) + 3u * Input::kKeyStateCount == Input::kKeyboardDeviceSize,
              "keyboard bank layout no longer fills CLTInput::Init's 0x304 allocation");
// Mouse: vtable, three 3-byte button banks, 3 bytes of padding to align the floats, three pairs of axis
// floats, then the two position ints. Written as the sum so a changed offset breaks the build.
static_assert(sizeof(uint32_t) + 3u * 3u + 3u + 3u * 2u * sizeof(float) + 2u * sizeof(int32_t) ==
                  Input::kMouseDeviceSize,
              "mouse field layout no longer fills CLTInput::Init's 0x30 allocation");

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

// ---- the engine's own answers about its devices ---------------------------------------------------

uintptr_t Input::interface_address() {
    return exe_at(kCLTInput);
}

uintptr_t Input::interface_vtable() {
    const uintptr_t obj = interface_address();
    if (obj == 0) {
        return 0;
    }
    uint32_t vt = 0;
    if (!seh_copy(&vt, obj, sizeof(vt))) {
        return 0;
    }
    return vt;
}

namespace {

// Resolve an ILTInput vtable slot, guarded and bounds-checked into the exe.
uintptr_t interface_slot(size_t slot) {
    const uintptr_t obj = Input::interface_address();
    if (obj == 0) {
        return 0;
    }
    uint32_t vtable = 0;
    if (!seh_copy(&vtable, obj, sizeof(vtable)) || vtable == 0) {
        return 0;
    }
    uint32_t fn = 0;
    if (!seh_copy(&fn, vtable + slot * sizeof(uint32_t), sizeof(fn)) || fn == 0) {
        return 0;
    }
    const auto* exe = Modules::get().exe();
    if (exe == nullptr || exe->base == 0 || fn < exe->base || fn >= exe->base + exe->size) {
        return 0;
    }
    return fn;
}

}  // namespace

std::optional<uint32_t> Input::engine_device_count() {
    const uintptr_t fn = interface_slot(kSlotGetDeviceCount);
    const uintptr_t obj = interface_address();
    if (fn == 0 || obj == 0) {
        return std::nullopt;
    }
    // Returns a byte in al; the thunk adjusts `this` itself, so pass the interface pointer.
    using Fn = uint8_t(__thiscall*)(void*);
    uint8_t out = 0;
    KANANLIB_SEH_TRY {
        out = reinterpret_cast<Fn>(fn)(reinterpret_cast<void*>(obj));
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        return std::nullopt;
    }
    return static_cast<uint32_t>(out);
}

namespace {

// MSVC refuses __try in a function that also needs object unwinding, so the guarded call lives here with
// nothing but PODs and the caller builds the string.
bool call_get_key_name(uintptr_t fn, uintptr_t obj, uint32_t vk, wchar_t* buffer, int count) {
    using Fn = uint8_t(__thiscall*)(void*, uint32_t, wchar_t*, int);
    uint8_t ok = 0;
    KANANLIB_SEH_TRY {
        ok = reinterpret_cast<Fn>(fn)(reinterpret_cast<void*>(obj), vk, buffer, count);
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return ok != 0;
}

}  // namespace

std::optional<std::string> Input::key_name(uint8_t vk) {
    if (vk == 0) {
        return std::nullopt;  // the engine rejects vk outside 1..255
    }
    const uintptr_t fn = interface_slot(kSlotGetKeyName);
    const uintptr_t obj = interface_address();
    if (fn == 0 || obj == 0) {
        return std::nullopt;
    }
    wchar_t buffer[64]{};
    if (!call_get_key_name(fn, obj, vk, buffer, static_cast<int>(std::size(buffer)))) {
        return std::nullopt;
    }
    std::string out;
    for (const wchar_t* p = buffer; *p != L'\0' && out.size() < 63; ++p) {
        out.push_back(static_cast<char>(*p < 128 ? *p : '?'));
    }
    return out;
}

std::optional<uint32_t> Input::engine_object_device_index(int object_id) {
    const uintptr_t fn = interface_slot(kSlotGetObjectDeviceIndex);
    const uintptr_t obj = interface_address();
    if (fn == 0 || obj == 0) {
        return std::nullopt;
    }
    using Fn = uint8_t(__thiscall*)(void*, int, uint32_t*);
    uint32_t index = 0xFFFFFFFFu;
    uint8_t ok = 0;
    KANANLIB_SEH_TRY {
        ok = reinterpret_cast<Fn>(fn)(reinterpret_cast<void*>(obj), object_id, &index);
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        return std::nullopt;
    }
    if (ok == 0) {
        return std::nullopt;
    }
    return index;
}

std::optional<bool> Input::engine_binding_is_active(uintptr_t record_address) {
    if (record_address == 0) {
        return std::nullopt;
    }
    const uintptr_t fn = interface_slot(kSlotIsBindingActive);
    const uintptr_t obj = interface_address();
    if (fn == 0 || obj == 0) {
        return std::nullopt;
    }
    using Fn = uint8_t(__thiscall*)(void*, uintptr_t);
    uint8_t out = 0;
    KANANLIB_SEH_TRY {
        out = reinterpret_cast<Fn>(fn)(reinterpret_cast<void*>(obj), record_address);
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        return std::nullopt;
    }
    return out != 0;
}

std::optional<bool> Input::device_is_present(size_t index) {
    if (index >= kDeviceSlots) {
        return false;  // the engine's own bound check is `index < 6`
    }
    const uintptr_t fn = interface_slot(kSlotIsDevicePresent);
    const uintptr_t obj = interface_address();
    if (fn == 0 || obj == 0) {
        return std::nullopt;
    }
    using Fn = uint8_t(__thiscall*)(void*, uint8_t);
    uint8_t out = 0;
    KANANLIB_SEH_TRY {
        out = reinterpret_cast<Fn>(fn)(reinterpret_cast<void*>(obj), static_cast<uint8_t>(index));
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        return std::nullopt;
    }
    return out != 0;
}

// ---- the binding sets ----------------------------------------------------------------------------

std::vector<Input::BindingSet> Input::binding_sets() {
    std::vector<BindingSet> out;
    const auto begin = read_exe<uint32_t>(kBindingListBegin);
    const auto end = read_exe<uint32_t>(kBindingListEnd);
    if (!begin.has_value() || !end.has_value() || *begin == 0 || *end < *begin) {
        return out;
    }
    // Stride 4: the iterator's advance adds 4 and its dereference yields the element address.
    const size_t count = (*end - *begin) / sizeof(uint32_t);
    for (size_t i = 0; i < count && i < kMaxBindingRecords; ++i) {
        uint32_t set_ptr = 0;
        if (!seh_copy(&set_ptr, *begin + i * sizeof(uint32_t), sizeof(set_ptr)) || set_ptr == 0) {
            continue;
        }
        BindingSet set{};
        set.address = set_ptr;
        uint32_t records = 0, record_count = 0;
        int8_t kind = 0;
        if (!seh_copy(&records, set_ptr + kSetRecords, sizeof(records)) ||
            !seh_copy(&record_count, set_ptr + kSetRecordCount, sizeof(record_count)) ||
            !seh_copy(&kind, set_ptr + kSetKind, sizeof(kind))) {
            continue;
        }
        set.records = records;
        set.record_count = record_count;
        set.kind = kind;  // signed, so the inert -1 survives
        const size_t n = record_count > kMaxBindingRecords ? kMaxBindingRecords : record_count;
        set.entries.reserve(n);
        for (size_t r = 0; r < n && records != 0; ++r) {
            uint32_t raw[8]{};
            if (!seh_copy(raw, records + r * kRecordStride, sizeof(raw))) {
                break;
            }
            BindingRecord rec{};
            rec.address = records + r * kRecordStride;
            rec.action_code = raw[0];
            rec.handler = raw[1];
            rec.userdata = raw[2];
            rec.owner = raw[3];
            rec.primary = static_cast<int32_t>(raw[4]);
            rec.alternate = static_cast<int32_t>(raw[5]);
            rec.primary_modifier = static_cast<int32_t>(raw[6]);
            rec.alternate_modifier = static_cast<int32_t>(raw[7]);
            set.entries.push_back(rec);
        }
        out.push_back(std::move(set));
    }
    return out;
}

std::vector<Input::BindingRecord> Input::bound_actions() {
    std::vector<BindingRecord> out;
    for (const auto& set : binding_sets()) {
        for (const auto& rec : set.entries) {
            if (rec.is_bound()) {
                out.push_back(rec);
            }
        }
    }
    return out;
}

// ---- the engine's object namespace ---------------------------------------------------------------

namespace {

// Vtable slots on an input device, all confirmed by who calls them:
//   1 = ObjectChanged(id) -> bool     (LTInput_ObjectChanged reaches it at +4)
//   2 = Poll()                        (CLTInput_PollDevices, +8)
//   3 = GetObjectValue(id) -> float   (LTInput_GetObjectValue, +12)
//   4 = GetPreviousObjectValue(id)    (LTInput_GetPreviousObjectValue, +16)
constexpr size_t kSlotObjectChanged = 1;
constexpr size_t kSlotGetValue = 3;
constexpr size_t kSlotGetPreviousValue = 4;

using ValueFn = float(__thiscall*)(void*, int);
using ChangedFn = bool(__thiscall*)(void*, int);

// Resolve the device a given object id belongs to, refusing joystick ids for the reason stated in the
// header. Returns the device address, or 0.
uintptr_t device_for_object(int object_id) {
    switch (Input::classify_object(object_id)) {
    case Input::ObjectClass::Keyboard:
        return Input::device(Input::DeviceKind::Keyboard).transform([](const Input::Device& d) {
            return d.address;
        }).value_or(0);
    case Input::ObjectClass::Mouse:
        return Input::device(Input::DeviceKind::Mouse).transform([](const Input::Device& d) {
            return d.address;
        }).value_or(0);
    default:
        return 0;
    }
}

// Read a vtable slot, guarded: a half-built or freed device faults here rather than handing back a
// plausible function pointer.
uintptr_t device_slot(uintptr_t device, size_t slot) {
    if (device == 0) {
        return 0;
    }
    uint32_t vtable = 0;
    if (!seh_copy(&vtable, device, sizeof(vtable)) || vtable == 0) {
        return 0;
    }
    uint32_t fn = 0;
    if (!seh_copy(&fn, vtable + slot * sizeof(uint32_t), sizeof(fn)) || fn == 0) {
        return 0;
    }
    // The method must live inside the exe: these are engine-side classes, and a pointer elsewhere means
    // this is not the object this mapping describes.
    const auto* exe = Modules::get().exe();
    if (exe == nullptr || exe->base == 0 || fn < exe->base || fn >= exe->base + exe->size) {
        return 0;
    }
    return fn;
}

}  // namespace

std::vector<uintptr_t> Input::device_vtable_entries(DeviceKind kind) {
    std::vector<uintptr_t> out;
    const auto dev = device(kind);
    if (!dev.has_value() || dev->vtable == 0) {
        return out;
    }
    const auto* exe = Modules::get().exe();
    if (exe == nullptr || exe->base == 0) {
        return out;
    }
    out.reserve(kDeviceVtableSlots);
    for (size_t i = 0; i < kDeviceVtableSlots; ++i) {
        uint32_t fn = 0;
        if (!seh_copy(&fn, dev->vtable + i * sizeof(uint32_t), sizeof(fn))) {
            break;
        }
        // Every entry must be engine code; one outside the image would mean this is not the class the
        // mapping describes -- or that the table is shorter than 11 and we are reading past it.
        if (fn < exe->base || fn >= exe->base + exe->size) {
            break;
        }
        out.push_back(fn);
    }
    return out;
}

Input::ObjectClass Input::classify_object(int object_id) {
    // Mirrors LTInput_DeviceIndexForObject exactly, including the keyboard fallthrough.
    if (static_cast<unsigned>(object_id - 2000) <= 0x15u) {
        return ObjectClass::Joystick;
    }
    if (static_cast<unsigned>(object_id - 1000) <= 6u) {
        return ObjectClass::Mouse;
    }
    return ObjectClass::Keyboard;
}

std::optional<float> Input::object_value(int object_id) {
    const uintptr_t dev = device_for_object(object_id);
    const uintptr_t fn = device_slot(dev, kSlotGetValue);
    if (fn == 0) {
        return std::nullopt;
    }
    float out = 0.0f;
    bool ok = false;
    KANANLIB_SEH_TRY {
        out = reinterpret_cast<ValueFn>(fn)(reinterpret_cast<void*>(dev), object_id);
        ok = true;
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        return std::nullopt;
    }
    if (!ok) {
        return std::nullopt;
    }
    return out;
}

std::optional<float> Input::object_previous_value(int object_id) {
    const uintptr_t dev = device_for_object(object_id);
    const uintptr_t fn = device_slot(dev, kSlotGetPreviousValue);
    if (fn == 0) {
        return std::nullopt;
    }
    float out = 0.0f;
    bool ok = false;
    KANANLIB_SEH_TRY {
        out = reinterpret_cast<ValueFn>(fn)(reinterpret_cast<void*>(dev), object_id);
        ok = true;
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        return std::nullopt;
    }
    if (!ok) {
        return std::nullopt;
    }
    return out;
}

std::optional<bool> Input::object_changed(int object_id) {
    const uintptr_t dev = device_for_object(object_id);
    const uintptr_t fn = device_slot(dev, kSlotObjectChanged);
    if (fn == 0) {
        return std::nullopt;
    }
    bool out = false;
    bool ok = false;
    KANANLIB_SEH_TRY {
        out = reinterpret_cast<ChangedFn>(fn)(reinterpret_cast<void*>(dev), object_id);
        ok = true;
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        return std::nullopt;
    }
    if (!ok) {
        return std::nullopt;
    }
    return out;
}

// ---- the subclass window procedure ---------------------------------------------------------------

std::optional<Input::WndProcChain> Input::wndproc_chain() {
    const uintptr_t raw = main_window();
    if (raw == 0) {
        return std::nullopt;
    }
    const auto saved = read_exe<uint32_t>(kOriginalWndProc);
    if (!saved.has_value()) {
        return std::nullopt;
    }
    WndProcChain c{};
    c.current = static_cast<uintptr_t>(
        ::GetWindowLongW(reinterpret_cast<HWND>(raw), GWL_WNDPROC));
    c.subclass = exe_at(kSubclassWndProc);
    c.saved_original = *saved;
    c.engine_wndproc = exe_at(kEngineWndProc);
    c.engine_owns_window = c.current != 0 && c.current == c.subclass;
    c.saved_is_engine = c.saved_original != 0 && c.saved_original == c.engine_wndproc;

    // Which module the installed proc belongs to, through the shared Modules helper -- the same one
    // Render uses to name a COM implementation, since it is the same question.
    if (c.current != 0) {
        c.current_owner = Modules::owning_module_name(c.current).value_or(std::string{});
    }
    return c;
}

std::optional<bool> Input::input_is_enabled() {
    const auto v = read_exe<uint32_t>(kInputEnabled);
    if (!v.has_value()) {
        return std::nullopt;
    }
    return *v != 0;
}

uintptr_t Input::input_enabled_address() {
    return exe_at(kInputEnabled);
}

uintptr_t Input::device_array_address() {
    return exe_at(kCLTInputDevices);
}

uintptr_t Input::published_device_array() {
    return read_exe<uint32_t>(kPublishedDeviceArray).value_or(0);
}

bool Input::EntryPoints::all_resolved() const {
    const auto* exe = Modules::get().exe();
    if (exe == nullptr || exe->base == 0) {
        return false;
    }
    const uintptr_t lo = exe->base;
    const uintptr_t hi = exe->base + exe->size;
    for (const uintptr_t a : {translate_window_message, mouse_on_move, mouse_set_incoming_button,
                              keyboard_set_incoming_key, mouse_on_wheel, center_cursor}) {
        if (a < lo || a >= hi) {
            return false;
        }
    }
    return true;
}

Input::EntryPoints Input::entry_points() {
    EntryPoints e{};
    e.translate_window_message = exe_at(kTranslateWindowMessage);
    e.mouse_on_move = exe_at(kMouseOnMove);
    e.mouse_set_incoming_button = exe_at(kMouseSetIncomingButton);
    e.keyboard_set_incoming_key = exe_at(kKeyboardSetIncomingKey);
    e.mouse_on_wheel = exe_at(kMouseOnWheel);
    e.center_cursor = exe_at(kCenterCursor);
    return e;
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
