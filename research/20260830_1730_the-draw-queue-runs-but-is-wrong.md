# Track A1: the draw queue runs, and is slower and wrong

2026-08-30. Work in progress, `bd_draw_defer` default off.

## What was built

`DispatchDraw` has exactly four call sites and every guest draw funnels through it, so a deferred
submission layer goes in there with no guest change at all. The constant rewrite is what made this
tractable: a fully resolved draw is now a pipeline, three dynamic uniform buffer offsets - the
shared block carries every texture and sampler descriptor index, so the material rides along - a
vertex and index binding, and the draw parameters.

- `gpu/draw_queue.{h,cpp}`: record, sort, replay.
- `FlushRenderState` records into `s.pending` instead of binding, under `s.deferring_draw`.
- Flushes where a render pass ends: a framebuffer change and a barrier.
- `bd_draw_defer` records and replays in submission order. `bd_draw_sort` additionally groups
  opaque draws by pipeline. Both default off.

## Measured on a Quest 2, and it is worse

| | `dt_ms` | `gpu_total_ms` |
| --- | --- | --- |
| immediate (default) | 66.82 | 56.40 |
| `bd_draw_defer=true` | **99.81** | **87.68** |

**1.5x slower, and the frame renders incorrectly** - `tools/stereo_check.py` fails on NaN in the
capture, which means the replayed scene is not just reordered but wrong.

## Why it is slower, and it is not a mystery

The immediate path binds *deltas*: `setPipeline`, `setVertexBuffers` and `setIndexBuffer` are all
dirty-gated and skipped when nothing changed. A deferred draw cannot rely on that, because the draw
that ran before it during recording may not be the draw that runs before it during replay. So the
queue records the full binding for every draw and replays it, and the emit-time deduplication only
collapses *consecutive* identical state.

Recording the full binding also means recording the *union* of every vertex stream bound since the
command list began, which is wider than any single draw needs.

So deferral currently pays a per-draw cost to buy an ordering benefit that is not yet being taken -
`bd_draw_sort` was not even on for that measurement. The trade only becomes positive once sorting
collapses pipeline switches and front-to-back ordering lets Adreno's LRZ reject fragments.

## Four crashes, and what each one taught

All four were `ACCESS_VIOLATION` reading address `0x10` - a null dereference - and the fourth was
only diagnosed by symbolising the backtrace against the unstripped `libreblue.so` rather than by
reasoning. Do that first next time.

1. **A queued draw with no pipeline.** `setPipeline` is dirty-gated, so a draw reusing the previous
   pipeline recorded `pipeline = nullptr`. Deferral has to capture live state (`s.current_pso`),
   never the delta.
2. **The same for vertex streams**, which additionally have to be captured as the union of
   everything bound, not the range that changed.
3. **A recorded vertex range with holes.** The guest does not fill its stream slots densely, and
   plume dereferences every view it is handed. The emit now walks the range in runs of slots that
   actually have a buffer.
4. **Flushing with no framebuffer bound.** plume starts a render pass lazily on the first draw,
   from the bound framebuffer, so a flush before anything is bound crashes inside `getRenderPass`.
   The flush is now guarded on `s.draw_framebuffer_bound`, and the present-time flush was replaced
   by `DrawQueueDiscardStragglers()` - reaching present with a non-empty queue is a bug, and
   emitting there against no framebuffer is not a repair.

## What is still wrong

The replayed frame renders incorrectly. Not yet diagnosed. Prime suspects, in order:

1. **Viewport and scissor are not recorded.** They are set outside `FlushRenderState` and a queued
   draw carries neither, so every replayed draw inherits whatever was last set in the pass.
2. **`input_slots` is captured as a pointer into `VideoState`, not by value**, so a replayed draw
   reads whatever the guest left there rather than what it was recorded with.
3. Render state that `FlushRenderState` binds and the queue does not know about at all.

## Next

Fix (1) and (2), prove `bd_draw_defer` is pixel-identical to immediate submission, and only then
turn on `bd_draw_sort`. Sorting an incorrect replay would produce a number that means nothing.


---

## Update: two more causes found on the AYN Thor, and it is now closer

The Thor renders and is connected, so Track A got a correctness instrument that does not need the
Quest: capture the **scene target** with `bd_mv_capture_array`, which photographs a guest texture
and works on the flat path.

**Cause 6: deferring a user-pointer draw replays overwritten vertices.** `DrawVerticesUP` and
`BeginVertices` upload the guest's vertices into a scratch buffer on the spot, and a later draw in
the same pass reuses that scratch. A deferred replay reads whatever overwrote it. On the Thor this
produced a scene target full of **NaN** and `gpu_total_ms 2.90` against a baseline of 21.50 -
degenerate geometry the rasteriser threw away.

Only `DrawIndexedVertices` and `DrawVertices` read a real guest vertex buffer, so only those defer.
The test is by explicit name: `"DrawVertices"` and `"DrawVerticesUP"` agree on every letter up to
the suffix that decides it, so a prefix or character check silently defers exactly the draws that
must not be - which is the bug the first attempt shipped.

**Cause 7: mixing immediate and deferred draws reorders them.** A draw that cannot be deferred now
flushes the queue first, so the guest's submission order is preserved exactly. That also removed
the `2 draws still queued at present` warnings, which were UP draws issued after the last
framebuffer change.

### Where it stands

| | baseline | deferred |
| --- | --- | --- |
| `dt_ms` | 33.75 | **29.08** |
| draws | 833 | 820 |
| queue errors | - | **0** |
| NaN in scene target | no | **no** (was yes) |
| scene target | correct | **black** |

So: no crashes, no dropped draws, no NaN, and it is now *faster* than the immediate path rather
than 1.5x slower. But the scene target comes out black, so something in the replay still does not
land on the target the recording was made against. `gpu_total_ms` also reads as garbage
(3.9e12), which suggests the frame's timestamp queries are being disturbed - worth reading as a
clue rather than noise, because the queue changes where passes begin and end.

Still default off.
