#pragma once

#include <cstdint>
#include <functional>
#include <string>

// Localhost HTTP command server: the DLL <-> tooling bridge (injector, ctest
// fixture driver, ad-hoc curl). DIAGNOSTICS ONLY: the mod ships no test
// assertions; tests live host-side in test/fixture_test_runner.cpp and observe
// through these endpoints (TESTING.MD). Endpoints:
//
//   GET /health                  -> {"ok":true,"unload_requested":false,<fragment>}
//                                   fragment fields supplied by the health handler
//                                   (pid/state/hooks/frame_ticks/sdk_ready).
//   GET /sdk/targets             -> JSON object: pattern-resolved engine addresses
//                                   and current live pointers (diagnostics).
//   GET /sdk/database             -> JSON object: DatabaseMgr's own regenny()-mapped
//                                   fields (vtable/array bounds/entry_count) --
//                                   diagnostics only, no assertions (see
//                                   sdk::DatabaseMgr for the "complex logic lives
//                                   in the SDK class" convention).
//   GET /engine-hook?name=<n>    -> calls cis_GetEngineHook(<n>) in-process;
//                                   {"ok":true,"rc":0,"value":"0x%08X"} on LT_OK,
//                                   rc<0 when our side couldn't call.
//   GET|POST /unload             -> responds ok, then latches unload_requested()
//                                   so the supervisor retires hooks and unmaps.
//
// Threading: ONE accept loop on a dedicated thread serializes all handlers.
// The game thread is never involved, so stop() only joins the socket thread.
namespace cmdsrv {

using HealthFn = std::function<std::string()>;                    // JSON object FRAGMENT (no braces)
using TargetsFn = std::function<std::string()>;                   // full JSON object
using DatabaseFn = std::function<std::string()>;                  // full JSON object
using EngineHookFn = std::function<std::string(const std::string& name)>; // full JSON body (with envelope)

struct Handlers {
    HealthFn health{};           // optional; default reports state only
    TargetsFn targets{};         // optional; /sdk/targets 404s without it
    DatabaseFn database{};       // optional; /sdk/database 404s without it
    EngineHookFn engine_hook{};  // optional; /engine-hook 404s without it
};

// Start listening on 127.0.0.1:port. Idempotent; returns false if the socket
// thread could not be created.
bool start(int32_t port, Handlers handlers);

// Break accept(), join the socket thread. Safe from any thread except the
// server thread itself.
void stop();

bool running();

// Latched by /unload. The supervisor polls this.
bool unload_requested();

} // namespace cmdsrv
