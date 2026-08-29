# Research: can the guest render its scene twice in one frame?

Date: 2026-08-29 06:00
Topic: the one unknown behind stereo, answered on the desktop build with no headset.

Stereo is the largest gap in this port: the camera modes, the per-eye matrices and the two-eye cull
volume are all written and unit-tested, but the game still renders once, from one eye, onto a
world-locked quad. CLAUDE.md has said for a while that "genuine stereo needs the guest's scene
rendered twice per frame ... That is a renderer change, not an XR one". This tests exactly that
claim, and it needs no runtime, which matters because there is no headset attached and no Meta XR
Simulator on this machine.

## Finding the seam without a disassembler

`bdCameraRender` (`0x82142D30`) is the per-view scene render. Its call sites came out of the
**recompiled source**, which is the whole point of the `guest-source` skill -
`generated/reblue_recomp.34.cpp` around line 12235:

```
  mr r3,r31 ; bl 0x82142d30     <- 0x822D3C34, the branch that brackets the call
                                   with a matched sub_82173DF8 pair
  ...
loc_822D3C4C:
  mr r3,r31 ; bl 0x82142d30     <- 0x822D3C50, the plain branch
```

An if/else over the same camera in `r31`, so exactly one of the two runs per frame. The addresses
fall straight out of the emitted `loc_` labels and the instruction comments; no disassembly needed.

## Calling a guest function from a hook

Two pieces make this work, both already precedented in the tree:

- `REX_IMPORT(__imp__bdCameraRender, GuestCameraRender, void(u32))` gives a plain callable, invoked
  as `GuestCameraRender(camera)` with no context threading - see `title_menu.cpp`, which calls
  `Color4fToARGB(colorAddr)` the same way.
- A `thread_local` re-entry guard, because `bdCameraRender` renders sub-views through these same
  call sites and would otherwise multiply rather than double.

## The result

`bd_stereo_test` (default off) renders the scene a second time from the **same** camera - visually
wrong on purpose, so the measurement is about feasibility and cost and not about matrices.

| | draws/frame | verts/frame | scene target draws |
| --- | --- | --- | --- |
| off | 836 | 213,441 | 176 |
| on | **965** (+15%) | **259,886** (+22%) | 197 (+12%) |

**No crash, no fatal, no assert.** So the mechanism is sound: a guest function can be re-entered
from a midasm hook mid-frame and it does real additional work.

**But it does not re-render the scene.** A second full render would roughly double the draw count;
+15% is a fraction of one. The reasonable reading is that the first call consumes per-frame state -
the render list that `bdRenderViewSubmitAllPasses` walks, and the sort buckets behind it - so the
second call finds most of it already drained and re-draws only what is still standing.

## What this means for stereo

The seam is **higher than `bdCameraRender`**. Candidates, in the order worth trying:

- `bdCameraRenderSetup` (`0x8213C8E0`) - runs before the render and is the most likely owner of the
  per-view state that has to be rebuilt.
- `bdRenderViewSubmitAllPasses` (`0x8213C160`) - "all passes" for a view, and the natural unit of
  work to repeat.
- `bdRenderViewInsertObject` (`0x8213BF98`) / `bdRenderSortBucketsInit` (`0x8213D3A0`) - if the list
  has to be rebuilt rather than replayed, this is where it is built.

The productive next experiment is to repeat the *setup plus render* pair rather than the render
alone, and watch the draw count for a genuine doubling. That is a one-line change to
`config/hooks/stereo.toml` plus a second `REX_IMPORT`, and the desktop loop answers it in about
three minutes.

**And the cost is now known to be real.** A true second view roughly doubles draws and vertices. On
a Quest frame already carrying a ~62ms CPU floor of which ~14ms is draw recording, stereo is not
free on the CPU side either, which is an argument for getting the fill and CPU work landed before
stereo rather than after.

## Second experiment: re-entering the whole view driver

