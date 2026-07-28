#pragma once

// Minimal printf-style logger: writes to a file and OutputDebugString. Kept
// printf-style (not spdlog's {}-style) so ported %s/%p/%i format strings work
// verbatim. Thread-safe.

namespace logger {
// Open (or create/truncate) the log file. Idempotent; safe to call once at startup.
void init(const char* filename);

// printf-style line writer. Appends a newline.
void writef(const char* fmt, ...);
} // namespace logger

#define LOGX(...) ::logger::writef(__VA_ARGS__)
