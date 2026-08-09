#!/usr/bin/env python3
"""Put FEAR 2 back into a playable, injected state without a human.

This is the crash-recovery half of the development loop. The mod is designed to be injected and uninjected
against a game that never restarts, but a genuine crash still costs a manual relaunch, a menu click and a
key press -- which is exactly the kind of thing that stalls an unattended session.

WHY IT DOES NOT SYNTHESISE THE MENU CLICK
-----------------------------------------
Every synthetic-input route was measured against the main menu and none of them works (the evidence is in
reversing/REVERSING_LESSONS.md):

  * SendInput moves the cursor and the menu highlight follows it, but no click or key activates anything.
  * The window-message key queue is only drained when buffered input is active, which is false at the menu.
  * Input written into the engine's device array is visible in the engine's own MouseState as a clean press
    and release, and the Scaleform menu ignores it.

What does work is asking the game to do it: gameclient.dll's UI command table is what a menu click dispatches
through, and `Menu.StartCheckpoint` is the row behind "Continue From Last Saved Point".

Note the console command that looks equivalent is not: `LoadCheckpoint` reaches the same dispatcher with a
different mode and refuses to run outside a loaded world, printing "You can only reload a checkpoint from
within the world" -- which is why it appeared to do nothing at the menu.

ONCE A WORLD IS LOADED, SYNTHETIC INPUT STARTS WORKING. The load screen's "press to continue" is consumed by
the game rather than by Scaleform, so a synthetic Space through the same device array does dismiss it.

HOW IT STARTS THE GAME
----------------------
Through `injector.exe --launch`, which gives the game a 4 GB address space, NOT through
`steam://rungameid`. The shipped exe genuinely refuses a direct launch -- SteamStub checks its parent
process -- but the conclusion "so ask Steam to launch it" is wrong: Steam's own launch produces a 2 GB
session, and Injector.cpp:840-845 refuses to fall back to it because that is "the 2 GB configuration that
causes the crashes this exists to prevent". This script used to do exactly what the injector will not,
which meant every cold `ctest` start ran the configuration the LAA launcher exists to avoid. Steam must
still be RUNNING, because the game is parented to it; it is simply never asked to launch anything.

Idempotent: run it any time. If the game is already in-world it verifies and exits.
"""

import argparse
import ctypes
import json
import subprocess
import sys
import time
import urllib.error
import urllib.request
from ctypes import wintypes

VK_SPACE = 32


def get(port, path, timeout=20):
    with urllib.request.urlopen("http://127.0.0.1:%d%s" % (port, path), timeout=timeout) as r:
        return json.loads(r.read().decode("utf-8", "replace"))


def try_get(port, path):
    try:
        return get(port, path)
    except Exception:
        return None


# ---- FINDING PROCESSES WITHOUT SPAWNING ONE --------------------------------------------------
#
# These used to shell out to tasklist.exe, once per poll, twice a second, for up to three minutes.
# That is thousands of process creations for a question the OS answers from a snapshot, and under
# that churn process creation itself starts failing: tasklist began dying with 0xc0000142
# (STATUS_DLL_INIT_FAILED) and each failure raised its own MODAL ERROR DIALOG, so a stalled poll
# loop papered the desktop with them faster than they could be dismissed. A poll must not be able
# to do that.
#
# CreateToolhelp32Snapshot is the same primitive the injector and the fixture runner already use
# (fixture_test_runner.cpp find_pid), it costs no process, and it lets one pass answer for several
# image names at once.
TH32CS_SNAPPROCESS = 0x00000002
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value


class _PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [("dwSize", wintypes.DWORD),
                ("cntUsage", wintypes.DWORD),
                ("th32ProcessID", wintypes.DWORD),
                ("th32DefaultHeapID", ctypes.POINTER(ctypes.c_ulong)),
                ("th32ModuleID", wintypes.DWORD),
                ("cntThreads", wintypes.DWORD),
                ("th32ParentProcessID", wintypes.DWORD),
                ("pcPriClassBase", ctypes.c_long),
                ("dwFlags", wintypes.DWORD),
                ("szExeFile", wintypes.WCHAR * 260)]


