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
//   GET /sdk/targets             -> JSON object: pattern-resolved engine addresses,
//                                   current live pointers, client_mgr_updating
//                                   (regenny::CClientMgr.updating via
//                                   sdk::CClientMgr::is_updating() -- true only for
//                                   the actual duration of a frame's Update call, so
//                                   almost always false when sampled here),
//                                   counter_elapsed_ms/counter_elapsed_time
//                                   (correlated pair, elapsed_ms==elapsed_time*
//                                   1000 -- advancement semantics UNVERIFIED,
//                                   see reversing/fear2.genny's
//                                   CClientMgrCounterNode comment), and
//                                   start_shell_list_count (bounded walk of a
//                                   generic engine list CClientMgr::StartShell
//                                   populates; 0 at the main menu) (diagnostics).
//   GET /sdk/objects             -> JSON object: per-type live object counts from
//                                   CClientMgr's 7 type-bucketed lists, plus a
//                                   bounded sample of copied-out LTObject
//                                   transforms (position/rotation, with the
//                                   rotation magnitude included as a correctness
//                                   signal). Snapshot-based: the SDK copies fields
//                                   in the same guarded pass that walks the list,
//                                   because these lists mutate live (diagnostics).
//   GET /sdk/database             -> JSON object: DatabaseMgr's own regenny()-mapped
//                                   fields (vtable/array bounds/entry_count) plus, for
//                                   entry0, real category/record enumeration (name,
//                                   record_count, a sample_records list) via
//                                   sdk::DatabaseMgr::category()/record()/*_name() --
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
using ObjectsFn = std::function<std::string()>;                   // full JSON object
using EngineHookFn = std::function<std::string(const std::string& name)>; // full JSON body (with envelope)

struct Handlers {
    HealthFn health{};           // optional; default reports state only
    TargetsFn targets{};         // optional; /sdk/targets 404s without it
    DatabaseFn database{};       // optional; /sdk/database 404s without it
    ObjectsFn objects{};         // optional; /sdk/objects 404s without it
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
