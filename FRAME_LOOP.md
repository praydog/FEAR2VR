# The frame loop: the game drives, xr64 executes

## The decision

The GAME's render loop issues the OpenXR frame calls, synchronously, over IPC. xr64 owns the
OpenXR handles and performs the calls on its own frame thread, but it no longer decides *when*.

```
game thread (inside CClientShell::Update)        xr64 frame thread
-----------------------------------------        ------------------------------
request WAIT  ------------------------------->   xrWaitFrame(...)        <-- THE THROTTLE
   (blocks)                                      reply: predictedDisplayTime,
             <-------------------------------    period, shouldRender, head pose,
                                                 request_id = N
request BEGIN ------------------------------->   xrBeginFrame()
   (blocks)  <-------------------------------    ack: began / discarded / failed
render frame N using the WAIT reply's predicted time and pose
publish pixels + the pose actually used, tagged N
request END   ------------------------------->   upload, build layers, xrEndFrame()
   (blocks)  <-------------------------------
```

ORDER IS THE SPEC'S, NOT A CHOICE. `xrBeginFrame` is "called prior to the start of frame rendering",
so BEGIN must be acked BEFORE the game draws anything -- an earlier draft of this document had the
game rendering between WAIT and BEGIN, which is exactly backwards and would have been implemented
that way.


## Why

`xrWaitFrame` throttles whoever blocks on it. Today xr64 blocks on it and then has to RELAY a
cadence to the game, which needs four separate mechanisms to approximate one fact:

- a tick event,
- a pacing divisor with a window and clean-window counting,
- a phase lock with a give-up counter,
- a bounded content wait on the host side.

All four exist to answer "when should the game render", which is a question the runtime already
answers exactly. Blocking the game's own loop on the remote `xrWaitFrame` deletes all of them.

It also fixes what none of them could. The spec says the runtime adjusts `xrWaitFrame`'s throttling
"in response to feedback from frame submission and completion times in `xrEndFrame`". With the game
driving, a slow game IS a slow application: `xrEndFrame` lands late, the runtime throttles the next
`xrWaitFrame`, and the game -- blocked in it -- slows to match. ASW engages because a real miss
occurred, and the game follows the halved cadence for free because it is waiting on the runtime
rather than on a number we derived.

Measured symptoms this is meant to end, all with the game capped to 60 by DXVK:

|symptom|measurement|
|---|---|
|game and xr64 oscillate 36<->60|Oculus overlay, both sides|
|divisor climbs but the game does not land on a submultiple|divisor 2, game 50.26/s, host 68.84/s|
|the runtime never sees a miss|`repeats 271`, `held 797`, `content-wait 845`|

## MEASURED: the oscillation is ours, not the runtime's

8192 frames captured with `--trace-frames` (game capped to 60 by DXVK, in world, headset on):

|field|mean|median|p95|max|
|---|---|---|---|---|
|`iv_ms` (loop period)|16.14|13.89|**27.80**|69.44|
|`wait_blocked_ms` (runtime throttling us)|5.91|5.41|13.23|**14.06**|
|`content_ms` (waiting for game pixels)|4.15|3.00|**11.90**|16.44|
|`end_ms` (xrEndFrame)|3.15|3.52|6.14|21.06|
|`begin_ms`|0.03|0.03|0.04|0.73|

`period_ms` had **exactly one distinct value: 13.889**. New content on 47.9% of frames. 16.0% of
frames had `iv > 20 ms`.

**WHICH RUNTIME THIS WAS TAKEN AGAINST IS UNCONFIRMED, and it bounds conclusion 1.** The Meta XR
Simulator was up during this session, and a simulator has no reason to implement ASW at all -- so
"the runtime never halves its period" may be a property of THAT runtime rather than of OpenXR or of
the Oculus runtime this ships against. Conclusion 2 does not depend on it: `wait_blocked_ms` never
exceeding one period and `iv` reaching two is arithmetic about our own loop, and holds either way.
Re-take this trace on real hardware with the simulator absent before treating conclusion 1 as
general. It does not change what to build -- the handshake is correct because it puts the throttle
where the work is, not because of what this particular runtime declined to do.

