# FEAR2 engine notes -- what is established about the game

What this project has MEASURED about F.E.A.R. 2 itself: the view chain, the renderer, weapons,
the player, input, and the engine's own clocks. Domain knowledge, not procedure.

- The recipe for producing more of it: `MAPPING_WORKFLOW.MD`.
- The craft, and the ways it goes wrong: `REVERSING_LESSONS.md`.
- Field-by-field layout: `fear2.genny` (the schema is the source of truth for offsets).

Sections are in the order they were found, so a later retraction sits below the claim it corrects.
**Read a subsystem to its end before acting on anything in it** -- several conclusions here were
overturned one section later, and the correction is kept rather than the mistake deleted.

## Contents

- [The view chain](#the-view-chain) -- 11 findings
- [Rendering and stereo](#rendering-and-stereo) -- 5 findings
- [Weapons, hands and attachments](#weapons-hands-and-attachments) -- 11 findings
- [Player state, aim and zoom](#player-state-aim-and-zoom) -- 4 findings
- [Input and locomotion](#input-and-locomotion) -- 4 findings
- [World and units](#world-and-units) -- 1 finding
- [Focus, timing and the engine clock](#focus-timing-and-the-engine-clock) -- 2 findings
- [Stability and known engine bugs](#stability-and-known-engine-bugs) -- 1 finding

## The view chain

### The camera object LAGS the applied pose within the frame

Hooking `PlayerCamera_UpdateViewPose` made a previously undecidable question answerable, and the answer was the
opposite of the expectation.

The question: does the applied pose equal the camera object's transform? Read from the IPC thread it is not
decidable -- `UpdateViewPose` rewrites the pose every frame, so an out-of-band reader has no way to land in a
known phase. Read inside the detour, immediately after the original returns, both sides are in the same phase.

```
same-phase, just after the write :  1 equal / 37 differ of 38
out-of-band, settled frame       : 16 equal /  0 differ
```

So when `UpdateViewPose` returns, the camera object still holds the PREVIOUS frame's pose, and something later
in the frame propagates it. By the time the frame settles the two agree.

**Why this is the finding and not a curiosity:** it places the hook UPSTREAM of the camera object. A VR override
here writes the pose and lets the engine carry it into the object, the render matrices and everything else
derived from it -- rather than fighting the propagation by writing the object directly, which the write probes
already showed is reclaimed within a frame. The two measurements together say the same thing from both ends:
the object is downstream and not writable; the pose is upstream and is where an override belongs.

It also corrects an assumption I had asserted twice: that same-phase reads would AGREE and out-of-band reads
would tear. Exactly backwards. The out-of-band reader is sampling settled frames, which is why it looked clean,
and the earlier "0 equal / 16 differ" readings came from heavy movement where the propagation had not caught up
by the time the response was built.

### The view CAN be driven: an absolute rotation at holder+324, written from the UpdateViewPose detour

The go/no-go for head tracking, and it took three wrong answers to get to it. All three are worth keeping
because each eliminated a candidate the previous one made look right.

### The chain, measured end to end

| write target | outcome |
|---|---|
| camera object `LTObject.rotation` | **Reclaimed** within one frame -- downstream, not writable |
| applied pose `holder+244` | write lands (the view visibly BLURS, so the renderer consumes it) but does not survive to the next frame -- derived, recomputed every frame |
| view rotation `holder+324` | **persists 900/900 frames**, and drives the camera AND the player's movement direction |

The blur is what identified `+244` as real-but-derived: a field nothing consumed would produce no visual
artefact at all, and one that persisted would not oscillate. Temporal blur means the renderer saw our value and
the engine's alternately, every frame.

### The bug that proved the point

The first `+324` test composed its offset onto whatever the field held THAT frame. Because the write persists,
the next frame read our own output and composed again -- 40 degrees at ~300 fps, which the player described as
the camera "spinning like crazy" and dragging their movement with it. That was a bug in the experiment, and it
is also the strongest possible evidence: only a field the engine truly derives the view from could runaway like
that. The fix is to apply the offset to a baseline captured once, which makes the write ABSOLUTE -- the shape a
head pose actually takes.

### The test that finally counted

An earlier "view lock" run reported the yaw pinned and I called it proof. It was not: the player was not trying
to look around, so there was no input for the override to beat. A null result had been read as a win -- the
exact vacuity this file keeps documenting, committed by the person documenting it.

The valid form asserts its own preconditions:

```
PHASE 1  free look, mouse moving:      yaw spread 237.03 deg   look_calls +4594
PHASE 2  override armed, still moving: yaw spread   0.00 deg   look_calls +4443
VERDICT: OVERRIDE HOLDS
```

Phase 1 must show movement or the run reports INVALID, and phase 2 must show look input still arriving. With
~4400 look events landing during the override the yaw sat at exactly -93.13 degrees, zero spread. The override
wins over player input outright.

**So VR head tracking is viable:** write an absolute rotation to `holder + 324` from inside the UpdateViewPose
detour, every frame. Releasing it returns control immediately and needs no restore, because the engine
recomputes from that field continuously -- which is also why a bounded frame countdown is enough to make the
experiment safe.

**Still open:** the write also steers movement direction, so a VR mod that decouples aim from view has to
separate them -- that is the next question, not a solved one.

### The view override is clean once it writes THROUGH the argument, not the field

A player fighting the view lock said it "rubber banded back" rather than holding still. The number agreed:
correcting the field after both hooks we owned still left a **3.24 degree worst-case excursion**, on 221 of
4421 corrections.

A data breakpoint on the live rotation address found the writer in one shot. An instruction scan for stores
into `+144h` had returned **67 functions across unrelated classes** -- the offset-collision false positive this
file already warns about -- so scanning was never going to answer it.

```
CPlayerCamera_ApplyLookToRotation  (gameclient 0x100E0830)
    fld [esi+144h] .. fld [esi+150h]     ; the quaternion at +324, copied to the STACK
    call CPlayerCamera_ApplyLookDelta    ; which modifies that stack copy in place
    fld [esp+..] / fstp [esi+144h]       ; and THIS function writes the result back
```

So `ApplyLookDelta` never touches the field at all: it transforms a stack copy and its caller commits it.
Writing the field from inside the ApplyLookDelta detour is therefore overwritten microseconds later by the
four stores below the call. That is the entire rubber-band.

Writing through the POINTER ARGUMENT puts our rotation into the value the caller is about to store:

```
                          worst drift    corrections drifting
field write, both hooks      3.2442 deg     221 / 4421
in-flight through a2         0.0000 deg       0 / 2420   (2475 look events fought it)
```

**The pattern generalises past this function.** When a routine takes a pointer to a value and its caller
commits the result, the interception point is the ARGUMENT, not the destination -- and the tell is exactly what
was seen here: the write lands, is visibly consumed, and is undone at a fixed point every frame. Fighting the
store is a losing race with the frame; replacing the value in flight is not a race at all.

It also explains the earlier reading that the applied pose at +244 "lands but does not survive". Same shape,
one level down.

### The view chain is not linear, and "zero drift" at one field proves nothing about the rendered view

A player still saw the view rubber-band while the instrument reported **0.00 degrees** of drift. Both were
right: the instrument measured the field being written, and the renderer reads something else. Measuring every
stage against what the override intended settled it.

```
                          override at +324 only     also writing +244
+324 source (we write)         0.0000 deg              0.0000 deg
+244 applied (derived)        58.8223 deg              8.5281 deg
camera object (RENDERED)      58.8223 deg            109.4697 deg
```

Two readings, two conclusions:

1. **With only +324 owned**, the applied pose and the camera object were 58.82 degrees away from our intent and
   agreed with EACH OTHER. So the render chain is not fed from +324, whatever writing +324 does to movement.
2. **Once +244 was written too**, +244 came mostly to heel (8.53) and the camera object got WORSE (109.47) --
   the two stopped agreeing. So the camera object is not a copy of +244 either; it is computed independently
   from a shared upstream, and forcing +244 only desynchronised them.

That is consistent with the write probe, which found the camera object's rotation RECLAIMED within one frame:
something rewrites it every frame, and that writer is what feeds the renderer.

**The methodological point, which cost several wrong conclusions here:** an override's instrument must measure
the stage the CONSUMER reads, not the stage the override writes. Every number I trusted was measured at the
write site, so each new field I captured reported a triumphant zero while the player kept seeing the same
artefact. The player's perception was the only signal tracking the truth, three times in a row.

Next step is a data breakpoint on the camera object's rotation, `object + 0x20`. Note that object lives in the
engine's pool rather than gameclient, so its writer may be in FEAR2.exe -- a different IDB from everything in
this chain so far.

### The camera reaches the renderer through LTObject_SetPosRot, and that is the override point

Four candidates, each eliminated by measurement rather than by reading. The numbers are the worst deviation of
each stage from what the override intended, with a player actively fighting it:

```
                          +324 only   +324 & +244   + SetRotation   + SetPosRot
+324  source (we write)     0.0000       0.0000        0.0000         0.0000
+244  applied pose         58.8223       8.5281        2.8086         1.1405
camera object (RENDERED)   58.8223     109.4697      106.1483         0.0000
```

**LTObject_SetRotation was not it, and only a live hook could say so.** It fired 50,475 times in eight seconds
and NOT ONCE on the player's camera object. Reading the disassembly would have made it look ideal -- it is the
engine's own rotation setter, it copies into `+0x20`, three callers -- and it is simply not on the camera's path.

**LTObject_SetPosRot (FEAR2.exe 0x004202B6) is it.** 1912 calls on the camera object in eight seconds, about
240/s, i.e. once per frame. It takes SEVEN floats: position at [0..2] and the quaternion at [3..6]. Replacing
only the quaternion brings the rendered rotation to exactly 0.00 degrees of deviation.

That it sets position AND rotation together is the tell in hindsight: a camera moves and turns as one operation
every frame, so it never needed the rotation-only setter.

### What this chain actually looks like

The three writable stages are not a pipeline. Forcing one does not feed the next:

* `holder+324` -- persists when written and steers MOVEMENT direction, but the renderer does not read it.
* `holder+244` -- the applied pose. A write is consumed (the view visibly blurs) but recomputed every frame.
* the camera object's `+0x20` -- what the renderer reads, written once per frame via SetPosRot.

Forcing `+244` while `+324` was pinned made the camera object WORSE, from 58.82 to 109.47 degrees: they share
an upstream and desynchronising them helps nothing. Only intercepting the engine's own setter reaches the view.

### The method that worked, after three that did not

1. Instruction scan for the field's offset: **67 functions** across unrelated classes. Useless.
2. Data breakpoint on the live address: found `LTRotation_Copy` -- a 43-caller helper. Necessary but not enough.
3. Filtering those 43 callers to the ones that target an object's `+0x20`: seven candidates, all named.
4. Hooking each and counting calls ON THE CAMERA OBJECT: one had 1912, the other zero.

Step 4 is the one that decides, and it is a runtime measurement. A consumer hooking a general-purpose engine API
must count how often it fires on the object it cares about before believing it is the right one -- 50,475 calls
with zero on the target looks identical, from the disassembly, to the function that works.

### The aim pose is DOWNSTREAM of the camera object, which reverses the chain I had assumed

A render-only override -- owning the camera object through SetPosRot and touching no aim field at all -- was
expected to freeze the view while the player's mouse ran the aim away. Measured over ten seconds of continuous
mouse movement:

```
camera SetPosRot overrides   : 2262
applied-pose writes          : 0        (nothing of ours touched the aim)
in-flight writes             : 0

camera object (RENDERED) : 0.2825 deg
+244 applied (the AIM)   : 0.2825 deg   <- identical, not divergent
```

The aim pose tracked the frozen camera exactly. So `holder+244` is DERIVED FROM the camera object, not a
sibling of it and not its source.

That retroactively explains the earlier reading that looked so strange: forcing `+244` while `+324` was pinned
made the RENDERED rotation worse, 58.82 -> 109.47 degrees. Writing a derived field desynchronises it from its
own source and helps nothing -- the same lesson as the camera-object write probe, one level along.

### The chain as it actually stands

```
    ??? -> camera object (+0x20, via LTObject_SetPosRot)  ->  +244 applied pose  ->  viewmodel
                                                          ->  bullets / aim
    holder+324 ------------------------------------------->  movement direction (separate)
```

Owning SetPosRot therefore gives authority over the view AND everything derived from it, which is more than a
head-tracked view wants: a player reported the weapon still following the mouse while bullets did not deviate,
and the viewmodel jittered only because a partial override of `+244` was fighting its source.

**RETRACTED, one measurement later: aim/view decoupling IS free.** The paragraph above concluded the opposite
from a render-only lock in which the aim pose read 0.28 degrees alongside the frozen camera. That was the wrong
field: `+244` is the CAMERA's pose, so it follows the camera by construction. The player's BODY has its own
rotation on the player object, and it was never instrumented -- every drift figure in this file was camera-side,
which is precisely why the conclusion came out backwards.

A player rotated their character through a full 360 while the view stayed stuck, and the added instrument agrees:

```
camera object (view)    :   0.0 deg    locked
+244 applied (cam pose) :   0.0 deg    follows the camera
PLAYER BODY             : 180.0 deg    turned freely (180 is this metric's ceiling)
```

So overriding the camera object at SetPosRot gives a head-tracked view while the body, aim and weapon stay under
player control -- which is exactly the split a VR mod wants, available with one hook and no further reversing.

### The lesson, stated plainly because it cost five wrong conclusions

**An instrument can only ever confirm conclusions about the thing it measures.** Every drift figure here was
read off a camera-side field, so each new override reported success at the field it wrote and the player kept
seeing something else: blur when the field was derived-but-consumed, a spin when it fed movement, rubber-banding
when a caller committed the value afterwards, a frozen aim that was really the camera's own pose, and finally a
body turning 360 degrees that no number in the endpoint could see.

Five times the player's perception was right and the instrumentation was pointing somewhere else. When a
consumer-visible symptom disagrees with a green measurement, the measurement is aimed at the wrong stage.

### View bob was the "lag", and it was passing two checks by accident

A player asked whether the residual instability came from view bob -- their character sways slightly while
standing still. It did, and the answer retracts an earlier finding in this file.

Controlled A/B, standing still, identical 12-second windows:

```
                              bob ON        bob OFF
same-phase pose vs object     2 eq / 45 df  46 eq / 0 df
camera object vs +244         0 eq / 16 df  16 eq / 0 df
camera object vs +324        16 eq /  0 df  16 eq / 0 df
```

**RETRACTED: "the camera object still holds the previous pose when UpdateViewPose returns, and something later
in the frame propagates it."** There is no lag. With view bob disabled the applied pose is bit-identical to the
camera object, every sample. The divergence was the bob OFFSET being applied to the pose, and the elaborate
propagation story was built to explain a graphics setting.

### The control read is what kept this honest

The toggle did NOT change the cvars that look like the mechanism:

```
CameraSwayXSpeed   3.0  unchanged      HeadBobSpeedScale  1.0  unchanged
CameraSwayXFreq   13.0  unchanged
```

So the options switch sets some other enable flag, and those 64 `HeadBob*` / `CameraSway*` variables are
parameters rather than the switch. Reading the supposed input BEFORE interpreting the output is the only reason
the mechanism was not mis-attributed to the first plausible cvar; finding the real flag is the next step, and it
matters for VR because a head-tracked view must disable bob.

### Two more checks were passing because of a setting

* "the camera's other position generation is NOT the same value" -- asserted that two pose generations differ.
  They differ ONLY while bob is on. Their difference was the bob offset, not evidence of two generations.
* "every catalogued tunable still holds the value the catalogue records" -- failed the instant a graphics option
  changed. The catalogue's purpose is the grid MAPPING (a composed name resolves to the computed record), which
  is asserted separately and does not care about values.

Same defect as `armor == 147`: a value recorded as a constant instead of a relationship. That is now four
distinct instances in this project, all found by somebody actually using the game.

With both corrected the suite passes twice consecutively -- 1477 and 1484 checks, zero failures -- where it had
been intermittently red.

### The camera's rotation is NOT written every frame

Measured, and it refuted the premise of the first watch test written: idle, zero hits in five seconds; with
the mouse moving, 957 in six. The engine ELIDES the write when the view has not changed, which is why
`vh_spr_camera` also reads 0 at rest. Any test that needs that write to happen needs a human moving the mouse,
which makes it a coincidence detector rather than a check -- so the suite now watches
`g_ClientGlob_bClientActive` instead, read ~480 times a second with no input, no world and no player.

### A head-tracked view, and the observable that finally settled it

The camera's rotation is the engine's own product `holder[+552] * holder[+324]`: an ADDITIVE slot times the
player's aim. A head pose belongs in the additive one, so looking around composes with aiming instead of
seizing it -- which is what `LTObject_SetPosRot` overriding did, and why that path fights the player.

Writing `+552` from outside is reclaimed within a frame (`probe_outer_operand` said so long before this), so
the mod owns the writer: `PlayerCamera_UpdateAttachedRotation` (gameclient +0xDED80, `__thiscall`, `this` IS
the holder). It runs ~28x/sec, not per frame. A write watchpoint on the field found exactly two writers -- the
engine's store at +0xDF179 and ours -- so nothing else contends for it.

**Three separate measurements agreed and all three were worthless.** The camera object rotated;
`camera == outer * inner` held; the pass argument turned 90 deg -> 150 deg for a 60 deg head yaw. Every one of
those is an intermediate field, and the screen did not appear to move. The trap underneath: the desktop
capture was STALE -- proven by a control, applying a 150-unit eye offset that had visibly moved the world
earlier in the same session and now changed nothing on screen while `cp_passes` kept climbing.

What settles it is a STATIONARY WORLD POINT changing pixel:

```
head yaw   0 deg -> x 1280.07   (screen centre of 2560)
head yaw  +6 deg -> x 1158.90   world slides left
head yaw +10 deg -> x 1076.79   further
head yaw   0 deg -> x 1280.07   exactly back
head yaw -10 deg -> x 1483.55   world slides right
```

Monotone, symmetric, exactly reversible. That projection MUST be taken inside the pass detour: read from the
IPC thread it lands on the frame's last pass, the full-screen ortho HUD pass, where a world point projects to
roughly itself -- the same phase trap that made the projection-centre check read "not determinable".

Consumer API: `PlayerMgr::camera_rotation_operands_from_holder()`, `HeadTracking::set_head_rotation()` /
`clear()`, `CameraPassHook::set_probe_point()` / `observed().probe_pixel`.

### The engine cancels roll on the player's aim, and that is why head tracking works where it does

A write watchpoint on `holder+324` found exactly ONE writer at gameclient +0xDF579, ~28 hits/sec, inside a
0x9C function now named **`PlayerCamera_CancelAimRoll`** (+0xDF500, `__thiscall`, `this` IS the holder). It
reads the aim quaternion, converts it to Euler, and rebuilds it as

```c
Quaternion_FromEuler(pitch, yaw, 0.0f);   // the roll term is a literal zero
```

So the player's aim is forced level every update. That is the mechanism behind the long-standing observation
that writes to +324 never survive a frame -- and it retroactively justifies putting the head pose in the
OUTER operand at +552, where nothing strips it. Head tilt and a level aim are not in conflict; they live in
different operands, by the engine's own design.

Verified live: with a 20 deg head roll applied, `aim_roll()` reads 0.00000 deg throughout.

### Two bugs this found in our own code

**The roll measurement.** The first `aim_roll()` used the textbook `atan2` extraction and read a rock-steady
`+180.00000` deg. That formula assumes Z-up; this engine is Y-UP, and for a level view its arguments
degenerate to `atan2(0, cos(yaw))`, which flips to pi as soon as the player faces backwards. **A constant
that tracks nothing is the tell.** Rotating (1,0,0) by the quaternion and taking the Y component needs no
convention: the right vector leaves the horizontal plane exactly when the view is rolled.

**The euler convenience.** `/vr/head?pitch=` put pitch in the quaternion's X term -- which is ROLL. Only yaw
had ever been tested, and yaw was correct, so the defect sat in a "working" API. Measured one component at a
time against a fixed world point:

```
x = ROLL    delta (+179, +40)   |move| 183.4   predicted chord 183.4
y = YAW     delta (-538, -44)
z = PITCH   delta (  +0, +537)
```

The roll identification is quantitative, not a guess: a point at screen radius r rotated by theta moves along a
chord of `2*r*sin(theta/2)`, and 183.4 measured against 183.4 predicted is the whole argument. The suite now
asserts that chord rather than "something moved".

### Three directions, and the one that still follows your head

A head-tracked view creates a question it must then answer: the camera turns and the aim does not,
so where does everything else point? `PlayerMgr::aim_vs_view` makes it a number instead of a
belief, and the numbers are exact:

```
head yaw | view-vs-aim | body-vs-view | body-vs-aim
   +0    |    0.000    |    90.000    |   90.000
  +25    |   25.000    |   115.000    |   90.000
  +45    |   45.000    |   135.000    |   90.000
```

`view-vs-aim` IS the commanded yaw, to three decimals. `body-vs-aim` never moves -- it is a
constant 90 degrees, an axis-convention offset between the body's forward and the aim's, not a
divergence. So neither the aim nor the body follows the head. That is the decoupling the whole
head-tracking design rests on, and it is now asserted rather than assumed.

### But the weapon still swings, and it is not the bones

Turning the head 25 degrees moves the hand socket 17.8 units and the muzzle 35.1. Chord geometry
(`2r sin(theta/2)`) gives radii 41.1 and 81.1, differing by 40.0 -- against a measured hand-to-muzzle
distance of 39.49. So the whole viewmodel rig rotates rigidly about the camera.

Four candidate bones were checked and all four are innocent: `aimer` (identity throughout),
`Pelvis_Cam`, `Pelvis`, `Torso` and `attach` all hold rotations that do not change by a
thousandth of a degree across a head sweep. The hand-to-hand bearing, however, rotates by 27 and
49 degrees for head yaws of 25 and 45.

A write watch found it. The SHELL player object -- the one `CClientShell::local_player` returns,
which is a DIFFERENT allocation from `PlayerMgr`'s (0x29C9F188 vs 0x18C855B0, and the latter's
rotation is never written at all) -- has its rotation set through `LTObject_SetRotation`, and the
write fires exactly when the view changes: two hits for a two-step sweep, four for a four-step
one, none while the view is still.

**So decoupling the weapon from head-look means owning that writer for that object and feeding it
the aim rather than the composed camera rotation.** That is the next VR piece, and it now has an
address instead of a hypothesis.

### A trap worth naming: two player objects

`CClientShell::local_player` and `PlayerMgr::player` return different objects. Watching the wrong
one produces a confident zero -- "nothing writes this rotation" -- which is true and useless. The
`pmgr_*_addr` diagnostics now publish both, plus the camera and model objects, because every
"who writes this" question starts by pasting one of them into `/watch/arm`.

## Rendering and stereo

### Stereo is reachable through one hook, because the pass entry takes angles and a fractional rect

`CLTRenderer_SetupPassPerspective` (slot 15, 0x0060B520) was already mapped as "where to hook". Hooking it
confirms why that note was right, and how little maths stereo actually needs here:

    __stdcall(const LTNodeTransform* camera, const float fov[2], const float rect[4], float dmin, float dmax)

It is `__stdcall`, NOT `__thiscall`, despite being a vtable slot: the wrapper loads `ecx` with g_SceneRenderer
itself and never reads an incoming `this`.

**The FOV argument predicts the record exactly.** Captured in the detour and pushed through the engine's own
clamp-and-tan (`predicted_half_view_plane`), it reproduces the mapped record field to the reported precision:

    intercepted arg -> predicted half view-plane : x=1.132569  y=0.637070
    record's mapped field                        : x=1.1326    y=0.6371

An intercepted call and a struct offset are independent routes to one pair, which is what makes both
trustworthy. Live FOV is 97.1 deg x by 65.0 deg y, viewport {0,0,1,1}, depth 4.3 .. 100000.

**Both stereo primitives work, and were verified on screen:**

* an eye offset -- `SceneCamera::offset_transform_local` displaces the pose along its OWN right vector (columns
  0/1/2 of the pose matrix are right/up/forward), rotation untouched. 18516 overrides, 0 rejected. At 100 world
  units the world visibly moves while screen-space HUD does not.
* a fractional viewport -- halving the rect renders the world into the right half of the target, which IS
  side-by-side's right eye, with no matrix work at all.

**~390 passes/second, i.e. more than one perspective pass per frame.** A stereo path cannot assume "one pass =
one eye"; which passes to displace is a filter that still needs establishing.

**What is NOT reachable here, stated so nobody plans around it:** the projection centre offset is hardcoded
(0,0) by this entry, so the asymmetric frustum a real HMD wants needs the record field written after setup.
Symmetric per-eye FOV plus a sub-rect is reachable; an off-centre frustum is not.

**Still missing for actual stereo:** rendering BOTH eyes needs the setup/draw/end group driven twice inside one
target. The engine already demonstrates the shape -- Renderer_MakeCubicEnvMap does it six times per cube map --
so the anchors exist; driving them is the next step and is not done.

### Both eyes render, and the viewport split had to be MEASURED because screenshots lied twice

The pass group repeated inside the render target the engine already opened:

    DrawScene(left)  ->  EndPass  ->  SetupPassPerspective(right)  ->  DrawScene(right)

and the engine's own EndPass closes the second pass, so the state machine ends where it began
(4 -> 3 -> 4 -> 3). That is the sequence `Renderer_MakeCubicEnvMap` performs six times per cube map, which is
where the shape came from. Both replay calls go down the TRAMPOLINES: through our own detours the transform
would be displaced twice and the draw detour would recurse.

Live: 2730 second-eye draws against 2730 setups, zero rejected, renderer stable.

**The second eye must come from the PRISTINE setup, not from eye A.** Deriving right from an already-displaced
left separates the views by two IPDs and puts the centre in the wrong place, so the detour keeps a clean copy
of the engine's own arguments before overriding anything.

### Two wrong readings from screenshots, settled by one number

I reported "the world renders only in the right half" from a screenshot, then later "the split is not taking
effect" from another. Both were readings of a dark corridor. The viewport the engine DERIVES settles it, read
inside the detour right after the setup call so it is in phase with the pass it describes:

    eye=off              [0    0 2560 1440]   w=2560
    eye=left  + split    [0    0 1280 1440]   w=1280
    eye=right + split    [1280 0 2560 1440]   w=1280

Complementary halves, exactly as intended, and the whole time. A read from the IPC thread cannot answer this:
it lands on whichever pass ran last, which is the full-screen ORTHO HUD pass (`sc_mode 2`, `sc_perspective
false`, identity pose) -- that pass is also what draws over the seam and makes side-by-side look continuous.

The lesson is the one this file keeps writing down in new forms: **a consumer-visible symptom judged by eye is
not a measurement.** Two contradictory conclusions came out of two screenshots of the same working feature,
and the disagreement was resolved by exposing the number the engine itself computed.

### Two perspective passes per frame, and they are indistinguishable by their arguments

The open question from the stereo work -- "which passes deserve an eye" -- has a measurement. A per-frame
census delimited by the engine's own frame boundary (RenderHook's present callback, not a timer):

    pass 0: fov (97.1, 65.0) deg   viewport [0..640   x 0..360]    depth 4.3..100000   cam (2134, 2376, -7842)
    pass 1: fov (97.1, 65.0) deg   viewport [0..2560  x 0..1440]   depth 4.3..100000   cam (2134, 2376, -7842)

Identical FOV, identical camera, identical `{0,0,1,1}` rect. **Nothing in the arguments separates them.** The
640x360 pass is exactly quarter resolution in each axis -- a screen-space input drawn from the same viewpoint,
not a second view.

**What separates them is the bound RENDER TARGET**, and prior work had already mapped where that lives:
`SceneRenderer_BeginRenderTarget` stores the dimensions at `g_SceneRenderer+0x174` (this[93]/this[94]), and
those are the numbers `LTRenderer_NormalizedRectToPixels` multiplies a fractional rect by. So
`SceneCamera::current_target_size()` reads them, and a pass is the main view when its target matches the swap
chain's back buffer.

Readable AT SETUP TIME, which is what makes it usable: a stereo path has to decide before forwarding the call.

Live with the filter on: 730 passes, 365 skipped as auxiliary -- exactly half, i.e. one per frame -- and every
displaced pass paired with exactly one second-eye draw, zero rejections.

### The pairing check needed a structural bound, not a tolerance

`overridden` is incremented in the setup detour and `second_eye_draws` in the draw detour, so a sample taken
between them sees one pass set up and not yet drawn. It cannot see two: the next setup cannot run until this
draw returns. The exact identity failed on one run of two, by exactly one. The bound is the pipeline depth and
is asserted as such, with the reason written next to it -- widening it further would be the fudge this project
prohibits.

### The asymmetric frustum, which is the last thing between side-by-side and a headset

`CLTRenderer_SetupPassPerspective` hardcodes the projection centre to (0,0), so the record's off-centre
capability is unreachable through it. That was recorded as a limit twice. It is now reachable.

`SceneRenderer_BuildCameraMatrices` (0x00610BA1, `__cdecl(record)`, renamed this pass) turns the centre into a
SHEAR and composes everything from the record's own scalars:

    shear = [1 0 -record[0x38] 0]
            [0 1 -record[0x3C] 0]
            [0 0  1            0]

    record[0x78]  projection      = BuildPerspective(near=record[0x40]) * shear
    record[0xB8]  view_projection = projection * view(record[0x48])
    record[0xF8]  world_to_screen = viewport_transform * view_projection

So the route is: write the centre AFTER the pass setup, then call the builder. The rebuild is the whole point
-- writing the centre alone changes nothing, because the matrices were already built, and patching the
projection alone is worse: view_projection and world_to_screen would still describe the old camera and
everything screen-space would disagree with what was drawn.

Live, applying opposite centres per eye: 254 rebuilds, 254 verified, ZERO inconsistent, `sc_w2s_coherent` true
throughout.

### The verification is the record checked against ITSELF

Multiplying the composition out gives row 0 as `m[0][2] = -centre_x * m[0][0]`, with both terms in the SAME
matrix. No baseline, no sibling structure, nothing to drift -- and it fails exactly when a centre was written
without the rebuild reaching the projection, which is the mistake that would otherwise look like success.

### And it had to be evaluated IN PHASE

The first version called the check from the IPC thread and it answered "not determinable" every single time.
The record is a PERSPECTIVE one only while a perspective pass is configured; by the time a diagnostic reads it,
the last pass of the frame is the full-screen ortho HUD pass, for which the shear identity does not hold. The
check moved into the detour, immediately after the rebuild, and started answering.

That is the third distinct thing in this project that could only be measured inside the hook -- after the
renderer gate and the pose comparison. The pattern is now unmistakable: **anything the render thread rewrites
per pass can only be checked from inside a pass.**

### The 2D pass, found by hooking both candidates instead of picking one

The HUD is painted in a screen-space pass, and a per-eye HUD needs to know which entry configures it. Two
candidates existed -- CLTRenderer slots 16 (`SetupPassAffine`) and 17 (`SetupPassStored`) -- and the header
had recorded slot 17 as "which pass it configures is NOT established".

Hooking BOTH answered it in one run: **slot 16 never fires at all in normal play, and slot 17 fires ~10 times
per frame and leaves the record orthographic every time.** Reading either one alone would have produced a
confident wrong answer; slot 16 decompiles like a perfectly plausible HUD pass.

Slot 17 carries MODE 2 -- exactly the mode `DrawScene` refuses -- which fits a pass whose contents are object
lists and 2D draws. Its camera is identity (`sub_426A42` builds position 0, quaternion (0,0,0,1)).

**The viewport, and the mis-read that nearly shipped.** The 2D pass takes no rect argument; it derives one:

```
pixels = NormalizedRectToPixels(target dimensions)
offset = sub_620CE4(+0x170)          <- added to ALL FOUR edges
half   = half the resulting extents  <- passed where the perspective pass passes FOV
```

The first reading took `+0x170` for the offset pair. It is not: it is a bound-target DESCRIPTOR, and the
already-mapped `kTargetSizeOffset` sits inside it.

```
+0x170  void*  target    -- its first dword is flags; the offset applies only when bit 0x800 is set
+0x174  int32  width     -- == kTargetSizeOffset, which is how the mis-read was caught
+0x178  int32  height
+0x17C  int32  offset x
+0x180  int32  offset y
```

Reading a pair from `+0x170` returned `(0, 2560)` -- a pointer and the target width -- and 2560 being exactly
the screen width is what exposed it. A plausible-looking pair is not a pair.

**Writing it needs the writer, again.** An external write to `+0x17C` never survives: the descriptor is
rebuilt every time a target is bound, which happens before each of the frame's ten passes. Written inside the
pass entry, before the original derives its rect, it takes immediately:

```
released   vp = [   0    0  2560  1440]
x = 640    vp = [ 640    0  3200  1440]
x = 1280   vp = [1280    0  3840  1440]
y = 360    vp = [   0  360  2560  1800]
released   vp = [   0    0  2560  1440]
```

Exact translation on both axes, independent, and releasing restores the engine's own rect. That is the
per-eye HUD mechanism. Consumer API: `SceneCamera::pass_offset()` / `pass_offset_stored()` /
`pass_offset_enabled()`, `HudPassHook::set_offset()` / `clear_offset()`, route `/vr/hud`.

### Who issues the 2D passes, and why the HUD cannot simply be replayed

Captured as return addresses from inside the slot-17 detour -- a vtable dispatch has no useful static caller
list, and the live one took a minute:

```
gameclient +0x31201  Screen2D_IssuePass_Shared           n=4 per frame
gameclient +0x11A741 Screen2D_IssuePass_Streaming        n=1
gameclient +0x80CA5  Screen2D_IssuePass_ScreenEffects    n=1
gameclient +0x80BE1  Screen2D_IssuePass_ScreenEffects    n=1
gameclient +0x11A2B3 Screen2D_IssuePass_Streaming        n=1
gameclient +0x311B2  Screen2D_IssuePass_Shared           n=1
<unmapped module>                                        n=1
```

Seven sites, ten passes, and the counts add up exactly. `ScreenEffects` is post-processing (EnableMotionBlur,
Perf_HDR_Enable) and is the only one reached from code -- `CGameClientShell_Update`. The rest are referenced
ONLY from vtables.

That is the answer to "can the HUD be drawn twice": **not by replaying one call.** The second eye works for
the scene because `DrawScene` is a single entry that can be re-issued; the HUD is a list of elements each
with its own virtual draw, so duplicating it means driving that list again. Moving the HUD is a different
question and is already solved.

## Weapons, hands and attachments

### The engine-vs-our-composition agreement is weapon-dependent

The strongest check in the fixture composes the weapon's mount point two independent ways -- the engine moves
the attached object with ITS arithmetic, we compose the same point from the asset's socket record and the bone
cache with OURS -- and required them within 0.05. Its comment records "live they agree EXACTLY (0.000)", which
is what justified a tolerance that tight.

With a different weapon in hand it is off by **27.5 units**:

```
muzzle_mdl        weapons\submachinegun\submachinegun.mdl
hands_clean       True     <- bones are NOT stale
muzzle_clean      True
muzzle_from_hand  66.73    <- inside the 5..150 barrel range, so that check is fine
weapon_vs_hand    27.548   <- required < 0.05
```

Both cheap explanations are ruled out by the same sample: the bones are clean, so it is not a stale cache, and
the muzzle distance is plausible, so the socket chain resolves. The disagreement is 27.5 units with everything
else healthy, which is a real gap in the composition rather than noise.

The likely cause, and the thing to test next: the comparison pits the weapon's ORIGIN against the **RightHand**
socket, and a weapon need not be mounted on that socket. A two-handed or differently-rigged weapon attaches
elsewhere, and composing against the wrong socket produces exactly this -- a fixed offset, clean data, wrong
answer. `attached_socket()` already returns the attachment's own record, so the fix is to compare against the
socket the ENGINE says the weapon is mounted to instead of assuming the right hand.

**Why it matters more than a red check:** hand and weapon placement IS the VR problem. A mod that puts the
weapon where our composition says will be 27 units out with this weapon, and the error is invisible in any
metric except this cross-check. That the suite caught it by having a different gun in hand is the entire
argument for cross-checking two independent producers instead of asserting one of them looks plausible.

### Driving a skeleton node, which is the mechanism VR hands and weapons ride on

`Model.hpp` had mapped ILTModel's four node-control slots (23-26) and deliberately stopped there,
on the grounds that registration lifetime is a consumer's problem. That left the single most
useful VR primitive in the engine documented and unreachable. This pass built the consumer.

### The ABI, which had to be read rather than guessed

The Add entries validate the object (`type == OT_MODEL`) and then **drop it**, calling on with just
`(node, fn, userdata)`. The object is not lost -- the thunk is `add ecx, 120h; jmp ...`, so
registration targets the sub-object at `LTObject + 0x120`, already in the schema as
`LTModelBlock120`. That one instruction is what ties the whole API to that block.

`LTModelNodeBlock_Init` then gives the block's entire layout, and it upgrades a field that had sat
as "a heap pointer unique to each model":

```
+0x00  node_control_heads  alloc(4 * node_count), zeroed   <- per-node callback list heads
+0x04  node_dirty_stride2  alloc(2 * node_count), {1,0}
+0x08  node_transforms     28 bytes each                    <- 0x1C == LTNodeTransform
+0x0C  asset               node_count at +0x20
```

The invoker is `LTModelObject_EvaluateSkeleton` at 0x428AA0, and the call site gives the signature
outright -- `fn(record, userdata)`, `__cdecl`, two pushes and two pops -- walking 12-byte cells of
`{fn, userdata, next}`.

### The record's fields are CHECKED, not read off the decompiler

The decompiler said the record's fourth dword is `node_transforms + 28*node`. That is a claim, and
it is checkable from inside the callback against the model's own array -- both sides derived from
the generated schema, so nothing restates an offset. `NodeControl::record_is_consistent` does
exactly that and the callback counts the verdicts.

**Live: 172 consistent, 0 inconsistent.** The naming is a measurement.

### What it does, measured

```
offset x=30   hand socket moved 29.99   muzzle moved 30.00
offset x=60   hand socket moved 59.99   muzzle moved 59.99
released      hand 0.000 from baseline  muzzle 0.000
```

Both the socket and the WEAPON follow, because the engine composes attachments from this transform
afterwards. The magnitude equality is an invariant rather than a tolerance: a rigid transform
preserves length, so a local displacement of d must appear as d in world space whatever the bone's
orientation.

A 35 deg rotation cross-checks the composition geometrically. Chord `= 2r sin(17.5 deg) = 0.601r`:

```
hand   moved 11.42  ->  r = 19.0
muzzle moved 34.71  ->  r = 57.7
difference 38.7  ~=  the measured hand->muzzle distance 39.49
```

Two radii differing by exactly the lever arm between them is not something a wrong composition
produces.

### Two traps worth carrying

**"RightHand" is a SOCKET, not a node.** The obvious lookup fails, correctly: this skeleton has 65
nodes (Pelvis, L_Hand, Head, ...) and 19 sockets (RightHand@38, camera@64, eyes@13, ...). Sockets
are the art's named attach points; nodes are the bones under them. Driving a socket means driving
its OWNING node, which is why `attach_to_player_socket` exists beside the node form and why
`/sdk/skeleton` now lists both with the node each socket rides.

**The registration outlives the DLL unless something removes it.** `Hooks::retire()` only knows
about safetyhook, so a live cell holding a pointer into this image is invisible to teardown --
and the next skeleton evaluation after unmap calls freed memory. `BoneControl::on_shutdown`
unlinks it, and the suite now leaves a callback registered ON PURPOSE across the uninject, exactly
as it already does with a hardware watch. Verified: module unmapped cleanly, game alive, re-inject
fresh, hand and muzzle back at their original coordinates to the decimal.

### Per-piece visibility, and the caveat it retires

`model_piece_hidden`'s own comment said the reader was "trustworthy in mechanism and untested in
the field": every hide bit was clear on all 215 models, so nothing in the scene could corroborate
it. A field nothing ever sets is a mapping nobody has checked.

ILTModel slot 9 (`ServerModelLT_SetPieceHideStatus`, 0x42B518) is the writer, and it settles the
question without a baseline:

```c
if ((hide != 0) == GetPieceHideBit(obj, piece)) return 70;   // LT_NOCHANGE
v3 = 1 << (piece & 0x1F);
v4 = (DWORD*)(obj + 4*(piece >> 5) + 268);                   // 268 == 0x10C == piece_hide_bits
if (hide) *v4 |= v3; else *v4 &= ~v3;
```

The WRITER computes the same offset the READER (slot 8) tests. Two independent engine functions,
one field. Live round trip on the player: hide `arms` -> getter reports hidden, hide
`Phead_Group` -> two hidden, `unhide_all` -> changed 2, nothing hidden. The caveat is retired.

Live the player model has 11 pieces, and they are exactly what a VR mod wants to switch off:
`Phead_Group`, `Phair_Group`, `Pglasses_Group`, `arms`, `arms_shadow`, `body`, `head_shadow`,
`pistol_Group`.

**Limits, stated rather than left implied.** Only dword 0 of the two-dword mask is exercised --
no sampled asset has 32+ pieces, so the `piece >> 5` path is unverified. And the VISUAL effect is
NOT confirmed: the desktop capture in this session returns a black frame while the engine reports
a live world and a climbing pass count, so a screenshot cannot presently corroborate anything.
What is proven is the engine's own state, through the engine's own getter.

### The weapon no longer follows your head

Last pass ended with a finding and an address: the first-person rig hangs off the SHELL player
object, whose rotation the engine rewrites to the VIEW whenever the view changes, so composing a
head pose swung the gun. This pass owns that writer.

### Correcting the earlier write-up first

The previous entry said "neither the aim nor the body follows the head". That was measured on
`PlayerMgr`'s player object and is true of it -- and it is misleading about the thing that matters,
because the object carrying the arms is the OTHER one. Measured side by side:

```
head yaw | view-vs-aim | pmgr-body-vs-aim | SHELL-vs-aim | SHELL-vs-view
   +0    |    0.000    |      90.000      |    0.708     |    0.708
  +25    |   25.000    |      90.000      |   24.803     |    0.194
  +45    |   45.000    |      90.000      |   44.646     |    0.355
```

The shell object sits within a degree of the CAMERA at every head yaw. `aim_vs_view` now reports
both objects side by side, with `shell_is_body` saying outright whether they are the same
allocation (they are not), because reading the wrong one produces a confident wrong answer.

### The intervention point

Not `LTObject_SetRotation`. Slot 4 dispatches per object type, and the player is a MODEL, so the
live entry is `OT_MODEL_SetRotation` (0x428E1C) -- which calls the base setter and then a gated
fixup at `this+304`. Hooking the base would have caught every OT_NORMAL in the level and missed
that branch.

`ViewmodelDecouple` resolves the entry from the OBJECT'S OWN VTABLE (slot 4) rather than from an
address or a scan: the object names its own setter, `object_rotation_setter` refuses anything that
does not land inside the exe, and there is nothing to keep in step with a rebuild.

The correction is one multiplication. The camera is `outer * inner`, the rig is being set to that
product, so removing the head pose is `conj(outer) * incoming`. With nothing composed, `outer` is
identity and the mod is inert by construction rather than by a switch.

```
45 degree head yaw   rig-vs-aim   rig-vs-view   muzzle travel
uncorrected             44.646        0.355         59.08
corrected                0.355       45.354          0.48
```

### The test bug this produced, which is a property worth knowing

The first version of the fixture check measured "corrected" as identical to "uncorrected". The
correction was fine; the test set `yaw=45` when the head was ALREADY at 45. **The engine elides the
rotation write when the view does not change** -- the same elision the watch counts showed (zero
hits while the view is still) -- so the setter never ran and there was nothing to correct.

Any test of this mechanism has to RELEASE the pose, set the state under test, and only then apply
the pose. The check now does, and reports both numbers so a future regression is legible rather
than a bare boolean.

### Firing is drivable, and the aim's pitch is now measurable

`aim_pitch()` joins `aim_yaw()`: the elevation of the aim's forward axis, taken as `asin` of its Y
component so it is a true angle rather than an Euler term whose meaning depends on application
order. A VR mod reconciling a head pose with the weapon needs both, and recoil, the engine's pitch
clamp and any look-assist all act on this axis rather than on yaw.

**Proven behaviourally rather than by a static read.** A number near zero proves nothing about which
angle it is, so the fixture fires the weapon: holding the trigger raises the pitch and releasing
lets it recover. Live, a held burst peaks at 3.4 to 4.9 degrees and returns to 0.0000. That is the
game's own mechanism confirming both that the shot happened and that this accessor tracks the axis
recoil acts on -- no target, no baseline.

Firing itself is `/input/tap?vk=256` (mouse button 0 encoded above the VK range), which had never
been exercised before this pass.

### The magazine drains, and that would have read as a broken accessor

Nothing in the suite reloads, so the peak fell from 7.46 to 1.10 degrees across two runs as the
weapon emptied. An empty weapon produces no recoil at all, and the check would then have failed
looking exactly like `aim_pitch` being wrong. The test now taps R first; the peak is stable at
3.4-4.9 across three runs.

Generalisable: a behavioural probe that CONSUMES a resource needs to replenish it, or it degrades
into a false negative at an unpredictable run count. The tell is a measurement that shrinks
monotonically across otherwise identical runs.

### The fire ray is still open, and here is what was ruled out

The question that matters for VR: with the view decoupled from the aim, does the shot follow the gun
or the camera? Still unanswered, and worth stating precisely rather than leaving as a vague TODO.

Ruled out this pass:

- **Not on the interfaces one would expect.** `ILTPhysics` (CLTPhysicsClient, 18 slots) has no trace
  entry -- it is dims, velocity, acceleration, move and push. `ILTCommon` (19 slots) is flags,
  attachments and parsing. Neither carries an IntersectSegment.
- **No trace function is named in the IDB.** A sweep for ray/cast/intersect/trace/segment across
  FEAR2_dump.exe returns nothing, and the same sweep over the 98-slot physics-sim vtable returns
  nothing.

Two routes remain, both bounded:

1. **Observe the impact.** Firing spawns impact effects as client objects, so diffing the object
   list across a shot gives the ray's endpoint, and its direction from the muzzle answers the
   question outright. This needs a per-object position listing, which `/sdk/objects` does not
   currently expose (it reports counts, banks and samples).
2. **Trap the trace.** With firing now drivable, an execute watch on a candidate entry fires only
   while shooting, and the watch report's registers would carry the segment endpoints directly.
   This needs a candidate address, which route 1 would also supply.

Route 1 subsumes route 2 and is the better next step.

### THE SHOT FOLLOWS THE VIEW, NOT THE GUN

The question this project has deferred five times, and the most consequential one for VR: with the
rig decoupled, the weapon points one way and the camera another -- where do the bullets go?

**The camera.** Turning the head while the aim is held still moves where shots land, by the angle
the head turned:

| head yaw | aim_yaw during | impact bearing shift |
|---|---|---|
| +30 | 89.8 | -29.64 / -30.65 / -29.32 / -29.96 / -30.27 / -30.26 |
| -30 | 90.1 | +30.60 |
| +60 | 90.9 | -61.38 |

The aim never moved (89.8 to 90.9 across every trial). The sign flips because a bearing is
`atan2(dz, dx)` while engine yaw runs the other way. Recorded in IDA on
`CPlayerCamera_ApplyLookToRotation` (gameclient 0x100E0830).

For the mod this settles a design question: `ViewmodelDecouple` keeps the weapon out of the head's
way visually, but it does NOT redirect fire. A VR mod that wants hand-aimed shooting has to drive
the view, not the rig.

### How it was measured, given the ray is unreachable

No trace function is named anywhere in the exe; `ILTPhysics` (0x0066EA70) and `ILTCommon`
(0x0066E600) have no segment-intersect entry between them. But the shot has a CONSEQUENCE: firing
spawns effects, so a newly-appeared object's position is a point the ray reached.

`sdk::ObjectWatch` is the class that makes that observable -- the difference between two looks at an
object bucket, with `appeared()`, `vanished()`, and `dominant_bearing()`. It is a real consumer API,
not test scaffolding: "what just spawned, and which way" is what a mod needs to react to projectiles,
impacts, bodies and pickups. Exposed at `/sdk/spawns`.

Three statistics were tried for "which way did that burst come from" and only the third survives:

- **Farthest object** -- returns a distant ambient emitter (7060 units away, 114 degrees off) that
  appeared in every single trial and had nothing to do with the shot.
- **Mean bearing** -- the same emitter drags it by tens of degrees.
- **Largest cluster** -- the impacts agreed to within ~5 degrees across 15 objects while the emitter
  sat alone. This is what `dominant_bearing` computes, with wrapped angle differences and unit-vector
  summation so the +/-pi seam cannot fold two neighbours into opposites.

### A NEGATIVE RESULT on the mechanism

An rw watch on the camera holder's pose fields during firing yields NO accessor that is absent while
idle: +244 (152 idle / 157 firing hits), +324 (1881 / 1940), +552 (1819 / 1843). The fire path does
not read the camera pose from the holder. Next candidate is the shell player object's own rotation,
already known to track the view exactly.

### RECOIL DOES NOT RECOVER, and last session's claim was wrong

Last session added "the recoil recovers, so firing leaves the aim where it found it". **That is
false on this build.** Measured with the player alive at full health and the world still (1157 idle
frames):

```
burst  200ms  start +5.332  peak +5.332  after 6s +6.431
burst  600ms  start +6.431  peak +6.431  after 6s +5.936
burst 1200ms  start +5.936  peak +7.522  after 6s +7.522   residual = 100% of the kick
burst 2000ms  start +7.522  peak +8.623  after 6s +8.623   residual = 100% of the kick
```

Sustained fire walks the aim upward and leaves it there. The old assertion passed only because a
short burst from an already-level aim lands back near zero -- a coincidence of the starting pose,
not an invariant. The engine has a `FireRecoilRecoverFactor` and it plainly does not return the full
kick.

The suite now drives the aim back itself through the public look primitive, with a MEASURED gain
(degrees per unit of look delta depends on the player's sensitivity, which is not ours to assume),
and asserts that closing the loop works. That is a claim about `sdk::Input::send_mouse_look` and
`PlayerMgr::aim_pitch` together, which is worth making, rather than about the engine tidying up,
which it does not do.

It is also a PRECONDITION, not just hygiene: an aim resting at +8.6 degrees shoots the ceiling,
produces no impacts, and silently breaks every measurement downstream of it. Three consecutive runs
failed three DIFFERENT ways from that one cause before it was found.

### AMMUNITION: CPlayerStats+248, AND THE INDEX THAT MAKES IT MEAN SOMETHING

A pointer at CPlayerStats+248 to an int32 array of counts, indexed by the ammo record's POSITION
within the Arsenal/Ammo database category (52 records live). Found from
`CPlayerStats_GetAmmoCount` (gameclient 0x10110FD0), which is the entire function:

    if (this[62] && rec && (i = IDatabaseMgr->vslot21(rec)) != -1) return this[62][i];

`HUD_UpdateAmmoCount` (0x100214B0) says what the number MEANS: it is the TOTAL for that ammo type,
and the HUD derives the reserve as `total - clip`, where clip is `*(weapon+184) + *(weapon+496)`.
Confirmed live -- a reload tap moves the pool by 0, so the clip is already inside the total.

Exposed as `sdk::PlayerMgr::ammo_count(name)` / `ammo_held()` / `ammo_total()` / `ammo_array()`.
Also named: `Cheat_GiveAmmo` (0x10046E60) and `CheatDispatch` (0x10047000), index 0x12.

### Why the test is about NAMES, not numbers

A wrong index still returns a plausible integer out of a live array, so "a count came back and it
went down" discriminates nothing. The equipped weapon's name does: with `submachinegun.mdl` held, a
0.6s burst moved the entry called **SMG** by exactly 9 while the total fell 220 -> 211. With
`assaultrifle.mdl`, the entry called **AssaultRifle**. Any off-by-anything puts the decrement on a
differently-named record.

The assertion that survives without knowing which weapon is equipped: **exactly one named kind
moves, and the total falls by exactly what that kind lost.** One weapon draws from one slot.

### Three wrong versions of this test, and what each taught

1. **"The largest holding decreased."** Wrong: largest is not equipped. Firing a pistol while
   carrying 82 rifle rounds moves neither the largest entry nor its name.
2. **"The total decreased."** Wrong in a *live* world: a checkpoint restore's loadout is still
   arriving, and the pool ROSE 275 -> 294 mid-burst. Now detected and reported as not-isolated
   rather than assumed away -- and fixed at source by waiting for four consecutive stable reads
   after a restore (two caught a plateau BETWEEN stages of the fill).
3. **"g_can_fire means this weapon can fire."** Wrong: it reads the total across every kind, so
   forty rounds of pistol ammunition satisfies it while the equipped rifle is empty. The checks
   now gate on spawned effects -- independent proof a round actually left the barrel.

### The probe that no longer spends what it measures

The suite's "can the player shoot" precondition used to FIRE a burst -- the only probe available
before ammunition was mapped, and self-defeating: it consumed the reserve it was checking, and a
single burst only ever proved there was ONE burst left. It now reads `ammo_total` and compares
against the rounds the suite will spend (~16 measured, 32 with margin).

### A weapon-dependent check that had never met a second weapon

Two muzzle/bone checks went red once the suite started draining magazines: an empty weapon makes
the game AUTO-SWITCH, and sampling mid-switch reads a composition still arriving. Measured at rest,
every weapon sits inside the existing bound -- submachinegun 39.5, shotgun 37.3, assaultrifle 64.8,
flamethrower 101.6 -- so the bound was never wrong; the check simply lacked the at-rest gate the
comparison two lines above it already used.

### The fire ray is traced SERVER-SIDE, and that resolves three failed hunts

`Weapon_FireServer` at gameserver.dll `0x101062B0`.

Three rw-watchpoint experiments looked for a memory read that happens only while the trigger is
down, and all three came back empty:

| watched                                    | idle hits | firing hits | fire-only accessors |
|--------------------------------------------|-----------|-------------|---------------------|
| camera holder pose (+244 / +324 / +552)     | --        | --          | 0                   |
| shell player object rotation (+0x20)        | 7806      | 7784        | 0                   |
| camera object rotation (+0x20)              | 2442      | 2614        | 0                   |

The reason is not a subtle one, and it is worth stating plainly so nobody repeats the sweep: the
client replicates its rotation to the server **every frame regardless of firing**, so pulling the
trigger adds no client-side read of any rotation. Nothing about the shot is decided in the client.
The ray is built and traced in `gameserver.dll`.

### The fire descriptor

`Weapon_FireServer(weapon, edi, desc)` receives a descriptor whose first two fields are what a VR
mod needs. The layout is not guessed -- it falls out of the function's own arithmetic:

    v57       = FireOffset * (*(float*)(desc+0)) + *(float*)(desc+12);   // and y, z
    v81[0..2] = *(desc+12..20);        // segment START
    v81[3..5] = v57, v58, v59;         // segment END = START + DIR * FireOffset
    physics->vtbl[148](physics, v81, v80)

A scalar multiplies `desc+0` and the product is added to `desc+12`. Only a direction can be scaled
and added to a point, so:

    +0x00  float[3]  DIRECTION (unit) -- rewritten per pellet with spread applied
    +0x0C  float[3]  ORIGIN
    +0x24            firing object handle
    +0x54..+0x57     flags, spline index, stance

`VectorsPerRound` from the weapon database is the pellet count and drives the loop; `Type` selects
hitscan (0, `Weapon_FireHitscanVector`) or projectile (1, `Weapon_SpawnProjectile`).

### What this means for hand-aimed shooting

Writing `desc+0` on entry to `Weapon_FireServer` redirects the shot, and it is the right place
rather than merely a workable one:

- It is **downstream of spread and pellet generation**, so one write redirects the entire shot,
  shotguns included, instead of correcting each pellet.
- It does **not fight head tracking**. The alternative -- overriding the player's aim rotation --
  is the same mistake catalogued at the top of this document under "THE FIELD IS RECLAIMED": seize
  a composed value and every consumer that derived from it has to be re-derived by hand.
- `desc+0x0C` optionally moves the muzzle origin to the hand, which is the other half of a weapon
  that points where you hold it.

The hook target is in a different module from everything the mod currently touches. `gameserver.dll`
is loaded in single player -- FEAR 2 runs a local server -- so this is reachable, but the mod's
module scanning presently looks at the exe and gameclient only.

### Hooking the server fire path: one capability gained, one negative result banked

`FireRedirect` (`src/mods/FireRedirect.hpp`) hooks gameserver.dll and now sees every shot.

### What works

The descriptor's direction **predicts where bullets land**. The fixture reads the direction out of
a struct in gameserver.dll and compares it against the impact bearing measured from spawned effect
objects -- two numbers from unrelated sources:

    [fixture] fire descriptor: predicts bearing -34.56, impacts measured -34.71 (err +0.15 deg)

Manual runs at a different heading agreed just as closely (88.93 predicted / 88.74 measured, and
88.65 / 88.03). That confirms the descriptor layout empirically rather than by reading.

### What does not work, measured three times

Writing that direction does not steer the shot. Three hook points, each verified to be executing
and writing:

| hook point                     | asked    | impacts moved |
|--------------------------------|----------|---------------|
| `Weapon_FireServer` entry      | +40 deg  | +1.23 deg     |
| `Weapon_FireHitscanVector` entry | +25 deg | +0.06 deg    |
| `Weapon_TraceShot` entry       | +25 deg  | +0.29 deg     |

**`fr_writes` incremented every time.** A hook on the right function at the wrong instant is
indistinguishable from a working one if you only count writes, which is why every attempt had to
end at the impact bearing. The reading these support: `descriptor+0` is a *record* of the aim,
derived alongside the trace from the weapon's own state, not the input the trace consumes. The
field that matters is further upstream and is not yet found.

Two functions were mis-read along the way, and both mistakes were only caught by measuring:

- `sub_100B94A0(desc)` looked like the trace. It is an AABB containment test of `desc+12` against
  object `desc+44` -- a muzzle-inside-target contact case. The normal shot takes its `else`.
- `sub_1014D350(..., desc)` runs before any work in `Weapon_FireHitscanVector` and refills the
  direction, which is what discarded the write at that hook point.

### kananlib pattern wildcards are ONE character, not two

`buildPattern` strips whitespace and then treats **each `?` as one wildcard byte**. IDA-style `??`
therefore means TWO bytes and silently lengthens the pattern -- it does not error, it just never
matches. A pattern here missed for exactly that reason. Every other pattern in this repo happens
to be wildcard-free, so nothing had exercised it before.

### Mid hooks

`Hooks` now manages `safetyhook::MidHook` alongside `InlineHook`, retired by the same
`retire()`/`retire_one()`. Mid hooks are required for `__userpurge` targets like these, where
arguments arrive in registers no C++ signature can express.

## Player state, aim and zoom

### The player's zoom controller, found by freezing fields while somebody played

The aim-limit selector had stood for several passes as "a byte at camera+1005, meaning zoom is the obvious
reading and nothing more". It was wrong on every count, and what refuted it was cheap: dump a window of raw
bytes, have a player perform the action six times, and diff.

```
pcam_window @ camera+768, 512 bytes : nothing changed at all
mm_window   @ controller+272        : +296 bit 0x20 toggled 17 times   (crouch, confirmed)
```

Crouch moved in the same session, so the player was doing the actions. The aim byte simply is not the field.
The real one, from `CPlayerCamera_ApplyLookDelta` at gameclient 0x100E0474:

```
mov ecx, [esi+4]              ; owner = *(camera + 4)  -- the player
mov edx, [ecx+100h]           ; sub   = *(player + 256)
cmp dword ptr [edx+0E0h], 3   ; a DWORD, not a flag
jz  -> CameraAimTrackingYMax (70)   ; == 3 takes the NORMAL limit
    -> CameraAimTrackingYMaxZoomed (65)
```

### Freezing a field is the cheapest way to learn what it means

Watching a value change tells you when it changes. FREEZING it tells you what depends on it, and the two
fields here separate only under that test:

| frozen | FOV zoom | weapon animation | recoil |
|---|---|---|---|
| state `+0xE0` at 3 | stops | still plays | **hip-heavy** |
| flag `+0x164` at 0 | stops | still plays | **ADS-light** |

Same visible symptom, opposite meaning. A mod suppressing the FOV zoom for a head-tracked view wants the
flag; the state is what the aim limit and recoil read. No amount of static reading distinguishes them,
because both feed the same FOV path.

The four states came out of the same freeze -- `3` hip, `0` entering, `1` full ADS, `2` leaving -- and then
`PlayerZoom_GetZoomFraction` turned out to switch on that exact field, returning `1.0` for 1, `0.0` by
default, the timer fraction for 0 and `1 - fraction` for 2. The code only makes sense under the reading the
freeze produced, so the two are independent evidence for each other.

### A fourth sub-object pointer, and a mis-attribution it exposed

`*(player + 256)` sits between the camera (+252) and the physics holder (+260) and had never been mapped.
Reading all four revealed the embedding claim was backwards:

```
*(player + 236) controller = player + 0x2760   embedded
*(player + 252) camera     = player + 0xE88    embedded   <- the 0xE88 the SDK attributed to the CONTROLLER
*(player + 256) zoom       = separate allocation
*(player + 260) physics    = player + 0x3020   embedded
```

The SDK asserted "only the controller is embedded, at 0xE88" and that the other two sat "far outside" via a
0x10000 window that both of them fall inside. Neither 0xE88 nor the controller's real 0x2760 appears as an
immediate anywhere in gameclient's `.text`, so the constant was never read off a constructor to begin with.
It still follows the `+4` owner convention despite not being embedded, which is what proves the slot holds
THIS player's object -- verified in ReGenny: `*(0x5FD20A8 + 4) == 0x1C636F60`.

### Observing must not mutate

The camera-rotation write probes ran on every read of `/sdk/shader-params`, so a player saw the view snap
away for a frame on each poll -- 171 times during one coverage run. They now live on `/sdk/write-probe`.
The old comment called the write "harmless for the frames it lasts"; harmless and invisible are different
claims, and only one of them was tested.

### Retraction: `build.bat` does propagate a failed link

A commit message in this session recorded that "build.bat exits 0 even when one project fails to link, so
`build.bat && run` did NOT gate". **That is wrong**, and it was published without testing it. Measured
directly, twice, by injecting the payload to lock the DLL and forcing a relink:

```
FEAR2VR_NO_UNLOAD=1 build.bat   ->  LNK1104, rc=1     (the gate works)
build.bat                       ->  unloads first, rc=0, fear2vr.dll relinked
```

`cmake --build ... || exit /b 1` does what it looks like it does. What actually happened in the run that
misled me: only `test/fixture_test_runner.cpp` had changed, so `fear2vr.dll` never relinked, no LNK1104
occurred in THAT build, and the `&&` fired correctly. The stale-DLL symptom was real; the explanation was
invented to fit it.

The lesson is the one this file keeps relearning from the other direction: a plausible mechanism assembled
around a real symptom is still a guess, and "the build system swallowed an error" is exactly the sort of
claim that is cheap to test and expensive to leave standing. It took one injected DLL and one touched file.

### Retraction: the player's sub-objects were never "embedded"

Recorded here and in `PlayerMgr.hpp` as members at `player+0x2760` (controller), `+0xE88` (camera) and
`+0x3020` (physics holder), with the header noting they were live-measured from ONE instance and that neither
constant appears as an immediate anywhere in gameclient.dll's `.text`. Two fixture checks asserted the exact
identities.

A later session read all four sub-objects **BELOW** the player address:

    so_off_controller_above  False
    so_off_camera_above      False
    so_off_physics_above     False
    so_all_own_player        True     <- still correct
    so_controller_class      True     <- still correct
    so_physics_class         True

Nothing below a base address can be a member of it, so the three offsets described a heap layout, not a class.
The constants and the four `*_is_embedded` predicates are DELETED rather than loosened -- a looser version
would have kept asserting a property that does not exist.

What survives is what was independently true all along and passes in every session measured: each sub-object
is identified by its **class vtable** and by its **owner back-pointer at +4**. `PlayerMgr::sub_object_offsets`
now reports the live distances instead, so the next session can see them move rather than fail on them.

The tell, in hindsight, is in the header's own words: "live-measured from one player instance" plus "no IDA
evidence exists". That is the description of a coincidence, and it was written down as a constant.

### AIMING THE VIEW IN BOTH AXES, AND THE CLAMP THAT BOUNDS IT

Shots follow the view, so in this game aiming the view IS aiming the gun. `TurnController` closed
the loop on yaw last session; it now does pitch too -- `pitch_to`, `pitch_by`, `aim_to`, `level` --
which is what a VR mod needs to point a head- or hand-indicated direction.

Pitch is not yaw with a different letter:

- **It is clamped, and the clamp MOVES.** Selected per player state from the Client/CameraClamping
  record: standing -80/+85 degrees, crouching -42/+85. Both measured live.
- **It does not wrap.** Yaw is modular, pitch is an interval; wrapping "look further up" near the
  top would produce a dive.
- **The engine never recentres it.** Recoil walks it upward and leaves it there.

### sdk::PlayerMgr::pitch_limits -- the engine's own answer

Rather than reimplement the selection, the SDK CALLS `CPlayerCamera_GetActiveCameraClamp`
(gameclient .text+0xDE160) with the live camera as `this`, SEH-guarded, resolved by pattern. It
returns `out[0] = degA*+(pi/180)`, `out[1] = degB*-(pi/180)` -- the negation is in the function --
so both flip into `aim_pitch`'s upward-positive convention.

**Verified two independent ways, and this is the part worth keeping:**

1. **Static prediction -> live stops.** The shipped StandIdle record is 80/85, predicting stops at
   +85 up and -80 down. Driving the look primitive into the stops lands at EXACTLY +85.000 and
   -80.000, with further input moving it 0.000.
2. **State dispatch -> live state change.** The decompile says CrouchIdle is 42/85. Crouching moves
   the reported DOWN limit -80.000 -> -42.000 while up stays +85.000.

The fixture asserts the invariant tying them: *the limit the engine reports is where the aim
actually stops* (84.790 vs 85.000, -79.588 vs -80.000; the residual is the controller's own 0.5
degree convergence tolerance). A wrong offset into the record, or a flipped sign, breaks that
agreement. No baseline of ours appears anywhere in it.

Consumer note: ask per use. Caching at startup is wrong the moment the player crouches.

### Calibration, and a trap in it

Pitch gain measured -0.1439 degrees per unit of dy -- the same magnitude as yaw's 0.144, so one
sensitivity drives both axes. Two of the six samples read exactly -0.2865, precisely double: that
is the documented delta ACCUMULATION in SyntheticInput (two queued deltas landing in one frame),
not a real gain change. Averaging all six would have produced a calibration 30% wrong.

### Two camera-mode functions named

`CameraMode_LookAndMoveLocal_ClampPitch80` (0x100668A0) and
`CameraMode_LookAndMoveWorld_ClampPitch80` (0x100669B0) are slot 4 of two sibling 5-entry vtables
(0x101CED5C, 0x101CD018) that share slots 0 and 2. Same look-delta handling and the same HARDCODED
symmetric +/-80 degree Euler clamp; they differ in object layout and in whether velocity is rotated
into the camera's basis (local) or added in world axes. Explicitly NOT the player camera, whose
clamp is asymmetric and state-selected -- a distinction that would be easy to conflate, since both
sites carry a 1.3962634.

### CPlayerStats is a schema class now, and the SDK derives its offsets

Authored `class CPlayerStats 0x120` in `fear2.genny` -- health/armor/max pairs, the air fraction,
`health_lost`, and the `ammo_counts` pointer -- carrying the evidence each field already had. The
seven `static constexpr uintptr_t kStats*` values in PlayerMgr.hpp are now
`offsetof(regenny::CPlayerStats, field)`, so the compiler derives every one and the layout exists
in exactly one place. Behaviour is unchanged by construction and was confirmed live: health 100,
ammo readable, 1656 checks green.

Size 0x120 is a LOWER BOUND covering the highest confirmed field, not a measurement -- safe only
because nothing allocates one; a raw engine pointer is cast on and only mapped fields are read.

Gotcha for the next class: a class written at the file's top level generates to
`shared/sdk/regenny/Foo.hpp` instead of `regenny/Foo.hpp`. It must be inside a
`namespace regenny { ... }` block.

Remaining debt after this: 78 hardcoded layout offsets across `shared/sdk/*.hpp`, 52 of them still
in PlayerMgr.hpp (the camera holder's pose fields are the big cluster).

## Input and locomotion

### Turning the player: the VR stick-turn primitive

`HeadTracking` turns the view and leaves the aim alone; `ViewmodelDecouple` keeps the weapon out of
it. Neither turns the PLAYER, and a seated VR player cannot keep rotating their chair -- so a stick
axis has to reach the game somehow.

`Input::mouse_on_move` had been mapped for several sessions and explicitly NOT driven ("deciding
when to drive them belongs to the consumer"). That left the last locomotion primitive one call
away and unreachable. It is now `Input::send_mouse_look(dx, dy)`.

The entry point takes an ABSOLUTE client point and the engine's look loop reads it relative to the
client centre, so the SDK does the geometry and the API takes a delta -- the same reasoning that
already applies to the device-vs-array asymmetry beside it. `SyntheticInput::queue_look` puts it on
the game thread and the poll detour drains it, accumulating rather than dropping, so two deltas in
one frame become one movement of their sum.

Driving the engine's own handler rather than writing the aim means sensitivity, acceleration, the
pitch clamp and every downstream consumer behave exactly as they do for a mouse. Measured: dx=200
turns the player +28.5 to +28.9 degrees, and the weapon follows because the aim itself moved.

### What is NOT claimed

The gain is not constant. The FIRST delta after an injection was measured turning roughly twice as
far as later identical ones, and a dx=400 turns less than twice a dx=200. That is unexplained, so
the suite reports it and asserts only what is established: the delta is delivered, it turns the
player, and its DIRECTION is consistent even where its magnitude is not.

### The restore had to become a closed loop

A test that turns the player must put them back, and the first version assumed equal-and-opposite
would do it. It does not, for the reason above -- one run finished exactly one turn's worth off.

So the suite now measures the heading (`PlayerMgr::aim_yaw`, added for this and for snap-turn,
which needs the same thing) and corrects until it is back: restored to within 0.06-0.26 degrees in
1-3 corrections across three runs. **That loop IS how a VR snap turn has to be implemented against
this input**, so the test and the feature share a mechanism rather than the test simulating one.

### And it needs a settled world

An intermediate version measured weapon DISPLACEMENT instead of heading and went red with a
residual of 1.897 while the turn itself was identical to the millimetre -- the arm animation, over
the four waits the block spans. Two fixes applied together: measure the heading (which the
animation does not move) and gate the block on `world_is_quiescent`, waiting up to four seconds for
the blocks above it to stop displacing the rig.

### Locomotion: movement is aim-relative, and turning had to become a control loop

With the view decoupled from the aim and the weapon decoupled from the view, the remaining question
is where the player WALKS. Measured rather than assumed: hold forward with a head pose composed in,
and the velocity's bearing equals `aim_yaw` to **0.00 degrees** at 312 world units/second. Movement
follows the BODY, never the view.

That is a defensible default -- it is body-relative locomotion -- but a VR mod has to choose it
knowingly, so `PlayerMgr::locomotion()` reports speed, bearing, and the bearing relative to BOTH the
aim and the view. Speed drives a comfort vignette; bearing-to-view is the input to "should I
recentre".

`CMoveMgr_UpdateInputFlags` / `SetInputDirectionFlags` were already mapped as the INPUT bits; the
world-space conversion is further down in the physics controller and remains unmapped. It did not
need to be: since movement follows the aim, "walk where I look" is achieved by turning the BODY,
not by rewriting the axes.

### TurnController, and the three things wrong with the obvious version

Turning to a heading cannot be one delta -- the engine's look gain is not constant. So the loop
reads `aim_yaw`, corrects, repeats. Getting it to converge took three fixes, each measured:

**1. A delta lands a frame after it is queued.** Re-evaluating immediately corrects against a STALE
error and oscillates: every turn hit the 24-iteration cap with residuals bouncing between -1.4 and
+2.0 degrees. Waiting three frames after each correction turned the same arithmetic into a
convergent loop -- 4 to 6 corrections.

**2. Being momentarily in tolerance is not being finished.** The first version stopped on the first
small reading, with a correction still in flight, and four turns each landed ~2.2 degrees past
target while the loop reported an error of 0.2. It now requires two consecutive in-tolerance
observations.

**3. Do not drive a corpse.** The player died mid-session and the loop burned its entire budget --
25 corrections with the yaw frozen to the decimal -- because input is still ACCEPTED when the player
is dead, it just does nothing. Any closed loop against game state needs a liveness gate, or it
mistakes "cannot move" for "has not moved yet".

Live: snap turns of +/-30 and +/-90 land within 0.5 degrees in 4-6 corrections, and `recentre()`
turned the body +39.60 degrees to catch a 40 degree head pose.

### The suite bound is derived, not chosen

The controller converges when it OBSERVES the error inside its 0.5 degree tolerance; the test reads
afterwards, and the smallest correction it can issue is one unit of look input (~0.144 degrees). So
the assertable bound is `0.5 + one quantum`. Asserting a flat 0.5 was measuring luck -- live
residuals reach 0.478.

### And the recovery loop earned itself again

The game died mid-session (the player had been turned into a firefight). `tools/resume_game.py` took
it from a dead process back to an injected, in-world, test-ready session in 71 seconds with no
human, and the session continued.

### View-motion comfort: the switches exist, and bob is already off

Head bob, camera sway and camera shake are the standard VR nausea sources, and this engine exposes
all three as its OWN console variables -- so suppressing them puts the game in a state it already
supports rather than one a mod invented:

```
HeadBobSpeedScale    1.0 -> 0     CameraSwayXSpeed   3.0 -> 0
DisableCameraShake   0.0 -> 1     CameraSwayYSpeed   1.0 -> 0
```

`Comfort` captures the originals, writes the suppressed values, and restores on release AND on
shutdown -- a console variable is ENGINE state that outlives the DLL, the same hazard as a hidden
model piece or a latched mouse button, both of which this project has shipped once.

### The honest half: suppressing bob changes nothing here

Every one of the 24 `HeadBob*Wave*` variables and every `*Amp` reads **0.0**. `HeadBobSpeedScale` is
1.0, but it scales a wave with no amplitude, so the bob system runs -- `bob_active` is true while
walking -- and displaces nothing. Measured in phase over ~100 render passes on a fixed path:
peak-to-peak height 3.4592 with bob on, 3.4592 with it off, to four decimals. That number is the
TERRAIN of the path.

So the suite asserts what is established (the variables resolve, the write lands, the originals come
back exactly) and REPORTS the excursion, rather than asserting a visual improvement that does not
occur on this build.

### The instrument had to be fixed before the negative result meant anything

A first attempt sampled the published camera position from the IPC thread and produced
byte-identical "measurements" for two different walks. That value is a snapshot of the LAST render
pass, so reads faster than the pass rate repeat -- sampling an oscillation that way ALIASES it.

`CameraPassHook` now accumulates min/max of the camera's Y inside the pass, one sample per pass, with
`reset_height_excursion()` to start a window. Only after that was the "bob changes nothing" result
worth believing, because only then could the instrument have detected it if it had.

An earlier note in this session claimed "walking moves the camera Y over a 13.48 unit range" as
evidence of bob. It was terrain, measured with the aliased instrument. Retracted.

### TURNING THE VIEW WITHOUT THE CURSOR: CPlayerCamera_ApplyLookToRotation

`Input::send_mouse_look` was the only way to turn the player, and it is the WRONG instrument for
anything unattended -- not because it uses "the mouse" (it never touched OS input; both SendInput
and window messages were measured dead, and it calls the engine's own device entry point directly)
but because that entry point is **positional**. It takes an absolute client point and the engine
derives the delta against the window centre, then recentres the cursor.

So the injected value competes with the real cursor. Alt-tabbed, with the pointer parked 976px left
of centre, the engine reported a constant look delta of **-976** every frame and a synthetic
dx=200 moved the aim **0.00 degrees**. Keyboard kept working, because keys are written into the
bank rather than derived from a position. That single environmental fact took down NINE checks
(snap turn, look primitive, pitch clamp both ends, aim_to) none of which had anything wrong.

The cursor-independent route, gameclient .text+0xE0830:

```c
void __thiscall ApplyLookToRotation(float* camera, float delta[3]) {
    if (delta[0] || delta[1] || delta[2]) {
        v7 = camera[81..84];               // the quaternion at +324
        ApplyLookDelta(camera, v7, delta, &out);
        camera[81..84] = out;              // commits
    }
}
```

Exposed as `sdk::PlayerMgr::apply_look_delta(index, pitch, yaw)`. Axis assignment MEASURED, since
the decompiler shows three anonymous floats -- driving each alone with +0.05 rad, unfocused:

| component | effect |
|---|---|
| 0 | pitch **-2.865 deg** (0.05 rad = 2.8648; DOWN is positive) |
| 1 | yaw **+2.865 deg** |
| 2 | nothing measurable |

**The gain is exactly 1**, unlike the mouse path where dx=200 turns ~28.5 deg and dx=400 turns less
than twice that. And it PERSISTS: +0.2 rad measured +11.459 deg (0.2 rad = 11.4592) and stayed, so
the camera update does not re-derive the rotation and undo it. It applies NO clamp, so a caller
driving pitch must consult `pitch_limits()` itself.

`TurnController` now drives this instead of queueing a mouse move; snap turns converge in 4
corrections with the window unfocused. The mouse path stays, because it is the right stimulus for
testing the input PIPELINE -- those two checks are now focus-gated and report NOT EXERCISED.

### A sign error that only appeared at a clamp

`kPitchRadiansPerUnit` is already signed for the mouse convention (positive dy looks down), and
`apply_look_delta` takes pitch upward-positive. Negating a second time asked for +85 degrees and
drove the view to -80 -- the two conventions cancelled, and the only check that could see it was
the one that compares against the engine's reported clamp at BOTH ends.

### The fire-ray test measured the right thing about the wrong quantity

It asserts the impact bearing shifts by the head yaw, which assumes the AIM holds still between the
two bursts. It does not always: a run measured -79.03 against a -30 prediction with 6 of 6 spawns
agreeing on the direction -- a clean measurement of aim drift plus head yaw. The prediction now
subtracts the measured heading drift, and reports when it exceeds a degree.

## World and units

### WORLD SCALE: ONE UNIT IS ONE CENTIMETRE, and the engine says so

`CClientMgr_GetGlobalForce` reports `(0, -980, 0)`. Earth gravity is 9.80665 m/s^2, so 980 units/s^2
is 99.93 units per metre -- and 980 is not a coincidence, it is 9.8 m/s^2 written in centimetres.

**The previous value was 64, invented by me and flagged "provisional".** It was 36% wrong, and
nothing looked broken: every controller position was silently under-scaled while remaining
plausible. That is exactly what this project's rules forbid, and it survived because "provisional"
was treated as permission rather than as a debt.

The anthropometric anchors were all worse, and disagreed with each other:

| anchor | implies |
|---|---|
| `pmgr_eye_offset_len` 75.6 units at a 1.70 m eye height | 44 u/m |
| `physics_stair_height` 40 units at a 0.20 m riser | 200 u/m |
| gravity 980 units/s^2 | **100 u/m** |

Neither field measured what its name suggested -- the "eye offset" is not eye-height-above-feet,
and the "stair height" is a maximum step-up, which at 100 u/m is 0.40 m and entirely ordinary.
Using the ENGINE's own constant rather than inferring from anatomy is the same move as calling
`GetActiveCameraClamp` instead of reimplementing the clamp selection.

Verified after the change: 0.25 m of controller travel moves the weapon 25.00 units, 0.10 -> 10.00,
0.30 -> 29.99. The fixture asserts the PREMISE (global force magnitude is 980) rather than the
conclusion, so a level with different gravity reports itself instead of quietly mis-scaling hands.

## Focus, timing and the engine clock

### Alt-tab: TWO flags, and the one that throttles is not the one that gates

`WinMain`'s loop, read from the decompiler rather than guessed:

    while ( !g_bQuitRequested ) {
      if ( g_ClientGlob_bLostFocus )   Sleep(5u);            // 0x6E4738 -- caps the loop at ~200/s
      if ( CClientMgr__Update(g_pClientMgr) ) break;
      LTClient_PumpMessages();
      if ( g_cvCursorCenter && !g_ClientGlob_bLostFocus ) ILTCursor->slot9();
    }

* `g_ClientGlob_bClientActive` (0x6E4734) gates the two simulation steps inside `CClientShell::Update` and the
  ILTInput poll in `CClientMgr::Update`.
* `g_ClientGlob_bLostFocus` (0x6E4738) makes the MAIN LOOP sleep, and gates cursor centring.

Both are written only by `LTClient_WndProc` (plus `WinMain` at init) -- established by xref, not by scanning.
Forcing `bClientActive` alone was measured NOT to resume the world (2473 re-asserts over 18s, never observed
cleared, engine clock advanced 0.000) and then HUNG the process: the input poll and render path run on an
unfocused window every iteration. So that flag is necessary, insufficient, and unsafe to force on its own.
Holding focus needs the pair, and clearing `bLostFocus` re-enables cursor warping into whatever window the
user is actually looking at. That is the hazard to solve before this becomes a feature.

### Alt-tab does not pause the game through the focus flags. It pauses the TIMER.

Chased the focus flags for most of a session and got it wrong twice. The watchpoint settled it in minutes.

Measured while alt-tabbed, payload injected and reporting:

    client_active      True     <- the engine still considers itself ACTIVE
    lost_focus         True     <- main loop takes Sleep(5): ~118/s instead of ~300/s
    eng_clock_paused   TRUE     <- scale 0.0, millisecond accumulator flat over 2 seconds
    UpdateViewPose     0 calls
    frame hook         236 ticks in 2s -- running the whole time

So the world stops because THE GAME PAUSES THE ENGINE TIMER, not because the engine thinks it is inactive.

**Found by trapping, not scanning.** `Engine::client_time_addresses()` publishes the clock's address by
walking the same pointer chain the engine's accessor walks. A write watch on it named the store -- `add
[esi+30h], eax` -- and the stack named `CClientMgr__Update -> CLTTimer_AdvanceByWallClock ->
CLTTimer_TickChildren`. Decompiling the innermost writer gave the whole node:

    +0x28 last applied step   +0x30 ms accumulator   +0x38 double seconds
    +0x48/+0x4C rational time SCALE                  +0x50/+0x54 step clamps (min 0, max 100 ms)
    +0x58 PAUSED byte -- when set, the tick multiplies its delta by 0.0

A second watch, on that pause byte, named `ILTTimer::SetPaused` (slot 20 of `g_vtbl_CLTTimer`,
`CLTTimerClient`, `CLTTimerServer`) with **gameclient.dll** frames above it. The pause policy is the GAME's.

**The fix is a hook on that one function**, refusing the request while the engine's own latch says focus was
lost, plus a one-shot clear of a pause that was already in effect when the mod was switched on. Both halves
were needed and each was proven separately: enabling while already paused reported `0 pause requests` and a
still-frozen clock until the clear was added, and the game then re-requests the pause repeatedly -- 24 requests,
24 refused -- so the clear alone would have been undone.

Verified: `clock advancing True`, 3371 ms per 3 s wall, while alt-tabbed.

**What is deliberately NOT done.** `lost_focus` is left alone even though clearing it would remove the
main-loop `Sleep(5)` and restore full rate: the same flag gates cursor centring, so clearing it warps the mouse
into whatever window the user is actually working in. ~118 iterations a second is plenty for a world to tick.
A pause requested while the window IS focused -- the pause menu -- passes through untouched.

**Why the earlier attempt failed, kept as the lesson.** Forcing `g_ClientGlob_bClientActive` true from the
per-frame hook was measured useless AND dangerous: 2473 re-asserts over 18 s, the flag never once observed
cleared, the clock advancing 0.000 -- then the process hung, because holding it true runs the DirectInput poll
and the render path against an unfocused window every iteration. Same lesson as the camera rotation: when an
owner rewrites a value, fighting the store loses. Intercept the call that DECIDES.

**Still suspended while unfocused:** `UpdateViewPose` and the renderer (`renderer_state` 1 rather than 4). Time
runs; the view and render path do not. That is a separate gate and remains unmapped.

## Stability and known engine bugs

### THE CRASH REPORTER PAID FOR ITSELF, and the answer was not what we assumed

First fully captured crash, 2026-07-31. Every layer fired: the formatting-free first record, the
full register dump, a symbolised stack, and a 430 KB minidump.

    code 0xC0000005 at MSVCR80.dll+0x173D0 -- tried to WRITE 0x254E8000
    EDI 0x254E8000 (destination)   EAX 0x25497080   -- 0x50F80 apart
    #02 FEAR2.exe+0x6AF2A   StreamReader_Read
    #03 FEAR2.exe+0x6B092
    #04 FEAR2.exe+0x6B1B7
    #05 FEAR2.exe+0x3FFF6
    #06 ntdll  (thread start)

**It is not our code.** The entire stack is FEAR2.exe and its own CRT (MSVCR80), on a worker
thread. We had two earlier crashes that WER attributed to Fear2vr.dll (memcpy_s, common_vsprintf),
so there appear to be TWO populations, and assuming one cause for all three would have been wrong.

`StreamReader_Read` is a buffered read out of a 64 KB window, and it carries two latent hazards
visible in its own decompilation:

- `this[3] - this[4]` (end - pos) is UNSIGNED, so any state with pos > end underflows to a ~4 GB
  length and memcpy runs off the destination -- which is precisely the observed signature.
- the refill triggers on `pos == 0x10000` EXACTLY rather than >=, so a pos that steps over the
  boundary without landing on it never refills.

Observed while the suite was driving repeated `LoadCheckpoint` restores, which hammer asset
streaming. A load-path race in the game is the leading explanation, and it means our restore-heavy
fixture is provoking a pre-existing engine bug rather than one of ours.

### What made this diagnosable

Three things, each of which had been necessary and absent at some point:

1. **Symbol resolution that works** -- the `SizeOfStruct` off-by-one had been silently returning
   "no symbol" for everything.
2. **A handler that survives its own subject.** Two earlier crashes produced NOTHING because the
   handler's first act was LOGX, and the fault was inside printf formatting; it re-entered and
   double-faulted. The hand-rolled hex record now goes out first.
3. **Re-asserting the filter every frame**, because the engine installs its own and there is one
   slot, not a chain.

Module+offset in the log pastes straight into the right IDB, which is how four anonymous addresses
became a named function in one lookup.

