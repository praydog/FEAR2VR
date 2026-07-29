#include "Console.hpp"

#include <algorithm>
#include <cctype>

#include "Memory.hpp"
#include "Modules.hpp"

namespace sdk {

namespace {

// The source descriptor the console initialiser fills in. Its layout is the initialiser's own writes:
//     +0x00 variable table    +0x04 variable count
//     +0x08 command table     +0x0C command count
//     +0x10 link.prev         +0x14 link.next
constexpr uintptr_t kSourceBlockOffset = 0x2ED4A0;
constexpr uintptr_t kBlockCmdTable = 0x08;
constexpr uintptr_t kBlockCmdCount = 0x0C;
constexpr uintptr_t kBlockListHead = 0x10;

// Live object fields, from the registrar.
constexpr uintptr_t kObjName = 0x00;
constexpr uintptr_t kObjTarget = 0x04;
constexpr uintptr_t kObjLink = 0x08;
constexpr uintptr_t kObjLinkNext = 0x0C;
constexpr uintptr_t kObjOwner = 0x10;
constexpr uintptr_t kObjFlags = 0x14;

// Descriptor fields.
constexpr uintptr_t kDescName = 0x00;
constexpr uintptr_t kDescTarget = 0x04;
constexpr uintptr_t kDescTag = 0x08;

// Command names are short and never contain punctuation beyond letters and digits, but the floor that
// matters is the length one: a single stray printable byte is what random memory produces. Two is low enough
// for the shortest real name in the registry ("IF", "SET", "OBJ") and high enough to reject noise.
constexpr size_t kMinCommandName = 2;
constexpr size_t kMaxCommandName = 96;

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        const auto ca = static_cast<unsigned char>(a[i]);
        const auto cb = static_cast<unsigned char>(b[i]);
        if (std::tolower(ca) != std::tolower(cb)) {
            return false;
        }
    }
    return true;
}

void attribute(Console::Command& cmd) {
    cmd.from_exe = Console::address_in_exe(cmd.handler);
    if (auto owner = Modules::owning_module_name(cmd.handler)) {
        cmd.module = std::move(*owner);
    }
}

}  // namespace

uintptr_t Console::source_block() {
    const auto* exe = Modules::get().exe();
    if (exe == nullptr || exe->base == 0) {
        return 0;
    }
    return exe->base + kSourceBlockOffset;
}

uintptr_t Console::static_table() {
    const auto block = source_block();
    if (block == 0) {
        return 0;
    }
    return mem::read_ptr(block + kBlockCmdTable).value_or(0);
}

std::optional<size_t> Console::static_count() {
    const auto block = source_block();
    if (block == 0) {
        return std::nullopt;
    }
    const auto count = mem::read_u32(block + kBlockCmdCount);
    if (!count.has_value() || *count > kMaxCommands) {
        return std::nullopt;
    }
    return static_cast<size_t>(*count);
}

uintptr_t Console::list_head() {
    const auto block = source_block();
    if (block == 0) {
        return 0;
    }
    return block + kBlockListHead;
}

bool Console::address_in_exe(uintptr_t address) {
    const auto* exe = Modules::get().exe();
    if (exe == nullptr || exe->base == 0 || exe->size == 0 || address == 0) {
        return false;
    }
    return address >= exe->base && address < exe->base + exe->size;
}

bool Console::node_is_consistent(uintptr_t object) {
    if (object == 0) {
        return false;
    }

    // The registrar writes the object's own base into the dword after the link. A node that disagrees is
    // either not a registry object or a corrupted one, and either way its name and handler are not
    // trustworthy.
    const auto owner = mem::read_ptr(object + kObjOwner);
    if (!owner.has_value() || *owner != object) {
        return false;
    }

    // Both link directions must land somewhere readable. This does not prove membership of any particular
    // list -- that is what the walk establishes -- only that the node has links at all.
    const auto prev = mem::read_ptr(object + kObjLink);
    const auto next = mem::read_ptr(object + kObjLinkNext);
    return prev.has_value() && next.has_value() && *prev != 0 && *next != 0;
}

