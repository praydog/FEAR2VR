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
- [ ] **3. `.genny`**: forward-declare with `struct Foo {}` for POINTER references (cycles are fine); by-VALUE members need the full definition declared first (a forward-declared type resolves with size 0). Every field: CONFIRMED-with-evidence or unverified-with-observed-values, never silently upgraded. `regenny_reload`, check `status:"ok"`. Re-verify full hierarchy through the type system before codegen.
- [ ] **4. Regenerate**: `sdk:generate(...)`. NEVER hand-patch generated output — fix `Primitives.hpp` or the schema instead.
- [ ] **5. SDK class**: SEH-guard every non-singleton dereference, own function scope (C2712: no lambda/static-init/non-POD locals sharing `__try`). String reads: pointer-to-member so the struct deref itself is guarded. Refcounted vtable lookups need a matching Release — prefer direct struct traversal instead.
- [ ] **6. Diagnostics**: extend an endpoint, keep `CommandServer.hpp`'s route doc in sync. Data only, no pass/fail.
- [ ] **7. Tests**: `fixture_test_runner.cpp` per TESTING.MD's 5 validity rules. Assert stable/structural facts, not volatile exact content. Run standalone first, then full `ctest`.
- [ ] **8. Close out**: `injector --status` before every build (locked DLL ≠ real break). Scoped `git status --short` before `add` (codegen writes files you didn't hand-edit). IDA evidence goes in the commit message.

## Phase 0 — Scope the target

- Identify which of the five IDBs plausibly owns the concept: FEAR2_dump.exe
  (client-side engine/game logic), gameclient.dll, gameserver.dll,
  gamedatabase.dll, ltmemory.dll. See AGENT.MD rule 9 for why FEAR2.exe
  itself (not `_dump`) is useless for static analysis, and rule "Five
  binaries" in TESTING.MD's pitfalls.
- Find an anchor: an exported symbol (`?LTGetXxx@@...`), a singleton getter
  pattern, or a known interface method name from the lithtech/FEAR source
  drop (`I:\Programming\projects\fear-source-code`) — structural analogue
  ONLY (AGENT.MD rule 9's last bullet), never assume exact FEAR2 layout from
  it. It tells you *what the method is probably called and roughly what it
  takes*, not where any field lives.

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

- `.genny` **does** have forward declarations: an empty `struct Foo {}`
  makes the name resolvable before its real definition appears. But it is
  NOT a merge — `find_struct("Foo")` resolves to the forward declaration,
  which has **size 0** and no fields. So:
  - **Pointer members** (`Foo* p`): forward declaration is correct and
    sufficient. A pointer is 4 bytes regardless of the pointee's size, so
    size-0 costs nothing. This is how you break genuinely **cyclic**
    owner/child relationships (owner holds `Child* children`, child holds
    `Owner* parent`) with BOTH sides properly typed. Do not reach for
    `void*` — that throws away type information you actually have.
  - **By-value members** (`Foo embedded`): the forward declaration's size 0
    would silently give you a 0-byte member and corrupt every subsequent
    offset. Declare the real definition FIRST, above its first by-value
    user. (See `CClientMgrListLink`, which is declared early precisely
    because `LTObject` and `CClientMgr` embed it by value.)
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
- `.genny` forward declarations (`struct Foo {}`) DO exist and are the right
  way to break cyclic owner/child references — but they resolve with SIZE 0,
  so they only work for POINTER members. Anything embedding the type BY
  VALUE needs the real definition declared above it, or it silently gets a
  0-byte member. (An earlier version of this doc wrongly claimed `.genny`
  had no forward declarations at all and recommended `void*` backpointers;
  that was wrong — don't discard type info you have.)
- MSVC C2712 (`__try` + non-trivial locals/lambdas/statics-in-initializer in
  the same function) bites almost every time an SEH guard's result needs to
  become a `std::string` — isolate the guard in its own POD-only helper.
