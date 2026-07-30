#include "Events.hpp"

#include "Memory.hpp"
#include "Modules.hpp"

namespace sdk {

const std::vector<Events::Event>& Events::all() {
    // Offsets are gameclient.dll-relative, taken from the dispatchers that reference each name literal.
    static const std::vector<Event> s_events = {
        // Player vitals -- the state a mod most often mirrors.
        {"HealthChanged", Category::Player, "d", 0x20780},
        {"HealthMaxChanged", Category::Player, "d", 0x207B0},
        {"ArmorChanged", Category::Player, "d", 0x208D0},
        {"ArmorMaxChanged", Category::Player, "d", 0x20900},
        {"PlayerAlive", Category::Player, "d", 0x20B70},
        {"OnPlayerStateChanged", Category::Player, "d", 0x9CC50},
        {"PlayerRemoved", Category::Player, "d", 0x20B10},
        {"DamageDirAdd", Category::Player, "d", 0x20E70},

        // Weapons and inventory.
        {"AmmoCountChanged", Category::Player, "sdd", 0x208A0},
        {"WeaponSelect", Category::Player, "s", 0x20840},
        {"WeaponRemove", Category::Player, "s", 0x20810},
        {"WeaponFireModeChanged", Category::Player, "d", 0x129660},
        {"WeaponSetHeatLevel", Category::Player, "f", 0x1185F0},
        {"GrenadeTypeChanged", Category::Player, "s", 0x20930},
        {"GrenadeCountChanged", Category::Player, "sdd", 0x20960},
        {"GearCountChanged", Category::Player, "sdd", 0x209C0},

        // Signature FEAR mechanics.
        {"SlowMoMaxChanged", Category::Player, "f", 0x209F0},
        {"FlashlightMaxChargeChanged", Category::Player, "f", 0x20750},

        // HUD state, which is where a stereo consumer has the most reason to interfere.
        {"ShowCrosshair", Category::Player, "b", 0x118620},
        {"UseWideScreen", Category::Player, "b", 0x20D20},
        {"SetMiniMapMode", Category::Player, "b", 0x118830},
        {"ScoreboardShow", Category::Player, "b", 0x1186B0},
        {"InitialHudState", Category::Player, "b", 0x118A60},
        {"SubtitleStrength", Category::Player, "f", 0x1189A0},
        {"LoadProgress", Category::Player, "d", 0x9CC20},
    };
    return s_events;
}

std::optional<Events::Event> Events::find(std::string_view name) {
    if (name.empty()) {
        return std::nullopt;
    }
    for (const auto& e : all()) {
        if (name == e.name) {
            return e;
        }
    }
    return std::nullopt;
}

uintptr_t Events::dispatcher(std::string_view name) {
    const auto e = find(name);
    if (!e.has_value()) {
        return 0;
    }
    const auto* gc = Modules::get().game_client();
    if (gc == nullptr || gc->base == 0) {
        return 0;
    }
    return gc->base + e->offset;
}

size_t Events::payload_arg_count(std::string_view payload) {
    return payload.size();
}

bool Events::payload_is_well_formed(std::string_view payload) {
    for (const char c : payload) {
        if (tag_for(c) == 0) {
            return false;
        }
    }
    return true;
}

uint32_t Events::tag_for(char letter) {
    switch (letter) {
    case 'b':
    case 'd':
        return kTagInt;
    case 'f':
        return kTagFloat;
    case 's':
        return kTagString;
    case 'w':
        return kTagWideString;
    default:
        return 0;
    }
}

uint32_t Events::value_type_for(char letter) {
    switch (letter) {
    case 'b':
        return 2;
    case 'd':
    case 'f':
        return 3;
    case 's':
        return 4;
    case 'w':
        return 5;
    default:
        return 0;
    }
}

std::optional<size_t> Events::payload_stack_bytes(std::string_view payload) {
    if (!payload_is_well_formed(payload)) {
        return std::nullopt;
    }
    size_t total = 0;
    for (const char c : payload) {
        // A float is promoted to double by the variadic sender; everything else is one 4-byte slot -- an int,
        // a widened bool, or a string POINTER.
        total += (c == 'f') ? sizeof(double) : sizeof(uint32_t);
    }
    return total;
}

std::optional<size_t> Events::frame_bytes(std::string_view payload) {
    const auto payload_bytes = payload_stack_bytes(payload);
    if (!payload_bytes.has_value()) {
        return std::nullopt;
    }
    (void)payload_bytes;
    // One tag PER ARGUMENT, then a single terminator.
    size_t total = kHeaderBytes + kTerminatorBytes;
    for (const char c : payload) {
        total += kTagBytes + ((c == 'f') ? sizeof(double) : sizeof(uint32_t));
    }
    return total;
}

bool Events::verify(const Event& event) {
    const auto at = dispatcher(event.name);
    if (at == 0) {
        return false;
    }

    // A dispatcher is a single call with its arguments pushed as immediates, so the name pointer appears
    // within the first few dozen bytes. Every 4-byte window is treated as a candidate pointer; one of them
    // must spell the event's name.
    constexpr size_t kWindow = 96;
    const std::string_view want{event.name};
    for (size_t i = 0; i + sizeof(uint32_t) <= kWindow; ++i) {
        const auto candidate = mem::read_ptr(at + i);
        if (!candidate.has_value() || *candidate < 0x10000) {
            continue;
        }
        // Minimum length 1: some event names are short, and this reader must not impose a floor the binary
        // does not have.
        const auto text = mem::read_name(*candidate, 64, 1);
        if (text.has_value() && *text == want) {
            return true;
        }
    }
    return false;
}

std::string Events::as_interface_name(Category category) {
    // "Monolith.I%sEvents" with the category in the middle -- the sender's error string is the source of this
    // shape, not a guess about naming conventions.
    std::string out = "Monolith.I";
    out += (category == Category::Menu) ? "Menu" : "Player";
    out += "Events";
    return out;
}

std::string Events::as_method_name(std::string_view path, std::string_view event_name) {
    if (path.empty()) {
        return {};  // exactly what the sender refuses, with a log line about a missing implementation path
    }
    std::string out{path};
    out += '.';
    out += event_name.empty() ? kDefaultMethod : std::string{event_name};
    return out;
}

namespace {

// The Hungarian body of a global's name, i.e. what follows "_global.g_" or a bare "g_".
std::string_view hungarian_body(std::string_view variable) {
    constexpr std::string_view kQualified = "_global.g_";
    constexpr std::string_view kBare = "g_";
    if (variable.size() > kQualified.size() && variable.compare(0, kQualified.size(), kQualified) == 0) {
        return variable.substr(kQualified.size());
    }
    if (variable.size() > kBare.size() && variable.compare(0, kBare.size(), kBare) == 0) {
        return variable.substr(kBare.size());
    }
    return {};
}

// A prefix counts only when the next character is upper case, so "nMonolith" matches 'n' and a name that
// merely starts with the letter does not.
bool prefix_matches(std::string_view body, std::string_view prefix) {
    return body.size() > prefix.size() && body.compare(0, prefix.size(), prefix) == 0 &&
           body[prefix.size()] >= 'A' && body[prefix.size()] <= 'Z';
}

}  // namespace

bool Events::variable_is_array(std::string_view variable) {
    const auto body = hungarian_body(variable);
    // Two-letter array prefixes are tested first: "as" would otherwise match the scalar 'a' reading.
    for (const auto p : {"as", "an", "ab", "af"}) {
        if (prefix_matches(body, p)) {
            return true;
        }
    }
    return false;
}

char Events::type_letter_for_variable(std::string_view variable) {
    const auto body = hungarian_body(variable);
    if (body.empty()) {
        return 0;
    }
    // Arrays first, for the same reason.
    if (prefix_matches(body, "an")) {
        return 'd';
    }
    if (prefix_matches(body, "as")) {
        return 's';
    }
    if (prefix_matches(body, "ab")) {
        return 'b';
    }
    if (prefix_matches(body, "af")) {
        return 'f';
    }
    if (prefix_matches(body, "n")) {
        return 'd';
    }
    if (prefix_matches(body, "b")) {
        return 'b';
    }
    if (prefix_matches(body, "s")) {
        return 's';
    }
    if (prefix_matches(body, "f")) {
        return 'f';
    }
    return 0;
}

size_t Events::setter_slot_for_variable(std::string_view variable) {
    if (type_letter_for_variable(variable) == 0) {
        return 0;
    }
    return variable_is_array(variable) ? kSetVariableArraySlot : kSetVariableSlot;
}

const std::vector<Events::UiPanel>& Events::ui_panels() {
    static const std::vector<UiPanel> s_panels = {
        {"Multiplayer", 59, 0x13380, 0x1F7558, 0x1F6E98},
        {"Player", 34, 0x26C40, 0x1F8C24, 0x1F8378},
        {"Menu", 23, 0xCE60, 0x1F6D6C, 0x1F6AF0},
        {"Performance", 17, 0x1EC50, 0x1F7EE8, 0x1F7D98},
        {"SaveGame", 15, 0x29060, 0x1F8D0C, 0x1F8C28},
        {"OnlineLogin", 14, 0x1AD40, 0x1F7A7C, 0x1F7980},
        {"Global", 13, 0x5860, 0x1F67CC, 0x1F66B8},
        {"Options", 13, 0x1D080, 0x1F7CB4, 0x1F7A80},
        {"MultiplayerHost", 12, 0x18320, 0x1F7734, 0x1F7560},
        {"MultiplayerJoin", 11, 0x19260, 0x1F7964, 0x1F7838},
        {"KeyBindings", 9, 0x8790, 0x1F6964, 0x1F68B0},
        {"WorldInterface", 6, 0x2A9D0, 0x1F8E5C, 0x1F8DF0},
        {"ControlPanel", 5, 0x4670, 0x1F6520, 0x1F64C0},
        {"LayoutPanel", 4, 0x8AC0, 0x1F69C8, 0x1F6968},
        {"SecurityPanel", 3, 0x29310, 0x1F8D78, 0x1F8D18},
        {"SystemLayer", 2, 0x29480, 0x1F8DBC, 0x1F8D80},
        {"WeaponDisplay", 2, 0x2A800, 0x1F8DE8, 0x1F8DC4},
    };
    return s_panels;
}

std::optional<Events::UiPanel> Events::find_panel(std::string_view name) {
    if (name.empty()) {
        return std::nullopt;
    }
    for (const auto& p : ui_panels()) {
        if (name == p.name) {
            return p;
        }
    }
    return std::nullopt;
}

uintptr_t Events::panel_dispatch(std::string_view name) {
    const auto p = find_panel(name);
    if (!p.has_value()) {
        return 0;
    }
    const auto* gc = Modules::get().game_client();
    if (gc == nullptr || gc->base == 0) {
        return 0;
    }
    return gc->base + p->dispatch_offset;
}

Events::BindingRole Events::binding_role_for_kind(uint8_t kind) {
    switch (kind) {
    case 0:
    case 1:
        return BindingRole::Lifecycle;
    case 2:
    case 3:
    case 5:
    case 6:
        return BindingRole::FlashToGame;
    case 7:
    case 11:  // Game_Player_ShowSubtitle, the one Game_* entry not on kind 7
        return BindingRole::GameToFlash;
    default:
        // 12..21 are all _global variable entries; the kind further encodes the element type, which is not
        // pinned (see the header). Anything outside the observed range stays Unknown rather than being folded
        // in, so a build with a new kind is visible instead of silently classified.
        if (kind >= 12 && kind <= 21) {
            return BindingRole::GlobalSetter;
        }
        return BindingRole::Unknown;
    }
}

bool Events::panel_table_initialised(std::string_view panel) {
    const auto p = find_panel(panel);
    if (!p.has_value()) {
        return false;
    }
    const auto* gc = Modules::get().game_client();
    if (gc == nullptr || gc->base == 0) {
        return false;
    }
    const auto guard = mem::read_u32(gc->base + p->guard_offset);
    return guard.has_value() && (*guard & 1u) != 0;
}

std::vector<Events::PanelBinding> Events::panel_bindings(std::string_view panel) {
    std::vector<PanelBinding> out;
    const auto p = find_panel(panel);
    if (!p.has_value() || !panel_table_initialised(panel)) {
        return out;  // an uninitialised table is all nulls; reading it would invent bindings
    }
    const auto* gc = Modules::get().game_client();
    if (gc == nullptr || gc->base == 0) {
        return out;
    }

    const auto table = gc->base + p->table_offset;
    // Generous but bounded: the largest panel observed holds around 60 entries.
    constexpr size_t kMaxEntries = 256;
    for (size_t i = 0; i < kMaxEntries; ++i) {
        const auto at = table + i * kBindingEntryBytes;
        const auto name_ptr = mem::read_ptr(at);
        if (!name_ptr.has_value() || *name_ptr == 0) {
            break;  // the null name IS the terminator
        }
        auto name = mem::read_name(*name_ptr, 96, 1);
        if (!name.has_value()) {
            break;
        }
        PanelBinding b{};
        b.name = std::move(*name);
        b.handler = mem::read_ptr(at + 4).value_or(0);
        b.kind = mem::read_u8(at + 8).value_or(0);
        b.role = binding_role_for_kind(b.kind);
        out.push_back(std::move(b));
    }
    return out;
}

bool Events::verify_panel(const UiPanel& panel) {
    const auto at = panel_dispatch(panel.name);
    if (at == 0) {
        return false;
    }

    // A dispatcher references its panel's method literals throughout, so a window rather than a fixed offset,
    // and a PREFIX rather than a specific method.
    constexpr size_t kWindow = 2048;
    std::string prefix{panel.name};
    prefix += '.';

    for (size_t i = 0; i + sizeof(uint32_t) <= kWindow; ++i) {
        const auto candidate = mem::read_ptr(at + i);
        if (!candidate.has_value() || *candidate < 0x10000) {
            continue;
        }
        const auto text = mem::read_name(*candidate, 96, 1);
        if (text.has_value() && text->size() > prefix.size() &&
            text->compare(0, prefix.size(), prefix) == 0) {
            return true;
        }
    }
    return false;
}

size_t Events::verified_panel_count() {
    size_t n = 0;
    for (const auto& p : ui_panels()) {
        if (verify_panel(p)) {
            ++n;
        }
    }
    return n;
}

size_t Events::verified_count() {
    size_t n = 0;
    for (const auto& e : all()) {
        if (verify(e)) {
            ++n;
        }
    }
    return n;
}

uint32_t Events::gfx_slot_for_kind(uint8_t kind) {
    if (kind >= 12 && kind <= 16) {
        return 9; // GFx SetVariable
    }
    if (kind >= 17 && kind <= 21) {
        return 11; // GFx SetVariableArray
    }
    return 0;
}

const char* Events::prefix_for_kind(uint8_t kind) {
    switch (kind) {
    case 12: return "g_f";
    case 13: return "g_s";  // 13 and 14 both hold strings; the split is not the GFx type
    case 14: return "g_s";
    case 15: return "g_n";
    case 16: return "g_b";
    case 17: return "g_af";
    case 18: return "g_as"; // as with 13/14, 18 and 19 both hold string arrays
    case 19: return "g_as";
    case 20: return "g_an";
    case 21: return "g_ab";
    default: return nullptr;
    }
}

std::vector<Events::GlobalVariable> Events::global_variables() {
    std::vector<GlobalVariable> out;
    const auto* gc = Modules::get().game_client();
    if (gc == nullptr || gc->base == 0) {
        return out;
    }
    for (const auto& panel : ui_panels()) {
        for (const auto& b : panel_bindings(panel.name)) {
            // Role AND prefix must agree. The role comes from the kind byte and the prefix from the name, so
            // requiring both refuses an entry whose kind says "variable" while its name says otherwise instead
            // of reporting a setter that would be called with the wrong argument shape.
            if (b.role != BindingRole::GlobalSetter || b.handler == 0) {
                continue;
            }
            if (b.name.rfind("_global.", 0) != 0) {
                continue;
            }
            GlobalVariable v;
            v.name = b.name;
            v.kind = b.kind;
            v.gfx_slot = gfx_slot_for_kind(b.kind);
            v.is_array = v.gfx_slot == 11;
            v.handler = b.handler;
            out.push_back(std::move(v));
        }
    }
    return out;
}

std::optional<Events::GlobalVariable> Events::find_global(std::string_view name) {
    for (auto& v : global_variables()) {
        if (v.name == name) {
            return v;
        }
    }
    return std::nullopt;
}

}  // namespace sdk
