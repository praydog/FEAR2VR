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
- **The same hazard runs BACKWARDS: searching for a field's writer by OFFSET
  finds every class that happens to share that offset.** Looking for who assigns
  `LTObject.slot_index` (+0xA8), an instruction scan for `mov [reg+0A8h], ...`
  returned **70 sites across many unrelated classes**. One of them,
  `SlotIndex_AcquireOrRelease` (0x444B9B), was a textbook match: allocate a slot
  when a predicate holds and none is held, free it and store -1 otherwise --
  precisely the shape of the -1 sentinel already mapped. Its gate even reads
  offsets that look LTObject-shaped (a BYTE type at +0x10, +0x38, a DWORD flag
  word at +0x3C, a WORD flag word at +0x46, +0xEC).
  It is still not the writer. **Transcribe the predicate and evaluate it over
  live memory** -- that is the cheap decisive test, and here it returned TRUE on
  **0 of 3583** objects while 3248 hold slots. One clause (`flags3 & 4`) passes
  on 0/3583 while every other passes on 2138..3583, which localises the
  disagreement instead of leaving a vague doubt.
  Two things worth keeping from this:
  * A predicate is *executable evidence*. When a candidate function claims to
    gate a field, run its condition against the live population and compare to
    the field's actual distribution. Agreement is strong support; total
    disagreement is a refutation, and a per-clause breakdown tells you which
    assumption broke.
  * **Write the negative result down next to the field.** A ruled-out candidate
    that looks this convincing will be rediscovered and re-believed otherwise.
    `slot_index`'s comment records the address, the measurement and the number,
    so the next pass starts after this dead end rather than inside it.
- **When you need to know WHICH object a `__thiscall` was invoked on, read the
  disassembly — the decompiler routinely drops it.** Hex-Rays renders a call
  whose `this` it could not name as an ordinary call and silently discards the
  `mov ecx, ...` that set it up. In `LTObjectOwner_UpdateSpatialRecord` the
  pseudocode showed a bare `g_pIWorldClientBSP->slot14()` whose result went
  nowhere, followed by `CollectSphere(record, volume)`. The disassembly shows
  what actually happens:
  ```
  mov ecx, g_pIWorldClientBSP_Default_6ECD04
  call dword ptr [eax+38h]   ; slot 14 -> returns the vis-tree manager
  mov ecx, eax               ; ... which BECOMES CollectSphere's `this`
  call LTSpatialRecord_CollectSphere
  ```
  That one dropped instruction is the anchor for an entire subsystem: it is how
  the visibility tree is reached from a named global. A "return value that is
  never used" sitting immediately before another call is the tell — in a
  `__thiscall` ABI it usually became the next call's `this`.
- **To close an open question, try hardest to confirm the explanation you find
  most plausible — and let it fail.** Refutation localises much faster than
  confirmation. `OT_CAMERA` holding zero visibility associations had two candidate
  causes: geometric (their volumes miss every sector) or a code gate. The
  geometric story was the intuitive one, so it got tested first: 459 of 474
  camera volumes DO overlap at least one sector. With geometry eliminated the
  answer had to be in the code path, and the gate
  (`(flags & 1) && !(flags2 & 0x700)`) was two lines of disassembly away — no
  camera has flags bit 0. Had the gate been checked first and matched, the
  geometric contribution would still have been unmeasured and the note would have
  read "probably the gate" instead of a number.
- **The DESTRUCTOR CHAIN gives you the class hierarchy, and derived-class fields
  are the commonest thing to mis-attribute without it.** MSVC emits, for each
  class in a chain, a destructor that installs ITS OWN vtable and then calls the
  base's. So reading one destructor names one inheritance edge:
  `OT_CAMERA_scalar_dtor` installs `vtbl_OT_CAMERA` then calls
  `OT_WORLDMODEL_dtor_body`, which installs `vtbl_OT_WORLDMODEL` then calls
  `LTObject_dtor_body` — i.e. `LTObject <- WorldModel <- Camera`.
  This corrected a real mistake. The cached transform pair at +0xDC had been
  mapped onto `LTCameraObject` as camera-specific for several passes. It is
  WorldModel state: the type-2 allocator hands out 0x13C and the pair ends at
  exactly 0x13C, while type 5 is 0x140 and adds only a uint16. So 1473 objects
  carry those matrices, not 474 — a nearly 4x difference in what a mod can reach.
  Two habits follow:
  * **Read the destructor before writing a derived class.** It is usually a
    handful of instructions and it tells you both the parent and (from the calls
    it makes before delegating) which fields this level owns. `this[51]` released
    by WorldModel's body is how `unk_CC` was identified as its first own field.
  * **A vtable survey that shows two types sharing most slots is a HINT of
    inheritance, not an explanation.** The earlier survey had already recorded
    that types 2 and 5 differ only in slots 0 and 1 and left it as a curiosity.
    Sharing slots is what inheritance looks like from the outside; go get the
    destructor and confirm the direction.
  Also: sharing slots measures OVERRIDE DENSITY, not depth. OT_NORMAL and
  OT_PARTICLESYSTEM share slots 1 and 3..15 and are SIBLINGS; OT_MODEL shares no
  slot with anything and is also a direct child of LTObject. Only one real edge
  existed in the whole family, and the slot table did not identify which.
- **`ctor(this + N)` DELIMITS A SUB-OBJECT, and any field you already mapped
  inside that span belongs to it, not to the outer class.** When a constructor
  passes `this + N` as another function's `this`, everything that callee touches
  is at offsets relative to N. Read the callee, add N to every offset it writes,
  and check that range against what you have already named on the outer class.
  `LTModelObject` had `asset @0xEC` on it. Then `LTModelRecord_ctor` turned up,
  called on `this + 0xCC`, zeroing its own `+0x20` -- and 0xCC + 0x20 is 0xEC.
  The offset was right; the OWNER was wrong. Restructuring it into a real
  embedded record immediately paid: the model's list members are records of that
  same class, so each member's `+0x20` should be the owner's asset, and live it
  is on 289/289 -- a check that did not exist while the field was misfiled.
  This is the SECOND time this exact error class showed up (the transform pair
  sat on `LTCameraObject` before the destructor chain moved it to WorldModel),
  and both times a CONSTRUCTOR was what settled ownership. The cost of getting it
  wrong is not a bad offset -- reads still work -- it is that every derived fact
  is attributed to the wrong population: 474 cameras instead of 1473
  worldmodels, one asset field instead of a list of records each holding one.
  So: before declaring a field on a class, check whether a ctor hands some
  sub-range of the object to another function. If it does, that range is a class.
- **A TEARDOWN LOOP hands you a container's base, its length AND its element
  stride at once -- take the stride from the code, never from your knowledge of
  the library.** LTModelObject's cleanup calls `0x429771(this+0x114, this+0x118)`,
  which is `for (i = count; i; --i) { ~string(p); p += 28; }`. That single loop
  says: 0x114 is an array base, 0x118 is its length, the element is a
  std::string, and the element is 28 bytes. Three of those four are structure you
  would otherwise have to infer, and the fourth -- 28 -- is the one worth
  dwelling on. MSVC's std::string is 24 bytes in the shape most people remember
  and 28 with `_SECURE_SCL`; had the stride been assumed rather than read, every
  string after the first would decode from the wrong address. Reading it made the
  layout a measurement instead of a bet, and the payoff was immediate: those
  strings are MATERIAL PATHS ("weapons\_global\shellcasings\...\
  assault_rifle_casing1.mat"), the first field in this mapping where an object
  says in plain text what it IS. For recon that is worth more than a dozen
  numeric fields -- identifying an object by path beats inferring it from
  geometry.
  Generally: destructors are the best place to learn container SHAPE, the way
  constructors are the best place to learn field OWNERSHIP. A dtor must know
  exactly how many elements there are and how far apart they sit; a ctor must
  know which sub-object each range belongs to. Neither has the option of being
  vague, which is what makes both more reliable than any reader you find later.
- **THE ALLOCATION CALL IS THE SIZE. Address spacing is only an upper bound, and
  it is a trap when the allocator is a general heap.** `LTModelAsset_FindOrLoad`
  does `LTMem_Alloc(0xA0)`, so the class is 160 bytes -- and its ctor's last write
  ends at exactly 0xA0, corroborating from the other side. Before finding that
  call I had concluded 0xC8, from a genuinely good-looking argument: the 34 live
  asset addresses are separated by exact multiples of 0xC8, minimum 0xC8, which is
  precisely the reasoning that correctly sized the object types from
  `CClientMgr.object_banks`. The difference is that objects come from FIXED-SIZE
  POOLS, where adjacent blocks are exactly one element apart; assets come from the
  general heap, where spacing reflects the allocator's bucketing and tells you
  nothing except "not larger than this". Same observation, opposite reliability,
  and the only way to tell is to know which allocator you are looking at.
  A second inference failed alongside it, worth recording because it looked like
  independent support: "the bytes from 0xA0 to 0xC8 are all zero, so the object
  probably ends around 0xA0". That was true of the ONE asset I had dumped and true
  of ZERO of the 34 once checked -- those bytes belong to the next allocation. Two
  weak arguments pointing at different answers, and the strong one was a single
  call instruction away.
- **Eyeballing the top of a distribution is not counting it.** The asset refcount
  looked like exactly `2*users + 1`: the ten biggest assets fit it perfectly
  (21 users/43 refs, 17/35, 9/19) and the story wrote itself -- one reference held
  by the cache, two per model. Counting all 34 gave 27 fits, 1 above and **6
  BELOW**. The six matter far more than the 27: a per-model floor cannot be
  undershot, so they refute the story rather than complicate it, while the one
  above is trivially explainable (non-model owners hold assets too). The sort
  order caused this -- the rows I looked at were the largest counts, which are
  exactly the ones where a constant offset is least visible. So: when a relation
  looks exact, count the whole population before writing it down, and look hardest
  at the SMALL cases where the noise is proportionally largest.
- **A MISSING switch case is proof of impossibility, and it upgrades an
  observation into a theorem.** OT_LIGHT's object list reads 0 entries in every
  live sample, which for a long time was only recorded as "consistent with it
  being unused". Then `CClientMgr_CreateObjectOfType` turned up: it switches on
  the requested type byte with cases for 0,1,2,3,5,6 and lets 4 fall through to
  `default: return 0`. Nothing can construct one. An always-empty container is
  weak evidence on its own -- it might just be empty in this level -- so when you
  have one, go find the code that would fill it and check whether it CAN.
  The same dispatcher was also the ground truth for the entire type table: each
  case calls a per-type creator whose base constructor names the class, which is
  a far better anchor than matching counts against a reference enum by eye.
- **When a live total disagrees with one you recorded earlier, suspect YOUR
  iteration bounds before you theorise about the game.** A walk of the object
  lists came to 3305 against 3583 from a previous session. The tidy story wrote
  itself immediately -- the missing 278 are particle systems, particles are
  transient, so they must have been destroyed -- and it was wrong. The loop ran
  `for i = 0, 5` over an array the schema declares as `object_lists[7]`; index 6
  is the particle bucket and holds exactly 278. Arithmetic caught it (3305 + 278
  lands exactly on 3583), not judgement. Two habits follow: take array bounds
  from the schema rather than typing a literal, and when a difference is EXACTLY
  one bucket's worth, look at the loop before inventing a mechanism.
- **When you finally find a manager/container object, re-read its header against
  everything you already derived the hard way.** Inferences you paid for with
  scanning and climbing are very often plain fields in there, and finding them
  both confirms the inference and replaces it with a cheap, stable route.
  `LTWorldClientBSP` did this twice in one sitting:
  * `vis_tree.sectors` / `sector_count` are the sector array base and length that
    an earlier pass had found only by scanning outwards from one known sector
    until the AABBs stopped looking valid.
  * `world_tree_root` / `world_tree_node_count` are the quadtree root and size
    that another pass had found only by climbing `parent_offset` from a linked
    object and counting nodes during the walk.
  Both agreed exactly — same address, same counts — which is far better than
  either result alone: two independent derivations of the same value are evidence
  that neither route is misreading, and it turns a "found by scanning" note into
  a field with a name.
  So the habit is: after mapping a container, list the things you currently reach
  by search or traversal and go looking for each of them in its header. Anything
  you find there should become the primary route, with the derivation kept in the
  comment as corroboration rather than as the mechanism.
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

**Derive a stride by ARITHMETIC on the surrounding allocation before trying to
recognise the record's contents.** Engine data is often one contiguous block, and
when you know a count and the address of whatever comes next, the stride falls
out exactly — no pattern-matching required, and no chance of talking yourself
into a shape.

`LTVisPortal` came out this way. `LTVisTree` had an unmapped pointer at +0x00 and
an unmapped 344 at +0x10, with `sectors` at +0x04 pointing further along. Two
divisions settled the whole layout:

- `(sectors - table) / 344 == 96.00` exactly, so the block between them is 344
  units of 96 bytes.
- `344 * 4 == 0x560`, and `table + 0x560` equals the value of the FIRST pointer —
  so the block is a 344-entry pointer table immediately followed by 344 records
  of `96 - 4 == 92` bytes, and every pointer is `records + i*92` in order,
  344/344.

That fixed the stride at 0x5C before a single field was interpreted, and the
mapped fields then filled it to the byte. Compare with what happened when the
same region was approached by eyeballing: read as bare 12-float records it
"looked like" 4-vertex quads, and testing that guess gave only 45 of 344
coplanar. The quads were real but sat at +0x2C, behind a plane and a sector pair.

So: exact division is evidence, a recognised shape is a hypothesis. Do the
division first, and if a gap divides evenly by the count, treat that as the
stride and make the fields fit it.

**A field that stays inside a KNOWN COUNT across a varied population is an index
into whatever that count measures.** This is the cheapest identification technique in
the file and it needs no disassembly at all -- just a count you already trust and a
population with spread.

`LTModelRecord` had four unmapped uint16s. Testing each against the owning asset's
animation-table size gave 215/215 for two of them, 154/215 and 86/215 for the other
two. The two that always fit are animation indices; the two that do not are not. What
makes that convincing is the SPREAD: the 34 assets involved have animation counts from
1 to 457, so a field tracking a per-asset bound across all of them is not doing it by
accident, while a field that merely holds small numbers fails the assets with count 1.

Two things follow:
- **Choose the population, not just the field.** A test against assets that all had
  ~40 animations would have passed for any field holding values under 40. The counts
  ranging over three orders of magnitude is what turned a plausible result into a
  decisive one.
- **Assert the bound afterwards.** It was the evidence, so it belongs in the test: if
  the field ever leaves the range, either it was never an index or one of the two
  offsets moved. Same discipline as asserting `entry_count == (b - a) / 4` after using
  that arithmetic to find the count field.
- **Require the maximum to SCALE with the count.** This is the guard that makes the
  technique safe to run in bulk, and without it the bulk version is worthless. Sweeping
  every offset of the model object against `node_count` returned four fields that fit
  on 215/215. Two were real. The other two were `list_count` (a small count, max 6) and
  `sphere_source` (values 0 and 1) -- fields that satisfy ANY bound above their own
  range and would "pass" against a count of a thousand just as happily.

  Bucket the field's maximum by the count and read the row:

  ```
  node_a      n=2:1   n=20:9   n=38:14   n=61:9    n=84:37     <- scales: an index
  list_count  n=2:1   n=20:1   n=38:3    n=61:5    n=84:3      <- capped: a count
  ```

  An index into N things eventually uses the top of N. A count, a flag, or a small
  enum never does, however many instances you test. The 215/215 figure is identical
  for both rows -- only the scaling separates them.

The same pass found a float in the same record sitting inside [0,1] on 215/215, which
identifies it as a NORMALISED fraction while saying nothing about what it measures --
and the mapping records exactly that much, because the data does not separate
"playback position" from "blend weight".

**Then run the arithmetic BACKWARDS to find the field that stores the count:
compute the value, and search every offset of every instance for a field equal to
it.** The division gives you a number the structure must know somewhere. Turn that
into a query -- "which dword or word equals 61 on this asset, 2 on that one, 84 on
the third, across all 34" -- and the struct answers by itself. No writer needs to
be found and no shape needs to be recognised.

`LTModelAsset` gave up `entry_count` this way. The loader lays two arrays end to
end inside one blob, so `(array_b - array_a) / 4` is the entry count; scanning all
40 dword offsets and all 80 word offsets of all 34 assets for a match returned
exactly two candidates, +0x20 and +0x40, each agreeing 34/34. That is a much
narrower result than it sounds: with counts spanning 2..84 across 34 assets, a
coincidental match would have to track the real value on every one of them.

Two things make this worth reaching for:
- **It is cheap and total.** One pass over the whole struct on every instance,
  rather than reading a loader hoping to spot the write.
- **The check writes itself.** Assert `entry_count == (b - a) / 4` and the
  derivation stays honest: if the field ever stops tracking the gap, one of the
  two has moved, and the test says so instead of silently reading a stale count.

The same query shape finds duplicated fields for free -- here it turned up a
second copy at +0x40 that no reader had pointed at. Engines duplicate more than
you would expect (this asset also stores its radius twice and its own address
once), and each duplicate is another cross-route check that costs nothing.

**A field that looks like a hash: try the standard ones ONCE, then go read the
writer.** `LTModelAsset.node_hashes` held large pseudo-random dwords, one per
node, beside an array of node names — obviously a hash of the name. Testing
FNV-1, FNV-1a, djb2, djb2-xor, sdbm and CRC32 across as-is, lowercased and
uppercased inputs scored **0 of 660**. That took one Lua cell and was worth doing,
because a hit would have ended the question; what would NOT have been worth doing
is trying another dozen variants, seeds and byte orders, which is an unbounded
search with no guarantee the function is even a published one.

The writer settled it in two decompiles. `LTModelAsset_ReadNodeTree` does
`hashes[i] = String_HashI(names[i])`, and `String_HashI` is
`h = 0; while (*s) h = g_HashCharTable[*s++] + 919 * h;` — a custom multiplier over
a 256-byte translation table that is neither identity nor tolower, but a compact
alphabet remap which happens to fold case. Reimplemented, it reproduced all 660
live values exactly, and confirmed `hash("Head") == hash("head")`.

The rule of thumb: guessing is only worth it while the candidate set is small and
enumerable. Standard hashes are; "some custom hash with a table" is not. The
moment the cheap enumeration fails, the writer is the only bounded path — and it
gives you the table's address and the case behaviour as well, neither of which any
amount of black-box matching would have produced.

**To locate an array whose RECORD you already understand, search for a
SELF-DESCRIBING field rather than for the array.** `LTModelAsset_ReadNodeTree`
strides by 64 and stores each node's own index at byte +2. That is a fingerprint
no unrelated data imitates: scan a blob for a base and stride where
`byte[base + stride*i + 2] == i` for every `i` up to the count, and either nothing
matches or the array is found. Across a 19 KB blob and six candidate strides it
returned exactly one hit, and the asset field holding that base (`+0x1C`) fell out
of a single comparison against every field of the struct.

