# Geometry census of a field frame, for the asset stage

2026-09-04, 04:00. Desktop, the village rock scene under autoplay, one frame of the draw
ledger with each draw's index count and view distance (`bd_draw_ledger`; the ledger line
carries `q.depth`, the node's view-distance squared, since this morning). Every scene draw
is a triangle strip, so triangles = index count - 2.

| view | draws | triangles |
| --- | --- | --- |
| 3, the scene | 321 | 79,492 |
| 1, the sun shadow map | 338 | 80,989 |
| 0, the water reflection | 127 | 49,397 |
| frame | 786 | 209,878 |

The shadow pass renders as many triangles as the scene, and the reflection two thirds of
it: before any LOD, the frame rasterises the world's geometry 2.6 times.

Scene view by distance (the ledger's units, the walk's distance cull is 350):

| distance | draws | triangles | share |
| --- | --- | --- | --- |
| under 200 | 116 | 20,542 | 26% |
| 200-350 | 13 | 8,660 | 11% |
| beyond 350 | 192 | 50,290 | 63% |

Two thirds of the scene's triangles are beyond the distance the cull uses for nodes (those
draws come from visuals whose bounding sphere crosses the cull distance, and from the
render list, which the host does not cull). That is the LOD target: decimated far geometry
and impostors past a few hundred units would take most of the vertex work and, on the
Quest's fragment-bound pass, the fragments of far foliage and rock layers.

Top visuals by triangles in the scene view:

| visual | triangles | draws | meshes | blended | distance | pixel shader |
| --- | --- | --- | --- | --- | --- | --- |
| 2381f218 | 15,398 | 44 | 1 | 44 | 167 | 6c478b0c |
| 2830d2e0 | 12,195 | 37 | 29 | 37 | 167-653 | bd_normal (5e67ceb7) |
| 282c32a0 | 11,206 | 36 | 21 | 36 | 145-498 | bd_normal |
| 2839e6a0 | 11,034 | 7 | 3 | 7 | 297-355 | bd_normal |
| 282c8720 | 9,301 | 7 | 4 | 7 | 167-661 | bd_normal |
| 282b89a0 | 8,076 | 27 | 23 | 27 | 167-661 | bd_normal |
| 282c5ce0 | 5,078 | 66 | 29 | 66 | 167-1007 | bd_normal |
| 282bde20 | 2,360 | 22 | 16 | 22 | 643-718 | bd_normal |

Every one of them is blended (alpha blend on with depth write - the game's terrain and
foliage layering), which is why the queue's blended reorder mattered (the note of
2026-09-03) and why instancing now merges nothing in this scene: the largest single item,
one mesh drawn 44 times at one distance under one pixel shader, is the obvious instancing
group, and its 44 draws differ in their shared block (textures or per-draw constants), or
the queue would have merged them. Naming that difference is the first cook question.

What the cook should produce, from this census, in order of triangles saved:

1. **Far LODs** for the tree-walk visuals beyond ~350 units: decimated meshes at a quarter
   and a sixteenth of the triangles, chosen per draw by the walk's distance (the host
   template holds each sub-draw's mesh; a LOD is another view over a cooked buffer).
2. **Shadow casters**: the shadow pass takes the same meshes; the coarsest LOD is enough for
   a 1024 map, and small casters far from the camera can be skipped.
3. **The reflection view**: a coarse LOD everywhere; the puddles and the water read a
   128x72 map on the Quest.
4. **Merged statics** for the 29-mesh ground visuals: one buffer per visual, the pieces
   concatenated, drawn as one strip with degenerate joins or as a list.

Sources: `out/build/win-amd64-release/logs/draw_ledger.txt` of the 04:00 run, frame 2684;
`src/gpu/hooks/draw.cpp` (`LedgerNote`).
