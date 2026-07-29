#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

//
// GUARDED READS AND WRITES AGAINST A LIVE GAME -- the primitive every class in this SDK is built on, and
// the reason it lives in a header rather than inside one .cpp.
//
// WHY A CONSUMER NEEDS THIS AND NOT JUST THE CLASSES. Every mapped structure hands out addresses: a model's
// node-control slot, a physics vtable entry, a resource record, a console command's handler. The moment a
// consumer follows one of those addresses itself -- which is the entire point of being handed an address --
// it needs exactly the same guarded read the SDK uses internally. Without this header it would either
// dereference raw pointers into a live engine, or reimplement the guard slightly differently. Fourteen
// files in this SDK each had their own private copy of the same helper before this existed; that was the
// SDK keeping its own safety to itself.
//
// WHAT THE GUARD IS FOR. These addresses come from reverse engineering, and a wrong one is not a rare
// accident -- it is the normal outcome of a mapping being incomplete. This project has shipped a wrong
// table base, a wrong container head and a wrong field offset, and in every case the wrong address pointed
// at MAPPED memory that read back plausible-looking bytes. So the guard is not "in case the game crashes":
// it is what turns a bad address into std::nullopt instead of a torn game process, and it must be cheap
// enough to use on every single field access without thinking about it.
//
// WHAT IS DELIBERATELY NOT HERE. No caching, no address translation, no module logic (see Modules.hpp), and
// no "read until it works" retries. A failed read means the address was wrong, and hiding that is how a
// wrong mapping survives long enough to be documented as fact.
//

namespace sdk::mem {

// Raw guarded copy out of the process. False means the read faulted or the arguments were rejected --
// never a partial copy, so `out` is untouched on failure.
bool copy(void* out, uintptr_t at, size_t bytes);

// Raw guarded store into the process. Separated from reading because the failure mode is different: a
// faulting write may already have modified engine state, so a consumer that cares must verify by reading
// back rather than trusting the return.
bool store(uintptr_t at, const void* src, size_t bytes);

// One trivially-copyable value. This is the form nearly every caller wants:
//
//     if (const auto count = sdk::mem::read<uint32_t>(table + 4)) { ... }
//
template <typename T>
std::optional<T> read(uintptr_t at) {
    static_assert(std::is_trivially_copyable_v<T>, "guarded reads copy bytes, so the type must be trivial");
    T value{};
    if (!copy(&value, at, sizeof(T))) {
        return std::nullopt;
    }
    return value;
}

// One trivially-copyable value, written back. Returns false on a rejected address or a faulting store; see
// the note on `store` about why a true return is not proof the engine accepted the value.
template <typename T>
bool write(uintptr_t at, const T& value) {
    static_assert(std::is_trivially_copyable_v<T>, "guarded writes copy bytes, so the type must be trivial");
    return store(at, &value, sizeof(T));
}

inline std::optional<uint32_t> read_u32(uintptr_t at) { return read<uint32_t>(at); }
inline std::optional<int32_t> read_i32(uintptr_t at) { return read<int32_t>(at); }
inline std::optional<uint16_t> read_u16(uintptr_t at) { return read<uint16_t>(at); }
inline std::optional<uint8_t> read_u8(uintptr_t at) { return read<uint8_t>(at); }
inline std::optional<float> read_f32(uintptr_t at) { return read<float>(at); }

// A pointer field. Separate from read_u32 only to say what the caller means at the call site; this build is
// 32-bit, so the widths are the same.
inline std::optional<uintptr_t> read_ptr(uintptr_t at) {
    const auto v = read<uint32_t>(at);
    if (!v.has_value()) {
        return std::nullopt;
    }
    return static_cast<uintptr_t>(*v);
}

//
// STRINGS ARE VALIDATED, NEVER TRUSTED.
//
// A wrong pointer that lands on mapped memory yields bytes, and bytes rendered as text look like data. This
// project has been fooled by exactly that: a container walk once reported 65534 "plausible names" from a
// region that held no records at all, and a supposed identity field turned out to be reading the debug fill
// pattern. So a name that fails these predicates is reported as absent rather than returned as a string a
// caller would then print, log, or compare.
//
constexpr size_t kMinNameLength = 4;
constexpr size_t kMaxNameLength = 256;

// ONE GUARD AROUND A WHOLE WALK.
//
// The per-value reads above are right for reading a field. They are the WRONG shape for a traversal that
// dereferences dozens of pointers per iteration -- a KD-tree descent, a model's node table, a material
// array -- where converting each dereference into its own guarded read would multiply the cost on the game
// thread and turn a readable walk into hundreds of nullopt checks whose only sensible response is to abandon
// the walk anyway.
//
// For those, run the whole body under a single guard:
//
//     const bool ok = sdk::mem::guarded([&] {
//         for (auto* node = root; node != nullptr; node = node->next) { ... }
//     });
//
// A fault anywhere inside abandons the body and yields false, which is exactly what one wide guard already
// did -- the difference is that the guard itself now lives in one place instead of being copied into every
// file that needs one, and a CONSUMER can reach it for its own walk over addresses this SDK handed out.
//
// THE BODY MUST HOLD ONLY TRIVIALLY DESTRUCTIBLE LOCALS. This is kananlib's constraint, not ours: on
// non-Windows platforms recovery is a siglongjmp that does not run destructors. Keep containers, locks and
// owning handles outside the body and copy results out through captured references.
//
// The SEH block itself stays in ONE translation unit: this template only wraps the callable into a plain
// function pointer plus a context, so no consumer of this header inherits the SEH macros.
namespace detail {
bool guarded_invoke(void (*fn)(void*), void* context);
}

template <typename F>
bool guarded(F&& body) {
    // Materialised here rather than inside the guard, so its destructor runs outside the guarded region.
    auto callable = std::forward<F>(body);
    return detail::guarded_invoke([](void* context) { (*static_cast<decltype(callable)*>(context))(); },
                                  &callable);
}

// Printable ASCII only, and at least `min_length` of it. The length floor is what rejects the single stray
// printable byte that random memory produces constantly.
bool is_printable_name(std::string_view text, size_t min_length = kMinNameLength);

// A NUL-terminated string, guarded, with no validation beyond termination inside `max_length`. Use when the
// caller genuinely wants whatever is there -- a path being diagnosed, for instance.
std::optional<std::string> read_cstring(uintptr_t at, size_t max_length = kMaxNameLength);

// A NUL-terminated string that must look like a name. This is the one to reach for when a wrong pointer is
// a live possibility, which is most of the time.
std::optional<std::string> read_name(uintptr_t at, size_t max_length = kMaxNameLength,
                                     size_t min_length = kMinNameLength);

//
// IS THIS DWORD A FLOAT OR AN INTEGER?
//
// Engine settings tables tag their entries with a type, and the tag can disagree with the bytes. Two entries
// in this build's variable table are tagged int32 while holding 0x3F000000 and 0x42800000 -- exactly 0.5f
// and 64.0f. Nothing in the executable reads either one, so the engine never notices; a consumer calling
// read_int on them gets 1056964608 and 1115684864.
//
// The test is the one that settled those two: a value that is absurd as an integer and ordinary as a float
// is a float. Deliberately conservative -- it answers false for zero, since zero is both, and false for any
// small integer, since small integers are the common case and their float reading is a denormal.
//
// This is a HEURISTIC about bytes, not a fact about the engine. It is the right tool for auditing a table
// or flagging a suspicious entry to a human, and the wrong tool for deciding how to interpret a field whose
// type is actually known.
bool looks_like_float(uint32_t raw);

}  // namespace sdk::mem
