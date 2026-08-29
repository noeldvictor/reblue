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
