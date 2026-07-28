#pragma once

#include <cstdint>
#include <functional>
#include <string>

// Localhost HTTP command server: the DLL <-> tooling bridge (injector, ctest
// fixture driver, ad-hoc curl). Modelled on il2cpp-scripting's luaeval server
// but WITHOUT Lua: handlers are plain C++ callbacks registered by the host,
// executed directly on the server thread. Endpoints:
//
//   GET  /health  -> {"ok":true,"state":...,"unload_requested":false,...}
//                    (counters/state supplied by the health callback)
//   GET|POST /test -> runs the host's in-DLL test registry, returns
//                    {"ok":true,"result":{"pass":N,"fail":M,"failures":[...]}}
//   GET|POST /unload -> responds ok, then flags unload_requested() so the
//                    supervisor retires hooks and unmaps the DLL.
//
// Threading: ONE accept loop on a dedicated thread serializes all handlers
// (handle_client runs on it). The game thread is never involved, so stop()
// only has to join the socket thread -- no game-thread pump exists (yet).
namespace cmdsrv {

using HealthFn = std::function<std::string()>;  // returns JSON object FRAGMENT (k:v pairs, no braces)
using TestFn = std::function<std::string()>;    // returns JSON {"pass":N,"fail":M,"failures":[...]}

struct Handlers {
    HealthFn health{}; // optional; default reports pid only
    TestFn test{};     // optional; /test 404s without it
};

// Start listening on 127.0.0.1:port. Idempotent; returns false if the socket
// thread could not be created. NOTE: start() returns before the bind happens;
// a busy port shows up as running()==false once the loop fails.
bool start(int32_t port, Handlers handlers);

// Break accept(), join the socket thread. Safe to call from any thread EXCEPT
// the server thread itself. After stop(), unload_requested() observations made
// earlier remain latched.
void stop();

bool running();

// Latched by /unload. The supervisor polls this.
bool unload_requested();

} // namespace cmdsrv
