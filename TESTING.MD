# fear2vr Testing & Verification Guide

Testing standard for the FEAR 2: Project Origin mod framework. Modeled on the
kananlifter / il2cpp-scripting testing bar. This file is the contract: when a
test, a claim, or a fix conflicts with it, this file wins.

## Core Principle

**Demonstrate before you fix.**

Never claim a mapping, offset, hook target, or engine semantic is wrong,
missing, or "unsupported" without a test that *fails at runtime against the
current code* — in this project "at runtime" means *against the live game
process*, observing the real `FEAR2.exe` from `test/fixture_test_runner.cpp`.

The same bar applies to a **POSITIVE** claim. "The pointer resolves", "the
hook fires per frame", "unload is graceful" all require a test that *passes*
against the current code for the exact reason stated — not because the check
was vacuous (`if (ptr != 0)` is not a resolution proof; see "The rules that
make a fixture test VALID").

When a test goes red, **fix the CODE — never weaken the test.** Prohibited:

- Loosening an assertion (`==` to `!= 0`, exact address to "somewhere in .text").
- Narrowing the input so the failing path is never exercised.
- Swapping the in-game observation for a stub/assumed value.
- Adding a special case so the test input bypasses the mechanism under test
  (e.g. accepting "/health answered" as proof a hook fires — that tests the
  socket, not the hook).
- Deleting or `#if 0`-ing a red test.
- Asserting against LOG output instead of observable process state.

The ONE exception is a test that is *itself* genuinely wrong: you must be able
to state precisely *why* the old expectation was invalid (with IDA evidence:
address, bytes, disassembly) and ideally show the corrected test still fails
the old code. **"The test is buggy" is the single most dangerous sentence in
this file** — it must come with that proof.

## Two tiers of test

| Tier | Target | When it runs | Exit |
|---|---|---|---|
| Tier-1 | `command-server-test` | Always (CI, headless, no game) | 0/1 — no environment excuse |
| Tier-2 | `fixture-test` | When FEAR2.exe can run | 0 pass / 1 fail / **77 skip** |

Tier-1 mock-tests the IPC command server over localhost HTTP in-process: health
fragment plumbing, `/sdk/targets` and `/engine-hook` routing, 404/400s, `/unload`
latching, stop() semantics. If Tier-1 fails, the fix is in
`src/ipc/CommandServer.cpp`, full stop.

Tier-2 (`test/fixture_test_runner.cpp`) is the real loop:
`spawn-or-attach FEAR2.exe -> inject fear2vr.dll -> /health live -> host-side
assertions over SDK-computed diagnostics + OS-level ground truth (Toolhelp32
module geometry) -> /unload -> module-vanish + game-alive proof -> re-inject
freshness proof`.
All assertions live in the runner; the DLL ships **no test code**. The runner
performs **no ReadProcessMemory at all** (rule 2 below).

## The fixture is ground truth — and tests are NOT in the mod

`FEAR2.exe` (32-bit LithTech Jupiter EX, D3D9) is the oracle of truth for every
mapping claim. Concretely:

- An address/offset/scan-pattern in `shared/sdk` is only *verified* when the
  fixture runner reproduces the claim from live process state: the diagnostics
  endpoint reports a value the SDK computed IN-PROCESS, that value satisfies
  the structural invariant (inside the owning module's `[base, base+size)` per
  the host's own Toolhelp32 module list — never per the DLL's self-report),
  and the process/IPC survived the SDK making the call.
- IDA static evidence (decompile, xrefs) justifies *writing* the mapping; the
  fixture test justifies *keeping* it. Both are required. A rename in IDA with
  no fixture coverage is a hypothesis, not a mapping.
- **Nothing test-shaped ships in fear2vr.dll.** The mod exposes diagnostics
  (`/health`, `/sdk/targets`, `/engine-hook`) that double as legit tooling; the
  pass/fail judgement lives host-side, in `test/fixture_test_runner.cpp`.
  Adding asserts to the shipped DLL is a review-level defect.
- `gameserver.dll` absent at main menu is why it is `required=false` in
  `shared/sdk/Modules.cpp` — a fixture-derived fact, not an assumption.

A fixture red is information, never "flaky". Reproduce the specific claim
standalone (`curl http://127.0.0.1:8798/sdk/targets` plus a debugger read)
before believing anything else.

## Getting a test-ready fixture: `python tools/resume_game.py`

Most of this suite needs a loaded world and a live player. At the main menu 103 checks
go red for want of a client shell, a world and a player -- legitimate engine state, not
defects -- against 3 in-world. So "is the fixture in the right state" is a prerequisite
of reading any result, and it is automated:

```
python tools/resume_game.py          # idempotent; run before a session or after a crash
```

Verified end to end from a TERMINATED process: 71 seconds, cold, no human input.
Dead game -> Steam launch -> inject -> `Menu.StartCheckpoint` -> Space -> in-world,
followed immediately by a full suite run of 1532 checks.

It matters for the evidence rules, not just convenience. A red that only appears at the
menu is a state, and a suite run against the wrong state produces a hundred plausible
failures that mean nothing -- exactly the "narrowing the input" failure this file
prohibits, arrived at by accident instead of by choice.

**Why it does not synthesise a menu click.** All three input routes were measured
against the main menu and none works: `SendInput` moves the cursor and the highlight
follows but nothing activates; the window-message key queue is never drained (buffered
input reads false); and a press written through the engine's own entry points is
visible in the engine's own `MouseState` and ignored by Scaleform. So the script
dispatches the game's own UI command instead -- `sdk::UiCommands`, the table a click
goes through anyway. Full evidence in `reversing/MAPPING_WORKFLOW.MD`.

Once a world IS loaded, synthetic input works, because the game consumes it rather than
the front end. That is why step 4 (a synthetic Space on the load screen) succeeds where
a synthetic click on the menu does not.

## Uninject is tested with something ARMED, and that is why it caught this

TESTING.MD already required the teardown path to be held to the same bar. The watchpoint
work sharpened it: the suite now leaves a hardware watch **armed on purpose** across the
uninject, because debug registers live in every thread's context and the vectored
handler lives in code about to be unmapped. The existing "game process survived
uninjection" assertion only means something if something was armed.

It earned its keep immediately. `Watchpoints` excluded the CALLING thread from its
thread sweep -- correct when arming, so our own SDK reads do not trap and report this
mod as the accessor, and fatal when disarming: the unload thread kept its DR7, then
`RemoveVectoredExceptionHandler` took the handler away, and the next trap had nowhere to
go. An unhandled `STATUS_SINGLE_STEP` terminates the process.

The suite reported it precisely as `[FAIL] game process survived uninjection`.

Two generalisations worth carrying:

- **A symmetric-looking pair of operations may not be symmetric.** Arm and disarm shared
  one helper and one exclusion rule; only one of them wanted it.
- **Raise the stress of the teardown test deliberately.** The bug existed while the
  armed watch was on a rarely-written field and stayed invisible. Switching the target
  to one read ~480 times a second is what turned a latent crash into a reproducible one.

## The rules that make a fixture test VALID

1. **Prove residency host-side.** Module geometry comes from the runner's own
   Toolhelp32 snapshot of the PID. "Pointer is inside module image" is asserted
   against THAT image list, with the image bounds printed on failure.
2. **NO ReadProcessMemory. NO MAGIC VALUES. Test the SDK, not a copy of it.**
   The mod runs in-process; the claim under test is "the SDK, compiled from
   `reversing/fear2.genny`, produces what we expect when it runs there." A
   host-side RPM read re-does what the SDK already did, and to do it the test
   must hardcode offsets — which duplicates the schema as magic values in the
   test and proves only that our hand-typed numbers agree with themselves.
   **A test containing a literal struct offset is broken by construction**,
   even when it passes: it silently rots the instant the schema changes.
   - Need to assert a structural relationship? Add an SDK METHOD that
     expresses the invariant through the generated schema (the compiler
     derives the offsets), report it in diagnostics, assert on that. See
     `sdk::CClientMgr::counter_node_registered()`.
   - Need to catch the DLL "hallucinating" its diagnostics? Use invariants
     between independently-mapped values (two schema fields at different
     offsets/types that must agree numerically — if either offset were wrong,
     they wouldn't), plus the OS-level oracle in rule 1. Toolhelp32 module
     geometry is OS *metadata*, not a memory read, and is the one external
     ground truth the DLL cannot fabricate.
3. **Live behavior beats static shape.** A hook test asserts
   `frame_ticks` advanced between two polls (the hook fired) and that after
   unload the module is gone *and* the game process still lives. Static
   "the hook is installed" health fields are necessary, never sufficient.
4. **Rejection tests assert semantics, then cross-check.** The
   `/engine-hook?name=fear2vr_no_such_hook` negative test must observe
   `rc != 0` — the engine's own rejection path, not a client-side 404.
5. **Freshness after reload.** Re-injecting into the never-restarted game must
   yield a fresh counter set (`frame_ticks` small again) — otherwise "graceful
   unload" only proved the port closed.

Violating any of these makes the test non-evidence. Fix the test, not the bar.

## Step-by-step: add a demonstrate-then-fix change

### 1. Map in IDA
Locate the target in FEAR2_dump.exe (or the gameclient/gamedatabase/gameserver
IDBs) via strings/xrefs; confirm shape against the lithtech / FEAR sources in
`I:\Programming\projects\fear-source-code` (structural analogue, not gospel).
Rename the function/globals in IDA via MCP. For the full mechanics (IDA →
ReGenny live-verify → `.genny` authoring → codegen), see
`reversing/MAPPING_WORKFLOW.md`.

### 1b. Don't know where it's read or written? Trap it — `/watch/*`

Before reasoning about *who* touches a field, measure it. The mod exposes hardware
data breakpoints (DR0–DR3 + a vectored exception handler, four slots, byte-exact):

```
/watch/arm?addr=0x02938740&size=4&type=write|rw|exec&max_hits=4000
/watch/report      -> accessors, registers, value at trap, caller candidates
/watch/clear?all=1
```

Each hit reports `module + offset + static`, so it goes straight into the right IDB,
plus `ecx` separately (`__thiscall`'s `this` — the difference between *"writes +144
on something"* and *"writes +144 on THE camera"*).

**Use this instead of scanning the binary for stores to an offset.** That scan
answers a different question than you asked and has produced a plausible wrong
answer every time it was tried in this project — 67 unrelated functions, in the case
that established the rule. `reversing/MAPPING_WORKFLOW.MD` records each instance.

**The loop is: trap → read in IDA → walk up → hook the decider.**

The engine clock is the worked example, and it ends in a shipped fix:

1. Publish the address (`Engine::client_time_addresses()` walks the same pointer
   chain the engine's own accessor does).
2. `/watch/arm` a **write** watch on it → the store is `add [esi+30h], eax`, and the
   caller candidates are `CClientMgr__Update`, `CLTTimer_AdvanceByWallClock`,
   `CLTTimer_TickChildren`.
3. Decompile the innermost writer → the whole timer-node layout falls out,
   including a **pause byte** at `+0x58` and a rational **time scale** at
   `+0x48/+0x4C`.
4. Watch the *pause byte* → its writer is `ILTTimer::SetPaused`, and the stack above
   it names **gameclient.dll** frames: the pause policy is the game's, not the
   engine's.
5. Hook that one function and refuse the request while the window is unfocused →
   the world no longer freezes on alt-tab. Measured: 24 pause requests, 24 refused,
   clock advancing 3371 ms per 3 s while alt-tabbed.

That chain replaced an earlier guess — that the *focus flags* stopped the world —
which was wrong, and which cost a hung game to disprove.

**Two hardware facts the suite asserts rather than trusting memory:**

- A **data** watch is a TRAP: the reported address is the instruction *after* the
  accessor, hence the field name `eip_after`. An **execute** watch is a FAULT and
  reports the instruction itself (`is_fault`), and it requires `EFlags.RF` or the
  game locks up re-faulting forever.
- x86 has **no read-only data breakpoint**. A read request is served by
  read-or-write, and the reply says so instead of pretending.

**Watch targets in tests must not need a human.** A check that only fires when
somebody moves the mouse is a coincidence detector. The suite watches
`g_ClientGlob_bClientActive` (read ~480×/s at rest) and `LTClient_IsClientActive`
(executed ~380×/s) — no world, no player, no input. The first version of that test
watched the camera's rotation and passed only by accident: the engine *elides* that
write when the view doesn't change (0 hits idle, 957 with the mouse moving).

**Arming across an uninject is deliberate.** Debug registers live in every thread's
context and the handler lives in code about to be unmapped, so the suite leaves a
watch armed on purpose before `/unload`. The existing "game process survived"
assertion is what covers the teardown ordering, and it only means something if
something was armed.

### 2. Write the SDK surface
Class-based, REFramework convention: resolution lives in the accessor itself
(function-local statics), ownership lives in the class that owns the concept
(`sdk::CClientMgr`, `sdk::CClientShell`, `sdk::Engine`, `sdk::DatabaseMgr`,
`sdk::Modules`). Patterns are kananlib-scanned by `Modules::get().scan_exe`
inside the owning class's .cpp — never a shared "all the scans" file, never a
hardcoded absolute VA. Document the IDA evidence inline.

**Engine interfaces are a special case with its own convention.** LithTech
publishes subsystems as named interfaces (`ILTClient.Default`,
`ILTModel.Server`, ...). Do NOT add a pattern per interface: they are all
reachable by name through `sdk::interfaces::Registry`, which rediscovers every
holder at runtime from one anchor. To add or refresh one, edit
`reversing/interfaces.txt` and run `python reversing/gen_interfaces.py` --
`shared/sdk/interfaces/*` is GENERATED; never hand-edit it. Rules that fall out
of the mechanism, all of them load-bearing:
  - **One class per interface, one getter per instance.** `.Default`/`.Client`/
    `.Server` are instance names of a single interface with a single ABI.
  - **Never cache a resolved interface pointer.** `APIRemoved()` clears slots;
    `get()` re-reads every call.
  - **`nullptr` is a state, not a failure.** Assert shape and invariants
    (holder count >= 1, `all_agree`, getter == registry), never "X is non-null"
    for an interface whose module may not be loaded.
  - **Interface types stay opaque** until that interface's vtable is actually
    reversed and mapped in `fear2.genny`. A speculative method is invention.

### 3. Extend the diagnostics (if the new surface needs visibility)
Minimal JSON field or endpoint on `/health`, `/sdk/targets` — diagnostics
format, no assertions.

### 3b. Two assertion anti-patterns that already bit us

**Never invent a numeric tolerance to make a comparison pass.** If two values
are compared and they don't match, the choice is to find the invariant that
actually holds or to drop the assertion — not to widen a window until it goes
green. A `diff <= 64` on live object counts was written and cut before landing
in 2026-07: 64 had no evidence behind it, it compared two walks taken at
different instants against a list known to churn, and it would have "passed"
while asserting nothing. What replaced it: assert the parts that are actually
invariant (the walk terminated, the count is non-negative, independent holders
agree) and record the observed exact agreement as evidence in the commit
message instead of as a test.

The rule has a second, sneakier form that bit in 2026-07 **despite the
paragraph above already existing**: a loose window used during *exploration*
gets reported as though it were an exact identity. While mapping `LTObject`'s
radius, the probe accepted `r <= |dims| + 1.0` — a one-unit-wide window — and
the finding was written up, in the schema and a commit, as "`radius == |dims| +
0.1`, CONFIRMED on 3583/3583". The fixture assertion, which tested what the
prose actually claimed, failed immediately: only 2126 objects satisfied the
formula. The other 1457 sat at `radius == 0` with `dims == 0`, because the base
constructor zeroes both and `SetDims` had never run on them.

Two lessons, both cheap to apply:

- **The window you explore with is not the claim you may publish.** A probe may
  be loose to find the shape; the write-up must state the relation you actually
  tested, at the tolerance you actually used. If the probe said `+/- 1.0`, you
  may not write `+ 0.1`.
- **Write the assertion at the precision of the prose.** Had the check been
  written first, at the stated precision, the mistake could not have survived
  the first run. This is the strongest argument for the in-process self-check
  pattern: the endpoint recomputes the claim, so the fixture fails when the
  prose drifts from the data.

A tolerance that scales with magnitude is still legitimate where it reflects
float representability rather than fudge: world coordinates here reach five
figures, so `d <= 0.01 + 0.0001*|b|` is defensible and `d <= 1.0` is not. State
which of the two you are doing.

**A field can have more than one correct state, and the extra state is usually
"its writer never ran".** The fix for the radius was NOT a looser epsilon but a
partition: every object is either sized (`radius == |dims| + 0.1`) or pristine
(`dims == 0 && radius == 0`), and live it is 2126/1457 with none outside. Two
things must then be asserted, not one:

- the **partition** is total (`sized + pristine == sampled`), and
- **both** states are populated (`sized > 0`, `pristine > 0`).

The partition alone passes vacuously if everything collapses into one bucket —
which is exactly how a broken `dims` offset would present, since a wrong offset
reading zeroes would make every object look pristine.

**A cross-count identity is only valid over a COMPLETE walk — so assert the
coverage, don't assume it.** Comparing two independently-walked populations is
one of the strongest checks available (it needs no recorded baseline and no
tolerance), but it silently stops meaning anything the moment either walk is
sampled rather than exhaustive.

Worked failure, 2026-07: `children_reached == parented` over `LTObject`'s
attachment graph. It held perfectly over a full ReGenny walk, then failed on the
fixture's first run — `parented 1`, `children_reached 0`. Nothing was wrong with
the mapping. The endpoint sampled 512 objects per type, covering 2215 of 3583,
and the one parented object's PARENT fell outside the sample, so the child was
counted while nothing walked the list holding it.

The fix is not to drop the check or to loosen it into `>=`:

- Have the producer report **both** the sampled count and the total traversed
  (`objects` and `listed` here), so truncation is visible rather than inferred.
- **Assert the coverage** (`listed == objects`) as its own check, ahead of the
  identity. A future scene that outgrows the cap then fails loudly with a
  message naming the fix, instead of quietly comparing mismatched populations.
- Size the cap to COVER the workload, not to sample it, when an identity depends
  on it. Sampling caps and coverage caps are different things with the same
  spelling.

Generalisation: before asserting any relation between two aggregates, ask what
population each was computed over. If the answers can differ, the relation is
conditional and the condition belongs in the test.

**For a cross-linked structure, walk BOTH sides and compare the SETS — not the
counts.** Count equality is the weak form of this; set equality is the strong one,
and it is what turns a plausible layout into a proven one.

`LTObject` and `LTVisSector` are cross-linked through `LTSpatialEntry`: each
entry sits in the object's record list AND in a sector's list. Verifying it meant
enumerating entries from the object side (3666), then walking each sector's own
list independently, and checking the two collections were **set-equal per
sector**: 263/263 sectors matched, neither side holding an entry the other did
not. That is far stronger than "both sides count 3666", which several wrong
offsets could also produce.

Why it is worth the effort: a single wrong offset in ANY of the six fields
involved (`entry_list`, `record_next`, `hit_head`, `hit_prev`, `hit_next`,
`entry_count`) breaks set equality, and usually breaks it loudly. It is the
cheapest way to validate a whole subsystem's linkage at once.

**Prefer a GRAPH-REACHABLE anchor over hunting for a global.** The sector array
has no global pointer we have found — but it did not need one. Each entry's
`hit_head` addresses a sector's `entry_list` slot, and `entry_list` is the
sector's first field, so the slot address IS the sector. That made the sectors
validatable in-process from `g_pClientMgr` alone.

Look for this before spending a pass on symbol hunting: if a structure is
referenced from something you already reach, you can check it now, and the
reachability path doubles as evidence for the field you traversed. Reserve the
global hunt for roots that genuinely nothing points at.

**A structure that stores its OWN counts is the best verification target you will
find — check the walk against the engine's numbers, not against yours.** Most
in-process checks compare a traversal to a value a previous session recorded,
which decays the moment the scene changes. When the structure carries the count
itself, the comparison is self-contained and stays valid in any world.

`LTVisTree` stores both `node_count` and `sector_count`. So the vis-tree check
asserts `nodes_walked == node_count` and `sectors_reached == sector_count` with no
baseline of ours involved at all — live 111 and 263, and both would be equally
valid numbers in a different level.

It also localises failure well, because the fields fail differently:

- `split_axis` doubles as the leaf marker (`> 2` means leaf), so a wrong offset
  there either truncates the walk or runs it into garbage — `node_count` stops
  matching either way.
- `elements`/`element_count` wrong makes elements fall outside the sector array,
  which is a separate counter.
- A wrong `sectors` base makes every element look out-of-range at once.

When mapping a container, look for a stored count before writing any assertion.
If one exists, prefer it over every recorded constant; if one does not, say so in
the check's comment so the next reader knows why a baseline was unavoidable.

**Strongest of all: an assertion a record can be checked against ITSELF.** No
baseline, no sibling structure, nothing to drift — the record either is
internally consistent or the mapping is wrong.

`LTVisPortal` is the clean example. Each portal stores a plane (normal +
distance), a centre point, and four vertices. Every one of those points must
satisfy that portal's OWN plane equation, so the check is
`dot(normal, p) == distance` and nothing else is involved:

- `plane_normal` unit length — 344/344
- `center` on the plane — 344/344
- all four `vertices` on the plane — 344/344

A wrong offset anywhere in the 0x5C record breaks the arithmetic immediately;
there is no way to be "slightly off" and still satisfy a plane equation across
344 records. Contrast the weaker forms this file already describes: a stored
count checks a traversal, and a cross-linked walk checks two structures against
each other, but both still depend on a second thing being right.

When a record contains a geometric relation — a plane and points on it, a
normal and a distance, a transform and its inverse, an AABB and the centre it
was derived from — assert the relation. Prefer it over any count, and prefer it
strongly over anything you recorded from a previous run.

**When the target stores a size, compare it to your `sizeof` — and be honest
about the reach.** Each of FEAR 2's per-type object pools records the
`element_size` the engine asks for, which is the concrete class's size as the
*engine* understands it. Our generated headers declare a size for the class we
mapped onto that type, from offsets we derived. Comparing the two pits one
derivation against another with no recorded baseline anywhere:

- six bank-allocated types, `element_size == sizeof(our class)` — 6/6
- `OT_LIGHT` has no bank at all — absence as the assertion

What makes it worth writing is that it is free and total. What makes it worth
documenting is its LIMIT: it pins the class total, so a field past the end, a
wrong array length or a mis-sized member fails instantly — but it says nothing
about *which* class inside a hierarchy owns a field. A cached transform pair sat
attributed to the derived class for several passes while every size involved was
correct, because moving fields between a base and its child leaves both totals
untouched. State that limit next to the check, or the next reader will trust it
for something it cannot do.

**Never assert on PER-FRAME state you do not control.** The bone matrix palette is
the clean example: the engine fills a model's 48-byte-per-node palette during that
model's draw, so whether any given slot holds a matrix depends on whether the
renderer touched that model in the instant the test ran. Live, on an idle game, 169
of 215 models read entirely zero — including the player's own viewmodel.

So the fixture asserts the ACCESSOR's contract and only reports the contents:

- asserted: `bone_matrix()` answers for every model with a skeleton, and for every
  node index below `node_count` — that follows from the allocator reserving
  `48 * node_count` unconditionally, and it holds whether or not a frame ran
- reported: how many slots are populated, with no floor

Requiring a populated palette would have produced a test that passes while the
player is moving and fails on a pause menu. That is worse than no test, because the
failure looks like a mapping regression and sends the next reader hunting for a
moved offset that never moved.

The tell for this class of field: its value is written by a subsystem on its own
schedule (render, animation, network) rather than at construction or load. Map it,
expose it with a populated-ness predicate so callers can tell, and let the test
watch the shape rather than the contents.

**Two paths to a LIVE value agree within a tolerance, not exactly.** Cross-checking
a layout read against a behaviour call is the strongest evidence this project has --
a wrong offset and a wrong byte pattern cannot agree by luck. The client clock now
has both: `counter_elapsed_ms` reads the mapped field, `Engine::client_time()` calls
the engine's own accessor.

But the two reads happen microseconds apart, and on a running game the clock
advances between them. Asserting exact equality would produce a check that passes at
a pause menu and fails during play — the same trap as asserting per-frame contents,
arriving from the opposite direction. So the assertion allows a second of slack,
which still catches every failure that matters: a mislocated accessor returns zero
or nonsense, never a value 40ms off.

The distinction to hold onto: values read under ONE guard share an instant and can
be compared tightly (the ms/seconds pair is checked to within 2ms). Values read by
two separate paths do not, however close together they look in the source.

**A check that compares things needs to be shown that it COMPARED something.**
The node-hash check asserts that the same name always carries the same hash. It
recomputes nothing — no translation table, no module offset — it just compares the
engine's stored hashes against each other, which is what makes it portable. But
that construction has a failure mode with no symptom: if every name were unique,
every name would define its own hash on first sight, nothing would ever be
compared, and the check would report a perfect `660/660` while testing nothing at
all.

So the fixture also asserts `repeated_names > 0`. Live 86 of 125 distinct node
names recur across the 34 assets, so the consistency figure reflects real
comparisons rather than 660 definitions.

Generalise it: whenever a check's strength depends on the data having some
property — duplicates to compare, a non-empty list to walk, both branches of a
condition being taken — assert that property too, next to the check. Otherwise a
scene change can quietly turn a strong check into a tautology, and a green
`660/660` is exactly what that looks like from outside.

**When you expect a biconditional and the data gives an IMPLICATION, assert the
implication and report the other side.** Symmetry is an aesthetic preference, not
evidence, and forcing it is the same error as widening a tolerance: it replaces
what the system guarantees with what you hoped it would.

Worked example, 2026-07. `LTObject_IsRenderable` decides world-tree membership —
`LTObject_SetFlags` adds or removes the tree link exactly when that predicate
flips — so "renderable if and only if linked" looks like the natural test. Live:

- renderable AND linked: 1758
- renderable AND NOT linked: **0**
- linked AND NOT renderable: **384**

One direction is airtight, the other is wrong by 384. Both facts are real:
insertion happens the moment the predicate turns true, while removal is less
prompt (writes to `flags2`/`flags3` do not re-run it). So the fixture asserts
`renderable_not_linked == 0` and merely *reads* `linked_not_renderable`.

The diagnostic shape is worth recognising: **0 exceptions one way and many the
other means a mechanism with lazy cleanup, not a broken mapping.** A genuinely
wrong offset produces failures in both directions, or a count that tracks nothing.
When you see this asymmetry, the correct response is to narrow the claim to the
direction with zero exceptions and write down why the other side is allowed to
differ — the comment explaining the asymmetry is what stops a later pass
"tidying" the test into a biconditional and getting a red suite.

**A frozen fixture answers IPC perfectly — so "it responded" proves nothing
about liveness.** Threads created after a process is suspended keep running, so
an injected IPC server happily serves stale reads from a game whose own threads
are frozen.

The gate MUST be independent of the thing under test. Use
`/sdk/targets`'s `last_sample_time_ms` — an ENGINE-side millisecond field the
SDK reads, which only advances when the engine executes (measured: delta 0
while suspended, tracking wall clock 1:1 while running). Never gate on
`frame_ticks`: that is OUR hook's counter and is itself under test, so using it
as the gate would silently reclassify a broken frame hook as "not exercised"
and destroy rule 3's assertion.

That gives three distinguishable outcomes, which is the whole point:

| engine advancing | `frame_ticks` advancing | verdict |
|---|---|---|
| no  | —   | fixture frozen → report "not exercised", skip change-based asserts |
| yes | no  | **FAIL — the hook is broken** (this must stay a hard failure) |
| yes | yes | assert normally |

A frozen fixture is the only case that downgrades to not-exercised, and it must
say so loudly ("fixture not advancing — resume the game or investigate"), never
pass quietly. See MAPPING_WORKFLOW Phase 2 for the mapping claim this cost us.

### 4. Write the host-side test and run it against the game
Add checks in `test/fixture_test_runner.cpp`. Build (`build.bat`), run:

```
bin\fixture-test.exe --injector bin\injector.exe --dll bin\fear2vr.dll ^
    --fixture W:\SteamLibrary\steamapps\common\FEAR2\FEAR2.exe --port 8798
```

You MUST see the new test fail first when the claim is a fix. A test that is
green before the change it "tests" exists is non-evidence — investigate why.

### 5. Make it pass, then run the WHOLE suite
"My new test passes" is necessary, not sufficient — the full run is the
contract: `ctest --test-dir build -C RelWithDebInfo --output-on-failure`.

## Graceful uninject is itself a tested feature

The reload loop is the point of this project: never restart the game to
iterate. Therefore the teardown path is held to the same bar:

- `/unload` responds before latching the flag (client-visible handshake), then
  stops IPC, retires every hook, and proves quiescence (all other threads
  suspended; the x86 `CONTEXT.Eip` check in `Framework::shutdown`).
- If ANY disable() fails or quiescence cannot be proven: stay DORMANT. The
  module remains mapped with hooks retired; the injector `--reload` injects the
  next build under a fresh filename so DllMain always re-runs. Dormant is the
  failure-visibility state: LOUD in the log, never silent corruption.
- The hook registry (`Hooks::get()`) and `g_framework` are deliberately LEAKED
  (never destructed in-process): ~InlineHook would free trampoline memory a
  straggler thread might still execute, and the CRT would run the framework
  destructor at unmap time. Do not "fix" those leaks; they are structural.

## Exit codes: skip is honest

`fixture-test` exit 77 (ctest SKIP) means "the environment could not run the
claim" — game missing, spawn failed, injection rejected, IPC never came up.
Exit 1 means the claim FAILED. Never convert a fail into a skip to keep CI
green; a skip after `inject` succeeded and `/health` answered is a bug in the
runner.

## Build & run

```
build.bat                                        :: configure (-G "Visual Studio 18 2026" -A Win32) + build RelWithDebInfo
ctest --test-dir build -C RelWithDebInfo --output-on-failure
```

Individual:

```
bin\command-server-test.exe                      :: Tier-1, always
bin\fixture-test.exe --injector bin\injector.exe --dll bin\fear2vr.dll ^
    --fixture W:\SteamLibrary\steamapps\common\FEAR2\FEAR2.exe --port 8798
bin\injector.exe --status                        :: process/module/IPC snapshot
bin\injector.exe --reload                        :: hot-swap without restart
```

Manual poking while attached to a running game:

```
curl http://127.0.0.1:8798/health
curl http://127.0.0.1:8798/sdk/targets
curl "http://127.0.0.1:8798/engine-hook?name=hwnd"
curl http://127.0.0.1:8798/unload
```

## Where things live

| Path | Contents |
|---|---|
| `shared/sdk/` | LithTech/FEAR2 bindings, one class per engine concept (static lib `fear2-sdk`) |
| `src/Framework.*` | singleton `g_framework`: SDK, hooks, mods fan-out, IPC, quiescence teardown |
| `src/Mod.hpp` `src/Mods.*` | Mod base + registry singleton (REFramework convention) |
| `src/Hooks.*` | `Hooks::get()` SafetyHook registry + retire contract (leaked by invariant) |
| `src/ipc/CommandServer.*` | localhost HTTP diagnostics bridge |
| `src/AgentRuntime.*` | supervisor: init -> poll /unload -> retire -> quiesce -> unmap/dormant |
| `src/Main.cpp` | thin DllMain |
| `injector/` | inject/unload/reload/status CLI |
| `test/` | Tier-1 headless + Tier-2 game-fixture runners (all assertions) |

## Fix the shape, not just the symptom

Rank the options and pick the highest feasible one:

1. **Remove the failure mode** (make the bug impossible): e.g. the leaked Hooks
   registry removes "trampoline freed under a straggler" as a class.
2. **Bound / guard / special-case the symptom** — acceptable ONLY when (1) is
   genuinely infeasible; you MUST say why and mark the deed
   `// WORKAROUND: proper fix is <X>`. The tell that you took tier 2 without
   tier 1 being infeasible: the user has to ask twice.

## Common pitfalls (this project specifically)

- **32-bit everywhere.** `Eip` not `Rip` in thread contexts; `__thiscall` is
  real again (detour as `__fastcall(this, edx_dummy)`); signatures are 32-bit
  byte patterns from IDA — never a 64-bit pattern "by analogy".
- **MSVC C2712.** `__try` (KANANLIB_SEH_TRY) cannot share a function scope with
  lambdas or statics-in-initializers. Own-function-scope the SEH guard.
- **IDB truth vs runtime truth.** The wrapped FEAR2.exe is unpacked at runtime
  into exactly the FEAR2_dump.exe layout at preferred base 0x400000 — but SDK
  code still treats addresses as scans-only, because the day that assumption
  breaks silently we won't notice until a crash.
- **Five binaries.** A "missing" vtable may live in `Game\gameclient.dll`, not
  the exe; a "missing" export may be in `gamedatabase.dll`. Check the right IDB
  (the five open IDA instances map 1:1).
- **gameserver.dll is lazy.** Absent at main menu is not a bug.
- **The game keeps running between iterations.** Dormant leftovers show in
  `injector --status` and are not evidence of failure.
- **kananlib first (hard rule -- see AGENT.MD 6 for the API table).** Never
  reimplement module/scan/SEH/instruction-decode gadgets. If you are writing a
  memory loop, a `VirtualQuery` walk, a byte search, or an operand decoder,
  stop and look in `I:\Programming\projects\kananlib\include\utility` first.
  This has already been violated once (a hand-rolled image scan that
  `utility::scan_relative_references` replaced in one call); a review that lets
  it through again is a review defect. Corollary for tests: a "helper" in the
  fixture runner that duplicates a library primitive is the same violation.

## Two values sampled at different times cannot be held to a tight bound

The suite's strongest check compared the engine's placement of the held weapon against our own composition of
the hand socket -- two independent producers of one point. It asserted they agree to 0.05, and it failed
intermittently: 0.000, 0.159, 2.139 across consecutive runs of UNCHANGED code.

**A residual that varies while the code does not is an indictment of the measurement, not the code.** Both
values were read from the IPC thread with a frame boundary free to land between them, and an idle animation
moving the arm across it. The check was measuring when the reads happened.

The rule this yields:

- A comparison between two producers MUST be sampled inside one frame -- on the engine's own tick -- or it is
  not a comparison. `Mod::on_frame` exists for this.
- When a residual cannot be eliminated, publish the SCALE it must be judged against alongside it (here, the
  weapon's per-frame travel). A real fault is wrong by units regardless of motion; skew shrinks to zero at
  rest. An assertion that cannot tell those apart is not testing anything.
- Gate the tight bound on having observed the quiet condition, and WAIT for it rather than asserting on
  whatever accumulated since injection. A freshly injected DLL has seen 20 frames, not 2000.

In phase the disagreement is 0.0000 over hundreds of still frames. The composition was exact all along; only
the test was wrong, and it had been wrong in the direction that looks like rigour.

## A screenshot is an observation, and it can be stale

Three independent in-engine measurements said a head rotation had reached the renderer -- the camera object
turned, `camera == outer * inner` held, and the pass argument moved 90 deg -> 150 deg for a 60 deg yaw -- while the
screen appeared unchanged across every capture. The instinct is to believe the picture.

The control settled it: applying a 150-unit eye offset, which had VISIBLY moved the world earlier in the same
session, also changed nothing on screen while the pass counter kept climbing. The capture was stale; the
renderer was fine.

- Before trusting a visual observation to contradict instrumentation, re-run a change that is KNOWN to be
  visible. If that one is invisible too, the observation is broken, not the code.
- Prefer an observable the engine computes: a stationary world point projected through the scene camera moves
  when the view turns, and it can be asserted. A screenshot cannot.

## A failure value that means three things is unactionable

The world-tree check asserted on a `nullopt`, and the assertion text said so plainly: `null == faulted or no
exe range`. When a level made it fire, the message could not distinguish a mapping fault from an unmapped
image range from a non-terminating list. It sat unexplained because nothing about it pointed anywhere.

Splitting the causes apart was one bool and one index, and it named the culprit immediately: list 0,
object 18, deterministically, after 13 objects had already agreed with the engine's own root address.

- A diagnostic that collapses distinct failures into one sentinel is not a diagnostic. If the assertion text
  needs the word "or", the report is missing a field.
- Make a partial result survive. The walk discarded 2065 correct observations because the 18th object
  faulted; guarded per item, it reports what it found AND what it stepped over.
- Then assert both: the invariant, and that the run was complete enough for the invariant to mean anything.
  `object_faults == 0` is the second half of `every linked object reached the root`.

## Readiness is what the checks need, not what the loader reports

`resume_game.py` exited 0 on `ws_world_ready` while the engine clock was paused on the load screen's "press
to continue". A suite run against that state fails scattered frame-dependent checks -- head tracking, in-phase
samplers, the pass census -- and every one of them looks like a code regression.

A fixture is ready when TIME IS PASSING, not when a flag says the world exists. Any state predicate a harness
exports has to be the one the tests actually depend on, or it will manufacture false failures faster than it
saves setup time.

## Measure the noise before asserting against a moving target

The bone-override test asserted that releasing the override returned the hand to its exact world
position, and failed once in four runs. Nothing was wrong with the override. The arm is ANIMATED,
so a before/after comparison spanning a second of wall clock measures idle sway as much as it
measures the thing under test -- the third time this project has been caught by comparing two
values sampled at different moments.

The general remedy, now applied three times and worth stating as a rule: **when a check must
compare against something the game is free to move, measure how much it moves on its own, over
the same interval, and judge the comparison against that.**

```
[fixture] idle drift over the comparison window: hand 0.0000 muzzle 0.0000
```

In a settled world the drift is zero, so the bound stays exactly as tight as a hardcoded one. When
the player is moving it widens by precisely as much as the world moved, and no more. That is the
difference between a self-calibrating bound and a fudged tolerance: one is derived from a control
measurement taken in the same conditions, the other is a number chosen because it made the run go
green. The failure that matters is untouched either way -- an override that fails to release
leaves 30 units behind, and no amount of sway accounts for 30 units.

Corollary: print the control measurement. A drift line in the log is what lets the next reader
tell "the check is loose" from "the world was busy".

## A gated block that says nothing when it skips is a suite that quietly stops testing

Three consecutive runs reported 1605, 1598 and 1605 checks. Zero failures, zero skips, no
explanation. Seven checks had simply not run: a block guarded by `if (mechanism_available)` did
nothing at all when the mechanism was not, and the total stayed green.

This is the same defect as a tautological check, arriving from the other side -- there, the check
runs and proves nothing; here, it does not run and nobody is told. Both print a healthy number.

- Every `if (available)` around a group of checks needs an `else` that REPORTS. `check_gated`
  exists for this: it tallies the skip and prints the reason.
- **A stable check count across runs is itself evidence.** Varying totals with no skip lines mean
  something is silently conditional. Watch the number, not just the colour.

## Assert the deterministic form, never the race

`the record holds the centre that was asked for` had failed intermittently for three sessions. One
printf in the failure path ended it immediately: the record held `-0.12000`, because an asymmetric
frustum gives the two eyes opposite centres and the read lands on whichever finished last. The
assertion had silently encoded "the right eye always wins".

The first fix was worse than the bug: poll across frames until both signs are observed. That
failed every run -- one eye reliably finishes last, so the poll detected a race rather than
checking a property.

What works is DRIVING the condition instead of waiting for it: ask each eye for the centre in turn
and read it back.

```
[fixture] per-eye frustum centre: left -0.12000 right +0.12000
```

It is deterministic, and it tests strictly more than the original -- which could not have told two
eyes sharing a centre from two eyes with opposite ones, the very property that makes the frustum
asymmetric. When a check depends on which of several things happened most recently, stop sampling
and arrange for only one of them to be true.

## An idempotent request is not a stimulus

A check compared the first-person rig's facing with a correction off and then on, applying the same
45 degree head pose each time, and measured the correction as having done precisely nothing. The
correction was fine.

The engine writes that rotation ONLY when the view changes -- the same elision the watchpoint counts
had already shown (zero hits while the view is still). Setting a pose the game is already in changes
no view, so the writer never ran and there was nothing to correct. The test had asked the question in
a form that could not produce an answer.

Generalisation for any check driving an engine that skips redundant work: **the second half of an
A/B comparison must re-establish the initial condition, not repeat the request.** Release, set the
state under test, then apply the stimulus. If a mechanism is edge-triggered, a test that assumes it
is level-triggered will report a working feature as broken -- or, worse in the other direction, a
broken one as working because the previous state happened to persist.

Both numbers now print (`uncorrected 44.646, corrected 0.355`), which is what made the diagnosis
immediate rather than a hunt.

## If the mechanism is not symmetric, restore with a loop, not an assumption

A test that turns the player has to put them back. The first version sent an equal-and-opposite
delta and asserted the result matched -- and one run in three finished exactly one turn's worth off,
because the engine's look gain is not constant (the first delta after an injection turns about twice
as far as later identical ones).

Two things were wrong and only one of them was the assumption:

- **Measuring the wrong quantity.** It compared WEAPON POSITIONS, which the arm animation moves on
  its own; a run went red at 1.897 units while the turn itself was identical to the millimetre. The
  heading is what changed, so the heading is what to measure.
- **Assuming reversibility.** Equal-and-opposite is a property of the input, not of the engine's
  response to it.

The restore is now a closed loop: measure the heading, apply a correction, repeat until within
tolerance (1-3 iterations, under 0.3 degrees). That is strictly better than a symmetric-input
assumption because it also HAPPENS to be how the shipped feature must work -- a VR snap turn against
a non-linear input is the same loop -- so the test exercises the real mechanism instead of
simulating one.

And what the suite asserts shrank to what is established: the input is delivered, it turns the
player, its DIRECTION is consistent. The non-constant gain is printed, not asserted. Reporting an
unexplained observation is not weakening a test; asserting it would be inventing one.

## A control loop's tolerance is not the test's bound

`TurnController` converges when it OBSERVES its heading error inside 0.5 degrees. The fixture reads
the heading AFTERWARDS, and the smallest correction the controller can issue is one unit of look
input, worth about 0.144 degrees. So what the design can promise a later reader is
`tolerance + one quantum`, not `tolerance`.

Asserting a flat 0.5 passed three runs and was measuring luck: live residuals reach 0.478. The bound
is now written as `kTurnTolerance + kOneStep` with both named and both traceable to the mechanism --
the controller's own constant and its input quantisation.

This is the legitimate version of a widened bound, and the distinction is worth stating because it
looks identical from outside a diff. A bound DERIVED from the mechanism's own resolution is evidence;
a bound enlarged until the run went green is not. If you cannot name where each term came from, it is
the second kind.

## Gate a control loop on the thing being controllable

A closed loop that drives game state has a failure mode an open one does not: the target stops
responding while the input keeps being accepted. `TurnController` burned its entire 24-iteration
budget against a heading frozen to the decimal, because the player had died -- and a dead player
still accepts look input, it simply does nothing with it.

The loop could not tell "cannot move" from "has not moved yet", and neither could its counters. Any
loop of this shape needs a liveness gate on the subject, not just an iteration cap: the cap bounds
the damage, the gate identifies the cause. `player_stats()->alive()` is the one here.
