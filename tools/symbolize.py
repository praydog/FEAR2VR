"""Resolve an RVA inside our own DLL to a function name, using its PDB.

WER reports a crash as `faulting_module=Fear2vr.dll offset=000e7b10`, which is exactly the
module+offset form this project uses for engine code -- except here the module is ours and we have
symbols, so the offset can become a name instead of an address to hunt.

    python tools/symbolize.py 0xe7b10 [more offsets...]
"""

import ctypes
import ctypes.wintypes as wt
import os
import sys

DLL = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build", "bin", "Fear2vr.dll")

MAX_SYM_NAME = 2000


class SYMBOL_INFO(ctypes.Structure):
    _fields_ = [
        ("SizeOfStruct", wt.ULONG), ("TypeIndex", wt.ULONG),
        ("Reserved", ctypes.c_ulonglong * 2), ("Index", wt.ULONG), ("Size", wt.ULONG),
        ("ModBase", ctypes.c_ulonglong), ("Flags", wt.ULONG), ("Value", ctypes.c_ulonglong),
        ("Address", ctypes.c_ulonglong), ("Register", wt.ULONG), ("Scope", wt.ULONG),
        ("Tag", wt.ULONG), ("NameLen", wt.ULONG), ("MaxNameLen", wt.ULONG),
        ("Name", ctypes.c_char * (MAX_SYM_NAME + 1)),
    ]


class MODULE_INFO(ctypes.Structure):
    """Only needed to answer "did the PDB actually load, and does it match?" -- the question that
    distinguishes a bad lookup from a bad build."""
    _fields_ = [("SizeOfStruct", wt.DWORD), ("BaseOfImage", ctypes.c_ulonglong),
                ("ImageSize", wt.DWORD), ("TimeDateStamp", wt.DWORD), ("CheckSum", wt.DWORD),
                ("NumSyms", wt.DWORD), ("SymType", ctypes.c_int),
                ("ModuleName", ctypes.c_char * 32), ("ImageName", ctypes.c_char * 256),
                ("LoadedImageName", ctypes.c_char * 256), ("LoadedPdbName", ctypes.c_char * 256),
                ("CVSig", wt.DWORD), ("CVData", ctypes.c_char * (260 * 3)), ("PdbSig", wt.DWORD),
                ("PdbSig70", ctypes.c_byte * 16), ("PdbAge", wt.DWORD), ("PdbUnmatched", wt.BOOL),
                ("DbgUnmatched", wt.BOOL), ("LineNumbers", wt.BOOL), ("GlobalSymbols", wt.BOOL),
                ("TypeInfo", wt.BOOL), ("SourceIndexed", wt.BOOL), ("Publics", wt.BOOL),
                ("MachineType", wt.DWORD), ("Reserved", wt.DWORD)]


class IMAGEHLP_LINE64(ctypes.Structure):
    _fields_ = [("SizeOfStruct", wt.ULONG), ("Key", ctypes.c_void_p),
                ("LineNumber", wt.ULONG), ("FileName", ctypes.c_char_p),
                ("Address", ctypes.c_ulonglong)]


