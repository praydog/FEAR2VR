#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <safetyhook.hpp>

// Owns every SafetyHook the framework installs, so graceful uninject can retire
// them in ONE place. Lifecycle contract:
//
//   install/adopt: any thread, any time (game thread, init thread, /test handler)
//   retire()     : disable()s every hook (restores original prologue bytes). No
//                  new entry can reach our detours afterwards. InlineHook objects
//                  are deliberately NOT destroyed on unload: a straggler thread
//                  already displaced into a trampoline must find its memory still
//                  mapped. We leak the hook allocations on unload (bounded, rare),
//                  the same tradeoff as the foothold stub in the reference design
//                  (il2cpp-scripting).
//
// Thread safety: installs serialized on m_mux; retire() holds m_mux while
// disabling so no install can race a shutdown.
//
// INVARIANT: Hooks::get() returns a deliberately LEAKED singleton (never
// destructed in the game process). Any future change that lets the InlineHook
// vector run its destructor in the game process breaks the unload contract --
// do not "fix" the leak.
class Hooks {
public:
    // Leaked singleton. NOT a function-local static (its destructor would free
    // trampoline memory under straggler threads during unmap).
    static Hooks& get();

    // Install and track a hook. Returns false if the safetyhook factory failed.
    // `name` is diagnostic-only (logs, /health).
    bool install(std::string name, void* target, void* destination);

    // Install a MID-function hook: the detour receives the full register
    // context and the original code continues afterwards. Needed for targets
    // whose arguments arrive in non-standard registers (`__userpurge`), where
    // no C++ signature can express the call and an inline hook cannot forward
    // it. Retired by the same retire()/retire_one() as inline hooks -- the
    // graceful-uninject contract covers both kinds or it covers neither.
    bool install_mid(std::string name, void* target, safetyhook::MidHookFn destination);

    // Move an already-created hook under management.
    void adopt(std::string name, safetyhook::InlineHook hook);
    void adopt_mid(std::string name, safetyhook::MidHook hook);

    // Disable every hook. Returns false if ANY disable() failed (fail-closed:
    // the caller MUST NOT unmap the DLL in that case).
    // ---- SEALING, WHICH CLOSES A RACE THAT CORRUPTED THE GAME ----------------------------------
    //
    // After this returns, every install*() call is REFUSED and logged. Retirement is only complete
    // if nothing can be added behind it, and until this existed something could:
    //
    //     Framework::shutdown() -> Mods::on_shutdown()   mods stop their own work...
    //                           -> Hooks::retire()       ...but the FRAME HOOK is still live here
    //
    // FireRedirect installs its gameserver hooks from on_frame() on a retry countdown, because
    // gameserver.dll is lazy. A retry landing in that window -- or during retire()'s own walk,
    // after it has passed the end of the registry -- adds a hook nothing will ever disable.
    //
    // FOUND IN A LIVE PROCESS, not by reading: gameserver.dll's Weapon_TraceShot began with `E9`,
    // jumping to an orphaned RWX stub at 0x0ADD01E0 that belongs to no loaded module and whose own
    // first byte is another jump into an unmapped image. The next hitscan shot would have run it.
    // It also explains the symptom that started the hunt -- the scan for that function MISSED,
    // because the pattern's first ten bytes had been overwritten by the stale patch.
    void seal();
    bool sealed() const;

    bool retire();

    // Disable ONE hook by name (no-op + false if absent/already retired).
    // Used by tests that must prove attach/fire/detach on a hot path without
    // waiting for full shutdown.
    bool retire_one(const std::string& name);

    // Borrowed pointer to a managed hook (nullptr if absent). Do NOT hold across
    // later install()/adopt() calls -- vector growth invalidates it. Intended
    // for detours that must call their original through the trampoline.
    safetyhook::InlineHook* find(const std::string& name);

    size_t count() const { return m_hooks.size() + m_mid_hooks.size(); }
    size_t retired_count() const { return m_retired; }

    Hooks() = default;
    Hooks(const Hooks&) = delete;
    Hooks& operator=(const Hooks&) = delete;

private:
    std::mutex m_mux;
    std::vector<std::pair<std::string, safetyhook::InlineHook>> m_hooks;
    std::vector<std::pair<std::string, safetyhook::MidHook>> m_mid_hooks;
    size_t m_retired{0};
    bool m_retire_started{false};
};