def pid_of(names):
    """First pid whose image name matches any of `names` (case-insensitive), or None.

    `names` is ordered and the order is load-bearing for the game: when both the LAA copy and a
    plain Steam-launched instance exist, the caller wants the COPY, because that is the session
    with the 4 GB address space. Same precedence the injector applies in find_game_pid.
    """
    wanted = [n.lower() for n in names]
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

    # DECLARE THE SIGNATURES. ctypes defaults every return type to c_int, which is 32 bits, and a
    # HANDLE on 64-bit Python is 64 -- so an undeclared CreateToolhelp32Snapshot silently TRUNCATES
    # its handle, and the Process32FirstW and CloseHandle that follow are then handed something
    # that is not the snapshot. It fails as "no processes found" rather than as an error, which is
    # indistinguishable here from "the game is not running".
    kernel32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    kernel32.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
    kernel32.Process32FirstW.restype = wintypes.BOOL
    kernel32.Process32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(_PROCESSENTRY32W)]
    kernel32.Process32NextW.restype = wintypes.BOOL
    kernel32.Process32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(_PROCESSENTRY32W)]
    kernel32.CloseHandle.restype = wintypes.BOOL
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]

    snap = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)

    if snap == INVALID_HANDLE_VALUE or snap is None:
        return None

    found = {}
    try:
        entry = _PROCESSENTRY32W()
        entry.dwSize = ctypes.sizeof(_PROCESSENTRY32W)
        ok = kernel32.Process32FirstW(snap, ctypes.byref(entry))
        while ok:
            image = entry.szExeFile.lower()
            if image in wanted and image not in found:
                found[image] = entry.th32ProcessID
            ok = kernel32.Process32NextW(snap, ctypes.byref(entry))
    finally:
        kernel32.CloseHandle(snap)

    for name in wanted:
        if name in found:
            return found[name]
    return None


# THE 4 GB SESSION IS NOT CALLED FEAR2.exe. `injector.exe --launch` runs the game from a
# Large-Address-Aware COPY named FEAR2_laa.exe (Injector.cpp kCopySuffix), so a lookup for
# FEAR2.exe alone does not see the session the launcher just started. That mismatch is not
# hypothetical: it made this script poll for three minutes and report "the game did not appear"
# while the game was on screen, and the fixture then never sent Menu.StartCheckpoint. The mod
# itself has always known better -- sdk::Modules resolves the main image by handle rather than by
# name for exactly this reason (Modules.cpp:81-86).
GAME_IMAGES = ("FEAR2_laa.exe", "FEAR2.exe")


def game_pid():
    return pid_of(GAME_IMAGES)


def game_running():
    return game_pid() is not None


def has_visible_window(pid):
    """Does `pid` own a visible top-level window yet?

    THE READINESS SIGNAL, and it replaces a blind 45-second sleep. That sleep was not arbitrary --
    FEAR2.exe is CEG/SteamStub-wrapped, so its .text is CIPHERTEXT until the stub decrypts it at
    runtime. Inject before that and every pattern scan misses, and because exe patterns latch their
    result forever (deliberately -- the exe is always mapped, so a miss is normally definitive) the
    session is dead for its whole lifetime with no error that says why.

    A visible top-level window is a signal that arrives strictly AFTER the stub has run and the
    engine has initialised its window, and it arrives when it actually happens rather than at a
    fixed pessimistic guess. Measured: the window appears in ~12s where the sleep waited 45.
    """
    import ctypes
    from ctypes import wintypes

    user32 = ctypes.WinDLL("user32", use_last_error=True)
    found = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def cb(hwnd, _):
        owner = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and user32.IsWindowVisible(hwnd):
            # Skip zero-sized helper windows -- splash and IME windows are visible but not the game.
            rect = wintypes.RECT()
            if user32.GetWindowRect(hwnd, ctypes.byref(rect)):
                if (rect.right - rect.left) > 320 and (rect.bottom - rect.top) > 240:
                    found.append(hwnd)
                    return False
        return True

    user32.EnumWindows(cb, 0)
    return bool(found)


