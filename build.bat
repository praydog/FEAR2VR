@echo off
rem fear2vr build: 32-bit ONLY (FEAR2.exe is a Win32 process).
rem   build.bat          configure + build Release
rem   build.bat Debug    configure + build Debug
setlocal
rem cmkr skips CMakeLists regeneration when CI is set (any value) -- clear it.
set CI=
set CONFIG=%1
if "%CONFIG%"=="" set CONFIG=Release
cmake -B build -A Win32 || exit /b 1
cmake --build build --config %CONFIG% || exit /b 1
echo.
echo Built %CONFIG% into build\bin (fear2vr.dll, injector.exe, fixture-test.exe, command-server-test.exe)
endlocal
