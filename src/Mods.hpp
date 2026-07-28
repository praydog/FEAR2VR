#pragma once

#include <string>
#include <vector>

#include "Mod.hpp"

// Mod registry singleton. Construct Mod singletons (each its own XxxMod::get())
// and add them here; Framework drives the fan-out.
class Mods {
public:
    static Mods& get() {
        static Mods s_instance;
        return s_instance;
    }

    // Registered mods are owned by their own singletons; we just hold pointers.
    void add(Mod* mod) { m_mods.push_back(mod); }

    // Event fan-out. on_frame runs on the game thread every CClientShell::Update.
    void on_initialize();
    void on_frame();
    void on_shutdown();

private:
    Mods() = default;
    std::vector<Mod*> m_mods;
};
