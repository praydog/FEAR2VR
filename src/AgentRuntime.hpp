#pragma once

#include <cstdint>

// Agent runtime lifecycle, extracted from the DLL entrypoint so the risky
// teardown sequencing lives in exactly one place (fear2-core), mirroring how
// il2cpp-scripting's runtime::run_supervisor centralizes unload/quiesce/unmap.
namespace runtime {

// Run the full lifecycle on the CALLING thread (intended: a thread spawned by
// the consuming DLL's DllMain). Blocks until an unload is requested over IPC,
// then retires every hook, proves quiescence (no other thread's EIP inside our
// module), and either:
//   - unmaps `self` via FreeLibraryAndExitThread (clean; this function never
//     returns), or
//   - stays DORMANT: the module remains mapped (hooks retired, IPC stopped) so
//     a wedged straggler survives; the injector loads the next build under a
//     fresh filename.
//
//   self     : the consuming module's HMODULE; passed as void* to keep this
//              header windows-free; used for the log path and the EIP-in-module
//              quiescence range.
//   ipc_port : localhost command-server port.
void run_supervisor(void* self, int32_t ipc_port);

} // namespace runtime
