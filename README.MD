# FEAR2VR

VR mod for F.E.A.R. 2: Project Origin

## Features

* Full stereoscopic 3D
* Head tracking
* Roomscale movement
* 6DOF hand tracking/weapon aiming
* World-space HUD

## Installation

TODO

## Usage of AI

AI was used heavily in development of this mod. Used as a fresh experimentation sandbox for me to see what I could do.

I directed the project, made the architectural decisions, defined the testing and verification standards, and kept development grounded in the behavior of the live game. AI was used to explore implementations, reverse-engineer systems, write code, run experiments, and iterate on failures.

There were cases where AI was going down a wrong/naive path and I had to bring it back and point it in the right direction. 

Some examples being:

#### Camera judder

* I had an issue with there being camera judder in some areas, that I thought we had solved
* I gave the AI several symptoms, game looked okay, headset rotation was only jittery in some scenarios
* The AI constructed several fixes and hypotheses that didn't work or correctly diagnose the issue
* I got frustrated and investigated the code, and told it to start from first principles, meaning I asked:
    * Where on the mod/game side is headset rotation is being obtained from the XR bridge?
    * How is that rotation being applied to the game?
    * How is that rotation being passed/synchronized to the XR side (what pose did we used to render that frame)?
    * How is that rotation then being consumed by the XR side?
* It found that we were simply passing a sequence number corresponding to pose histories
* I then made the decision to simply pass the pose directly instead of a desynchronized index which fixed the issue

### Naive approach to camera movement

* The AI constructed an enormous pipeline for directing camera movement via injecting mouse inputs with the right sensitivity to perform head tracking
* I noticed this and pointed it towards modifying the render/aim camera directly
* It quickly pivoted and correctly solved the issue
## Interesting implementation details

### 4 GB address space

F.E.A.R. 2 is a 32-bit game that normally runs without the `LARGE_ADDRESS_AWARE` flag, limiting it to roughly 2 GB of user-mode virtual address space. With the VR mod loaded, the process approached that limit during normal play. Memory fragmentation left very little contiguous address space available, causing allocations to fail and producing misleading crashes in otherwise unrelated systems, including rendering and audio.

Simply enabling the flag on `FEAR2.exe` was not sufficient because SteamStub validates the executable and rejects a modified image.

To work around this, the launcher creates an LAA-enabled copy of the executable and starts it suspended. Windows observes the flag during process creation and grants the game a 4 GB address space. Before SteamStub begins validation, a temporary entry-point stub restores the process-visible executable path, module identity, and mapped PE characteristics to those of the original executable.

The launcher verifies that the restoration succeeded before allowing execution to continue, then restores the original entry-point bytes. This gives the game the additional address space required by the VR mod without modifying the shipped executable on disk.
