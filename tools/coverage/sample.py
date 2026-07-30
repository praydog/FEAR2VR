#!/usr/bin/env python3
"""Harvest state coverage from a live FEAR2 session.

Several suite checks can only discriminate in states the player has to CREATE: standing still, crouching,
aiming down sights, a camera clamp actually engaging. A fixture run lasts ~30s and samples each field once,
so it sees whatever state the player happened to be in -- and a conditional check passes for free on the
permissive branch, which looks exactly like a verified one.

This polls the injected DLL while somebody plays and records, per tracked state, whether it was ever
OBSERVED. The output is a coverage table: what a play session actually exercised, and what it did not.

    python tools/coverage/sample.py --seconds 90

Requires the payload injected and the IPC live (build/bin/injector.exe --inject).
"""

from __future__ import annotations

import argparse
import json
import sys
import time
import urllib.error
import urllib.request

DEFAULT_PORT = 8798


def fetch(port: int, path: str, timeout: float = 8.0):
    url = f"http://127.0.0.1:{port}{path}"
    with urllib.request.urlopen(url, timeout=timeout) as r:
        return json.loads(r.read().decode("utf-8", "replace"))


# Each target is (label, predicate over the sample, why it matters). A target is COVERED once the predicate
# has been true at least once. Predicates return None when the field is absent, which is distinct from False:
# a missing field means the sampler could not tell, not that the state did not happen.
def _b(d, k):
    v = d.get(k)
    return v if isinstance(v, bool) else None


def _f(d, k):
    v = d.get(k)
    return float(v) if isinstance(v, (int, float)) and not isinstance(v, bool) else None


TARGETS = [
    (
        "player STATIONARY",
        lambda d: (lambda m: None if m is None else not m)(_b(d, "mv_moving")),
        "the cached-position, camera-pose and physics-velocity equality checks only discriminate at rest",
    ),
    (
        "player MOVING",
        lambda d: _b(d, "mv_moving"),
        "proves the cache trails by construction rather than being broken",
    ),
    (
        "player CROUCHING",
        lambda d: _b(d, "mv_crouching"),
        "flips CMoveMgr+296 & 0x20 and should select the CrouchIdle/CrouchMoving clamp (42/85)",
    ),
    (
        "aim flag SET (ADS)",
        lambda d: _b(d, "aim_flag_value"),
        "the ONLY evidence that +224 selects the zoomed limit is that 65 < 70 -- flipping it live is proof",
    ),
    (
        "aim flag CLEAR",
        lambda d: (lambda v: None if v is None else not v)(_b(d, "aim_flag_value")),
        "the other half of the selector",
    ),
    (
        "clamp ENGAGED",
        lambda d: _b(d, "pitch_corrected"),
        "a clamp that never fires leaves its bound unverified; engaging pins it to a database number",
    ),
    (
        "clamp recovery timer ACTIVE",
        lambda d: _b(d, "pitch_timer_active"),
        "distinguishes the hard clamp from the interpolated one",
    ),
    (
        "engine clock ADVANCING",
        lambda d: (lambda s: None if s is None else s != _STATE.get("first_engine_seconds"))(
            _f(d, "engine_seconds")
        ),
        "unfocused freezes simulation, so every camera/shader reading is a stale snapshot",
    ),
    (
        "renderer state NON-ZERO",
        lambda d: (lambda r: None if r is None else r != 0.0)(_f(d, "renderer_state")),
        "state 0 is observed but unexplained; 1/2/3/4 are idle/frame/target/pass",
    ),
    (
        "camera pose MATCHES object",
        lambda d: _b(d, "pmgr_camera_rot_matches"),
        "the strong form of the pose check",
    ),
    (
        "cached position MATCHES engine",
        lambda d: _b(d, "ms_position_matches_engine"),
        "the strong form of the cache check",
    ),
    (
        "physics velocity READS ZERO",
        lambda d: _b(d, "pe_velocity_zero"),
        "the store is only unconditional for acceleration; this one needs standing still",
    ),
]

_STATE: dict = {}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--seconds", type=float, default=60.0)
    ap.add_argument("--interval", type=float, default=0.4)
    ap.add_argument(
        "--until-covered",
        action="store_true",
        help="stop as soon as every target has been observed, instead of running the full duration",
    )
    args = ap.parse_args()

    try:
        fetch(args.port, "/health")
    except (urllib.error.URLError, OSError, TimeoutError) as e:
        print(f"IPC not reachable on 127.0.0.1:{args.port} ({e}) -- inject the payload first")
        return 2

    covered: dict[str, bool] = {label: False for label, _, _ in TARGETS}
    unknown: dict[str, bool] = {label: True for label, _, _ in TARGETS}
    samples = 0
    errors = 0
    seen_states: set = set()
    seen_records: set = set()
    seen_renderer: set = set()
    speed_max = 0.0

    deadline = time.monotonic() + args.seconds
    print(f"sampling for {args.seconds:.0f}s at {args.interval:.1f}s intervals -- play the game now")
    while time.monotonic() < deadline:
        try:
            d = fetch(args.port, "/sdk/shader-params")
        except (urllib.error.URLError, OSError, TimeoutError, json.JSONDecodeError):
            errors += 1
            time.sleep(args.interval)
            continue
        samples += 1
        if "first_engine_seconds" not in _STATE:
            _STATE["first_engine_seconds"] = _f(d, "engine_seconds")

        for label, pred, _why in TARGETS:
            v = pred(d)
            if v is None:
                continue
            unknown[label] = False
            if v:
                covered[label] = True

        st = d.get("cc_state")
        if isinstance(st, (int, float)):
            seen_states.add(int(st))
        rn = d.get("cc_record_name")
        if isinstance(rn, str) and rn:
            seen_records.add(rn)
        rs = _f(d, "renderer_state")
        if rs is not None:
            seen_renderer.add(int(rs))
        sp = _f(d, "mv_speed")
        if sp is not None:
            speed_max = max(speed_max, sp)

        # EARLY EXIT once nothing is left to observe. The game runs in exclusive fullscreen, so whoever is
        # playing cannot read a prompt without alt-tabbing -- which freezes the simulation and defeats the
        # measurement. They work from a memorised list at their own pace; the sampler decides when it is done.
        if args.until_covered and all(covered[l] or unknown[l] for l, _, _ in TARGETS):
            print("all targets observed -- stopping early")
            break

        time.sleep(args.interval)

    print(f"\n{samples} samples ({errors} failed)\n")
    width = max(len(l) for l, _, _ in TARGETS)
    missing = []
    for label, _pred, why in TARGETS:
        if unknown[label]:
            mark, tail = "?", "  (field absent -- sampler could not tell)"
        elif covered[label]:
            mark, tail = "+", ""
        else:
            mark, tail = "-", f"  <- {why}"
            missing.append((label, why))
        print(f"  [{mark}] {label:<{width}}{tail}")

    print(f"\n  camera states observed   : {sorted(seen_states)}")
    print(f"  clamp records observed   : {sorted(seen_records)}")
    print(f"  renderer states observed : {sorted(seen_renderer)}")
    print(f"  peak speed               : {speed_max:.1f}")

    if missing:
        print(f"\n{len(missing)} state(s) NOT exercised -- checks depending on them proved nothing:")
        for label, why in missing:
            print(f"  {label}: {why}")
    else:
        print("\nevery tracked state was exercised")
    return 0


if __name__ == "__main__":
    sys.exit(main())