This is the same move as running the count arithmetic backwards, generalised: the
writer told me the SHAPE (stride 64, index at +2, parent at +0), and the shape
became the query. Prefer a field whose value is a function of its own position --
an index, a self-pointer, a back-reference -- because the check has no false
positives worth worrying about. Contrast searching for "an array of plausible
pointers", which found three candidate runs in the same blob and needed the
arithmetic to disambiguate.

Then let the shape validate itself. Once the array was found, four properties of a
TREE were checkable with nothing but the asset's own node_count: one root marked
255, `own_index == array index`, every parent index below its child's, and
`sum(child_count) == count - 1`. The last is the one to reach for first -- an
off-by-one in the stride or a mis-sized record breaks the sum long before any
individual field looks wrong.

**Once you know a record's SIZE, scan for the multiply and you have found every
function that indexes the array.** `LTModelNode` is 64 bytes, so any code walking
the node array must scale an index by 64 — `shl r, 6` or `imul r, r, 40h`. One pass
over the model code range for those two instruction shapes returned 25 functions,
sorted by how many times each does it, and the smallest ones were the accessors:

- a 35-byte function resolving `nodes[i + nodes[i].byte3 + ordinal].own_index`,
  which is what identified byte +3 as a FIRST-CHILD OFFSET and proved children are
  stored contiguously
- a recursive walk marking a node and its descendants, which incidentally exposed
  three PER-NODE arrays hanging off the model object (strides 2, 3 and 2, the last
  gated on a flags bit)
- `LTMem_Alloc(count << 6)`, i.e. the node allocator — confirming the 64 from the
  allocation side, which is the rule this file already gives for sizes

Why it works so well: a record size is a CONSTANT the compiler must materialise at
every indexing site, so it behaves like a symbol you can grep for. Compare the
alternative I was about to take — decompiling 80 vtable slots looking for node
accessors — which is twenty times the work and would have missed the allocator and
the internal walk entirely, since neither is a vtable entry.

Caveats worth knowing: small powers of two get folded into addressing modes
(`[base + r*8]`) rather than an explicit multiply, so this works best for sizes
above 8; and a size that is a common constant (16, 32) will return noise. 64 was
ideal. Check the smallest matches first — a short function that scales by the
record size is almost always a pure accessor.

**One allocation, many arrays: read the SIZE EXPRESSION and every region falls out
at once.** `LTModelObject_BindAsset` computes a single size as a sum of aligned
terms, `LTMem_Alloc`s it, and hands `{base, size}` to a chain of carvers that each
take a slice. That one expression is a manifest:

```
align4(a) + align4(b) + align4(2*node_count) + align4(48*node_count)
          + align4(28*asset->unk_54)
```

It says there are per-node arrays of stride 2 and stride 48, and another array of
28-byte records whose count is a field at asset+0x54 -- so it simultaneously
identified a region AND told me that an unmapped uint16 is a count. The base is
stored on the object, so every carved region can be bounds-checked against it,
which is what makes reading them safe.

This is the same lesson as "the allocation call is the size", one level up: a
single-allocation-many-regions pattern hides several array descriptors inside one
arithmetic expression, and it is far denser than any individual accessor.

**But an allocated region is not a populated one.** The 48-byte-per-node region is
obviously a world-transform cache, and I could not confirm it: live, only 181 of
2222 slots hold a matrix with unit rows and determinant +1, and 1 model of 215 is
clean throughout. So the mapping records the region's stride and length as PROVEN
(the allocator's own arithmetic) and its meaning as UNPROVEN, and the consumer API
exposes it as `node_matrix_raw` with an `is_rigid` predicate rather than as a
`world_transform` accessor. Naming it for the guess would have shipped the guess to
every caller; naming it for the shape costs one awkward identifier and keeps the
open question visible.

**A vtable slot's IDENTITY comes from what its worker touches, never from the
reference interface's method order.** FEAR 2's `ILTModel` has 80 slots and the
reference SDK's `iltmodel.h` lists methods in a plausible-looking order, so
matching them up is tempting. It goes wrong quickly.

Slot 69 takes `(hObj, index, char* dest, size_t len)` — exactly the shape of the
reference's `GetNodeName(HOBJECT, HMODELNODE, char*, uint32)`, and it sits among
other node-ish entries. I had it pencilled in as GetNodeName. Its worker settled
otherwise in one decompile: it reads `this[69]` and bounds-checks against
`this[70]` — which are `+0x114` and `+0x118`, the MATERIAL path array, whose
strings end in `.mat`. It is an indexed material-name getter. Two slots away,
`(hObj, float)` calling into the model update path really is `UpdateMainTracker`,
so the reference is a useful *source of candidate names* while being worthless as
an ordering.

The consolation is that a misread reader is still a reader. Chasing the wrong
hypothesis produced independent corroboration of the material array: the
destructor had given base, length and a 28-byte stride, and now a getter arrives
at the same three by a completely different route — and calls
`std::string::c_str` on the element, naming the type outright rather than leaving
it inferred from a stride. Follow a slot to its worker even when you expect to
confirm something; the cost is one decompile and the payoff is either a name you
can defend or a field you can defend.

**A float blob's GROUPING is a hypothesis too — test it against a field you
already know, never against how the numbers look.** Reading a span of floats
and recognising a shape is the single easiest way to invent structure, because
plausible shapes are everywhere in numeric data. A worked failure from this
project: 96 bytes of floats at `LTCameraObject+0xDC` grouped into six
`(x,y,z,d)` rows whose normals were orthonormal with matched ± distance
pairs, which reads unmistakably as *six bounding planes*. It is not. It is two
3x4 rigid transforms — the same bytes, a completely different meaning, and the
plane reading would have produced silently wrong camera math forever.

What killed the wrong reading was not more staring, it was an **independent
field**: the 3x3 part was compared against the rotation matrix rebuilt from
the object's *own quaternion* at `+0x20`, a value mapped in an earlier pass.
It matched to 0.00000 across 40 objects with determinant 1.0. So:

- Derive the expected bytes from an ALREADY-CONFIRMED field, then compare.
  Quaternion -> matrix, count -> array length, position -> translation.
- Prefer a test that **discriminates**, not one that merely passes. Here
  `R` and `R^T` are identical for the identity rotation, so most objects
  cannot tell them apart; one object with a real rotation matched `R` at 0.0
  and `R^T` at 2.0. Find the sample that can fail before believing the ones
  that pass. Report the discriminating sample, not just the aggregate.
- Check the relationships BETWEEN blocks, not just each block's shape. That
  the second 3x4 satisfies `R2 == R1^T` and `t2 == -R1^T*t1` on 60/60 pins
  it as the inverse; no amount of looking at its floats would have.
- A near-miss is information, not noise. That block 1's translation equals
  `position` on 55/60 samples and diverges by up to 50 units on the rest is
  precisely why the schema does NOT call it the position. Record the 5, do
  not round them away.
- **A residue of exactly ONE is the most informative sample you will get —
  chase it.** When the spatial-record volume matched the recomputed volume on
  1472 of 1473 OT_WORLDMODELs, the single holdout paid for itself three times
  over. It turned out to be the level-geometry object (`flags3 == 0x490`,
  `handle == 0xFFFF`, dims spanning the whole map); it was the SAME object as
  the lone `flags3 == 0x490` anomaly noted a pass earlier; and explaining it
  exposed a mask misreading (below) that had already been written into the
  schema as a claim. One outlier, one object, three corrections. A 1/1473
  discrepancy is small enough to feel like noise and specific enough to have
  exactly one cause — the best possible ratio. Do not aggregate it away.
- **`(char)field[n] < 0` on a 16-bit field tests 0x80, not 0x8000.** A signed
  BYTE comparison against zero is a bit-7-of-that-byte test, and the decompiler
  writes it as a comparison rather than a mask, which is what makes it easy to
  misread. `LTObject_GetCullVolume_AABB` does `if ((char)this[70] < 0) return 0`
  on `flags3` (a uint16 at +0x46): the gate is `flags3 & 0x80`. Recording it as
  `0x8000` produced a knock-on error — a search for that bit found no objects, so
  the code path was documented as NOT EXERCISED when in fact exactly one object
  reaches it every frame. Two habits that prevent this:
  * When a decompiled test is a signed comparison rather than an `&`, work out
    the mask from the ACCESS WIDTH at that offset, not from the field's declared
    width. `char` at +0x46 of a uint16 field means the low byte.
  * Before writing "not exercised" into the schema, count the objects that
    satisfy the condition you actually derived. Zero hits is as likely to mean
    "wrong condition" as "unused path".

**Before assuming one formula governs a field, ask whether its writer ever
ran.** A derived field has at least two legitimate states: the value its setter
computes, and whatever the *constructor* left there. `LTObject.radius` is
`|dims| + 0.1` — on 2126 of 3583 live objects. The remaining 1457 read exactly
`0`, with `dims` also `0`, because `LTObject_ctor` zeroes both and `SetDims`
never ran on them. Neither state is wrong; assuming a single formula is.

This is why Phase 1 should note what the **constructor** writes, not only what
the setters write. Reading `LTObject_ctor` (0x420408) end to end was the single
highest-yield step of that pass: it pinned `handle`'s 0xFFFF sentinel, proved
the quaternion's `w` sits at +0x2C by writing `(0,0,0,1)`, showed four
self-pointed list heads, and — precisely because it zeroes `dims`/`radius` —
explained the second radius state. When a "confirmed" identity holds on most
but not all objects, the ctor's initial value is the first hypothesis to test,
ahead of any tolerance change.

**To learn what a field MEANS, find the code that READS it. More samples of its
value will not tell you.** A setter gives you the offset, the width and the
notification, and none of that is meaning. `LTObject.scale` (+0x30) sat as
`unk_30` for a whole pass with a fully mapped setter, a known change code and a
known constructor default of 1.0 -- all of which is compatible with a scale, a
radius, an alpha, a rate, or a dozen other things. It was settled in one step by
`OT_MODEL_GetCullVolume`, which computes its cull-sphere radius as
`vis_radius * scale`. Once a consumer multiplies a field into a length, the
field is a ratio, and the argument is over.

Practical way to find the reader when the setter is already known: look at the
**type-specific overrides of the same vtable slot** and at the interface the
setter notifies. Here the notify chain led to owner slot 1, which asks the object
for a bounding volume through slot 2, and the per-type slot-2 bodies are where
every geometry field is finally consumed.

**Count the DISTINCT values of a pointer field to tell a resource handle from an
owned sub-object.** It is one read per object and it settles the question
outright. `LTModelObject.asset` (+0xEC) is non-null on 215/215 live models but
takes only **34 distinct values** — so it points at a shared per-ASSET record,
not at something each object owns. That changes what may be written about it
(shared state, so a mod must not mutate it per-instance) and it changes the
lifetime story (the pointee outlives any one object). A field with 215 distinct
values across 215 objects would have meant the opposite. Do this before mapping
the pointee, because it decides whether "per-object" belongs in the comment.

**When a per-object field always equals a value reachable through a pointer, it
is a CACHE — and the interesting question is why both are read.** `vis_radius`
(+0x16C) equals `asset->radius` on 215/215 objects. The lazy conclusion is "one
is redundant". The actual answer came from the two READERS inside a single
function: `OT_MODEL_GetCullVolume`'s first branch uses `asset->radius` UNSCALED
with an explicit centre, and its second uses the cached copy multiplied by
`LTObject.scale` with the object's own position. Same number, different
treatment — that is why the engine keeps both.

So when equality holds:

- Say "cache of X" in the schema, not "duplicate of X" — it tells the next
  reader which one is authoritative.
- Warn against treating them as independent knobs. Writing one will not affect
  the consumer that reads the other, which is exactly the kind of bug that looks
  like a wrong offset and is not.
- Then go find why both are read. The difference in USE is usually the real
  finding, and it is invisible if you stop at the equality.

**Corollary, and this is the part that actually cost time: do not argue against a
hypothesis using a correlation between two fields you have not finished
mapping.** The earlier pass rejected "scale" because the models whose value was
not 1.0 all had `dims == 0`, reading that as "nothing varies with it". Both facts
were true and the inference was backwards -- those models are SPHERE-culled, so
`LTObject_SetDims` never runs on them, and `dims == 0` is a CONSEQUENCE of the
same cause rather than evidence against. Two unexplained fields moving together
is a hint to find their common cause, never a proof about either one. Write the
observation down; do not promote it to an argument until you have the reader.

**A NEGATIVE claim in the schema needs exactly as much evidence as a positive
one, and it is far easier to write by accident.** "Always empty", "never
written", "nothing consumes it", "the ctor's value is never replaced" -- these
feel like observations and read like facts, but each is a universal quantifier
over live data. Two went into `LTObject` in one sitting and both were wrong
within minutes of being checked:

- "live every `owned_list` is EMPTY" -- actually **1931 of 3583** objects hold
  entries, 3260 in total.
- `+0xA8` "ctor writes NaN ... nothing observed recomputes it" -- actually
  written on **3248 of 3583**.

Neither had been measured; both were extrapolated from a small look. The rule:
if a comment contains "always", "never", "every" or "nothing", either run the
count and cite it, or write "unmapped" instead. "Unmapped" is honest and cheap;
a wrong universal is a trap for whoever reads it next.

**A float whose live values are all DENORMAL is not a float.** Anything with
magnitude below ~1e-38 is a denormal, and a field full of them is almost always
an integer being read through the wrong type. `LTObject.slot_index` (+0xA8)
looked like a float cache: the decompiler rendered the constructor's constant as
`NAN` and every live sample printed as `0.000` under `%.3f`. Two things were
going on, and both are reusable:

- `%g`, not `%f`, when sanity-checking floats. `%.3f` prints 4.8e-42 as `0.000`,
  which reads as "the field is zero" when it is really "the field is tiny".
  Re-read as `uint32` the values were 0..3885, all distinct -- an index.
- **IDA rendering a constant as `NAN` can just mean `0xFFFFFFFF`.** As an
  integer that is `-1`, i.e. the standard "no slot" sentinel, which is a
  completely different claim from "an invalidated float cache". When the
  decompiler shows `NAN`, check the raw bytes before believing the type.

When you find such a field, map it as a **partition** and give each branch a
name and a count. Then assert both that the partition is total and that both
branches are populated: a partition alone goes green if every object collapses
into one branch, which is how a wrong offset that reads zeroes presents.

**A rule read off the WRITER describes the moment of writing, not the live
state.** Code tells you the condition under which a field is set; it does not
tell you that the condition still holds for data written earlier. The world
tree's inserter (`LTWorldTree_FindNodeForObject`) descends until the object's
AABB straddles a split plane, so "every object on an internal node straddles
one of that node's planes" looks like a theorem. Live it is 332 of 467 — the
counter-examples sit at the root while lying wholly inside one quadrant, i.e.
objects that MOVED after being linked, because `SetPos` updates the AABB while
the tree link is only revisited when renderability changes.

So separate the two kinds of claim, and label them differently in the schema:

- **Layout** claims (this offset is that field, this size, this relation
  between fields) must hold on 100% of live samples, and a shortfall means the
  map is wrong.
- **Policy** claims (the engine puts objects here under condition X) are about
  a code path, and live data legitimately contains residue from earlier states.
  A partial rate is the expected answer, not a failure.

Never assert a policy claim in the fixture, and never quietly loosen it into a
layout claim. Write down the rate and the reason for the gap: 332/467 with
"objects retain their node after moving" is a useful, durable observation; "all
internal-node objects straddle" would have been a lie with a 29% failure rate.

**Two link types with the same SHAPE can have opposite field ORDER -- and a
circular list walks fine either way, so nothing complains.** `CClientMgrListLink`
is `prev@0x00 / next@0x04`; the world tree's link is the reverse, and the only
reason it surfaced is that traversing a node's object list required following
`+0x00` where the object buckets require `+0x04`. Both walks terminate, both
produce the right object count, and reusing one type for the other would
compile, browse correctly in ReGenny, and silently invert every `prev`/`next`
in code written against it.

Rules that follow:

- Do not reuse an existing link/list type just because the size and shape
  match. Give each structure its own type unless you have checked the field
  order in that structure's own code.
- A traversal succeeding proves the list is well-formed, NOT that you named the
  direction correctly. Both directions close.
- Where the code cannot distinguish them, say so. The world tree's unlink is
  symmetric in the two fields (`link[0]->[1] = link[1]` and
  `link[1]->[0] = link[0]`), so `next` there is a traversal convention, not a
  proven asymmetry, and `LTWorldTreeLink`'s comment records exactly that.

**An element with TWO link groups belongs to TWO lists — and the two may use
different linkage disciplines.** `LTSpatialEntry` (0x14) carries `record_next` at
0x08 and a `hit_prev`/`hit_next` pair at 0x0C/0x10. It is not a doubly-linked
node with a spare pointer: it is simultaneously

- a member of its record's list, **singly** linked, head at
  `LTSpatialRecord.entry_list`, length in `entry_count`; and
- a member of a hit's list, **doubly** linked, so an entry can be unlinked from
  the hit side without walking that list — which is exactly what
  `LTSpatialRecord_DetachEntries` does when it tears the whole record down.

The linker is where this is legible: `LTSpatialRecord_LinkEntry` writes five
fields in two obvious groups. Count the fields a linker writes and how they
cluster before deciding what shape the element is.

Two practical consequences:

- **Check both lists separately, because they fail for different reasons.** A
  wrong RECORD-side offset makes `entry_count` disagree with the walked length; a
  wrong ENTRY-side offset makes the hit-side pointers stop pointing back. Two
  counters localise the error; one merged "ok" tally would not.
- **`hit_head` is a pointer-TO-pointer.** It addresses the hit's head *slot*, not
  the head element — `DetachEntries` does `*hit_head = hit_next` when unlinking
  the first entry. Reading it as "pointer to the head element" type-checks, walks
  plausibly, and quietly breaks the unlink story. When a field is only ever
  dereferenced-and-assigned-through, suspect a slot address rather than an
  element pointer.

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

- **WHEN IDA HAS NO XREF, FOLLOW THE POINTER VARIABLE.** These wrappers name themselves for their error
  reports -- 'CLTClient::GetObjectPos' and so on -- but IDA recorded a data xref for only 15 of the 52
  such strings, so a plain xref walk finds almost nothing. The reason is an indirection: the code pushes the
  address of a POINTER VARIABLE whose value is the string, not the string itself. The chain that works is
  string -> .rdata/.data dword equal to the string address -> code referencing that dword, and it reached 37
  of the remaining 37.
  
  Search by raw little-endian address bytes, but treat every hit as a CANDIDATE: the same bytes occur in
  displacements and embedded data, so require the hit to sit inside a decoded instruction, and remember a
  shared string can name a callee rather than its caller.
  
  Disambiguating them was the useful part. Seven names had two candidate functions each. In five cases
  exactly ONE was in CLTClient_vftable and the other sat in a different block, which names the vtable entry
  and leaves the twin alone -- sharing an error string does not make a function that method. In the other two
  BOTH were in the vtable, at adjacent slots (21/22 and 23/24), so the class has two entry points for one
  operation. Those keep neutral _slotNN suffixes: overloads look like that, but so does one method inherited
  from each of two interfaces, and arity cannot separate them since both pop two dwords.

- **THE HARNESS CRASHED ON A LEGITIMATE GAME STATE, AND HID ITS OWN LOG WHILE DOING IT.** Two independent
  faults, and the second is what made the first hard to find:
  * `wpath.compare(wpath.size() - wname.size() - 4, ...)` underflows when no world is loaded -- both strings
    empty, so the offset wraps to a huge size_t, compare() throws out_of_range, nothing catches it, and
    MSVC's abort() raises __fastfail. The run dies as 0xC0000409, which reads like a stack-buffer overrun in
    the harness rather than an unguarded index.
  * stdout was block-buffered, so a mid-suite fault printed NOTHING -- the log that would have named the
    failing check was still in the buffer. `setvbuf(stdout, nullptr, _IONBF, 0)` in main() now makes a crash
    keep its evidence, which is the one run where you cannot afford to lose it.
  Guarded the arithmetic and made the no-world case report UNEXERCISED. Sitting at the main menu is a state
  the harness must survive, not a state it may fault on.
  
- **A FIXED SLEEP BEFORE REINJECTION IS A RACE.** The stale-instance path unloaded then slept 500ms, while the
  runner already had `wait_unloaded()` for exactly that question and used it at the end of the suite. On a
  slow unload the reinject lands on a still-resident payload, the injector refuses, and the run proceeds
  against a stale DLL -- silently testing the wrong build. Now it waits, and skips with 77 rather than
  guessing if the module is still there.

- **THE GENERATED-HEADER OUTPUT PATH IS `shared/sdk/regenny`.** Not recorded anywhere before, which is how I
  generated a whole stray tree at `shared/regenny/` and then watched the build keep compiling the OLD field
  names -- the schema said one thing and the headers another. `git status` naming the new directory as
  untracked is what caught it. Writing it down here so the next regeneration does not repeat it:
  
      regenny:sdk():generate("<repo>/shared/sdk/regenny")
  
  And a rename that crosses the schema is a FOUR-surface edit: the .genny field, the regenerated header, the
  C++ readers, and the prose that cites the old name. Missing any one leaves the tree inconsistent in a way
  that compiles or -- worse -- does not.

- **A DIAGNOSTIC FIELD'S ABSENCE MUST NOT READ AS ITS ZERO.** My new bind-pose fixture block parsed seven
  `bp_*` fields and ignored every parse result, then branched on `edges > 0`. If the endpoint ever stopped
  emitting them, `edges` would stay at its -1 sentinel and the block would print "no world loaded" -- an
  ENDPOINT REGRESSION reported as an ENVIRONMENT STATE, which is the same absence-versus-zero confusion this
  log keeps recording about the engine's data. Presence is now a check() of its own, and the no-world branch
  requires the fields to have parsed.

- **A VARARGS MISMATCH IN THE PROBE LOOKS EXACTLY LIKE AN UNSAFE ENGINE CALL.** Adding two probe fields, I
  updated the snprintf ARGUMENT list but the format-string edit silently matched nothing, so two doubles
  were passed with no conversions. Every later argument shifted, and that format has `%s` conversions
  downstream -- snprintf then read a double's bits as a char pointer. The game died.
  
  I diagnosed it as the new off-thread vt[22] calls and wrote "GAME THREAD ONLY, and that is not
  boilerplate" into the SDK header, citing the crash as evidence. Wrong, and the contradiction was already
  in front of me: an earlier run had made 1994 of those same calls successfully. Slot 22's disassembly is a
  pure asset read that neither dirties nor evaluates the skeleton, so it never had vt[3]'s game-thread
  requirement. Retracted.
  
  Two habits from this: when a scripted edit is supposed to change a format string AND its arguments,
  assert BOTH replacements landed -- a `.replace()` that matches nothing is silent. And when a crash
  coincides with new code, check what else changed in the same edit before writing causation into
  documentation.
  
- **THE FIXTURE CAN RELAUNCH THE GAME BUT NOT RESTORE THE SESSION.** Launching FEAR2.exe directly leaves it
  stuck pre-init -- gameclient.dll never maps, engine globals stay null, last_sample_time_ms does not
  advance -- so the world-dependent majority of the suite cannot run. Worth knowing before interpreting a
  wall of failures as regressions: check `gameclient.dll mapped in game` first, since everything downstream
  depends on it.

- **`ret N` IS A STACK-POP SIZE, NOT AN ARGUMENT COUNT, AND NOT A CONVENTION.** I inventoried all 83
  ILTModelClient entries by their `ret N` and was about to call the column "arity". Three things it is not:
  a __thiscall member carries an additional ECX `this` on top of the popped dwords; hidden return-buffer or
  register parameters never appear in it; and MSVC __thiscall cleans up its own stack too, so the number
  cannot distinguish the conventions. The one aggregate claim it supports is that all 83 clean up their own
  stack.
  
- **"IT OPENS BY READING A STACK ARGUMENT" DOES NOT SHOW ECX IS UNUSED.** That was my basis for documenting
  slot 22 as callable __stdcall, and it is not sufficient: ECX can be read later in the body or forwarded to
  a callee. Settling it was cheap for a 119-byte function -- ECX is read exactly twice, both reads dominated
  by a `mov ecx, [esp+node_index]` with no branch target landing between write and reads, and there is no
  indirect call to forward it. So the caller's ECX is genuinely dead and the documentation stands, but on
  the dominance check rather than on the opening instruction.
  
  Also worth separating: slots 2/3's convention has BEHAVIOURAL proof (passing the interface first returns
  LT_INVALIDPARAMS, observed), while slot 22's is structural. Both are evidence; only one was tested.