def foreground(pid):
    """Bring the game's window to the front.

    THE STARTUP LOADING SCREEN STALLS WHILE THE WINDOW IS IN THE BACKGROUND. Not the level load --
    that one runs fine unfocused, and FocusKeeper already keeps the clock advancing -- but the
    game's INITIAL load, before our payload is even injected, sits there until the window is
    activated. Unattended that never happens because the window stays foreground; it bites when a
    human alt-tabs away in the seconds after launch, which is precisely when they have started a
    recovery and gone to do something else.

    NOT AUTOMATIC, and the first live test is why: the foreground window turned out to be another
    fullscreen game, and Windows refuses a foreground steal in that situation anyway -- but the
    attempt still disturbed it. A recovery script that yanks focus out of whatever someone is
    doing is worse than one that prints an instruction, so this is behind --foreground.
    """
    import ctypes
    from ctypes import wintypes

    user32 = ctypes.WinDLL("user32", use_last_error=True)
    target = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def cb(hwnd, _):
        owner = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and user32.IsWindowVisible(hwnd):
            rect = wintypes.RECT()
            if user32.GetWindowRect(hwnd, ctypes.byref(rect)):
                if (rect.right - rect.left) > 320 and (rect.bottom - rect.top) > 240:
                    target.append(hwnd)
                    return False
        return True

    user32.EnumWindows(cb, 0)

    if not target:
        return False

    hwnd = target[0]

    # SetForegroundWindow is refused for a background process unless the calling thread shares
    # input state with the target's, so attach first. Failure is not fatal -- the game simply waits
    # for a human, which is the behaviour we had before.
    fg = user32.GetForegroundWindow()
    ours = ctypes.windll.kernel32.GetCurrentThreadId()
    theirs = user32.GetWindowThreadProcessId(fg, None)
    user32.AttachThreadInput(ours, theirs, True)
    user32.ShowWindow(hwnd, 9)   # SW_RESTORE
    user32.SetForegroundWindow(hwnd)
    user32.AttachThreadInput(ours, theirs, False)

    # CHECK THE EFFECT, NOT THE RETURN CODE. SetForegroundWindow returned FALSE in the very first
    # live test while the window nevertheless came forward -- the engine's own focus flag flipped
    # to "focused" immediately afterwards. Activation can complete asynchronously and the return
    # value does not promise anything useful, so ask the window manager what actually happened.
    for _ in range(20):
        if user32.GetForegroundWindow() == hwnd:
            return True
        time.sleep(0.05)

    return False


def wait_for_window(wait_s):
    deadline = time.time() + wait_s
    while time.time() < deadline:
        pid = game_pid()
        if pid is not None and has_visible_window(pid):
            return True
        time.sleep(0.5)
    return False


def steam_exe():
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam") as k:
            return winreg.QueryValueEx(k, "SteamExe")[0].replace("/", "\\")
    except Exception:
        return None


def steam_running():
    return pid_of(("steam.exe",)) is not None


def ensure_steam(wait_s=60):
    """Steam must be RUNNING, but it is never asked to start the game.

    `launch_with_laa` parents the game to steam.exe via PROC_THREAD_ATTRIBUTE_PARENT_PROCESS
    (Injector.cpp:544-548) -- without that parent the SteamStub wrapper reports "Application load
    error 5:0000065434", because it checks who started it. So the client has to be up; it just does
    not have to launch anything.

    NOTE WHAT IS NO LONGER HERE: the whole stale-RunningAppID dance. A hard kill used to leave
    Steam believing the game was still running, which made `steam://rungameid` a silent no-op
    FOREVER and cost this project three consecutive 200-second launch timeouts before anyone
    thought to look at the registry. The only fix was to restart Steam. We no longer ask Steam to
    launch anything, so that state cannot block us and the restart is gone with it.
    """
    if steam_running():
        return True

    exe = steam_exe()

    if exe is None:
        print("[resume] Steam is not running and its path is not in the registry")
        return False

    print("[resume] Steam is not running -- starting it (the game is parented to it)")
    subprocess.Popen([exe], close_fds=True)
    deadline = time.time() + wait_s

    while time.time() < deadline:
        if steam_running():
            time.sleep(5)  # the process exists a moment before it can parent anything
            return True
        time.sleep(1)

    return False


