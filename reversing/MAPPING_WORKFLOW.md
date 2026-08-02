# Mapping workflow -- from a raw pointer to a tested `sdk::X` method

The recipe for "map [engine concept] in [one of the five IDBs] and expose it as `sdk::X::method()`".
This file is the MECHANICS. The rules it must obey live in `AGENTS.md` (rules 5a, 6, 9) and
`TESTING.MD` (the evidence contract); it cites them rather than restating them.

## The three reversing documents, and which one you want

| file | answers | when |
|---|---|---|
| **`MAPPING_WORKFLOW.MD`** (this) | *how do I map something* | you are about to reverse |
| **`ENGINE_NOTES.md`** | *what is already known about FEAR2* | before reversing ANYTHING -- check it is not done |
| **`REVERSING_LESSONS.md`** | *how does this go wrong* | a result surprises you, or a technique fails |

`fear2.genny` is the source of truth for LAYOUT. `INTERFACE_HOLDERS.md` lists every LithTech
interface pointer slot in the exe -- 147 holders across 36 interfaces, and usually the fastest anchor.

**Check `ENGINE_NOTES.md` first, every time.** This project has re-derived a mapped structure at
least three times, once destroying a better version of it by writing over the file. One grep of the
concept costs nothing.

## Before you start: a live fixture and the right IDB

```
python tools/resume_game.py     # dead or menu -> injected, in-world, test-ready. Idempotent.
```

Most of this needs a loaded world. At the main menu 103 checks go red for want of a client shell --
legitimate state, not defects. Readiness is `ws_world_ready AND NOT eng_clock_paused`; the loader's
own flag is not enough.

Then point the tools at the right target, in this order:

1. **IDA** -- `list_instances` FIRST, and pick by BINARY NAME. Ports are NOT stable across restarts;
   FEAR2_dump.exe and gameclient.dll have swapped ports mid-session. Then `select_instance`, then
   `server_health`, and verify by **filename**. A silent misroute is real and will attribute every
   rename to the wrong binary.
2. **ReGenny** -- `regenny_attach` needs BOTH `pid` AND `name`; either alone fails with a type error
   about the other. After a game restart ReGenny stays attached to the DEAD process and its module
   list simply comes back without FEAR2.exe.

The five IDBs live in `I:\Programming\projects\fear2recon\*.i64`: FEAR2_dump.exe (the UNPACKED exe --
the on-disk FEAR2.exe is CEG/SteamStub-wrapped and its `.text` is ciphertext), gameclient.dll,
gameserver.dll, gamedatabase.dll, ltmemory.dll.

## Checklist

- [ ] **0. Scope** -- pick the IDB, find an anchor. Check `ENGINE_NOTES.md` and `grep` the SDK first.
- [ ] **1. IDA static** -- decompile, classify by BEHAVIOUR, rename with the `rename` tool, comment the
      evidence, `idb_save`.
- [ ] **2. ReGenny live-verify** -- walk the FULL population; 100% or it is not confirmed.
- [ ] **3. `.genny`** -- author the class; every field CONFIRMED-with-evidence or unverified-with-values.
- [ ] **4. Codegen** -- `regenny:sdk():generate(...)`, migrate renamed fields in the SAME commit.
- [ ] **5. SDK class** -- SEH-guard caller-provided dereferences; consumer API, not just `check_*`.
- [ ] **6. Diagnostics** -- extend an endpoint; data only, never pass/fail.
- [ ] **7. Tests** -- host-side in `fixture_test_runner.cpp`, per TESTING.MD's five validity rules.
- [ ] **8. Close out** -- `--status` before building, scoped `git status`, IDA evidence in the message.

## Phase 0 -- Scope

Identify which of the five binaries plausibly owns the concept, and find an anchor: an export, a
singleton getter, a known method name, or an interface slot from `INTERFACE_HOLDERS.md`.

Two caveats on the holder list, both of which make a slot look wrong when it is not: the slots are
NULL until `CInterfaceDatabase` resolves modules, and `define_holder` is per-translation-unit, so one
interface has MANY equivalent slots with no canonical one. Pick by reader count, not by layout.

