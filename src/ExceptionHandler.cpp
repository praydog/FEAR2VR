#include "ExceptionHandler.hpp"

#include <windows.h>

#include <dbghelp.h>
#include <psapi.h>

#include <atomic>
#include <mutex>
#include <string>

#include "Log.hpp"

namespace exception_handler {

namespace {

LPTOP_LEVEL_EXCEPTION_FILTER g_previous{nullptr};
std::atomic<bool> g_installed{false};

// Where a dump goes. Beside fear2vr.log, which is where anyone debugging is already looking.
std::string dump_path() {
    char buf[MAX_PATH]{};
    HMODULE self{};
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(&dump_path), &self);
    GetModuleFileNameA(self, buf, MAX_PATH);

    std::string p{buf};
    const auto slash = p.find_last_of("\\/");

    if (slash != std::string::npos) {
        p.resize(slash + 1);
    } else {
        p.clear();
    }

    return p + "fear2vr_crash.dmp";
}

// MODULE + OFFSET, which is the form this project needs. A raw absolute address is useless the
// moment the process exits; `gameclient.dll+0xE0830` pastes straight into the right IDB, which is
// the same convention /watch/report uses and for the same reason.
std::string describe(uintptr_t addr) {
    if (addr == 0) {
        return "0x00000000";
    }

    HMODULE mod{};
    char line[512]{};

    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(addr), &mod) &&
        mod != nullptr) {
        char path[MAX_PATH]{};
        GetModuleFileNameA(mod, path, MAX_PATH);
        std::string name{path};
        const auto slash = name.find_last_of("\\/");

        if (slash != std::string::npos) {
            name = name.substr(slash + 1);
        }

        snprintf(line, sizeof(line), "0x%08IX  %s+0x%IX", addr, name.c_str(),
                 addr - reinterpret_cast<uintptr_t>(mod));
    } else {
        snprintf(line, sizeof(line), "0x%08IX  (no module -- heap, stack or freed code)", addr);
    }

    return line;
}

void log_callstack(CONTEXT* ctx) {
    // A COPY: StackWalk64 mutates the context it is given, and the one we were handed belongs to
    // the faulting thread and is used again below.
    CONTEXT walk = *ctx;

    STACKFRAME64 frame{};
    frame.AddrPC.Offset = walk.Eip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = walk.Ebp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = walk.Esp;
    frame.AddrStack.Mode = AddrModeFlat;

    LOGX("[crash] call stack (module+offset -- paste the offset into the matching IDB):");

    for (int i = 0; i < 48; ++i) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_I386, GetCurrentProcess(), GetCurrentThread(), &frame, &walk, nullptr,
                         SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) {
            break;
        }

        if (frame.AddrPC.Offset == 0) {
            break;
        }

        // NAME THE FRAME IF WE CAN. dbghelp is already initialised, and our own frames have a
        // PDB beside the DLL -- so ours resolve to functions and source lines while the engine's
        // stay as module+offset for pasting into IDA. Both halves of a mixed stack stay useful.
        char symbuf[sizeof(SYMBOL_INFO) + 512]{};
        auto* sym = reinterpret_cast<SYMBOL_INFO*>(symbuf);
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 511;
        DWORD64 disp = 0;

        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(line);
        DWORD ldisp = 0;
        const bool have_line = SymGetLineFromAddr64(GetCurrentProcess(), frame.AddrPC.Offset, &ldisp, &line) != FALSE;

        if (SymFromAddr(GetCurrentProcess(), frame.AddrPC.Offset, &disp, sym)) {
            LOGX("[crash]   #%02d %s  -> %s+0x%IX%s%s", i,
                 describe(static_cast<uintptr_t>(frame.AddrPC.Offset)).c_str(), sym->Name,
                 static_cast<uintptr_t>(disp), have_line ? "  " : "",
                 have_line ? line.FileName : "");
        } else {
            LOGX("[crash]   #%02d %s", i, describe(static_cast<uintptr_t>(frame.AddrPC.Offset)).c_str());
        }
    }
}

