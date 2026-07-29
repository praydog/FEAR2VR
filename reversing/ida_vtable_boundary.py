"""Vtable BOUNDARY-CANDIDATE analyzer for the FEAR 2 IDBs.

Run inside IDA (ida-pro-mcp py_exec_file, or File > Script file), then call
`report(0x66F258)`.

WHY THIS EXISTS
---------------
Sizing a vtable by reading forward until the dwords "stop looking like code" is
unsound, and this project got it wrong five times before writing the tool:

  * a valid slot may target a thunk or another module, which TRUNCATES the scan;
  * the next table's entries are executable too, so the scan OVERRUNS;
  * /OPT:ICF folds identical methods, so adjacent tables can share entries and
    the join is invisible.

WHAT IT DOES NOT DO
-------------------
It does not prove an extent. Every signal below is a CANDIDATE with evidence
attached, and the caller decides. The only boundary this project treats as
settled is one where something independent says "the array stops here" -- for
CLTClient_vftable that was the dword past the end being referenced as
`offset aCltclient`, i.e. demonstrably a string literal.

THE SIGNALS
-----------
1. INLINE_DATA  a DEFINED data item sits where the next slot would be. A string
                here is the strongest stop available, and it is what settled
                CLTClient_vftable. This is the only signal that halts the scan
                on its own.

1b. The target-side cases are deliberately NOT stops, because treating them as
    stops IS the truncation failure above:
      UNDEFINED_CODE  target is in an executable segment but IDA never made a
                      function there. Scan continues; consider define_func.
      UNKNOWN_TARGET  target is unmapped or in a non-executable segment. A weak
                      candidate: a foreign-module or runtime-filled slot looks
                      exactly the same.
2. TABLE_START  an address inside the run is installed as a vptr from outside it
                (`mov [reg], offset X`). Credible evidence that a table starts
                there -- and therefore that everything after it belongs to a
                DIFFERENT table.
2b. REFERENCED  an interior address is referenced, but not by a vptr store. Could
                be a patch site or a pointer-to-table; weak.
3. REPEAT       a WARNING, not a boundary: an entry that writes the scan's own
                base through a register (a vptr reset, i.e. destructor-shaped)
                appears twice. Usually that means the run crossed into an
                adjacent table, which is the mistake whose absence cost this
                project a withdrawn count. But it is not proof -- MSVC emits
                destructor VARIANTS (scalar and vector deleting), and ICF can
                make unrelated slots share one address, so a legitimate table
                can contain the same pointer twice. Treat it as "stop and check
                what these slots are".
"""

import collections
import struct

import ida_bytes
import ida_funcs
import ida_segment
import ida_xref
import idautils
import idc

CAP = 600


def _describe(ea):
    sc = idc.get_strlit_contents(ea)
    if sc:
        try:
            return "string %r" % sc.decode("ascii", "ignore")
        except Exception:
            return "string (non-ascii)"
    return "0x%08X" % ida_bytes.get_dword(ea)


_CODE_IMM_CACHE = None
_DATA_SLOT_CACHE = None


def _build_caches():
    """One pass over .text and the data segments, instead of a segment search per address.

    Per-address searching made a 30-table measurement time out; the addresses of interest all lie in a
    known range, so a single indexing pass answers every query afterwards.
    """
    global _CODE_IMM_CACHE, _DATA_SLOT_CACHE
    if _CODE_IMM_CACHE is not None:
        return
    import struct as _struct
    rd = ida_segment.get_segm_by_name(".rdata")
    lo, hi = (rd.start_ea, rd.end_ea) if rd else (0, 0)
    code = {}
    sg = ida_segment.get_segm_by_name(".text")
    if sg is not None:
        blob = ida_bytes.get_bytes(sg.start_ea, sg.end_ea - sg.start_ea) or b""
        for k in range(len(blob) - 3):
            v = _struct.unpack_from("<I", blob, k)[0]
            if lo <= v < hi:
                code.setdefault(v, []).append(sg.start_ea + k)
    data = {}
    for segname in (".data", ".rdata", ".bss"):
        d = ida_segment.get_segm_by_name(segname)
        if d is None:
            continue
        blob = ida_bytes.get_bytes(d.start_ea, d.end_ea - d.start_ea) or b""
        for k in range(0, len(blob) - 3, 4):
            v = _struct.unpack_from("<I", blob, k)[0]
            if lo <= v < hi:
                data.setdefault(v, []).append(d.start_ea + k)
    _CODE_IMM_CACHE, _DATA_SLOT_CACHE = code, data