`sub_822D3598` is the view driver holding both `bdCameraRender` call sites, and its prologue is
`mr r31,r3` - so the argument it was handed is the same pointer it passes on as the camera, and the
hook already has it. That makes re-entering the *driver* a one-line change from re-entering the
render, and it repeats whatever per-view setup sits above the render.

Host code can call a recompiled function directly: `generated/reblue_funcs.h` declares every
`sub_`, and `rex::ppc::detail::current_ctx()` / `current_base()` supply the two arguments from
inside a midasm hook that only receives `PPCRegister&`.

| seam | draws/frame | verts/frame | scene target draws |
| --- | --- | --- | --- |
| off | 826 | 213,410 | 171 |
| `bdCameraRender` (0x82142D30) | 965 (+15%) | 259,886 | 197 (+12%) |
| **`sub_822D3598`, the whole view driver** | **997 (+21%)** | **260,014 (+22%)** | **213 (+25%)** |

Still no crash at either level, and still nothing like a doubling. Re-entering higher up recovers
more, which points the right way, but the shortfall is the same shape: **the render list is built
once per frame, above both seams**, and replaying either only redraws whatever is still standing.

Chasing it further up means `bdRenderViewInsertObject` (`0x8213BF98`),
`bdRenderSortBucketsInit` (`0x8213D3A0`) and the scene traversal that feeds them - i.e. re-running
visibility and sorting for the whole scene, a second time, on the CPU.

## The conclusion, which is a design one

**Stereo here should not re-run the guest.** Two experiments say the guest's submission path is not
re-entrant in a useful way, and the only way to force it is to redo scene traversal and sorting per
eye - on a frame that already carries a ~62ms CPU floor with ~14ms of draw recording in it. That
buys a second eye by making the CPU problem worse, which is the wrong trade on a device that is
GPU-fill-bound and CPU-capped at once.

The alternative is the one the host is already positioned for: **record the scene pass once and
submit it twice with different per-eye constants.** The renderer already owns its final target (the
offscreen path in `present.cpp`, added for the quad layer, and explicitly noted there as what stereo
would need), it already uploads view and projection constants per draw, and the per-eye matrices
already exist in `xr_math`/`xr_camera`. That path doubles GPU fill and leaves the guest, the
traversal, the sort and the draw recording untouched - one guest frame, two views.

It also composes with the fill work rather than fighting it: at `bd_render_scale=50` each eye costs
a quarter of a full-resolution view, so two eyes land at half the fragments of today's mono frame.

**Do not implement stereo by calling guest functions twice.** That is the finding, and it is worth
more than the +25% was.

---

## It renders. Renderer-side stereo, verified by looking at it

`bd_stereo` (default off) submits every **scene geometry** draw twice, into left and right
half-width viewports, in `DispatchDraw`. One guest frame, one render list, two views - the design
the two guest-side experiments above pointed to.

**A side-by-side stereo frame of the field scene now renders.** Same image in both halves, because
this step deliberately changes the viewport and nothing else; the per-eye matrices are the next
increment and already exist, unit-tested, in `xr_math`/`xr_camera`.

### Two wrong versions first, both caught by screenshotting the window

Neither would have been caught by a draw count or a log line, and both looked like plausible code.

**1. Doubling every draw.** The frame came back as ~40 vertical stripes. The post-process chain is
full-screen passes that *read the target they are doubling*, so each pass samples an image that is
already two half-width copies and writes two more. The subdivision compounds once per pass.

**2. Doubling every draw to a target at or above the design canvas.** Still striped, ~60 of them.
The bloom chain is small enough to be excluded by size, but the **full-resolution** post passes
render to the scene surface itself and sail straight through a size test.

**What actually separates them is vertex count.** A post pass is a full-screen quad - three or four
vertices. Scene geometry is not. `args.vertexOrIndexCount > 6` alongside the size test gives a clean
frame.

This is the third time this project has been saved by looking at the output instead of a metric, and
the first two are already recorded in the devloop skill. A draw-count check would have reported
"draws doubled, working" for all three versions.