The lithtech/FEAR sources in `I:\Programming\projects\fear-source-code` tell you **what a thing is
probably called**. They are a FALSE FRIEND for anything else -- close enough to feel authoritative
and wrong on the specifics that matter. Measured divergences: `LTList` is 16 bytes there and 8 here,
`LTLink` carries `m_pData` there and FEAR2's lists are intrusive, `CAPIHolderBase` has a version field
there and an output pointer here. Never take a layout, a size, a field order, or a member's existence
from it. See AGENTS.md rule 9.

## Phase 1 -- IDA static

Decompile the anchor, follow xrefs, and classify each function by **behaviour** rather than by name
or by the reference SDK's method order:

| body shape | what it is | what it gives you |
|---|---|---|
| `if (a1) return *(a1+K)` | a getter | the field at K |
| `if (a1 && i < *(a1+K1)) return *(a1+K2) + STRIDE*i` | `GetXByIndex` | count, array base AND stride at once |
| `(addr - *(*(addr+K1)+K2)) / STRIDE` | `GetIndexOf` | the child's parent pointer; cross-check against the above |
| hash-prep then tree search | a name lookup | note it exists; do NOT call it (refcounts) |

Highest-yield reads, in rough order:

- **Error strings.** The engine logs its own qualified name on argument-validation paths
  (`"CLTClient::IsServerObject"`). 258 such strings exist, 222 referenced by exactly one function.
  This gives an EXACT name where everything else gives a guess. Require the string to be unique to
  one function -- a shared string names a callee, and one function in this binary demonstrably logs
  the WRONG name.
- **The allocation call** is the struct's size. Address spacing is only an upper bound, and a trap
  when the allocator is a general heap.
- **The constructor** settles field OWNERSHIP (`ctor(this + N)` delimits a sub-object) and explains
  fields whose setter never ran.
- **The destructor** gives the class hierarchy and container SHAPE -- a teardown loop hands you base,
  length and element stride together.
- **The reader, not the writer, gives MEANING.** A setter tells you an offset; only a consumer tells
  you what the value is for.

Then:

- **Rename with the `rename` MCP tool, batched, by address.** `set_type` sets a prototype and does
  NOT rename the symbol -- you will see the signature change while `func_query` still shows
  `sub_XXXXXXXX`. Verify with a FRESH `func_query`.
- If `rename` fails with "Function not found", the address is undefined bytes: use `define_func`.
  Every such gap is a real function IDA's analysis stopped short of.
- Comment the evidence (vtable slot, offset, live sample count) on the handful of functions backing
  a field you are about to commit to the schema.
- `idb_save` after every batch that lands.

Full technique catalogue and the attribution errors that recur:
`REVERSING_LESSONS.md` § *Phase 1 technique notes*.

## Phase 1b -- Do not know who touches a field? TRAP it

**This is mandatory, not a preference.** Scanning the binary for stores to a struct OFFSET answers a
different question than the one you asked and has produced a plausible WRONG answer every single
time it was tried here -- 67 unrelated functions in the case that established the rule.

```
/watch/arm?addr=0x02938740&size=4&type=write|rw|exec&max_hits=4000
/watch/report      -> accessors, registers, value at trap, caller candidates
/watch/clear?all=1
```

Every hit resolves to `module + offset`, so it pastes into the right IDB, and `ecx` is reported
separately because `__thiscall` puts `this` there -- the difference between *"writes +144 on
something"* and *"writes +144 on THE camera"*.

**The loop is: trap it, read it in IDA, walk UP the stack to the function that DECIDES, hook that.**

Two hardware facts, asserted by the suite rather than remembered: a data watch is a TRAP, so the
reported address is the instruction AFTER the accessor (`eip_after`), while an execute watch is a
FAULT and reports the instruction itself; and x86 has no read-only data breakpoint, so a read request
is served by read-or-write.

Worked chain, end to end: `ENGINE_NOTES.md` § *Alt-tab does not pause the game through the focus
flags*.

