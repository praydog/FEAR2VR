#include "Log.hpp"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>

#include <windows.h> // OutputDebugStringA only (no std equivalent)

namespace logger {
namespace {
std::mutex g_mux;
std::ofstream g_file;
} // namespace

void init(const char* filename) {
    std::scoped_lock _{g_mux};
    if (g_file.is_open()) {
        return;
    }
    g_file.open(filename, std::ios::out | std::ios::trunc);
}

void writef(const char* fmt, ...) {
    char buf[4096];

    va_list args;
    va_start(args, fmt);
    int32_t n = std::vsnprintf(buf, sizeof(buf) - 2, fmt, args);
    va_end(args);

    if (n < 0) {
        return;
    }
    if (n > static_cast<int32_t>(sizeof(buf)) - 2) {
        n = static_cast<int32_t>(sizeof(buf)) - 2;
    }
    buf[n] = '\n';
    buf[n + 1] = '\0';

    std::scoped_lock _{g_mux};
    OutputDebugStringA(buf);
    if (g_file.is_open()) {
        g_file << buf;
        g_file.flush();
    }
}
} // namespace logger