- **A CONVENTION ESTABLISHED ON ONE SLOT DOES NOT CARRY TO ANOTHER.** Merging my slot map into the incumbent
  ILTModel section, I attached its calling-convention finding -- __stdcall, object first, `retn 10h`, four
  dwords -- to slot 22. That finding was made on slots 2 and 3.
  
  Disassembling 0x42C958 shows the shape does transfer but the ARITY does not: `mov eax, [esp+object]`, gate
  on `[eax+10h] == 1`, and `retn 0Ch` -- THREE dwords, because GetSocketTransform's fourth argument is a
  world_space flag the bind-pose getter has no use for. Documenting "four dwords" would have handed a
  consumer a stack-corrupting call.
  
  The useful generalisation is narrower than the tempting one: what these wrappers SHARE is object-first
  dispatch, the OT_MODEL gate, and the LT_INVALIDPARAMS refusal path. Arity is per function, and `ret N` is
  one instruction to check.

- **GREP THE GENNY FOR THE ADDRESS BEFORE MAPPING A TABLE.** I walked CLTModelClient_vftable and wrote it up
  as a new section -- and the file already had "ILTModel VTABLE (client 0x66E7E8, server 0x674CD8)",
  documenting the same table, the same 83 count, AND things I had not established: the parallel server table,
  and the calling convention (these entries are __stdcall wrappers taking the OBJECT first, `retn 10h`, the
  interface `this` never read). Only spotted it because a heading grep for an unrelated fix happened to list
  it.
  
  Merged the one thing mine added -- the slot map for the 60 already-named implementations -- into the
  incumbent section, and deleted my duplicate. The incumbent had the better contract, exactly as with
  `pose_a`/`pose_b` earlier: the duplicate arrives wearing better documentation of a narrower fact.
  
  The check costs one grep of the ADDRESS, not the name. I searched for concepts, and the section was titled
  ILTModel while I was thinking ILTModelClient.
  
- **A HEADING IS THE CLAIM MOST PEOPLE READ.** My section title said "83 SLOTS, EXTENT PROVEN" while its own
  body explained why 83 is short of proof. Same failure as naming a function pre_update_is_empty() after
  narrowing the prose to "the entry returns immediately": when a claim gets qualified, the label is where the
  old version survives. Both headings now describe runs, not proofs.

- **"NO SIGNAL DETECTED" IS NOT "NO SIGNAL PRESENT", INCLUDING FROM MY OWN TOOL.** The boundary analyzer
  reported CLTModelClient_vftable as 83 entries with the strongest evidence class, and I wrote "extent
  proven". But its interior-boundary check matches ONE instruction shape (`mov [reg], offset X`), so a table
  installed differently, or one whose destructor does not reset a vptr, would sit inside the run unseen.
  What the tool actually establishes is that the RUN ends at 83.
  
  The fix was to try the corroborations and record that they do not apply, rather than let their absence read
  as agreement:
  * RTTI is present in the image (206 ".?AV" descriptors) but NOT for these classes -- [vt-4] is string
    bytes, not a Complete Object Locator. Worth knowing generally: RTTI existing somewhere in a binary says
    nothing about the class you are looking at.
  * There is no constructor to corroborate from. The table has exactly one reference image-wide -- the
    instance's own vptr field -- because the singleton is statically initialised, its vptr baked into .data
    at link time.
  * The reference header declares ~59 virtuals across ILTModel and ILTModelClient. Different engine version,
    so context only, and this project has already been burned assuming the reference's layout transfers.
  
  Recorded as an 83-entry RUN with the slot indices as build observations. The mapping is just as useful --
  60 of 83 implementations were already named, so the indices attached for free, which is the whole reason
  the table was worth walking.

- **THE BOUNDARY LESSON IS NOW TOOLING: reversing/ida_vtable_boundary.py.** Five repetitions of one error
  earned a script rather than another paragraph. `report(vt)` prints boundary CANDIDATES with evidence --
  never a proven extent -- from three signals: the dword stops being a function (a string here is the
  strongest stop available), an address inside the run is referenced from outside it (`mov [reg], offset X`
  means a table starts at X), and a vptr-reset-shaped entry repeats. `scan_dtor_repeats(vt)` audits a count
  someone already recorded.
  
  Its first two versions each embodied one half of the error it exists to prevent, which is worth
  recording because the pair is the whole lesson:
  * V1 STOPPED at the first dword that was not a defined function -- the TRUNCATION failure verbatim. A
    slot may point at undefined code, a thunk, or another module.
  * V2 continued past everything and reported the first string it found as proof. On the 2-entry table at
    0x678C80 it printed "187 entries", because a string only bounds the whole SCANNED RUN once the scan has
    crossed into adjacent tables -- the OVERRUN failure.
  The fix is not a better heuristic but a different output: INLINE_DATA counts as proof for THIS table only
  when no earlier vptr store names an interior address. Otherwise the tool prints an AMBIGUITY INTERVAL
  [earliest credible start .. inline stop], names the leading estimate, says outright that the string does
  not bound this table, and asks for the interior reference to be classified by hand. A tool that must
  sometimes answer "ambiguous, here is the interval" is the honest shape for this problem.

  Two more corrections during review, both worth keeping in mind when reading its output:
  * the vptr-reset predicate must require the destination be memory via a register (o_phrase/o_displ). Any
    `mov <anything>, imm` also matches `mov some_global, offset base` -- a static initializer or atexit
    reset, of which this binary has many -- and counting those as destructors invents boundaries.
  * repetition is a WARNING, not proof. MSVC emits destructor variants (scalar and vector deleting) and ICF
    can make unrelated slots share an address, so a legitimate table can hold one pointer twice.
  
  Credibility matters in the signal too: only a `mov [reg], offset X` counts as TABLE_START, since a
  reference that is not a vptr store may be a patch site or a pointer-to-table. With that in place it
  reproduces both known answers -- 147 for CLTClient_vftable as proof, and 0x678C80 as ambiguous with a
  leading estimate of 2, which the two independent vptr stores of off_678C88 then settle by hand.

- **FIFTH BOUNDARY ERROR, AND ITS OWN OUTPUT REFUTED IT.** Having just written the boundary pattern into
  this log, I sized off_678C80 by scanning forward 151 entries and quoted statistics over them: "137
  distinct, a single _purecall, so a fully implemented class". The scan crossed multiple adjacent vtables.
  
  The disproof was already printed: the histogram showed the DESTRUCTOR thirteen times. A class has one
  destructor, so thirteen occurrences is thirteen tables whose folded slot 0 is shared -- which is also
  precisely why the run never "looked" like it ended. Every count from that scan is withdrawn.
  
  What survives is the part that never needed an extent: the atexit evidence. Segment, xref count, and the
  registering push are all facts about ONE address, and they are what resolve the live/teardown timing.
  The pattern to internalise: when a claim needs an extent, no statistic computed over the guessed range
  can support it -- and check whether the sample contains something that CANNOT repeat (a destructor, a
  vptr, a type name), because that is the cheapest boundary detector available.

- **THE BOUNDARY MISTAKE, FOURTH TIME THIS SESSION -- NAME THE PATTERN.** Its shapes so far: `0xC3`
  proving a body is one byte; "exactly six bytes" for an entry sequence; a vtable sized by scanning until
  the pointers stopped looking like code; and now a "block of direct method pointers" read off consecutive
  .data dwords after an object's vptr. Every instance is the same move -- a fixed-size or run-length
  READ licensing a claim about an OBJECT'S EXTENT.
  
  The tell is available every time: ask what ELSE produces the same bytes. Consecutive .text addresses in
  .data are equally consistent with unrelated neighbouring globals -- and here two of them provably ARE
  (0x6E31B0/0x6E31B4 are pushed as their own globals). What would settle it is an initializer writing the
  aggregate, or code doing [ptr + offset]; neither was found, so nothing past +0x00 gets modelled.
  
  Also worth recording: five CLTClient_* methods really are absent from the vtable, which made the field
  block attractive. Missing-from-vtable has a boring explanation -- non-virtual members are not in one --
  and an attractive story is exactly when to demand the initializer.
  
- **RECORD CONTRADICTIONS -- THEN CLASSIFY THE CODE BEFORE TRUSTING EITHER SIDE.** sub_66B35F appeared to
  store a different 151-entry vtable into the ILTClient object's first dword while the live field read the
  147-entry one. Recording it beat smoothing it over, but "both observations are solid" was still one step
  short: I had not checked whether the WRITER was code at all.
  
  Three cheap checks settled it. Segment: .text, perm R+X -- so not data misread as code, which was the
  live worry given .rdata sits at 0x66D79C. Reachability: ZERO code xrefs, ONE data xref, and that
  reference is `push offset sub_66B35F; call sub_6527B9` -- an atexit registration, identical to the pair
  one line above. So it runs at static destruction, never during play. Mechanism: off_678C80's commonest
  entry is a destructor whose body writes off_678C80 into [this], and that function is CLTClient_vftable's
  slot 0 -- the ordinary MSVC vptr reset.
  
  No contradiction: the two facts describe different lifecycle phases. The lesson is the ORDER -- for any
  "X writes Y", establish that X executes, and when, before weighing it against a live read. `sub_` in the
  name means IDA made a function, not that anything calls it.

- **IF THE PRODUCER ALREADY WALKS THE DATA, LET IT COUNT.** My first slot-1 fixture check re-parsed the
  endpoint's JSON array with a bespoke `},{` splitter to count resolved entries -- fragile, and a
  duplicate of a loop the producer was already running. Moved the counts into the endpoint
  (`resolved`, `slot1_constant_strings`) plus two named fields for the semantic anchors, and the test
  now uses the existing json_int/json_str helpers. Also cost a failed run to learn the descriptor count
  was already published under a different key (`expected_names`): check the producer's field names
  before inventing a reader.

- **A VTABLE'S EXTENT NEEDS A BOUNDARY, NOT A SCAN.** I sized ILTClient's vtable by reading forward until
  a pointer left FEAR2.exe: 147 slots. That is unsound both ways -- a valid slot can target another
  module (truncating), and the next vtable's entries also look executable (overrunning). The proof came
  from a static xref instead: the dword just past the table is referenced as `offset aCltclient`, so it
  is the string "CLTClient" and the array demonstrably stops there. Same number, but now it is evidence.
  
- **READING A CONSTANT BEATS CALLING A GETTER.** Wanting each interface's implementation name, the
  obvious move is to call slot 1. Better: require the entry sequence `B8 imm32 C3` and read the immediate.
  No invocation, so no side effects to reason about, and a slot that does anything else simply fails the
  test. Two bugs found in that helper during review, both of which would have made it worse than the
  naive version: it accepted a 127-byte prefix when no NUL was in range (presenting arbitrary bytes as a
  verified string), and it masked PAGE_GUARD off before testing protection -- reading a guard page CLEARS
  the guard, the exact side effect the design existed to avoid.
  
- **"EXACTLY SIX BYTES" IS THE EXTENT MISTAKE AGAIN.** I described `B8 imm32 C3` as "the whole function"
  in the new header, having recorded earlier this same session that `0xC3` proves an ENTRY returns, not
  what a body contains. The safety argument never needed extent. Third recurrence of this shape of error;
  the tell is any sentence where a fixed-size read licenses a claim about a whole object.
  
- **ASSERT AGAINST THE RIGHT DENOMINATOR.** The first version of the slot-1 fixture check compared the
  shape count against the DESCRIPTOR TABLE size, while Interface.hpp documents that a null interface is
  normal (early startup, server absent, mid-unload). That test would have failed for availability and
  read as a shape regression. Compare against RESOLVED entries, and pin two known strings so a change of
  meaning cannot hide behind a passing count.

- **TWO SAMPLES DIFFERING IS NOT "EACH HAS ITS OWN".** I saw holder vtables 0x101D0434 and 0x101D0444
  differ and wrote that each holder has its own. Counting all 46 refuted it: 13 distinct vtables, one
  per interface NAME, with all 9 ILTInput holders sharing one and all 18 ILTOnlineService sharing
  another. The two I sampled differed because they belonged to two different interfaces -- the sample
  could not distinguish "per holder" from "per interface", which is exactly the question.
  
  Counting was cheaper than hedging. But the conclusion I drew from it went too far: I called the
  vtable a second identity route, when the measured fact is only "13 observed names map one-to-one onto
  13 addresses among these 46 holders". Nothing shows two interfaces cannot share a vtable ADDRESS --
  and this is the very DLL where ~133 identical retn-only methods were measured folded onto one. Folding
  identical CAPIHolder<T> vtables would break the inference in the dangerous direction, making "same
  vtable => same interface" quietly false. Downgraded to a heuristic worth cross-checking; identity
  stays with the name at +4 and ppInterface at +8.
  
  Worth naming the pattern: a one-to-one mapping observed across a sample is evidence about the sample.
  Promoting it to a rule needs a reason the mapping CANNOT collapse -- and in this binary there is a
  known mechanism that collapses exactly this kind of thing.

- **DECODE THE RELATIONSHIP, DO NOT READ THE ADDRESSES.** Seven interface pointers sit consecutively at
  0x101FC160..0x101FC178 in gameclient.dll, so which global belongs to which interface looked obvious.
  It was not: identity lives in the CAPIHolder at +8, which stores the ADDRESS OF the pointer variable
  for the name at +4. Decoding one thunk end-to-end gave the rule; applying it to all 46 holders gave
  the mapping; reading holder+4 and holder+8 live in the running game confirmed all seven.
  
  The trap was real, not hypothetical. An interface can have SEVERAL pointer variables (ILTInput 9,
  ILTOnlineService 18), and the one in the tidy band is not always the one the DLL uses --
  ILTCustomRender's most-read copy is 0x1020172C (7 readers) while the band copy has 2. I had named the
  minor one. Reader counts, not layout, settle "which copy matters".
  
