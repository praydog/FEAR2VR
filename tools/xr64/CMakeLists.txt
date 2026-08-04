cmake_minimum_required(VERSION 3.20)

# A standalone 64-bit control, deliberately NOT part of the mod's build: the mod is 32-bit only
# because FEAR2.exe is, and this exists to answer whether the 32-bit OpenXR failure is Meta's or
# ours. Configure it separately:
#
#   cmake -B build64 -A x64 -S tools/xr64
#   cmake --build build64 --config RelWithDebInfo
#
# RelWithDebInfo, NOT Release: this process crashes in the field and a Release build leaves nothing
# to read -- no PDB, so a faulting address is just an address. The optimiser still runs; the only
# cost is the symbol file sitting beside the exe.
project(xr64 CXX)

set(CMAKE_CXX_STANDARD 20)
set(OPENXR_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../deps/openxr")

# ---- RMLUI, FOR THE IN-HEADSET SETTINGS PANEL -----------------------------------------------
#
# FetchContent, pinned to a tag, matching how the root project's cmake.toml vendors kananlib and
# safetyhook (see [fetch-content.*] there) -- this file is plain CMake rather than cmkr, since xr64
# is deliberately NOT part of that build, so the same policy is spelled out by hand here instead.
#
# CMAKE_POLICY_VERSION_MINIMUM: FreeType's own CMakeLists still declares
# `cmake_minimum_required(VERSION 3.0...3.5)`, which CMake 4.x refuses outright ("Compatibility
# with CMake < 3.5 has been removed") unless told to accept it. The root project's cmake.toml sets
# the same variable for the same reason (kananlib/safetyhook are equally old).
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

include(FetchContent)

# RmlUi wants a real `Freetype::Freetype` target (it does `find_package(Freetype)` then checks
# `if(NOT TARGET Freetype::Freetype)` -- see RmlUi's CMake/Dependencies.cmake); FetchContent does
# not manufacture that name on its own, so an alias is added right after MakeAvailable. Optional
# codecs FreeType would otherwise auto-detect on the machine (HarfBuzz/Brotli/PNG/BZip2/zlib) are
# turned off: none of them matter for rendering a settings panel's own UI font, and leaving them on
# means the build's shape depends on what happens to be installed on whichever machine configures
# it.
FetchContent_Declare(freetype
	GIT_REPOSITORY https://github.com/freetype/freetype.git
	GIT_TAG VER-2-13-3
	GIT_SHALLOW TRUE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)  # static: one fewer DLL to keep beside the exe
FetchContent_MakeAvailable(freetype)
if(NOT TARGET Freetype::Freetype)
	add_library(Freetype::Freetype ALIAS freetype)
endif()

# RmlUi 6.2 (newest tagged release as of writing; task only required 5.1+). Samples/backends/Lua
# are all off -- this project supplies its own D3D11 render interface and system interface
# (tools/xr64/ui/), so none of RmlUi's own sample windowing backends are built.
FetchContent_Declare(rmlui
	GIT_REPOSITORY https://github.com/mikke89/RmlUi.git
	GIT_TAG 6.2
	GIT_SHALLOW TRUE)
set(RMLUI_SAMPLES OFF CACHE BOOL "" FORCE)
set(RMLUI_FONT_ENGINE "freetype" CACHE STRING "" FORCE)
set(RMLUI_LUA_BINDINGS OFF CACHE BOOL "" FORCE)
set(RMLUI_LOTTIE_PLUGIN OFF CACHE BOOL "" FORCE)
set(RMLUI_SVG_PLUGIN OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(rmlui)

add_executable(xr64 main.cpp
	ui/UiSystemInterface.cpp
	ui/UiRenderInterfaceD3D11.cpp
	ui/SettingsUi.cpp)
target_include_directories(xr64 PRIVATE "${OPENXR_ROOT}/include"
	"${CMAKE_CURRENT_SOURCE_DIR}/../../shared")  # SharedFrame.hpp is compiled into both bitnesses
target_link_libraries(xr64 PRIVATE
	"${OPENXR_ROOT}/native/x64/release/lib/openxr_loader.lib"
	d3d11
	dxgi
	d3dcompiler
	RmlUi::Core
)
target_compile_definitions(xr64 PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)

# The 64-bit loader must sit beside the exe, exactly as the 32-bit one does beside the mod.
add_custom_command(TARGET xr64 POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_if_different
		"${OPENXR_ROOT}/native/x64/release/bin/openxr_loader.dll"
		"$<TARGET_FILE_DIR:xr64>/openxr_loader.dll"
	VERBATIM)

# settings.rml/.rcss, loaded at runtime from beside the exe (SettingsUi::init resolves its own
# module path) -- same "ship it beside the binary" pattern as the loader above, just a directory
# instead of a single file, and re-synced on every build so an edited .rml is never stale.
add_custom_command(TARGET xr64 POST_BUILD
	COMMAND ${CMAKE_COMMAND} -E copy_directory
		"${CMAKE_CURRENT_SOURCE_DIR}/ui/assets"
		"$<TARGET_FILE_DIR:xr64>/ui/assets"
	VERBATIM)
# The 64-bit half of the IPC benchmark. Same source as the 32-bit target in the main build, because
# the whole point is that both bitnesses agree on the shared layout.
add_executable(ipcbench64 ../ipcbench/main.cpp)
set_target_properties(ipcbench64 PROPERTIES OUTPUT_NAME ipcbench64)
target_compile_definitions(ipcbench64 PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)

# ---- CRASHES MUST LEAVE SOMETHING BEHIND --------------------------------------------------------
#
# The mod writes a dump and a symbolised stack when it faults; the host used to write nothing at
# all, so "xr64 crashed" was the entire report. Debug info is generated for EVERY configuration so
# an accidental Release build is still readable.
target_compile_options(xr64 PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/Zi>)
target_link_options(xr64 PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/DEBUG /OPT:REF /OPT:ICF>)
