#pragma once

#include <cstdint>

// Engine fundamentals for FEAR2.exe (client engine -- LithTech Jupiter EX),
// runtime-resolved by kananlib signature scans. Every address comment cites its
// IDA evidence from FEAR2_dump.exe.i64 (imagebase 0x400000); the scans target
// the LIVE (unpacked-in-memory) image, so no absolute RVA math is trusted --
// the pattern match IS the resolution.
namespace sdk::engine {

// LTRESULT cis_GetEngineHook(char* pName, void** pData) -- the engine's
// name->variable bridge. Evidence: lithtech/runtime/client/src/sys/win/
// winclientde_impl.cpp cis_GetEngineHook (handles "hwnd", "cres_hinstance",
// "cresl_hinstance", "cshell_hinstance", "d3ddevice"); FEAR2_dump.exe
// 0x46AA1E (retn 8 = __stdcall, 2 args) handling at least "hwnd" and
// "cshell_hinstance".
using GetEngineHook_fn = int(__stdcall*)(const char* name, void** out_data);

struct Targets {
    uintptr_t client_mgr_update = 0;   // CClientMgr::Update   (FEAR2_dump.exe 0x40B665) -- per-frame
    uintptr_t client_shell_update = 0; // CClientShell::Update (FEAR2_dump.exe 0x40CC5E) -- per-frame when shell exists
    uintptr_t get_engine_hook = 0;     // cis_GetEngineHook    (FEAR2_dump.exe 0x46AA1E, __stdcall)

    uintptr_t p_g_pClientMgr = 0; // &g_pClientMgr  (0x6ECCA0) -- CClientMgr* global
    uintptr_t p_g_hMainWnd = 0;   // &hWnd          (0x6E4724) -- engine main window HWND slot
};

// Resolve all targets via signature scans over the live FEAR2.exe image.
// Returns false (and logs the first failure) if any pattern fails to match.
// Idempotent: subsequent calls return the cached result.
bool resolve();
bool is_resolved();
const Targets& targets();

// Typed conveniences (valid only when is_resolved()):
//   client manager instance -- engine-side g_pClientMgr, non-null once the
//   engine has initialized (main menu onward).
void* get_client_mgr();

//   engine main window (HWND as void*), read from the engine's global each
//   call (window existent from very early boot).
void* get_main_hwnd();

//   cis_GetEngineHook callable wrapper; nullptr when unresolved.
GetEngineHook_fn get_engine_hook();

// Offset of m_pClientShellDE within CClientMgr: the dump reads the shell
// pointer as *(this + 5172) = *(this + 0x1434) in CClientMgr::Update.
constexpr uint32_t kClientMgr_pClientShell_offset = 0x1434;

// IDatabaseMgr via the gamedatabase.dll export
//   ?LTGetIDatabaseMgr@@YAPAVIDatabaseMgr@@XZ  (cdecl; returns address of the
//   static CDatabaseMgr object at gamedatabase!0x100122BC, vtable
//   gamedatabase!0x1000F648 -- FEAR2 gamedatabase.dll.i64).
// nullptr when gamedatabase.dll is unresolved or the export is missing.
void* get_database_mgr();

// SEH-guarded 32-bit read; false on fault. Use this (not inline __try in a
// lambda -- MSVC C2712) when probing game memory from test code.
bool safe_read32(const void* address, uint32_t& out);

} // namespace sdk::engine
