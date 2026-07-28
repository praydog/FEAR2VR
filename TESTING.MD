# fear2vr Testing & Verification Guide

Testing standard for the FEAR 2: Project Origin mod framework. Modeled on the
kananlifter / il2cpp-scripting testing bar. This file is the contract: when a
test, a claim, or a fix conflicts with it, this file wins.

## Core Principle

**Demonstrate before you fix.**

Never claim a mapping, offset, hook target, or engine semantic is wrong,
missing, or "unsupported" without a test that *fails at runtime against the
current code* — in this project "at runtime" means *inside the live game
process*, on the real `FEAR2.exe`, through `/test`.

The same bar applies to a **POSITIVE** claim. "The pointer resolves", "the
hook fires per frame", "unload is graceful" all require a test that *passes*
against the current code for the exact reason stated — not because the check
was vacuous (`if (ptr != nullptr && ptr != 0xBAD)` is not a resolution proof;
see "The rules that make a fixture test VALID").

When a test goes red, **fix the CODE — never weaken the test.** Prohibited:

- Loosening an assertion (`==` to `!= 0`, exact address to "somewhere in .text").
- Narrowing the input so the failing path is never exercised.
- Swapping the in-game observation for a stub/assumed value.
- Adding a special case so the test input bypasses the mechanism under test
  (e.g. marking a hook test "ok" because IPC answered — that tests the socket,
  not the hook).
- Deleting or `#if 0`-ing a red test.
- Asserting against LOG output instead of observable in-process state.

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
fragment plumbing, `/test` envelope, 404s, `/unload` latching, stop() semantics.
If Tier-1 fails, the fix is in `src/ipc/CommandServer.cpp`, full stop.

Tier-2 (`test/fixture_test_runner.cpp`) is the real loop:
`spawn-or-attach FEAR2.exe -> inject fear2vr.dll -> /health live -> GET /test
-> assert fail==0 -> teardown (/unload, or terminate the instance we spawned)`.
The DLL's registered tests (see `src/GameTests.cpp`, registered from
`Framework::initialize`) are the assertions; the runner only transports them.

## The fixture is ground truth

`FEAR2.exe` (32-bit LithTech Jupiter EX, D3D9) is the oracle of truth for every
mapping claim. Concretely:

