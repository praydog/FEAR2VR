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
assertions over diagnostics endpoints + ReadProcessMemory -> /unload ->
module-vanish + game-alive proof -> re-inject freshness proof`.
All assertions live in the runner; the DLL ships **no test code**.

## The fixture is ground truth — and tests are NOT in the mod

`FEAR2.exe` (32-bit LithTech Jupiter EX, D3D9) is the oracle of truth for every
mapping claim. Concretely:

- An address/offset/scan-pattern in `shared/sdk` is only *verified* when the
  fixture runner reproduces the claim from live process state: the diagnostics
  endpoint reports a value AND `ReadProcessMemory` on the PID reads the same
  bytes at the reported slot AND the value satisfies the structural invariant
  (inside the owning module's `[base, base+size)` per the host's own Toolhelp32
  module list — never per the DLL's self-report).
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
2. **Cross-check the DLL's self-report.** Any value the DLL exposes over IPC
   that can be read independently (a global slot) MUST also be read via
   `ReadProcessMemory` and compared — otherwise the DLL could be hallucinating
   its own diagnostics and no test would notice.
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
Rename the function/globals in IDA via MCP.

### 2. Write the SDK surface
Class-based, REFramework convention: resolution lives in the accessor itself
(function-local statics), ownership lives in the class that owns the concept
(`sdk::CClientMgr`, `sdk::CClientShell`, `sdk::Engine`, `sdk::DatabaseMgr`,
`sdk::Modules`). Patterns are kananlib-scanned by `Modules::get().scan_exe`
inside the owning class's .cpp — never a shared "all the scans" file, never a
hardcoded absolute VA. Document the IDA evidence inline.

### 3. Extend the diagnostics (if the new surface needs visibility)
Minimal JSON field or endpoint on `/health`, `/sdk/targets` — diagnostics
format, no assertions.

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
- **kananlib first.** Don't reimplement module/scan/SEH gadgets — check
  `I:\Programming\projects\kananlib\include\utility` before writing a helper.
