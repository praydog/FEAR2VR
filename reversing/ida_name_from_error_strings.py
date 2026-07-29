"""Name FEAR 2 functions from the diagnostic strings they push about themselves.

Run inside IDA (ida-pro-mcp py_exec_file, or File > Script file). It prints a
report; pass apply=True to `run()` to rename.

WHY THIS EXISTS
---------------
The engine's API wrappers report errors with their own qualified name, e.g.
`LT ERROR: %s returned %s (%s)` fed 'CLTClient::GetObjectPos'. That is the
binary naming itself -- ground truth, not a reference header -- so it is the
best naming source in this image.

IDA's xrefs mostly do not reach it. Of 52 CLTClient:: strings only 15 had a
recorded data xref, because the call site pushes the address of a POINTER
VARIABLE holding the string rather than the string itself. The chain that works:

    string -> .rdata/.data dword whose VALUE is the string -> code referencing
    that dword

WHAT IT REFUSES TO DO
---------------------
Raw little-endian address bytes appear in displacements, in embedded data, and
inside unrelated constants, so every byte hit is a CANDIDATE:

  * the hit must lie inside a DECODED INSTRUCTION, and that instruction must
    have an operand equal to the address. A match in the middle of a
    displacement is discarded.
  * a name is applied only when exactly ONE function is reachable that way. Two
    candidates mean two entry points sharing a string -- overloads, or one
    method inherited from each of two interfaces -- and this cannot tell those
    apart, so it reports them and renames nothing.
  * a function that already has a non-`sub_` name is never overwritten.

A shared string can also name a CALLEE rather than the function pushing it, so a
single candidate is strong evidence and not proof; the report exists to be read.
"""

import collections
import re
import struct

import ida_bytes
import ida_funcs
import ida_name
import ida_segment
import idautils
import idc

QUALIFIED = re.compile(r"^[A-Za-z_][A-Za-z0-9_]{1,40}::[A-Za-z_][A-Za-z0-9_]{1,60}$")

# The error path every one of these wrappers ends in:
#     push <detail> ; push <"LT_INVALIDPARAMS"> ; push <ptr to "Class::Method"> ; push g_pLTErrorFormat
#     call LTLog_Printf
# Requiring BOTH of those in a short window after the reference is the semantic gate. Uniqueness alone
# only says no OTHER function mentions the string; it does not say the string names THIS function. A
# helper that merely forwards someone else's name would pass a uniqueness test and fail this one.
LTLOG_PRINTF = 0x00468BD1
LT_ERROR_FORMAT_HOLDER = 0x006E307C
GATE_WINDOW_INSNS = 12


def _seg_blob(name):
    sg = ida_segment.get_segm_by_name(name)
    if sg is None:
        return None, 0
    return ida_bytes.get_bytes(sg.start_ea, sg.end_ea - sg.start_ea), sg.start_ea


def qualified_strings():
    out = {}
    for s in idautils.Strings():
        t = str(s)
        if QUALIFIED.match(t):
            out[s.ea] = t
    return out


def instruction_refs(addr, blob, base):
    """Functions containing an instruction with an OPERAND equal to `addr`."""
    out = set()
    needle = struct.pack("<I", addr)
    off = 0
    while True:
        i = blob.find(needle, off)
        if i < 0:
            break
        off = i + 1
        ea = base + i
        head = idc.prev_head(ea + 1)
        if head == idc.BADADDR:
            continue
        if not ida_bytes.is_code(ida_bytes.get_full_flags(head)):
            continue
        # The documented pattern PUSHES the name holder as an argument. Requiring the mnemonic keeps an
        # unrelated load of the same address in the same block from passing as an argument.
        if idc.print_insn_mnem(head) != "push":
            continue
        matched = False
        for op in range(4):
            if idc.get_operand_value(head, op) == addr:
                matched = True
                break
        if not matched:
            continue
        if not _feeds_error_report(head):
            continue
        f = ida_funcs.get_func(head)
        if f is not None:
            out.add(f.start_ea)
    return out


