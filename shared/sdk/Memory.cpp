#include "Memory.hpp"

// For EXCEPTION_EXECUTE_HANDLER. Deliberately in the .cpp: the header must not drag Windows macros into
// every consumer that only wants a guarded read.
#include <windows.h>

#include <utility/Seh.hpp>

namespace sdk::mem {

bool copy(void* out, uintptr_t at, size_t bytes) {
    if (out == nullptr || at == 0 || bytes == 0) {
        return false;
    }

    bool ok = false;
    KANANLIB_SEH_TRY {
        std::memcpy(out, reinterpret_cast<const void*>(at), bytes);
        ok = true;
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return ok;
}

bool store(uintptr_t at, const void* src, size_t bytes) {
    if (src == nullptr || at == 0 || bytes == 0) {
        return false;
    }

    bool ok = false;
    KANANLIB_SEH_TRY {
        std::memcpy(reinterpret_cast<void*>(at), src, bytes);
        ok = true;
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return ok;
}

namespace detail {

bool guarded_invoke(void (*fn)(void*), void* context) {
    if (fn == nullptr) {
        return false;
    }

    // One of the three SEH blocks in this SDK, and all three are in this file: copy, store, and this. The
    // per-value helpers keep their own rather than routing through here, because a walk reads thousands of
    // fields and an indirect call per field is a cost with nothing to show for it.
    bool ok = false;
    KANANLIB_SEH_TRY {
        fn(context);
        ok = true;
    }
    KANANLIB_SEH_EXCEPT(EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return ok;
}

}  // namespace detail

bool is_printable_name(std::string_view text, size_t min_length) {
    if (text.size() < min_length) {
        return false;
    }
    for (const char c : text) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || u > 0x7E) {
            return false;
        }
    }
    return true;
}

std::optional<std::string> read_cstring(uintptr_t at, size_t max_length) {
    if (at == 0 || max_length == 0) {
        return std::nullopt;
    }

    std::string out;
    out.reserve(max_length < 64 ? max_length : 64);

    // One byte at a time, because the string's length is unknown and a bulk read of max_length could span
    // into an unmapped page and fail for a string that was perfectly readable.
    for (size_t i = 0; i < max_length; ++i) {
        const auto c = read<char>(at + i);
        if (!c.has_value()) {
            return std::nullopt;
        }
        if (*c == '\0') {
            return out;
        }
        out.push_back(*c);
    }

    // Ran to the limit without a terminator. That is not a string, and returning the truncation would hand
    // the caller a value that looks like data.
    return std::nullopt;
}

std::optional<std::string> read_name(uintptr_t at, size_t max_length, size_t min_length) {
    auto text = read_cstring(at, max_length);
    if (!text.has_value() || !is_printable_name(*text, min_length)) {
        return std::nullopt;
    }
    return text;
}

bool looks_like_float(uint32_t raw) {
    if (raw == 0) {
        return false;  // zero is both readings, so it is not evidence either way
    }

    float f = 0.0f;
    std::memcpy(&f, &raw, sizeof(f));
    if (!std::isfinite(f)) {
        return false;
    }

    const float magnitude = std::fabs(f);
    if (magnitude < 1e-6f || magnitude > 1e9f) {
        return false;  // a denormal or an absurdity -- which is what a genuine integer reads as
    }

    // And it must be implausible as an integer. Anything below 2^24 is an ordinary count, index or
    // dimension, and those are far more common in these tables than floats.
    return raw > (1u << 24);
}

}  // namespace sdk::mem
