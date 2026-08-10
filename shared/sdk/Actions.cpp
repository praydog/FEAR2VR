#include "Actions.hpp"

#include <windows.h>

#include <shlobj.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string.h>
#include <unordered_map>

#include "Log.hpp"

#pragma comment(lib, "shell32.lib")

namespace fs = std::filesystem;

namespace sdk {
namespace {

// Mouse buttons share SyntheticInput's key table, encoded above the virtual-key range. Kept in step
// with SyntheticInput's own kMouseButtonBase; a mismatch would silently press the wrong thing.
constexpr uint32_t kMouseButtonBase = 0x100;

// ---- THE ENGINE'S INPUT OBJECT NAMES -----------------------------------------------------------
//
// LithTech stores a binding's input as a printable name -- "Key Space", "Mouse Button1" -- rather
// than a scancode, so this table is the whole decode. Names are exactly as they appear in a profile.
//
// Anything absent decodes to 0 and is reported as unbound rather than guessed at: an axis
// ("Mouse Axis X") and a wheel ("Mouse Wheel Up") have no press/release form, and inventing one
// would be the same class of mistake as the hardcoded keys this replaces.
const std::unordered_map<std::string, uint32_t>& name_table() {
    static const std::unordered_map<std::string, uint32_t> t = {
        {"Key Space", VK_SPACE},       {"Key Return", VK_RETURN},
        {"Key Escape", VK_ESCAPE},     {"Key Tab", VK_TAB},
        {"Key Back", VK_BACK},         {"Key Delete", VK_DELETE},
        {"Key Insert", VK_INSERT},     {"Key Home", VK_HOME},
        {"Key End", VK_END},           {"Key Prior", VK_PRIOR},
        {"Key Next", VK_NEXT},         {"Key Up", VK_UP},
        {"Key Down", VK_DOWN},         {"Key Left", VK_LEFT},
        {"Key Right", VK_RIGHT},
        // UNSIDED, deliberately. The engine's keyboard array is indexed by the generic virtual key
        // -- holding VK_SHIFT sets the sprint move flag while VK_LSHIFT does not, which this
        // project measured the hard way when sprint silently did nothing.
        {"Key Shift", VK_SHIFT},       {"Key Control", VK_CONTROL},
        {"Key Menu", VK_MENU},
        {"Mouse Button1", kMouseButtonBase + 0},
        {"Mouse Button2", kMouseButtonBase + 1},
        {"Mouse Button3", kMouseButtonBase + 2},
        {"Mouse Button4", kMouseButtonBase + 3},
        {"Mouse Button5", kMouseButtonBase + 4},
    };
    return t;
}

uint32_t decode_object(const std::string& name) {
    const auto& t = name_table();
    const auto it = t.find(name);
    if (it != t.end()) {
        return it->second;
    }
    // "Key A".."Key Z" and "Key 0".."Key 9" are their own virtual keys, so they need no table.
    if (name.size() == 5 && name.compare(0, 4, "Key ") == 0) {
        const char c = name[4];
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            return static_cast<uint32_t>(c);
        }
        if (c >= 'a' && c <= 'z') {
            return static_cast<uint32_t>(c - 'a' + 'A');
        }
    }
    if (name.compare(0, 10, "Key NumPad") == 0 && name.size() == 11) {
        const char c = name[10];
        if (c >= '0' && c <= '9') {
            return VK_NUMPAD0 + static_cast<uint32_t>(c - '0');
        }
    }
    if (name.size() >= 5 && name.compare(0, 5, "Key F") == 0) {
        const int n = std::atoi(name.c_str() + 5);
        if (n >= 1 && n <= 24) {
            return VK_F1 + static_cast<uint32_t>(n - 1);
        }
    }
    return 0;  // axis, wheel, gamepad object, or a name this build does not use
}

// ---- FINDING THE PROFILE -----------------------------------------------------------------------
//
// Documents\WBGames\FEAR2\<steam id>\Game.ini names the active profile, and the bindings live in
// Profiles\<name>.ltprofile beside it. The id folder is per-account, so pick the one whose Game.ini
// was written most recently rather than assuming a single account has ever played.
fs::path documents_dir() {
    wchar_t buf[MAX_PATH]{};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, SHGFP_TYPE_CURRENT, buf))) {
        return fs::path(buf);
    }
    const wchar_t* up = _wgetenv(L"USERPROFILE");
    return up != nullptr ? fs::path(up) / L"Documents" : fs::path{};
}

