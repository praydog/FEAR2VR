#include "Modules.hpp"

#include <cinttypes>

#include <utility/Module.hpp>
#include <utility/Scan.hpp>

#include "Log.hpp"

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

} // namespace sdk