def launch_game(injector, wait_s):
    """Through the INJECTOR's own launcher, never `steam://rungameid`.

    The shipped FEAR2.exe genuinely refuses a direct launch -- it is CEG/SteamStub-wrapped and the
    stub checks its parent process. The WRONG conclusion drawn from that true fact, and what this
    function used to do, is "therefore ask Steam to launch it". Steam's own launch produces a 2 GB
    address space, and `injector.exe --launch` exists precisely to avoid that: it copies the exe,
    sets the Large-Address-Aware bit on the COPY, and starts it with steam.exe as the parent, so
    the stub is satisfied and the game gets 4 GB. Injector.cpp:840-845 refuses to fall back to
    Steam for exactly this reason -- "the 2 GB configuration that causes the crashes this exists to
    prevent" -- and this script was quietly doing the thing the injector will not do, on every
    cold ctest start.

    --no-host because a test run must not wake an OpenXR runtime. Loading one starts that vendor's
    services and can raise a firewall prompt, and a plain `ctest` should stay free of side effects
    on someone else's machine. A human who wants the host runs `injector.exe --launch` without
    this flag.

    --launch also INJECTS on success, so a caller must not then inject again.
    """
    if not ensure_steam():
        return False

    r = subprocess.run([injector, "--launch", "--no-host", "--wait", str(wait_s)],
                       capture_output=True, text=True)

    # The injector's own diagnosis is the useful part when this fails -- it names whether the exe
    # was found, whether Steam could be opened for parenting, and whether gameclient.dll came up.
    # The Steam path printed NOTHING on failure, which is what made those timeouts so expensive.
    for line in (r.stdout or "").splitlines():
        if line.strip():
            print("[resume]   %s" % line.rstrip())

    if r.returncode != 0:
        return False

    deadline = time.time() + wait_s

    while time.time() < deadline:
        if game_running():
            return True
        time.sleep(0.5)

    return False


def wait_for_ipc(port, wait_s):
    deadline = time.time() + wait_s
    while time.time() < deadline:
        if try_get(port, "/health") is not None:
            return True
        time.sleep(0.5)
    return False


def inject(injector, wait_s, port):
    subprocess.run([injector, "--inject"], capture_output=True, check=False)
    return wait_for_ipc(port, wait_s)


def wait_for_presents(port, min_presenting_s=3.0, wait_s=120):
    """Wait until the renderer has been CONTINUOUSLY presenting for `min_presenting_s`.

    WHY A WINDOW IS NOT ENOUGH, which is the whole bug. `injector.exe --launch` produces a visible
    top-level window about a SECOND after the process is created, so every window-based wait in
    this script returned immediately and the old code queued Menu.StartCheckpoint at once. Under
    the previous Steam launch the extra startup latency hid this completely -- the delay was
    ACCIDENTAL, not designed, and it vanished the moment the launcher got faster.

    WHAT IS ACTUALLY OBSERVED, and what is not. At a 0s settle the command was queued and
    run_ui_command reported it still unconsumed 60 seconds later. That is the observation. The
    tempting explanation -- "it was queued before the engine presented, and ConsoleRunner drains
    on the present callback" -- does NOT survive arithmetic: presents begin ~13-15s in, so a
    command queued at 0.3s should have been drained long inside that 60s window. Something else
    was true of that run (the process may not have survived; the very first failure of this kind
    took a crash in gameclient with it), and process liveness and rh_frames were NOT sampled
    across the 60s, so the mechanism is genuinely unknown. Do not repeat the plausible story as
    if it were established -- if this matters to you, instrument it rather than inherit it.

    `rh_frames` is RenderHook's count of presents observed since injection. Unlike /health's
    `frame_ticks` (CClientMgr::Update, which does not tick at the menu at all) it advances while
    the front end is up, so it is the one counter here that separates "the engine is running" from
    "a window exists".

    THE FLOOR, MEASURED. Five cold launches, dispatching once at each threshold:
        0s -> FAIL: queued, never consumed in 60s   (0s is degenerate -- it accepts the first
                                                     sample without ever observing an INCREASE)
        2s -> loaded, 22s total
        3s -> loaded, 23s total (twice)
        5s -> loaded, 25s total
    3s is the default: above the smallest value that worked, repeated, and one second is a cheap
    margin. Note the settle is a small tail on the real cost -- the engine takes ~13-15s to reach
    its first present, and that part is irreducible from here.

    This is still a proxy. Presents advancing does not prove the Scaleform front end has finished
    building itself, and nothing this mod exposes today does; a real front-end readiness
    diagnostic would replace this function outright.
    """
    deadline = time.time() + wait_s
    rising_since = None
    last = -1

    while time.time() < deadline:
        sp = try_get(port, "/sdk/shader-params") or {}
        now = sp.get("rh_frames")

        if now is None:
            time.sleep(0.5)
            continue

        if now > last:
            if rising_since is None:
                rising_since = time.time()
        else:
            rising_since = None  # stalled: restart the settle rather than counting a freeze

        last = now

        if rising_since is not None and (time.time() - rising_since) >= min_presenting_s:
            return True

        time.sleep(0.5)

    return False