- An address/offset/scan-pattern in `shared/sdk` is only *verified* when an
  in-DLL test reproduces the claim from live process state (module base +
  pattern scan + dereference), AND the observed value satisfies the structural
  invariant (pointer lands inside the expected module's image, vtable entries
  land in the owning module's `.text` range, etc.).
- IDA static evidence (decompile, xrefs) justifies *writing* the mapping; the
  fixture test justifies *keeping* it. Both are required. A rename in IDA with
  no fixture test is a hypothesis, not a mapping.
- `gameserver.dll` absent at main menu is why it's `required=false` in
  `shared/sdk/Modules.cpp` — that's a fixture-derived fact, not an assumption.

Conversely, a fixture red for one REASON (SDK regression, broken pattern, game
update shifting a target) is one red test — never "flaky Scheduled test". If it
is red, it is telling you something. Reproduce it standalone
(`curl http://127.0.0.1:8798/test`) before believing anything else.

## The rules that make a fixture test VALID

1. **Prove residence, not just non-null.** A pointer test must assert the value
   is inside the *owning* module's `[base, base+size)` (or for vtables: every
   sampled entry inside the owner's executable range as established in the
   test's comment).
2. **Every mutation is restored AND the restore is asserted.** Hook a function
   in a test? Retire it before the test returns and assert the retire
   succeeded. Read-only tests are preferred whenever the claim permits.
3. **Rejection test asserts the PRESERVED shape is distinct from the poison.**
   If a test writes a sentinel then restores, the sentinel must be impossible
   as a legitimate value (e.g. `0xDEADBEEF` as an engine pointer), and the
   post-restore read must equal the pre-test read, captured FIRST.
4. **Prove the path is live before testing the payload.** A hook round-trip
   test must FIRST observe the hook fire in the game (counter increments over
   a bounded wait), THEN test retire (counter stops changing). A counter that
   never moved proves nothing about removal — it may never have been attached.
5. **Tests run on the IPC server thread.** They must be allocation-light and
   must never block on the game's own threads' cooperation beyond bounded
   waits. No test may sleep more than 5 seconds total.

Violating any of these makes the test non-evidence. Fix the test, not the bar.

## Step-by-step: add a demonstrate-then-fix change

### 1. Map in IDA
Locate the target in FEAR2.exe / gameclient.dll via strings/xrefs, confirm with
the lithtech / FEAR sources in `I:\Programming\projects\fear-source-code`
(structural analogue — FEAR 2's exact build differs). Rename the function /
globals in IDA (MCP rename; address-anchored methods per the IDA skill notes).

### 2. Write the SDK surface
Add the binding under `shared/sdk/` (signature scans via kananlib
`utility::scan`, resolved lazily inside the process — never hardcoded absolute
VA). Document the RVAs as comments with IDA evidence.

### 3. Register the in-DLL test and run it against the game
Add via `gametests::add(...)` in `Framework::initialize`. Build
(`build.bat`), run `fixture-test` through ctest or directly:

```
cmake --build build --config Release
bin\fixture-test.exe --injector bin\injector.exe --dll bin\fear2vr.dll ^
    --fixture W:\SteamLibrary\steamapps\common\FEAR2\FEAR2.exe --port 8798
```

You MUST see the new test fail first when the claim is a fix. A test that is
green before the change it "tests" exists is non-evidence — investigate why.

### 4. Make it pass, then run the WHOLE suite
"My new test passes" is necessary, not sufficient — the full run is the
contract: `ctest --test-dir build -C Release --output-on-failure`.

## Graceful uninject is itself a tested feature

The reload loop is the point of this project: never restart the game to
iterate. Therefore the teardown path is held to the same bar:

- `/unload` must return `{"ok":true}` before latching the flag (client-visible
  handshake), then stop IPC, retire every hook (original bytes restored), and
  prove quiescence (all other threads suspended; no EIP inside our module
  image — the x86 `CONTEXT.Eip` check in `Framework::shutdown`).
- If ANY disable() fails or quiescence cannot be proven: stay DORMANT. The
  module remains mapped with hooks retired; the injector `--reload` injects the
  next build under a fresh filename so DllMain always re-runs. Dormant is the
  failure-visibility state: the failure is LOUD in the log, never a silent
  corruption.
- Hook InlineHook objects are NEVER destroyed in the game process (see
  `src/Hooks.hpp`). The registry is deliberately leaked so ~InlineHook can
  never free trampoline memory a straggler thread might still execute. Do not
  "fix" that leak; it is structural.

## Exit codes: skip is honest

`fixture-test` exit 77 (ctest SKIP) means "the environment could not run the
claim" — game missing, spawn failed, injection rejected, IPC never came up.
Exit 1 means the claim FAILED. Never convert a fail into a skip to keep CI
green; a skip after `inject` succeeded and `/health` answered is a bug in the
runner.

## Build & run

```
build.bat                                        :: configure (-A Win32) + build Release
ctest --test-dir build -C Release --output-on-failure
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
curl http://127.0.0.1:8798/test
curl http://127.0.0.1:8798/unload
```

## Where things live

| Path | Contents |
|---|---|
| `shared/sdk/` | LithTech/FEAR2 bindings mapped from IDA (static lib `fear2-sdk`) |
| `src/Framework.*` | Top object: SDK, hooks, IPC, test registry, quiescence teardown |
| `src/Hooks.*` | SafetyHook registry + retire contract |
| `src/ipc/CommandServer.*` | localhost HTTP bridge (/health /test /unload) |
| `src/AgentRuntime.*` | supervisor: init -> poll /unload -> retire -> quiesce -> unmap/dormant |
| `src/Main.cpp` | thin DllMain |
| `injector/` | inject/unload/reload/status CLI |
| `test/` | ctest drivers (this file's two tiers) |

## Fix the shape, not just the symptom

Rank the options and pick the highest feasible one:

1. **Remove the failure mode** (make the bug impossible): e.g. the leaked Hooks
   registry removes "trampoline freed under a straggler" as a class.
2. **Bound / guard / special-case the symptom** — acceptable ONLY when (1) is
   genuinely infeasible; you MUST say why and mark the deed
   `// WORKAROUND: proper fix is <X>`. The tell that you took tier 2 without
   tier 1 being infeasible: the user has to ask twice.

## Common pitfalls (this project specifically)

- **32-bit everywhere.** `Eip` not `Rip` in thread contexts; `__thiscall`
  real-again (x86 has none of the x64 uniformity); signatures are 32-bit byte
  patterns from IDA — never reuse a 64-bit pattern "by analogy".
- **IDB truth vs runtime truth.** IDA imagebase is `0x400000` for FEAR2.exe;
  at runtime ASLR moves modules — all SDK addresses are scans/RVAs, period.
- **Five binaries.** A "missing" vtable may live in `Game\gameclient.dll`, not
  the exe; a "missing" export may be in `gamedatabase.dll`. Check the right
  IDB (the four open IDA instances map 1:1).
- **gameserver.dll is lazy.** Absent at main menu is not a bug.
- **The game keeps running between iterations.** State the previous injection
  left behind (leaked hooks — intentional) is inert; dormant leftovers show in
  `injector --status` and are not evidence of failure.