- **CHECK THE SDK'S VOCABULARY BEFORE COINING ONE.** I named the class LTInterfaceRequest, then found
  shared/sdk/interfaces/Registry.cpp already scans FEAR2.exe for this exact ctor and calls it
  CAPIHolder, documenting the same `this[2] = slot; *slot = 0` shape. Renamed to match. A second name
  for one concept is the same failure as a second accessor for one field.
  
- **A SCANNER'S SCOPE IS NOT A FACT ABOUT THE GAME.** Our Registry scans the exe only, so the DLL's 46
  holders are invisible to it. That tempted a claim that gameclient.dll has a "separate registry".
  Unknown: the module-local list at g_pAPIHolderList may or may not be handed to the engine's database,
  and pointer agreement would not settle topology either way. Recorded as untraced.

- **AN EMPTY FUNCTION IDENTIFIES NOTHING IN THIS DLL, AND ITS ADDRESS IS NOT A HOOK TARGET.** I named
  0x100F6680 `CGameClientShell_PreUpdate` because IClientShell slot 2 points there. Then
  `CGameClientShell_Update` turned out to CALL that address twice, which makes no sense for PreUpdate
  -- and the xrefs explained why: 133 code xrefs, and 133 of 138 data xrefs are `.rdata`
  pointer-array entries. IDA had it as `nullsub_3` all along. One shared empty stub, most likely
  /OPT:ICF folding every retn-only method (an inference from the pattern, not read from the linker).
  
  Three consequences, and the third is the one that would have shipped a bug:
  1. The ADDRESS gets a neutral name (`shared_empty_stub`); the SLOT is still PreUpdate by interface
     layout. ICF erases an address's exclusive identity, not a slot's semantics.
  2. "Slot N is empty" is not corroboration for anything. My +2 anchor cited it; the anchor stands on
     slot 1's string, and that citation is now marked as worth nothing.
  3. **A DETOUR ON THAT ADDRESS IS NOT A HOOK ON THAT METHOD** -- it intercepts every one of those
     call sites. The consumer-safe operation is repointing the VTABLE ENTRY, so `pre_update_fn()` is
     documented as introspection-only and `pre_update_vtable_entry()` is the hook point.
  
  Scope, stated correctly after getting it backwards: an entry patch is NARROWER than a body detour,
  which catches every caller of the body. For slots 3/4 they coincide -- one data xref, zero code
  xrefs each -- but that is the only STATICALLY VISIBLE route, not proof none can be computed.
  
- **COUNT WHAT YOU COUNTED.** I wrote "138 vtable slots" from a raw data-xref total, then "133 tables"
  from the classified subset. The measurement was 133 `.rdata` pointer-array ENTRIES; how many distinct
  tables they sit in was never counted. Also `grep 'a\|b'` silently matches nothing in this shell --
  it cost a wrong "already correct" reading of a header. Use the grep tool for alternation.

- **A NAME IS THE CLAIM MOST PEOPLE READ. Retract it there first.** After narrowing the prose about
  `*fn == 0xC3` to "the entry returns immediately", the function was still called
  `pre_update_is_empty()` -- so the overclaim survived in the one place every consumer sees, while the
  correction sat in a comment they might not. Renamed to `pre_update_entry_returns_immediately()`.
  When a claim gets narrowed, check the identifier, not just the sentence.
  
  Two smaller versions of the same sweep: the class-level prose and the genny slot map both still said
  "a single `retn`" after the accessor's doc had stopped saying it, and a retraction that leaves the
  old wording anywhere is only half done. And int3 padding after the retn is strong evidence a body
  ends there -- it is what MSVC puts between functions -- but it is still evidence, so it is now
  written that way rather than as "genuinely no further body".

- **"RE-VERIFIED AT RUNTIME" MUST NAME WHICH PART IS RE-VERIFIED.** Calling IClientShell slot 1 and
  matching "CGameClientShell" re-checks WHERE THE INTERFACE STARTS -- the string proves slot 1 is
  IBase's single virtual, hence slot 0 is implementation-only, hence the +2. It does not touch WHICH
  of slots 2/3/4 is which: reorder those and the identity check and the module-containment check both
  still pass. I wrote that it meant "a wrong slot map refuses to hand out addresses", which was one
  claim too many.
  
  The fix was cheap and worth doing rather than just narrowing the prose: each slot has a PROLOGUE,
  and the three are mutually distinct (retn+int3 padding; `sub esp, 0x128`; ebp frame with
  `and esp, -64` for SSE locals). Comparing those makes the plausible failure -- a reordering --
  detectable. Still not semantics: it cannot tell PreUpdate from any other empty function, and a
  rebuild could change a prologue, which shows up as a false negative rather than a wrong hook.
  
  Also: `*fn == 0xC3` proves the ENTRY returns immediately, not that the body is one byte. Here the
  0xCC int3 padding after it is what actually evidences an empty body -- the compiler's filler.

- **A SLOT MAP CAN BE RE-VERIFIED AT RUNTIME, SO DO THAT INSTEAD OF TRUSTING THE READ.** The
  IClientShell anchor rests on slot 1 returning the literal "CGameClientShell". That is not just
  static evidence -- slot 1 is a pure return of a constant, so the SDK can CALL it and compare the
  string on every run. A wrong slot map, or a different game build, then refuses to hand out hook
  addresses rather than returning something arbitrary to detour.
  
  Pair it with containment: every anchor is checked to land inside gameclient.dll, since an
  implementation slot pointing elsewhere means the layout assumption is wrong. And where a slot has a
  fingerprint, assert it -- slot 2 being a lone `retn` is a check that a shifted vtable would fail.
  Label such a check for what it is: a property of the shipped game code, not an invariant.

- **A REFERENCE HEADER GIVES DECLARATION ORDER, NOT SLOT INDICES. Anchor the shift with evidence.**
  The reference declares IClientShell as PreUpdate / PostUpdate / Update over an IBase with exactly
  ONE virtual, which puts the triple at slots 1/2/3. FEAR2 calls 2/4/3. The tempting move is to assume
  a +2 shift because it makes the call order come out as Pre -> Update -> Post, which is "obviously
  right" -- and that is exactly the reasoning this log keeps catching.
  
  What settled it was reading the vtable: slot 1 returns the literal "CGameClientShell", which IS
  IBase's `_InterfaceImplementation`, so slot 0 is an implementation-only leading slot that the
  published interface does not contain. The +2 is then a fact about the binary, not a fit to a
  hypothesis. Corroborated twice more: slot 2 is a single `retn`, matching the reference's own remark
  that PreUpdate exists for organisation only, and slot 4 is 0x940 bytes against slot 3's 0x1A2.
  
  Note also what was NOT named: slot 0 has an MSVC vector-deleting-destructor shape, and it keeps its
  auto name, because the +2 anchor needs only that the slot EXISTS, not what it does.
  
- **NAME THE DISPATCH PATH WHEN TWO VTABLES SHARE A CLASS NAME.** "CClientShell slot 2" is ambiguous
  in this project: the ENGINE has a CClientShell whose vtable is in FEAR2.exe, and the GAME DLL has a
  CGameClientShell implementing IClientShell. CClientShell::Update reaches the second through the
  GLOBAL g_pIClientShell_Default_6ECD3C -- not through any field of the first -- so recording which
  object the dispatch goes through is what stops a slot number meaning two things.

- **IF A DOC COMMENT PROMISES AN INVARIANT, THE FUNCTION MUST ENFORCE IT.** `local_player()` said the
  handle and the pointer "are the same object" while only checking both were non-empty. The obvious
  fix was to add a checker beside it -- but an optional check a caller must remember is not a
  contract, and the accessor was still free to hand out a torn pair. It now resolves the handle and
  refuses on disagreement, with the diagnostic kept separately for observing what the accessor hides.
  
  Two follow-on holes, both worth the pattern:
  
  * The verification was skipped when CClientMgr was absent -- so the one state where the invariant
    could NOT be checked was the state where the pair was returned unchecked. "Unverifiable" must fail
    the same way as "wrong".
  * The diagnostic initially went THROUGH the accessor, which now fails closed, so it could never have
    observed a disagreement. A checker for a condition its own input already excludes measures nothing.
  
- **AND DO NOT UPGRADE A CONSISTENCY CHECK INTO A FRESHNESS GUARANTEE.** With the check in place I
  wrote that the pointer "carries no staleness caveat". It does: the two reads are not atomic and the
  object can be unregistered the instant after they agree, so the pointer has the same lifetime caveat
  as every other LTObject*. What the check buys is refusing an ALREADY-torn pair -- not one that stays
  valid. The handle is the durable identity; the pointer is a per-frame resolution of it.

- **CHECK FOR AN INCUMBENT ACCESSOR BEFORE ADDING ONE. I built four duplicates in two passes.** The
  header already had `pose_a`/`pose_b` reading the very fields I "added" `bind_pose()` and
  `anim_fallback_position()` for, a `Pose` struct my `NodePose` duplicated, and `path_to_root()` --
  SEH-guarded, bounded, returning `optional` -- which my `node_chain_to_root()` reimplemented with a
  WEAKER contract that conflated malformed with empty.
  
  Two walks over one structure can drift, and two names for one field means a consumer picks one and
  inherits whichever set of comments is stale.
  
  The fix was NOT to revert to the incumbent names. `pose_a`/`pose_b` existed precisely because the
  fields' roles were unknown, and this session established them -- so the right move was to CUT OVER:
  `pose_a` -> `bind_pose`, `pose_b` -> `anim_fallback_position` (position only), one `Pose` struct,
  and `node_depth`/`node_has_ancestor` expressed in terms of `path_to_root()` so there is one guarded
  walk with three views. Reverting would have kept an API that hides semantics I had just proven.
  
  The lesson is the grep, not the taste: an accessor for a field I have just mapped is exactly the
  thing most likely to exist already under a non-committal name.

- **RETRACTING A CLAIM MEANS SWEEPING EVERY PLACE IT LANDED.** Deleting the speculative helper was
  only the first step. The header still had, in the SAME comment block: "+0x24 is parent-relative"
  applied to the whole pair, the 268/2196 composition presented as evidence about coordinate spaces,
  and a line calling the quaternion convention unvalidated when it had since been read directly. Three
  disproven or superseded statements sitting next to the corrected ones, which is worse than either
  alone -- a reader cannot tell which paragraph is current.
  
  So the block was REWRITTEN rather than patched, and the confidence split was carried through to
  every surface: the schema field renamed `anim_fallback_rotation` -> `anim_getter_rotation` (named
  after its only evidence, not a role), the accessor narrowed to `anim_fallback_position()`, and even
  the SEH read reduced to three floats so the accessor's success no longer depends on data it
  deliberately does not expose.
  
  When a finding narrows, the retraction has to reach the schema, the accessor, the type, the guarded
  read and the prose. A correction that stops at the function leaves the old claim alive everywhere
  else.

- **WHEN THE PRODUCER CONTRADICTS A SHIPPED HELPER, DELETE THE HELPER.** `composed_fallback_pose()`
  composed the +0x24 pair down the hierarchy and was documented as the transform the engine would
  reach along an all-fallback path. Then reading LTModelObject_EvaluateSkeleton showed the engine
  builds each local transform with the rotation from g_AnimEval_NodeLocalRotations -- always,
  animated or not -- taking only the POSITION from +0x24. The composed rotation is a path the engine
  never walks.
  
  The temptation was to keep the function and document the caveat, since the arithmetic itself is the
  engine's own and validated elsewhere. That would still leave a confident name over speculative
  semantics. Removed, along with the assertion built on it, and the header now carries a note WHERE
  SOMEONE WOULD LOOK FOR IT explaining why it is absent and what would be needed to do it properly.
  
  An absent helper with a reason beats a present one with a caveat.

- **READ THE PRODUCER, NOT JUST THE GETTERS.** Two getters had been read for the node poses and both
  left the important questions open. The PRODUCER -- LTModelObject_EvaluateSkeleton, the function that
  actually fills the per-node cache -- answered three at once: +0x24 is a local position, the cache is
  built by composing locals onto the PARENT'S CACHED transform, and the composition rule is
  LTTransform_Compose exactly as the SDK reimplements it.
  
  A getter shows you a field's shape. The writer shows you its role.

- **A FIELD'S HALVES CAN HAVE DIFFERENT EVIDENCE.** The same read showed the two readers of +0x24
  disagreeing: GetAnimNodeTransform copies the whole (position, rotation) pair, while the evaluator
  takes ONLY the position and sources the rotation from the animation buffer. So the position half is
  established by the producer and the rotation half rests on a single secondary reader.
  
  That asymmetry is also a candidate explanation for the earlier failed composition -- feeding a
  rotation the evaluator would never have used. Record confidence per FIELD, not per record.

- **TRACE THE DELEGATE BEFORE DOUBTING THE CLAIM (and before shipping it).** I had "+0x24 is
  parent-relative" from ILTModel_GetAnimNodeTransform walking the parent chain. Then I noticed its
  combiner zeroes the output rotation to identity, read that as evidence AGAINST a plain compose, and
  weakened the claim in the header.
  
  It was output INITIALISATION. One more decompile showed LTTransform_Compose doing exactly
  `q_parent * q_child` and `p_parent + R(q_parent) * p_child` -- the standard composition, term for
  term what the SDK helper computes. The claim was right, my doubt was wrong, and both the hedge and
  the original overconfidence came from stopping one call short.
  
  A wrapper that sets up an output tells you nothing about the operation. Follow the delegate.

- **A HELPER'S RETURN TYPE IS AN ASSERTION.** `anim_fallback_pose()` returned a struct named
  `BindPose`, in the same header that spends paragraphs proving the fallback is NOT bind data. The
  type name would have quietly taught every consumer the opposite of the finding. Renamed to
  `NodePose`.
  
  When two things are measured to be different, make sure the C++ cannot be read as saying they are
  the same.

- **IDA MCP PORT ASSIGNMENTS ARE NOT STABLE ACROSS THE SESSION. `list_instances` first, every time.**
  Early in this session FEAR2_dump.exe was on 13339 and gameclient.dll on 13338; later they had
  swapped. Selecting the cached port "succeeded" and `server_health` still reported FEAR2_dump --
  which reads exactly like the silent-select failure the friction notes describe, but was really a
  no-op select onto the instance already active. Two different faults with one symptom, so the
  filename check after selecting is what distinguishes them.

- **THE REFERENCE SOURCE ANSWERS "WHAT IS THIS FOR" FASTER THAN ANY MEASUREMENT -- and is still not
  ground truth.** After three inconclusive statistical attempts at the bind pose's coordinate space,
  one grep of the LithTech SDK produced `mat = pNode->GetGlobalTransform()`. That is a direction in
  seconds where measurement had produced nothing in an hour.
  
  It does NOT close the question. FEAR2's implementation already differs in shape -- it fills a
  position/quaternion pair where the reference returns a matrix -- so a matching name does not carry
  matching semantics. Recorded as the standing hypothesis, explicitly labelled reference-derived and
  unverified here.
  
  Use the reference to generate hypotheses and to know what to look for; verify in the binary.

- **A GOOD TEST THAT REFUTES IS STILL A GOOD TEST.** The reference hypothesis suggested a decisive
  check needing no reference at all: +0x24 is provably parent-relative, so composing it from the root
  should reproduce +0x08 if the two are one pose in two spaces. It does not -- 268 of 2196 nodes, i.e.
  the roots where composition is the identity. So the two pairs are DIFFERENT DATA, which is a fact
  worth having even though it names neither space, and it is now asserted so it cannot quietly drift.

- **NAMING A HELPER AFTER WHAT YOU HOPE IT IS.** I documented the composed fallback chain as "a node's
  rest transform" and "a reference skeleton" in the same pass that MEASURED it to be different data
  from the bind pose. The reader only establishes the narrow role -- the local transform substituted
  when a node has no animation key -- so the honest description is a composed animation-fallback
  transform, and that is what it now says.
  
  A helper's name and comment are where an unsupported semantic quietly becomes a project assumption.

- **POPULATION STATISTICS CANNOT SETTLE A COORDINATE CONVENTION. Find the consumer.** Three
  measurements were tried on whether `LTModelNode`'s bind pose is local or model-space, and all three
  failed in different ways:
  
  |            | result | why it failed |
  |---|---|---|
  | depth vs magnitude | 12.9 -> 57.2 | mixed asset scale; depth bands are different models |
  | parent delta | 38.6 vs 57.2 | question-begging -- undefined if the pairs ARE local |
  | fit to bounding radius | raw 147/147, composed 144/147 | BOTH hypotheses pass |
  
  The third was the best-designed of them -- an independent yardstick from elsewhere in the engine
  rather than another statistic about the poses -- and it still did not discriminate, because a
  composed skeleton that stays inside the model's bounds is consistent with either reading. It is
  also weaker than it looks: helper bones can legitimately sit outside mesh bounds, and the
  quaternion order used to compose is itself unvalidated, so a mistake there could flatter either
  answer.
  
  THE PATTERN IS THE LESSON. A convention is a fact about CODE, so it is settled by finding code that
  depends on it -- a caller that composes the value, or an engine-produced result in a known space to
  compare against. `ILTModel_GetBindPoseNodeTransform` has no callers inside FEAR2.exe at all, only
  its two vtable slots, which localises the answer to gameclient.dll and is itself worth knowing.
  
  Three cheap failures beat one confident guess, and recording WHICH approaches failed is most of
  their value -- the next reader does not repeat them.

- **A PREDICATE'S PROMISE MUST COVER ONLY WHAT IT COMPARES.** `shares_node_data()` compares one
  pointer -- the node RECORD array -- but its comment offered "bind poses, the hierarchy, socket
  offsets". Sockets come from a separate `asset->sockets` pointer resolved independently, so record
  identity establishes nothing about the socket table, and socket offsets are precisely what a
  caller would assume such a predicate covers.
  
  The function was right and the documentation was generous. When writing a "can I reuse X" helper,
  enumerate the fields it actually proves and stop there -- a consumer trusts the comment, not the
  body.

- **A METRIC THAT IS ONLY INTERPRETABLE UNDER ONE HYPOTHESIS CANNOT CHOOSE BETWEEN HYPOTHESES.**
  Trying to settle whether `LTModelNode`'s bind pose is parent-relative or model-space, I measured
  mean `|pos(child) - pos(parent)|` and read 38.6 against positions of 57.2 -- "too large to be a
  bone length, so not model-space". The subtraction only MEANS anything if the vectors share a
  frame, which is exactly what was in question: under the local hypothesis they sit in different
  parent frames and the difference is geometric noise. Question-begging, not evidence.
  
  The other measurement -- magnitude growing 12.9 -> 57.2 with depth -- was confounded a different
  way: the sample mixes assets of different scale, and the depth bands are not drawn from the same
  models, so scale alone reproduces the pattern.
  
  Both were cheap and both were worth running; what mattered was not adopting the flattering
  reading. Before believing an aggregate, ask what it would look like if the OTHER hypothesis held.
  If the answer is "the same" or "undefined", it is not a discriminator.
  
  BEHAVIOUR OF A SIBLING FIELD IS NOT EVIDENCE EITHER. +0x24 is provably parent-relative because
  ILTModel_GetAnimNodeTransform accumulates it up the chain -- and I briefly wrote that this
  "confirms both pairs are parent-relative". It confirms one. Two fields in one record are two
  facts.
  
  Recorded as OPEN. What would settle it: compose +0x08 down the hierarchy and compare against an
  engine-produced transform, or find a caller of the bind getter that combines it with parent
  transforms.

