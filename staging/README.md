# staging/ -- what gets placed next to the game

Everything in this directory is copied beside `injector.exe` at build time, and the injector stages
it next to `FEAR2.exe` on `--launch` when it is not already there. It is the one place to put a file
that the GAME must see but that we do not want to keep in the game folder by hand.

```
staging/            committed here, the source of truth
  -> build/bin/     copied by the build (post-build step on the injector target)
  -> the game dir   copied by injector.exe --launch, ONLY when absent
```

## The rules the injector follows

- **It never overwrites.** A file already present next to the game is left exactly as it is -- it may
  be your own build, a different version, or something you are deliberately testing. Silently
  replacing a graphics driver shim is not a thing an injector should do. Delete the file in the game
  folder if you want the staged one to win.
- **It stages before the process is created.** The Windows loader reads `d3d9.dll` out of the exe's
  directory at process start, so staging afterwards would stage it for the *next* run.
- **A missing staging file is silent, a failed copy is loud.** Not having DXVK here is a legitimate
  configuration; failing to place it when it exists is not.

## Why the file has to sit in the game folder at all

FEAR2 **statically imports** `d3d9.dll`, so the loader resolves it before any of our code runs.
Preloading it from the injector's own directory cannot win a race that is already over, and the
alternatives -- a `.local` redirection directory, or running the exe from elsewhere with the game
folder as the working directory -- either need writes in the same place anyway, or risk the engine
resolving its own data relative to the exe. The loader searches the exe's directory first. The file
goes there or it does not get used.

## Contents

### `d3d9.dll` -- DXVK

**This is part of the configuration, not an optimisation.** Same build, same runtime, measured
either side (the full grid is in `AGENTS.md`):

|configuration|result|
|---|---|
|real hardware, native D3D9|50-60 fps|
|real hardware, DXVK|**72 fps** (the cap)|
|Meta XR Simulator, native D3D9|10-20 fps, minutes to load, the whole PC stutters|
|Meta XR Simulator, DXVK|**71.2 fps**, loads normally, no machine-wide lag|

The cause is not the simulator: getting a frame to the 64-bit host costs a `GetRenderTargetData`
readback, and on the native runtime that call BLOCKS until the GPU has drained every prior
operation. DXVK's readback does not stall that way.

So a session without this file will be *misjudged*, not merely slower, and that is why it is
committed rather than fetched -- a CI run that has to reach the network to be correct is a CI run
that is silently wrong when the network is down.

**Provenance, stated honestly:** this is the exact binary every measurement in this repo was taken
with, hash below. Its upstream release version was not recorded at the time and cannot be recovered
from the file -- DXVK sets its version resource to mimic the native runtime (`FileDescription:
Direct3D 9 Runtime`, `ProductVersion: 10.0.17763.1`), so the resource says nothing about which DXVK
build it is. It is unambiguously DXVK (454 `DXVK` strings, `vkCreateInstance`, `DXVK_CONFIG_FILE`,
`dxvk.conf`). If you replace it, record the release tag here and re-run the measurements -- do not
assume a newer build is a faster one for this game.

### `dxvk-LICENSE.txt`

DXVK is **zlib/libpng**: redistribution in binary form is permitted for any purpose, requiring only
that its origin is not misrepresented and that altered versions are marked as such. The notice-
retention clause binds *source* distributions. Shipping this file beside the binary is therefore a
courtesy rather than an obligation, and the acknowledgement it constitutes is, in the licence's own
words, "appreciated but is not required".

We ship an **unmodified** binary. If you ever patch DXVK, the licence requires the altered version be
plainly marked -- say so here and in the filename.

## Verifying

```
python tools/verify_staging.py          check every payload against SHA256SUMS
python tools/verify_staging.py --write  regenerate SHA256SUMS after changing something here
```

Exit status is 0/1, so it drops straight into a CI step. It also flags a file present here but
absent from the manifest, which is the case that matters: an unlisted payload is one the injector
will happily place next to the game with nobody having reviewed it.

**Do not use `sha256sum -c SHA256SUMS` on Windows here.** On the volume this repo lives on it
reports `FAILED` for files whose hash it computes correctly when asked directly -- and returns in
less time than reading a 7.8 MB file would take, so its check path is not reading the file at all.
The manifest itself is the standard format and is fine for real coreutils on a Linux CI runner; it
is only the local `-c` that lies.
