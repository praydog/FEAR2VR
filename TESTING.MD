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
