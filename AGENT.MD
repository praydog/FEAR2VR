# AGENT.MD — fear2vr project rules

Working rules for anyone (human or AI) touching this repo. They encode the
conventions established while bootstrapping the project; TESTING.MD owns the
testing contract and is referenced (not duplicated) here.

## What this is

fear2vr: a 32-bit injected-DLL mod framework for F.E.A.R. 2: Project Origin
(LithTech Jupiter EX, D3D9), heading toward a VR mod. Rapid iteration via
inject → test → uninject with the game never restarting.

## Hard rules

1. **32-bit only.** Build is `-A Win32`; cmake fails hard on 64-bit. `Eip` not
   `Rip` in thread contexts; x86 `__thiscall` detours are written as
   `__fastcall(this, edx_dummy)`.
2. **No test code in the shipped mod (fear2vr.dll).** Assertions live host-side
   in `test/fixture_test_runner.cpp`. The DLL may expose *diagnostics*
   (`/health`, `/sdk/targets`, `/engine-hook`) — data, never pass/fail
   judgement. Rules of evidence are in TESTING.MD and are load-bearing.
3. **Commit frequently.** Small, build-clean commits. Never commit a tree that
   doesn't build; run `ctest --test-dir build -C RelWithDebInfo --output-on-failure`
   before committing. Commit after each coherent unit (scaffold, one SDK class,
   one test tier), not in one end-of-day blob. Never `git push`.
4. **REFramework/UEVR structure.** Singletons and the `g_framework` global, mir-
   roring `I:\Programming\projects\re2-barebones` and `ue4poc`:
   - `Framework` lives behind `extern std::unique_ptr<Framework> g_framework`
     (set once by the supervisor; `release()`d before unmap — the CRT must not
     run its destructor during `DLL_PROCESS_DETACH`).
   - Managers are `Xxx::get()` singletons: `Hooks::get()`, `Mods::get()`.
     `Hooks` is deliberately *leaked* (never deleted): `~InlineHook` frees
     trampoline memory that straggler game threads can still be executing
     during unmap. Do not "fix" that leak.
   - Features are `Mod` subclasses registered into `Mods::get()`; the framework
     fans out `on_initialize` / `on_frame` / `on_shutdown`.
   - Naming: snake_case methods/functions, PascalCase types/files, `m_`
     members, `g_` globals, `s_` statics. Errors as
     `std::optional<std::string>` (empty = success).
5. **SDK = class-per-concept with function-local static resolution.**
   `shared/sdk/` has one class per engine concept (`sdk::CClientMgr`,
   `sdk::CClientShell`, `sdk::Engine`, `sdk::DatabaseMgr`, `sdk::Modules`).
   Each class *owns* its patterns and derivation logic in its own .cpp;
   resolution happens inside the accessor:
   ```cpp
   uintptr_t CClientMgr::update_fn() {
       static const uintptr_t s_fn = Modules::get().scan_exe(kUpdate, "CClientMgr::Update");
       return s_fn;
   }
   ```
   - A global engine object gets `SomeClass::get()` on its owning class.
   - **No dumping-ground files.** No `detail/Scan.*`, no "helpers" TU holding
     other classes' patterns. If logic is genuinely shared, it belongs to the
     class that owns the underlying resource (module scanning →
     `Modules::get().scan_exe`).
   - Always document the IDA evidence inline (IDB name, address, instruction
     context) in a comment next to each pattern/offset.
   - Never hardcode absolute VAs. Runtime addresses come from pattern scans.
   - **A function-local static latches its result FOREVER, including a
     failure.** That is correct for a FEAR2.exe pattern (the exe is always
     loaded in-process, so a miss is definitive and rescanning would only
     spam), and WRONG whenever the prerequisite can still arrive later. Two
     cases to watch:
       - `gameserver.dll` is documented lazy ("session start"). The first
         SDK surface that scans it and gets warmed at framework init will
         cache 0 permanently and be dead for the process lifetime.
       - Anything needing `Modules::initialize()` to have run. Today
         `Framework::initialize()` calls it before warming anchors, so the
         five existing resolvers are safe by ORDERING, not by construction.
     If a resolver can fail for a reason that may resolve itself, do not use a
     plain function-local static: separate RETRYABLE (prerequisite absent --
     no latch) from DEFINITIVE (owning module present, evidence absent -- latch
     and log once). `sdk::interfaces::Registry::initialize()` is the worked
     example; it also shows why the definitive case MUST latch, since without
     it every getter would re-scan the whole image from the game thread.
   - **State an API's thread affinity in its own comment.** A surface handing
     out live engine pointers is only sound on the thread that owns them, and
     diagnostics run on the IPC thread. `CClientMgr::for_each_object` was
     briefly called from `build_objects_json` -- i.e. from the one thread its
     own documented precondition excludes. Diagnostics take snapshots or ask
     the engine thread to do the work; they do not borrow live pointers.
