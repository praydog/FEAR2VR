#pragma once

#include <optional>
#include <string>
#include <string_view>

// Base class for per-feature mods (the REFramework/UEVR Mod convention).
// Mods are singletons (XxxMod::get()), registered into Mods during static
// setup, and live for the entire injection lifetime. Lifecycle fan-out:
//
//   on_initialize  -- after SDK resolution, before hooks go hot for the frame
//   on_frame       -- every CClientShell::Update tick (game hot path; stay lean)
//   on_shutdown    -- graceful uninject starts; mods MUST remove nothing here,
//                     hook retirement is handled globally by Hooks::retire()
class Mod {
public:
    virtual ~Mod() = default;

    virtual std::string_view get_name() const { return "UnknownMod"; }

    // Empty optional = success; string = failure reason (logged loudly).
    virtual std::optional<std::string> on_initialize() { return std::nullopt; }
    virtual void on_frame() {}
    virtual void on_shutdown() {}
};