### What is left for real stereo

- **Per-eye matrices.** The view matrix reaches the shader through the per-draw constant block, so
  the second submission needs its own upload with the second eye's view. `xr_math::FromOpenXRPose`
  and the camera modes already produce them.
- **Per-eye targets rather than half-viewports.** OpenXR wants one image per view; half-width
  viewports of one target are the desktop-visible stand-in.
- **The 2D and post passes**, currently composited once over an already-stereo scene. Correct for
  HUD-in-world, wrong for a HUD that should sit at a fixed depth per eye.

### Cost

Scene draws double, and the frame is fill-bound, so this roughly doubles GPU cost - which is exactly
what `bd_render_scale` exists to pay for. At 50 each eye is a quarter of a full view, so two eyes
land at half the fragments of today's mono frame.

## Per-eye parallax, without decomposing a matrix

The second view now gets its own vertex constants. The mechanism avoids the handedness trap
entirely: rather than reconstructing a per-eye view matrix and re-multiplying, it skews clip space.

```
clip.x' = clip.x + separation * clip.z
```

which is column 0 += separation * column 2 of the view-projection - one element in each of the four
float4 registers at VS register 32, applied after the byte swap on the host-side copy. Left eye
takes the negative, right the positive, so the two views diverge about the mono image instead of one
of them being the original.

**Why `clip.z` and not `clip.w`.** Skewing by `w` slides the whole image sideways by a constant and
reads as nothing at all. Skewing by `z` displaces a vertex in proportion to its depth, which is
parallax and is the entire depth cue. Confirmed on screen: at `bd_stereo_separation = 0.08` the near
fence rail and the character move measurably between the eyes while the distant cliffs barely do -
near shifting more than far is the signature of correct stereo.

`UploadVertexShaderConstants` gained an optional `eye_skew`, and `Video::BindEyeVertexConstants`
re-uploads and rebinds inside the per-eye loop. `FlushRenderState` has already bound the unskewed
block by then, so the loop dirties `vertexShaderConstants` on the way out or the next draw inherits
an eye.

### What this is not, yet

- **Not calibrated.** `bd_stereo_separation` is a clip-space skew, not an interpupillary distance in
  metres, and it has no relationship to the head pose or to `xr_camera`'s per-eye matrices. Comfort
  on a real headset is unknown and cannot be judged from a screenshot.
- **Not an off-centre projection.** A correct per-eye frustum is asymmetric; this is a shear. It
  gives the depth cue and it is cheap, but the geometry is an approximation and objects at the edges
  will not land exactly where a true per-eye projection would put them.
- **Not on the headset.** Half-width viewports of one target are the desktop stand-in; OpenXR wants
  one image per view, and the present path already owns its target, which is what makes that step
  small.

The honest description is: **stereo with real parallax renders, verified on screen, and the exact
per-eye geometry is the next thing to make correct.** `xr_math` already has the correct off-centre
projection and it is unit-tested; wiring it in replaces the shear.

## The convergence term is wrong, and the screenshot said so

Correct off-axis stereo wants two terms, not one:

```
clip.x' = clip.x + separation * clip.z + convergence * clip.w
```

`separation * clip.z` is the eye translation and gives the parallax. `convergence * clip.w` should
move the projection centre, which after the perspective divide is a constant NDC shift and sets the
distance at which parallax is zero. Without it every object sits behind the screen, which is the
usual reason cheap stereo is uncomfortable to fuse - so this looked like the right next step and the
arithmetic is standard.

Implemented as `column 0 += separation * column 2 + convergence * column 3`, by analogy with the
skew that works.

**It is wrong.** At `convergence = 0.05` the frame is a horizontal smear. At `0.004` - twelve times
smaller - the two eyes show *completely different viewpoints*, not a small offset: different camera
angle, character in a different place on the terrain. A constant NDC shift cannot do that, so
element 3 of those four registers is not the translation column the analogy assumed.

