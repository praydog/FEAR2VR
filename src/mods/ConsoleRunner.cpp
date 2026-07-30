#include "ConsoleRunner.hpp"

#include <atomic>
#include <cstring>

#include "sdk/Console.hpp"
#include "sdk/UiCommands.hpp"

#include "Log.hpp"
#include "RenderHook.hpp"

namespace {

// Fixed storage, no allocation on the game thread. A slot is claimed by setting `used`; the text is written
// before that flag is published, so the drain never reads a half-filled line.
struct Entry {
    char text[ConsoleRunner::kMaxCommandLength]{};
    std::atomic<bool> used{false};
};

Entry g_queue[ConsoleRunner::kMaxQueued];
std::atomic<uint64_t> g_executed{0};
std::atomic<uint8_t> g_last{static_cast<uint8_t>(ConsoleRunner::Outcome::None)};
std::atomic<bool> g_registered{false};
char g_last_command[ConsoleRunner::kMaxCommandLength]{};

void present_callback() {
    ConsoleRunner::get().run_pending_on_game_thread();
}

}  // namespace

ConsoleRunner& ConsoleRunner::get() {
    static ConsoleRunner s_instance;
    return s_instance;
}

std::optional<std::string> ConsoleRunner::on_initialize() {
    // THE FRAME BOUNDARY IS THE TICK. RenderHook's callback runs on the render thread at the main menu and in
    // play; Mods::on_frame runs on neither at the menu. Registering here also exercises that extension point,
    // which is the reason it exists.
    if (!RenderHook::get().add_present_callback(&present_callback)) {
        return std::string{"could not register a present callback -- commands would never execute"};
    }
    g_registered.store(true, std::memory_order_release);
    LOGX("[console] runner attached to the frame boundary");
    return std::nullopt;
}

bool ConsoleRunner::queue(std::string_view command_line) {
    if (command_line.empty() || command_line.size() >= kMaxCommandLength) {
        return false;
    }
    for (auto& e : g_queue) {
        if (e.used.load(std::memory_order_acquire)) {
            continue;
        }
        memcpy(e.text, command_line.data(), command_line.size());
        e.text[command_line.size()] = '\0';
        // Published last: the drain keys on `used`, so the text must be complete before it becomes visible.
        e.used.store(true, std::memory_order_release);
        LOGX("[console] queued \"%s\"", e.text);
        return true;
    }
    return false;
}

void ConsoleRunner::run_pending_on_game_thread() {
    for (auto& e : g_queue) {
        if (!e.used.load(std::memory_order_acquire)) {
            continue;
        }

        // Copied out before the slot is released, so a caller queueing again cannot rewrite the buffer while
        // the handler is reading argv.
        char line[kMaxCommandLength];
        memcpy(line, e.text, sizeof(line));
        line[kMaxCommandLength - 1] = '\0';
        e.used.store(false, std::memory_order_release);

        // Split on spaces in place. argv[0] is the command name, which is how the engine's own dispatcher
        // builds the vector.
        char* argv[8]{};
        int argc = 0;
        char* p = line;
        while (*p != '\0' && argc < 8) {
            while (*p == ' ') {
                *p++ = '\0';
            }
            if (*p == '\0') {
                break;
            }
            argv[argc++] = p;
            while (*p != '\0' && *p != ' ') {
                ++p;
            }
        }
        if (argc == 0) {
            continue;
        }

        memcpy(g_last_command, argv[0], strlen(argv[0]) + 1);

        // THE UI TABLE FIRST. Its names are namespaced ("Menu.StartCheckpoint"), so they cannot collide with
        // a console command, and it is the table that actually drives the front end -- the console's
        // similar-looking LoadCheckpoint refuses to run outside a loaded world.
        if (sdk::UiCommands::invoke(argv[0])) {
            g_executed.fetch_add(1, std::memory_order_relaxed);
            g_last.store(static_cast<uint8_t>(Outcome::Ran), std::memory_order_relaxed);
            LOGX("[console] ran UI command \"%s\"", argv[0]);
            continue;
        }

        // Case-insensitive, because a caller types a command the way a person would. find() is the exact form
        // and is the right one when a specific entry is meant; here the friendlier match is correct.
        const auto cmd = sdk::Console::find_insensitive(argv[0]);
        if (!cmd.has_value()) {
            g_last.store(static_cast<uint8_t>(Outcome::NotFound), std::memory_order_relaxed);
            LOGX("[console] \"%s\" is not in the live registry", argv[0]);
            continue;
        }
        if (cmd->handler == 0) {
            g_last.store(static_cast<uint8_t>(Outcome::NoHandler), std::memory_order_relaxed);
            continue;
        }

        LOGX("[console] running \"%s\" (%d arg(s), handler in %s)", argv[0], argc, cmd->module.c_str());
        cmd->as_handler()(argc, argv);
        g_executed.fetch_add(1, std::memory_order_relaxed);
        g_last.store(static_cast<uint8_t>(Outcome::Ran), std::memory_order_relaxed);
    }
}

ConsoleRunner::State ConsoleRunner::state() const {
    State out;
    for (const auto& e : g_queue) {
        if (e.used.load(std::memory_order_acquire)) {
            ++out.pending;
        }
    }
    out.executed = g_executed.load(std::memory_order_relaxed);
    out.last = static_cast<Outcome>(g_last.load(std::memory_order_relaxed));
    out.last_command = g_last_command;
    out.callback_registered = g_registered.load(std::memory_order_acquire);
    return out;
}