void write_minidump(EXCEPTION_POINTERS* ei) {
    auto* dbghelp = LoadLibraryA("dbghelp.dll");

    if (dbghelp == nullptr) {
        LOGX("[crash] could not load dbghelp.dll -- no dump written");
        return;
    }

    const auto path = dump_path();
    auto* f = CreateFileA(path.c_str(), GENERIC_WRITE, FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS,
                          FILE_ATTRIBUTE_NORMAL, nullptr);

    if (f == nullptr || f == INVALID_HANDLE_VALUE) {
        LOGX("[crash] could not create %s", path.c_str());
        return;
    }

    MINIDUMP_EXCEPTION_INFORMATION info{GetCurrentThreadId(), ei, FALSE};
    auto* write = reinterpret_cast<decltype(MiniDumpWriteDump)*>(GetProcAddress(dbghelp, "MiniDumpWriteDump"));

    if (write != nullptr) {
        write(GetCurrentProcess(), GetCurrentProcessId(), f, MiniDumpNormal, &info, nullptr, nullptr);
        LOGX("[crash] dump written to %s", path.c_str());
    }

    CloseHandle(f);
}

// STACK OVERFLOW NEEDS A DIFFERENT STACK.
//
// On STATUS_STACK_OVERFLOW the faulting thread has one guard page left, and MiniDumpWriteDump plus
// a 48-frame symbolising walk will not fit in it -- the dump attempt faults again and the process
// dies with nothing written, which is the worst possible outcome for the one crash class where the
// stack IS the evidence. So that case is handed to a fresh thread, which has a whole stack.
struct OverflowJob {
    EXCEPTION_POINTERS* ei;
    CONTEXT* ctx;
};

DWORD WINAPI overflow_thread(LPVOID param) {
    auto* job = static_cast<OverflowJob*>(param);
    log_callstack(job->ctx);
    write_minidump(job->ei);
    return 0;
}

} // namespace

