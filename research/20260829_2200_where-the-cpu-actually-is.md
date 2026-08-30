# The 15ms, located: 770,000 marshalled memory operations a frame

2026-08-29. The frame is CPU-bound and this is where the CPU is. Written so the rewrite starts from
measurements rather than from the assumption that a renderer is slow because of the renderer.

## The GPU is not the problem and neither is the geometry

```
~2000 draws   ~320,000 verts   fence 0.2-0.35ms   CPU ~20ms
```

An Adreno 650 eats 320k verts. The fence says the GPU finishes in a third of a millisecond. Blue
Dragon is a 2006 title and is not a graphically intense game, so **72Hz (13.9ms) is what the
hardware should already be doing**. Everything between here and there is CPU.

## And it is not our renderer either

Per frame, from the `[perf]` line: `mutex 0.1ms, bindFB 0.3ms, flushState 2.1ms`. **About 2.5ms of
~20ms is the host renderer.** Batching, sorting and instancing what we submit therefore caps out
around 2.5ms even if it went to zero. Worth doing, not the win.

## It is one guest function

`bdSceneNodeDrawSingle` is 7,740 bytes and runs 2084 times a frame - **16 MB of guest code executed
per frame**, and 23x the next consumer on device. Its instruction mix, counted out of the recompiled
body:

| | per node | per frame |
| --- | --- | --- |
| `stw` store word | 150 | 312,600 |
| `lwz` load word | 108 | 225,072 |
| `stfs` store float | 64 | 133,376 |
| `lfs` load float | 45 | 93,780 |
| `bl` call | 26 | 54,184 |

**~370 guest memory operations per node, ~770,000 a frame.** Every one goes through the `volatile`,
byte-swapping accessors into the guest address space.

That is the X360 pattern in a single table. The function marshals a per-node transform and material
into guest memory in big-endian, so a Xenos command processor could read it back out - and now our
hooks read it back out instead. The round trip exists for hardware that is not here.

## What the replacement looks like

Stop round-tripping. The per-node transform and material belong written once into a GPU buffer -
which is also what makes instancing possible, since instancing is exactly "put the per-copy
transforms in GPU memory". The two changes are the same change.

Structure mapping has started. The offsets read off the node pointer cluster in a way that looks
like a transform block plus a small material header:

```
+408  x9      +392  x4      +360 x2   +356 x2   +348 x2
+404  +400  +396  +388      +0 x2   +4   +8
```

`bdSceneNodeDrawSingle` has **only two callers** - `bdSceneNodeCullTraverse` and `sub_82282608` -
so the contract surface is small. Use `tools/callgraph.py` before touching any of it.

## Closed routes - do not spend time here again

- **`REBLUE_RELAXED_GUEST_MEMORY`** removes the `volatile` from exactly these accesses. It **hangs
  the game on ARM64** (starts, logs 67 lines, sits there alive) and measured **0%** on x86, 17.9ms
  either way. Nothing to trade for the risk.
- **State deduplication** is already done *by the guest*. `bdSetSamplerState` computes
  `sampler*20 + state>>2`, loads its own cached value and returns early when unchanged. The five
  calls per node are mostly no-ops.
- **Detail culling** measures zero behind the existing distance cull: `detail cull: 0 nodes` against
  `distance cull: 837600 of 880650`. The 350-unit cull already rejects 95% of nodes.
- **Shader and texture work** cannot help while the fence is 0.2ms.
- **Trading image quality for frame time** failed its first contact with a wearer:
  `bd_render_scale=25` hits 30fps and reads as "blurry gibberish" through the lenses.
