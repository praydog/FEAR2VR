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
5a. **ReGenny is ground truth for LAYOUT; the SDK class owns BEHAVIOR.** Once
   a type is live-mapped in `reversing/fear2.genny` (ReGenny, verified field-
   by-field against raw memory -- see "IDA/dump workflow" below), generate
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
   - **Testing corollary**: because the mod is truly injected and running
     in-process, host-side tests must NOT reimplement the SDK's struct
     traversal via ReadProcessMemory (that only proves hand-derived offsets
     agree with themselves, not that the compiled SDK code is correct or
     safe to actually call). Let the DLL exercise `entry_count()`/`entry(i)`/
     etc. for real and report results; the host verifies OS-level ground
     truth it alone can determine (Toolhelp32 module residency), sanity-
     checks the reported content, and confirms the process/IPC survived the
     call (crash-proof). RPM stays legitimate for simple global-pointer
     cross-checks the DLL could otherwise "lie" about (e.g. `/sdk/targets`'s
     `hWnd_slot`/`g_pClientMgr_slot`), never for re-walking a mapped struct.
6. **kananlib first.** Before writing any binary-analysis helper, check
   `I:\Programming\projects\kananlib\include\utility/*.hpp` (Scan, Module, Seh,
   Address, Patch, PointerHook, VtableHook). Do not reimplement what it has;
   use `KANANLIB_SEH_TRY/KANANLIB_SEH_EXCEPT` instead of raw `__try`.
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
9. **IDA workflow.** Five IDBs: FEAR2_dump.exe (unpacked; the on-disk FEAR2.exe
   is CEG/SteamStub-wrapped and *useless* for static analysis — its `.text` is
   ciphertext), gameclient.dll, gameserver.dll, gamedatabase.dll, ltmemory.dll.
   - After `select_instance`, ALWAYS verify with `server_health` by IDB
     filename before trusting any result (MCP instance switching can silently
     misroute).
   - Rename functions/globals in IDA as you map them (MCP `rename` batch; see
     `skill://ida-friction` for the persistent-rename gotchas). Verify by
     re-decompiling, and run `idb_save` when a mapping batch lands.
   - The lithtech/FEAR sources in `I:\Programming\projects\fear-source-code`
     are *structural analogues* for naming (Build 69 ≈ FEAR1; FEAR2 is "Loki"),
     not ground truth for FEAR2's exact build.
10. **Build system is cmkr.** Edit `cmake.toml`, never `CMakeLists.txt`
    (regenerated). cmkr skips regeneration when `CI` is set in the environment;
    `build.bat` clears it — beware when invoking cmake by hand. cmkr expands
    `**.cpp` globs at configure time: after adding/removing source files you
    must re-run configure (`env -u CI cmake -B build -G "Visual Studio 18 2026" -A Win32`).
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

## Layout

```
cmake.toml / build.bat        -- cmkr build (Win32 enforced)
shared/                       -- Log, HttpClient, sdk/ (fear2-sdk static lib)
shared/sdk/                   -- Modules, CClientMgr, CClientShell, Engine, DatabaseMgr
shared/sdk/regenny/           -- generated (sdk:generate) layout structs + Primitives.hpp shim
reversing/                    -- fear2.genny (ground truth), DatabaseMgrDump.lua, dump artifacts
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
