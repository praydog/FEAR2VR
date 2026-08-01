"""Put the mod into the full VR configuration, in one command.

WHY THIS EXISTS. The VR path is now eight independent switches, and every one of them is off after a
fresh inject -- which is correct, since none of them should turn themselves on in a flatscreen game.
But it means that re-injecting during development silently disarms the mod, and the failure mode is
confusing rather than obvious: the headset still tracks (the host is a separate process and never
stopped), while the game's picture never arrives, so the wearer sees the host's colour test pattern
and reasonably concludes the pixel path broke.

That happened. Twice. Re-arming from memory is how, because "everything I changed" is not the same
list as "everything that has to be on".

    python tools/arm_vr.py              arm everything
    python tools/arm_vr.py --status     report without changing anything
    python tools/arm_vr.py --off        back to flatscreen
"""

import argparse
import json
import sys
import time
import urllib.request

PORT = 8798

# Ordered deliberately: stereo and publishing FIRST, because the host cannot show anything until
# frames arrive, and everything below only shapes frames that already exist.
ARM = [
    ("stereo split rendering", "/stereo/eye?both=1&split=1&centre_x=0&centre_y=0&half_ipd=3.2"),
    ("frame publishing", "/xr/capture?stage=second_eye&divisor=1&publish=1"),
    ("head pose from the host", "/xr/capture?host_pose=1"),
    ("runtime frame pacing", "/xr/capture?paced=1"),
    ("aim levelling", "/xr/capture?level_aim=1"),
    ("eye offset pinning", "/xr/capture?pin_eye=1"),
    ("roomscale", "/xr/capture?roomscale=1"),
    ("roomscale moves the body", "/xr/capture?roomscale_body=1"),
    ("player body hidden", "/xr/capture?hide_body=1"),
    ("head tracking applied", "/xr/enable?on=1"),
    ("recenter", "/xr/capture?recenter=1"),
]

DISARM = [
    ("head tracking applied", "/xr/enable?on=0"),
    ("player body hidden", "/xr/capture?hide_body=0"),
    ("roomscale moves the body", "/xr/capture?roomscale_body=0"),
    ("roomscale", "/xr/capture?roomscale=0"),
    ("eye offset pinning", "/xr/capture?pin_eye=0"),
    ("aim levelling", "/xr/capture?level_aim=0"),
    ("runtime frame pacing", "/xr/capture?paced=0"),
    ("head pose from the host", "/xr/capture?host_pose=0"),
    ("frame publishing", "/xr/capture?publish=0"),
    ("stereo split rendering", "/stereo/eye?both=0&eye=off&half_ipd=0&split=0"),
]

# Field -> what it means, so a status line names the thing rather than the flag.
CHECKS = [
    ("fp_publishing", "frames published to the host"),
    ("vr_host_pose", "head pose from the host"),
    ("vr_paced", "paced by the runtime"),
    ("enabled", "head tracking applied to the camera"),
    ("vr_level_aim", "aim levelled"),
    ("vr_pin_eye", "eye offset pinned"),
    ("vr_roomscale", "roomscale"),
    ("vr_hide_body", "player body hidden"),
    ("vr_room_body", "roomscale moves the body"),
]


def get(path):
    with urllib.request.urlopen("http://127.0.0.1:%d%s" % (PORT, path), timeout=30) as r:
        return json.loads(r.read().decode("utf-8", "replace"))


def status():
    head = get("/xr/head")
    stereo = get("/stereo/state")
    ok = True

    for key, label in CHECKS:
        on = bool(head.get(key))
        ok = ok and on
        print("  [%s] %s" % ("x" if on else " ", label))

    split = bool(stereo.get("stereo")) and bool(stereo.get("split_viewport"))
    ok = ok and split
    print("  [%s] stereo split rendering" % ("x" if split else " "))

    # A count that does not move means the switch is on and the path is still dead, which is a
    # completely different problem from a switch being off.
    before = head.get("fp_frames") or 0
    time.sleep(1.0)
    after = get("/xr/head").get("fp_frames") or 0
    moving = after > before
    print("  [%s] frames actually flowing (%d in 1s)" % ("x" if moving else " ", after - before))
    return ok and moving


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--status", action="store_true", help="report without changing anything")
    ap.add_argument("--off", action="store_true", help="return to flatscreen")
    args = ap.parse_args()

    try:
        get("/health")
    except Exception as e:
        print("[arm] the mod is not answering on port %d: %s" % (PORT, e))
        return 2

    if args.status:
        return 0 if status() else 1

    steps = DISARM if args.off else ARM

    for label, route in steps:
        try:
            get(route)
            print("  %-28s %s" % (label, "off" if args.off else "on"))
        except Exception as e:
            print("  %-28s FAILED: %s" % (label, e))

    if args.off:
        return 0

    time.sleep(1.5)
    print("[arm] verifying:")
    return 0 if status() else 1


if __name__ == "__main__":
    sys.exit(main())