def run_ui_command(port, cmd, wait_s=60):
    """Queue a UI command and PROVE the engine consumed it, before anything times out on it.

    /console/run reports enough to separate two failures that look identical from outside and have
    OPPOSITE fixes:
      * queued but never CONSUMED -- the runner is not draining, so the engine is not presenting;
        waiting is the answer.
      * consumed and RAN, but nothing happened -- it was dispatched into a front end that was not
        ready; waiting longer will not help, re-issuing might.
    Collapsing both into "the world never loaded" is what sent the last investigation chasing
    window focus for an hour.

    `cmd=` with an empty value is a safe read-only probe: ConsoleRunner::queue rejects an empty
    command line (ConsoleRunner.cpp:50-53) and queues nothing.

    NOTE: `callback_registered` is deliberately NOT used as a readiness gate. ConsoleRunner sets it
    as soon as it attaches its present callback at init, specifically so commands drain both at the
    menu and in play -- so it is true at the same early instant that caused this bug. It is checked
    only to report a runner that never attached at all.

    Returns (ok, detail). `ok` means CONSUMED AND RAN. What the command then achieved is the
    caller's business.
    """
    state = try_get(port, "/console/run?cmd=") or {}

    if not state.get("callback_registered"):
        return False, "the console runner never attached to the frame boundary"

    executed_before = state.get("executed") or 0

    r = try_get(port, "/console/run?cmd=%s" % cmd)
    if r is None or not r.get("ok"):
        return False, "could not queue %s: %s" % (cmd, r)

    deadline = time.time() + wait_s
    while time.time() < deadline:
        time.sleep(0.5)
        state = try_get(port, "/console/run?cmd=") or {}
        if (state.get("executed") or 0) > executed_before:
            if state.get("last_command") == cmd and state.get("last_outcome") == "ran":
                return True, None
            return False, "%s was consumed but the engine reported last_command=%r outcome=%r" % (
                cmd, state.get("last_command"), state.get("last_outcome"))

    return False, ("%s was queued and never consumed in %ds -- the engine is not presenting, so it "
                   "was issued before the front end came up" % (cmd, wait_s))


