#include "Hooks.hpp"

#include "Log.hpp"

Hooks& Hooks::get() {
    // Deliberately leaked, NOT a function-local static: a static's destructor
    // would run during unload and destroy the InlineHook vector, and
    // ~InlineHook() frees the trampoline allocation that retired-but-straggling
    // game threads may still be executing. Leaking makes "trampolines stay
    // mapped for the process lifetime" structural instead of conventional.
    static Hooks* s_hooks = new Hooks();
    return *s_hooks;
}

bool Hooks::install(std::string name, void* target, void* destination) {
    // create_inline returns the hook directly (truthy on success); error detail
    // is only available via InlineHook::create, which we don't need here --
    // failure handling is identical (log + false), the log just loses the enum.
    auto hook = safetyhook::create_inline(target, destination);
    if (!hook) {
        LOGX("[hooks] FAILED to install %s at %p", name.c_str(), target);
        return false;
    }
    LOGX("[hooks] installed %s at %p -> %p", name.c_str(), target, destination);
    adopt(std::move(name), std::move(hook));
    return true;
}

void Hooks::adopt(std::string name, safetyhook::InlineHook hook) {
    std::scoped_lock _{m_mux};
    if (m_retire_started) {
        // Shutdown already began: disable immediately so this hook can never
        // fire after the DLL starts unmapping. Fail-closed on error assumption
        // is N/A here -- adoption during retire is a bug; log loudly.
        LOGX("[hooks] WARNING: adopting %s AFTER retire started; disabling immediately", name.c_str());
        (void)hook.disable();
    }
    m_hooks.emplace_back(std::move(name), std::move(hook));
}

safetyhook::InlineHook* Hooks::find(const std::string& name) {
    std::scoped_lock _{m_mux};
    for (auto& [hook_name, hook] : m_hooks) {
        if (hook_name == name) {
            return &hook;
        }
    }
    return nullptr;
}

bool Hooks::retire_one(const std::string& name) {
    std::scoped_lock _{m_mux};
    for (auto& [hook_name, hook] : m_hooks) {
        if (hook_name == name) {
            if (!hook || !hook.enabled()) {
                return false;
            }
            const auto result = hook.disable();
            if (result) {
                LOGX("[hooks] retired %s", name.c_str());
                ++m_retired;
                return true;
            }
            LOGX("[hooks] FAILED to retire %s (error %d)", name.c_str(), static_cast<int>(result.error().type));
            return false;
        }
    }
    return false;
}

bool Hooks::retire() {
    std::scoped_lock _{m_mux};
    m_retire_started = true;

    bool ok = true;
    for (auto& [name, hook] : m_hooks) {
        if (!hook) {
            continue; // empty (moved-from) slot
        }
        if (!hook.enabled()) {
            continue; // never armed or already disabled
        }
        auto result = hook.disable();
        if (!result) {
            LOGX("[hooks] FAILED to retire %s (error %d) -- fail-closed, DLL stays mapped",
                 name.c_str(), static_cast<int>(result.error().type));
            ok = false;
        } else {
            LOGX("[hooks] retired %s", name.c_str());
            ++m_retired;
        }
    }
    return ok;
}