def _address_taken_in_code(addr):
    """Instructions loading `addr` as an IMMEDIATE. A WEAK signal, reported separately.

    Broader than a vptr store on purpose -- the common install is two instructions, `mov reg, offset table`
    then `mov [ptr], reg`, which a single-instruction test cannot see. But taking an address is not
    installing it: this does NOT verify the value is subsequently stored into an object, so it must not be
    presented as a table start. It is a hint that something treats the address as a value.
    """
    _build_caches()
    out = []
    for site in _CODE_IMM_CACHE.get(addr, []):
        head = idc.prev_head(site + 1)
        if head == idc.BADADDR or not ida_bytes.is_code(ida_bytes.get_full_flags(head)):
            continue
        for op in range(4):
            if idc.get_operand_type(head, op) == idc.o_imm and idc.get_operand_value(head, op) == addr:
                out.append(head)
                break
    return out


def _static_vptr_slots(addr):
    """Aligned dwords in initialised data whose value is `addr` -- an object's vptr field.

    Covers .data, .rdata (a const object) and .bss, because restricting this to .data was itself a way to
    miss a table start.
    """
    _build_caches()
    return list(_DATA_SLOT_CACHE.get(addr, []))


def _is_vptr_store_to(ins, target):
    """Is `ins` a `mov [reg{+disp}], offset target` -- i.e. something installing `target` as a vptr?

    This is what makes a REFERENCED candidate CREDIBLE as a table start. A reference that is not a
    vptr store (a patch site taking a slot's address, a relocation entry, a table of pointers to
    tables) does not indicate a class boundary.
    """
    if idc.print_insn_mnem(ins) != "mov":
        return False
    if idc.get_operand_type(ins, 1) != idc.o_imm or idc.get_operand_value(ins, 1) != target:
        return False
    return idc.get_operand_type(ins, 0) in (idc.o_phrase, idc.o_displ)


def _is_vptr_reset_for(fn, base):
    """Does `fn` write `base` through a register, i.e. `mov [reg{+disp}], offset base`?

    The destination MUST be memory reached via a register -- o_phrase (`[esi]`) or o_displ
    (`[esi+4]`). Accepting any `mov <anything>, imm` would also match `mov some_global, offset base`,
    which is a static initializer or an atexit reset, not a vptr write through `this`. That
    distinction matters here: several such global stores exist in this binary and counting them as
    destructors would flag boundaries that are not there.
    """
    for ins in list(idautils.Heads(fn, fn + 0x30))[:12]:
        if idc.print_insn_mnem(ins) != "mov":
            continue
        if idc.get_operand_type(ins, 1) != idc.o_imm or idc.get_operand_value(ins, 1) != base:
            continue
        if idc.get_operand_type(ins, 0) in (idc.o_phrase, idc.o_displ):
            return True
    return False


def analyze(vt, cap=CAP):
    """Return (candidates, entries) for the pointer array at `vt`.

    `candidates` is a list of (index, signal, evidence) in scan order. A CANDIDATE
    INDEX is the only extent estimate this returns -- `len(entries)` is merely how
    far the scan walked, which past a real boundary includes the next table's
    entries, so never quote it as a count.

    Only INLINE_DATA halts the scan. Everything else records and continues, so a
    strong stop later on is not hidden by a weaker signal earlier.
    """
    candidates = []
    entries = []
    seen_reset = {}
    for n in range(cap):
        ea = vt + n * 4
        v = ida_bytes.get_dword(ea)

        # (a) PROVEN INLINE DATA at `ea` -- a defined string or data item where the next slot would be.
        #     This is the only strong stop, and it is the one that settled CLTClient_vftable.
        sc = idc.get_strlit_contents(ea)
        if sc is not None:
            candidates.append((n, "INLINE_DATA", _describe(ea)))
            break

        # (b)/(c) A slot whose TARGET is not a defined function does NOT end the array. IDA may simply
        #     never have created the function, or the slot may point at a thunk or into another module.
        #     Breaking here is the truncation failure this tool exists to prevent, so both cases are
        #     recorded and the scan CONTINUES.
        if ida_funcs.get_func(v) is None:
            tseg = ida_segment.getseg(v)
            if tseg is not None and (tseg.perm & ida_segment.SEGPERM_EXEC):
                candidates.append((n, "UNDEFINED_CODE",
                                   "target 0x%08X is in executable %s but has no function -- undefined "
                                   "code, NOT a boundary; consider define_func" % (
                                       v, ida_segment.get_segm_name(tseg))))
            else:
                where = ida_segment.get_segm_name(tseg) if tseg is not None else "unmapped"
                candidates.append((n, "UNKNOWN_TARGET",
                                   "target 0x%08X is in %s -- weak candidate only; a foreign-module or "
                                   "runtime-patched slot looks identical" % (v, where)))

        # An address inside the run named from outside it: something treats it as a table start. This
        # does NOT halt the scan -- an interior slot can legitimately be referenced (to patch it, for
        # one), and stopping here would hide a later INLINE_DATA, which is the only strong proof.
        if n > 0:
            outside = []
            x = ida_xref.get_first_dref_to(ea)
            while x != idc.BADADDR:
                if not (vt <= x < ea):
                    outside.append(x)
                x = ida_xref.get_next_dref_to(ea, x)
            stores = [a for a in outside if _is_vptr_store_to(a, ea)]
            # A STATICALLY INITIALISED OBJECT installs its vptr in .data, with no code store at all --
            # which is how most of this engine's singletons are built. Looking only for `mov [reg], off`
            # therefore misses exactly those table starts and lets a scan run on into the next class.
            static_vptrs = _static_vptr_slots(ea)
            taken = _address_taken_in_code(ea)
            if stores or static_vptrs:
                src = stores or static_vptrs
                how = "vptr store in code" if stores else "vptr field in initialised data"
                candidates.append((n, "TABLE_START", "%s: %s" % (how, ", ".join(
                    "0x%08X" % a for a in src[:3]))))
            elif taken:
                # WEAK, and kept out of the extent decision: something loads this address as a value, but
                # nothing here shows it being installed as a vptr.
                candidates.append((n, "ADDRESS_TAKEN", "immediate load at " + ", ".join(
                    "0x%08X" % a for a in taken[:3])))
            elif outside:
                candidates.append((n, "REFERENCED", "from " + ", ".join(
                    "0x%08X (%s)" % (a, idc.GetDisasm(a).split(";")[0].strip()[:44]) for a in outside[:2])))

        if v in seen_reset:
            candidates.append((n, "REPEAT", "0x%08X first at index %d -- vptr-reset shape seen twice; "
                                           "usually a crossed table boundary, but destructor variants "
                                           "and ICF can also share a slot, so verify the slot roles"
                                           % (v, seen_reset[v])))
        elif _is_vptr_reset_for(v, vt):
            seen_reset[v] = n
        entries.append(v)
    return candidates, entries


