#include "Console.hpp"

#include <iterator>

#include <algorithm>
#include <string>
#include <cctype>

#include "Memory.hpp"
#include "Modules.hpp"
#include "interfaces/Registry.hpp"

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
            if (cmd->registered_at_runtime()) {
                ++s.runtime;
            } else {
                ++s.builtin;
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

uintptr_t Console::client_interface() {
    return reinterpret_cast<uintptr_t>(interfaces::Registry::get().resolve("ILTClient.Default"));
}

uintptr_t Console::client_vtable_slot(size_t slot) {
    const auto iface = client_interface();
    if (iface == 0) {
        return 0;
    }
    const auto vtable = mem::read_ptr(iface);
    if (!vtable.has_value() || *vtable == 0) {
        return 0;
    }
    return mem::read_ptr(*vtable + slot * sizeof(void*)).value_or(0);
}

namespace {

// A console method must live inside the executable: ILTClient is implemented by CLTClient in FEAR2.exe, so
// a slot pointing anywhere else means the vtable is not the one this mapping describes.
uintptr_t checked_console_slot(size_t slot) {
    const auto fn = Console::client_vtable_slot(slot);
    return Console::address_in_exe(fn) ? fn : 0;
}

}  // namespace

std::optional<Console::FindVariableFn> Console::find_variable_fn() {
    const auto fn = checked_console_slot(kSlotFindVariable);
    if (fn == 0) {
        return std::nullopt;
    }
    return reinterpret_cast<FindVariableFn>(fn);
}

std::optional<Console::SetVariableFloatFn> Console::set_variable_float_fn() {
    const auto fn = checked_console_slot(kSlotSetVariableFloat);
    if (fn == 0) {
        return std::nullopt;
    }
    return reinterpret_cast<SetVariableFloatFn>(fn);
}

std::optional<Console::RegisterProgramFn> Console::register_program_fn() {
    const auto fn = checked_console_slot(kSlotRegisterProgram);
    if (fn == 0) {
        return std::nullopt;
    }
    return reinterpret_cast<RegisterProgramFn>(fn);
}


namespace {

constexpr Console::Registrar kRegistrars[] = {
    {0x07F3B0, "GameClient_RegisterConsoleCommands", 56},
    {0x108CD0, "CMoveMgr_Init", 5},
    {0x114F10, "CPlayerStats_Init", 5},
    {0x0B5500, nullptr, 3},
    {0x0F8B40, nullptr, 3},
    {0x07ACF0, nullptr, 2},
    {0x047A30, "CCheatMgr_Init", 1},
    {0x053FC0, "CClientInfoMgr_Init", 1},
    {0x0F4920, "PlayerInputBindings_Init", 1},
    {0x159D20, nullptr, 1},
    {0x0C1FB0, nullptr, 1},
};

struct CommandRegistrar {
    const char* command;
    uintptr_t registrar;
};

// 76 registrations swept from gameclient's registration sites. See the header for why the first sweep
// produced 71.
constexpr CommandRegistrar kCommandRegistrars[] = {
    {"ABORT", 0x07F3B0},
    {"ADD", 0x07F3B0},
    {"AIDebug", 0x07F3B0},
    {"AddGoal", 0x07F3B0},
    {"AllocateMemory", 0x07F3B0},
    {"CameraClientFX", 0x07F3B0},
    {"ChaseToggle", 0x07F3B0},
    {"ClientFX", 0x07F3B0},
    {"Cmd", 0x07F3B0},
    {"ConsoleRunWorld", 0x07F3B0},
    {"DELAY", 0x07F3B0},
    {"DELAYID", 0x07F3B0},
    {"DoDamage", 0x07F3B0},
    {"ExitLevel", 0x07F3B0},
    {"ForceSpectatorClipMode", 0x07F3B0},
    {"FragSelf", 0x07F3B0},
    {"GPDClear", 0x07F3B0},
    {"GPDOutput", 0x07F3B0},
    {"GPDSet", 0x07F3B0},
    {"GetPlayerOrientation", 0x07F3B0},
    {"GetPlayerPos", 0x07F3B0},
    {"IF", 0x07F3B0},
    {"INT", 0x07F3B0},
    {"InitSound", 0x07F3B0},
    {"LISTCMDS", 0x07F3B0},
    {"LOOP", 0x07F3B0},
    {"LOOPID", 0x07F3B0},
    {"List", 0x07F3B0},
    {"LoadCheckpoint", 0x07F3B0},
    {"MSG", 0x07F3B0},
    {"MapCapture", 0x07F3B0},
    {"OBJ", 0x07F3B0},
    {"OrbitalSSCapture", 0x07F3B0},
    {"OrbitalSSClearMarker", 0x07F3B0},
    {"OrbitalSSPlaceCameraMarker", 0x07F3B0},
    {"OrbitalSSPlaceMarkerAt", 0x07F3B0},
    {"PlaySoundRecord", 0x07F3B0},
    {"PlaySoundString", 0x07F3B0},
    {"PrintCollisionProperties", 0x07F3B0},
    {"RAND", 0x07F3B0},
    {"RANDWEIGHT", 0x07F3B0},
    {"REPEAT", 0x07F3B0},
    {"REPEATID", 0x07F3B0},
    {"RebindFX", 0x07F3B0},
    {"RemoveGoal", 0x07F3B0},
    {"ReportClientObjects", 0x07F3B0},
    {"ReportMemory", 0x07F3B0},
    {"ReportUnusedAnimProps", 0x07F3B0},
    {"SET", 0x07F3B0},
    {"SHOWVAR", 0x07F3B0},
    {"SUB", 0x07F3B0},
    {"SetPlayerOrientation", 0x07F3B0},
    {"Stimulus", 0x07F3B0},
    {"Teleport", 0x07F3B0},
    {"VMSG", 0x07F3B0},
    {"WHEN", 0x07F3B0},
    {"PlayerLeash", 0x108CD0},
    {"TestFire", 0x108CD0},
    {"TestFire2", 0x108CD0},
    {"TestGrenade", 0x108CD0},
    {"TestSAFire", 0x108CD0},
    {"Air", 0x114F10},
    {"Armor", 0x114F10},
    {"Health", 0x114F10},
    {"MaxArmor", 0x114F10},
    {"MaxHealth", 0x114F10},
    {"DetectPerf", 0x0B5500},
    {"PerformanceGroupLevel", 0x0B5500},
    {"RunPerfTest", 0x0B5500},
    {"DumpIP", 0x0F8B40},
    {"StartIP", 0x0F8B40},
    {"StopAllIPs", 0x0F8B40},
    {"NextSpawnPoint", 0x07ACF0},
    {"PrevSpawnPoint", 0x07ACF0},
    {"Cheat", 0x047A30},
    {"AddClient", 0x053FC0},
    {"ShowCommandSet", 0x0F4920},
    {"SetProfileName", 0x159D20},
    {"SpawnLaserSight", 0x0C1FB0},
};

}  // namespace

const Console::Registrar* Console::registrars(size_t& count) {
    count = std::size(kRegistrars);
    return kRegistrars;
}

const Console::Registrar* Console::registrar_of(std::string_view name) {
    if (name.empty()) {
        return nullptr;
    }
    for (const auto& cr : kCommandRegistrars) {
        if (name == cr.command) {
            for (const auto& r : kRegistrars) {
                if (r.offset == cr.registrar) {
                    return &r;
                }
            }
            return nullptr;
        }
    }
    return nullptr;
}

std::vector<std::string> Console::commands_registered_by(uintptr_t registrar_offset) {
    std::vector<std::string> out;
    for (const auto& cr : kCommandRegistrars) {
        if (cr.registrar == registrar_offset) {
            out.emplace_back(cr.command);
        }
    }
    return out;
}

uintptr_t Console::empty_stub() {
    const auto* gc = Modules::get().game_client();
    if (gc == nullptr || gc->base == 0) {
        return 0;
    }
    return gc->base + kEmptyStubOffset;
}

std::optional<bool> Console::is_noop(std::string_view name) {
    const auto cmd = find(name);
    if (!cmd.has_value()) {
        return std::nullopt;
    }
    const auto stub = empty_stub();
    if (stub == 0) {
        return std::nullopt;
    }
    return cmd->handler == stub;
}

std::vector<std::string> Console::noop_commands() {
    std::vector<std::string> out;
    const auto stub = empty_stub();
    if (stub == 0) {
        return out;
    }
    for (const auto& c : all()) {
        if (c.handler == stub) {
            out.push_back(c.name);
        }
    }
    return out;
}


std::vector<std::string> Console::unattributed_commands() {
    std::vector<std::string> out;
    const auto* gc = Modules::get().game_client();
    if (gc == nullptr || gc->base == 0) {
        return out;
    }
    for (const auto& c : all()) {
        if (c.from_exe || c.handler < gc->base || c.handler >= gc->base + gc->size) {
            continue;
        }
        if (registrar_of(c.name) == nullptr) {
            out.push_back(c.name);
        }
    }
    return out;
}

std::vector<std::string> Console::unregistered_table_commands() {
    std::vector<std::string> out;
    for (const auto& cr : kCommandRegistrars) {
        if (!find(cr.command).has_value()) {
            out.emplace_back(cr.command);
        }
    }
    return out;
}

}  // namespace sdk
