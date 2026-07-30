#include "UiCommands.hpp"

#include <algorithm>
#include <cctype>

#include <windows.h>

#include <utility/Scan.hpp>

#include "Memory.hpp"
#include "Modules.hpp"

namespace sdk {

namespace {

// The anchor. Any name in the table would do; this one is chosen because it is the action this project needs
// most (reload the last checkpoint from the front end) so a build that dropped it should fail loudly here
// rather than silently resolve a table missing the row we came for.
constexpr const char* kAnchorName = "Menu.StartCheckpoint";

bool in_game_client(uintptr_t address) {
    const auto* gc = Modules::get().game_client();
    if (gc == nullptr || gc->base == 0 || gc->size == 0) {
        return false;
    }
    return address >= gc->base && address < gc->base + gc->size;
}

// A row is {name, handler, flag}: both pointers must land inside gameclient's image and the name must read as
// a plausible command string. That triple is what bounds the walk -- there is no count field.
bool row_is_plausible(uintptr_t entry, std::string& name_out, uintptr_t& handler_out, uint8_t& flag_out) {
    const auto name_ptr = mem::read<uint32_t>(entry);
    const auto handler = mem::read<uint32_t>(entry + 4);
    const auto flag = mem::read<uint8_t>(entry + 8);
    if (!name_ptr.has_value() || !handler.has_value() || !flag.has_value()) {
        return false;
    }
    if (!in_game_client(*name_ptr) || !in_game_client(*handler)) {
        return false;
    }
    char buf[96]{};
    if (!mem::copy(buf, static_cast<uintptr_t>(*name_ptr), sizeof(buf) - 1)) {
        return false;
    }
    buf[sizeof(buf) - 1] = '\0';
    const size_t len = strnlen(buf, sizeof(buf) - 1);
    if (len == 0) {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        const auto c = static_cast<unsigned char>(buf[i]);
        if (c < 0x20 || c > 0x7E) {
            return false;
        }
    }
    name_out.assign(buf, len);
    handler_out = static_cast<uintptr_t>(*handler);
    flag_out = *flag;
    return true;
}

}  // namespace

uintptr_t UiCommands::table_address() {
    static const uintptr_t s_base = []() -> uintptr_t {
        const auto* gc = Modules::get().game_client();
        if (gc == nullptr || gc->handle == nullptr) {
            return 0;
        }
        // kananlib does both halves: find the literal, then find the pointer to it. Hand-rolling either would
        // be the image-scan mistake AGENT.MD rule 6 already records.
        const auto str = utility::scan_string(gc->handle, kAnchorName, true);
        if (!str.has_value()) {
            return 0;
        }
        const auto slot = utility::scan_ptr(gc->handle, *str);
        if (!slot.has_value()) {
            return 0;
        }
        // `slot` is that row's NAME field. Walk backwards while rows stay plausible to reach row zero.
        uintptr_t first = *slot;
        for (size_t i = 0; i < kMaxCommands; ++i) {
            const uintptr_t prev = first - kEntryStride;
            std::string n;
            uintptr_t h = 0;
            uint8_t f = 0;
            if (!row_is_plausible(prev, n, h, f)) {
                break;
            }
            first = prev;
        }
        return first;
    }();
    return s_base;
}

std::vector<UiCommands::Command> UiCommands::all() {
    std::vector<Command> out;
    const uintptr_t base = table_address();
    if (base == 0) {
        return out;
    }
    for (size_t i = 0; i < kMaxCommands; ++i) {
        const uintptr_t entry = base + i * kEntryStride;
        Command c{};
        if (!row_is_plausible(entry, c.name, c.handler, c.flag)) {
            break;  // the walk ends where the rows stop being rows; there is no terminator to trust
        }
        c.entry = entry;
        out.push_back(std::move(c));
    }
    return out;
}

std::optional<UiCommands::Command> UiCommands::find(std::string_view name) {
    std::string wanted{name};
    std::transform(wanted.begin(), wanted.end(), wanted.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (auto& c : all()) {
        std::string have = c.name;
        std::transform(have.begin(), have.end(), have.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (have == wanted) {
            return c;
        }
    }
    return std::nullopt;
}

bool UiCommands::invoke(std::string_view name) {
    const auto cmd = find(name);
    if (!cmd.has_value() || cmd->handler == 0) {
        return false;
    }
    // The handlers examined ignore all three parameters; they are the UI's panel/event arguments and the load
    // actions never read them.
    cmd->as_handler()(0, 0, 0);
    return true;
}

}  // namespace sdk
