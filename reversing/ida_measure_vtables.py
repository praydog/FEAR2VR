"""Measure the engine's self-naming vtables and report PROVEN extents apart from ambiguous ones.

Run inside IDA via py_exec_file. It loads ida_vtable_boundary.py into a FRESH namespace, so a stale
symbol left in IDA's shared globals by an earlier script cannot make a broken run look fine -- which
happened once: _static_vptr_slots used struct.pack while the module had no `import struct`, and it only
worked because another script had already put `struct` in the shared globals.

A table is listed as STRONGLY BOUNDED when a defined data item follows the pointer run and no interior
address looks like a table start. That is NOT proof the first table spans the whole run: the trailing
string bounds the SCANNED RUN, and "no interior start" is the absence of a hit from two recognizers --
a vptr field in initialised data (.data/.rdata/.bss) and a vptr store in code. A start installed some
other way would not be seen. Everything weaker is reported with its evidence and NO count, because a
count past an interior boundary belongs to the next class.
"""

import idc

ANALYZER = r"I:/Programming/projects/fear2/reversing/ida_vtable_boundary.py"

# Every .rdata pointer run whose slot 1 returns a class-name literal, plus the four named earlier.
TABLES = [
    (0x0066E600, "CLTCommonClient"), (0x0066EA70, "CLTPhysicsClient"), (0x0066FC38, "CCompress"),
    (0x006720C8, "ILTClientContentTransfer"), (0x00672130, "CLTFileMgr"),
    (0x00672184, "CLTLoadingProgress"), (0x00672708, "CLTResourceMgr"),
    (0x006727A4, "ILTServerContentTransfer"), (0x00672808, "CLTTextureString"),
    (0x00672878, "CLTTimer"), (0x006728E0, "CLTTimerClient"), (0x00672948, "CLTTimerServer"),
    (0x00674BD8, "CLTCommonServer"), (0x00674E90, "CLTPhysicsServer"),
    (0x00674F60, "CLTSoundMgrServer"), (0x00676520, "CSoundMgr"),
    (0x0067764C, "CWorldParticleBlockerData"), (0x00677E3C, "CLTCursor"), (0x00677FA0, "CLTInput"),
    (0x0067820C, "CLTUI"), (0x00678A60, "ILTOnlineService"), (0x0068FD20, "CLTRenderer"),
    (0x0068FEA0, "CWin32CustomRender"), (0x00690090, "CLTVideoTexture"),
    (0x006900D4, "CLTTextureMgr"), (0x006D3270, "CLTGameUtil"),
    (0x0066F258, "CLTClient"), (0x0066E7E8, "CLTModelClient"),
    (0x00675670, "CLTServer"), (0x00674CD8, "CLTModelServer"),
]


def main():
    ns = {}
    exec(compile(open(ANALYZER).read(), "ida_vtable_boundary.py", "exec"), ns)
    analyze = ns["analyze"]
    proven, ambiguous = [], []
    for vt, cls in TABLES:
        candidates, _ = analyze(vt)
        inline = None
        start = None
        for n, sig, ev in candidates:
            if sig == "INLINE_DATA" and inline is None:
                inline = (n, ev)
            if sig == "TABLE_START" and start is None:
                start = (n, ev)
        if start is not None and (inline is None or start[0] < inline[0]):
            ambiguous.append((cls, vt, start, inline))
        elif inline is not None:
            proven.append((cls, vt, inline[0]))
        else:
            ambiguous.append((cls, vt, None, None))

    print("STRONGLY BOUNDED -- data item follows the run, no interior start DETECTED: %d" % len(proven))
    for cls, vt, n in sorted(proven):
        print("   %-26s 0x%08X  %3d entries   %s" % (cls, vt, n, idc.get_name(vt) or ""))
    print("\nEXTENT NOT ESTABLISHED -- start candidate only, no count recorded: %d" % len(ambiguous))
    for cls, vt, start, inline in sorted(ambiguous):
        where = start[0] if start else None
        why = start[1][:60] if start else "no signal at all"
        print("   %-26s 0x%08X  interior start at %s (%s); string at %s"
              % (cls, vt, where, why, inline[0] if inline else None))
    return proven, ambiguous


main()
