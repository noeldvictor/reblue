# Multiview: the scene renders in stereo. The composite is what is black.

2026-08-30, rewritten at the end of the session that produced it. **Read this section; the rest of
the file is the trail that got here and contains three conclusions that were later shown wrong.**

## What is actually true

**Multiview scene rendering works.** The scene's layered target holds two genuinely different views:

```
layer0 vs layer1, per-pixel absolute difference over 2,073,600 px
  mean 3.694   max 105   nonzero 23.20%
```

Bit-identical would be mean 0.000. Nearly a quarter of pixels differ, which is what a parallax shift
looks like.

**The final composited frame is black.** So the failure is entirely downstream of the scene: in the
resolve, the post chain, or present - not in the pipelines, the view masks, the shaders or the
constants.

Verified correct, each by measurement:

- All 55 vertex shaders carry the per-eye skew, before the return.
- All 55 compiled SPIR-V blobs carry **both** `OpCapability MultiView` and `BuiltIn ViewIndex`
  (XenosRecomp now dumps `.spv` beside the `.hlsl` for exactly this check).
- The separation constant is non-zero on 1,165,758 draws a run, `sep=0.03`.
- Surfaces are two-layer, colour and depth; the scene binds both; framebuffers and pipelines both
  carry `viewMask=3`; the device feature is enabled; `maxViewCount=32`.

## Two instrument errors that made this look like something else

Both cost hours and both are the same mistake in different clothing.

1. **The capture read the wrong surface.** `last_drawn_rt` is whatever bound last in a frame, which
   is the end of the post chain, not the scene. Every "the array is empty, max pixel zero" reading
   photographed the post output. `last_scene_rt` - the last bind carrying a depth attachment - fixes
   it, and the array turned out to be full.
2. **The layers were compared by mean, not per pixel.** A downsampled mean barely moves under a
   horizontal parallax shift, so two correctly different eyes read as "identical" - which matched a
   pre-existing note and looked like confirmation. A per-pixel difference says otherwise.

**Measure the thing, not a statistic of the thing, and check that the thing is the thing you meant.**

## Where to look next

The scene array is right and the composite is black, so: the resolve reads the array and writes the
companion; the post chain reads the companion; present reads the end of that chain. One of those
three loses it. `bd_mv_capture_array` now photographs the scene target, and the companion can be
reached the same way - point a capture at `rt->resolvedTexture` for the *scene* surface and the
question splits in one run.

Note the resolve was also fixed this session to actually run on the scene target - it previously
fired 501 times a frame on a 120x67 bloom target and never on the scene - so any older reasoning
about it predates a real bug being removed.

---

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

And the frame is still black.

## The probe, and what it narrows to

Clearing the layered scene target to magenta on **every** bind - a colour read out of the capture
rather than another counter - the capture comes back **black, not magenta**.

(The first attempt at this cleared once, on the first bind, roughly 140 seconds before the capture,
and proved nothing. Worth saying because it is the fourth time in this investigation that a probe
answered a different question than the one asked.)

So: the surface the scene is drawn into, cleared to a colour that cannot be missed, does not reach
the capture. And present logs `rt == last_drawn`, the same pointer. Those two facts together mean
the divergence is **after** the target - between the layered array and the companion the capture
reads:

- `RecordCapture` reads `rt->resolvedTexture`.
- `ResolveMultiviewSurfaceLocked` renders into `rt->resolvedFramebuffer`.

If those are not the same texture - if the companion framebuffer was built around a different
allocation than the one `resolvedTexture` points at, which a *pool* can easily arrange - then the
resolve writes one image and the capture reads another, and every check above still passes. That is
the next thing to verify, and it is one log: print the plume texture pointer behind
`resolvedFramebuffer` and the one behind `resolvedTexture` and see whether they match.

**Checked, and that is wrong too.** `surface_pool.cpp` builds `resolvedFramebuffer` directly from
`resolvedTexture` in the same block, from the same object - they cannot diverge. And
`bd_mv_redirect_srv` defaults **on**, so a multiview surface's primary sampled view is already
pointed at the resolved companion rather than array layer 0: the post chain, present and the capture
all read the same texture the resolve writes. That chain is self-consistent.

## The answer to "where does it go": nowhere. The array is empty.

