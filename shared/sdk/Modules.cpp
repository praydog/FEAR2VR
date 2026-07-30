#include "Modules.hpp"

#include <cinttypes>
#include <cstring>

#include <utility/Module.hpp>
#include <utility/Scan.hpp>

#include "Log.hpp"
#include "Memory.hpp"

namespace sdk {

uintptr_t Modules::scan_exe(const char* pattern, const char* name) const {
    if (exe()->handle == nullptr) {
        LOGX("[sdk] scan '%s': FEAR2.exe module unresolved", name);
        return 0;
    }
    const auto result = utility::scan(exe()->handle, pattern);
    if (!result) {
        LOGX("[sdk] pattern MISS: %s", name);
        return 0;
    }
    LOGX("[sdk] %-22s -> 0x%08" PRIXPTR, name, *result);
    return *result;
}

bool Modules::initialize() {
    // Statically initialized: the table exists from image load; only the
    // resolution pass happens here.
    m_modules[0] = {"FEAR2.exe", nullptr, 0, 0, true};
    m_modules[1] = {"gameclient.dll", nullptr, 0, 0, true};
    m_modules[2] = {"gameserver.dll", nullptr, 0, 0, false}; // lazy: session start
    m_modules[3] = {"gamedatabase.dll", nullptr, 0, 0, true};
    m_modules[4] = {"ltmemory.dll", nullptr, 0, 0, true};

    bool all_required = true;
    for (auto& m : m_modules) {
        // kananlib utility::get_module resolves by basename inside the already-
        // loaded process (we are injected into FEAR2.exe); never loads anything.
        m.handle = utility::get_module(m.name);
        if (m.handle == nullptr) {
            if (m.required) {
                all_required = false;
            }
            continue;
        }
        m.base = reinterpret_cast<uintptr_t>(m.handle);
        m.size = utility::get_module_size(m.handle).value_or(0);
        if (m.size == 0 && m.required) {
            all_required = false;
        }
    }
    m_initialized = all_required;
    return all_required;
}

std::optional<std::string> Modules::owning_module_name(uintptr_t address) {
    HMODULE owner = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(address), &owner) == 0 ||
        owner == nullptr) {
        return std::nullopt;
    }
    char path[MAX_PATH]{};
    if (GetModuleFileNameA(owner, path, sizeof(path)) == 0) {
        return std::nullopt;
    }
    std::string full{path};
    const auto slash = full.find_last_of("\\/");
    return slash == std::string::npos ? full : full.substr(slash + 1);
}


namespace {

// Walk one module's section table. Returns false when the headers do not look like a PE.
template <typename F>
bool for_each_section(uintptr_t base, F&& body) {
    if (base == 0) {
        return false;
    }
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return false;
    }
    const auto* sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (!body(*sec)) {
            return true;
        }
    }
    return true;
}

}  // namespace

std::optional<Modules::SectionInfo> Modules::section_of(uintptr_t address) {
    if (address == 0) {
        return std::nullopt;
    }
    const auto& self = get();
    std::optional<SectionInfo> found;
    for (const auto& mod : self.m_modules) {
        if (mod.base == 0 || address < mod.base || address >= mod.base + mod.size) {
            continue;
        }
        // The headers themselves are mapped; reading them can still fault if a module was unmapped
        // underneath us, so the walk goes through the guard.
        mem::guarded([&] {
            for_each_section(mod.base, [&](const IMAGE_SECTION_HEADER& sec) {
                const auto start = mod.base + sec.VirtualAddress;
                const auto end = start + (sec.Misc.VirtualSize != 0 ? sec.Misc.VirtualSize : sec.SizeOfRawData);
                if (address < start || address >= end) {
                    return true;  // keep looking
                }
                SectionInfo out;
                std::memcpy(out.name, sec.Name, 8);
                out.name[8] = '\0';
                out.start = start;
                out.end = end;
                out.kind = ((sec.Characteristics & IMAGE_SCN_CNT_CODE) != 0 ||
                            (sec.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0)
                               ? SectionKind::Code
                               : SectionKind::Data;
                found = out;
                return false;  // stop
            });
        });
        if (found.has_value()) {
            break;
        }
    }
    return found;
}

bool Modules::looks_like_vtable_pointer(uintptr_t value) {
    const auto sec = section_of(value);
    return sec.has_value() && sec->kind == SectionKind::Data;
}

std::optional<bool> Modules::object_has_vtable(uintptr_t object) {
    if (object == 0) {
        return std::nullopt;
    }
    const auto first = mem::read_ptr(object);
    if (!first.has_value()) {
        return std::nullopt;
    }
    return looks_like_vtable_pointer(*first);
}

} // namespace sdk
