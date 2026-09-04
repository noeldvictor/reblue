# The layered XR swapchain: one array layer an eye

2026-09-04, 11:30. Desktop under `tools/xrsim`, multiview, half width. Stage 7's first
piece: the frame is handed to the runtime as a two-layer array with one projection view
per layer, instead of a side-by-side panel the compositor has to resample.

## What it replaces

Until now the XR swapchain was `arraySize = 1` at a fraction of the window
(`bd_xr_present_scale`, 0.4), and the present pass flattened the multiview pair into it:
the gamma shader read array layer 0 into the left half of the image and layer 1 into the
right, and the two projection views took the two halves of that one image. So every frame
paid a full-screen flatten, and the compositor then scaled each half back up to the eye's
own resolution.

## What it is

- `Session::CreateSwapchain(w, h, array_size)`; `array_size` is 2 when
  `bd_xr_layered_swapchain` (default on) and `bd_stereo_multiview` are both set.
- The swapchain's size is **latched from the first real game frame's layer**, not from the
  window. The runtime hands out its images once and the size is fixed for the session, and
  the first present happens before any game frame exists - which is how the panel-sized
  swapchain of 2026-09-02 got locked in. `EnsureXrTargets` now refuses to create a layered
  swapchain until a front buffer has been seen, and the offscreen path covers those frames.
- The runtime's images are wrapped with `desc.arraySize = 2` (which makes plume's own view
  a 2D array), a subresource range of two layers, and a framebuffer with `viewMask = 3`.
- A second gamma pipeline carries `viewMask = 3`. The present pass picks it when the back
  buffer is the runtime's layered image and the source is a two-layer target, and passes
  mode 3 to the shader: each output layer reads the source layer of its own `SV_ViewID`.
  `gamma_correction_ps` moved to `ps_6_1` for that.
- The projection views take the full rect with `imageArrayIndex` 0 and 1.

## Verified

`[xr] projection views: eye0 rect 960x1080+0 layer 0, eye1 rect 960x1080+0 layer 1
(sideBySide=false, layered=true)`, and the presented image captured with both layers
stacked (`RecordCapture` is handed 2 layers on this path now):

| | |
| --- | --- |
| presented capture | 960x2160, i.e. two 960x1080 layers |
| layer 0 vs layer 1 | 6.5% of pixels differ by more than 8, mean absolute 4.4 |
| non-black | layer 0 47.7%, layer 1 47.8% |

Both layers are complete single-eye views of the same frame with horizontal disparity on
the near geometry, which is what a stereo pair looks like; neither is empty and neither is
a squeezed half-panel. The vertical letterboxing is the game's 16:9 fitted into the
layer's aspect, which is the guest chain's business and not this change's.

## Two things found on the way

- **`bd_xr_mirror` defaults to true off Android**, and direct present is gated on
  `bd_xr_direct_present && !bd_xr_mirror`. So a desktop XR run has never taken the direct
  present path at all; the layered path only appeared once the mirror was turned off. On
  Android the mirror is off, so the device has always taken it. The one-shot
  `[xr] present pass: layered=...` line names which of the four conditions is missing.
- **A crash on every multiview XR frame, pre-existing.** `HostDrawReplay` composes a
  replayed node's constants as `r.stable ? r.value : v->vs[r.reg]`, where `v` is the
  visual's interpreted node this frame. The validation loop above it bails when a moving
  delta has no `v` - but it `continue`s past the pass-camera registers (c0, c1) without
  checking, so a node whose visual had no interpreted draw reached the use site and
  dereferenced a null `v` at `v->vs[1]` (ACCESS_VIOLATION reading 0x890 = the offset of
  `vs[1]` in `VisualRegs`). The use site now skips those registers, which the pass-camera
  block overwrites unconditionally a few lines later anyway. It reproduced with the
  layered swapchain off, so it is not this change's.

## What is left for stage 7

Foveation itself. `XR_FB_foveation` attaches to a swapchain, and the fragments it saves
are the ones the *scene* pass shades - so the scene has to render into the foveated image,
not into a host target the composite later reads. The layered swapchain is the shape that
makes that possible; the next step is the scene pass targeting the swapchain's layers
directly, at the runtime's recommended per-eye size rather than the guest chain's.

Sources: `src/xr/xr_session.cpp` (`CreateSwapchain`, the projection views),
`src/gpu/present.cpp` (`XrWantsLayeredSwapchain`, `EnsureXrTargets`, the present pass),
`src/gpu/shaders/hlsl/gamma_correction_ps.hlsl` (mode 3),
`src/gpu/device_pipelines.cpp` (the layered twin),
`src/gpu/scene/host_draw.cpp` (the null-visual fix).