## Phase 2 -- Live verification in ReGenny

Preconditions: `regenny_status` shows `attached:true`, `sdk_loaded:true`, the project `.genny` open.

The one lookup pattern that works:

```lua
local sdk = regenny:sdk()
local ns  = sdk:global_ns():find_namespace("regenny")   -- NOT find_struct("regenny.X")
if not ns then error("namespace not found -- is the right .genny open?") end
local T = ns:find_struct("MyType")
if not T then error("struct not found -- schema changed?") end
local o = sdkgenny.StructOverlay(address, T)
o.some_count        -- primitives decode to a Lua number
o.some_name         -- strptr/utf8* decode to a Lua STRING directly (no :ptr())
o.some_ptr:ptr()    -- pointers need :ptr() for the raw address, :d() to follow
```

`find_struct("regenny.MyType")` on the GLOBAL namespace does a literal-string lookup and silently
returns `nil`. Null-check every `find_*`. For anything not yet in the schema, read raw:
`proc:read_uint32(addr)`, `proc:read_string(addr, true)`.

The bar for CONFIRMED:

- **Walk the FULL population**, not element 0. Every sample must check out -- a name field decoding
  printable AND a backpointer equalling the owning object's address, 100%, or it is a hypothesis.
- **Prove the engine was EXECUTING** before any claim that a field changes, never changes, or is a
  counter. A suspended game answers IPC perfectly while its own threads are frozen, so a stale read is
  indistinguishable from a constant. This cost a whole conclusion, published to four places.
- **Layout claims must hold on 100%; POLICY claims need not.** "This offset is that field" failing
  anywhere means the map is wrong. "The engine puts objects here under condition X" legitimately
  contains residue from earlier states -- record the rate, never assert it.

The techniques that identify a field cheaply -- exact division for a stride, a value bounded by a
known count that SCALES with it, running the arithmetic backwards to find the field storing a count,
self-describing fields, scanning for the record size's multiply -- are catalogued with their failure
modes in `REVERSING_LESSONS.md` § *Phase 2 technique notes*.

## Phase 3 -- Author `fear2.genny`

- **Self-reference works** and is usually the fix you want -- no forward declaration needed, and it
  is what makes an intrusive list browsable rather than a wall of `void*`:
  ```
  class CClientMgrListLink 0x8 {
      CClientMgrListLink* prev @0x00
      CClientMgrListLink* next @0x04
  }
  ```
- **Forward declarations SHADOW rather than merge.** `struct Foo {}` parses, and then `find_struct`
  returns the EMPTY one (size 0, no fields) while the real definition becomes unreachable. Measured:
  forward-declaring `DatabaseMgrCategory` took it from 0x14 to 0x0.
- **So a true A<->B cycle cannot be fully typed.** Type the owned/array direction (the way you
  browse) and leave the BACKPOINTER `void*`, documenting the real pointee.
- A class must be declared before use; by-value members need the full definition above them.
- A class at the file's top level generates to the wrong path -- it must sit inside
  `namespace regenny { ... }`.
- Every field gets **CONFIRMED** (with the specific evidence) or **unverified** (with the observed
  values and the guess labelled as one). Never silently upgrade.
- `regenny_reload` and check `{"status":"ok"}`. A parse error leaves the PREVIOUS SDK in place, so a
  broken edit can look like it still works.
- **Before codegen, re-verify the hierarchy THROUGH THE TYPE SYSTEM** rather than against hardcoded
  offsets: read each embedding type's `find_variable(...):type():size()` and confirm it is the size
  you expect and not 0. A shadowed type reads 0 and everything downstream still parses.

## Phase 4 -- Regenerate the C++ headers

```lua
regenny:sdk():generate("I:/Programming/projects/fear2/shared/sdk/regenny")
```

- Open the schema, confirm `parse_status == ok`, THEN generate. `regenny:sdk()` faults only when no
  schema has ever parsed on that instance; a FAILED parse does not null it.