- **IDENTICAL VALUES ACROSS INSTANCES DO NOT ESTABLISH OWNERSHIP OR IMMUTABILITY.** I asserted that
  the bind pose is asset-owned because 171 objects sharing an asset reported bit-identical poses,
  and reasoned that a per-object field "would have diverged". It would not: a per-object field
  initialised from the same asset stays bit-identical forever if nothing writes it. Worse, in this
  case both reads land on the SAME memory -- our accessor indexes `asset->node_records` -- so the
  agreement was close to tautological rather than evidence.
  
  WHAT DOES CARRY THE ARGUMENT IS THE ADDRESS: the engine's own
  `ILTModel_GetBindPoseNodeTransform` indexes `asset->node_records + 8`, and so does the accessor,
  so the bytes live in the asset's array. That is a fact about storage, from code.
  
  And note what still is NOT established: that the array is never written. Proving immutability
  needs a mutation-isolation test -- write through one instance, observe a sibling -- which is not
  something to do to a live game, so the claim is scoped to provenance and stops there.
  
  This is the second form of one mistake this session. Earlier, `engine_bounds()` read past the end
  of an object and returned CORRECT values because the address coincided, and every value check
  passed. Both directions of the same confusion: **a value comparison cannot tell you where a value
  came from.** Ask what the check would still report if the layout claim were wrong -- if the
  answer is "the same thing", the check is about something else.

- **PREFER THE STALENESS-FREE SOURCE when one exists.** Socket offsets live in the ASSET, not in a
  per-object cache, so eye and camera geometry can be measured with no evaluation, no engine call,
  no dirty flag and no game-thread requirement -- and the answer is identical on every instance of
  the model. After several passes fighting the bone cache, the useful measurement turned out to sit
  in data that was never volatile.

  Ask which of a fact's several representations is IMMUTABLE before reaching for the live one.

- **ANATOMY IS NOT AN INVARIANT. Measure the art before asserting anything about it.** I expected a
  character rig to place eye sockets symmetrically, level, with `left` on one side. All three failed:
  |left.x + right.x| reaches 6.013, only 8 of 30 rigs have the eyes level in y and z, and `left` is
  the -x side in 22 of 30. One rig has the two sockets at the SAME point, so the separation is 0.0 --
  which would have made a stereo baseline divide by zero.

  Assert the ENGINE's structure (both sockets share a node: 30/30, and the camera socket with them),
  report the ARTIST's numbers. A plausible geometric claim about content is a wish, and the range is
  what a consumer actually needs.

- **A PER-FRAME HOOK HAS A BUDGET, and exceeding it wedges the payload beyond recovery.** I wired
  a walk that evaluated every dirty skeleton on every model into the frame hook, driven by an
  off-thread request that then SLEPT waiting for the result. The game survived; the injected DLL's
  IPC did not.

  THAT FAILURE MODE IS UNUSUALLY EXPENSIVE, because `unload` is itself an IPC request. A dead IPC
  means there is no way to ask the module to leave, `--inject` refuses while any instance is
  resident, and every later run skips. The harness stops working until the game restarts -- which
  is exactly the cost this project is built to avoid.

  Rules taken from it: engine-thread work raised from off-thread must be BOUNDED per frame (a fixed
  small number of items, accumulated across frames), and the requester must not block on it. If a
  measurement needs hundreds of engine calls, it does not belong in a frame hook at all.

- **DO NOT "FIX" THE HARNESS THAT STARTS THE GAME WITH AN UNVERIFIED CHANGE.** Wedged out of
  `--inject`, I switched the fixture to `--reload` (which paints a fresh DLL name and so tolerates
  a dormant predecessor). It hung the run and cost a second restart.

  The reasoning was sound and the change may well be right, but the fixture is the one thing that
  must work before anything can be measured -- so it is the last thing to modify while recovering
  from a broken state, not the first. Reverted to the verified path; the leftover-instance problem
  is addressed by not wedging the payload.

- **`LNK1104` on the output DLL is not proof the game has it mapped.** The link failed to open
  `build/bin/fear2vr.dll` while a dormant instance was resident, which looked like the obvious
  cause. Renaming the file SUCCEEDED, so it was never locked -- a transient handle was. The
  genuinely mapped image was the renamed copy, which then refused deletion.

  One command distinguished a real mapping from a transient lock, and it was cheaper than any of
  the theories: try to MOVE the file. A mapped image cannot be deleted but a locked-by-a-stale-handle
  file can often be renamed.

- **RELATIONSHIP ASSERTIONS SURVIVE A RESTART; COUNTS DO NOT.** The game was restarted twice
  mid-session and the clean-socket population moved from 181 to 183, with the model-space split
  going from 131+50 to 131+52. Every check still passed, because they assert `engine == mine`,
  `a + b == total` and `usable == clean` rather than the numbers themselves. The one check that had
  been written as an absolute -- "every transform is finite" -- is the one that broke.
- **`retn N` AND THE FIRST INSTRUCTION SETTLE A CALLING CONVENTION. The decompiler does not.**
  Hex-Rays typed `ILTModel_GetSocketTransform` as `__stdcall(a1, a2, a3, a4)` and passed only three
  of them onward, which is not a signature you can call. Two reads fixed it exactly: `retn 10h`
  gives four dword arguments, and `mov ecx, [esp+arg_0]` shows the FIRST STACK ARGUMENT is loaded
  into ECX as `this` -- so the entry takes the OBJECT and the interface pointer is never touched.

  Before calling anything, get the argument COUNT from `retn N` and the argument ROLES from the
  first few instructions. A `__stdcall` callee pops its own arguments, so a wrong count is a
  corrupted stack, not a wrong answer.

- **THE ENGINE'S ERROR PATH IS A FREE ORACLE FOR ITS OWN SIGNATURE.** I passed the interface as
  argument one. The gate read `iface+0x10`, found 252 instead of 1, and returned exactly 60 --
  `LT_INVALIDPARAMS`, the constant sitting in the decompile. No crash, no garbage, and the return
  code named the reason.

  So report the engine's RAW RETURN CODE from a failed call rather than collapsing it to nullopt.
  "The call failed" cost a rebuild; "it returned 60 and the gate byte is 252" identified the bug
  outright. And a documented error constant is worth reading BEFORE the first call, because it
  tells you what a rejection will look like.

- **CALL THE ENGINE TO VALIDATE A REIMPLEMENTATION, and let ITS parameter tell you the convention.**
  `socket_world_transform()` had never been checked against anything but itself. The engine's own
  `GetSocketTransform` agrees on EVERY clean socket -- 181 of 181 -- which covers the branch the
  composition deliberately skips (models whose bone cache is already world-space) and which no
  internal check could reach.

  The trailing argument's meaning was measured, not guessed, by comparing against BOTH candidate
  answers at once: flag 1 matched the world pose 181/181, flag 0 matched the model pose on 131.
  The gap is the story rather than a discrepancy -- the other 50 keep their cache in world space,
  so both spaces coincide for them, and 131 + 50 = 181 exactly. Asserting that the two populations
  ADD UP is what shows the split is structural instead of a tolerance artefact.

- **GUARD A VTABLE SLOT INDEX; do not trust it.** A slot number is a claim, and a wrong one calls
  an arbitrary function instead of failing. `engine_socket_transform_available()` compares the
  entry against the function's known module offset before any call, so the SDK refuses rather than
  dispatching into the unknown -- and the fixture asserts it, so the claim cannot rot silently.

  Corroboration first, though: the client and server ILTModel tables independently place
  `GetSocket` at 2 and `GetSocketTransform` at 3. Two parallel implementations agreeing is what
  made the index worth verifying live at all.

- **KNOW WHICH ENGINE CALLS MUTATE.** `LTModelObject_GetNodeTransform` EVALUATES the skeleton when
  the node is dirty and clears the flag -- so `GetSocketTransform` is a pure read on a clean node
  and a state change on a dirty one. Restricting the comparison to clean sockets was not caution
  for its own sake: it kept an off-thread call read-only, and it happens to be exactly the
  population worth comparing.

  This also reframes the staleness finding. The engine NEVER reads a dirty cache raw: it recomputes,
  and if that fails it zeroes the output and returns false. Only a direct cache reader -- this SDK
  -- can hand back the garbage, which is why `stale` has to be exposed rather than smoothed over.
- **AN OVER-STRONG ASSERTION IS A LATENT FAILURE, and only a state change finds it.** "EVERY
  composed socket position is finite" passed for many runs and failed the moment the game was
  restarted. Nothing had changed in the code. 7 of 702 live socket transforms now had non-finite
  positions -- and all 7 were STALE, with zero clean ones affected.

  The assertion was wrong, not the engine. A model whose bone cache the engine has never
  evaluated holds whatever its allocation held, so requiring finiteness of never-written memory
  asserts something nobody promised. The contract that DOES hold is `clean => finite`, which is
  also the one a consumer depends on, and it is strictly more useful: it says "if the SDK reports
  this pose usable, it is".

  Generalise: when an assertion covers a population that includes a NOT-YET-INITIALISED subset,
  scope it to the subset the engine has actually written and assert the IMPLICATION instead.
  Then partition the population so the excluded rows still have to add up --
  `finite + nonfinite_stale + nonfinite_clean == total` -- rather than quietly ignoring them.

  A suite that only ever runs against one uninterrupted session cannot find this class of bug.
  Restarting the fixture is a test input.

- **The fourth varargs misalignment, and the first one a value check caught.** Adding two `%zu`
  fields whose arguments failed to land printed `3221225472` and `1080106038` -- float bit
  patterns from further down the argument list. Recognising THAT is the skill: an unsigned counter
  reading in the billions when the population is 702 is not a big number, it is somebody else's
  float.

  The cause was a `str.replace` whose pattern assumed a line break the file did not have, so the
  format string gained specifiers and the argument list gained nothing. Scripted edits to a
  60-argument `snprintf` MUST assert that both halves applied; the `JsonFields` builder added
  earlier exists precisely so new fields cannot drift, and I reached past it again because the
  surrounding code is still one big format string.
- **THE NEXT GLOBAL OBJECT'S ADDRESS BOUNDS THE CLASS. Check it before mapping a far field.**
  `LTWorldClientBSP` stood at `0x244` with a bounds pair at `+0x22C`, mapped because
  `IsPointOutsideWorld` reads `0x6F6E04` and `0x6F6BD8 + 0x22C` lands on it exactly. The
  coincidence is real. The conclusion was wrong: `g_pIWorldServerBSP`'s object sits at
  `0x6F6D48`, only `0x170` past the client's, with its OWN distinct vtable -- so the client class
  cannot reach `+0x22C`, and those addresses are file-scope globals shared by both classes.

  Two confirmations beyond the address arithmetic. The SERVER's methods reference the same
  addresses absolutely, and two classes cannot share one instance field. And the writer settles
  it outright: the server's world load READS those globals and stores `global -/+ 100.0` into its
  own `+0x04`/`+0x10` -- which is exactly the difference measured live between the two objects'
  bounds. Reading the producer decided in one function what address arithmetic could not.

  I have now been wrong in BOTH directions on this one pair: first reading them as globals
  (right, for weak reasons), then "correcting" that to instance fields on the strength of the
  schema note (wrong, and I recorded a self-criticism for the correct version). The fix is not
  more care, it is a cheap mechanical bound: the distance to the next singleton is a hard ceiling
  on a class, it needs no understanding of any field, and it is now asserted --
  `sizeof(LTWorldClientBSP) <= server_addr - client_addr`. Under the old claim, 580 against a
  368-byte gap fails outright, so several passes of this error would have been caught on the
  first run.

