# Multiview: the pipelines are fine, the resolve runs on the wrong surface

2026-08-30. `bd_stereo_multiview` still presents a black frame, and the four causes CLAUDE.md
records as eliminated are all genuinely eliminated - but so is the fifth one everyone assumed.
Measured on the desktop build, no headset needed.

## What is actually happening

```
[mv] layered surface 1920x1080 layers=2 depth=false
[mv] framebuffer 1920x1080 rtLayers=2 dsLayers=0 -> viewMask=3
[mv] SetRenderTarget #2 surface=yes 1920x1080 layers=2 -> mv=true
[mv] MULTIVIEW pipeline created, viewMask=3          (x3)
[mv] built a resolve pipeline for format 10
[mv] resolved 960x540 to side-by-side (1 times)
[mv] resolved 120x67 to side-by-side (501 times)
errors: 0
```

Everything upstream is correct, and this contradicts the reading that was being worked from:

- Surfaces are allocated with two layers.
- Framebuffers get `viewMask=3`.
- `PipelineState::multiview` follows the bound target - `SetRenderTarget` reports `mv=true` for the
  layered 1920x1080 surface.
- **Multiview pipelines are created**, `viewMask=3`. An earlier session read
  `[mv] 201 mono pipelines so far, 0 multiview` as proof they were not, but that counter only prints
  at its 40th and 200th mono pipeline, both of which happen during startup before any layered target
  is ever bound. It was reporting a moment, not a state.
- No validation errors, no pipeline creation failures.

**The scene target is never resolved.** The resolve fires 501 times on a 120x67 bloom target and
once on a 960x540 one, and not at all on the 1920x1080 surface the scene is drawn into. So the
geometry does render into two layers, correctly, and the pass that flattens the pair into a
side-by-side image runs on everything except the surface that matters.

## Where to look

`ResolveMultiviewSurfaceLocked` is called from `src/gpu/hooks/draw.cpp:200`, gated on
`surf->layers > 1 && surf->multiviewDirty`, and `multiviewDirty` is set at `draw.cpp:226` on
`s.render_target` after a draw. The resolve therefore happens when the render target *changes away*
from a dirty layered surface.

That is the right shape, so the question is why the scene surface does not take it. Candidates, in
the order they are cheap to test:

1. The scene surface is still bound at present time, so the target never changes away from it and
   the trigger never fires. The bloom chain does change targets constantly, which fits exactly the
   observed pattern of small surfaces resolving and the big one not.
2. `multiviewDirty` is cleared by something else before the change is noticed.
3. The scene surface is presented from a different alias than the one the resolve sees - the surface
   pool hands out pooled textures, and `ResolveMultiviewSurfaceLocked` already carries a comment
   about stale views from exactly that.

(1) looks right, and the obvious fix does **not** work where it was first put.

Calling `ResolveMultiviewSurfaceLocked` from `SelectPresentSource` crashes with an
`ACCESS_VIOLATION`, at exactly the frame the capture was due. `SelectPresentSource` is a pure
selector - it decides which surface present should read and is evidently called outside the window
in which the command list is open for recording, so issuing a resolve there records into a list that
is not accepting commands. The resolve has to go somewhere the command list is provably still open
and the surface is still in `SHADER_READ`-able state, which means inside the present recording
itself rather than in the routine that picks its source. Reverted.

`RecordPresentPass` is the right place and the change is now in: it already issues barriers and
discards, so the command list is provably open. With it, the log goes from

```
[mv] resolved 120x67 to side-by-side (501 times)      # bloom, every frame
```

to

```
[mv] resolved 1920x1080 to side-by-side (501 times)   # the scene target, every frame
```

**The scene target is now resolved every frame, and the frame is still black.** So that was a real
bug and not the last one.

## What the debug cvars then eliminated

`bd_mv_debug_clear` paints the companion magenta before the copy draws - and the draws then run over
it, so a black result means the pass *and* the draws execute and simply sample nothing. It is not a
pass that never runs.

`bd_mv_debug_known_srv` makes the copy sample the surface's own descriptor - the one the rest of the
renderer uses and which is known to work - instead of the per-eye views this file registers. **Still
black.** So the per-eye view registration is not the fault either, which is the fifth candidate
eliminated and the one most people would have reached for.

### And it is not the wrong surface either

That was checked rather than assumed:

```
[mv] present rt=0001BBCB0310 1920x1080 layers=2 desc=163 | last_drawn=0001BBCB0310 layers=2
```

**Same object.** Present hands the resolve exactly the surface the scene drew into. Sixth cause
eliminated.

## Where it actually stands

Six things are now ruled out, each by measurement rather than reading:

1. Pipelines are multiview - `MULTIVIEW pipeline created, viewMask=3`.
2. Framebuffers carry `viewMask=3`.
3. `PipelineState::multiview` follows the bound target correctly.
4. The resolve pass and its draws both execute (`bd_mv_debug_clear`).
5. The per-eye views are not at fault (`bd_mv_debug_known_srv` is black too).
6. Present resolves the same surface the scene drew into.

So the layered surface genuinely has no content: the draws are issued, into a correctly-masked
framebuffer, with correctly-masked pipelines, and nothing lands.

**The next lead is the depth attachment.** The framebuffer log reads
`rtLayers=2 dsLayers=0 -> viewMask=3` - colour is layered and there is no depth attachment on that
framebuffer at all, while `[mv] layered surface 1920x1080 layers=2 depth=true` says a two-layer
depth surface was created. Vulkan requires every attachment in a multiview render pass to have at
least as many layers as the view mask needs, and a depth surface that is single-layer or absent
where the pipeline expects one is another render-pass incompatibility of exactly the kind that
already produced this symptom once (see the comment in `draw_framebuffer.cpp`, which records
precisely this class of bug being found and fixed for the colour side). Check what `ds` is for the
scene's framebuffer, and whether the depth surface it pairs with is the two-layer one.

## How to reproduce in 3 minutes, no device

`profiles/default/reblue.toml`:

```
bd_xr_autoplay = true
bd_stereo = true
bd_stereo_multiview = true
bd_mv_resolve = true
bd_capture_after_s = 140
```

then run `reblue_vk.exe`, and convert the capture. Black frame, and the `[mv]` lines above in the
log. `bd_mv_debug_clear` paints the companion magenta instead of resolving into it, which separates
"the pass never runs" from "the pass runs and the draw is wrong" - worth using before changing
anything, because this note exists precisely because that distinction was assumed rather than
measured.