- **`open_file` silently DETACHES from the target process.** Re-attach afterwards or every later read
  fails for a reason that looks like the schema.
- The output path is `shared/sdk/regenny`. Generating elsewhere leaves the build compiling the OLD
  field names while the schema says something else.
- **Migrate renamed fields in the SAME commit.** A rename crossing the schema is a four-surface edit:
  the `.genny`, the generated header, the C++ readers, and the prose citing the old name.
- Generated headers are NEVER hand-patched. `Primitives.hpp` is the hand-written shim and stays
  hand-written.
- Forward-declaration ORDER churns on every generation (~60 lines across unrelated headers). Noise --
  confirm no offsets or names moved and move on.

## Phase 5 -- The SDK class

Governed by AGENTS.md 5a and rule 13. The mechanics that trip people up:

- **SEH-guard every dereference of a caller-provided pointer**, in its OWN function scope. MSVC C2712:
  `__try` cannot share a scope with a lambda, a static-local initialiser, or ANY non-POD local --
  including a `std::string` constructed anywhere in that function, even after the block ends. Shape:
  a POD-only helper does the guarded read, a thin non-`__try` wrapper builds the return value.
- For strings, generalise through a `strptr T::* field` pointer-to-member so the struct dereference
  itself (`obj->*field`), not merely the resulting `char*` walk, sits inside the guard -- `obj` can be
  garbled just as easily as the string it points at.
- Guard the field READ; do index-scaled pointer arithmetic outside it (pure address computation).
- **Do not call refcounted `Get`/`Find` vtable entries** from a diagnostic loop without the matching
  Release -- visible in Phase 1 as `++*(result+K)` after a successful search. Traverse the
  count/array-base fields directly instead. `DatabaseMgr::category()`/`record()` is the worked example:
  it walks the arrays rather than calling `IDatabaseMgr::GetCategory(name)`.
- **Map for CONSUMERS.** A `check_*` aggregates a consumer primitive; it never CONTAINS one. If a
  check computes something a mod would want, the computation belongs in the header and the check
  calls it. Delete the private path when you extract it.
- Never hand out a pointer into engine memory that an asset owns -- copy out.
- Do not encode an unresolved question in a name. `pose_a`/`pose_b` beats a guessed `local`/`bind`.
- **A function-local static latches FOREVER, including a failure.** Correct for an exe pattern; wrong
  whenever the prerequisite can arrive later -- `gameserver.dll` is lazy (session start), so a
  resolver warmed at init caches 0 for the process lifetime. Separate RETRYABLE from DEFINITIVE.
- State the API's **thread affinity** in its own comment. Diagnostics run on the IPC thread and must
  not borrow live pointers the game thread owns.

## Phase 6 -- Diagnostics

Extend an endpoint and keep `CommandServer.hpp`'s route doc in sync. Data only, no pass/fail
judgement -- nothing test-shaped ships in the DLL.

Use the `JsonFields` builder, not a format string: three varargs misalignments in one sitting is why
it exists, two of which printed plausible wrong numbers. Any string leaving the process goes through
the escaper (`json_escape_append`) -- Windows paths broke the payload twice.

## Phase 7 -- Tests

Host-side in `test/fixture_test_runner.cpp`, against TESTING.MD's five validity rules. The ones most
often got wrong here:

- **No ReadProcessMemory, no magic values.** A test containing a literal struct offset is broken by
  construction even when it passes. Need a structural relationship? Add an SDK METHOD expressing it
  through the generated schema, report it, assert on that.
- Assert **structural/stable** facts, not volatile content.
- **Prefer an assertion a record can be checked against ITSELF** (a plane and points on it, a
  transform and its inverse), then a stored count, then a cross-walk. Prefer any of them to a number
  you recorded last session.
- **A gated block needs an `else` that REPORTS** -- `check_gated` tallies the skip. A stable check
  count across runs is itself evidence.

```
build.bat
build\bin\fixture-test.exe --injector build\bin\injector.exe --dll build\bin\Fear2vr.dll ^
    --fixture W:\SteamLibrary\steamapps\common\FEAR2\FEAR2.exe --port 8798
ctest --test-dir build -C RelWithDebInfo --output-on-failure
```