def _feeds_error_report(head):
    """Is this reference in the SAME STRAIGHT-LINE RUN as a call to LTLog_Printf with the error format?

    Both conditions are required: the call, and the error-format holder referenced before it. The holder
    is what separates the failure path from any other formatted log a function might emit.

    WHAT THIS IS NOT. It is not data-flow: it does not prove the qualified-name operand is still an
    argument to that call, only that both appear in one run of instructions with no branch between them.
    The scan therefore STOPS at any control-flow boundary -- a jump, a conditional, or a return -- so it
    cannot wander across an unconditional jmp into an unrelated or unreachable block, which a plain
    address-proximity window would happily do. Inside a basic block ending in the reporter call, an
    intervening push of the name is strong evidence and still not proof.
    """
    f = ida_funcs.get_func(head)
    if f is None:
        return False
    saw_format = False
    ea = head
    for _ in range(GATE_WINDOW_INSNS):
        ea = idc.next_head(ea, f.end_ea)
        if ea == idc.BADADDR or ea >= f.end_ea:
            return False
        mnem = idc.print_insn_mnem(ea)
        for op in range(4):
            if idc.get_operand_value(ea, op) == LT_ERROR_FORMAT_HOLDER:
                saw_format = True
        if mnem == "call":
            if idc.get_operand_value(ea, 0) == LTLOG_PRINTF:
                return saw_format
            continue  # some other call in the same block is fine; keep looking
        # END OF THE BASIC BLOCK: anything that transfers control means the reporter call reached from
        # here would be a different path, so stop rather than credit it.
        if mnem.startswith("j") or mnem.startswith("ret") or mnem in ("loop", "loope", "loopne"):
            return False
    return False


def pointer_vars(string_eas):
    """dword slots in .rdata/.data whose value is one of `string_eas`."""
    out = collections.defaultdict(list)
    for segname in (".rdata", ".data"):
        blob, base = _seg_blob(segname)
        if blob is None:
            continue
        for sea in string_eas:
            needle = struct.pack("<I", sea)
            off = 0
            while True:
                i = blob.find(needle, off)
                if i < 0:
                    break
                if i % 4 == 0:
                    out[sea].append(base + i)
                off = i + 1
    return out


def resolve():
    strs = qualified_strings()
    text, tbase = _seg_blob(".text")
    ptrs = pointer_vars(strs.keys())
    single, ambiguous, unreferenced = [], [], []
    for sea, name in strs.items():
        fns = instruction_refs(sea, text, tbase)
        for v in ptrs.get(sea, []):
            fns |= instruction_refs(v, text, tbase)
        if len(fns) == 1:
            f = next(iter(fns))
            single.append((name, f, idc.get_name(f) or ""))
        elif fns:
            ambiguous.append((name, sorted(fns)))
        else:
            unreferenced.append(name)
    return single, ambiguous, unreferenced


def run(apply=False, only_class=None):
    single, ambiguous, unreferenced = resolve()
    if only_class is not None:
        single = [r for r in single if r[0].startswith(only_class + "::")]
        ambiguous = [r for r in ambiguous if r[0].startswith(only_class + "::")]
    fresh = [(n, f) for n, f, cur in single if cur.startswith("sub_")]
    taken = [(n, f, cur) for n, f, cur in single if not cur.startswith("sub_")]
    print("qualified strings: single-candidate %d (of which unnamed %d), ambiguous %d, unreferenced %d"
          % (len(single), len(fresh), len(ambiguous), len(unreferenced)))
    print("\n-- AMBIGUOUS, renamed by nothing here (two entry points share the string) --")
    for name, fns in sorted(ambiguous):
        print("   %-46s %s" % (name, " ".join("0x%08X" % f for f in fns)))
    print("\n-- ALREADY NAMED, agreement check --")
    mismatch = 0
    for name, f, cur in sorted(taken):
        method = name.split("::")[1]
        flag = "" if method in cur else "   <- name does NOT contain the method"
        if flag:
            mismatch += 1
        print("   %-46s 0x%08X %s%s" % (name, f, cur, flag))
    print("   (%d of %d existing names do not contain the method)" % (mismatch, len(taken)))
    print("\n-- WOULD RENAME --" if not apply else "\n-- RENAMING --")
    done = 0
    for name, f in sorted(fresh):
        new = name.replace("::", "_")
        print("   0x%08X  %s" % (f, new))
        if apply and ida_name.set_name(f, new, ida_name.SN_FORCE):
            done += 1
    if apply:
        print("renamed %d" % done)
    return fresh, ambiguous
