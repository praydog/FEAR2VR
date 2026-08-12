# AGENTS.md — fear2vr project rules

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
   `__fastcall(this, edx_dummy)`. The one 64-bit thing in the repo is
   `tools/xr64`, the OpenXR host, and it is deliberately NOT in this build --
   it configures on its own (`cmake -B build64 -A x64 -S tools/xr64`). Nothing
   in the mod calls OpenXR; the two processes meet only at the shared mapping
   in `shared/xr/SharedFrame.hpp`.
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
5. **Unknown reader or writer? Use `/watch/*`, never an offset scan.** The mod
   exposes HARDWARE data breakpoints — DR0-DR3 plus a vectored exception handler,
   four slots, byte-exact:

   ```
   /watch/arm?addr=0x02938740&size=4&type=write|rw|exec&max_hits=4000
   /watch/report      -> accessors, registers, value at trap, caller candidates
   /watch/clear?all=1
   ```

   Every hit resolves to `module + offset + static`, so it pastes straight into
   the right IDB. `ecx` is reported separately because `__thiscall` puts `this`
   there — which is how you tell *"writes +144 on something"* from *"writes +144
   on THE camera"*.

   **This is mandatory for "what touches X".** Scanning the binary for stores to
   a struct OFFSET answers a different question than the one you asked (67
   functions across unrelated classes, in the case that established this rule)
   and has produced a plausible WRONG answer every single time it was tried
   here. Every instance is recorded in `reversing/REVERSING_LESSONS.md`.

   **The workflow is: trap it, then read it.** Arm the watch, take the reported
   static address and caller candidates into IDA, decompile the writer, walk the
   stack upward until you find the function that *decides* — then hook THAT.
   Worked example, end to end: the engine clock's address was published, a write
   watch named the store (`add [esi+30h], eax` in `CLTTimer_TickNode`), the
   stack gave `CClientMgr__Update -> CLTTimer_AdvanceByWallClock ->
   CLTTimer_TickChildren`, and the decompiled tick gave the whole timer-node
   layout including the pause byte and rational time scale that turned out to be
   what alt-tab actually freezes. Two hardware semantics matter and are asserted
   in the suite rather than remembered: a data watch is a TRAP, so the reported
   address is the instruction *after* the accessor (`eip_after`), while an
   execute watch is a FAULT and reports the instruction itself; and x86 has no
   read-only data breakpoint, so a read request is served by read-or-write.

6. **SDK = class-per-concept with function-local static resolution.**
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
   - **ANYTHING YOU REVERSE GETS NAMED IN THE IDB, IN THE SAME SESSION. This is
     unconditional and does not need asking for.** It applies when reversing is
     incidental to some other job -- fixing a bug, chasing a crash, adding a
     feature -- exactly as much as when "map X" was the task. Most reversing here
     happens on the way to something else, so a rule that only covers deliberate
     mapping passes covers the minority of it.

     The test is not "was this a reversing task" but **"did I learn what an
     address is?"** If a `sub_XXXXXXXX` became meaningful to you, it gets a name
     before you move on; if it is still a guess, it does not get a name at all.
     Leaving it as `sub_` throws the finding away: the next session re-derives it,
     and this project has re-derived the same structure three times.

     Name FUNCTIONS, GLOBALS and VTABLES; comment the EVIDENCE (slot number,
     offsets, how it was found) on anything load-bearing, because a name says
     what and only the comment says how you know. MCP `rename` batch; see
     `skill://ida-friction` for the persistent-rename gotchas. Verify by
     re-decompiling, and `idb_save` when a batch lands.

     **Do NOT rename a symbol that already has a good name.** Existing names are
     cited from `reversing/*.md` and `fear2.genny`, and improving one to taste
     silently orphans those references -- `g_vtbl_GFxMovie` was renamed for
     precision and reverted for exactly this reason. Extra nuance belongs in a
     comment, which costs nobody a broken citation.
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
9a. **ReGenny codegen WORKS -- the earlier "BROKEN" rule was over-general and is retracted.**
    Re-tested 2026-07 with a debugger attached: `regenny:sdk()` returned a live
    `sol.sdkgenny::Sdk*` and `s:generate(dir)` produced 62 headers, no crash.
    - The crash is real but CONDITIONAL: the binding does not null-check `m_sdk` (the UI's
      `action_generate_sdk` does), so it faults only when there is no SDK yet -- a freshly
      launched instance on which no schema has ever parsed. That is what earlier sessions hit,
      because they called it before opening the file.
    - A FAILED PARSE DOES NOT null it: induced one deliberately and `sdk_loaded` stayed true,
      because ReGenny retains the last good SDK. So "open the schema, confirm it parsed, then
      generate" is safe.
    - Use `regenny:sdk()`. There is no `sdk` global -- it is nil, which is why the checklist's
      `sdk:generate(...)` fails with "attempt to index a nil value".
    - GOTCHA: `open_file` silently DETACHES from the target process. Re-attach afterwards.
    `shared/sdk/regenny/` is nonetheless still hand-frozen for now, and unfreezing is a decision
    with a measured cost rather than a formality -- the schema has drifted ahead of the headers by
    5 files of pure field RENAMES (`unk_14`->`name_hash`, `region_count_a`->`physics_node_count`,
    `unk_04`->`num_attributes`, ...) touching ~17 callsites, plus 7 types that have never been
    generated at all (`CPlayerCamera`, `CLTInput`, `GameTimer`, `LTInputDeviceMouse`,
    `LTInputDeviceKeyboard`, `LTNodeControlCell`, `PlayerZoom`). Regenerate deliberately, migrate
    the callsites in the same commit, and run the suite. Full record in
    `reversing/REVERSING_LESSONS.md` (ReGenny and codegen).
