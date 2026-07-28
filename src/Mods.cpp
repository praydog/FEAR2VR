#include "Mods.hpp"

#include "Log.hpp"

void Mods::on_initialize() {
    for (auto* mod : m_mods) {
        if (auto err = mod->on_initialize()) {
            LOGX("[mods] %s::on_initialize FAILED: %s", mod->get_name().data(), err->c_str());
        }
    }
}

void Mods::on_frame() {
    for (auto* mod : m_mods) {
        mod->on_frame();
    }
}

void Mods::on_shutdown() {
    for (auto* mod : m_mods) {
        mod->on_shutdown();
    }
}