std::optional<Console::Command> Console::read_object(uintptr_t object) {
    if (!node_is_consistent(object)) {
        return std::nullopt;
    }

    const auto name_ptr = mem::read_ptr(object + kObjName);
    if (!name_ptr.has_value()) {
        return std::nullopt;
    }
    auto name = mem::read_name(*name_ptr, kMaxCommandName, kMinCommandName);
    if (!name.has_value()) {
        return std::nullopt;
    }

    Command cmd{};
    cmd.object = object;
    cmd.link = object + kObjLink;
    cmd.name = std::move(*name);
    cmd.handler = mem::read_ptr(object + kObjTarget).value_or(0);
    cmd.flags = mem::read_u32(object + kObjFlags).value_or(0);
    attribute(cmd);
    return cmd;
}

std::vector<Console::Command> Console::all(size_t limit) {
    std::vector<Command> out;
    const auto head = list_head();
    if (head == 0) {
        return out;
    }

    auto node = mem::read_ptr(head + 4);
    if (!node.has_value()) {
        return out;
    }

    out.reserve(128);
    size_t walked = 0;
    while (*node != 0 && *node != head && walked < kMaxCommands) {
        ++walked;

        // The link sits at object+8, so the object begins two dwords earlier. Rather than trusting that
        // arithmetic, read_object verifies it against the owner back-pointer the registrar wrote.
        if (auto cmd = read_object(*node - kObjLink)) {
            out.push_back(std::move(*cmd));
            if (limit != 0 && out.size() >= limit) {
                break;
            }
        }

        const auto next = mem::read_ptr(*node + 4);
        if (!next.has_value()) {
            break;
        }
        node = next;
    }

    return out;
}

std::vector<Console::Command> Console::static_commands() {
    std::vector<Command> out;
    const auto table = static_table();
    const auto count = static_count();
    if (table == 0 || !count.has_value()) {
        return out;
    }

    out.reserve(*count);
    for (size_t i = 0; i < *count; ++i) {
        const auto entry = table + i * kDescriptorEntrySize;
        const auto name_ptr = mem::read_ptr(entry + kDescName);
        if (!name_ptr.has_value()) {
            continue;
        }
        auto name = mem::read_name(*name_ptr, kMaxCommandName, kMinCommandName);
        if (!name.has_value()) {
            continue;
        }

        Command cmd{};
        cmd.name = std::move(*name);
        cmd.handler = mem::read_ptr(entry + kDescTarget).value_or(0);
        cmd.flags = mem::read_u32(entry + kDescTag).value_or(0);
        attribute(cmd);
        out.push_back(std::move(cmd));
    }

    return out;
}

std::optional<Console::Command> Console::find(std::string_view name) {
    for (auto& cmd : all()) {
        if (cmd.name == name) {
            return cmd;
        }
    }
    return std::nullopt;
}

std::optional<Console::Command> Console::find_insensitive(std::string_view name) {
    for (auto& cmd : all()) {
        if (iequals(cmd.name, name)) {
            return cmd;
        }
    }
    return std::nullopt;
}

std::optional<Console::CommandHandler> Console::handler_of(std::string_view name) {
    const auto cmd = find(name);
    if (!cmd.has_value() || cmd->handler == 0) {
        return std::nullopt;
    }
    return cmd->as_handler();
}

std::optional<Console::Stats> Console::stats() {
    const auto head = list_head();
    if (head == 0) {
        return std::nullopt;
    }

    auto node = mem::read_ptr(head + 4);
    if (!node.has_value()) {
        return std::nullopt;
    }

    Stats s{};
    std::vector<std::string> names;
    names.reserve(128);

    while (*node != 0 && *node != head) {
        if (s.nodes_walked >= kMaxCommands) {
            s.hit_cap = true;
            break;
        }
        ++s.nodes_walked;

        const auto object = *node - kObjLink;
        if (!node_is_consistent(object)) {
            ++s.inconsistent_nodes;
        } else if (auto cmd = read_object(object)) {
            ++s.total;
            if (cmd->from_exe) {
                ++s.from_exe;
            } else {
                ++s.from_modules;
            }
            names.push_back(cmd->name);
        } else {
            ++s.unreadable_names;
        }

        const auto next = mem::read_ptr(*node + 4);
        if (!next.has_value()) {
            break;
        }
        node = next;
    }

    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    s.distinct_names = names.size();
    return s;
}

}  // namespace sdk
