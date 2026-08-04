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
