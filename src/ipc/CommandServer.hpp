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
//                                   populates; 0 at the main menu),
//                                   counter_node_registered (the CClientMgr_Init
//                                   wiring invariant, computed from the schema's
//                                   own offsetof -- see
//                                   sdk::CClientMgr::counter_node_registered),
//                                   last_sample_time_ms (the ms timestamp Update
//                                   differences for its frame delta; unit
//                                   confirmed, zero point and WRITER not --
//                                   Update only reads it, so its advancement is
//                                   not evidence about frames specifically),
//                                   and pending_shell_release (true
//                                   only inside the deferred-destruction window
//                                   that CClientMgr.updating guards, so like
//                                   client_mgr_updating it reads false here
//                                   essentially always) (diagnostics).
//   GET /sdk/models              -> JSON object: the CONSUMER-facing model API
//                                   exercised through sdk::Model -- per model its
//                                   .mdl path, node count, material count, and for
//                                   each of Head/L_Hand/R_Hand that it has, the
//                                   node index, the name read BACK from that index
//                                   (round_trip), its parent, its depth from the
//                                   root, and its first pose vector. Unlike
//                                   /sdk/objects this touches no offsets and no
//                                   schema types: it is what a mod would call, so
//                                   it breaks when the API breaks rather than when
//                                   a field moves.
//   GET /sdk/objects             -> JSON object: per-type live object counts from
//                                   CClientMgr's 7 type-bucketed lists, with
//                                   bucket_names[] giving each index's
//                                   sdk::ObjectType name (OT_NORMAL..
//                                   OT_PARTICLESYSTEM), plus a bounded sample of
//                                   copied-out LTObject transforms
//                                   (address/vtable/type/handle/pos/rot and a
//                                   rotation magnitude as an offset sanity
//                                   signal). all_terminated reports whether
//                                   every bucket walk closed cleanly AND every
//                                   object's type matched its bucket.
//                                   Sampling uses snapshot_objects(), which
//                                   copies fields inside the same guarded pass
//                                   that walks the list, because these lists
//                                   mutate live and this runs off-thread.
//                                   engine_walk_* reports the engine thread's
//                                   in-place for_each_object walk: reading this
//                                   endpoint RAISES a one-shot request that the
//                                   frame hook services, and reports the last
//                                   published result (count -1 = none yet).
//                                   Non-blocking, so a paused game simply never
//                                   advances engine_walk_generation. Compare the
//                                   generation across two polls to know a fresh
//                                   walk landed (diagnostics).
//   GET /sdk/interfaces          -> JSON object: the LithTech interface layer.
//                                   Header: ctor (runtime CAPIHolder_ctor addr),
//                                   call_sites / holders (runtime rediscovery
//                                   counts -- must match the 147 recorded by
//                                   static reversing), names / expected_names
//                                   (discovered vs. generated set).
//                                   Then one entry per interface INSTANCE:
//                                   holders (how many translation units asked
//                                   for it), non_null (how many slots currently
//                                   hold a pointer), all_agree, value, plus
//                                   getter/getter_matches -- the value obtained
//                                   by calling that interface's own generated
//                                   typed getter, so the report exercises the
//                                   public API and not just the registry.
//                                   value 0 is NORMAL: slots are filled by
//                                   APIFound() and cleared by APIRemoved(), so
//                                   an interface reads null before module
//                                   resolution or after an unload. all_agree
//                                   false is the real anomaly (diagnostics).
//   GET /sdk/database             -> JSON object: DatabaseMgr's own regenny()-mapped
//                                   fields (vtable/array bounds/entry_count) plus, for
//                                   entry0, real category/record enumeration (name,
//                                   record_count, a sample_records list) via
//                                   sdk::DatabaseMgr::category()/record()/*_name() --
//                                   diagnostics only, no assertions (see
//                                   sdk::DatabaseMgr for the "complex logic lives
//                                   in the SDK class" convention).
//   GET /engine-hook?name=<n>    -> calls cis_GetEngineHook(<n>) in-process;
//                                   {"ok":true,"rc":0,"value":"0x0040CC5E"} on LT_OK
//                                   (pointer-valued fields are printed full-width
//                                   from uintptr_t, never truncated),
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
using ModelsFn = std::function<std::string()>;                    // full JSON object
using InterfacesFn = std::function<std::string()>;                // full JSON object
using EngineHookFn = std::function<std::string(const std::string& name)>; // full JSON body (with envelope)

struct Handlers {
    HealthFn health{};           // optional; default reports state only
    TargetsFn targets{};         // optional; /sdk/targets 404s without it
    DatabaseFn database{};       // optional; /sdk/database 404s without it
    ObjectsFn objects{};         // optional; /sdk/objects 404s without it
    ModelsFn models{};           // optional; /sdk/models 404s without it
    InterfacesFn interfaces{};   // optional; /sdk/interfaces 404s without it
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
