# The render list and the bones: most node draws were never tagged

2026-09-02, 20:40. Desktop village runs, one Quest run.

## The question

`[node] host-issued 111 of 366 node draws a frame (refused: ... 230 no template ...)`
with 184 templates in the store. Why did most nodes never get a template?

## What the runs said

A per-run diagnostic on `HostDrawCommit` printed the reason: every refused run was
`valid 1 replayable 1 draws 0` - the interpreter had issued **no draw at all** for
that node. Tallied per key over 300 frames:

```
[node] keys: 415 always empty, 0 mixed, 599 of 599; 319 untagged draws a frame
```

415 of the village's 599 (mesh, view, tech) keys draw nothing directly, never
mixed, and 319 draws a frame reach the queue with no node tag - the majority of
the scene pass.

## Where those draws come from

`bdSceneNodeDrawSingle` has one `bdSceneNodeDrawIndexed` site, but for a sorted
or translucent material the interpreter resolves the mesh's tokens into an
**entry of a global render list** and returns. `sub_8227F360` (called from
`bdSceneSubmitRenderList` and `bdSceneNodeSetupRenderPass`) sorts that list
(`sub_8227F290`, by depth) and draws every entry in one loop with four
`bdSceneNodeDrawIndexed` sites (fur shells, stencil passes).

The entry (`reblue_recomp.84.cpp`, loop at `loc_8227F520`, `r31` = entry):

| offset | field |
| --- | --- |
| +16 | world matrix, inline (`bdBuildViewMatrix(entry+16, 0, 0)`) |
| +252 | node index (foliage table index, `* 20`) |
| +268 | bone palette base |
| +272 | visual |
| +280 / +282 / +284 | u16 start, count, base (the draw parameters) |
| +286 | u16 |
| +288 .. +295 | pass flags; **291 and 294 move every frame** and the loop never reads them; 295 is scratch the loop clears |
| +289 | s8 bone count |
| +376 / +380 / +384 | vertex declaration, streams, indices |
| +800 | bone index table, u32 per bone |

## The hook

`config/hooks/render_list.toml`: a midasm hook at `0x8227F524` (the instruction
after `lwz r31,0(r14)`) with `jump_address_on_true = 0x8227FD28`, the loop's own
tail - the edge the guest takes itself for an entry whose technique is 3. The
body (`hooks/scene_node.cpp`) tags the entry, replays it from a template when one
exists (true skips the guest's iteration), or opens a capture that the next
entry or the function's return (`REX_HOOK_RAW(sub_8227F360)`) commits.

Identity is a hash of the geometry, draw parameters, pass flags and visual -
not the entry address (a pooled slot whose occupant changes with the sort) and
not the matrix. With bytes 291 and 294 in the hash the store grew to 9518 keys;
without them, 644. The keys of list draws carry a kind bit so they cannot
collide with a mesh VA.

Result, village:

```
[node] keys: 507 always empty, 0 mixed of 1301; 0 untagged draws a frame;
       render list: 195 of 245 entries host-issued
[node] host-issued 307 of 611 node draws a frame (... 103 never)
```

Frame verified by screenshot: rock behind the character, foliage, fences,
buildings, two shots identical.

## The bones

The 103 "never" were skinned nodes: `VertexShaderReplayable` refused any vertex
shader declaring c60..c151. The bone upload is a **gather**:
`bdSceneNodeDrawSingle` copies palette slot `idx[i]` (64 bytes at
`ctx.palette + slot * 64`, `idx` from the mesh's bone-index tokens on its stack,
at most 24) into `stack+1744` and calls `sub_82286BC0(60, buf, count)`, which is
`SetVertexShaderConstantFN(60, buf, count * 4)` and nothing else.
`sub_8227F360` does the same from the entry's table (+800) and palette (+268).

So a template stores the slot list - recovered by value against the palette for
a direct node (each 64-byte upload matched to a slot after the host's byte swap
and NaN flush), read from the entry for a list draw - and the replay gathers the
matrices live. c60..c155 leave the delta. Village:

```
[node] host-issued 350 of 659 node draws a frame (... 54 never); 876 templates
       render list: 238 of 292 entries host-issued
```

The character is intact in the screenshot with his 9 bones gathered by the host.

## What remains refused, per frame

- **274 "no template"**: the always-empty DrawSingle runs. They are not draws;
  they build list entries, and the interpreter has to run for them until the
  host builds the entries itself.
- **54 never**: foliage before the host's vector is trusted, and whatever list
  entries fail the slot check.
- **45 fresh values, 39 volatile** (33 textures, 6 fetch): materials whose
  texture pointer or sampler moves between sightings.

## The Quest run (render-list build, before the bones)

`verify_quest.sh` defaults, `out/probe/reblue_renderlist.apk`:

```
[node] host-issued 130 of 261 node draws a frame; render list: 86 of 110
5709 of 6213 field frames in one 60 Hz slot; gpu_total_ms p50 18.5 at 481 draws
```

The path runs on the headset without a crash, at 60 fps in that scene. **The
scene is not yesterday's**: 261 node draws against 535, and the capture is a
DoF-blurred close-up like yesterday's. The number is not a comparison and is not
quoted as one; the render-stage trace is the instrument for the next run.

## Sources

- `generated/reblue_recomp.84.cpp` `sub_8227F360` (loop at `loc_8227F520`)
- `generated/reblue_recomp.40.cpp` `bdSceneNodeDrawSingle` (bone loop at `loc_82280E1C`)
- `generated/reblue_recomp.23.cpp` `sub_82286BC0`
- `python tools/callgraph.py callers sub_8227F360`, `callers bdSceneNodeDrawIndexed`
- Commits `0cdb465` (render list), `1f22638` (bones)