Two conclusions, and they close the question the pacing work kept circling:

1. **The runtime never halves its period.** One distinct `predictedDisplayPeriod` across 8192
   frames means ASW-style feedback through that field does not happen here, so nothing derived from
   it could ever have produced or cured the oscillation. The doubling theory is dead.
2. **The runtime is not throttling us.** `wait_blocked_ms` never exceeds 14.06 ms -- one display
   period. Yet `iv` reaches two periods at p95. We miss alternate compositor deadlines on our OWN
   time: `wait_blk 5.9 + content 4.2 + end 3.2` already fills a 13.89 ms budget, and `content_ms`
   alone reaches 11.9 ms at p95.

So the 36 in the performance overlay is the host missing every other frame, not ASW engaging. The
largest controllable term is `content_ms` -- time spent inside the frame waiting for the game to
publish pixels -- which is exactly what the handshake removes: a game that is asked for frame N and
answers frame N is not something to wait a variable amount of time for.

This also retires the phase-lock/divisor approach on its own terms. It was built to align two
clocks; the measurement says there is only one clock (13.889 ms, immovable) and the host simply
overruns it.

## The call sites already exist -- no new reversing needed

The outer once-per-frame boundary is `CClientShell::Update`, which is ALREADY hooked and is where
`wait_for_host_tick()` blocks today. Its own comment says so: "This runs inside
CClientShell::Update, so blocking here paces the ENTIRE game loop -- exactly what xrWaitFrame does
for a native VR application, relayed across the process boundary." The handshake replaces the relay
with the real thing at the same point.

|handshake step|call site|why there|
|---|---|---|
|WAIT, then BEGIN|where `wait_for_host_tick()` is called from `VR::on_frame`, inside `CClientShell::Update`|once per frame, before the frame's work, already blocking there|
|render + publish N|unchanged -- the camera pass consumes the WAIT reply's pose, `FrameCapture` publishes the pixels|the pose already travels verbatim from the reply|
|END|`FrameCapture`'s publish point, after the readback|the only place that knows the pixels for frame N are actually out|

**NOT `CameraPassHook`.** It runs PER PASS and is already inside rendering, so an RPC there would be
issued several times per frame and from inside the work it is supposed to bracket. It keeps its
current job -- consuming the pose and committing it -- and issues nothing.

**TWO IDS, AND CONFLATING THEM DEADLOCKS.** An earlier draft said END must name the id that
travelled with the pixels. That cannot work: the readback is a frame deep, so those pixels belong to
an earlier request, while the spec makes Begin/End a strict pair (`XR_FRAME_DISCARDED` if a begin is
followed by another begin). Requiring END to name the older id leaves the begun frame unmatched
forever, and the next iteration -- the one that would make its pixels ready -- can never begin. The
draft deadlocked.

They are separate concerns and stay separate:

|id|what it pairs|lifetime|
|---|---|---|
|**begun request**|`xrBeginFrame` to `xrEndFrame`|one iteration; END always names the frame just begun|
|**slot id**|pixels to the pose they were drawn from|travels in the capture slot, may lag a frame|

END therefore always ends the frame it just began -- that is a call-pairing rule and nothing else.
The pixels it submits may be a frame older, and they carry their OWN id and their own pose in the
slot, which is what makes the projection layer's stated pose correct. Pixel age is a latency
property; call pairing is a spec requirement. The old draft turned the first into a constraint on
the second.

## SAME-FRAME SUBMISSION IS LOAD-BEARING. Do not pipeline it back.

Confirmed in the headset: the head jitter that survived six earlier attempts is GONE with the
handshake, in the problem location, at variable frame rate. And it was fixed only ONCE the loop was
serialized -- which means the cure is not the round-tripped pose on its own.