def main(offsets):
    dbghelp = ctypes.WinDLL("dbghelp.dll")
    proc = ctypes.c_void_p(0x1234)  # a fake but unique handle: we never touch a live process

    dbghelp.SymSetOptions(0x00000002 | 0x00000010)  # UNDNAME | LOAD_LINES
    # SEARCH PATH EXPLICITLY. Deferred loading will not go looking beside the image on its own
    # when the "process" is a fabricated handle with no loaded modules to infer a path from.
    search = os.path.abspath(os.path.dirname(DLL)).encode()
    dbghelp.SymInitialize.argtypes = [ctypes.c_void_p, ctypes.c_char_p, wt.BOOL]
    if not dbghelp.SymInitialize(proc, search, False):
        print("SymInitialize failed"); return 1

    # ARGTYPES MATTER HERE. Without them ctypes marshals the 64-bit base as a 32-bit int and the
    # module loads at a garbage address, so every lookup silently returns <no symbol> -- which
    # reads exactly like "the build is stale" and sent me looking at timestamps instead.
    dbghelp.SymLoadModuleEx.restype = ctypes.c_ulonglong
    dbghelp.SymLoadModuleEx.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_char_p,
                                        ctypes.c_char_p, ctypes.c_ulonglong, wt.DWORD,
                                        ctypes.c_void_p, wt.DWORD]
    base = dbghelp.SymLoadModuleEx(proc, None, os.path.abspath(DLL).encode(), None,
                                   0x10000000, 0, None, 0)
    if not base:
        print("could not load symbols for", DLL); return 1

    mi = MODULE_INFO()
    mi.SizeOfStruct = ctypes.sizeof(MODULE_INFO)
    dbghelp.SymGetModuleInfo64.argtypes = [ctypes.c_void_p, ctypes.c_ulonglong, ctypes.POINTER(MODULE_INFO)]
    if dbghelp.SymGetModuleInfo64(proc, ctypes.c_ulonglong(base), ctypes.byref(mi)):
        kinds = {0: "None", 1: "Coff", 2: "Cv", 3: "Pdb", 4: "Export", 5: "Deferred", 6: "Sym", 7: "Dia"}
        print("  symbols: %s from %s%s" % (
            kinds.get(mi.SymType, mi.SymType), mi.LoadedPdbName.decode(errors="replace"),
            "  *** PDB DOES NOT MATCH THE BINARY ***" if mi.PdbUnmatched else ""))

        # AN RVA IS ONLY MEANINGFUL AGAINST THE BUILD THAT PRODUCED IT, and this project rebuilds
        # constantly. The same offset resolved to `memcpy_s` before a rebuild and to
        # `__crt_strtox::divide` after -- both answers confident, one of them nonsense. So the
        # binary's own link timestamp is printed for comparison against when the crash happened.
        import datetime
        linked = datetime.datetime.fromtimestamp(mi.TimeDateStamp, datetime.timezone.utc)
        print("  binary linked: %s UTC  <-- an RVA from a DIFFERENT build resolves to garbage" %
              linked.strftime("%Y-%m-%d %H:%M:%S"))
        print("  file mtime   : %s" % datetime.datetime.fromtimestamp(
            os.path.getmtime(DLL)).strftime("%Y-%m-%d %H:%M:%S"))
    for off in offsets:
        addr = 0x10000000 + off
        buf = SYMBOL_INFO()
        # SizeOfStruct is the size with Name declared as CHAR[1], not CHAR[0]. Getting this one
        # byte wrong makes SymFromAddr fail with GetLastError() == 0 -- no symbol, no error, no
        # clue -- which reads exactly like "the PDB did not match" and cost a detour through
        # timestamps and search paths before SymGetModuleInfo64 showed the PDB was loaded fine.
        buf.SizeOfStruct = ctypes.sizeof(SYMBOL_INFO) - (MAX_SYM_NAME + 1) + 1
        buf.MaxNameLen = MAX_SYM_NAME
        disp = ctypes.c_ulonglong(0)

        dbghelp.SymFromAddr.argtypes = [ctypes.c_void_p, ctypes.c_ulonglong,
                                        ctypes.POINTER(ctypes.c_ulonglong), ctypes.POINTER(SYMBOL_INFO)]
        ok = dbghelp.SymFromAddr(proc, ctypes.c_ulonglong(addr), ctypes.byref(disp), ctypes.byref(buf))
        name = buf.Name.decode(errors="replace") if ok else "<no symbol>"

        line = IMAGEHLP_LINE64()
        line.SizeOfStruct = ctypes.sizeof(IMAGEHLP_LINE64)
        ldisp = wt.DWORD(0)
        dbghelp.SymGetLineFromAddr64.argtypes = [ctypes.c_void_p, ctypes.c_ulonglong,
                                                 ctypes.POINTER(wt.DWORD), ctypes.POINTER(IMAGEHLP_LINE64)]
        where = ""
        if dbghelp.SymGetLineFromAddr64(proc, ctypes.c_ulonglong(addr), ctypes.byref(ldisp), ctypes.byref(line)):
            where = "  %s:%d" % (line.FileName.decode(errors="replace"), line.LineNumber)

        print("  +0x%X  ->  %s +0x%X%s" % (off, name, disp.value, where))

    return 0


if __name__ == "__main__":
    args = sys.argv[1:] or ["0xe7b10"]
    sys.exit(main([int(a, 0) for a in args]))
