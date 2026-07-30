@echo off
rem fear2vr build: 32-bit ONLY (FEAR2.exe is a Win32 process).
rem   build.bat               configure + build RelWithDebInfo
rem   build.bat Debug         configure + build Debug
rem
rem Set FEAR2VR_NO_UNLOAD=1 to skip the pre-build unload (see below).
setlocal
rem cmkr skips CMakeLists regeneration when CI is set (any value) -- clear it.
set CI=
set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=RelWithDebInfo

rem UNLOAD FIRST. A resident fear2vr.dll holds its own file open, so relinking it
rem fails with `LNK1104: cannot open file ...\fear2vr.dll` -- and that error names
rem a file, not the cause, so it reads like a broken build rather than "the game
rem still has the last one loaded". The iteration loop is inject -> test ->
rem uninject with the game never restarting, which means the DLL is resident most
rem of the time and this was the normal case, not an edge one.
rem
rem Removing the failure mode beats remembering to avoid it. The unload is a
rem no-op when nothing is resident ("no fear2vr instance resident; nothing to
rem unload", exit 0), so this is safe to run unconditionally.
rem
rem Deliberately NOT gated on success: if the payload cannot unload it goes
rem dormant on purpose (see TESTING.MD's graceful-uninject contract), and the
rem right response is to let the link fail loudly with the injector's own
rem explanation directly above it in the log.
if not defined FEAR2VR_NO_UNLOAD (
    if exist "build\bin\injector.exe" (
        build\bin\injector.exe --unload
    )
)

cmake -B build -G "Visual Studio 18 2026" -A Win32 || exit /b 1
cmake --build build --config %CONFIG% || exit /b 1
echo.
echo Built %CONFIG% into build\bin (fear2vr.dll, injector.exe, fixture-test.exe, command-server-test.exe)
endlocal