Which means the matrix layout at VS register 32 is not simply four rows of a row-vector
view-projection, and **the skew that does work is working for a reason not yet understood**. It
produces correct-looking depth-ordered parallax, but that should be treated as empirical rather than
derived until the layout is actually established - read out of `bdCameraViewSetMatrices`
(`0x82135228`), which sets view and projection and is the place the convention can be confirmed
rather than inferred.

Reverted. Separation-only stereo stands, because that one was looked at and was right.

Third time in this file that a plausible change was killed by a screenshot: doubling every draw,
doubling by target size, and now this. None would have been caught by a draw count, a log line, or a
clean build.

## What is actually at VS register 32, and why the stereo is a prototype

The convergence failure said the matrix layout was not what the skew assumed, so the matrix was
dumped rather than reasoned about: sixteen floats out of the uploaded block at
`kViewProjRegister`, printed once per run.

**First attempt, sampled blind at the 2000th draw:**

```
c32 = [ 1.0000  0.0000  0.0000  0.0000]
c33 = [ 0.0000  1.0000  0.0000  0.0000]
c34 = [ 0.0000  0.0000  1.0000  0.0000]
c35 = [ 0.0000  0.0000  0.0000  1.0000]
```

The identity. Many draws never write this block, and a blind sample landed on one - the same class
of mistake as sampling MSAA at the title screen.

**Skipping the identity:**

```
c32 = [ 0.0410  0.0000 -0.0489 -0.0000]
c33 = [-0.0375  0.0410 -0.0314 -0.0410]
c34 = [-0.0001 -0.0001 -0.0001  0.6670]
c35 = [ 0.0000  0.0000  0.0000  1.0000]
```

**This is not a view-projection.** A perspective matrix cannot have `[0 0 0 1]` as its last row -
that is the signature of an affine transform - and the uniform ~0.041 scale (about 1/24) does not
belong to a camera either.

So `g_mViewProj` is the name XenosRecomp took from the shader's constant table, and BD aliases that
register across different uses; what actually sits there varies per draw, and for many draws it is
the identity.

### The consequence

**The working stereo is empirical, not principled.** `column 0 += separation * column 2` on an
identity matrix sets element [2][0] to `separation`, which is a shear, and shearing whatever
transform happens to occupy those registers produced depth-ordered parallax that looks right on
screen. It is a prototype that happens to work, not an implementation of a per-eye frustum, and it
should be described that way.

It also explains the convergence failure exactly: `column 0 += convergence * column 3` on a matrix
whose last column is `(0, -0.041, 0.667, 1)` is not a projection-centre shift, it is arbitrary
corruption - which is why 0.004 was enough to send the two eyes to different viewpoints.

### Where a correct implementation goes

Not in the constant block. The per-eye view belongs where the guest computes its camera:
`bdCameraViewSetMatrices` (`0x82135228`), which calls two helpers with `r5 = r31+84` and
`r5 = r31+148` - two 64-byte destinations, 16 floats each, i.e. the view and the projection as
separate matrices. Hooking there gives both in a known form, and `xr_math` already has the correct
off-centre per-eye projection with 49 passing unit tests behind it.

That is a real piece of work rather than a one-line skew, and it is what "people feel in the world"
actually requires - the current prototype gives a depth cue with geometry that is not tied to any
interpupillary distance, head pose, or frustum.

## Dead end recorded: r31+84 is not where the camera matrices land

`bdCameraViewSetMatrices`' prologue is `lis r11,-32034 ; addi r31,r11,-22320`, so r31 is
**0x82DDA8D0**, and it calls two helpers with `r5 = r31+84` and `r5 = r31+148` - two consecutive
64-byte slots, which looked exactly like a view and a projection being written out.

Dumped both from guest memory at 0x82DDA924 and 0x82DDA964, 500 draws into a field scene:
**all thirty-two floats are zero.** So `r5` is a source or a template there, not a destination, and
the matrices are somewhere else.