fs::path find_profile() {
    std::error_code ec;
    const fs::path root = documents_dir() / L"WBGames" / L"FEAR2";
    if (root.empty() || !fs::exists(root, ec)) {
        return {};
    }

    fs::path best;
    fs::file_time_type best_time{};
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (!entry.is_directory(ec)) {
            continue;
        }
        const fs::path ini = entry.path() / L"Game.ini";
        if (!fs::exists(ini, ec)) {
            continue;
        }
        const auto when = fs::last_write_time(ini, ec);
        if (ec) {
            continue;
        }
        if (best.empty() || when > best_time) {
            best = ini;
            best_time = when;
        }
    }
    if (best.empty()) {
        return {};
    }

    std::string profile_name = "Profile000";
    std::ifstream in(best);
    std::string line;
    while (std::getline(in, line)) {
        const auto eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        std::string key = line.substr(0, eq);
        key.erase(std::remove_if(key.begin(), key.end(), [](unsigned char c) {
                      return std::isspace(c) != 0;
                  }),
                  key.end());
        if (_stricmp(key.c_str(), "ProfileName") == 0) {
            profile_name = line.substr(eq + 1);
            while (!profile_name.empty() &&
                   (profile_name.back() == '\r' || profile_name.back() == '\n' ||
                    profile_name.back() == ' ')) {
                profile_name.pop_back();
            }
            break;
        }
    }

    const fs::path p = best.parent_path() / L"Profiles" / (profile_name + ".ltprofile");
    return fs::exists(p, ec) ? p : fs::path{};
}

// ---- THE BINDING TABLE -------------------------------------------------------------------------
//
// Serialised as a uint32 record count followed by that many {asciiz object name, uint32 command}
// pairs. The table's offset is not fixed, so it is FOUND rather than assumed: the count sits four
// bytes before the first record's name, and a candidate is only accepted if the whole run parses
// into plausible records. Guessing an offset would break on the first profile that differs.
bool parse_table(const std::vector<uint8_t>& d, size_t name_at,
                 std::vector<Actions::Binding>* out) {
    if (name_at < 4) {
        return false;
    }
    size_t off = name_at - 4;
    uint32_t count = 0;
    std::memcpy(&count, d.data() + off, 4);
    if (count < 8 || count > 4096) {
        return false;
    }
    off += 4;

    std::vector<Actions::Binding> got;
    got.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        const size_t start = off;
        while (off < d.size() && d[off] != 0) {
            ++off;
        }
        if (off >= d.size() || off + 5 > d.size()) {
            return false;
        }
        std::string name(reinterpret_cast<const char*>(d.data() + start), off - start);
        ++off;  // the terminator
        uint32_t cmd = 0;
        std::memcpy(&cmd, d.data() + off, 4);
        off += 4;

        // Every record is an input object name and a command id in range. Anything else means the
        // candidate offset was not the table.
        if (name.empty() || name.size() > 64 || cmd > 4096) {
            return false;
        }
        if (name.compare(0, 4, "Key ") != 0 && name.compare(0, 6, "Mouse ") != 0 &&
            name.compare(0, 9, "Joystick ") != 0 && name.compare(0, 8, "Gamepad ") != 0) {
            return false;
        }
        got.push_back({cmd, name, decode_object(name)});
    }
    *out = std::move(got);
    return true;
}

struct Cache {
    std::mutex lock;
    bool tried{false};
    bool ok{false};
    std::string path;
    fs::file_time_type stamp{};
    std::vector<Actions::Binding> bindings;
};

Cache& cache() {
    static Cache c;
    return c;
}

