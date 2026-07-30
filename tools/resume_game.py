#!/usr/bin/env python3
"""Put FEAR 2 back into a playable, injected state without a human.

This is the crash-recovery half of the development loop. The mod is designed to be injected and uninjected
against a game that never restarts, but a genuine crash still costs a manual relaunch, a menu click and a
key press -- which is exactly the kind of thing that stalls an unattended session.

WHY IT DOES NOT SYNTHESISE THE MENU CLICK
-----------------------------------------
Every synthetic-input route was measured against the main menu and none of them works (the evidence is in
reversing/MAPPING_WORKFLOW.MD):

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


def game_running():
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq FEAR2.exe", "/FO", "CSV", "/NH"],
                         capture_output=True, text=True).stdout
    return "FEAR2.exe" in out


def launch_game(wait_s):
    # Through Steam, not the exe: the on-disk binary is CEG/SteamStub-wrapped and refuses a direct launch.
    subprocess.run(["cmd", "/c", "start", "", "steam://rungameid/%d" % STEAM_APPID], check=False)
    deadline = time.time() + wait_s
    while time.time() < deadline:
        if game_running():
            return True
        time.sleep(2)
    return False


def inject(injector, wait_s, port):
    subprocess.run([injector, "--inject"], capture_output=True, check=False)
    deadline = time.time() + wait_s
    while time.time() < deadline:
        if try_get(port, "/health") is not None:
            return True
        time.sleep(1)
    return False


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--injector", default="build/bin/injector.exe")
    ap.add_argument("--port", type=int, default=8798)
    ap.add_argument("--launch-timeout", type=int, default=180)
    ap.add_argument("--load-timeout", type=int, default=120)
    args = ap.parse_args()

    if not game_running():
        print("[resume] FEAR2 is not running -- launching through Steam")
        if not launch_game(args.launch_timeout):
            print("[resume] FAILED: the game did not appear")
            return 1
        # The engine needs to reach the menu before the payload has anything to bind to.
        print("[resume] waiting for the engine to come up")
        time.sleep(45)

    if try_get(args.port, "/health") is None:
        print("[resume] injecting")
        if not inject(args.injector, 60, args.port):
            print("[resume] FAILED: IPC never answered after injection")
            return 1

    sp = try_get(args.port, "/sdk/shader-params") or {}
    if sp.get("ws_world_ready"):
        print("[resume] already in-world -- nothing to do")
        return 0

    if not sp.get("ws_world_loaded"):
        print("[resume] at the menu -- invoking Menu.StartCheckpoint")
        r = try_get(args.port, "/console/run?cmd=Menu.StartCheckpoint")
        if r is None or not r.get("ok"):
            print("[resume] FAILED: could not queue the UI command: %s" % r)
            return 1
        deadline = time.time() + args.load_timeout
        while time.time() < deadline:
            time.sleep(3)
            sp = try_get(args.port, "/sdk/shader-params") or {}
            if sp.get("ws_world_loaded"):
                break
        else:
            print("[resume] FAILED: the world never loaded")
            return 1
        print("[resume] world loaded")

    # The load screen waits on a key. Synthetic input reaches the GAME (unlike the menu), so this works --
    # retried because the prompt appears a moment after the world reports loaded.
    deadline = time.time() + 60
    while time.time() < deadline:
        try_get(args.port, "/input/tap?vk=%d&frames=4" % VK_SPACE)
        time.sleep(2)
        sp = try_get(args.port, "/sdk/shader-params") or {}
        if sp.get("ws_world_ready"):
            print("[resume] in-world: %s" % (sp.get("world_name") or "(unnamed)"))
            return 0

    print("[resume] world loaded but never became ready -- check the screen")
    return 1


if __name__ == "__main__":
    sys.exit(main())