5a. **ReGenny is ground truth for LAYOUT; the SDK class owns BEHAVIOR.** Once
   a type is live-mapped in `reversing/fear2.genny` (ReGenny, verified field-
   by-field against raw memory -- see `reversing/MAPPING_WORKFLOW.md` for the
   concrete step-by-step, phase 0-4), generate
   its C++ header (`sdk:generate("shared/sdk/regenny")` in a ReGenny Lua eval)
   and wire an `sdk::X` wrapper around it:
   ```cpp
   class DatabaseMgr {
   public:
       static DatabaseMgr* get();
       regenny::DatabaseMgr* regenny() const { return (regenny::DatabaseMgr*)this; }
       size_t entry_count() const;                 // "complex logic" lives HERE
       regenny::DatabaseMgrEntry* entry(size_t i) const;
       static std::string read_path(const regenny::DatabaseMgrSubRecord*);
   private:
       char m_data[sizeof(regenny::DatabaseMgr)];   // same size as the real object
   };
   ```
   - `sdk::X` is sized identically to `regenny::X` (`m_data[sizeof(regenny::X)]`)
     so a raw engine pointer can be `reinterpret_cast`'d directly onto it.
   - `sdk::X` exposes NO raw fields itself -- only `.regenny()` to reach the
     mapped struct, plus named methods for anything worth wrapping. Complex
     behavior (traversal, safe reads, future lookups) is a MEMBER FUNCTION
     here, never re-derived ad hoc at each call site (Framework.cpp's
     diagnostics, tests, future mods all call the SAME method).
   - The generated headers need a primitives shim BEFORE inclusion:
     `shared/sdk/regenny/Primitives.hpp` defines `strptr`/`wstrptr` (the
     .genny prelude's tagged pointer aliases have no C++ definition of their
     own in generated output) -- hand-written, permanent, never regenerated.
   - Diagnostics (`/sdk/database` etc.) call the SDK's own methods and
     serialize the result -- they do not reimplement traversal logic inline.
   - **Testing corollary -- NO ReadProcessMemory IN TESTS, EVER.** No
     exceptions, no carve-outs. The mod is truly injected and running
     in-process: the thing under test is whether the SDK -- compiled from
     the `.genny` schema -- produces what we expect when it runs there.
     A host-side RPM read proves nothing the SDK didn't already do, and it
     forces the test to hardcode offsets, which:
       1. duplicates the schema as MAGIC VALUES in the test (the schema is
          the single source of truth; a test that restates offsets silently
          rots the moment the schema changes), and
       2. only demonstrates that our hand-typed numbers agree with
          themselves -- not that the compiled SDK code is correct, or even
          safe to call.
     **THERE MUST BE NO MAGIC VALUES IN TESTS AT ALL.** If a test needs to
     assert a structural relationship (e.g. "this node is linked into that
     list"), the SDK gets a METHOD expressing that invariant through the
     generated schema (`&node->self_link == head.next` -- the compiler
     derives every offset), the diagnostics report its result, and the test
     asserts on that. See `sdk::CClientMgr::counter_node_registered()` for
     the canonical example.
     What the host legitimately verifies: the SHAPE of what the DLL reports
     (well-formed JSON, correct types, plausible ranges), INVARIANTS between
     independently-mapped values (two schema fields at different offsets
     that must correlate -- if either offset were wrong they wouldn't), that
     the process and IPC SURVIVED the in-process call (crash-proof), and
     OS-level ground truth it alone owns: module residency from the host's
     own Toolhelp32 snapshot. Toolhelp32 is OS *metadata* about what is
     mapped where -- it is NOT reading the target's memory, and is the one
     external oracle the DLL cannot fake.
6. **kananlib first -- this is a HARD rule, not a suggestion.** Before writing
   ANY binary-analysis helper, read
   `I:\Programming\projects\kananlib\include\utility/*.hpp` (Scan, Module, Seh,
   Address, Patch, PointerHook, VtableHook). Use `KANANLIB_SEH_TRY` /
   `KANANLIB_SEH_EXCEPT`, never raw `__try`.

   **Stop-and-check trigger:** if you are about to write a loop over memory, a
   `VirtualQuery` walk, a byte-comparison search, or anything that decodes an
   instruction, STOP -- kananlib almost certainly has it:

   | you want | use |
   |----------|-----|
   | find a byte pattern | `utility::scan(module, pattern)` (via `Modules::scan_exe`) |
   | find code referencing an address/function | `utility::scan_relative_reference{,s}`, `scan_displacement_reference{,s}` |
   | that, restricted to a specific opcode | `utility::scan_relative_reference_strict(mod, ptr, "E8")` |
   | search backwards from a point | `utility::scan_reverse(start, len, pattern)` |
   | find a pointer-sized value in memory | `utility::scan_ptr`, `scan_data_t` |
   | find a string | `utility::scan_string{,s}` |
   | module base/size/handle | `utility::get_module_size` etc., `sdk::Modules` |
   | resolve an instruction operand | `utility::resolve_displacement`, `scan_opcode`, `scan_disasm` |

   **Worked example of getting this wrong (2026-07, real).** To enumerate the
   engine's interface holders I first hand-wrote a `VirtualQuery` region walk
   that scanned the whole image for a vtable pointer and then validated
   candidates. That was ~60 lines reimplementing, badly, what
   `utility::scan_relative_references(module, CAPIHolder_ctor)` does in one
   call -- and the library version is *more* precise, because call sites carry
   the operands we actually wanted instead of requiring heuristic validation.
   It was replaced. If your helper is longer than the kananlib call it
   replaces, that is the smell.

   One caveat learned the same day: `scan_relative_references` (plural) splits
   the module across threads with 4-byte segment overlap and does NOT dedupe,
   so sort+unique the result or your counts vary with core count.
   MSVC C2712: `__try` cannot share a function scope with lambdas,
   static-in-initializer, or ANY non-trivial (non-POD) local variable or
   return type -- including `std::string` constructed anywhere in that
   function, even after the `__try/__except` block ends. Isolate SEH guards
   in their own free function with POD-only locals and a POD return (e.g.
   raw pointer, `int32_t` status + out-buffer); build the real return value
   in a separate, non-`__try` wrapper that calls it.
7. **safetyhook via the registry only.** All hooks go through `Hooks::get()`
   (`install` / `retire_one` / `retire`). The pinned safetyhook returns
   `InlineHook` truthy/falsey from `create_inline`; `enable/disable()` return
   an expected with `.error().type`. Detours call originals through
   `Hooks::get().find(name)->original<...>()`.
8. **Graceful uninject contract** (see TESTING.MD): `/unload` → stop IPC →
   retire hooks → quiescence proof (all threads suspended, no `Eip` inside our
   image) → `FreeLibraryAndExitThread`, else stay dormant and let the injector
   bring the next build under a fresh filename.
9. **IDA workflow.** For the full "map a new concept end-to-end" recipe
   (IDA → ReGenny live-verify → `.genny` → codegen → SDK class →
   diagnostics → tests), see `reversing/MAPPING_WORKFLOW.md` -- this rule
   covers only the IDA-MCP-specific gotchas. Five IDBs: FEAR2_dump.exe
   (unpacked; the on-disk FEAR2.exe
   is CEG/SteamStub-wrapped and *useless* for static analysis — its `.text` is
   ciphertext), gameclient.dll, gameserver.dll, gamedatabase.dll, ltmemory.dll.
   - IDBs live in `I:\Programming\projects\fear2recon\*.i64` (moved off the
     network/W: volume on 2026-07 after an instance crashed there --
     `skill://ida-friction` flags network-share IDBs as a wedge risk, and it
     happened). The `input_path` in `server_health` still points at the
     original binary under `W:\SteamLibrary\...`; that is expected.
   - After `select_instance`, ALWAYS verify with `server_health` by IDB
     filename before trusting any result (MCP instance switching can silently
     misroute).
   - **MCP ports are NOT stable across IDA restarts.** After the relocation the
     assignments shuffled: FEAR2_dump.exe moved 13339 -> 13338 and
     gameclient.dll took 13339, which is exactly the setup for renaming a
     hundred symbols into the wrong database. Always `list_instances` first and
     pick the port by BINARY NAME, never from memory of a previous session.
   - Rename functions/globals in IDA as you map them (MCP `rename` batch; see
     `skill://ida-friction` for the persistent-rename gotchas). Verify by
     re-decompiling, and run `idb_save` when a mapping batch lands.
   - The lithtech/FEAR sources in `I:\Programming\projects\fear-source-code`
     are *structural analogues* for naming (Build 69 ≈ FEAR1; FEAR2 is "Loki"),
     not ground truth for FEAR2's exact build.

     **Treat the drop as a FALSE FRIEND, not a reference.** It is close enough
     to feel authoritative and wrong on the specifics that matter. Measured
     divergences so far, every one of which would have produced a silently
     wrong mapping if trusted:
       - `LTList` is 16 bytes there (count + 12-byte `LTLink`); FEAR2's list
         heads are 8 bytes at stride 8, no count.
       - `LTLink` carries `m_pData` and the drop's walkers use it; FEAR2's
         lists are INTRUSIVE and walkers recover the object by `link - 172`.
       - `CAPIHolderBase` has `int32 version` at `+0x08`; FEAR2 has the output
         pointer slot there, read as such by all three virtuals.
       - `CClientMgr` nests its object lists inside an `ObjectMgr` member;
         FEAR2 puts the 7 heads at offset 0 with nothing ahead of them.
     Use it for *what a thing is probably called*. Never for a layout, a size,
     a field order, or a member's existence. When it disagrees with the binary,
     the binary is right and the divergence goes in the `.genny` comment.
10. **Build system is cmkr.** Edit `cmake.toml`, never `CMakeLists.txt`
    (regenerated). cmkr skips regeneration when `CI` is set in the environment;
    `build.bat` clears it — beware when invoking cmake by hand. cmkr expands
    `**.cpp` globs at configure time: after adding/removing source files you
    must re-run configure (`env -u CI cmake -B build -G "Visual Studio 18 2026" -A Win32`).
    **The failure mode is a GREEN build that never compiled your file**, which is
    why this bites: `cmake --build` on a stale cache reports no errors because the
    new `.cpp` is not in the project at all. It bit in 2026-07 with
    `shared/sdk/VisTree.cpp` — two clean builds in a row, zero diagnostics, zero
    object files. Worse, a hand-run `cmake -S . -B build` did NOT fix it, because
    `CI` was set in that shell and cmkr skipped regeneration silently while CMake
    still printed "Generating done".
    Verify, do not assume: `grep -c "<NewFile>" CMakeLists.txt` (or the
    `.vcxproj`) must be non-zero after configuring. Simplest reliable route is
    `build.bat`, which clears `CI` itself. Treat "it built and my new code did
    nothing" as this bug before debugging the code.
10a. **Generator is pinned to Visual Studio, NOT Ninja.** `.vscode/settings.json`
    sets `cmake.generator: "Visual Studio 18 2026"` + `cmake.platform: "Win32"`
    so VSCode's CMake Tools and manual `cmake -B build -G "Visual Studio 18
    2026" -A Win32` produce the SAME `build/` cache. Do not let CMake Tools
    auto-pick a Ninja kit: Ninja invokes `link.exe` directly with no VS dev
    environment, so linking fails on system libs (`ws2_32.lib` etc.) from any
    shell that hasn't run `vcvarsall.bat`. The VS generator's MSBuild handles
    that environment itself — no vcvarsall needed anywhere. If `build/` was
    ever configured with a different generator, CMake refuses to reconfigure
    in place; delete it (`cmd /c rmdir /s /q build` — plain `rm -rf` can choke
    on fetched-dependency junctions) and reconfigure fresh.
11. **Don't cross-test the tiers.** Tier-1 (`command-server-test`) is headless
    and ALWAYS must pass (no skip allowed). Tier-2 (`fixture-test`) may skip
    honestly with exit 77 only when the game/IPC genuinely can't come up.
12. **Log everything decision-relevant in-DLL.** LOGX to fear2vr.log: pattern
    hits/misses, hook install/retire, lifecycle transitions. The log is the
    first stop when a fixture check goes red.
13. **Map for CONSUMERS, not just for the test suite.** Every mapping pass must
    ask what a mod would call, and leave that behind. This rule exists because
    the SDK drifted the wrong way for several passes: it accumulated twelve
    `check_*` diagnostics against barely a handful of usable methods, so the
    skeleton work could report "660/660 node names printable" while offering no
    way to answer "where is the head bone?". Both are needed and they are not the
    same artefact:
    - `check_*` methods prove the MAPPING still holds. They return counters, get
      called by the fixture, and are diagnostics. Keep them.
    - Consumer API (`sdk::ModelSkeleton`, `sdk::model_filename`, ...) answers a
      question in the caller's terms: names not offsets, `std::string` not
      `char*` into engine memory, `std::optional` not a sentinel. It is what the
      mapping was FOR.
    Two design rules for that surface, both learned the hard way here:
    - **Never hand out a pointer into engine memory.** Node names live in an
      allocation owned by a shared asset that is freed when its last model goes
      away, so a returned `const char*` is a use-after-free waiting for a level
      change. Copy out.
    - **Don't encode an unresolved question in the API.** `LTModelNode` stores two
      pose pairs and which is which is not established, so they are exposed as
      `pose_a`/`pose_b` rather than a guessed `local`/`bind`. A wrong name in an
      interface propagates into every caller and outlives the guess.
    Test the API at its own level too: assert that `find_node(n)` then
    `node_name(i)` returns `n`, not that some offset is 660/660. The contract can
    break while every offset underneath stays correct.

## Layout

```
cmake.toml / build.bat        -- cmkr build (Win32 enforced)
shared/                       -- Log, HttpClient, sdk/ (fear2-sdk static lib)
shared/sdk/                   -- Modules, CClientMgr, CClientShell, Engine, DatabaseMgr,
                                 VisTree, Model (consumer-facing skeleton/material API)
shared/sdk/regenny/           -- generated (sdk:generate) layout structs + Primitives.hpp shim
reversing/                    -- fear2.genny (ground truth), MAPPING_WORKFLOW.md
                                 (the mapping recipe), DatabaseMgrDump.lua, dump artifacts
src/                          -- Main.cpp (DllMain), AgentRuntime, Framework,
                                 Hooks, Mod/Mods, ipc/CommandServer
injector/                     -- LoadLibrary injector (inject/unload/reload/status)
test/                         -- command_server_tests (Tier-1), fixture_test_runner (Tier-2)
TESTING.MD                    -- the testing contract (rigor, evidence rules)
```

## Iteration loop

```
build.bat                                   # configure + build (Win32)
bin\injector.exe --reload                   # hot-swap into the running game
bin\injector.exe --status                   # module/IPC snapshot
ctest --test-dir build -C RelWithDebInfo --output-on-failure
```

**Gate the test run on the compiler's verdict, not on your own echo.** Filtering
build output through `grep` and then printing a cheerful marker discards the exit
code, so a compile error becomes a *green-looking* build followed by a fixture run
against the PREVIOUS DLL. That has happened twice here; the second time it produced
two red palette checks that looked like a mapping regression and were nothing of
the kind. Chain on the build's own status instead:

```
cmd /c build.bat > b.log 2>&1 && <run the thing> || grep -E "error C|error LNK|fatal" b.log
```

The `&&` only fires when MSBuild returned 0, and the `||` branch shows why it did
not. Same reasoning as rule 10's silent-regeneration trap: a build that looks fine
while producing nothing new is the most expensive failure mode in this project,
because every downstream signal stays plausible.