void load_locked(Cache& c) {
    c.tried = true;
    c.ok = false;
    c.bindings.clear();

    const fs::path p = find_profile();
    if (p.empty()) {
        LOGX("[actions] no FEAR2 profile found -- actions cannot be resolved to inputs");
        c.path.clear();
        return;
    }
    c.path = p.string();

    std::error_code ec;
    c.stamp = fs::last_write_time(p, ec);

    std::ifstream in(p, std::ios::binary);
    if (!in) {
        LOGX("[actions] profile %s could not be opened", c.path.c_str());
        return;
    }
    const std::vector<uint8_t> d((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());

    // Walk every plausible first-record position rather than trusting one offset.
    for (size_t i = 4; i + 8 < d.size(); ++i) {
        const bool key = std::memcmp(d.data() + i, "Key ", 4) == 0;
        const bool mouse = i + 6 < d.size() && std::memcmp(d.data() + i, "Mouse ", 6) == 0;
        if (!key && !mouse) {
            continue;
        }
        if (parse_table(d, i, &c.bindings)) {
            c.ok = true;
            LOGX("[actions] %zu bindings from %s", c.bindings.size(), c.path.c_str());
            return;
        }
    }
    LOGX("[actions] %s held no recognisable binding table", c.path.c_str());
}

void ensure_locked(Cache& c) {
    if (!c.tried) {
        load_locked(c);
        return;
    }
    // A rebind rewrites the profile; noticing costs one stat and keeps a long session correct.
    if (c.ok && !c.path.empty()) {
        std::error_code ec;
        const auto now = fs::last_write_time(fs::path(c.path), ec);
        if (!ec && now != c.stamp) {
            LOGX("[actions] profile changed on disk -- re-reading bindings");
            load_locked(c);
        }
    }
}

}  // namespace

std::optional<uint32_t> Actions::input_for(Action action) {
    auto& c = cache();
    std::lock_guard<std::mutex> g(c.lock);
    ensure_locked(c);
    if (!c.ok) {
        return std::nullopt;
    }

    const auto want = static_cast<uint32_t>(action);

    // A command can carry SEVERAL bindings -- FEAR2 ships sprint on both Alt and Shift -- and the
    // rest are usually an axis or a wheel, which have no press form. Take the first that decodes,
    // with one exception: VK_MENU is deprioritised because Windows acts on Alt itself (it opens the
    // window menu and can cost focus), so holding it to sprint would fight the OS. It is still used
    // when nothing else is bound, since a working action beats a tidy one.
    std::optional<uint32_t> alt_only;
    for (const auto& b : c.bindings) {
        if (b.command != want || b.input == 0) {
            continue;
        }
        if (b.input == VK_MENU) {
            if (!alt_only.has_value()) {
                alt_only = b.input;
            }
            continue;
        }
        return b.input;
    }
    return alt_only;
}

std::optional<uint32_t> Actions::weapon_slot_input(unsigned slot) {
    // 1..4 are commands 30..33 and 5..8 are 40..43. The gap is the engine's own, confirmed in a
    // live profile (Key 1->30, Key 4->33, Key 5->40, Key 8->43), and it is why this is not a plain
    // base + slot. WeaponMgr::slot_virtual_key returns '0' + slot instead, which is the default
    // binding rather than the player's -- this is the portable answer to the same question.
    if (slot >= 1 && slot <= 4) {
        return input_for(static_cast<Action>(30 + (slot - 1)));
    }
    if (slot >= 5 && slot <= 8) {
        return input_for(static_cast<Action>(40 + (slot - 5)));
    }
    return std::nullopt;
}

std::vector<Actions::Binding> Actions::all() {
    auto& c = cache();
    std::lock_guard<std::mutex> g(c.lock);
    ensure_locked(c);
    return c.bindings;
}

std::string Actions::source_path() {
    auto& c = cache();
    std::lock_guard<std::mutex> g(c.lock);
    ensure_locked(c);
    return c.path;
}

void Actions::invalidate() {
    auto& c = cache();
    std::lock_guard<std::mutex> g(c.lock);
    c.tried = false;
}

bool Actions::loaded() {
    auto& c = cache();
    std::lock_guard<std::mutex> g(c.lock);
    ensure_locked(c);
    return c.ok;
}

}  // namespace sdk