def is_ready(sp):
    """Test-ready means the world is up AND TIME IS PASSING.

    `ws_world_ready` alone is not enough, and treating it as enough is what let this script report success
    while the game sat on the load screen's "press to continue" prompt with the engine clock paused. Every
    frame-dependent check downstream -- the head-tracking probe, the in-phase samplers, the pass census --
    measures nothing in that state and fails in ways that look like code faults.

    A LIVE PLAYER IS PART OF IT. `ws_world_ready` and a running clock are both true of a world
    containing a corpse, and the suite then fails ~40 checks from one cause: the camera stops
    tracking, the in-detour samplers report nothing to sample, the loadout empties so the weapon
    normalisation finds no firearm, and every one of those reads as a defect somewhere else. This
    exact state cost a full suite run, and the script had reported "already in-world and running --
    nothing to do" immediately beforehand.

    Same lesson the clock check above encodes: the readiness a harness exports must be the
    readiness the tests depend on, or it manufactures false failures faster than it saves setup.
    """
    if not bool(sp.get("ws_world_ready")) or sp.get("eng_clock_paused", True):
        return False
    # ps_alive is absent on builds/states that cannot report it; absent must not read as dead, or
    # this would loop reloading a perfectly good world.
    if "ps_alive" in sp and not sp.get("ps_alive"):
        return False
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--injector", default="build/bin/injector.exe")
    ap.add_argument("--port", type=int, default=8798)
    ap.add_argument("--launch-timeout", type=int, default=180)
    ap.add_argument("--load-timeout", type=int, default=120)
    # How long the renderer must have been continuously presenting before the front end is driven.
    # A knob rather than a constant because it stands in for a readiness signal the mod does not
    # expose yet, and because that is how the default stopped being a guess -- see
    # wait_for_presents() for the five cold launches that fixed it at 3s.
    ap.add_argument("--present-settle", type=float, default=3.0)
    ap.add_argument("--foreground", action="store_true",
                    help="activate the game window after launching (see foreground(); off by "
                         "default because it steals focus from whatever you are using)")
    args = ap.parse_args()

    launched = False

    if not game_running():
        print("[resume] FEAR2 is not running -- launching with a 4 GB address space "
              "(injector --launch)")
        if not launch_game(args.injector, args.launch_timeout):
            print("[resume] FAILED: the game did not appear")
            return 1
        # THE INJECTOR ALREADY WAITED FOR THIS, and for gameclient.dll, before it injected -- see
        # do_launch's two-signal gate. So on the cold path this normally returns immediately. It
        # stays because it is the script's own guarantee that the engine is up before anything
        # below tries to drive its menu, and because `--launch` deliberately degrades to injecting
        # anyway after 180s rather than refusing: if that happened, this is what notices.
        print("[resume] waiting for the engine's window")
        if not wait_for_window(args.launch_timeout):
            print("[resume] FAILED: the game never showed a window")
            return 1
        # OPT-IN ONLY. See foreground() for why this is not automatic.
        if args.foreground and foreground(game_pid()):
            print("[resume] brought the game window forward")
        launched = True

    if try_get(args.port, "/health") is None:
        if launched:
            # `injector --launch` ALREADY INJECTED. Running `--inject` again would be a second
            # attempt against a resident DLL, which the injector correctly refuses -- and that
            # refusal reads like a failure to anyone watching a cold start. IPC simply has not
            # come up yet, so wait for the mod that is already in there.
            print("[resume] waiting for IPC (the launch already injected)")
            if not wait_for_ipc(args.port, 60):
                print("[resume] FAILED: IPC never answered after the launch injected")
                return 1
        else:
            print("[resume] injecting")
            if not inject(args.injector, 60, args.port):
                print("[resume] FAILED: IPC never answered after injection")
                return 1

    sp = try_get(args.port, "/sdk/shader-params") or {}
    if is_ready(sp):
        print("[resume] already in-world and running -- nothing to do")
        return 0

    # A DEAD PLAYER IN A LOADED WORLD is its own case: the world is fine, so Menu.StartCheckpoint
    # (a MENU command) is not the route -- `LoadCheckpoint` is, and it only works from inside a
    # world, which is exactly where we are. AGENTS.md records the mode split: Menu.StartCheckpoint
    # is mode 8, LoadCheckpoint mode 9, and each refuses the other's context.
    if sp.get("ws_world_loaded") and "ps_alive" in sp and not sp.get("ps_alive"):
        print("[resume] in-world but the player is DEAD -- reloading the checkpoint")
        r = try_get(args.port, "/console/run?cmd=LoadCheckpoint")
        if r is None or not r.get("ok"):
            print("[resume] FAILED: could not queue LoadCheckpoint: %s" % r)
            return 1
        # DO NOT RETURN HERE. The reload leaves the same "press to continue" prompt the initial
        # load does, with the engine clock PAUSED -- measured: ps_alive false, eng_clock_paused
        # true, last_outcome "ran". The first version of this branch waited for readiness that
        # could not arrive and reported the reload as failed while it was merely waiting for a
        # key. Fall through to the shared dismissal loop at the bottom, which already handles it.
        deadline = time.time() + args.load_timeout
        while time.time() < deadline:
            time.sleep(1.0)
            sp = try_get(args.port, "/sdk/shader-params") or {}
            if is_ready(sp):
                print("[resume] revived and in-world: %s" % (sp.get("world_name") or "(unnamed)"))
                return 0
            if sp.get("eng_clock_paused"):
                print("[resume] checkpoint reloaded -- dismissing the prompt")
                break
        else:
            print("[resume] FAILED: the checkpoint reload never came back")
            return 1

    if not sp.get("ws_world_loaded"):
        # WAIT FOR THE ENGINE TO BE PRESENTING before touching the front end. See
        # wait_for_presents() -- a visible window arrives ~1s after the LAA launch and means
        # nothing about whether there is a menu to drive.
        settle_started = time.time()
        if not wait_for_presents(args.port, min_presenting_s=args.present_settle):
            print("[resume] FAILED: the renderer never started presenting -- the engine is not "
                  "running frames, so there is no front end to drive")
            return 1
        print("[resume] renderer presenting, settled %.1fs (waited %.1fs)" %
              (args.present_settle, time.time() - settle_started))

        # ONE DISPATCH. An earlier version retried once when the first command was consumed and
        # reported "ran" but no world followed, on the theory that this is the signature of a
        # front end that was not ready. It is not: `ran` only proves the handler was INVOKED, and
        # nothing distinguishes "invoked too early" from "invoked fine, loading is just slow" --
        # so a retry can fire into a world that is already coming up. Until a reproduced run shows
        # a retry is both safe and necessary, this issues the command once and fails with the
        # evidence rather than guessing.
        print("[resume] at the menu -- invoking Menu.StartCheckpoint")
        ok, detail = run_ui_command(args.port, "Menu.StartCheckpoint")

        if not ok:
            print("[resume] FAILED: %s" % detail)
            return 1

        loaded = False
        deadline = time.time() + args.load_timeout
        while time.time() < deadline:
            time.sleep(0.5)
            sp = try_get(args.port, "/sdk/shader-params") or {}
            if sp.get("ws_world_loaded"):
                loaded = True
                break

        if not loaded:
            # STATE THE EVIDENCE, because the two causes need different work and the last
            # investigation lost an hour to guessing between them. The command was consumed and
            # the engine said it ran -- so this is NOT a queueing or liveness problem.
            print("[resume] FAILED: Menu.StartCheckpoint was consumed and the engine reported it "
                  "ran, but no world loaded within %ds" % args.load_timeout)
            print("[resume]   that leaves two candidates, and they are distinguishable by looking:")
            print("[resume]   1. the front end was not ready for it -- raise wait_for_presents()'s")
            print("[resume]      floor, and better, expose a real front-end readiness diagnostic;")
            print("[resume]   2. focus -- the game's INITIAL load screen stalls while its window is")
            print("[resume]      in the background (the level load does not; FocusKeeper covers")
            print("[resume]      that). Click the game once, or pass --foreground.")
            return 1

        print("[resume] world loaded")

    # The load screen waits on a key. Synthetic input reaches the GAME (unlike the menu), so this works --
    # retried because the prompt appears a moment after the world reports loaded.
    deadline = time.time() + 60
    while time.time() < deadline:
        try_get(args.port, "/input/tap?vk=%d&frames=4" % VK_SPACE)
        time.sleep(0.75)
        sp = try_get(args.port, "/sdk/shader-params") or {}
        if is_ready(sp):
            print("[resume] in-world and running: %s" % (sp.get("world_name") or "(unnamed)"))
            return 0

    sp = try_get(args.port, "/sdk/shader-params") or {}
    if sp.get("ws_world_ready"):
        print("[resume] FAILED: in-world but the clock is still paused -- the prompt was never dismissed")
    else:
        print("[resume] world loaded but never became ready -- check the screen")
    return 1


if __name__ == "__main__":
    sys.exit(main())
