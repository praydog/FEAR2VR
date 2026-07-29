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