The mechanism. With the frame-deep readback, the pixels being submitted were drawn a frame earlier.
The stated pose was correct FOR THOSE PIXELS, but the image the compositor reprojected was a frame
stale relative to the pose it was warping toward, so any head movement left a residual that scaled
with speed -- which is what was being seen. Serializing made pixels, pose and submission all belong
to the same frame, and the residual went with it.

So this is a CONSTRAINT on any future throughput work:

- **Reducing rendezvous is fine.** Collapsing three RPCs into one transaction, or replacing the
  Sleep(0) polling with events, does not change which frame's pixels are submitted. That is pure
  overhead removal.
- **Reintroducing pipeline depth is NOT.** Letting the game render N+1 while the host submits N
  restores exactly the staleness that was just removed, and it will bring the jitter back. An earlier
  commit message in this repo (c90c00c) proposed doing that as "the next change" -- it was wrong, and
  it is recorded here because a plausible-sounding performance fix is precisely how a hard-won
  correctness property gets undone by someone who was not in the room.

The measured cost of the constraint, for whoever weighs it later: a 240 fps-capped game runs 240 with
the headset off and 40-60 with it on, iv 17.06 ms mean against a 13.889 ms period, of which 8.68 ms is
real work and 4.16 ms is not yet attributed to anything. Attribute that residual before trading any of
this away -- it is uninstrumented time, not proven overhead, and the per-request timing that would
settle it does not exist yet.

## The rules it must follow

1. **Every OpenXR call stays on xr64's frame thread.** The game never links OpenXR; that is the
   whole reason the host exists (the 32-bit Oculus runtime dies inside its own RuntimeIPC init
   during `xrCreateSession` -- measured, and the reason for the two-process split).
2. **Tagged request/ack, never "any newer frame".** The WAIT reply carries a `request_id`; the
   pixels the game publishes carry the same id; xr64's END accepts only that id. Accepting whatever
   is newest is what makes a pose/pixel pairing unprovable, and this project has already spent a
   session on that class of bug.
3. **Every wait is bounded, a timeout is LOUD, and it leaves a DEFINED state.** A game blocked
   forever on a dead host is worse than an unpaced game. On timeout: count it, name it in `/xr`,
   and fall through -- never substitute a default cadence, which is the bug that produced the
   hardcoded 22 ms and 12 ms.
   The state machine has to survive each failure without stranding a frame:
   - **WAIT times out or fails** -> no frame exists. The game renders unpaced this iteration and
     MUST NOT send BEGIN or END for it.
   - **BEGIN times out, fails, or is never acked** -> the game MUST NOT send END, because an END
     without a matching successful begin is an unpaired end.
   - **BEGIN acked, then the game cannot produce pixels** -> END is still owed, with zero layers.
     The pairing belongs to Begin/End, not to whether there was anything to show.
   - xr64 tracks at most ONE outstanding begun frame -- the spec allows no more -- and rejects an
     END that does not name it, so a late or duplicated game request cannot end someone else's
     frame. The pixels submitted with it are whatever the capture slot holds, tagged with their own
     id and pose; that is a latency question, not a pairing one.
4. **Begin/End stay paired.** One successful `xrBeginFrame` gets exactly one `xrEndFrame`, with zero
   layers if there is nothing to show. The spec's own wording: a skipped begin means a skipped end.
5. **The pose the game renders with comes from the WAIT reply and goes back with the pixels,
   verbatim.** Not reconstructed, not re-read later. That is already the rule; the handshake makes it
   structural rather than a convention.

## What gets deleted when this lands

`FramePublisher`'s tick wait, divisor, window and give-up state; the host's bounded content wait;
`HostState::predicted_display_period_ns` as a pacing input (it stays as information). None of them
should be left beside the new path -- two mechanisms deciding when to render is how the current
oscillation happened.

## What must be measured before and after

Per frame, through a 36<->60 transition: `xrWaitFrame` return interval, `predictedDisplayPeriod`,
whether content was new or repeated, and `xrEndFrame` timing. Without that log, "it feels better" is
the only available verdict, and this subsystem has produced six plausible-but-wrong fixes judged
exactly that way.