Run `fixture-test.exe` standalone FIRST -- you see the full check count and anything ctest's
summary-only output would hide -- then the whole suite.

## Phase 8 -- Close the loop

- `injector.exe --status` before every build; `--unload` and retry if something is resident.
  `LNK1104: cannot open ...fear2vr.dll` usually means the DLL is still injected, not a broken build.
  (`build.bat` unloads first, and its exit code DOES gate -- chain on it, never on a grep of its
  output.)
- Review a scoped `git status --short` before `git add`: codegen writes files you did not hand-edit.
- Put the IDA evidence -- addresses, offsets, live sample counts -- in the COMMIT MESSAGE, not only in
  comments.

## The pitfalls that have cost the most

Each has a full write-up in `REVERSING_LESSONS.md`. These are the ones that recur.

1. **Scanning for an offset to find a writer.** Wrong answer every time. Trap it (Phase 1b).
2. **A value check cannot tell you where a value came from.** `engine_bounds()` read past the end of
   an object and returned CORRECT numbers because the address coincided. Prefer a check that
   constrains the LAYOUT over one that compares contents.
3. **A predicate testing a value's APPEARANCE when the question is its ROLE.** Three instances:
   "points into the module" admitted function pointers as vtables; `get_strlit_contents` read a
   function's code bytes as the string `"Q"`; a printable byte run glued a float's exponent onto a
   name. The tell: a passing condition of "these bytes could be X" rather than "this location is
   used as X".
4. **An extent claimed from a scan.** Five occurrences. A vtable ends where its TERMINATOR says; a
   fixed-size read never licenses a claim about an object's size. Check whether your sample contains
   something that CANNOT repeat (a destructor, a vptr, a type name).
5. **A tolerance widened until the run went green.** Legitimate only when DERIVED from the mechanism's
   own resolution or from float representability. `acos` near dot==1 amplifies error to
   `sqrt(2*eps)` -- 0.02 degrees at single precision, which is why a 0.01 bound was "flaky" for two
   sessions.
6. **Comparing two values sampled at different instants.** Four occurrences. If the game is free to
   move between the reads, you are measuring when they happened. Sample in phase (`Mod::on_frame`), or
   measure the drift over the same window and judge against THAT.
7. **An instrument that can only confirm things about what it measures.** Five wrong conclusions in a
   row came from reading a field the consumer does not consume. When a visible symptom disagrees with
   a green number, the number is aimed at the wrong stage.
8. **A vacuous check.** An oracle that finds nothing agrees with a query that finds nothing. Assert
   the population exists (`hits > 0`, `repeated_names > 0`) BEFORE believing an agreement.
9. **A universal quantifier written from a small look.** "Always", "never", "every", "nothing" --
   count it and cite the number, or write "unmapped".
10. **A wrong name outliving the guess that produced it.** The name is the claim most people read;
    when a finding narrows, fix the identifier, the schema field, the header prose and the commit,
    not just the sentence.

## Where the answers already are

`ENGINE_NOTES.md`, by subsystem:

- **The view chain** -- what the renderer actually reads, why writing most of it is reclaimed, and
  where a head pose belongs (the camera's additive operand at `holder+552`).
- **Rendering and stereo** -- the pass entry, both eyes rendering, the asymmetric frustum, the HUD pass.
- **Weapons, hands and attachments** -- bone control, the viewmodel, ammunition, and the fire ray
  (traced SERVER-side; redirecting it is still open).
- **Player state, aim and zoom** -- the pitch clamp, the zoom state machine, `CPlayerStats`.
- **Input and locomotion** -- the cursor-independent look primitive, snap turn, comfort.
- **World and units** -- one unit is one centimetre, from the engine's own gravity constant.
- **Focus, timing and the engine clock** -- what alt-tab actually stops, and how FocusKeeper works.
- **Stability and known engine bugs** -- the crash the reporter caught, which was the game's.
