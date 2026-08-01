#include "Hooks.hpp"

#include <atomic>

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

namespace {
std::atomic<bool> g_sealed{false};
}  // namespace

void Hooks::seal() {
    if (!g_sealed.exchange(true, std::memory_order_acq_rel)) {
        LOGX("[hooks] registry SEALED -- no further installs will be accepted");
    }
}

bool Hooks::sealed() const {
    return g_sealed.load(std::memory_order_acquire);
}

std::string Hooks::owner_of(void* target) const {
    for (const auto& [name, addr] : m_targets) {
        if (addr == target) {
            return name;
        }
    }
    return {};
}

bool Hooks::install(std::string name, void* target, void* destination) {
    if (g_sealed.load(std::memory_order_acquire)) {
        LOGX("[hooks] REFUSED install of '%s' -- registry is sealed (shutting down)", name.c_str());
        return false;
    }

    // ---- ONE OWNER PER ENGINE ENTRY -----------------------------------------------------------
    //
    // Two inline hooks on the same address crash the process on UNLOAD, not on install, which is
    // what makes it worth refusing here rather than leaving to discipline. The second hook patches
    // over the first one's jmp and saves it as its "original"; on teardown the restores put those
    // bytes back in an order that can leave the function permanently jumping to a detour inside an
    // image that is no longer mapped. Measured: FEAR2.exe died with 0xC0000005 in
    // `fear2vr.dll_unloaded` at the offset of ViewHook's set_pos_rot_detour, seconds after a clean
    // unload that the quiescence check had passed -- because quiescence proves no thread is INSIDE
    // the image, and says nothing about a jmp that will send one there later.
    if (const auto owner = owner_of(target); !owner.empty()) {
        LOGX("[hooks] REFUSED install of '%s' at %p -- already owned by '%s'. Extend that detour "
             "instead of stacking a second one.",
             name.c_str(), target, owner.c_str());
        return false;
    }

    // create_inline returns the hook directly (truthy on success); error detail
    // is only available via InlineHook::create, which we don't need here --
    // failure handling is identical (log + false), the log just loses the enum.
    auto hook = safetyhook::create_inline(target, destination);
    if (!hook) {
        LOGX("[hooks] FAILED to install %s at %p", name.c_str(), target);
        return false;
    }
    LOGX("[hooks] installed %s at %p -> %p", name.c_str(), target, destination);
    {
        std::scoped_lock _{m_mux};
        m_targets.emplace_back(name, target);
    }
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

bool Hooks::install_mid(std::string name, void* target, safetyhook::MidHookFn destination) {
    if (g_sealed.load(std::memory_order_acquire)) {
        LOGX("[hooks] REFUSED install of '%s' -- registry is sealed (shutting down)", name.c_str());
        return false;
    }

    auto hook = safetyhook::create_mid(target, destination);
    if (!hook) {
        LOGX("[hooks] FAILED to install mid %s at %p", name.c_str(), target);
        return false;
    }
    LOGX("[hooks] installed mid %s at %p", name.c_str(), target);
    adopt_mid(std::move(name), std::move(hook));
    return true;
}

void Hooks::adopt_mid(std::string name, safetyhook::MidHook hook) {
    std::scoped_lock _{m_mux};
    if (m_retire_started) {
        LOGX("[hooks] WARNING: adopting mid %s AFTER retire started; disabling immediately", name.c_str());
        (void)hook.disable();
    }
    m_mid_hooks.emplace_back(std::move(name), std::move(hook));
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
    // Same search over mid hooks -- a caller retiring "weapon_fire" by name must
    // not have to know which kind it is.
    for (auto& [hook_name, hook] : m_mid_hooks) {
        if (hook_name == name) {
            if (!hook || !hook.enabled()) {
                return false;
            }
            const auto result = hook.disable();
            if (result) {
                LOGX("[hooks] retired mid %s", name.c_str());
                ++m_retired;
                return true;
            }
            LOGX("[hooks] FAILED to retire mid %s (error %d)", name.c_str(), static_cast<int>(result.error().type));
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
    // Mid hooks are part of the SAME contract: if any of these stays armed the
    // DLL must not unmap, exactly as for inline hooks.
    for (auto& [name, hook] : m_mid_hooks) {
        if (!hook || !hook.enabled()) {
            continue;
        }
        auto result = hook.disable();
        if (!result) {
            LOGX("[hooks] FAILED to retire mid %s (error %d) -- fail-closed, DLL stays mapped",
                 name.c_str(), static_cast<int>(result.error().type));
            ok = false;
        } else {
            LOGX("[hooks] retired mid %s", name.c_str());
            ++m_retired;
        }
    }
    return ok;
}
