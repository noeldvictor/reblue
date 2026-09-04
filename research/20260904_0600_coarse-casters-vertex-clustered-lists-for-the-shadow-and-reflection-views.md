# Coarse casters: vertex-clustered index lists for the shadow and reflection views

2026-09-04, 06:00. Desktop, village rock scene under autoplay. Stage 3's first generated
asset, built live on the host from the guest's own meshes (`gpu/scene/mesh_lod.*`,
`bd_lod`, `bd_lod_shadow_grid` = 24, `bd_lod_reflection_grid` = 16).

## Why

The 04:00 census: the shadow pass rasterises 81k triangles a frame and the reflection 49k,
both the scene's full detail, into a 1024 map and a 128x72 reflection on the Quest. The
05:00 view culls (camera distance, the fitted light square) took the reflection to 37k and
the shadow only to 77k: the casters are the visible world. What shrinks them is the mesh.

## What it is

At the host replay of a node draw for render view 1 (shadow) or 0 (reflection), the draw's
index range is replaced by a decimated triangle list over the **same vertex buffer**:

- The guest's strip is read from guest memory (big-endian u16, `0xFFFF` restarts a run -
  every strip in the field uses them, and the first build refused all 419 meshes because
  65535 was read as a vertex) and converted to triangles.
- The position element (from the vertex declaration: stream, offset, type - float3 in every
  mesh seen so far; 16-bit and half formats are decoded too, axis order left permuted since
  clustering is permutation-invariant) is read for every referenced vertex.
- Vertices snap to a grid of `grid` cells across the mesh's longest axis; each cell keeps
  the original vertex nearest its centroid; triangles remap, degenerate and same-winding
  duplicate ones drop.
- The list uploads once into an INDEX buffer and is cached by (index buffer, vertex buffer,
  range, offsets, stride, position element, grid) - the plume buffer pointers make a
  refreshed physical block a new key. The draw goes out as a triangle list with the same
  base vertex; the vertex shader, the pulled path and the instance records see nothing.

Two guards decide whether a list is used at all:

- **Saving**: a list that keeps more than 80% of the triangles is refused (186 of 419
  meshes at grid 24 - low-poly meshes whose vertex spacing is already coarser than the
  cell). 65 more have under 8 triangles.
- **Surface area**: a list must keep 80% of the mesh's triangle area. Without it the
  fence's shadow became a sliver: a thin caster narrower than a cell flattens to a line
  and its triangles all go degenerate. A grid that fails the guard is doubled up to 256
  until one passes or the saving is gone. Eighteen meshes moved to the finer grids.

## Numbers, one frame of the ledger (which now carries each draw's topology)

| view | before (05:00) | after | lists |
| --- | --- | --- | --- |
| 1, the sun shadow | 313 draws, 77,100 tris | 313 draws, **58,830** (-24%) | 70 |
| 0, the reflection | 90 draws, 37,202 | 90 draws, **22,545** (-39%) | 47 |
| 3, the scene | 321 draws, 79,492 | unchanged | 0 |

Across the run: 168 lists built, triangles 44,153 -> 22,752 (52%) inside them.

## Verification

Consecutive frames with `bd_lod` flipped by `bd_ab_flag`/`bd_ab_period = 1` and
`bd_capture_frames = 2`: 0.33% of pixels differ by more than 24, and the difference map
is the wind-blown bushes plus one-to-two pixel shifts along shadow edges (the rock's
shadow boundary). The shadow shapes match; the fence, the posts and the character all
cast as before. (Cross-run comparison is impossible: the whole shadow band moves between
any two runs, as the 2026-09-03 note said; the first "the fence's shadow vanished" reading
came from the area collapse, confirmed by the guard fixing it, not from a cross-run pair.)

Within-run A/B over 100 s (period 120): `other_ms` 5.34 off / 5.43 on, `gpu_total_ms`
5.62 / 5.54. Flat on the desktop, as expected: the lists are for the Quest's vertex work
and the shadow pass's primitive setup, not for a 1080p desktop GPU.

## Addendum, 07:30: the scene view's distance LOD

`bd_lod_scene_distance` (300) and `bd_lod_scene_grid` (32, halved past twice the distance):
a direct node's replayed draw in the scene view takes the clustered list when the view
distance the walk published for the node is past the threshold. The node matrix's
translation is useless for this (terrain pieces carry identity matrices, the geometry is
in world space), so the walk's sphere-through-matrix distance is the source; render-list
entries have no published distance and stay full, as do skinned nodes. Village frame: 26
of 321 scene draws take a list, 79,492 -> 73,736 triangles (-7%). The per-frame on/off
pair differs in 1.4% of pixels: the far huts' outlines, the wind-blown bushes, the shadow
edges. The render-list entries (the majority of the far triangles) need the entry's own
depth for this to reach the 63%.

## Addendum, 07:30: what the overdraw is

The census keyed per visual and per view (this commit's `[frag]` report) reads 6.6 M
scene fragments on a 2.07 M pixel desktop frame, 3.2 a pixel, with four ground and rock
visuals and the sky dome as the owners. A probe that draws every blended depth-writing
draw opaque and sorts the queue front to back (`bd_debug_blend_off` + `bd_draw_sort`)
takes the scene view to 5.6 M: hidden overdraw that early depth rejection can remove is
a sixth of the fragments, not half. The rest is the game's visible layering (ground
pieces, detail patches and rocks over each other, all alpha one where they overlap in the
pixel history) and quad overshade from dense small triangles. No draw qualifies for
promotion at the texture level (`GuestTexture::alphaOpaque`): every ground and rock
texture carries partial alpha somewhere. RenderDoc's pixel history was not a usable
counter here: it listed only the clear for most pixels of a plain-draw capture.

## What is next for this

- The cook (stage 3) moves the build offline and keeps a proper decimator (edge collapse
  with quadrics) for the thin casters the clustering refuses; the live path stays for
  meshes the cook has not seen.
- Scene-view LODs by distance need the same machinery plus a per-draw distance, and a
  choice about the blended terrain layering (a coarser layer under a finer one shows).
- Merged statics for the 29-mesh ground visuals.

Sources: `src/gpu/scene/mesh_lod.cpp`, `src/gpu/scene/host_draw.cpp` (the replay's
`lod_grid` block), `src/gpu/hooks/draw.cpp` (`LedgerNote`, topology column),
`out/build/win-amd64-release/logs/draw_ledger.txt` frame 2852, perf CSV
`perf-20260903-235002.csv`.
