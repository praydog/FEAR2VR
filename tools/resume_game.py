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

Idempotent: run it any time. If the game is already in-world it verifies and exits.
"""

import argparse
import json
import subprocess
import sys
import time
import urllib.error
import urllib.request

STEAM_APPID = 16450  # F.E.A.R. 2: Project Origin
VK_SPACE = 32


def get(port, path, timeout=20):
    with urllib.request.urlopen("http://127.0.0.1:%d%s" % (port, path), timeout=timeout) as r:
        return json.loads(r.read().decode("utf-8", "replace"))


def try_get(port, path):
    try:
        return get(port, path)
    except Exception:
        return None


def game_pid():
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq FEAR2.exe", "/FO", "CSV", "/NH"],
                         capture_output=True, text=True).stdout
    for line in out.splitlines():
        parts = [p.strip('"') for p in line.split('","')]
        if parts and parts[0].lower() == "fear2.exe":
            try:
                return int(parts[1])
            except (IndexError, ValueError):
                pass
    return None


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


def steam_thinks_game_runs():
    """Steam's own idea of what is running, which survives a crash and blocks every relaunch.

    A hard kill -- a crash, a taskkill, a runtime faulting the process -- leaves RunningAppID set to
    our appid with no process behind it. `steam://rungameid` is then a silent no-op FOREVER: Steam
    believes the game is already up, so it does nothing and reports nothing. That cost this project
    three consecutive 200-second launch timeouts before anyone thought to look at the registry.
    """
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam") as k:
            return int(winreg.QueryValueEx(k, "RunningAppID")[0]) == STEAM_APPID
    except Exception:
        return False


def restart_steam(wait_s=60):
    """The only reliable way to clear it: Steam caches the state in memory, so poking the registry
    does nothing. -shutdown exits cleanly and resets RunningAppID to 0 on the way out."""
    exe = steam_exe()

    if exe is None:
        return False

    print("[resume] Steam still thinks FEAR2 is running -- restarting Steam to clear it")
    subprocess.run([exe, "-shutdown"], check=False)
    deadline = time.time() + wait_s

    while time.time() < deadline:
        out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq steam.exe", "/FO", "CSV", "/NH"],
                             capture_output=True, text=True).stdout
        if "steam.exe" not in out:
            break
        time.sleep(1)

    subprocess.Popen([exe], close_fds=True)
    deadline = time.time() + wait_s

    while time.time() < deadline:
        out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq steamwebhelper.exe", "/FO", "CSV",
                              "/NH"], capture_output=True, text=True).stdout
        if "steamwebhelper.exe" in out:
            time.sleep(8)  # the client is up but not yet answering rungameid
            return True
        time.sleep(1)

    return False


def launch_game(wait_s):
    # Through Steam, not the exe: the on-disk binary is CEG/SteamStub-wrapped and refuses a direct launch.
    if steam_thinks_game_runs():
        restart_steam()

    for attempt in range(2):
        subprocess.run(["cmd", "/c", "start", "", "steam://rungameid/%d" % STEAM_APPID], check=False)
        deadline = time.time() + wait_s

        while time.time() < deadline:
            if game_running():
                return True
            time.sleep(0.5)

        # A launch that produced no process at all is the stale-state signature rather than a slow
        # disk, so it is worth one Steam restart before giving up on the whole loop.
        if attempt == 0 and steam_thinks_game_runs() and not restart_steam():
            break

    return False


def inject(injector, wait_s, port):
    subprocess.run([injector, "--inject"], capture_output=True, check=False)
    deadline = time.time() + wait_s
    while time.time() < deadline:
        if try_get(port, "/health") is not None:
            return True
        time.sleep(0.5)
    return False


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
    ap.add_argument("--foreground", action="store_true",
                    help="activate the game window after launching (see foreground(); off by "
                         "default because it steals focus from whatever you are using)")
    args = ap.parse_args()

    if not game_running():
        print("[resume] FEAR2 is not running -- launching through Steam")
        if not launch_game(args.launch_timeout):
            print("[resume] FAILED: the game did not appear")
            return 1
        # WAIT FOR THE WINDOW, not for a fixed interval. See has_visible_window() for why this
        # cannot simply be dropped: injecting before the SteamStub decrypts .text latches every
        # pattern miss permanently.
        print("[resume] waiting for the engine's window")
        if not wait_for_window(args.launch_timeout):
            print("[resume] FAILED: the game never showed a window")
            return 1
        # OPT-IN ONLY. See foreground() for why this is not automatic.
        if args.foreground and foreground(game_pid()):
            print("[resume] brought the game window forward")

    if try_get(args.port, "/health") is None:
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
    # world, which is exactly where we are. AGENT.MD records the mode split: Menu.StartCheckpoint
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
        print("[resume] at the menu -- invoking Menu.StartCheckpoint")
        r = try_get(args.port, "/console/run?cmd=Menu.StartCheckpoint")
        if r is None or not r.get("ok"):
            print("[resume] FAILED: could not queue the UI command: %s" % r)
            return 1
        deadline = time.time() + args.load_timeout
        while time.time() < deadline:
            time.sleep(0.5)
            sp = try_get(args.port, "/sdk/shader-params") or {}
            if sp.get("ws_world_loaded"):
                break
        else:
            print("[resume] FAILED: the world never loaded")
            # THE USUAL CAUSE IS FOCUS, and saying so saves the next person the investigation.
            # The game's INITIAL load screen stalls while its window is in the background --
            # distinct from the level load, which runs fine unfocused because FocusKeeper keeps
            # the clock advancing. Unattended this never happens; it bites when a human alt-tabs
            # in the seconds after launch.
            print("[resume]   if you alt-tabbed just after launch, click the game once -- its")
            print("[resume]   startup load screen waits for the window to be activated.")
            print("[resume]   `--foreground` makes this script do it, at the cost of stealing focus.")
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
