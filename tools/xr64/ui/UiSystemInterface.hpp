#pragma once

// RmlUi's SystemInterface: time and logging. Both are one-way from RmlUi's side -- it asks the
// clock, it hands us a formatted string -- so this is a thin adapter rather than anything with
// real state, aside from the clock's own epoch.

#include <RmlUi/Core/SystemInterface.h>

namespace xrui {

class UiSystemInterface final : public Rml::SystemInterface {
public:
    UiSystemInterface();

    // Seconds since this interface was constructed, i.e. since SettingsUi::init(). RmlUi uses this
    // for animation/transition timing and for its own internal timers -- it does not need to agree
    // with any other clock in the process.
    double GetElapsedTime() override;

    // Routed through the host's own `[host]` printf convention rather than RmlUi's default
    // (OutputDebugString on Windows), which is invisible unless a debugger happens to be attached.
    // A layout warning that only a debugger can see is a layout warning nobody sees.
    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override;

private:
    long long m_start_qpc;
    long long m_qpc_freq;
};

} // namespace xrui