9b. **The SCHEMA owns layout. Codegen is unfrozen -- regenerate rather than hand-declare.**
    `shared/sdk/regenny/` is generated again (see 9a). The rule that follows from it: a structure
    or class layout belongs in `reversing/fear2.genny` and reaches the SDK through generated
    headers. Do NOT restate a field order, and do not add a `static constexpr uintptr_t kThing =
    N` for something the schema could name.
    - Recipe: open the schema, confirm `parse_status == ok`, `regenny:sdk():generate(...)`, then
      re-attach (generation's `open_file` sibling detaches).
    - Migrate renamed fields in the SAME commit as the regeneration. The five-file drift found
      when this was unfrozen (`unk_14` -> `name_hash` and friends, ~17 callsites) accumulated
      purely because the old rule said "don't regenerate" instead of "regenerate and migrate".
    - `Primitives.hpp` stays hand-written; codegen does not emit it.
    - DEBT, measured at the unfreeze: 85 hardcoded layout offsets remain in `shared/sdk/*.hpp`,
      59 of them in `PlayerMgr.hpp` (CPlayerStats' fields, the camera holder's pose offsets, the
      ammo array). Each is a candidate to become a schema class. Convert opportunistically when
      touching one -- do not add more.
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
    Two more, added after `check_transforms` had to be taken apart:
    - **A `check_*` aggregates a consumer primitive; it never CONTAINS one.** That
      check built `R(q)`, read both cached matrices, compared them against each
      other and computed a determinant, all inside one SEH walk -- then returned
      three counters. Every ingredient a mod needed was in there and unreachable.
      If a check computes something a caller would want, the computation belongs
      in the header and the check calls it. Where the SEH guard lives is an
      implementation detail of the primitive, not a reason to bury logic.
    - **Delete the private path when you extract it.** The old walker was removed,
      not left beside the new one: two copies of an offset or a formula is how one
      of them goes stale. Refactoring measurement code is verifiable in a way most
      refactors are not -- the counts must not move. They did not (1473/1464/1450/
      1473 and 474/474/474/474, matching what the schema already recorded).

## Layout

```
cmake.toml / build.bat        -- cmkr build (Win32 enforced)
shared/                       -- Log, HttpClient, sdk/ (fear2-sdk static lib)
shared/sdk/                   -- Modules, CClientMgr, CClientShell, Engine, DatabaseMgr,
                                 VisTree, Model (consumer-facing skeleton/material API)
shared/sdk/regenny/           -- generated (sdk:generate) layout structs + Primitives.hpp shim
shared/xr/                    -- SharedFrame.hpp, the ONLY contract between the mod and the
                                 64-bit OpenXR host (frame/UI slots, haptic ring, host state)
reversing/                    -- fear2.genny (ground truth for LAYOUT),
                                 MAPPING_WORKFLOW.MD (the recipe -- start here),
                                 ENGINE_NOTES.md (what is known about FEAR2),
                                 REVERSING_LESSONS.md (technique + how it goes wrong),
                                 INTERFACE_HOLDERS.md, DatabaseMgrDump.lua, dump artifacts
src/                          -- Main.cpp (DllMain), AgentRuntime, Framework,
                                 Hooks, Mod/Mods, ipc/CommandServer
src/mods/                     -- ViewHook (the view override), Watchpoints (hardware data
                                 breakpoints, /watch/*), FocusKeeper (keeps the world
                                 running while the window is not focused)
injector/                     -- LoadLibrary injector (inject/unload/reload/status)
test/                         -- command_server_tests (Tier-1), fixture_test_runner (Tier-2)
tools/                        -- resume_game.py (fixture bring-up), check_formats.py (Tier-0)
tools/xr64/                   -- the 64-bit OpenXR host: separate x64 build, owns the instance,
                                 session and swapchains (see hard rule 1)
TESTING.MD                    -- the testing contract (rigor, evidence rules)
```

## DXVK is part of the configuration, not an optimisation

Put DXVK's `d3d9.dll` in the game folder. Measured, same build and runtime either side:

|configuration|result|
|---|---|
|real hardware, native D3D9|50-60 fps|
|real hardware, DXVK|**72 fps** (the cap)|
|Meta XR Simulator, native D3D9|10-20 fps, minutes to load, the whole PC stutters|
|Meta XR Simulator, DXVK|**71.2 fps**, loads normally, no machine-wide lag|

Simulator only, DXVK the sole variable:

|measurement|native D3D9|DXVK|
|---|---|---|
|game frame rate|10-20 fps|**71.2 fps**|
|mod publish (avg / aggregate)|61.16 ms / 856 ms per s|**2.44 ms / 173 ms per s**|
|host waiting for a game frame|568-660 ms per 90|**0.0-13.6 ms per 90**|
|host content-wait expiries|+70 per 90|**0**|
|host repeats (stale frame re-shown)|+70 per 90|+18 per 90|

**The cause was never the simulator.** Getting a frame to the 64-bit host costs a
`GetRenderTargetData` readback, and on the native runtime that call BLOCKS until the GPU has drained
every prior operation. The simulator's compositor interleaves its own GPU work, which makes each
drain long enough to be fatal -- the game sat at 100% of one core inside the D3D9 driver while using
8.6% GPU, which is what a sync stall looks like from outside. DXVK's readback does not stall that
way, so the amplifier stops mattering. Chasing the simulator for this cost three sessions; the grid
above is what ended it, and no single row of it would have.

A session's worth of plausible causes were eliminated by measurement rather than argument -- an
OpenXR spec violation, software-backed swapchains, a slow host, VR pacing, GPU contention, memory
pressure, CPU saturation, our own render-target override. Every one of them was wrong, and each is
recorded in `reversing/ENGINE_NOTES.md` with the number that killed it.

## The MCP operator layer freezes the simulator's display

**Do not judge what the headset shows while the MCP operator API layer is attached.** It enables
`XR_METAX1_simulator_compositor_output_capture` and starts the simulator's compositor readback, which
then logs `[DIAG-ZEROSKIP] Skipping blit: source image center pixel is zero` once a second and holds
the first frame it accepted. The capture tool cannot produce a frame either (`Failed to capture
frame`). Disable the layer and the display updates normally.

It cost about six fixes aimed at innocent code to find that, and the evidence was in hand early:
every value the host submits was verified correct (swapchains created once, images primed, indices
rotating, `imageRect` valid, `xrEndFrame XR_SUCCESS`, live pixels), our centre pixel read non-zero in
the same second the runtime called it zero -- two readers looking at different images -- and starting
the host in an already-loaded world froze with no resize and no geometry change at all.

**When every value a component submits is verified and the symptom persists, the fault is in the
environment.** The next step is a CONTROL -- run a known-good app, or remove the one thing this setup
has that a working setup does not -- never a seventh change to code the evidence has already
cleared. "This mechanism is real" is not evidence that "this mechanism is the cause"; three of those
six were genuine defects worth fixing, and none of them was the bug.

The layer is still good for DRIVING the runtime: poses, controller input, session and frame queries
all work. Just not for looking.

## Iteration loop

```
build.bat                                   # unload, then configure + build (Win32)
bin\injector.exe --reload                   # hot-swap into the running game
bin\injector.exe --status                   # module/IPC snapshot
ctest --test-dir build -C RelWithDebInfo --output-on-failure
```

## Crash recovery: `python tools/resume_game.py`

**One command takes a dead game to an injected, in-world, test-ready session with no
human.** Verified end to end from a terminated process: 71 seconds, cold -- clocked when the
launch step still went through Steam, and not re-timed since it became the LAA launch.

```
python tools/resume_game.py          # idempotent -- run it any time
```

```
[resume] FEAR2 is not running -- launching with a 4 GB address space (injector --launch)
[resume]   <the injector's own stdout, echoed line by line, indented two spaces>
[resume] waiting for the engine's window
[resume] at the menu -- invoking Menu.StartCheckpoint
[resume] world loaded
[resume] in-world
```

What it does, and why each step is what it is:

1. **`injector.exe --launch --no-host`** -- the project's own launcher, not Steam and not the
   shipped exe. `launch_with_laa` copies `FEAR2.exe`, sets the Large-Address-Aware bit on the
   COPY, and starts it parented to steam.exe, which is the only path that gives the game a 4 GB
   address space. `--no-host` leaves xr64.exe out of it: a fixture wants the address space, not a
   headset.
2. **Wait for `/health`.** There is no separate inject step on the cold path -- `--launch` injects
   in the same call, so running `--inject` after it would be a second attempt against an already
   resident DLL. The warm path (game up, mod not) still injects, and still says so.
3. **`/console/run?cmd=Menu.StartCheckpoint`** -- the game's OWN UI command, not a
   synthesised click. This is "Continue From Last Saved Point".
4. **`/input/tap?vk=32`** to dismiss the load screen's "press to continue".

**"It cannot be launched directly" is TRUE. "So launch it through Steam" does not follow.** The
on-disk `FEAR2.exe` is CEG/SteamStub-wrapped and a plain `CreateProcess` of it fails -- that half
has never stopped being true, and it is why the test runner's `CreateProcessW` never worked. The
conclusion drawn from it was the wrong one. `launch_with_laa` satisfies the stub without asking
Steam to launch anything: Steam's environment and command line come from the registry, and the
process is created with steam.exe as `PROC_THREAD_ATTRIBUTE_PARENT_PROCESS`. Steam must be
RUNNING, since the parent handle is opened on a live steam.exe (`Injector.cpp:544-548`) and the
stub answers "Application load error 5:0000065434" without it -- `resume_game.py` starts Steam
itself when it is not up. It is simply never asked to start the GAME. **Do not launch through
`steam://rungameid`.** It yields a 2 GB session, and
`do_launch`'s "NO SILENT FALLBACK" branch in `injector/Injector.cpp` refuses to fall back to it deliberately, because that configuration "causes
the crashes this exists to prevent".

**When a launch fails, read the injector's echoed lines**, indented under `[resume]`. They are the
whole diagnosis -- whether the exe was found, whether steam.exe could be opened for parenting,
whether gameclient.dll ever came up. The Steam route printed nothing at all on failure, which is
what made three consecutive 200-second launch timeouts so expensive to explain.

**The menu CANNOT be driven by synthetic input.** Measured, not assumed -- all three
routes fail, and the evidence is in `reversing/REVERSING_LESSONS.md`:

| route | result |
|---|---|
| `SendInput` (VK and scancode, window verified foreground) | cursor moves, highlight follows, nothing activates |
| window-message key queue | never drained -- buffered input is false at the menu AND in play |
| engine device array via its own entry points | press visible in the engine's own `MouseState`, Scaleform ignores it |

So step 3 dispatches what a click would have dispatched. `sdk::UiCommands` maps
gameclient's table of 52 `{name, handler, flag}` rows, found by scanning for a POINTER
to a known command name (the table is built by a run of `mov`s, so a byte pattern over
it would encode link-time offsets).

**Do not substitute the console's `LoadCheckpoint`.** It reaches the same dispatcher at
a different mode and refuses outside a loaded world -- it prints "You can only reload a
checkpoint from within the world" and returns, which at the menu looks exactly like a
broken call. `Menu.StartCheckpoint` is mode 8, `Menu.ContinueGame` mode 1,
`LoadCheckpoint` mode 9.

**Step 4 works where step 3's equivalent does not**, and that asymmetry is the useful
fact: once a world is loaded the GAME consumes device-array input, so synthetic keys
land. Only the Scaleform front end ignores them.

## Iteration, once you are in

**`build.bat` unloads the payload before building.** A resident `fear2vr.dll`
holds its own file open, so relinking it fails with `LNK1104: cannot open file
...\fear2vr.dll` — an error that names a file rather than the cause, so it reads
as a broken build instead of "the game still has the last one loaded". Since the
whole point of this project is inject → test → uninject without restarting the
game, the DLL is resident most of the time and that was the NORMAL case. The
unload is a no-op when nothing is resident (exit 0), so it runs unconditionally;
`FEAR2VR_NO_UNLOAD=1` opts out. It is deliberately not gated on success — a
payload that cannot unload goes dormant on purpose, and then the right outcome is
a loud link failure with the injector's own explanation directly above it.

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

## Head tracking

`HeadTracking` composes a head orientation into the camera's ADDITIVE rotation slot -- the engine computes
`camera = holder[+552] * holder[+324]`, aim on the right, and the left operand is identity until something
leans or shakes the view. Putting the head pose there means looking around ADDS to aiming instead of seizing
it, and releasing restores exactly.

Writing the field from outside is reclaimed within a frame, so the mod owns the writer
(`PlayerCamera_UpdateAttachedRotation`, gameclient +0xDED80, `__thiscall`, `this` IS the holder) and amends
its result. Drive it with `/vr/head?yaw=&pitch=` or `?x=&y=&z=&w=`, release with `?clear=1`.

**Axes, measured:** `x = roll, y = yaw, z = pitch`. `/vr/head?yaw=&pitch=&roll=` takes degrees and builds
the quaternion; `?x=&y=&z=&w=` takes one directly. The euler helper originally put pitch in `x` and applied
roll instead -- only yaw had been tested. If you add an axis, test it against a fixed world point.

**The aim is always level.** `PlayerCamera_CancelAimRoll` (gameclient +0xDF500) rebuilds the aim quaternion
as `FromEuler(pitch, yaw, 0)` about 28 times a second, so head tilt cannot be stored at +324 -- it belongs in
the outer operand, which is where `HeadTracking` puts it. `sdk::PlayerMgr::aim_roll()` reports the aim's roll
(measured as the right vector's departure from horizontal, not an Euler term).

**Verify it with the projection probe, never a screenshot.** `/vr/head?px=&py=&pz=` names a world point that
`CameraPassHook` projects inside the pass detour; a stationary point sliding across the screen is the only
direct evidence the rendered view turned. Read from any other thread the projection lands on the frame's
ortho HUD pass and means nothing. Desktop captures in this session were stale while the renderer was fine.

## The 2D / HUD pass

`HudPassHook` owns CLTRenderer slot 17 (`SetupPassStored`), which is where the HUD is painted -- established
by hooking slots 16 and 17 together and watching which one runs. Slot 16 never fires in normal play; slot 17
fires ~10 times a frame and leaves the record orthographic.

Its viewport comes from a bound-target descriptor, not from an argument: `+0x170` is the target pointer
(flag bit `0x800` gates the offset), `+0x174/+0x178` the dimensions, `+0x17C/+0x180` the offset added to all
four edges. An external write there is reclaimed -- the descriptor is rebuilt per target bind -- so the mod
writes it inside the pass entry. `/vr/hud?x=&y=` translates the HUD exactly; `?clear=1` restores.

Both the descriptor and the gate read as unavailable from the IPC thread, because no target is bound between
frames. Read them in phase or not at all.

## Bones and pieces: putting hands, weapons and a body into VR

`BoneControl` drives a skeleton node through ILTModel's node-control mechanism -- the engine calls
a registered function during skeleton evaluation and hands it the node's own writable transform.
Nothing is hooked, and there is no race with the animation system, because the callback runs
INSIDE the evaluation. Everything socketed to the node follows: displacing the hand bone by 30
moves the hand socket by 29.99 and the weapon's muzzle by 30.00.

`/vr/bone?socket=RightHand&x=&y=&z=` displaces, `&qx=&qy=&qz=&qw=` rotates, `?detach=1` releases.
`/sdk/skeleton` lists the 65 nodes and 19 sockets to pick from.

**"RightHand" is a SOCKET, not a node.** Sockets are the art's named attach points and ride nodes;
there is no node of that name, so a node lookup correctly fails on it. Use
`attach_to_player_socket`, or `/sdk/skeleton` to see which node a socket rides.

**A registration is a pointer into this DLL that `Hooks::retire()` cannot see.** It only covers
safetyhook. If a cell is still linked when the image unmaps, the next skeleton evaluation calls
freed memory. `BoneControl::on_shutdown` unlinks it, and the suite leaves one registered on
purpose across the uninject -- the same discipline the hardware watch already gets.

`sdk::model_set_piece_hidden` switches a piece's draw off while its geometry, sockets and
animation carry on, so a weapon on a hidden arm still tracks. `/sdk/piece` lists every piece with
its live hidden state; `?name=arms&hide=1` sets one, `?unhide_all=1` restores. The player has
`Phead_Group`, `arms`, `body` and `head_shadow` among its 11.

**The desktop capture is not currently usable as evidence.** It returns a black frame while the
engine reports a live world and a climbing pass count. Verify through engine state -- the getter,
the projection probe -- not a screenshot.

## The three facing directions, and the two player objects

`sdk::PlayerMgr::aim_vs_view()` reports where the camera looks, where the aim points and which way
the body faces, plus the angles between them -- all from one read, so they share an instant.
`sdk::forward_of()` and `sdk::rotate_vector()` (Object.hpp) are the primitives under it; this
engine is +X right, +Y up, +Z FORWARD.

Live, with a head pose composed in: view-vs-aim equals the commanded yaw to three decimals, and
body-vs-aim never moves off a constant 90 degrees (an axis-convention offset, not a divergence).
Head-look moves the view and nothing else.

**The weapon is the exception, and it is not the bones.** The first-person rig rotates rigidly with
the camera because the SHELL player object's rotation is rewritten through `LTObject_SetRotation`
whenever the view changes. Every candidate bone (`aimer`, `Pelvis_Cam`, `Pelvis`, `Torso`,
`attach`) is constant across a head sweep. Decoupling the gun means owning that writer.

**TWO PLAYER OBJECTS, and watching the wrong one answers confidently and wrongly.**
`CClientShell::local_player` and `PlayerMgr::player` return different allocations; the latter's
rotation is never written at all. `/sdk/shader-params` publishes `pmgr_object_addr`,
`pmgr_holder_addr`, `pmgr_camera_addr` and `pmgr_model_addr`, and `/sdk/targets` publishes
`shell_obj`, because every "who writes this" question starts by pasting one into `/watch/arm`.

## Viewmodel decoupling

`ViewmodelDecouple` stops head-look from swinging the weapon. The first-person rig hangs off
`CClientShell::local_player`'s object (NOT `PlayerMgr`'s), whose rotation the engine rewrites to the
VIEW whenever the view changes. The mod owns that setter and passes on `conj(outer) * incoming`,
removing whatever is in the camera's additive slot.

`/vr/viewmodel?on=1` arms it, `?on=0` releases. Measured at 45 degrees of head yaw: the rig's angle to
the aim falls 44.646 -> 0.355, and the muzzle's world travel 59.08 -> 0.48.

**Hook slot 4 of the OBJECT's own vtable, not the base setter.** Slot 4 dispatches per type: the
player is a MODEL, so the live entry is `OT_MODEL_SetRotation` (0x428E1C), which calls
`LTObject_SetRotation` and then a gated fixup. Taking the base would catch every OT_NORMAL in the
level and miss that branch. `sdk::object_rotation_setter(obj)` resolves it from the object and
refuses anything outside the exe, so no address is hardcoded.

**The setter only fires when the view CHANGES** -- the engine elides it otherwise. Anything testing or
driving this must release the pose, set its state, then apply the pose; setting the same pose twice
measures nothing.

## Turning the player (VR stick turn)

`sdk::Input::send_mouse_look(dx, dy)` drives the engine's own `LTInputDevice_Mouse_OnMouseMove`, so
sensitivity, acceleration, the pitch clamp and every downstream consumer behave exactly as they do
for a mouse -- unlike writing the aim, which bypasses all of it and fights
`CPlayerCamera_ApplyLookToRotation`. The entry takes an ABSOLUTE client point; the SDK does the
centre-relative arithmetic and takes a delta.

`SyntheticInput::queue_look(dx, dy)` is the one to call: it puts the move on the game thread (device
state is read without a lock) and ACCUMULATES, so two deltas in a frame become one move of their
sum. Route: `/input/look?dx=&dy=`.

**The gain is not constant.** dx=200 turns ~28.5-28.9 degrees, dx=400 turns less than twice that, and the
first delta after an injection was seen turning roughly double a later identical one. Anything
needing an exact heading must CLOSE THE LOOP: read `sdk::PlayerMgr::aim_yaw()`, correct, repeat --
converges in 1-3 iterations to under 0.3 degrees. That is also how a snap turn should be built.

## Locomotion and snap turn

**Movement is AIM-relative.** Measured: holding forward with a head pose composed in gives a velocity
whose bearing equals `aim_yaw` to 0.00 degrees, at 312 units/second. The player walks where the body
points, never where they look. `sdk::PlayerMgr::locomotion()` reports speed, bearing, and the bearing
relative to both the aim and the view -- speed for a comfort vignette, bearing-to-view for "should I
recentre".

`TurnController` turns the player to a HEADING, which an open-loop delta cannot do because the
engine's look gain is not constant. `/vr/turn?by=` snap-turns, `?to=` takes an absolute heading,
`?recentre=1` faces the body where the head is looking (which is how "walk where I look" works, since
movement follows the aim). Live: +/-30 and +/-90 land inside 0.5 degrees in 4-6 corrections.

Three things it has to do, each learned by measuring the failure:
- **Wait for a correction to land** (3 frames) before evaluating the next, or it corrects stale error
  and oscillates into the iteration cap.
- **Require two consecutive in-tolerance readings**, or it stops with a delta in flight and overshoots
  by ~2 degrees.
- **Gate on the player being alive.** Input is still accepted when dead, it just does nothing, so the
  loop burns its whole budget against a frozen heading.

## Comfort (view-motion suppression)

`Comfort` suppresses the VR nausea sources through the engine's OWN console variables --
`HeadBobSpeedScale`, `CameraSwayXSpeed`, `CameraSwayYSpeed`, `DisableCameraShake` -- capturing the
originals and restoring them on release and on shutdown. A console variable is engine state that
OUTLIVES the DLL, so restoring exactly is the contract. `/vr/comfort?on=1` arms, `?on=0` releases.

**Head bob is already off on this build.** All 24 `HeadBob*Wave*` variables and every `*Amp` read
0.0, so `HeadBobSpeedScale` scales a wave with no amplitude: the bob system runs (`bob_active` true
while walking) and displaces nothing. Measured in phase over ~100 passes, the camera-height
excursion is 3.4592 with bob on and 3.4592 with it off -- that is the terrain of the path. Sway and
shake still have non-zero settings and are worth suppressing.

**Measure oscillation IN PHASE.** The published camera position is a snapshot of the last render
pass; sampling it from the IPC thread aliases anything periodic and produced byte-identical
"measurements" for two different walks. `CameraPassHook::reset_height_excursion()` plus
`observed().height_min/max` accumulate one sample per pass, which is what makes a negative result
trustworthy.

## sdk::ObjectWatch -- differences, not snapshots

`CClientMgr` answers "what exists now". Most of what a mod reacts to is a CHANGE in that set:
projectiles, impacts, bodies, pickups. `sdk::ObjectWatch` owns that difference so no consumer
re-invents the identity rule -- objects are matched on (address, handle) TOGETHER, because the
allocator reuses freed object memory and 335 of 3583 live objects carry `kNoHandle`.

Sort-and-search, not a nested scan: OT_NORMAL alone runs to ~1900 objects and the pairwise form
would rule the class out of the per-frame use it exists for.

It reports nothing on the first sample. Reporting the whole world as "appeared" because nobody
looked before is a lie every consumer would then work around.