- **CODE CAN BE WRONG IN MEANING WHILE RIGHT IN VALUE, and no test notices.** While the schema
  said `+0x22C`, `engine_bounds()` read `bsp->bounds_min_2` -- past the end of the object -- and
  returned perfectly correct numbers, because that address IS the global's address. Every check
  passed: the copies agreed, and 15/15 points matched the engine's own function.

  Values agreeing is not provenance agreeing. When a field's ADDRESS can be reached two ways,
  a value check cannot tell you which one you are using -- only a structural fact (here the
  object's extent) can. Prefer the check that constrains the LAYOUT over the one that compares
  the contents.

- **A flag pair written together should be named as a pair, not guessed apart.**
  `IWorldClientBSP_AttachToServerWorld` sets `+0x48` and `+0x49` to 1 in consecutive stores on
  its success path, and the constructor zeroes both. They have separate getters (slots 7 and 8)
  but nothing found so far distinguishes them, so `is_world_loaded()` requires BOTH rather than
  picking one -- and the schema says outright that they are indistinguishable so far instead of
  inventing a difference.

  That attach function was worth reading for more than the flags: it is where the world path is
  written (`+0x4A`), which corroborates the getter at slot 9, and where `+0x44`'s resource is
  created, which explains slot 15. One function closed out four fields.
- **A MAGIC AND VERSION CHECK IDENTIFIES A LOADER INSTANTLY, and names its neighbours.**
  `IWorldClientBSP` slot 12 reads two dwords and compares them against `1128549463` and `126`.
  That constant is `"WLDC"` little-endian, so the function is the world-file entry point --
  and everything it calls is thereby placed: it loads through slots 17 and 18, and calls slot 5
  on failure, which independently CORROBORATES the `Unload` name I had given slot 5 from its
  body alone. A failure path is a free second opinion on whatever it calls.

  Convert unexplained comparison constants to ASCII as a reflex. Four printable bytes in a
  version check is a format tag, and it renames the function for you.

- **A getter's offset can identify a field the schema already describes.** Slot 9 is
  `lea eax, [ecx+4Ah]`, and `+0x4A` was already recorded as the loaded `.wld` path. That makes
  the slot `GetWorldPath` with no further work, and it cuts the other way too: the constructor
  zeroes exactly that byte, which is what an empty path looks like. Getter offset, schema field,
  constructor init -- three cheap facts that pin each other.

- **When a wrong offset would not FAULT, assert the value's SHAPE.** `world_path()` reads a
  character array; a wrong base returns adjacent bytes rather than crashing, and a plausible
  string is exactly what would slip through. So the checks are: every character printable, the
  length matching the returned string, the `.wld` extension the `WLDC` magic implies, and
  `world_name()` being the path's final component with no separator or extension left. Garbage
  fails all five.

  Also worth noting the corroboration: the live path is 43 characters, and the schema recorded
  the array as 44 bytes from an earlier observation -- 43 plus a terminator, arrived at
  independently.

- **Any string leaving this process must go through the escaper, not the format string.** The
  world path is full of backslashes, so `%s` produced invalid JSON at column 2678.
  `json_escape_append` has existed since the DatabaseMgr paths hit this exact trap, and I still
  reached for `%s` first because the surrounding code is one big `snprintf`.

  The reading side needs the inverse: the fixture gained `json_str`, which UNESCAPES. Comparing
  the raw JSON text would have compared `\\\\` against `\\` and silently tested the escaping
  instead of the path.
- **A BARE ADDRESS IN A DECOMPILE MAY BE A STATIC OBJECT'S FIELD -- BUT PROVE THE OBJECT REACHES
  IT. [SUPERSEDED, and left standing as the mistake it was.]** `IsPointOutsideWorld` reads six
  floats at `0x6F6E04..0x6F6E18`, and I reasoned that since the singleton sits at `0x6F6BD8` and
  `0x6F6BD8 + 0x22C` lands there exactly, they must be its fields rather than globals -- and
  "corrected" a working implementation to match, recording a self-criticism for the version that
  was right.

  THE REASONING WAS BACKWARDS. The next pass found `g_pIWorldServerBSP`'s object at `0x6F6D48`,
  only `0x170` past the client's and carrying its own vtable, so the client class cannot reach
  `+0x22C` at all. They are genuine file-scope globals, which is why both classes reference them
  absolutely. See the class-extent entry above for the bound that settles this kind of question
  mechanically.

  What survives of the original point: an absolute address inside a method CAN be a static
  object's field, and it is worth testing. What does not survive: treating the arithmetic as
  proof. Subtracting the singleton's address always yields *some* offset; that it looks sane means
  nothing until the class is known to extend that far.

- **Never derive a probe's INPUTS from the code the probe is testing.** My first run built fifteen
  probe points from `engine_bounds()` and compared my predicate against the engine's. Every point
  disagreed and the engine called all fifteen outside -- which reads as "the engine disagrees with
  me" when the truth was that `engine_bounds()` was broken and the engine was correctly rejecting
  fifteen nonsense coordinates.

  One bug produced a symptom that pointed at the wrong component entirely. Build probe inputs from
  an INDEPENDENT source (here the instance bounds), so a failure implicates the thing under test
  rather than poisoning the comparison.

- **A "module accessor" that returns a descriptor is not a base address.** `Modules::exe()` hands
  back a `const Module*`; `reinterpret_cast<uintptr_t>(exe)` compiles, yields a plausible
  in-process address, and read six floats of unrelated memory. The fix is `exe->base`.

  What caught it was printing THE VALUES, not the verdict. "The two bounds disagree" is not
  actionable; `[-13167.474, ...]` against `[-2.3e+29, 0.0, ...]` names the bug instantly -- the
  second is not stale data, it is not data. When two things disagree, print both before theorising.

- **Calling the engine's own function is the best oracle there is, when it is safe.**
  `IsPointOutsideWorld` is a pure comparison with no allocation, no locking and no side effects,
  and it is reachable through vtable slot 16 -- so no hardcoded address is needed, and the call
  site matches the engine's own. 15/15 agreement with the reimplementation.

  And CHOOSE POINTS WHERE A MISTAKE WOULD SHOW: the six face points and two corners lie EXACTLY on
  a bound, so they classify as inside only because the comparison is strict. An inclusive `>=`
  flips eight of the fifteen. A centre-and-far-away probe set would have passed either way.
- **A vtable's base comes from the CONSTRUCTOR'S STORE, never from scanning backwards.** I
  walked back from a known slot while the preceding dword looked like a function and concluded
  `GetVisTree` was slot 13, one below the schema's recorded 14 -- and was about to "correct" a
  note that was right. The constructor settles it: `mov dword ptr [esi], offset vtbl_...` gives
  the base, and it was one slot lower than my scan found because slot 0 held an address IDA had
  not recognised as a function (the destructor, since defined).

  Backward scanning is off by one for every unrecognised slot it stops at. And the near miss is
  the lesson: BEFORE overwriting a recorded claim, find the authority that settles it. I have
  corrected genuinely wrong notes this way, which makes it easy to correct a right one.

- **"There is no loader" is itself a finding.** I went looking for a world-tree stream loader by
  analogy with the vis tree and there is none. `LTWorldClientBSP_Construct` calls
  `LTWorldTree_InitFields`, which zeroes the bounds, sets `node_count = 1` and leaves the root
  null; nodes appear only as objects are inserted.

  That answers a question left open several passes ago. The vis tree is immutable level data
  loaded once, so nothing in it CAN go stale, while the world tree indexes moving objects and is
  mutated at runtime -- so the stale world-tree entries measured earlier are native to the
  design, not a fault. An absent function can explain a behaviour as well as a present one.

- **When a query is composition rather than reversing, check the PROPERTIES, not a second
  implementation.** `sectors_within`/`sector_hops`/`sector_component` are one breadth-first walk
  over an adjacency already validated from both directions. Writing a second walk to compare
  against would only prove the code agrees with itself -- the exact trap the plane-sign oracle
  fell into.

  What cannot be faked: one hop must equal the primitive (`{s}` + neighbours), the set must only
  grow with the limit, reachability must be SYMMETRIC because the adjacency is (2495/2495), and
  every member of a component must compute the same component. Then reconcile with a number
  measured independently: the component came out at 262 sectors and the portals were separately
  measured to join 262 of 263, so the sector outside the component is exactly the portal-less
  one. Two routes to one partition.

- **A truncation guard is what lets you size a buffer REACTIVELY.** The report's JSON buffer
  overflowed again, and the guard returned `{"ok":false,"error":"targets truncated",
  "needed":3118}` -- parseable, specific, and immediately actionable. An earlier version shipped
  half-written JSON and cost a confusing parser error instead.

  Note what made the difference: my probe raised a KeyError on a missing field, which looks
  exactly like a wiring mistake, and I spent two reads checking the format string and argument
  list before printing the raw payload. When a field is missing, READ THE RAW RESPONSE FIRST --
  the transport says what happened, the parsed view cannot.
- **THE ALLOCATION SIZE IS THE STRUCT DEFINITION. Read it before believing a stride.**
  `LTVisPortal` was recorded as `0x5C` with `vertices[4]` and a note that 4 "may be a hard
  maximum rather than a coincidence". `LTVisPortal_LoadFromStream` allocates
  `12*(vertex_count-1) + 56` -- a 44-byte head plus 12 per vertex -- so the record is
  VARIABLE-LENGTH and 0x5C is just the four-vertex case.

  `LTVisTree_ComputeLoadSize` says the same thing from the opposite end, budgeting
  `56*portal_count + 12*(total_vertices - portal_count)`: arithmetic that only makes sense if
  the head already contains one vertex and the remainder are per-portal extras. Two independent
  functions, one conclusion.

  The measured uniformity is real but explains nothing structural: 344/344 portals have 4
  vertices and all 343 consecutive pointer gaps are exactly 92 bytes, which is precisely what
  the formula predicts for n=4. A stride that matches on every live record is still a property
  of the ART when the allocator is size-dependent. Index through the engine's pointer table.

- **A bump allocator explains "contiguous" without promising it.** One `LTMem_Alloc` sized by
  `LTVisTree_ComputeLoadSize`, then `LTLinearAlloc_Alloc` carves the pointer table, portal
  bodies, sectors, planes and nodes out of it in order. That is why the first portal body starts
  exactly at `table_base + 4*portal_count`, and why every "immediately followed in memory by"
  observation in this schema holds. It is also why none of them are guarantees: change one count
  and every later base moves.

- **Clamping a COUNT is a wrong answer, not a safe one.** The portal reader clamped
  `vertex_count` to 4 "so a larger count would not read past the record". But the record is
  variable-length, so a larger count is legal and clamping tells the caller a six-vertex portal
  has four -- silently losing geometry, in a struct a consumer would use for clipping.

  Split the two facts: keep the stored count as the stored count, report separately how many
  fitted (`vertices_truncated`), and offer `portal_polygon()` that reads the real length. Bound
  the READ, never the reported count. And the bound should be justified as a bound -- 64 here,
  far past any plausible polygon -- rather than dressed up as knowledge of the format.

- **I swapped two function names last pass, and the callee bodies had the answer.** I named
  0x446000 the node loader and 0x4462E6 the portal loader, from the loop bounds at the CALL
  SITES. Both were backwards. The bodies settle it instantly: 0x446000 writes 8 floats, zeroes a
  pointer pair for `AttachSector` to fill later, and allocates 92 bytes for 4 vertices; 0x4462E6
  fills two child pointers at stride 24 and an element array at stride 48.

  A loop bound tells you HOW MANY, not WHAT. When two loaders sit side by side, read both
  bodies before naming either -- and my own note that "a wrong name outlives the pass that
  guessed it" is exactly what happened, one pass later, to me.
- **THE LOADER NAMES EVERYTHING. Read it before probing fields one at a time.** Three of
  `LTVisSector`'s unmapped fields fell to a single decompile of `LTVisSector_LoadFromStream`,
  because a loader has to touch every field in order and its allocation sizes give away what
  each count is for: `alloc(20 * n)` beside a byte read names that byte the plane count,
  `alloc(4 * n)` filled from `table[index]` names the other one a POINTER array.

  It also settles derived-versus-stored for free. The asset holds only 16 bytes per plane while
  the in-memory stride is 20, and the loader computes the difference through
  `LTVisPlane_ComputeCornerCode` -- so `corner_code` is provably a cache, not data, because it
  never existed in the file.

  And best of all it turned last pass's MEASURED rule into a READ one. `LTVisPlane_ComputeCornerCode`
  is, instruction for instruction, the corner rule I fitted to 125 data points: `x >= 0`,
  `y < 0`, `z >= 0`. The measurement predicted the code exactly. Fitting a rule to a whole
  population is sound, but reading the generator is free once you know which function to open.

- **A POINTER TABLE and the objects it points at have DIFFERENT BASES.** `LTVisTree.portals` is
  `LTVisPortal**`; the bodies follow the table in the same allocation. I converted a portal
  pointer to an index by differencing against the table base, and every conversion silently
  resolved nothing -- the alignment check rejected all of them, so the accessor returned an
  empty list rather than a wrong one.

  Caught only because the probe printed `listed = 0` beside `declared = 688`. Print BOTH SIDES
  of a conversion; a lone zero from a function whose contract allows empty looks like a
  legitimate answer. The fix computes the index from the body base and then confirms it by
  reading `table[idx]` back, so the fast path does not depend on the contiguity the schema
  happens to record.

- **One sample cannot tell a flag from an index.** `LTVisSector+0x20` was recorded as "live 0
  or 1" from a one-record sample. Read across all 263 sectors it is 0,1,2,...,262 -- exactly one
  of each. It is the sector's own index, and it is loaded from the asset rather than computed.

  Any field sampled on one or two records should say so, and small-integer fields deserve a
  full-population read before they get a name. The payoff here is a free check on every
  accessor in the file: the engine's stored index versus the pointer arithmetic used to reach
  it, 263/263.

- **"The loader writes a constant" does not mean the field IS constant.**
  `LTVisSector_LoadFromStream` stores a literal 35 into `+0x26`, but live only 62 of 263 hold
  35 -- the rest hold 3 or 6. Something later clears bits down (the values nest: 1, 3, 35).
  Re-reading after half a second of gameplay changed nothing on any sector, which rules out a
  per-frame visit stamp and leaves it a load-time classification. Recorded as flags with the
  clearing pass unmapped, rather than named on the strength of the one writer I had read.
- **The engine's STORED results are the best oracle available, and they are usually free.**
  Every spatial check before this compared a query against a scan built on the SDK's own
  per-sector test. But `LTSpatialRecord_CollectSphere` runs `LTVisTree_QuerySphere` with
  `AddEntry` as its callback, so each object's entry list *is* this query's output as the
  ENGINE computed it -- different code, different moment, same stored input. Feeding the SDK
  the volume the record itself holds makes the comparison exact.

  Result: `extra == 0` over 2565 entries -- the reimplementation reaches every association the
  engine made. No hand-written oracle earns that.

  Look for stored results generally: a cache, an index, a record, a count the engine maintains
  for its own use is a free answer key for whatever computed it.

- **Count the DIRECTION of a disagreement before naming its cause.** 569 of 2215 records
  disagreed with a query on their own volume. "Stale" was the obvious guess and it was wrong
  about most of them. Splitting the difference into `missing` (query found it, record did not)
  and `extra` (record has it, query missed it) assigns blame, because the two accuse opposite
  sides -- a stale engine record versus a hole in this SDK's traversal.

  `extra` was 0 everywhere, so the traversal was sound and every mismatch was the record
  lagging. A single "does not match" boolean cannot make that distinction, so the SDK exposes
  `spatial_record_diff()` with both counts rather than a bare bool.

- **A gate explains a mismatch population; find it before calling anything a bug.** Of those
  569, five hundred and fifty-three were objects the engine deliberately never collected:
  `LTObjectOwner_UpdateSpatialRecord` stores the volume UNCONDITIONALLY and collects only when
  `is_renderable()`, so a non-renderable object legitimately holds a current volume beside an
  empty list. 1087 of 1087 such objects were empty -- the gate is exact, not statistical.

  THIS IS THE THIRD TIME the same asymmetry has appeared: `LTObject_SetPos` writes the AABB
  unconditionally and relinks the world tree only when renderable; `LTSpatialRecord_SetSphere`
  is called before the collect gate. Treat "one writer, two consistency domains, an
  eligibility test between them" as a house pattern and look for the gate first.

- **A refuted hypothesis still pays if it was CHEAP and the measurement is kept.** `unk_10`'s
  discovery suggested the record might also tag its shape, and it does -- bit 0x80 of
  `volume_flags`, set by the sphere writer and cleared by the AABB writer. I predicted this
  would explain the 569 mismatches, since the SDK was re-deriving the shape from the object's
  type on the written-down belief that no tag existed.

  It explained none of them: tag and type rule agree 2215/2215. The prediction was wrong and
  the finding was still worth keeping -- the shape now comes from one engine-written bit
  instead of a reimplementation of six virtual functions, and the agreement is corroboration
  from two independent routes.

  The discipline that matters: I had already written "disagreed on 569 of 2215" into the code
  comment as the justification, BEFORE measuring. It was false. Write the number down only
  after the run prints it.
- **An unmapped field is often a CACHE, and finding its source proves both at once.**
  `LTVisPlane+0x10` sat as `unk_10` for several passes because the sphere path never touches
  it. It turned up as the first argument to the BOX path's plane reject, indexing an
  eight-entry table of AABB-corner selectors. Predicting it from the normal's signs failed
  0/125 on the first guess (I assumed the corner furthest along `-n`; it is the one furthest
  along `+n`), then matched 125/125 once corrected.

  A derived field is the cheapest thing in a binary to verify, because the source is right
  there: recompute and compare across the whole population. 0/125 versus 125/125 is also the
  most informative failure shape there is -- a wrong rule fails everywhere at once, so there
  is no ambiguity about whether you have it right.

  Then EXPOSE the currency check, not just the value. A cache can disagree with its source,
  and this one disagreeing would break the engine's box queries while leaving sphere queries
  correct -- a mod-visible asymmetry no consumer would guess at. Live: 125/125 current.

- **Two implementations of one geometric idea are the oracle you actually want.** Every
  earlier spatial check compared a query against a full scan built on the *same* per-sector
  test, which validates the traversal and nothing else -- and once shared an inverted
  predicate with the code under test, agreeing perfectly while both were wrong.

  The sphere and box queries share NO code: different engine functions, different descent
  arithmetic (`split > c+r` versus `split <= max`), different plane rejects (a `-radius` slack
  term versus a precomputed corner against zero). Geometry still binds them -- a sphere of
  radius `e` sits inside the box of half-extent `e` -- so `sectors_in_sphere` must return a
  subset of `sectors_in_box`. That check cannot be fooled by a shared mistake, because there
  is nothing shared to be mistaken in.

  Look for these pairs deliberately. An engine that implements the same idea twice for
  different argument types has handed you a free independent oracle, and the relation between
  them (subset, equality, bound) is usually simple enough to assert in one line.

- **Regenerating the SDK headers churns forward-declaration ORDER.** `sdk:generate()` emits
  them from an unordered container, so a one-field change shows up as ~60 changed lines across
  unrelated headers. `file_list.txt` also records the path style you passed in. Both are noise;
  confirm the diff touches no field offsets or names and move on.
- **Probe a spatial query at SEVERAL SCALES, and require the oracle to find something first.**
  `sectors_in_sphere` is checked against a full scan at radius 0, 250 and 4000 -- point,
  play-space, and most of the level. One radius is not enough: a descent bug can be invisible
  when the volume fits inside a leaf and only appear once it spans a split, or vice versa. All
  three agree exactly, over 106 sector hits.

  And the FIRST assertion is `hits > 0`, before any agreement is believed. That is not
  defensive padding -- this project has already shipped an oracle that found nothing, agreed
  with a query that found nothing, and read as success. `0 == 0` is the shape of a vacuous
  proof, so the population has to be established before the comparison means anything.

- **Collapse a special case INTO the general one rather than beside it.** `sector_contains`
  (point) and `sector_overlaps_sphere` (volume) started as separate code with the same
  structure -- box test then plane test -- and the point version is where the inverted plane
  sign lived. A point is a zero-radius sphere, so the point path is now literally
  `sector_overlaps_sphere(index, point, slop)`.

  Two implementations of one rule is two places for a sign to be wrong and only one of them
  gets exercised. Where the general case is the engine's own formula, the special case should
  be a call into it, not a copy of it.
- **A brute-force oracle that CALLS the code under test is blind to a shared error.** Point
  location was validated by scanning all 263 sectors and requiring the KD descent to name the
  same one. That caught two real bugs. It could never have caught a third: the plane
  containment sign was INVERTED, and both the descent path and the scan went through the same
  `sector_contains()`, so both were wrong together and agreed perfectly.

  `LTVisSector_TestSphere` settled it -- the engine tests the AABB first, then rejects when
  `dot(n, c) - d < -radius`, so POSITIVE IS INSIDE. My version rejected on `d > slop`.

  The blast radius was total, not marginal, and the counterfactual is now data rather than
  argument: all 125 planes across the 19 plane-bearing sectors read d > 0 at their own sector's
  centre, so the old rule rejected EVERY plane-bearing sector. It went unnoticed for several
  passes because the one query that exercised planes -- locating the player -- lands in a
  box-only sector, which 244 of 263 are.

  Two rules follow. First, an oracle must be INDEPENDENT of the implementation, not merely a
  slower path through it; scanning everything is independence in the SEARCH, not in the
  PREDICATE. Second, when a predicate is only exercised by a minority of the population, add a
  test that targets that minority directly -- here "every plane-bearing sector contains its own
  box centre", which is an invariant about the CONVENTION and fails loudly the moment a sign
  flips.
- **A weak check HIDES the error that would justify strengthening it.** The cull-volume rule
  was validated by comparing against the volume the engine had stored, and it matched
  3583/3583. But the sphere comparison deliberately omitted the RADIUS, because the radius
  rule on OT_MODEL's `sphere_source` path was not established -- so the check could not have
  detected a wrong radius, and therefore never did.

  Reading the engine's own virtual closed both halves at once. Object vtable SLOT 2 is the
  cull-volume producer and its RETURN CODE is the shape tag (0 none / 1 sphere / 2 box):

    OT_NORMAL          returns 0 outright
    OT_MODEL           returns 1 on both paths
    OT_WORLDMODEL      LTObject_GetCullVolume_AABB, shared with OT_CAMERA -> 2
    OT_SPRITE          tests its kind, returns 2 or 1
    OT_PARTICLESYSTEM  returns cull_volume_type ITSELF -- the field IS the tag

  `OT_MODEL_GetCullVolume` takes the radius from the shared asset (`**(float**)(obj+0xEC)`,
  LTModelAsset.radius) with NO scale on that path, while the other path does scale. My
  reimplementation applied `* scale` to both -- invisible live because scale is 1.0 on every
  object, and unreachable by the check because the check skipped the radius.

  Fixed, and then the check was STRENGTHENED to compare the radius too: still 3583/3583, so
  four floats are pinned where three were. The order matters -- understanding the mechanism is
  what made the stronger assertion legitimate, and the stronger assertion is what will catch
  the next error in it.

  Also worth noting what the virtuals CONFIRMED: `OT_SPRITE_GetCullVolume` reads the sprite's
  own aabb pair at +0x120/+0x12C, which is exactly the field a previous pass got wrong by
  reading LTObject's pair instead. The engine's code independently validated that fix.
- **A near-miss recorded honestly is worth chasing one more indirection.** Last pass ended by
  noting that `LTWorldTree_AddObject` starts its descent at `world + 0x1C` and that this could
  NOT be `LTWorldClientBSP + 0x1C`, since that offset is the confirmed node count (649) while
  the root sits at +0x20. The wrong claim was kept out of the schema and the discrepancy
  written down instead.

  Two indirections closed it. `world` comes from the object's owner (`LTObject +0x34`, the
  singleton `g_pLTObjectOwner`, shared by all 3583 objects) through owner vtable slot 3, which
  forwards to `IWorldClientBSP` vtable slot 13 -- and that slot is literally
  `lea eax, [ecx+4]; retn`. So `world` is `bsp + 4`, `world + 0x1C` is `bsp + 0x20`, and the
  engine's insertion starts at exactly the `world_tree_root` this SDK descends from.

  The arithmetic only closes once the `+4` is followed, which is why the offsets appeared to
  contradict. A pointer handed out by an accessor is frequently NOT the object's base -- check
  what the accessor returns before matching offsets against a class.

- **Correct a name that describes a guess, even when nothing is broken.** `0x40C44B` was
  called `LTObjectOwner_NotifyWorldBSP`. It notifies nothing: it returns a base pointer, and it
  is the link in the chain above. Renamed `LTObjectOwner_GetWorldTreeBase`. A wrong name is a
  claim that outlives the pass that guessed it, and this one would have sent the next reader
  looking for a notification path that does not exist.
- **Two engine functions can share a NAME and test different things.** The SDK exposed one
  "renderable" predicate -- `(flags & 1) && !(flags2 & 0x700)`, the draw gate from
  `LTObjectOwner_UpdateSpatialRecord` -- and a check quietly contained a second:
  `!(flags & 0x200) && (flags & 0x10C30)`, which IS `LTObject_IsRenderable` (0x4200A0).
  Different mask, different flag word, different purpose. Reproducing one and assuming it
  answers for the other is wrong in both directions.

  And the engine's name for the second one is misleading: every one of its callers is a
  `LTWorldTree_AddObject` gate, so what it actually decides is SPATIAL INDEX MEMBERSHIP.
  Exposed as `is_tree_eligible()` -- named for what it does, with the engine's name in the
  comment, per the rule about not letting a borrowed name outlive the evidence.

  That closed the staleness story quantitatively. `eligible => linked` holds with ZERO
  exceptions, so the engine always indexes what qualifies -- but 384 linked objects are no
  longer eligible, because the gate is only checked on the way IN and nothing removes an
  object when it stops qualifying. Those are the ones that go stale when they move: 384
  linked-not-eligible against 370 stale entries.

  Deliberately NOT asserted as `stale <= linked_not_eligible`: that holds only if every move
  of an eligible object goes through SetPos, which is exactly the sort of thing this log
  already records being wrong about twice. Printed instead.

