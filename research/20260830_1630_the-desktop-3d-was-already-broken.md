# The desktop 3D was already broken, and three instrument bugs hid it

2026-08-30. Bisect result, with the two invalid attempts recorded so they are not repeated.

## Result

On a **verified field frame** - the capture gated on `>= 1500 draws`, not on elapsed time - the
desktop Vulkan build produces an all-black scene at every one of these commits:

| commit | what it is | field frame |
| --- | --- | --- |
| `66e296b` | **before** the guest constant rewrite | **black** |
| `301a741` | guest constants as bound chunks | black |
| `4a33749` (main) | guest constants as dynamic uniform buffers | black |

**The desktop 3D regression predates the constant rewrite.** That work is exonerated. The Quest 2
renders correctly at every one of those points and is verified at 15.0 fps with correct stereo.

Desktop **2D is fine**: the camp menu and the status-effect glossary photograph with correct
textures, colours and alpha. So the fault is specific to the 3D scene, not to the renderer as a
whole.

## Why nobody had noticed

**The composited capture had never worked.** plume created swapchain images without
`VK_IMAGE_USAGE_TRANSFER_SRC_BIT`, so copying from one is invalid usage - silently dropped, no
error without the validation layers, readback stays zero. Every `bd_capture_after_s` grab of the
finished frame has been black for as long as the feature has existed. CLAUDE.md's claim that the
desktop loop works was never tested against a desktop field frame, because it *could not be*.

## Two invalid bisect attempts, and why

Both produced "black" and both were meaningless. Recorded because the mistake is easy to repeat.

1. **A whole-screen grab photographs whatever is in front.** One reading of "99.3% non-black" that
   looked like a clean pre-rewrite render was a photograph of two terminal windows. Use
   `tools/shot_window.ps1`, which finds the reblue window by process, foregrounds it, and captures
   only its client rect.
2. **Checking out an old commit reverts the submodules too.** `git checkout 66e296b` took plume
   back to a commit *without* the `TRANSFER_SRC` fix, so the capture at the control point was
   structurally incapable of working. The control only became valid after hand-patching that one
   line into the old plume checkout.

A bisect across a superproject with submodules has to carry the *instrument* forward while moving
the *subject* back. Otherwise both arms report the instrument's failure.

## The three instrument fixes that made the question askable

- `TRANSFER_SRC` on swapchain images (plume).
- **The capture latch was being consumed by the wrong site.** `CaptureDue()` fires once; the
  composited grab was taking it before `bd_mv_capture_array` could, so the array capture
  photographed nothing - which reads exactly like a black scene target and produced two rounds of
  wrong conclusions.
- **`bd_capture_min_draws`.** `bd_xr_autoplay` does not land in the same place twice on desktop;
  menus, loading and field scenes all appear at a given elapsed time across runs. Six consecutive
  black samples were read as "the renderer is broken" when they were menus, with field scenes at
  2187 draws either side. Gate on content, never on the clock.

## What is still unknown

*Why* the desktop 3D scene is black. Not investigated - the bisect only establishes that it is not
today's work. Worth noting the desktop reaches field scenes and does real GPU work
(`draws max 2187`, `gpu_total_ms median 9.30, max 13.10`), so it is not a renderer that does
nothing; it is one whose 3D output does not survive to the scene target.

**This should not block the port.** The Quest is the target, it renders correctly, and its loop is
verified end to end.
