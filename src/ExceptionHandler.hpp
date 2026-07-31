#pragma once

// Last-chance crash reporting: a log line per register, a symbolised call stack in module+offset
// form, and a minidump. Ported from re2-barebones' ExceptionHandler, adapted for x86 and for this
// project's logging.
//
// ---- WHY THIS DOES NOT FIGHT Watchpoints ----------------------------------------------------
//
// Watchpoints registers a VECTORED handler (AddVectoredExceptionHandler) to service hardware data
// breakpoints. This installs an UNHANDLED EXCEPTION FILTER, which is a different stage of the same
// dispatch:
//
//     vectored handlers  ->  SEH frames  ->  unhandled exception filter
//
// VEH runs FIRST, so every STATUS_SINGLE_STEP a watchpoint generates is still claimed by
// Watchpoints and continued before this is consulted. This only ever sees exceptions that nobody
// handled -- which is precisely the definition of a crash, and precisely what we want reported.
//
// The two are therefore complementary rather than competing, and the ordering is the reason.
namespace exception_handler {

// Install. Call once, from framework init -- never from DllMain, where the loader lock makes
// LoadLibrary("dbghelp.dll") on the crash path a deadlock waiting to happen.
void install();

// Re-assert ownership. Call once per frame.
//
// THE GAME INSTALLS ITS OWN FILTER. There is only one slot -- SetUnhandledExceptionFilter is not a
// chain -- so whoever calls last wins, and the engine calls it at points we do not control. When
// it does, our reporter goes silent: measured exactly that, an access violation INSIDE Fear2vr.dll
// reported by WER (faulting_module=Fear2vr.dll, offset 0xE7B10) with nothing in our log and no
// dump, because the slot was no longer ours.
//
// There is no Get* counterpart, but Set* RETURNS the outgoing filter, so re-asserting doubles as
// the detector: if what comes back is not our own function, someone took the slot.
void reassert();

// Restore whatever filter was there before us.
//
// MANDATORY ON UNLOAD, and for the same reason BoneControl removes its node cell inline: the
// process holds a POINTER to our handler, and if we unmap while it is still installed the next
// crash -- or any exception that reaches the last stage -- calls freed memory. That converts a
// diagnosable fault into an unexplainable one, in the component whose entire job is explaining
// faults.
void remove();

} // namespace exception_handler
