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

    # The pass census lives on a different route from the pose counters.
    a_sp = get("/sdk/shader-params")
    print("[judder] sampling %.0fs -- MOVE YOUR HEAD steadily the whole time" % seconds)
    print("         (use the HEAD only -- turning with the stick puts body yaw in the camera "
          "and not in the head pose)")
    time.sleep(seconds)
    b = get("/xr/head")
    b_sp = get("/sdk/shader-params")

    # `frames` is the SIMULATED RUNTIME's counter, which the mod advances on the game thread -- so
    # it is the game's rate, not the host's. Printing it as "host fps" compared the game against
    # itself and made every beat invisible. The host's own counter comes from the mapping.
    d_frames = b.get("host_frames", 0) - a.get("host_frames", 0)
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
    passes = b_sp.get("cp_passes_last_frame", 0)
    if passes > 2:
        # NOT a differentiator. Measured 3 passes in an ordinary spot as well, so the extra pass is
        # normal here and the earlier wording -- "which is why this spot differs" -- was an
        # assertion dressed as an observation. Reported, not blamed.
        print("[judder] %d camera passes this frame (two eyes plus %d other). Normal in this game; "
              "compare against a good spot before reading anything into it." % (passes, passes - 2))
    if void:
        print("[judder] SAMPLE VOID -- measuring nothing:")
        for v in void:
            print("           - %s" % v)
        return 1

    total = d_agree + d_drift
    pct = 100.0 * d_drift / total
    game_fps = d_pub / seconds
    host_fps = d_frames / seconds

    print("[judder] game %.0f fps published | COMPOSITOR %.0f Hz | %d frames stamped"
          % (game_fps, host_fps, total))
    # ---- IS THE GAME ON A SUBMULTIPLE OF THE COMPOSITOR? ---------------------------------------
    # Anything else beats: 66 into 72 means some display frames get a new image and some get a
    # repeat, on a ~6 Hz cycle, and that cycle IS the judder.
    # ---- IS PACING ACTUALLY IN EFFECT? ---------------------------------------------------------
    # A cadence verdict means nothing if the game is not waiting on the tick at all. Timeouts
    # climbing every frame means the wait is not blocking, and the lock cannot bite however it is
    # configured.
    d_to = b.get("vr_tick_timeouts", 0) - a.get("vr_tick_timeouts", 0)
    d_drop = b.get("ticks_dropped", 0) - a.get("ticks_dropped", 0)
    print("[judder] pacing: paced=%s lock=%s | %d tick timeouts, %d boundaries dropped"
          % (b.get("vr_paced"), b.get("phase_lock"), d_to, d_drop))
    if d_pub > 0 and d_to > d_pub * 0.5:
        print("[judder] -> PACING IS NOT IN EFFECT: the wait timed out on %.0f%% of frames, so the\n"
              "         game is free-running whatever the lock says. Is xr64.exe up the whole time?"
              % (100.0 * d_to / d_pub))

    if host_fps > 1.0:
        ratio = host_fps / max(game_fps, 0.001)
        nearest = max(1, round(ratio))
        off = abs(ratio - nearest) / nearest
        print("[judder] cadence: %.2f compositor frames per game frame (nearest submultiple 1/%d, "
              "%.1f%% off)%s"
              % (ratio, nearest, 100.0 * off,
                 "  phase_lock=%s" % b.get("phase_lock")))
        if off > 0.04:
            print("[judder] -> NOT ON A SUBMULTIPLE. %.0f fps into %.0f Hz beats at ~%.1f Hz: some\n"
                  "         display frames get a new image and some get a repeat, and that cycle is\n"
                  "         the judder. Pacing should settle it on %.0f fps instead of sliding."
                  % (game_fps, host_fps, abs(host_fps - game_fps * nearest),
                     host_fps / nearest))
    print("[judder] head poses ingested: %d" % d_updates)
    print("[judder] STAMP DRIFT: %d of %d (%.1f%%), worst %s sequence steps ahead"
          % (d_drift, total, pct, b.get("stamp_worst_ahead")))
    print("[judder] view thread %s, stamp thread %s%s"
          % (b.get("view_tid"), b.get("stamp_tid"),
             "  (SAME -- no cross-thread window)"
             if b.get("view_tid") == b.get("stamp_tid") else "  (DIFFERENT -- a window exists)"))

    # ---- DID THE VIEW ROTATE BY WHAT THE HEAD ROTATED? -----------------------------------------
    # The sequence check above proves only that we named the right pose RECORD. This asks whether
    # the picture actually turned by the amount the pose claims, which is what timewarp acts on.
    d_rot = b.get("rot_samples", 0) - a.get("rot_samples", 0)
    if d_rot > 0:
        cam = (b.get("rot_sum_cam", 0.0) - a.get("rot_sum_cam", 0.0)) / d_rot
        host = (b.get("rot_sum_host", 0.0) - a.get("rot_sum_host", 0.0)) / d_rot
        miss = (b.get("rot_sum_miss", 0.0) - a.get("rot_sum_miss", 0.0)) / d_rot
        ratio = cam / host if host > 1e-6 else 0.0
        print("[judder] ROTATION per frame over %d moving frames: camera %.3f deg, head %.3f deg"
              % (d_rot, cam, host))
        print("         mismatch %.3f deg/frame (%.0f%% of head motion), worst %.2f deg"
              % (miss, 100.0 * miss / host if host > 1e-6 else 0.0, b.get("rot_worst_deg", 0.0)))
        lag = (b.get("rot_sum_lag", 0.0) - a.get("rot_sum_lag", 0.0)) / d_rot
        print("         against the PREVIOUS frame's head motion: %.3f deg/frame" % lag)
        if lag < miss * 0.6:
            print("[judder] -> THE PICTURE IS A FRAME BEHIND THE POSE STAMPED ON IT. It matches the\n"
                  "         previous frame's head motion (%.3f) far better than this one's (%.3f),\n"
                  "         which is a PHASE error, not a scale one. Timewarp is correcting by a\n"
                  "         difference we introduced." % (lag, miss))
        elif miss > 0.25 * host:
            print("[judder] -> THE VIEW IS NOT TURNING WITH THE HEAD (ratio %.2f). The frame is drawn\n"
                  "         with a rotation the stamped pose does not describe, so timewarp corrects\n"
                  "         by the difference. If you were turning with the STICK, re-run without it\n"
                  "         -- body yaw lands in the camera and not in the head pose." % ratio)
        else:
            print("[judder] -> the view is turning with the head (mismatch %.0f%% of motion, and no\n"
                  "         phase offset). The rotation handed to timewarp is faithful."
                  % (100.0 * miss / host if host > 1e-6 else 0.0))
    else:
        print("[judder] rotation census: no moving frames sampled (hold still? head not tracked?)")

    # ---- DID EVERY MAIN-VIEW FRAME GET ITS SECOND EYE? -----------------------------------------
    # Where a frame contains a second view -- a monitor, a camera feed -- an auxiliary setup can
    # land between the main view's setup and its draw. The replay is then built from, or skipped
    # because of, the wrong pass, and the right half of the split keeps the PREVIOUS frame. That is
    # a stale half image once per affected frame, and it looks like the whole scene juddering.
    d_clob = b.get("cp_pristine_clobbered", 0) - a.get("cp_pristine_clobbered", 0)
    d_draws = b_sp.get("cp_draw_calls", 0) - a_sp.get("cp_draw_calls", 0)
    d_eyes = b_sp.get("cp_second_eye_draws", 0) - a_sp.get("cp_second_eye_draws", 0)
    print("[judder] passes: %d scene draws, %d second-eye replays, CLOBBERED %d"
          % (d_draws, d_eyes, d_clob))
    if d_clob > 0:
        print("[judder] -> THE PRISTINE TRANSFORM WAS OVERWRITTEN on %d frames (%.1f%% of "
              "published).\n"
              "         Each is a frame whose second eye was built from another pass, or skipped --\n"
              "         the right half keeps the previous frame. This is spot-specific by nature."
              % (d_clob, 100.0 * d_clob / max(1, d_pub)))

    # ---- DID THE ENGINE RENDER FROM THE CAMERA WE GAVE IT? -------------------------------------
    # The only NON-CIRCULAR check here. Everything else compares our inputs to each other -- the
    # pose we ingested against the camera we built from it -- and answers 0.000 by construction.
    # This reads the engine's own view matrix back out of its record, inside the pass.
    d_vc = b.get("cp_view_checks", 0) - a.get("cp_view_checks", 0)
    d_vm = b.get("cp_view_mismatch", 0) - a.get("cp_view_mismatch", 0)
    if d_vc > 0:
        print("[judder] engine view matrix: %d checked, DISAGREED on %d (%.1f%%), worst %.3f deg"
              % (d_vc, d_vm, 100.0 * d_vm / d_vc, b.get("cp_view_mismatch_deg", 0.0)))
        if d_vm > 0:
            print("[judder] -> THE ENGINE RENDERED FROM A DIFFERENT CAMERA on %.1f%% of passes.\n"
                  "         The frame is not the pose we stamped on it, and no check of our own\n"
                  "         bookkeeping could see it because our bookkeeping is correct."
                  % (100.0 * d_vm / d_vc))
    else:
        print("[judder] engine view matrix: not sampled (is stereo armed?)")

    # ---- ARE WE PUBLISHING THE SAME PICTURE TWICE? ---------------------------------------------
    # The only theory-free question left. Every counter can read clean while the PIXELS repeat.
    d_dup = b.get("fc_dup_frames", 0) - a.get("fc_dup_frames", 0)
    d_dupmoved = b.get("fc_dup_moved", 0) - a.get("fc_dup_moved", 0)
    print("[judder] duplicate published frames: %d (%.1f%%), of which %d went out under a NEW pose"
          % (d_dup, 100.0 * d_dup / max(1, d_pub), d_dupmoved))
    if d_dupmoved > 0:
        print("[judder] -> STALE PICTURE, FRESH POSE on %d frames (%.1f%% of published). The image\n"
              "         is one already shown and the pose has moved on, so the compositor warps an\n"
              "         old picture to a new head position. That is the judder, and no pose counter\n"
              "         can see it because the pose is correct -- the PIXELS are not."
              % (d_dupmoved, 100.0 * d_dupmoved / max(1, d_pub)))

    # ---- DID THE READBACK THAT FILLS THE FRAME ACTUALLY SUCCEED? -------------------------------
    # A failed GetRenderTargetData leaves the slot holding the PREVIOUS frame's pixels. Published
    # under the current pose that is a stale image wearing a fresh pose -- which is what "it
    # judders to a stale frame" looks like from inside the headset.
    d_rb = b.get("fc_readback_failures", 0) - a.get("fc_readback_failures", 0)
    if d_rb > 0:
        print("[judder] -> READBACK FAILED on %d frames (%.1f%% of published), last hr 0x%08X.\n"
              "         Each one held the previous frame's pixels. Those are now withheld rather\n"
              "         than published, but the failures themselves are the defect."
              % (d_rb, 100.0 * d_rb / max(1, d_pub),
                 b.get("fc_last_readback_hr", 0) & 0xFFFFFFFF))
    else:
        print("[judder] readback: 0 failures")

    # A frame published without a fresh second eye ships half a stale image.
    d_pass = b_sp.get("cp_second_eye_draws", 0) - a_sp.get("cp_second_eye_draws", 0)
    if d_pub > 0 and d_pass > 0 and d_pass < d_pub * 0.98:
        print("[judder] STALE HALF-FRAMES: %d second-eye draws for %d published frames (%.1f%% "
              "short)" % (d_pass, d_pub, 100.0 * (1.0 - d_pass / d_pub)))

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
    # ---- ONE LINE TO DIFF BETWEEN TWO PLACES ---------------------------------------------------
    # Everything measured, compactly, so the same command at a good spot and a bad one can be put
    # side by side. Whatever is actually different will be in here; guessing at it from one sample
    # has not worked.
    print("[diff] fps=%.0f hz=%.0f cad=%.2f | drift=%d stale=%d | dup=%d/%d rb=%d | "
          "passes=%d draws=%d eyes=%d clob=%d | rot=%.3f/%.3f miss=%.3f | poses=%d"
          % (game_fps, host_fps, host_fps / max(game_fps, 0.001),
             d_drift, d_dupmoved,
             d_dup, d_dupmoved, d_rb,
             b_sp.get("cp_passes_last_frame", 0),
             b_sp.get("cp_draw_calls", 0) - a_sp.get("cp_draw_calls", 0),
             b_sp.get("cp_second_eye_draws", 0) - a_sp.get("cp_second_eye_draws", 0),
             b.get("cp_pristine_clobbered", 0) - a.get("cp_pristine_clobbered", 0),
             cam if d_rot else 0.0, host if d_rot else 0.0, miss if d_rot else 0.0,
             d_updates)
          + " | viewchk=%d/%d worst=%.3f" % (d_vm, d_vc, b.get("cp_view_mismatch_deg", 0.0)))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
