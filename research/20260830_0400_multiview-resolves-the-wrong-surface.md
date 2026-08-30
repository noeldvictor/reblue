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

## Root cause candidate: the layered framebuffers have no depth attachment

Checked, and it is not subtle:

```
[mv] LAYERED fb 1920x1080 rtLayers=2 ds=null dsLayers=0 -> viewMask=3
[mv] LAYERED fb  960x540  rtLayers=2 ds=null dsLayers=0 -> viewMask=3
[mv] LAYERED fb  480x270  rtLayers=2 ds=null dsLayers=0 -> viewMask=3
```

**Every layered framebuffer has `ds=null`, including the scene target**, while
`[mv] layered surface 1920x1080 layers=2 depth=true` says a two-layer depth surface was created. It
exists and is never attached.

A graphics pipeline built with a depth target format, bound into a render pass that has no depth
attachment, is a render-pass incompatibility. Vulkan calls that undefined; in practice it is silent,
produces no validation error and draws nothing - which is precisely the symptom, and precisely the
class of bug the comment a few lines above this log already records being found and fixed on the
*colour* side. The same mistake, one attachment over.

The post-chain framebuffers legitimately have no depth. The 1920x1080 scene one must.

### But `ds` is not null where it is chosen - the draw path is not building these

`ResolveEffectiveTargets` sets `ds = s.depth_stencil` and only discards it when it has no texture.
Logged at the moment a layered framebuffer is created:

```
LAYERED fb 1920x1080 rtLayers=2 ds=null | s.depth_stencil=0001BBCACCD0 tex=yes layers=2 1920x1080
```

**`s.depth_stencil` is a valid two-layer 1920x1080 surface with a texture, and `ds` still arrived
null.** That cannot come from `ResolveEffectiveTargets`, so these framebuffers are being built by a
path that passes `nullptr` explicitly - present's late clear does exactly that
(`GetFramebuffer(s, rt, nullptr)`).

Widening the log settles it: **40 layered framebuffers are created in a run and not one of them has
a depth attachment.** So the draw path never creates a layered framebuffer at all.

That is the thing to explain next, and it has two candidate shapes:

1. `rt->layers` is 1 when `BindDrawFramebufferLocked` runs, so the scene's draw framebuffer is built
   single-view and the multiview pipelines are bound into it - a `viewMask` mismatch, which is
   undefined and silent, and is the same failure the comment in this file already records for the
   colour side.
2. The scene's draw framebuffer is a **cache hit** from before the surface became layered.
   `GetFramebuffer` keys on `container->framebuffers[rt->texture]`, so an entry built while the
   target was mono is reused unchanged once it is not. Nothing in the key mentions the layer count.

Tested, and it is neither. Logging every `GetFramebuffer` call that has a layered colour target:

```
[mv] GetFramebuffer rt 1920x1080 layers=2 ds=null -> creating
[mv] GetFramebuffer rt  960x540  layers=2 ds=null -> creating
[mv] GetFramebuffer rt  480x270  layers=2 ds=null -> creating
[mv] GetFramebuffer rt  240x135  layers=2 ds=null -> creating
[mv] GetFramebuffer rt  120x67   layers=2 ds=null -> creating
```

No `CACHE HIT` anywhere, so (2) is out. And **not one call has a depth attachment**, so the draw
path never asks for a layered framebuffer *with depth* - which is what the scene needs.

## Retracted: that was a log-budget artifact, and the scene binds layered targets

The section that stood here concluded the scene was never rendered into a layered surface. **It is
wrong.** Logging the depth-tested binds directly:

```
[mv] scene bind rt 1920x1080 layers=2 | ds 1920x1080 layers=2
[mv] scene bind rt  480x270  layers=2 | ds  480x270  layers=2
```

The scene binds a two-layer colour target **and** a two-layer depth target. The earlier
`GetFramebuffer` log showed only `ds=null` calls because it was capped at ten and the present-path
calls, which legitimately pass no depth, consumed every slot before a scene bind was reached.

**That is the third wrong conclusion in this one investigation caused by a capped counter**, after
`[mv] N mono pipelines so far, 0 multiview` (which only prints at its 40th and 200th, both during
startup) and the framebuffer log before it. The lesson is worth more than the bug: **a bounded log
answers "what happened first", never "what happens".** Count into a total and print the total, or
filter to the case you care about before you cap, but do not read absence from a budget that
something else spent.

## Where it honestly stands

Everything checked is correct:

1. Surfaces are two-layer, colour and depth, scene and post chain.
2. The scene binds both of them.
3. Framebuffers get `viewMask=3`.
4. Pipelines are multiview, `viewMask=3`.
5. `PipelineState::multiview` follows the bound target.
6. The resolve runs on the scene target every frame (fixed this session).
7. The resolve pass and its draws execute.
8. Sampling through the surface's own known-good descriptor is equally black.

And the frame is still black. **I do not have the cause.** What is left unchecked is the one thing
none of these touch: whether the *geometry* reaches the layered target - whether the draws that
should fill it are being submitted at all under multiview, or are being culled, or are landing in
layer 0 of something that is then not what is read. The next probe should be a colour, not a
deduction: clear the scene target to magenta at the start of the frame and see whether the capture
comes back magenta, black, or the scene. That distinguishes "nothing is drawn" from "something is
drawn and then lost" in one run, and it does not depend on any counter.



**When the scene draws, `rt->layers` is 1.** The whole post chain is layered - 1920x1080 down to
120x67, all `layers=2`, exactly as `bd_stereo_multiview` intends - and the surface the guest binds
for the depth-tested scene pass is not. So the geometry goes into a single-layer target, the layered
surfaces never receive it, and the resolve dutifully flattens two empty layers.

That reframes the bug entirely. It is not the resolve, not the per-eye views, not the pipelines, not
the framebuffer masks and not the cache: **the scene is not being rendered into the layered surface
in the first place.** Every one of those was checked and cleared on the way to this.

The question for the next attempt is why `surface_pool` gives two layers to the post-chain targets
and one to the scene's, when `SetRenderTarget` was logged earlier binding a `layers=2` 1920x1080
surface and setting `PipelineState::multiview` true. Those two facts are in tension and one of them
is about a different surface than it appears to be. Start by logging, in one place, every surface
the pool hands out under multiview with its size, layer count and whether it carries depth.

## Superseded lead: the depth attachment The framebuffer log reads
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
