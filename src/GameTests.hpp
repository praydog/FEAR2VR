#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

// In-DLL test registry. Tests run INSIDE the game process (the game is the
// fixture; see TESTING.MD) and are driven over IPC: the fixture runner POSTs
// /test, we execute every registered test and return a JSON result object:
//   {"pass":N,"fail":M,"failures":[{"name":"...","error":"..."}, ...]}
//
// Rationale (mirrors the Lua POST pattern from il2cpp-scripting but without a
// scripting runtime): tests ship INSIDE the DLL being injected, so the
// inject -> test -> unload -> rebuild -> re-inject loop needs no game restart.
namespace gametests {

struct TestSink {
    // One boolean observation with a failure message (message kept only on fail).
    void check(bool ok, std::string message);

    bool any_failure() const { return !failures.empty(); }
    std::vector<std::string> failures;
};

using TestFn = std::function<void(TestSink&)>;

// Register a test. Call from Framework::initialize only (startup, before IPC
// serves requests) -- registration is intentionally NOT thread-safe.
void add(std::string name, TestFn fn);

size_t count();

// Run all registered tests; returns the JSON result object fragment described
// above (no outer envelope -- cmdsrv wraps it in {"ok":true,"result":...}).
std::string run_all();

} // namespace gametests