def report(vt, cap=CAP):
    sg = ida_segment.getseg(vt)
    print("table 0x%08X in %s" % (vt, ida_segment.get_segm_name(sg) if sg else "?"))
    candidates, entries = analyze(vt, cap)
    for n, sig, ev in candidates[:12]:
        print("  index %-4d %-14s %s" % (n, sig, ev))
    if len(candidates) > 12:
        print("  ... %d more candidate(s) suppressed" % (len(candidates) - 12))

    inline = next((c for c in candidates if c[1] == "INLINE_DATA"), None)
    # A vptr store naming an interior address is the credible "a table starts here" signal.
    start = next((c for c in candidates if c[1] == "TABLE_START"), None)

    if start and (inline is None or start[0] < inline[0]):
        # THE TRAP THIS BRANCH EXISTS FOR: a later string bounds the whole SCANNED RUN, not this table.
        # Reporting it as this table's extent is how a 2-entry table gets called 187 entries.
        hi = inline[0] if inline else len(entries)
        print("  -> AMBIGUOUS. Something installs 0x%08X as a vptr, so a table plausibly starts at index "
              "%d and the scan has run through adjacent tables." % (vt + start[0] * 4, start[0]))
        print("     Extent of THIS table is in [%d .. %d]; the leading estimate is %d."
              % (start[0], hi, start[0]))
        if inline:
            print("     The string at index %d bounds the whole run, NOT this table -- do not quote it."
                  % inline[0])
        print("     Resolve by classifying the reference at index %d by hand." % start[0])
    elif inline:
        print("  -> %d entries. A defined data item follows the array and no earlier vptr store names an "
              "interior address, which is the strongest available evidence." % inline[0])
    else:
        weak = [c for c in candidates if c[1] in ("UNDEFINED_CODE", "UNKNOWN_TARGET", "REFERENCED")]
        print("  -> NO stop found within %d scanned dwords; %d weak flag(s) only. Extent UNKNOWN -- resolve "
              "the flagged slots before believing any count." % (len(entries), len(weak)))
    return candidates, entries


def scan_dtor_repeats(vt, cap=CAP):
    """Standalone form of signal 3, for auditing a count someone already recorded."""
    hist = collections.Counter()
    for n in range(cap):
        v = ida_bytes.get_dword(vt + n * 4)
        if ida_funcs.get_func(v) is None:
            break
        hist[v] += 1
    dupes = [(v, c) for v, c in hist.items() if c > 1 and _is_vptr_reset_for(v, vt)]
    for v, c in dupes:
        print("  0x%08X appears %d times with a vptr-reset shape -- likely several tables in this run "
              "(destructor variants and ICF are the innocent explanations to rule out)" % (v, c))
    return dupes
