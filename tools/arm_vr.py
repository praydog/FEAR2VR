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
import subprocess
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
    ("hands driven by controllers", "/xr/hands?on=1"),
    ("gun follows the controller", "/vr/viewmodel?on=1"),
    ("stick locomotion + snap turn", "/xr/capture?locomotion=1"),
    ("trigger fires the weapon", "/xr/trigger?on=1"),
    ("shots leave the muzzle", "/xr/fire-muzzle?on=1"),
    ("player body hidden", "/xr/capture?hide_body=1"),
    ("head tracking applied", "/xr/enable?on=1"),
    ("recenter", "/xr/capture?recenter=1"),
]

DISARM = [
    ("head tracking applied", "/xr/enable?on=0"),
    ("player body hidden", "/xr/capture?hide_body=0"),
    ("shots leave the muzzle", "/xr/fire-muzzle?on=0"),
    ("trigger fires the weapon", "/xr/trigger?on=0"),
    ("stick locomotion + snap turn", "/xr/capture?locomotion=0"),
    ("gun follows the controller", "/vr/viewmodel?on=0"),
    ("hands driven by controllers", "/xr/hands?on=0"),
    ("roomscale moves the body", "/xr/capture?roomscale_body=0"),
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
    ("hands", "hands driven by controllers"),
    ("vr_locomotion", "stick locomotion + snap turn"),
    ("trigger_armed", "trigger fires the weapon"),
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
    #
    # BOTH DIRECTIONS ARE SAMPLED, because they fail independently and each looks like the other's
    # problem from inside a headset. `fp_frames` is the game WRITING pixels to the host;
    # `hands_frames` is the game READING controllers back from it. A session was spent on a gun that
    # would not track while pixels flowed perfectly and the mod dutifully re-applied one stale pose
    # forever -- every switch green, every counter that anybody looked at climbing.
    before = get("/xr/head")
    time.sleep(1.0)
    after = get("/xr/head")

    def moved(key):
        return (after.get(key) or 0) - (before.get(key) or 0)

    pixels = moved("fp_frames")
    hands = moved("hands_frames")
    poses = moved("vr_hand_updates")

    print("  [%s] frames actually flowing (%d in 1s)" % ("x" if pixels > 0 else " ", pixels))

    # THE HOST'S OWN LIVENESS, because everything else can read perfectly while it is dead: the mod
    # publishes into a mapping nobody consumes and every switch stays green. That exact state has
    # happened twice, once because the host was killed for a rebuild and never restarted, and once
    # because it was launched with `--seconds 7200` and quietly hit its own deadline mid-session.
    # (`--seconds` defaults to 0, which means no limit. Do not pass it.)
    try:
        out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq xr64.exe", "/FO", "CSV", "/NH"],
                             capture_output=True, text=True, timeout=10).stdout
        host_up = "xr64.exe" in out
    except Exception:
        host_up = None
    if host_up is False:
        print("  [ ] xr64.exe is NOT RUNNING -- nothing is reading the frames or publishing poses")
    elif host_up:
        print("  [x] xr64.exe alive")

    # Only meaningful once the controllers are bound: with the headset off the runtime leaves
    # FOCUSED and the host correctly publishes nothing, which is not a fault to report as one.
    bound = bool(after.get("hands_profile_bound"))
    live = hands > 0 and poses > 0

    if bound:
        print("  [%s] controller poses arriving (%d blocks, %d consumed in 1s)"
              % ("x" if live else " ", hands, poses))
        if not live:
            # ORDERED BY WHAT IT ACTUALLY TURNED OUT TO BE, twice. "Restart the host" used to be the
            # advice here and it was wrong: reading the shared mapping from a third process showed
            # the game and an outsider advancing in lockstep, so the mapping was never stale and the
            # restart fixed nothing. The headset was simply unworn both times.
            print("      ^ no new controller blocks. In order of likelihood:")
            print("        1. the headset is not being WORN -- the runtime leaves FOCUSED and the")
            print("           host correctly publishes nothing. hands_profile_bound stays true from")
            print("           the last session, so it is NOT a liveness signal.")
            print("        2. xr64.exe is not running -- see the host line above.")
            print("        (the mapping itself has never once been the cause)")
    else:
        print("  [ ] controller poses arriving -- no interaction profile bound")
        print("      ^ put the headset ON and wake the controllers; the runtime leaves FOCUSED")
        print("        while it is unworn and the host then publishes nothing by design.")

    return ok and pixels > 0 and (not bound or live)


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
