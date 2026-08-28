# Research: placing the VR screen, driving the camera, and finding the controllers

Date: 2026-08-28 19:00
Topic: three problems between "renders in VR" and "playable in VR", and what each turned out to be.

The previous note ended with Blue Dragon compositing into a Quest 2 as an OpenXR quad layer. This
one covers what stood between that and a game someone can actually sit down and play. None of the
three was a porting problem. All three were the port being wired to the wrong thing.

---

## 1. The screen was in the wrong place, three times over

A quad layer needs a pose in a reference space. The first version used the LOCAL space origin, whose
position and orientation are *wherever the headset happened to be when the session opened* — so the
screen sat off to one side, or behind, depending on how the headset was lying on the desk at launch.

Anchoring it to the player's own gaze on the first frame took three tries, and each failure is worth
recording because each is a trap that will be walked into again:

**Ordering.** `AnchorQuad` ran before `SubmitQuadLayer`, and `SubmitQuadLayer` is what sets the
distance the anchor is placed at. On the first frame that distance is still zero, so the anchor
landed exactly on the player's head and a 2 m quad filled both eyes with solid white. The symptom
reads as a rendering failure and is a sequencing one.

**Two coordinate systems in one expression.** `FrameState` hands out poses already converted to game
space; the placement then combined that with OpenXR-space direction vectors. The mirror-on-Z is its
own inverse, so the round trip *compiles and looks like a no-op* while being exactly wrong. Any
conversion that is an involution will do this. The fix was to do the whole calculation in one space
with a single explicit conversion at the top.

**A sign that is zero in the test pose.** The quad's facing yaw needs `atan2(-x, -z)` — the direction
back toward the head — not the head's own yaw. A quad's normal is +Z, and a yaw of `t` sends +Z to
`(sin t, 0, cos t)`, which must equal the vector *back* to the viewer.

Both forms are zero for a player looking straight down −Z, which is exactly the pose anyone testing
this starts from. The error only appears once you turn, and then the screen goes edge-on and nearly
disappears. This is the same shape as the projection-matrix sign bug the maths tests caught earlier:
**a symmetric or axis-aligned test case cannot see a sign error.** It is worth deliberately testing
these from an off-axis pose.

## 2. The camera composition was already written and connected to nothing

`src/xr/xr_camera.cpp` — four camera modes, recentring, anchor smoothing, 49 unit tests — had been
sitting complete and driving nothing. The work was not writing it. The work was finding the seam.

Blue Dragon keeps its view matrix at `camera+160` and its camera world position at `camera+288`, and
builds the matrix in `bdBuildViewMatrix` (0x82286C40). That function is **already hooked**, by frame
interpolation, which needs the same thing for the same reason:

> Never write camera+160: the follow camera controller reads it in the concurrent logic phase and
> would feed back.

So the interpolation hook redirects `r4` at a scratch copy instead of writing the field. Head
tracking has an identical problem — write the head pose into the camera and the follow-camera
controller chases the player's neck — so it uses the identical trick, and composes *on top of* the
interpolated matrix rather than instead of it. The head pose is an offset from the shot the game
framed, not a replacement for it.

Worth knowing for anything else that wants to influence the camera: `config/hooks/output_resolution.toml`
already rewrites FOV and aspect at four separate sites through `bdProjectionAspectHook`, so the
projection side has a proven seam too, at `bdBuildProjectionMatrix` (0x82168E18) and three inlined
copies.

### The remaining structural problem

`BeginXrFrame` runs inside `Present`, which is the *end* of the guest's frame, so the pose latched
there drives the frame after it — one frame of latency, built in. The fix is to move the
`xrWaitFrame`/`xrBeginFrame` pair to the top of the guest frame. That is a frame-pacing change and
wants doing deliberately, not as a side effect.

## 3. Quest controllers are not gamepads

This one cost the least to fix and would have cost the most to guess at. The game sat on its title
screen. `adb shell input keyevent` did nothing. SDL reported no controllers. The obvious readings —
input not wired up, the guest not polling, a broken key map — were all wrong.

**Touch controllers do not exist as Android input devices.** They are delivered exclusively as
OpenXR actions. An application that does not create an action set and call `xrSyncActions` sees a
headset with two controllers in the player's hands and correctly reports that no gamepad is
connected. There is nothing to fix in the input path, because there is no input path.

The fix is a 13-action set bound to `/interaction_profiles/oculus/touch_controller`, synced once per
frame, published through a plain struct to an `InputDriver` that presents it as an Xbox 360 pad.

Two things that made this cheaper than expected:

- **The SDK's input driver interface is public and pluggable**, and re:Blue already supplies its own
  input factory in `reblue_app.cpp` to install `SharedAssignment`. So the driver is added there, and
  needed no SDK patch.
- **`InputSystem::GetState` merges across every device assigned to the user** rather than picking
  one, so arriving after the NOP fallback driver costs nothing. Worth knowing before spending time
  on driver ordering, which is what the NOP driver's `nop_index` argument makes it look like matters.

### The mapping, and why

Touch has exactly one menu button, on the left controller; the right controller's system button
belongs to the runtime and cannot be bound. So:

| Guest | Touch |
| --- | --- |
| A / B / X / Y | A / B (right), X / Y (left) |
| START | Menu (left) |
| BACK | Left stick click |
| LB / RB | Left / right grip past halfway |
| Triggers | Triggers, analogue |
| Left stick | Left stick |
| D-pad | Right stick, past 0.6 |

BACK goes on the stick click because Blue Dragon uses it throughout its menus and there is nowhere
else for it. The d-pad rides the right stick so the left one stays free for movement.

## 4. Verifying input without hands in the headset

A mis-bound action is invisible from inside the game — the guest just quietly does the wrong thing —
and none of it can be exercised without someone physically holding a controller. So the chain logs
one line at each link:

```
OpenXR: 13 input actions attached
[xr] controller connected
[xr] guest is polling the OpenXR pad
```

That last line is the only one that distinguishes "the driver works" from "the guest found the
device and is reading it rather than the NOP pad". Everything up to a physical press is verifiable
from a desk, and presses log a line each so the mapping can be checked from outside the headset.

This is the same lesson as the two multi-hour hunts recorded in the devloop skill — **make it
visible before debugging it** — applied ahead of time rather than after.

## 5. State after this note

| Piece | State |
| --- | --- |
| Quad layer, world-locked, correctly placed | Works |
| Head pose reaching the guest view matrix | Works |
| OpenXR action set, Touch bindings, guest pad | Works |
| Stereo projection layer (per-eye render) | Not started |
| Cel shading, tourist mode | Not started |
| Occlusion descriptor set on Adreno | Dropped, not fixed |

The camera modes are composed and delivered every frame, but what the player sees is still a flat
quad — the game renders once, from one eye. Turning that into genuine stereo means either rendering
the guest's scene twice per frame or getting per-view matrices into shaders that were recompiled
from Xenos microcode with their constants already baked. That is the next real piece of work, and it
is a renderer change rather than an XR one.
