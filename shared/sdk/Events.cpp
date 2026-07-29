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
        if (c != 'd' && c != 'f' && c != 's' && c != 'b') {
            return false;
        }
    }
    return true;
}

std::optional<size_t> Events::payload_stack_bytes(std::string_view payload) {
    if (!payload_is_well_formed(payload)) {
        return std::nullopt;
    }
    // Every one of d/f/s/b occupies a 4-byte slot in a 32-bit cdecl call: an int, a float promoted to no
    // wider than its own size, a bool widened, or a string POINTER.
    return payload.size() * sizeof(uint32_t);
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

size_t Events::verified_count() {
    size_t n = 0;
    for (const auto& e : all()) {
        if (verify(e)) {
            ++n;
        }
    }
    return n;
}

}  // namespace sdk
