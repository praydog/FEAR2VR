# Mapping Workflow — from a raw pointer to a tested `sdk::X` method

Concrete, replayable recipe for "map [engine concept] in [one of the five
IDBs] and expose it as `sdk::X::method()`". This is the *mechanics*; the
*rules* it must obey live in `AGENT.MD` (5a, 6, 9) and `TESTING.MD` (the
evidence contract) — this file does not restate them, it fills in the "how"
between them and cites the section that governs each phase.

Worked example throughout: `sdk::DatabaseMgr::category()/record()` (see the
`Map IDatabaseMgr category/record enumeration...` commit and
`reversing/fear2.genny`'s `DatabaseMgrCategory`/`DatabaseMgrRecord` comments
for the full evidence trail this recipe produced).

## Checklist (do this; details/rationale below)

- [ ] **0. Scope**: pick the IDB, find an anchor (export/singleton-getter/known method name). Source drop = naming hint only.
- [ ] **1. IDA static**: `select_instance`→`server_health` (verify by FILENAME). Decompile, classify by BEHAVIOR (getter / `GetByIndex` / `GetIndexOf` / hash-lookup). Rename via `rename` tool (NOT `set_type`), verify with fresh `func_query`. Evidence comments on load-bearing funcs. `idb_save`.
- [ ] **2. ReGenny live-verify**: `find_namespace("regenny"):find_struct("X")` (NEVER dotted `find_struct("regenny.X")` — silently nil). Null-check every lookup. Walk the FULL array (every element, not just [0]): name decodes printable AND backpointer matches owner, 100% or it's not confirmed.
- [ ] **3. `.genny`**: SELF-reference by pointer works inside a class's own body (`Foo* next` in `class Foo` — use it, that's what makes a list browsable). Forward decls (`struct Foo {}`) parse but SHADOW the later real definition (size 0, no fields) — useless for a true A<->B cycle, so keep `void*` on the BACKPOINTER and type the owned/array direction. Every field: CONFIRMED-with-evidence or unverified-with-observed-values, never silently upgraded. `regenny_reload`, check `status:"ok"`. Re-verify sizes through the type system before codegen.
- [ ] **4. Regenerate**: `sdk:generate(...)`. NEVER hand-patch generated output — fix `Primitives.hpp` or the schema instead.
- [ ] **5. SDK class**: SEH-guard every non-singleton dereference, own function scope (C2712: no lambda/static-init/non-POD locals sharing `__try`). String reads: pointer-to-member so the struct deref itself is guarded. Refcounted vtable lookups need a matching Release — prefer direct struct traversal instead.
- [ ] **6. Diagnostics**: extend an endpoint, keep `CommandServer.hpp`'s route doc in sync. Data only, no pass/fail.
- [ ] **7. Tests**: `fixture_test_runner.cpp` per TESTING.MD's 5 validity rules. Assert stable/structural facts, not volatile exact content. Run standalone first, then full `ctest`.
- [ ] **8. Close out**: `injector --status` before every build (locked DLL ≠ real break). Scoped `git status --short` before `add` (codegen writes files you didn't hand-edit). IDA evidence goes in the commit message.

## Phase 0 — Scope the target

- Identify which IDB plausibly owns the concept: FEAR2_dump.exe (client-side
  engine/game logic), gameclient.dll, gameserver.dll, gamedatabase.dll,
  ltmemory.dll. See AGENT.MD rule 9 for why FEAR2.exe itself (not `_dump`) is
  useless for static analysis, for the IDB locations, and for the
  port-shuffling hazard; plus rule "Five binaries" in TESTING.MD's pitfalls.
  NOTE (2026-07): only four are open in IDA -- ltmemory.dll is NOT currently
  loaded, so LTMemory internals (e.g. the pool allocator's vtables at
  0x10004274/0x10004254) can be reasoned about from FEAR2.exe's side and from
  live memory, but cannot be renamed or decompiled until it is reopened.
- Find an anchor: an exported symbol (`?LTGetXxx@@...`), a singleton getter
  pattern, or a known interface method name from the lithtech/FEAR source
  drop (`I:\Programming\projects\fear-source-code`) — structural analogue
  ONLY (AGENT.MD rule 9's last bullet), never assume exact FEAR2 layout from
  it. It tells you *what the method is probably called and roughly what it
  takes*, not where any field lives.
- **Start from `reversing/INTERFACE_HOLDERS.md` when the concept is an engine
  interface.** It lists every LithTech interface pointer slot in FEAR2.exe --
  147 holders across 36 interfaces (ILTClient, ILTServer, ILTModel.Client/
  .Server, ILTPhysics, ILTRenderer, ILTDrawPrim, ILTTextureMgr, IClientShell,
  IServerShell, ...) with each slot's address, recovered by enumerating every
  call site of `CAPIHolder_ctor`. That is usually a faster anchor than a
  string or export search: pick the interface you want, take a slot address,
  and xref it to find the code that actually uses that interface.
  Two caveats: the slots are NULL until CInterfaceDatabase resolves modules,
  and `define_holder` is per-translation-unit so one interface has many
  equivalent slots (no single canonical one).

## Phase 1 — IDA static reversing

- `select_instance` then `server_health`, confirm by **filename**, not the
  `success`/`active` flags (`skill://ida-friction` §16 — silent misroute is
  real and will make you attribute findings to the wrong binary).
- Locate the anchor, decompile, follow xrefs (`trace_data_flow`/`xrefs_to`).
- For a vtable-based interface (COM-style, e.g. `IDatabaseMgr`): find the
  vtable's static address (decompile the exported getter; the singleton's
  first field is usually `&vtable`), `get_bytes` the `N * 4` region, parse as
  little-endian addresses, `analyze_batch` with `include_decompile` on all N
  in one or two calls. Classify each by **behavior**, not by guessing:
  - `if (a1) return *(a1+K)` → a getter for a field at offset K.
  - `if (a1 && idx < *(a1+K1)) return *(a1+K2) + STRIDE*idx` → an
    `GetXByIndex(container, index)` — this pins down `count@K1`,
    `array_base@K2`, and the element stride simultaneously, and is usually
    your single highest-value find (it hands you an entire array's layout
    from one function).
  - `(addr - *(*(addr+K1)+K2)) / STRIDE` → the inverse, `GetIndexOf`; confirms
    a child's *parent* pointer offset (`K1`) and the parent's array-base
    offset (`K2`) — cross-check these against what `GetXByIndex` already told
    you; they must agree.
  - String-hash/tree-lookup bodies (calls into a hash-prep + tree-search
    helper) are name-based `Get`/`Find` calls — useful to know exist, but do
    **not** wire live SDK calls through them without a matching Release path
    (see "Refcounted lookups" below).
- **Before attributing a field to a class, prove which object the function's
  `this` actually is.** A `__thiscall` body writing `*(ecx + 0x1424)` tells you
  an offset, NOT whose offset. If the decompiler did not resolve `ecx` at the
  call site, you have a field on *something*, and assuming it belongs to the
  caller's class is a guess wearing evidence's clothes.
  This produced a wrong mapping in 2026-07: `FrameDelta_TickAndReport` (dump
  0x409FD0) does `*(float*)(ecx + 0x1424) += 1.0`, and `CClientMgr::Update`
  calls it, so `CClientMgr+0x1424` got named `frame_counter`. Live sampling
  then showed that field advancing by non-integral amounts -- it is not the
  `+= 1.0` accumulator, and the attribution was never established. The name
  was retracted the same session and the function lost its `CClientMgr_`
  prefix, because even the prefix asserts the thing that is unproven.
  Provable attribution looks like: the offset is read inside a method whose
  `this` IS the class (e.g. `CClientMgr::Update` reading `this + 5120`
  directly), or the call site visibly loads `ecx` from that object.
- Two functions at *different vtable slots* sharing the *identical* body
  address is real (ICF/linker folding when two methods compile identically —
  e.g. `GetNumRecords`/some other `GetNumX` both reducing to `return *(a1+8)`).
  Don't over-claim which slot means what beyond what you can attribute to a
  live-verified offset; document the ambiguity rather than picking one.
- Rename: use the dedicated `rename` MCP tool (batched, by **address**), not
  `set_type` — `set_type(kind="function", name=..., signature=...)` sets the
  prototype (parameter names/types) but does **not** rename the symbol; you
  will see the signature change in `decompile` output while `func_query`
  still shows `sub_XXXXXXXX`. Verify with a **fresh** `func_query` after
  every batch (skill://ida-friction §3/§13).
- Add evidence comments (`diff_before_after`, action `set_comment`) on the
  handful of functions that most directly back a field/offset you're about
  to commit to `fear2.genny` — cite the vtable slot, the offset it reads,
  and the live sample count that confirmed it.
- `idb_save` after every batch that lands (AGENT.MD rule 9).

## Phase 2 — Live-memory verification via ReGenny

Preconditions: `regenny_status` shows `attached:true`, `sdk_loaded:true`,
`open_file` is the project's `.genny`.

**The one lookup pattern that actually works** (get this wrong and you get a
silent `nil` at best, a crash on old luagenny builds at worst):

```lua
local sdk = regenny:sdk()
local ns = sdk:global_ns():find_namespace("regenny")  -- NOT find_struct("regenny.X")
if not ns then error("namespace not found -- is the right .genny open?") end
local MyType_t = ns:find_struct("MyType")
if not MyType_t then error("struct not found -- schema changed?") end
```

`find_struct("regenny.MyType")` on the *global* namespace does a **literal
string** lookup and returns `nil` (the type is named `MyType`, nested inside
namespace `regenny` — dotted notation is how `.genny` *files* reference
namespaced types, not how the Lua API looks them up). Always null-check
every `find_*` result before constructing an overlay from it — a
`StructOverlay`/`PointerOverlay` built from a `nil` type used to null-deref
on first field access instead of erroring (fixed upstream in
`praydog/luagenny`, commit `1e262a6` — if you hit an unexplained ReGenny
crash mid-session on an old build, suspect this class of bug first).

Construction and field access:

```lua
local overlay = sdkgenny.StructOverlay(address, MyType_t)
overlay.some_count        -- primitive fields auto-decode to a Lua number
overlay.some_name         -- strptr/utf8* fields auto-decode to a Lua STRING
                           --   directly -- no :ptr() on these
overlay.some_ptr:ptr()    -- pointer fields need :ptr() for the raw address
overlay.some_ptr:d()      -- or :deref()/:dereference() to follow it
```

For anything not yet in the schema, read raw process memory directly:
`proc:read_uint32(addr)`, `proc:read_string(addr, true)`.

**Any claim about a field CHANGING requires proof the engine was executing.**
Static values prove nothing about dynamics if the process was not running.
Before concluding "this advances" / "this never moves" / "this is a counter":
confirm frames are actually being produced -- `/health`'s `frame_ticks`
advancing between two polls is the cheap check, and the process consuming CPU
is the OS-level one.

This burned a whole conclusion in 2026-07: `CClientMgrCounterNode.elapsed_ms`
was recorded as "re-sampled minutes apart and it did NOT advance", and that
went into the schema, the SDK header, the route doc and a test comment. The
samples had been taken against a game the user had deliberately SUSPENDED --
its own threads frozen while our injected IPC thread kept answering, so every
read returned a stale-but-valid value and looked like a static field. Reading a
frozen engine is indistinguishable from reading a constant. The claim was
retracted from all four places.

Corollary: a suspended or paused fixture still answers IPC perfectly, because
threads created after the suspend keep running. "The endpoint responded" is NOT
evidence the game is live.

**Validate before you commit to the schema.** The bar is the same one
TESTING.MD sets for the shipped SDK: *every* sample must check out, not "the
first one looks plausible". For an array hypothesis (count field + array-base
field + stride from Phase 1's `GetXByIndex`), walk **all** N elements and
assert two things per element: the field you think is a name/string decodes
as printable ASCII, and the field you think is a parent backpointer equals
the actual owning object's address. A 100% hit rate across the full live
array (not just element 0) is what makes an offset CONFIRMED rather than a
hypothesis — this is the same distinction TESTING.MD draws between "IDA
static evidence justifies writing the mapping" and "the fixture justifies
keeping it", just run one phase earlier, interactively, before any C++ exists
to fixture-test.

## Phase 3 — Author/extend `fear2.genny`

- **Self-reference works, and is the fix you usually want.** A class may
  point to itself inside its own body -- no forward declaration needed:
  ```
  class CClientMgrListLink 0x8 {
      CClientMgrListLink* prev @0x00
      CClientMgrListLink* next @0x04
  }
  ```
  Verified: size stays 0x8, both fields type as `CClientMgrListLink*`, and
  every by-value embedder (`LTObject.list_link`, `CClientMgr.object_lists`)
  keeps its size. Type intrusive list links this way -- `void*` links are
  what make lists un-browsable in the ReGenny UI.
- **Forward declarations SHADOW, they do not merge -- do not use them.**
  `struct Foo {}` parses fine, but with one present `find_struct("Foo")`
  returns the EMPTY declaration (size 0, no fields) and the later real
  definition becomes unreachable. Measured on this build: forward-declaring
  `DatabaseMgrCategory` took it from size 0x14 to 0x0 with `name` MISSING.
  A forward decl is therefore only safe for a type you never define fully,
  which defeats the purpose.
- **So a genuine A<->B cycle cannot be fully typed.** One side must stay
  `void*`. Choose deliberately: type the **owned/array direction** (the way
  you browse) and leave the **backpointer** untyped, documenting the real
  pointee and the evidence. `DatabaseMgrCategory.records` is
  `DatabaseMgrRecord*` while `DatabaseMgrRecord.owner_category` is `void*`
  for exactly this reason.
- Ordering still matters for everything else: a type must be declared
  before it is *used*, and **by-value** members need the real definition
  above them. (`CClientMgrListLink` sits early because `LTObject` and
  `CClientMgr` embed it by value.)
- Verify either way: after `regenny_reload`, read the embedding type's
  `find_variable(...):type():size()` through the type system and confirm
  it is the size you expect, not 0.
- Every field gets a comment stating either **CONFIRMED** (cite the specific
  evidence — vtable offset + behavior match from Phase 1, live sample count
  from Phase 2) or **unverified** (state the actual observed value(s) and
  your best plausible-but-unproven guess, clearly labeled as a guess — see
  `HashEntry`/`DatabaseMgrRecord`'s `unk_*` fields in `fear2.genny` for the
  house style). Never silently upgrade a guess to a confirmed name.
- Reload after every edit (`regenny_reload`); check `{"status":"ok"}` before
  trusting anything downstream — a parse error leaves the *previous* parsed
  SDK in place, so a broken edit can look like it "still works" if you don't
  check the status.
- Before moving to codegen, re-verify the **full** hierarchy one more time
  through the type system (`find_namespace`/`find_struct`/`StructOverlay`,
  reading `:offset()`/`:size()` from the parsed types) — not hardcoded
  offsets. This is the final gate before the schema becomes generated code.

## Phase 4 — Regenerate C++ headers

- `sdk:generate("shared/sdk/regenny")` via `regenny_lua_eval`.
- Generated headers are **never hand-patched.** If something's missing (a
  primitive like `strptr`/`wstrptr` has no C++ definition of its own in
  generated output), fix it once in `shared/sdk/regenny/Primitives.hpp`
  (hand-written, permanent shim — see AGENT.MD 5a) or fix the `.genny`
  schema, then regenerate. If a generated field is the wrong C++-usability
  shape for what you need (e.g. you want `offsetof`/`sizeof` off the real
  type), that's a signal to adjust the `.genny` type, not to patch the
  output.

## Phase 5 — Extend the SDK class (`shared/sdk/X.hpp`/`.cpp`)

Governed by AGENT.MD 5a; the mechanics that trip people up:

- Every raw-memory dereference that isn't through the **trusted root
  singleton** (i.e. any caller-provided handle — a child object obtained
  from a previous traversal step, not `X::get()`'s own `this`) gets
  SEH-guarded, in its **own function scope** (MSVC C2712: `__try` cannot
  share a scope with a lambda, a static-local initializer, or *any*
  non-trivial/non-POD local or return type — including a `std::string`
  constructed anywhere in that function, even after the guarded block ends).
  AGENT.MD rule 6 has the precise rule; the fix shape is always: a POD-only
  (raw pointer / integer status + out-buffer) helper does the actual guarded
  dereference, and a thin non-`__try` wrapper builds the real return value.
- For string reads, generalize via a `strptr T::* field` pointer-to-member
  helper so the **struct dereference itself** (`obj->*field`), not just the
  resulting `char*` walk, stays inside the guard — `obj` can be a garbled
  pointer just as easily as the string it points to.
- For count/array-base reads on caller-provided handles: SEH-guard the field
  *read* (as a raw integer/pointer value), then do index-scaled pointer
  arithmetic **outside** the guard — that's pure address computation with no
  dereference, so it doesn't need protecting, and keeping it out keeps the
  guarded helpers small and reusable.
- "Refcounted lookups" — a vtable `Get`/`Find` that increments a refcount on
  a match (visible in Phase 1 as `++*(result+K)` after a successful
  tree/hash search) need a matching `Release` before you can safely call
  them repeatedly from a diagnostic or test loop. Prefer exposing the
  **already-loaded** data via direct struct traversal (count/array-base
  fields, as above) instead of calling the refcounted lookup, unless you've
  also mapped and wired the release path — this is why
  `DatabaseMgr::category()`/`record()` walk `categories`/`records` arrays
  directly rather than calling `IDatabaseMgr::GetCategory(name)`.

## Phase 6 — Diagnostics

- Extend an existing endpoint or add a new one in `src/ipc/CommandServer` —
  and update the route doc comment block at the top of `CommandServer.hpp`
  to match (it's the source of truth for "what does this endpoint return",
  don't let it drift).
- Diagnostics format only — data, never pass/fail judgement (AGENT.MD rule 2,
  TESTING.MD "Nothing test-shaped ships in fear2vr.dll").

## Phase 7 — Tests

- Add assertions to `test/fixture_test_runner.cpp` against TESTING.MD's 5
  validity rules (residency, cross-check, live-behavior-over-static-shape,
  rejection-semantics, freshness-after-reload) — whichever apply to the new
  surface.
- Assert **structural/stable** facts (a known-stable name exists in an
  enumerated list, an array's JSON shape, a plausible numeric range) rather
  than exact volatile content (first-hit ordering, exact record text) —
  unless that exact content is itself the thing under test. Game data and
  ordering can shift with patches/localization; the mapping's correctness
  shouldn't be coupled to that.
- Run `fixture-test.exe` standalone first (see the full check count and
  catch anything ctest's summary-only output would hide), then the whole
  suite (`ctest --test-dir build -C RelWithDebInfo --output-on-failure`) —
  TESTING.MD step 5.

## Phase 8 — Close the loop

- `injector.exe --status` before **every** build — a `LNK1104: cannot open
  ...fear2vr.dll` link failure almost always just means the DLL is still
  injected, not a real build break. `--unload` and retry.
- Review a scoped `git status --short` before `git add` — know exactly which
  files are yours; never a blind `-A` without checking (this matters more
  than usual here, since `sdk:generate()` writes files you didn't hand-edit).
- Put the IDA evidence (addresses, offsets, live sample counts) **in the
  commit message**, not only in code comments — AGENT.MD rule 3's "commit
  after each coherent unit" plus this makes the mapping's provenance
  reviewable without re-opening IDA.

## Gotchas log (append here as new ones are found)

- IDA `set_type(kind="function", name=..., signature=...)` sets the
  prototype but does **not** rename the symbol — use the `rename` tool,
  verify with a fresh `func_query`.
- ReGenny `find_struct("ns.Type")` with a dotted name silently returns `nil`
  — use `find_namespace("ns"):find_struct("Type")`.
- A `StructOverlay`/`PointerOverlay` built from a `nil` type used to
  null-deref instead of erroring cleanly — fixed in `praydog/luagenny`
  commit `1e262a6`; null-check every `find_*` result regardless, since the
  clean-error behavior is still "your script is wrong", just no longer
  "your ReGenny process is gone".
- `.genny` forward declarations (`struct Foo {}`) parse but **shadow** the
  real definition -- `find_struct` returns the empty one (size 0, no
  fields). They cannot break a cycle. Measured: forward-declaring
  `DatabaseMgrCategory` dropped it 0x14 -> 0x0. Use SELF-reference (a class
  pointing at itself in its own body) where that suffices; otherwise keep
  `void*` on the backpointer side.
  (This doc has been wrong here twice: first claiming forward declarations
  don't exist, then claiming they work for pointer members. Both were
  written without testing the resulting SIZE. Test it.)
- MSVC C2712 (`__try` + non-trivial locals/lambdas/statics-in-initializer in
  the same function) bites almost every time an SEH guard's result needs to
  become a `std::string` — isolate the guard in its own POD-only helper.
- **A surprising address, port, module name or count is a STOP, not a thing to
  rationalise.** The IDA MCP silently re-routes between IDBs (skill://ida-friction
  §16), and it happened twice in one session: once mid-analysis (active IDB had
  drifted to `gameserver.dll` on its own) and once as an unexpected port in an
  output-spill URL, which I explained away as "probably the proxy" and carried
  on renaming ~300 globals. It turned out to be benign, but that was luck, not
  reasoning — had it not been, every rename would have landed in the wrong
  binary. Cost of checking: one `server_health` call, verified BY FILENAME.
  Do it before the next write, every time, and re-verify after any batch that
  surprised you.
- **`scan_relative_references` (plural) does not dedupe.** It splits the module
  across threads with 4-byte segment overlap, so a displacement in an overlap is
  reported twice and any count derived from it varies with core count. Sort +
  unique. (Found by reading kananlib's implementation, which is the general
  lesson: read the primitive before depending on its result shape.)
