#include "UiSystemInterface.hpp"

#include <windows.h>

#include <cstdio>

namespace xrui {

UiSystemInterface::UiSystemInterface() {
    LARGE_INTEGER freq{};
    LARGE_INTEGER now{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    m_qpc_freq = freq.QuadPart;
    m_start_qpc = now.QuadPart;
}

double UiSystemInterface::GetElapsedTime() {
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart - m_start_qpc) / static_cast<double>(m_qpc_freq);
}

bool UiSystemInterface::LogMessage(Rml::Log::Type type, const Rml::String& message) {
    const char* level = "info";
    switch (type) {
        case Rml::Log::LT_ERROR:
            level = "error";
            break;
        case Rml::Log::LT_ASSERT:
            level = "assert";
            break;
        case Rml::Log::LT_WARNING:
            level = "warn";
            break;
        case Rml::Log::LT_INFO:
            level = "info";
            break;
        case Rml::Log::LT_DEBUG:
            level = "debug";
            break;
        default:
            level = "log";
            break;
    }
    std::printf("[host] [ui/%s] %s\n", level, message.c_str());
    // Always continue: this process has no debugger reliably attached in the field, so breaking
    // on LT_ASSERT would just hang a headless run instead of surfacing anything.
    return true;
}

} // namespace xrui
