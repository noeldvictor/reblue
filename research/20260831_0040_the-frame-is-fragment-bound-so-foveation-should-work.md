# The frame IS fragment-bound, so foveation should have worked

2026-08-31. Quest 2, multiview, field scene. Corrects the reading of the foveation measurement.

## The measurement

`bd_debug_fill_scale=50` shrinks the scissor to half in each axis - a quarter of the fragments -
while leaving the viewport, the draw count, every pipeline and every upload bit-identical. Only the
surviving fragment count moves.

| | `gpu_total_ms` | draws |
| --- | --- | --- |
| baseline | 56.18 | 566 |
| `fill_scale=50` | **27.88** | 566 |

**A quarter of the fragments costs half the GPU time.** The frame is substantially fragment-bound.

## Why that matters: it exonerates foveation as a technique

Foveation measured as a net loss earlier today - `gpu_total` 56.18 to 73.19 at strength 0.5, and
70.00 at 0.15 - and that was written up as a ~17ms fixed overhead against ~3ms of savings.

The overhead reading still stands. The **savings** reading does not. If a quarter of the fragments
is worth 28ms, then dropping the peripheral shading rate to 15% over most of the screen should have
been worth far more than 3.2ms. It was not, which means **the density map was barely reducing
shading at all** while still costing the pass its fast path.

So the earlier conclusion - "foveation is a net loss on this hardware" - is right about the number
and wrong about the cause. It is not that the workload has nothing to give. It is that
`VK_EXT_fragment_density_map` on an app-owned render pass is both expensive and largely ineffective
on this driver.

That strengthens rather than weakens the case for the other route: `XR_FB_foveation`, through the
runtime's own integration, on a scene rendered into the XR swapchain. The runtime advertises
`Vulkan FFR is supported, with density map size 32x32` and forces dynamic foveation on for its own
compositing, so the fast path plainly exists - reblue is just not on it.

## The other thing this makes suspicious

If the pass is fragment-bound then it has overdraw, and front-to-back submission should reduce
shaded fragments through the tiler's low-resolution Z. **It measured exactly zero** - `gpu_total`
55.98 sorted against 55.95 unsorted, with the sort provably firing (pipeline binds 14 to 39).

A fragment-bound pass where depth ordering changes nothing is a pass where **LRZ is not rejecting**.
Worth checking before anything else, because it is free if true: Adreno disables LRZ under several
conditions, and a depth attachment that is *loaded* rather than *cleared* at pass start is one of
them - plume's render passes use `VK_ATTACHMENT_LOAD_OP_LOAD` for depth by default, and the clear
variants only switch that when the guest asks for a clear.

## Where the budget stands

At 60Hz, `gpu_total` must go under 50.0ms for 20fps and 33.3ms for 30fps. It is at 56.18ms.

Measured levers:

| lever | effect |
| --- | --- |
| fragment count (diagnostic only) | **-28ms** at a quarter of the fragments |
| second eye | -20.5ms, unavailable, it is the product |
| mono post chain | ~-3ms |
| draw batching and sorting | 0 |
| foveation by density map | +17ms |

The first line is the important one: the work is in fragments, and something that removes fragments
without removing image is the lever. Foveation is exactly that, and the route to it is the runtime's
path, not the generic extension.
