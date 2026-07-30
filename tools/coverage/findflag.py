#!/usr/bin/env python3
"""Find which byte -- and which bit -- actually moves when the player performs an action.

Every named bit in this SDK started as an offset read out of a decompiler. Static reading can say "this dword
is tested against 0x20 on the crouch path"; it cannot say the dword is the one a live game updates, nor that
0x20 is the bit rather than a neighbour that happens to be clear. When a coverage run reports that crouching
was never observed, there are two explanations -- the player did not crouch, or the offset is wrong -- and
only watching the memory move under a deliberate action separates them.

So: poll a raw window while somebody performs the action repeatedly, then report every byte that changed and
every bit that toggled. A bit that flips once per action is the flag. A bit that never moves is not.

    python tools/coverage/findflag.py --seconds 45

Requires the payload injected (build/bin/injector.exe --inject).
"""

from __future__ import annotations

import argparse
import json
import sys
import time
import urllib.error
import urllib.request

DEFAULT_PORT = 8798

# The windows the DLL exposes, and the offset each one's mapped flag is claimed to sit at. The claim is what
# is under test: if the byte at that offset never moves while a neighbour does, the mapping is wrong.
WINDOWS = [
    ("pcam_window", "pcam_window_at", "pcam_window_hex", 768, {1005: "aim selector (claimed)"}),
    ("mm_window", "mm_window_at", "mm_window_hex", 272, {296: "flags dword (claimed, bit 0x20 = crouch)"}),
]


def fetch(port: int, path: str, timeout: float = 8.0):
    with urllib.request.urlopen(f"http://127.0.0.1:{port}{path}", timeout=timeout) as r:
        return json.loads(r.read().decode("utf-8", "replace"))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--seconds", type=float, default=45.0)
    ap.add_argument("--interval", type=float, default=0.15)
    ap.add_argument(
        "--max-values",
        type=int,
        default=6,
        help="a byte with more distinct values than this is noise (a float, a timer), not a flag",
    )
    args = ap.parse_args()

    try:
        fetch(args.port, "/health")
    except (urllib.error.URLError, OSError, TimeoutError) as e:
        print(f"IPC not reachable on 127.0.0.1:{args.port} ({e}) -- inject the payload first")
        return 2

    # per window: offset -> set of observed byte values, and offset -> transition count
    seen: dict[str, dict[int, set]] = {name: {} for name, _, _, _, _ in WINDOWS}
    flips: dict[str, dict[int, int]] = {name: {} for name, _, _, _, _ in WINDOWS}
    prev: dict[str, bytes] = {}
    base_at: dict[str, int] = {}
    samples = 0
    scalars: dict[str, set] = {"mv_flags": set(), "aim_flag_value": set(), "mv_crouching": set()}

    deadline = time.monotonic() + args.seconds
    print(f"watching for {args.seconds:.0f}s -- perform the action repeatedly now")
    while time.monotonic() < deadline:
        try:
            d = fetch(args.port, "/sdk/shader-params")
        except (urllib.error.URLError, OSError, TimeoutError, json.JSONDecodeError):
            time.sleep(args.interval)
            continue
        samples += 1
        for k in scalars:
            if k in d:
                scalars[k].add(d[k])
        for name, at_key, hex_key, base, _claims in WINDOWS:
            hx = d.get(hex_key)
            if not isinstance(hx, str) or not hx:
                continue
            base_at[name] = int(d.get(at_key) or 0)
            try:
                cur = bytes.fromhex(hx)
            except ValueError:
                continue
            for i, b in enumerate(cur):
                seen[name].setdefault(base + i, set()).add(b)
            if name in prev and len(prev[name]) == len(cur):
                for i, (a, b) in enumerate(zip(prev[name], cur)):
                    if a != b:
                        flips[name][base + i] = flips[name].get(base + i, 0) + 1
            prev[name] = cur
        time.sleep(args.interval)

    print(f"\n{samples} samples\n")
    for name, _at_key, _hex_key, _base, claims in WINDOWS:
        at = base_at.get(name, 0)
        print(f"=== {name} @ 0x{at:08X} ===")
        moved = {off: vals for off, vals in seen[name].items() if len(vals) > 1}
        if not moved:
            print("  nothing in this window changed at all")
        for off in sorted(moved):
            vals = sorted(moved[off])
            n = flips[name].get(off, 0)
            claim = claims.get(off)
            if len(vals) > args.max_values:
                desc = f"{len(vals)} distinct values (noisy -- not a flag)"
            else:
                desc = "values " + ",".join(f"0x{v:02X}" for v in vals)
                # BIT-LEVEL, which is the whole point for a flags byte: a flag is one bit that toggles, and a
                # counter is several bits that all move together.
                bits = 0
                for v in vals:
                    bits |= v ^ vals[0]
                if bits:
                    desc += f"  bits changed: 0x{bits:02X}"
            tag = f"   <- {claim}" if claim else ""
            print(f"  +{off:<5} {desc}  ({n} transitions){tag}")
        # And say explicitly whether the claimed offset held still, since that is the question.
        for off, claim in claims.items():
            if off in seen[name] and len(seen[name][off]) == 1:
                only = next(iter(seen[name][off]))
                print(f"  +{off} DID NOT MOVE (constant 0x{only:02X}) -- {claim}")
            elif off not in seen[name]:
                print(f"  +{off} not inside the window -- {claim}")
        print()

    print("=== scalars ===")
    for k, vals in scalars.items():
        if not vals:
            print(f"  {k:<18} absent")
        else:
            shown = sorted(vals, key=lambda v: (isinstance(v, bool), v))
            print(f"  {k:<18} {len(vals)} distinct: {shown[:8]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