The addresses are right - the arithmetic checks - so this is not an addressing slip; the reading of
what the function does with them is wrong. Establishing that properly means following
`sub_82135128` and `sub_821351A8` in the recompiled source to see what they do with r3/r4/r5, rather
than inferring from the call shape. That is the next step and it is pure source reading, no device.

Recorded so the same 20 minutes are not spent again on the same guess.

## The guest's projection, in a known convention at last

`config/functions.toml` already named the two helpers `bdCameraViewSetMatrices` calls:
**`bdCameraViewSetMatrix` (0x82135128) is the view and `bdCameraViewSetProjMatrix` (0x821351A8) is
the projection**, each taking the source matrix in `r5`. So the earlier reading was wrong in an
instructive way - `r31+84` and `r31+148` *are* the right addresses, they are just the buffers the
engine copies *from*, and 0x82DDA964 is exactly what `r5` turns out to hold.

Hooking the projection setter and reading `r5` gives, in a live field scene:

```
[ 2.41421  0.00000  0.00000  0.00000]
[ 0.00000  4.29193  0.00000  0.00000]
[ 0.00000  0.00000 -1.00005 -1.00000]
[ 0.00000  0.00000 -1.00005  0.00000]
```

An ordinary perspective projection. 2.41421 is cot(22.5), so a 45 degree horizontal field of view;
4.29193 is that times 16/9; and the -1 at [2][3] is the perspective divide taken from **-Z**. So the
engine's projection is right-handed with -Z forward - **the same convention OpenXR uses**, which is
a genuine piece of luck given how much of this file is about handedness going wrong.

### Three failed reads, one cause

It took three attempts to see this, and all three failed the same way: **`bd::mem::try_load` already
returns host order.** Byte-swapping its result again turned a clean matrix into 8.4e34 and a field
of zeros, which read as uninitialised memory and sent two investigations down the wrong path. The
fix was to stop interpreting and dump raw words in hex both ways round - `401A8279` is 2.41421 read
directly, and nothing at all read backwards.

Every read of a *guest structure* swaps, which is why the reflex was there. A helper that has already
done it for you is the exception, and this one is not obviously named.

### What this unlocks

An off-centre per-eye frustum is now a single term in a matrix whose layout is known: **`[2][0]`
shifts the frustum horizontally without moving the eye**, which is exactly the asymmetric projection
`xr_math` already produces and unit-tests. Paired with an eye translation in the view matrix from
the sibling hook, that is correct stereo geometry rather than the empirical shear currently in
`DispatchDraw`.

`bd::xr::LastGuestProjection()` now exposes the captured matrix, and
`config/hooks/stereo.toml` carries the hook. The remaining work is to apply the per-eye projection
at the point the constants are uploaded, replacing the skew.

## Correction: VS register 32 *is* the view-projection, on scene draws

The earlier claim in this file - that register 32 holds "an affine matrix with a 0.041 scale" and is
"certainly not a view-projection" - is **wrong**, and wrong for a reason that has now caused four
separate mistakes in this port: **the sample was not filtered to scene geometry.**

Dumped from a draw known to be scene geometry (222 vertices, target at the design canvas):

```
[-1.72855  0.00000  0.11006  -15.49801]
[ 0.02380  3.05633  0.37383 -485.24396]
[ 0.06308 -0.12166  0.99062   -7.26695]
[ 0.06307 -0.12165  0.99057   -6.26658]
```

Rows 2 and 3 are identical to four decimal places except in the last column, where they differ by
almost exactly 1.0. That is precisely the relationship between `[2][3] = -1` and `[3][3] = 0` in the
projection captured from `bdCameraViewSetProjMatrix`. **This is the world-to-clip view-projection**,
and the earlier identity and 0.041-scale samples were non-scene draws that never write the block.

So the clip-space skew in `DispatchDraw` is operating on the real view-projection after all, and the
parallax it produces is less accidental than the previous entry concluded.

