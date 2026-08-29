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