- **A 78-specifier printf is a defect, not a style preference -- fix it at the source.** Three
  misalignments in one sitting, two of which printed plausible wrong numbers because the
  shifts partially cancelled. Replaced with a `JsonFields` builder that names each field AT
  its value: no format string, no argument list to drift, no fixed buffer, and the JSON
  escaping (which had already broken the payload twice on Windows paths) centralised.

  The conversion itself was done by SCRIPT rather than by hand: parse the format's key/
  specifier pairs, parse the argument list, and fail loudly unless the counts match. That both
  generated 78 correct calls and PROVED the call was aligned at the moment of conversion
  (78/78), which hand-editing could not have established.
- **Two derived values written by ONE function can still disagree -- read the gates, not just
  the writes.** `LTObject_SetPos` updates both of an object's cached geometry facts, so
  "written by the same function" looked like it should mean "consistent with each other".
  Measured from both sides: **zero of 3583 world AABBs are stale, while 370 of 2142 world-tree
  entries are.**

  The reason is in the function's own control flow. It calls `SetWorldAABB(position -/+ dims)`
  unconditionally, then relinks only `if (LTObject_IsRenderable(this))`. An object that moves
  while not renderable therefore gets fresh bounds and keeps its old spatial node. That
  replaced the previous pass's weaker story ("moved by some other route") with the engine's
  own gate.

  Generalise: when one writer maintains several caches, the interesting question is which
  writes are CONDITIONAL. A cache behind an `if` is the one that goes stale, and the way to
  find it is to assert the unconditional sibling EXACTLY -- 3583/3583 here -- so the
  asymmetry shows up as a contrast instead of being lost in two approximate numbers.

- **State the CAP with any sampled count.** `check_object_geometry`'s doc quoted "2126 sized,
  1457 pristine" -- the whole 3583-object set -- while its only caller passes `max_per_type`
  512, which samples ~2215 and splits ~1164/1051. Re-deriving the counts after a refactor
  looked like the refactor had lost objects. The invariant that does NOT depend on the cap is
  that the two states PARTITION the sample, and that is what the fixture asserts.
- **RESOLVED, and the accessor is what resolved it.** The 235 worldmodels a proximity query
  could not find have STALE index entries: `index_is_current()` -- which descends the
  engine's own box rule with an object's CURRENT bounds and compares against the node it is
  actually parked in -- reports all 235 as mismatched, out of 370 stale entries in 2142. The
  engine relinks only from SetPos/SetPosRot/SetDims/SetFlags, so a brush moved by any other
  route keeps its old node.

  Five hypotheses were needed. Four were guesses and all four were refuted by measurement;
  the fifth was found by building the primitive that could MEASURE the question rather than
  narrow it. Both primitives that came out of it -- `tree_slot()` and `index_is_current()` --
  are consumer API, and the second is the one a mod actually needs: do not trust a proximity
  result for something that moves without asking whether its entry is current.

  Note also which assertion is which. `every unlocated object is stale` HOLDS (235 == 235)
  and is asserted. The converse does NOT -- 370 are stale but only 235 are unlocated, because
  a stale node can still lie on the path a point descent takes -- so that direction is a
  bound and a report.

- **A printf format list and its arguments can go out of step in ways that CANCEL.** Scripted
  edits to a 60-argument `snprintf` left three missing arguments in one place and one extra
  `%zu` in another. The two shifts partially offset, so most fields still printed
  plausible-looking numbers and only the tail was obviously wrong -- `tree_linked` read 669,
  which is a real count from elsewhere in the same struct.

  What caught it was checking FIELDS WITH KNOWN VALUES first (objects 3583, attachments 362,
  brushes 1947) before reading the new ones. Do that whenever a varargs call is edited: a
  format/argument list has no type checking across it, so the only cheap guard is a handful of
  values you already know the answer to.
- **When guesses keep failing, stop guessing and build the accessor that MEASURES.** Four
  hypotheses about the 235 worldmodels that a spatial query cannot find were tested and
  refuted: truncation (raising the cap recovered 3), a position outside the object's own AABB
  (zero), a split-boundary tie between the engine's `split <= aabb_min` and a point's
  `p > split` (branching on it changed nothing), and being absent from the tree.

  The pass that made progress did not add a fifth guess -- it added `WorldBSP::tree_slot()`,
  which searches the tree for the node actually holding an object. That converted the mystery
  into facts: all 235 ARE in the tree, all 235 sit at LEAVES, at the same maximum depth as
  the 669 that work. Two structural explanations died at once, and the guess space collapsed
  to one candidate (a stale link -- the engine relinks only from SetPos/SetPosRot/SetDims/
  SetFlags, so a brush moved by another route keeps its old node).

  The accessor is also the better deliverable: two objects in the same node are spatial
  neighbours by the engine's own bucketing, which is a cheaper proximity test than any
  distance computation. A diagnostic worth writing is usually worth exporting.

  And write the elimination list down where the consumer reads it. Five ruled-out causes in
  the header are worth more than "known limit", because the next person to look starts from
  the surviving candidate instead of re-running the four that failed.
- **SELF-LOCATION is the oracle for any spatial index.** Extracting a proximity query from
  `WorldBSP::check` needed a quadtree descent, and the reusable trick is that an index must
  be able to find the thing it indexed: the engine linked each object at the node covering
  its AABB, an object's position lies inside its own AABB, so a descent toward that position
  must pass through that node. Every linked object has to find ITSELF. No wrong quadrant
  mapping survives that, and neither does harvesting only the leaf -- both still return
  plausible-looking neighbours.

  Live: 669 of 669 non-worldmodels self-locate. The convention itself was not guessed --
  `LTWorldTree_FindNodeForObject` states it, `child = (x > split_x ? 2 : 0) + (z > split_z ?
  1 : 0)` -- but the oracle is what proves the transcription, including that objects whose
  AABB STRADDLES a split stay at the parent, so every node on the path must be harvested.

- **Two refuted hypotheses in a row is a signal to scope the claim, not to keep guessing.**
  235 of 1473 worldmodels do NOT self-locate. Guess one, truncation: raising the result cap
  256 -> 4096 recovered 3. Guess two, a position outside its own AABB (plausible, since a
  brush's origin often is not its geometry's centre -- an earlier pass measured that):
  ZERO of the 235. Both measured, both wrong.

  So the assertion was scoped to the population where the invariant holds WITHOUT exception
  (non-worldmodels, 669/669) and the shortfall is REPORTED with the two refutations written
  next to it, in the header a consumer reads and in the fixture output. A percentage-based
  assertion would have buried a fact that is not understood; an exact claim over a smaller
  population keeps it visible for the pass that explains it.
- **When a record names two peers, SYMMETRY is the assertion -- not that each name is
  valid.** The portal table joins sectors through a `sector_a`/`sector_b` pair of pointers,
  converted to indices by an alignment-and-range test. Checking "both indices are in range"
  is nearly worthless: a wrong offset lands on a neighbouring array entry and produces a
  perfectly valid index. What no wrong offset survives is reading the SAME portal from both
  ends and requiring the two answers to agree -- if B is reachable from A then A must be
  reachable from B, because both edges come from one record.

  Live: 344 portals, 688 directed edges, 688 symmetric. The arithmetic closes exactly (two
  per portal), which also confirms the pointer-to-index conversion works in both directions.

  The generalisation is the same one the `entry_count`, `name_hash` and handle/pointer
  checks share: a structure that says one thing twice hands you a free oracle. A pair of
  peer pointers is that shape, and the oracle is a round trip through the graph.

  Note the bound rather than the equality, though: `sector_neighbours` deduplicates, so two
  doors between the same pair of rooms collapse to one edge. `edges <= 2 * portals` is the
  invariant; `edges == 2 * portals` is a property of this level's art and is REPORTED.
- **Build the brute-force oracle BEFORE the clever query, then keep it as the test.**
  Extracting point-location out of `VisTree::check` needed a KD descent, and the structure
  does not state two things it depends on: which child holds which side of a split, and
  whether sectors hang off internal nodes or only leaves. Guessing either is a coin flip
  that produces a plausible answer.

  So the fixture scans ALL 263 sectors and tests each volume directly -- O(n), useless in a
  frame, perfect as ground truth -- and asserts the descent names the same sector. That
  caught two separate errors in a row:

  1. A plane-ONLY containment test answered "cannot say" for 244 of 263 sectors, because
     only 19 carry planes. Every result was `nullopt`, the oracle returned 0, and the
     descent's 0 "agreed" with it. **A vacuous oracle agrees with anything** -- the first
     fix was making the oracle able to answer at all.
  2. With the oracle live, brute force found sector 142 and the descent offered 2
     candidates, neither of them 142. Flipping the split side changed nothing. The real
     cause: the tree attaches sectors to INTERNAL nodes too, so a descent that harvests
     only leaves misses the sector covering a whole region. Collecting at every node on the
     path gives 19 candidates including 142.

  The tell for (2) was in the original code all along -- `seh_read_and_walk` harvests
  `elements` from every node it visits, not just leaves. Reading what the existing walk DOES
  beats reasoning about what a KD-tree SHOULD do, which is the same lesson the shadowed
  sprite field taught one entry above.

- **A sparse optional field will masquerade as a broken read.** `LTVisSector.plane_count` is
  zero on 244 of 263 sectors, so the first version of this API -- which treated planes as
  the sector's shape and the AABB as a loose hull -- looked like a field-offset bug. It was
  the opposite: the box IS the shape and planes are an extra the art supplies 7% of the
  time. Measure the POPULATION of a field before designing around it; "present on the ones
  I looked at" is the same selection error as a range measured on interesting samples.

  Worse, the schema ALREADY SAID SO: `LTVisSector.plane_count`'s comment reads "live, only
  19 of 263 sectors have any planes -- the rest are AABB-only, which is why the loop is
  skipped for them." I wrote a header contradicting a fact this project had already
  established and recorded. Before designing an API over a structure, READ ITS SCHEMA
  COMMENT -- it is where past passes left exactly this kind of population fact.
- **A derived class can redeclare a base field's NAME at a different offset -- and lifting
  logic out is exactly when you switch to the wrong one.** Extracting the cull-volume rule
  out of `check_spatial_records` broke 9 objects. The old code read `s->aabb_min` on an
  `LTSpriteObject*`; my extraction read `obj->aabb_min` on the `LTObject*`. Same field name,
  and both compile:

      LTObject::aabb_min        @0x48
      LTSpriteObject::aabb_min  @0x120   <- a DIFFERENT pair, same name

  Nothing in the type system objects, because the derived class genuinely has both. The old
  code was right and I silently changed which one it meant while "just moving" it.

  Two things caught it, in this order: `unexplained` went 0 -> 9, and then reading the ORIGINAL
  expression character by character rather than reasoning about what it should have been.
  The first guess -- that my tolerance was too tight -- was WRONG, and chasing it cost a
  build; the existing `approx_eq` did carry a relative tolerance worth copying, but that was
  not the bug. Diff the expressions, not your model of them.

  Corollaries now in force: a refactor of measurement code must reproduce the measurement
  EXACTLY before it is believed (0 -> 9 is a failed refactor, not a discovery), and the
  geometric invariant that caught this -- `min <= max` on every box -- is now asserted,
  because it fails on a wrong field where a count can still look plausible.
- **A `check_*` function must AGGREGATE a consumer primitive, never contain one.** The rule
  this project now follows: if logic lives inside a check, a mod cannot use it, and the
  check is the only thing that ever benefits from the reversing that produced it.

  `CClientMgr::check_transforms` was the worst offender. Inside one SEH walk it built the
  quaternion-to-matrix conversion `R(q)`, read both cached matrices, compared the 3x3
  against `R`, compared the inverse against the transpose, verified the translation against
  `-R^T t`, and computed the determinant -- then threw all of it away and returned three
  counters. A consumer could learn "1450 of 1473 pairs are exact" and NOTHING about the
  object in front of it, despite the SDK having every ingredient in hand.

  Extracted to `Object.hpp` as `rotation_matrix(q)`, `brush_transform(obj)`,
  `brush_inverse_transform(obj)` and `brush_transform_quality(obj)` (with a `trustworthy()`
  verdict). The check became a snapshot loop that calls the last one and counts. The old
  private walker was DELETED rather than left beside the new path -- two copies of an
  offset or a formula is exactly how one goes stale.

  **Proof the extraction was faithful:** the refactored check reproduces the previous
  counts exactly -- worldmodels 1473 sampled / 1464 rotation / 1450 inverse / 1473 det, and
  cameras 474 on all three -- which are the numbers already written down in the schema from
  the old implementation. A refactor of measurement code is verifiable in a way most
  refactors are not: the measurement must not move.

  It also paid immediately. Exposing the matrix made a NEW cross-check possible that the
  aggregate could never express: the matrix's translation column must equal what
  `brush_to_world(origin)` composes -- two independent routes, one reading `m[3]/m[7]/m[11]`
  and one going through the helper, so a transposed read in either breaks it. 1947/1947.
  And the "round-trip a probe point yourself" advice in the header became a function call.
- **To prove a struct's EXTENT, read its far fields -- and prefer fields with a fixed
  encoding.** Having taken the real `D3DCAPS9` at renderer+0x0C, the obvious check was its
  first two fields (DeviceType 1, AdapterOrdinal 0). Those would read correctly for a base
  that was wrong about the struct's SIZE, or off by a member somewhere later.

  The fields that actually settle it are deep and self-identifying: `MaxTextureWidth`
  (16384, mid-struct) and the two shader versions near the end, which D3D encodes as tokens
  with a fixed high word -- 0xFFFE for vertex, 0xFFFF for pixel. Reading exactly
  `0xFFFE0300` and `0xFFFF0300` at those displacements is a fingerprint a misaligned read
  does not produce, and it is what makes returning the whole struct honest instead of a
  guess past the prefix we measured.

  Generalise: when a struct is large, pick check fields for (1) distance from the base and
  (2) a constrained encoding -- magic prefixes, version tokens, tagged enums, bit patterns.
  A field that can hold any integer proves the least.
- **To ask "is this interface hooked", ask where its METHODS are -- never where its vtable
  is.** I shipped `d3d9_vtable_owner()` reporting the module that owns an interface's vtable
  POINTER, and it happened to work: the Steam overlay's proxy `IDirect3D9` has a static
  vtable inside `gameoverlayrenderer.dll`'s image. Then the real device turned up with a
  vtable at `0x0A3ECBFC` -- heap memory, owned by no module -- and the function returns
  nothing for it. That is not an exotic case; it is how the D3D9 runtime allocates a device.

  I first read the heap vtable as evidence the device was ALSO proxied. Checking where its
  slots pointed refuted that in one measurement: all 61 sampled methods land in `d3d9.dll`,
  so the heap vtable is just the runtime's own allocation. The factory's 17 all land in
  `gameoverlayrenderer.dll`. The precise finding is that Steam wraps the FACTORY to
  intercept CreateDevice and hands back the GENUINE device -- which a vtable-location test
  could not have told apart from either alternative.

  API changed to `interface_impl_owner(iface, slot = 2)`. A vtable's ADDRESS says where a
  table was allocated; a method's address says who wrote the code.

- **When the engine hands an address to a typed API, the whole struct is mapped -- take
  it.** The renderer's fields were nearly published as three hand-rolled structs holding the
  few leading values measured. But FEAR 2 passes those exact addresses to D3D9:
  `GetAdapterDisplayMode(0, adapter+0x04)`, `GetDeviceCaps(renderer+0x0C)`, and
  `CreateDevice(..., renderer+0x158, ...)`. The engine has already declared each one's type,
  so returning a real `D3DDISPLAYMODE` / `D3DCAPS9` / `D3DPRESENT_PARAMETERS` is not
  over-claiming -- it is reading the declaration instead of re-deriving a prefix of it.

  The corroboration came free: every field is a canonical enum at its declared offset
  (`D3DDEVTYPE_HAL` 1, `D3DSWAPEFFECT_DISCARD` 1, `D3DFMT_D24S8` 75, `D3DFMT_A8R8G8B8` 21).
  And it makes the strongest assertion available: `CreateDevice` SUCCEEDED with that struct,
  so every field in it MUST be a value D3D accepts -- anything illegal means we are not
  reading the struct the engine passed.
- **"Its vtable is in that DLL" identifies the LIBRARY, not the CLASS.** Hunting the D3D9
  device, I scanned live globals for pointers whose first word lands inside `d3d9.dll` and
  found two holding the same object -- "found the device". They were Scaleform GFx texture
  slots, registered by name as `tGFxTexture1`/`tGFxTexture2`; an `IDirect3DTexture9` vtable
  is in `d3d9.dll` too. The scan was sound and the conclusion did not follow from it.

  What caught it was reading the ONE function that referenced each global instead of
  stopping at the scan result. A module-range test narrows a pointer to a library's worth
  of classes -- for `d3d9.dll` that is textures, surfaces, buffers, queries, the device and
  the factory. Narrow further with something class-specific (who writes it, what it is
  named, how many exist) before naming it.

- **A proxy in front of an interface is worth detecting, not just noting.** The engine's
  `IDirect3D9` vtable does NOT live in `d3d9.dll` on this machine -- it is in
  `gameoverlayrenderer.dll`, because the Steam overlay hands the game a proxy. Any mod that
  identifies D3D objects by comparing a vtable against `d3d9.dll`'s range, or patches a
  vtable slot expecting the runtime's code behind it, is wrong on most machines. So the SDK
  exposes `Render::d3d9_vtable_owner()` rather than a bool: the fixture asserts the vtable
  belongs to SOME loaded module (the mechanical claim) and REPORTS which one, since that
  depends on the machine and hard-coding either answer is the bug.
- **Write the prediction down BEFORE the measurement, so a wrong one cannot be quietly
  reinterpreted.** The brush round-trip check reported a worst error of 49.84 units. I
  reasoned that a near-inverse pair's error scales with distance from the brush origin, and
  the furthest origin was 17741 units out, so probing in brush space instead of world space
  would shrink it. Re-measured: **49.84466 -> 49.84435**. Not at all.

  The real answers were two, and neither was the guess: most of the "failures" were float32
  precision (0.07..0.1 units at level coordinates, so the 0.05 tolerance was simply too
  tight for the type), and a residual dozen are pairs that genuinely are not inverses --
  with scale 1.0 and det exactly 1.0, so not scaled or skewed, just inconsistent.

  The comment in the code now records the refuted prediction alongside the change it
  motivated, because the change was kept for a different reason than the one that prompted
  it. A tolerance should be MEASURED against the type and the coordinate range, not chosen
  because it looks strict.

- **A negative result about where NOT to look is worth as much as a mapping.** `OT_CAMERA`
  is not the view camera: 474 live instances, integer grid-aligned positions, none tracking
  the player, nearest 84.8 units away, no FOV field on the class, and no "fov" string
  anywhere in FEAR2.exe. That is a day a consumer does not spend. It is recorded at
  `ObjectKind::Camera` -- where someone hunting the view will actually read it -- and not
  only in the schema.
