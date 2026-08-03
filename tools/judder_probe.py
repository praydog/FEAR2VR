"""Is the projection layer claiming a pose the frame was not rendered from?

WHY THIS EXISTS
---------------
Judder while moving the head has two very different causes and they are easy to confuse:

  1. THE STAMP LIES.  The frame is published with a head-pose sequence it was not drawn with, so
     the compositor reprojects by the difference.  The error is proportional to head speed, and
     every counter on the host still reads a clean hit -- `pose_hits` only proves the host FOUND
     the pose it was told to use, never that the game rendered with it.

  2. THE GAME CANNOT KEEP UP.  Frames are held and re-shown.  The host reuses each frame's OWN
     pose, so the compositor reprojects correctly, but stale content still judders on motion.

This measures (1) directly and reports what it needs to tell it apart from (2).

IT REFUSES TO REPORT ON A DEAD SAMPLE.  An unworn headset publishes no valid pose, the game then
ingests nothing, and the drift count sits at a truthful-looking zero because NOTHING MOVED.  That
shape of false negative has already been reached three times in this project by measuring a game
that was not doing the thing.  So liveness is asserted first, and a sample that fails it is void
rather than green.
"""

import json
import sys
import time
import urllib.request

BASE = "http://127.0.0.1:8798"


def get(path):
    with urllib.request.urlopen(BASE + path, timeout=30) as r:
        return json.loads(r.read().decode("utf-8", "replace"))


def main():
    seconds = float(sys.argv[1]) if len(sys.argv) > 1 else 6.0

    try:
        a = get("/xr/head")
    except Exception as e:  # noqa: BLE001 -- the mod not being injected is the common case
        print("[judder] no IPC on %s -- the mod is not injected" % BASE)
        print("         run:  build/bin/injector.exe --inject")
        return 2

    print("[judder] sampling %.0fs -- MOVE YOUR HEAD steadily the whole time" % seconds)
    time.sleep(seconds)
    b = get("/xr/head")

    d_frames = b.get("frames", 0) - a.get("frames", 0)
    d_updates = b.get("vr_host_updates", 0) - a.get("vr_host_updates", 0)
    d_agree = b.get("stamp_agree", 0) - a.get("stamp_agree", 0)
    d_drift = b.get("stamp_drift", 0) - a.get("stamp_drift", 0)
    d_pub = b.get("fp_frames", 0) - a.get("fp_frames", 0)

    # ---- LIVENESS, BEFORE ANY VERDICT ----------------------------------------------------------
    void = []
    if d_frames <= 0:
        void.append("the host published no frames (xr64.exe not running, or no session)")
    if d_updates <= 0:
        void.append("the game ingested NO new head pose -- put the headset ON; an unworn "
                    "runtime reports no valid pose and nothing can drift")
    if d_agree + d_drift <= 0:
        void.append("no frames were captured and stamped (is the VR capture armed?)")
    if void:
        print("[judder] SAMPLE VOID -- measuring nothing:")
        for v in void:
            print("           - %s" % v)
        return 1

    total = d_agree + d_drift
    pct = 100.0 * d_drift / total
    game_fps = d_pub / seconds
    host_fps = d_frames / seconds

    print("[judder] game %.0f fps published | host %.0f fps | %d frames stamped"
          % (game_fps, host_fps, total))
    print("[judder] head poses ingested: %d" % d_updates)
    print("[judder] STAMP DRIFT: %d of %d (%.1f%%), worst %s sequence steps ahead"
          % (d_drift, total, pct, b.get("stamp_worst_ahead")))
    print("[judder] view thread %s, stamp thread %s%s"
          % (b.get("view_tid"), b.get("stamp_tid"),
             "  (SAME -- no cross-thread window)"
             if b.get("view_tid") == b.get("stamp_tid") else "  (DIFFERENT -- a window exists)"))

    if d_drift > 0:
        print("[judder] -> THE STAMP IS LYING on %.1f%% of frames. The layer claims a pose the "
              "image\n"
              "         was not rendered from, and the compositor corrects by the difference." % pct)
    elif game_fps < host_fps * 0.9:
        print("[judder] -> the stamp is honest, but the game publishes %.0f fps into a %.0f Hz\n"
              "         compositor, so frames are held and re-shown. Stale content judders on\n"
              "         motion even when every pose is correct. This is a PERFORMANCE cause."
              % (game_fps, host_fps))
    else:
        print("[judder] -> the stamp is honest AND the game is keeping up. Look elsewhere: "
              "prediction\n"
              "         (display time vs when the pose was sampled), or the FOV/frustum, not "
              "the pose path.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