LONG WINAPI global_exception_handler(EXCEPTION_POINTERS* ei) {
    static std::recursive_mutex mtx{};
    std::scoped_lock lock{mtx};

    if (ei == nullptr || ei->ExceptionRecord == nullptr || ei->ContextRecord == nullptr) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    auto* rec = ei->ExceptionRecord;
    auto* ctx = ei->ContextRecord;

    // ---- THE DUMB RECORD, WRITTEN FIRST -------------------------------------------------
    //
    // Before touching the logger, because the logger may BE the problem. Two crashes were reported
    // by WER inside our own DLL -- one in memcpy_s, one in common_vsprintf, i.e. inside printf-style
    // formatting -- and this handler logged nothing for either. That is the expected outcome if the
    // handler's first act (LOGX, which formats) re-enters the very code that faulted: it faults
    // again, and a fault inside an exception filter takes the process down with no output at all.
    //
    // So the essentials go out through hand-rolled hex and a raw WriteFile: no varargs, no
    // formatting, no heap, nothing that can fail the same way twice.
    {
        char raw[256];
        size_t n = 0;
        auto put = [&](const char* text) {
            while (*text != '\0' && n < sizeof(raw) - 1) {
                raw[n++] = *text++;
            }
        };
        auto put_hex = [&](uintptr_t v) {
            static const char* digits = "0123456789ABCDEF";
            put("0x");
            for (int shift = 28; shift >= 0; shift -= 4) {
                if (n < sizeof(raw) - 1) {
                    raw[n++] = digits[(v >> shift) & 0xF];
                }
            }
        };

        put("[crash] code=");
        put_hex(rec->ExceptionCode);
        put(" eip=");
        put_hex(reinterpret_cast<uintptr_t>(rec->ExceptionAddress));
        put(" esp=");
        put_hex(ctx->Esp);
        put("\r\n");

        const auto path = dump_path();
        std::string txt = path.substr(0, path.size() - 4) + "_first.txt";
        auto* fh = CreateFileA(txt.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (fh != nullptr && fh != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(fh, raw, static_cast<DWORD>(n), &written, nullptr);
            FlushFileBuffers(fh);
            CloseHandle(fh);
        }
    }

    LOGX("[crash] ================ UNHANDLED EXCEPTION ================");
    LOGX("[crash] code 0x%08lX at %s", rec->ExceptionCode,
         describe(reinterpret_cast<uintptr_t>(rec->ExceptionAddress)).c_str());

    if (rec->ExceptionCode == EXCEPTION_STACK_OVERFLOW) {
        LOGX("[crash] STACK OVERFLOW -- unbounded recursion, or a deep call from a thread with a "
             "small stack");
    } else if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
        LOGX("[crash] access violation: tried to %s 0x%08IX",
             rec->ExceptionInformation[0] == 0 ? "READ" : (rec->ExceptionInformation[0] == 1 ? "WRITE" : "EXECUTE"),
             static_cast<uintptr_t>(rec->ExceptionInformation[1]));
    }

    LOGX("[crash] EIP %s", describe(ctx->Eip).c_str());
    LOGX("[crash] ESP 0x%08lX  EBP 0x%08lX  EFLAGS 0x%08lX", ctx->Esp, ctx->Ebp, ctx->EFlags);
    LOGX("[crash] EAX 0x%08lX  EBX 0x%08lX  ECX 0x%08lX  EDX 0x%08lX", ctx->Eax, ctx->Ebx, ctx->Ecx, ctx->Edx);
    // ECX called out separately in the log's own words because __thiscall puts `this` there, and
    // "which object" is usually the question a crash in engine code raises.
    LOGX("[crash] ESI 0x%08lX  EDI 0x%08lX   (ECX is `this` for a __thiscall frame)", ctx->Esi, ctx->Edi);

    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);

    if (rec->ExceptionCode == EXCEPTION_STACK_OVERFLOW) {
        OverflowJob job{ei, ctx};
        auto* t = CreateThread(nullptr, 0, overflow_thread, &job, 0, nullptr);

        if (t != nullptr) {
            WaitForSingleObject(t, 15000);
            CloseHandle(t);
        }
    } else {
        log_callstack(ctx);
        write_minidump(ei);
    }

    SymCleanup(GetCurrentProcess());
    LOGX("[crash] =====================================================");

    // CONTINUE_SEARCH, not EXECUTE_HANDLER: we are a reporter, not a recovery mechanism. Letting
    // the process die the way it was going to die keeps the game's own crash path -- and WER --
    // intact, and avoids papering over a fault so it recurs somewhere less legible.
    return EXCEPTION_CONTINUE_SEARCH;
}

void install() {
    if (g_installed.exchange(true)) {
        return;
    }

    g_previous = SetUnhandledExceptionFilter(global_exception_handler);
    LOGX("[crash] unhandled-exception filter installed (previous %p)", reinterpret_cast<void*>(g_previous));
}

void reassert() {
    if (!g_installed.load(std::memory_order_relaxed)) {
        return;
    }

    auto* was = SetUnhandledExceptionFilter(global_exception_handler);

    if (was == global_exception_handler) {
        return;
    }

    // Someone else held the slot. Keep the FIRST foreign filter we ever displaced as the one to
    // restore -- that is the game's own, installed before us; a later one may be ours from a
    // previous injection and restoring that would leave a dangling pointer.
    if (g_previous == nullptr) {
        g_previous = was;
    }

    static std::atomic<uint32_t> s_steals{0};
    const auto n = s_steals.fetch_add(1) + 1;

    // Logged sparsely: if the engine re-installs every frame this would otherwise be the whole log.
    if (n == 1 || n % 500 == 0) {
        LOGX("[crash] filter had been replaced by %p -- reclaimed (%u time(s))",
             reinterpret_cast<void*>(was), n);
    }
}

void remove() {
    if (!g_installed.exchange(false)) {
        return;
    }

    SetUnhandledExceptionFilter(g_previous);
    g_previous = nullptr;
}

} // namespace exception_handler