- **"The value is in range" is not evidence when two index spaces overlap.** The
  attachment field at +0x20 was mapped as a NODE index because all 27 live model-parent
  values fall inside `node_count`. They also fall inside `socket_count` -- both, 27/27.
  Worse, resolving them as node indices returned *plausible bone names* (`L_Shoulder`,
  `Torso`, `Neck`, `Pelvis`, `Null`), and a fixture assertion confirmed "EVERY socketed
  attachment resolves its socket to a bone name" for several passes. It passed because a
  table accepted the index, which is all it ever tested.

  A range test cannot separate overlapping spaces. What separated them:

  1. **The engine's own call.** `CLTCommonShared_GetAttachmentTransform` passes the field
     to `ILTModel_GetSocketTransform` -- a *unified* accessor addressing sockets first and
     nodes beyond `socket_count`. The field is a socket handle by construction.
  2. **Where the child actually is.** As socket handles the player's 0 and 1 are
     `RightHand`/`LeftHand`, and each child sits exactly there; as node indices they are
     `Null`/`Pelvis`, nowhere near. Generalised: 27/27 handles resolve as sockets and
     25/25 measurable children match our composed transform, worst error 0.0005.

  So: when a field indexes something and the target has MORE THAN ONE plausible table,
  find the engine call that consumes it, and test against a CONSEQUENCE (where does the
  thing end up) rather than a PRECONDITION (is the index valid).

- **A discrepancy you cannot explain is evidence, not a footnote.** Last pass measured the
  weapon sitting at `RightHand` while the record "named" node 0 (`Null`), wrote it up as an
  open question, and moved on. That mismatch WAS the disproof of the node reading -- it
  just needed to be treated as data rather than as an oddity to be documented around.

- **Do not "correct" the reference on the strength of a live distribution.** The same field
  had a note claiming the sentinel was 127, "NOT -1, whatever the reference SDK says",
  because 335/335 non-model records held 127. The engine tests `== -1` and never reads
  those 335 -- the type check short-circuits first. The reference was right; 127 was
  unread data. When live data contradicts a reference, look for the code that READS the
  field before deciding which is wrong.
- **The best test for derived arithmetic is a place the ENGINE already wrote the answer.**
  Our socket composition (asset socket record + bone cache + object transform) had been
  checked only against plausibility bounds -- is the point near the body, is it finite.
  Then the attachment system turned up an object the engine itself places at a socket, and
  the weapon's own `position` matched our composed `RightHand` transform to **0.000**.

  That is a different KIND of check from a bound. A bound says "not obviously wrong"; an
  independently-produced identical value says "right". Look for these deliberately: any
  place the engine caches, copies, or acts on a quantity you also compute is a free
  oracle, and it costs one comparison.

  The tolerance can then be tight on purpose (0.05 here, against a value that reads
  0.000), because a composition error moves such a check by units, not by epsilon. A loose
  tolerance on a strong oracle wastes the oracle.

- **An exact agreement is not an explanation.** The same pass: the weapon record names
  `parent_node` 0 (`Null`), while the weapon sits at the `RightHand` socket on node 38.
  The placement agrees perfectly with our composition and does NOT come from the field the
  record carries. It would have been easy -- and wrong -- to write "parent_node is where
  the child goes, confirmed to 0.000". Both the agreement and the unexplained route are
  recorded, and the SDK warns the consumer not to trust the field for position.
- **A range measured on the interesting samples is not the range.** `socket_count` was
  documented as "live 1..15 per asset" from a pass that looked at characters and weapons.
  Re-walking all 34 live assets gave **0..19** -- wrong at both ends, and the wrong end
  that mattered was the LOW one: most assets have ZERO sockets, because a shell casing or
  a wood-debris chunk has nothing to attach. A consumer trusting "at least one" would have
  treated the common case as a fault.

  The failure mode is selection, not arithmetic: sockets were being studied ON the assets
  that have sockets. When recording a range, say what population it was measured over, and
  prefer walking the whole set even when the interesting members are obvious.

  Same pass, same shape: a note claimed the player was "the only asset with a `camera`
  socket" while the fixture was already printing `32 with a 'camera'` two lines below it.
  The true claim was narrower and more interesting -- the player is the only one that
  routes `camera` to a bone other than `Head`.
- **When a structure stores the SAME thing twice, that redundancy is your best test.**
  The client shell keeps each local player in two parallel arrays: a `uint16` handle at
  +0x60 and the already-resolved `LTObject*` at +0x6C. Neither array proves anything on
  its own -- but resolving the handle through the engine's handle table and comparing the
  result against the cached pointer does, because the two are stored independently and a
  wrong offset on either side breaks the equality.

  This is the cheapest kind of confirmation available and worth looking for deliberately:
  a cached copy beside its source, a count beside a list, a hash beside a name, an index
  beside a pointer. The project has now used all four -- `entry_count` against a walked
  length, `name_hash` against a recomputed hash, `handle` against a table lookup, and
  here a pointer against a resolved handle -- and every one of them caught or would have
  caught a wrong offset that looked plausible in isolation.

  Corollary for the API: expose BOTH forms when the engine does. A caller talking to an
  ILT* entry point needs the handle, a caller reading through this SDK wants the pointer,
  and converting between them is exactly the step where a mistake hides.
- **Once you have a named API surface, AUDIT YOUR UNKNOWNS AGAINST IT.** The previous
  entry's lesson (a writer gives shape, a reader gives meaning) turns into a concrete
  procedure once the error-string pass has named a few hundred functions: take each field
  whose only evidence is a constructor or a setter, and go looking for a NAMED getter
  that touches its offset.

  First application resolved a field on the first try. LTObject+0x04 had sat for many
  passes as "ctor writes all four bytes as 0xFF, live 0xFFFFFFFF on 3265/3583, a -1
  none default, meaning unmapped". CLTClient_GetObjectColor hands out that exact dword;
  CLTClient_SetObjectRGB writes `obj[6]=r, obj[5]=g, obj[4]=b`; CLTClient_SetObjectAlpha
  writes only `obj[7]`. It is RGBA, and 0xFFFFFFFF is not a sentinel at all -- it is
  white, fully opaque, exactly what a constructor should write.

  Note how the old note's own evidence became the confirmation once the meaning arrived:
  "3265 objects at 0xFFFFFFFF and 318 with other values" reads as nonsense for a
  sentinel and as obvious for a default colour. The measurement was right; only the
  frame was missing. Which is the argument for recording raw distributions even when
  they explain nothing yet -- a later reader turns them into evidence for free.
- **You can map a MECHANISM perfectly and still name the CONCEPT wrong. Only a consumer
  settles it.** LTObject+0x74/0x7C/0x88/0x8C were mapped several passes ago as
  `child_list` / `parent_link` / `parent` / `attach_extra`, all marked CONFIRMED, and
  every structural claim in those comments was TRUE: the setter does link 0x7C into the
  other object's 0x74 list and write 0x88/0x8C as a pair, the live biconditional between
  0x88 and the link's self-pointing state did hold 3582/3582, and `self` at +0x84 really
  is the walker's way back to the object.

  The concept was still wrong. It is the STANDING-ON relation, not parent/child:
  ILTPhysics::GetStandingOn reads 0x88 as an object, gates on its type being a
  worldmodel, computes the surface height as `pos.y + dims.y` from it, and takes a plane
  from the node at 0x8C. Live the one non-null case points at the level geometry
  (dims 13375 x 9149 x 10200) with a floor plane of normal (0,1,0) at distance 1406.
  FEAR 2 keeps real attachments somewhere else entirely, at +0xB4.

  Two things follow:
  1. **A writer tells you the shape; only a READER tells you the meaning.** Every earlier
     claim came from the setter and the teardown -- functions that move bytes without
     revealing why. The interpretation arrived with the first function that CONSUMED the
     field for something, and it arrived immediately.
  2. **"CONFIRMED" should scope to the claim, not the field.** Those comments would have
     been right if they had said "the setter writes this pair and links that list";
     wrapping the same evidence in a borrowed name is what made them wrong. Prefer
     naming after the observed operation until a consumer names the concept for you.

  Also note this rescued a standing puzzle: 0x8C had defeated two guesses (a socket
  index, then an LTAttachment record). Both failed because they were the right shape
  family in the wrong subsystem, which is a specific and recognisable kind of stuck.
- **A reimplemented PRIMITIVE gets re-validated free in every later subsystem, so
  reimplement rather than call.** `String_HashI` was reversed several passes ago for
  skeleton node-name hashes -- `h = 0; h = g_HashCharTable[c] + 919*h` over a 256-byte
  remap table -- and reproducing all 660 stored hashes was the evidence then.

  The console-variable table turned out to key on the SAME function. Reusing the
  reimplementation reproduced all 191 stored hashes there too, and `hash & 0x7F`
  landed in the containing bucket on 191/191. Two unrelated subsystems, one function,
  zero mismatches -- which is far better evidence for the hash than either table alone,
  and it cost nothing because the code already existed.

  The general point: when a primitive is small enough to transcribe, transcribing it
  buys a cross-check every time it reappears. Calling into the engine's copy instead
  proves only that the engine agrees with itself. (The counterweight is real, and the
  transform math this project transcribes is the example: copy TERM FOR TERM when a
  convention is involved, because a rederived quaternion product can be self-consistent
  and still disagree with the engine.)
- **Two caches of the same shape can hold different SPACES. Check before composing.**
  A model carries two per-node transform arrays behind a mode selector, and an earlier
  pass mapped both as "the per-node transform cache" -- correct about the layout and
  silent about the frame of reference. They are not the same:

  ```
  selector == 0    297 clean bones, ALL near the model origin      -> model space
  selector != 0     46 clean bones, ALL at the object's position   -> WORLD space
  ```

  Zero exceptions either way. Composing the object's transform onto a bone that already
  carries it put a socket **5449 units** from its owner; branching on the selector
  brought the maximum to **137**, which is model scale.

  How to notice, since the layout test cannot: pick a point the two spaces disagree
  about and measure the distance to it. Model-space data clusters near the origin,
  world-space data near the object. One line of arithmetic separates them, and it needs
  no disassembly.

  **And a validation worth stealing: check a signed quantity against a SEMANTIC label.**
  Wanting to know whether the composition was oriented correctly, the test was "a
  character's camera socket should sit above its object". It held on one model and
  failed on the other -- which turned out to be right: the failing model was playing
  `Corpse_surface_facedown` and its head bone reads -83. The animation NAME and the
  bone geometry agreeing on "face down" is far stronger evidence than the naive check
  passing would have been, and it is why that count is REPORTED rather than asserted.
- **A TOTAL can match by accident while every element differs. Never validate a field
  by its aggregate.** Having just mapped the asset's piece count at +0x30, the endpoint
  reported 697 pieces summed over 215 models -- the exact number the socket walk had
  reported in an earlier pass. Two different fields at two different offsets summing to
  the identical total looks like a misread.

  It was not. Checked per asset, the counts differ on **215 of 215 objects**:

  ```
  assaultrifle    pieces  8   sockets  5
  grunt           pieces  7   sockets 14
  submachinegun   pieces 10   sockets  5
  shell casing    pieces  1   sockets  0
  ```

  and they still sum to 697 apiece. A coincidence, and a cheap one to resolve: print
  the per-element pairs and count the disagreements. The general rule is that a sum
  compresses away exactly the information that would distinguish two fields, so a
  matching total is never evidence of sameness and a differing total is only evidence
  of difference. Compare element-wise or not at all.

  The mirror-image error is more dangerous and this project has committed it: bounding
  a piece index by the MATERIAL count. Those two totals differ (476 against 697), so
  the aggregate WOULD have caught it -- but nothing was comparing them, because both
  numbers looked plausible on their own.
- **When a cache looks like garbage, look for the VALIDITY FLAG before doubting the
  layout.** The per-node transform arrays read as `(position, unit quaternion)` at
  stride 28 -- 2222 of 2222 slots had a unit rotation, which is about as strong as a
  shape signal gets. But the positions included values like `7.8e37`, so the obvious
  conclusion was that the layout was wrong.

  It was not. The engine keeps a per-node DIRTY BYTE and recomputes a node's transform
  before using it when that byte is set. Partitioning the same 2222 slots by that flag:

  ```
  dirty clear   343 slots   343 sane positions     0 wild
  dirty set    1879 slots  1692 sane positions   187 wild
  ```

  Clean implies sane with zero exceptions; every wild value sits behind a set flag. The
  layout was right the whole time and the data was simply stale.

  Three things worth carrying forward:
  1. **A field can be structurally valid and semantically meaningless at once.** The
     rotation is unit-length even in stale slots, so it cannot serve as the validity
     test -- the neighbouring flag can.
  2. **The engine's own reader tells you the predicate.** Rather than inventing a
     sanity heuristic, find the function that consumes the field and copy its gate.
     Here `GetNodeTransform` tests `dirty[stride*i + off]` and takes a recompute path;
     that IS the contract, and it belongs in the API as a `stale` flag.
  3. **Two caches can coexist behind a selector.** This model holds TWO per-node
     arrays with dirty arrays of DIFFERENT strides (2 and 3, flag at byte +0 and +1),
     and a mode field picks one. Reading the wrong one looks correct on the 193 models
     whose selector is zero and is silently wrong on the other 22 -- which no
     single-array stride scan could ever have revealed.
- **A probe loop that forgets to ADVANCE reports inflated counts that look real.**
  While classifying attachment sockets I wrote a walk that read `record + 0x20`, did
  the classification, and never assigned `cur = record->next`. It re-read the FIRST
  record of every list 64 times and reported "768 model sockets in range, 8128
  sentinels" -- ratios that were perfectly consistent, conclusions that were
  perfectly wrong, and no error anywhere.

  What exposed it was ARITHMETIC, not suspicion: 768 + 8128 = 8896 = 139 x 64, i.e.
  every list had run exactly to the iteration cap. A total that is a clean multiple of
  your own loop bound is never a coincidence.

  Two habits fall out, and both are cheap:
  1. **Print the record count and compare it against an independently-derived total.**
     The correct walk found 362 records; the broken one found 8896. Either number
     alone looks fine.
  2. **Walk with cycle detection from the start** -- a visited set, and a distinct
     report for "terminated" versus "hit the cap". The corrected walk reported 139
     lists, 362 records, lengths 1..16, zero cycles, which is a sentence a broken walk
     cannot produce.

  Note the SDK version of this walk carries the cycle guard into shipped code for a
  different reason -- the engine's list can be torn mid-frame -- so the test habit and
  the production requirement happen to agree here.

- **CONFIRMED failure mode of the error-string naming technique.**
  `ILTModel::GetAnimName`'s not-found path logs the string **"GetNodeName"** -- a
  copy-paste bug in the shipped binary. So a function CAN print somebody else's name,
  exactly as the earlier warning feared. Two mitigations, both already in the recipe
  and both load-bearing: requiring a string to have exactly ONE referencing function
  drops most of these, and spot-checking behaviour catches the rest. Here the
  identification survives because the CODE indexes an animation table and copies a
  name out of the record -- what a function prints on failure is the least reliable
  thing about it.
- **THE ENGINE NAMES ITS OWN FUNCTIONS. Read the error strings first.** This is the
  highest-yield technique in this file and it should be tried before any behavioural
  classification, because when it applies it gives an EXACT name, not a guess.

  FEAR 2's engine logs the qualified name of the failing function on its argument-
  validation path:

  ```
  if ( a1 && a2 ) { *a2 = *(_WORD *)(a1 + 18) != 0xFFFF; return 0; }
  sub_4687B8(60);
  if ( dword_6ECAA4 >= 2 ) sub_468BD1(off_6E307C, aCltclientIsser);  // "CLTClient::IsServerObject"
  ```

  Harvesting every string that looks like a qualified C++ name (`::`, no spaces,
  initial capital, under 60 chars) found **258**, of which **222 are referenced by
  exactly one function** — a 1:1 name-to-function mapping, applied in one pass:
  `CLTClient::GetObjectPos`, `SetObjectRotation`, `IsServerObject`,
  `ProcessAttachments`, `CLTCommonClient::GetAttachmentObjects`, the whole light API,
  and so on. 213 renames, 9 already named.

  **Validate before bulk-applying, and this dataset validated itself twice over.**
  Four of the pairings landed on functions already named from independent evidence
  (`CClientMgr::Init`, `CClientShell::Update`, `CClientMgr::StartShell`) and MATCHED.
  Then two were checked by decompiling: `IsServerObject` is `handle != 0xFFFF` and
  `GetObjectPos` copies `obj[5..7]`, both exactly what the schema already said at
  +0x12 and +0x14. A mapping that reproduces facts you established another way is
  safe to trust across the rest of its range.

  The failure mode to guard against is a function logging SOMEONE ELSE'S name --
  a forwarder, or a shared validator. Requiring the string to have exactly one
  referencing function removes most of that risk; the rest is why you spot-check.

  Corollary worth internalising: an engine that validates arguments and logs is
  documenting itself. Look for the logger, then read its callers' string arguments --
  it inverts the whole problem from "what does this code do" to "what did the authors
  call it".
- **Searching for another INSTANCE of a known class: match an invariant, never a
  shape.** Looking for a second object manager, the obvious filter was structural --
  seven consecutive 8-byte pairs that each either self-point or hold two heap
  pointers. Scanning 512KB of globals with it returned **798 candidates**, and every
  one inspected was ASCII text (`0x736F6940`, `0x243F5544`) or unrelated data that
  happened to fit. Swapping the filter for a proven INVARIANT -- walk all seven
  buckets and require every object's `type` byte to equal its bucket index -- returned
  **0 false positives** over the same range, while still scoring the known manager at
  3583 objects with zero mismatches.

  The difference is that a shape asks "could these bytes be a manager", which random
  data answers yes to surprisingly often, while an invariant asks "do these bytes
  BEHAVE like one", which requires the pointed-at memory to be internally consistent.
  Corollary: proving an invariant pays twice -- once as a test, and again as a search
  key. A pointer-validity check is not an invariant; a relationship between two
  independently-read fields is.
- **A vtable slot can point at bytes IDA never turned into a function.**
  `OT_MODEL`'s slot 0 (0x42A844) was live, referenced from a vtable, and
  decompiled fine afterwards — but `rename` failed with "Function not found"
  and `set_type(kind="function")` failed the same way. `define_code` only
  makes an instruction; the tool that works is **`define_func`** (with an
  explicit `end` for the byte range). Read the raw bytes first and confirm the
  prologue/epilogue before defining: here `push esi; mov esi,ecx; call ...;
  test byte [esp+8],1; ...; retn 4` is the unmistakable MSVC scalar-deleting
  destructor shape, so the definition was safe rather than a guess. If a
  rename fails with "Function not found", do not skip the slot — undefined
  bytes at a referenced address mean IDA's analysis stopped short, and every
  such gap is a real function you are missing.
- **Hex-Rays pointer casts are inference, not evidence.** In `LTObject_SetPos`
  the pseudocode reads `v3 = *(char **)(*(_DWORD *)(this + 56) + 52)` and
  passes it to a `%s` format, which reads unambiguously as "the object's name
  lives at record+0x34". Live, that field holds values like `0x0001D87A` on
  3583/3583 objects — never a valid pointer. The `char*` came from the
  decompiler matching a printf argument, not from the data. Any field the
  pseudocode types as a string, pointer, or float is a hypothesis until read
  out of the live process. (The branch in question also never executes: it is
  gated on `flags3 & 0x400` being clear, and that bit is set on every live
  object — so dead code taught a wrong lesson twice over.)
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