`bd_mv_capture_array` (added for this) photographs the layered array itself instead of the companion
the resolve writes. present.cpp already had a both-slices path; it was only reachable when no
companion existed, so this makes it selectable.

Both layers, read separately out of the 1920x2160 stacked capture:

```
layer0: mean (0,0,0)  nonblack 0/576  max 0
layer1: mean (0,0,0)  nonblack 0/576  max 0
```

**Max pixel value zero.** Nothing is written to either layer, ever.

So the scene binds a two-layer colour target and a two-layer depth target, multiview pipelines with
`viewMask=3` are bound into a framebuffer with `viewMask=3`, the draws are submitted - and not one
pixel lands. Everything the resolve, the companion and the SRVs do downstream is faithful work on an
empty image, which is why nine correct checks explained nothing.

## Two more causes closed, from stderr

`stderr` - discarded on every run this session until now - was reporting the answer to two of the
open hypotheses all along:

```
plume: multiview feature ENABLED
plume: multiview maxViewCount=32 maxInstanceIndex=134217727
```

So the device feature is on (plume's patch works) and the view-count limit is nowhere near binding.
There are also **no `vkCreateRenderPass` or `vkCreateFramebuffer` failures** in the whole run, so
creation succeeds and the framebuffer is real.

## What to do next, and it is not more inference

This is a draw-time rejection that Vulkan is tolerating silently, and that is precisely what
validation layers report. This project has already used them for exactly this: CLAUDE.md records
that they "settled the multiview question in one run after three sessions of inference" on Android,
and that they immediately surfaced an unrelated live bug (`Int64` declared by every shader while
`shaderInt64` is not enabled) in the same run.

They are not currently run on the desktop build, and they should be. That is the next step - not
another probe reasoned from what a capture does not contain.

The likely shapes, for what it is worth, all of which validation names outright:

- A pipeline/render-pass incompatibility beyond the view mask - the depth attachment's format or
  sample count differing between the pipeline and the framebuffer.
- `multiviewGeometryShader` / `multiviewTessellationShader` or a `maxMultiviewViewCount` limit.
- The vertex shader writing `SV_ViewID`-dependent output without the `MultiView` capability actually
  enabled on the device, which plume was patched for and which is worth re-confirming here.

## The array that was "empty" was the wrong array

Logging the plume texture at both ends, which is the check that should have come first:

```
[mv] test-clear into guest 0001BBCB0710  plume tex 028000F27100
[mv] capture   from guest 0001BBCB0310  plume tex 028000F26020
```

**Different `GuestTexture` objects and different plume textures.** The scene is drawn into `B0710`;
present captures `B0310`.

The earlier check that "present `rt` == `last_drawn`" was true and misleading: both are `B0310`, and
*neither is the scene target*. `last_drawn_rt` is whatever bound last in the frame, which is the end
of the post chain, not the scene. So every capture in this investigation - including the one that
concluded "both layers empty, max pixel zero" - photographed the post-chain output.

**That invalidates the finding it produced.** The scene's layered array has not actually been looked
at. Multiview may be rendering correctly into `B0710` and failing somewhere in the post chain, which
is a completely different bug from the one this note has been chasing.

## With the capture pointed at the scene, the array is not empty

`last_scene_rt` now tracks the last colour target bound *with a depth attachment* - the scene, as
opposed to the post chain that binds after it - and `bd_mv_capture_array` photographs that:

```
layer0  mean (0,60,0)  max 60
layer1  mean (0,60,0)  max 60
```

**Both layers are populated, and they are identical.**

So the entire "the array is empty, nothing is drawn, not even a clear arrives" line was an artifact
of photographing the post-chain output. Multiview renders. What it does not do is render *two
different views* - which is the symptom CLAUDE.md recorded before this session began:

> the two layers come out *identical* - `SV_ViewID` is still not varying the skew, with a fresh
> shader cache and validation clean.

A day was spent walking past a correct diagnosis because the instrument pointed at the wrong
texture. The nine "verified correct" checks were all real and all beside the point.

## The shader side is correct, and the constant is non-zero

Checked, because "SV_ViewID is not varying" has two halves and only one of them is the shader.

**All 55 of 55 vertex shaders carry the skew** (`--target reblue_shader_hlsl_dump`), and it is
well-formed and before the return:

```hlsl
in uint iViewID : SV_ViewID,
...
const float eyeSign = (iViewID == 0) ? -1.0f : 1.0f;
oPos.x += eyeSign * (g_StereoSeparation - g_StereoConvergence * oPos.w);
```

So the "0 of 55 shaders" trap from a previous session is genuinely fixed, and DXC is invoked with
`-fspv-target-env=vulkan1.1`, without which `SV_ViewID` cannot lower to `ViewIndex`.

**And the constant is not zero.** Counting rather than sampling - which matters, given how many
capped logs misled this investigation:

```
[stereo] separation applied to 1165758 draws, zero for 2514242 (sep=0.03)
```

A third of draws get a real separation, which is the scene-geometry share the `stereoEligible` gate
is meant to select.

So: the shader declares `SV_ViewID`, uses it, is compiled for an environment that supports it, all
55 shaders have it, the constant feeding it is non-zero for scene draws, both layers render - and
the layers are identical. **What is left is `SV_ViewID` itself not varying between views at
execution**, which is below the HLSL: the SPIR-V's `BuiltIn ViewIndex`, or the driver's handling of
it.

Verifying that means reading the shipped SPIR-V. The HLSL dump does not compile standalone - it
carries an `#if` selecting between `vk::RawBufferLoad` and cbuffer `packoffset` forms and needs the
surrounding definitions - so the practical route is to decompress a shader out of the cache
(smol-v) and look for `BuiltIn ViewIndex`, or to have XenosRecomp dump the SPIR-V alongside the HLSL
it already dumps. **That is the next step, and it is a small tooling addition rather than a guess.**

## Where that leaves it

The bug is what it was said to be: **`SV_ViewID` is not varying per view.** That is in XenosRecomp's
emitted vertex shader and the per-eye constants feeding it, not in the resolve, the surfaces, the
framebuffers or the pipelines - all of which this session verified at length.

Two things are now better than they were, though:

- `bd_mv_capture_array` plus `last_scene_rt` means the question "what is actually in each eye's
  layer" is answerable in one desktop run, which it was not before. That is the instrument the
  `SV_ViewID` work needs.
- The shader-cache trap is worth re-reading before touching it (CLAUDE.md, "A XenosRecomp change
  needs two manual steps or it never reaches the device"). A previous session lost a day to the skew
  sitting in 0 of 55 shaders while the C++ looked right.

## Superseded: what to do with the wrong-array finding

1. **Capture `B0710`, not `last_drawn_rt`.** `bd_mv_capture_array` needs to select the surface the
   scene drew into - the last bind that had a depth attachment - rather than the last bind of any
   kind. That is a small change to the capture-source selection and it makes the question answerable
   for the first time.
2. Then re-ask the original question, because every answer in this note above rests on looking at
   the wrong texture. The nine "verified correct" checks are still correct - they were about
   pipelines, view masks, features and framebuffers, not about contents - but the two that were
   about *contents* (the empty array, and the magenta clear not arriving) are void.

The lesson is the same one this note has now recorded three times in different clothes: **verify that
the thing you are measuring is the thing you think it is, before believing what it says.** A pointer
comparison at both ends would have caught this at the start and cost one line.

## Closing state, honestly

Nine hypotheses checked, nine correct, frame still black:

surfaces layered (colour and depth) · scene binds both · framebuffer `viewMask=3` · pipelines
`viewMask=3` · `PipelineState::multiview` tracks the target · resolve runs on the scene target every
frame · resolve pass and draws execute · known-good descriptor samples black · companion texture,
its framebuffer and the sampled SRV are all the same allocation.

Plus the decisive negative: **magenta-clearing the layered scene target every bind does not reach the
capture.** Something between a cleared array and a companion that everything agrees on loses the
image, and none of the nine explains it.

I do not have the cause, and four probes in this investigation answered different questions than
they were asked - three capped counters and a one-shot clear 140 seconds early. Anyone picking this
up should distrust the instrumentation before the theory, and should probably start by making the
*array* visible directly: capture `rt->texture` layer 0 and layer 1 with
`bd_capture_after_s` (present.cpp already has a both-slices path for exactly this) rather than
inferring what is in them from what comes out of the resolve.



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