### The open question, narrowed

Under the assumed layout - `m[row * 4 + col]`, row-vector - `column 0 += c * column 3` is
algebraically a uniform NDC shift of `c`, because `clip.x' = clip.x + c * clip.w`. It is not: at
`c = 0.004` the two eyes went to different viewpoints. Meanwhile `column 0 += s * column 2` produces
correct depth-ordered parallax.

Both cannot be true under one layout, so the row/column mapping is still not established. The
magnitudes are the clue worth following: column 3 runs to -485 while column 0 is around 0.02, so a
small coefficient against column 3 swamps column 0 entirely - which is what a *transposed* reading
would produce.

The decisive test is cheap and has not been run: apply the convergence term under the transposed
mapping and look at the frame. Two builds, six minutes, on the desktop loop.

### The lesson, now four times over

Filter the sample to the thing being measured. MSAA at the title screen, `bd_debug_max_draws`
removing fragments along with draws, stereo doubling the post chain, and now a matrix read from
draws that never write it. Every one of them produced a confident and wrong conclusion from a
correctly-executed measurement.

## Settled: the registers are columns, and convergence works

The mapping was decided from numbers already in hand rather than another build. Taking the four
registers as **columns**, register 35 is `(0.06307, -0.12165, 0.99057, -6.26658)` - its xyz has
length 1.0000, so it is the camera's forward axis plus a distance, which is exactly what `clip.w`
must be. Read as **rows**, the w coefficients would include -485.24, and no perspective matrix has
that. One reading is possible and the other is not.

So the transform is whole registers at a time:

```
register 32 += skew * register 34 + shift * register 35
```

i.e. `clip.x' = clip.x + skew * clip.z + shift * clip.w`. The previous code took *one element* from
each register - what an assumption of row-major produces - which mixes x with z and w and is exactly
why the convergence term sent the two eyes to different viewpoints while the skew still happened to
shear something plausible.

**Verified on screen at separation 0.06, convergence 0.03:** both eyes render the full scene
cleanly, with near geometry displaced more than far, and none of the smearing or divergence the
row-major version produced at a twelfth of the convergence.

This is now a genuine off-axis frustum: `bd_stereo_separation` sets the eye offset and
`bd_stereo_convergence` sets the distance at which parallax is zero, which is the parameter that
decides whether a headset is comfortable.

### Still to do

The two knobs are in clip-space units, not metres, and are not tied to head pose or to an actual
interpupillary distance. `bd::xr::LastGuestProjection()` now captures the guest's own projection
(45 degree horizontal fov, right-handed, -Z forward - the same handedness OpenXR uses), so deriving
both from a real IPD and the runtime's per-view fov is the remaining step, and `xr_math` already has
the maths with tests behind it.

## The pre-flight that saved the device session

Both `bd_render_scale` and `bd_stereo` were verified individually, on screen. The `all` preset runs
them **together**, which is how they would actually be used - stereo doubles a fill-bound frame and
the render scale is what pays for it - so the combination was screenshotted before taking any of it
to a headset.

**It rendered mono.** No stereo at all, silently, with `bd_stereo = true` set.

The scene-pass gate in `DispatchDraw` tested `width >= kDesignCanvasWidth`, a fixed 1280. With
`bd_render_scale=50` the scene target becomes 960x540, falls under the threshold, and stereo never
applies to a single draw. **The two features were mutually exclusive**, and they are precisely the
pair that belong together.

Neither feature's own test could find this: each is correct alone. The draw count was right, both
builds were green, nothing crashed, and no log line was wrong. Only looking at the combination
found it.

Fixed by scaling the threshold with `bd_render_scale`, and re-verified: the full combination -
half-scale scene, no reflections, no shadows, stereo with parallax and convergence - renders
correctly side by side.

**Run the combination you intend to ship, and look at it.** Verifying features one at a time is
necessary and is not sufficient.
