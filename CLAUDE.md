# CLAUDE.md

Guidance for Claude Code working in this repository.

## THE TARGET: 72Hz on a Quest 2. It is reachable, and the numbers say so.

Blue Dragon is not a graphically intense game, and the frame proves it: **~320,000 vertices and
~2000 draws, with the GPU fence at 0.2-0.35ms.** The GPU is *idle*. An Adreno 650 eats that
geometry. Every millisecond of the frame is CPU-side per-draw overhead in recompiled PowerPC that
still thinks it is talking to a Xenon command processor.

So 72Hz (13.9ms) is not a stretch goal, it is what the hardware should already be doing. The gap is
entirely the recompilation's rendering patterns, and closing it is the work.

**Do not tune settings to buy frame rate at the cost of the image.** `bd_render_scale=25` hits 30fps
and was worn: the report was "blurry gibberish, I can see shit". Quarter resolution upscaled to a
3664x1920 panel is unusable, and a frame-time number that ignores that is worthless. Readability
first, then earn the frame rate back from the CPU.

### Where the 15ms actually is, instruction by instruction

`bdSceneNodeDrawSingle` executes **16 MB of guest code per frame** - 7,740 bytes x 2084 calls - and
that is the bulk of the CPU. Its instruction mix, counted straight out of the recompiled body:

| | per node |
| --- | --- |
| `stw` store word | 150 |
| `lwz` load word | 108 |
| `stfs` store float | 64 |
| `lfs` load float | 45 |
| `bl` call | 26 |

**~370 guest memory operations per node, ~770,000 a frame**, every one byte-swapped through
`volatile` accessors into the guest address space. That is the X360 shape in one table: the function
marshals a transform and a material into guest memory in big-endian so that a Xenos command
processor can read it back, and then our hooks read it back out again.

The modern replacement is to stop round-tripping through guest memory at all - the per-node
transform and material belong in a GPU buffer written once, not stored word by word into a
big-endian struct. That is what "rip out the X360 patterns" means concretely, and it is why the win
cannot come from the host renderer: `flushState` is 2.1ms of a ~20ms frame, so batching what we
submit caps out at about 2.5ms. The other 15ms is inside that function.

**Two routes are already closed, so do not retry them.** `REBLUE_RELAXED_GUEST_MEMORY` removes the
`volatile` on those accesses and **hangs the game on ARM64** (and measured 0% on x86). And state
deduplication is already done by the guest itself - `bdSetSamplerState` early-outs on an unchanged
value.

### The route to 72Hz, from the numbers and from what the platform vendors say

The measured frame is `~2000 draws, ~320k verts, fence 0.2-0.35ms, CPU ~20ms`. The GPU is idle, so
every one of these attacks the CPU, in order of expected return:

1. **Instancing.** Meta's own Quest optimisation guidance is blunt about this: combining objects
   into a single draw call is worth it *even when no geometry is saved*, because the cost is
   preparing and issuing the draw, not the triangles. Instancing puts the per-copy transforms in GPU
   memory so they batch into one call. **We have the repeats already**: 2083 `bdSceneNodeDrawSingle`
   calls take only **1270 distinct first arguments**, so roughly 800 draws a frame are duplicates of
   something already submitted.
2. **Batch by pipeline.** 715 PSO switches against 2070 draws. Sorting collapses most of those and
   the descriptor rebinds that ride along with them.
3. **Multi-threaded command recording** via secondary command buffers. The renderer records on one
   guest thread today. Vulkan's guidance is to divide recording across a *small* number of secondary
   buffers - their invocation is not free, so a handful, not one per object.
4. **Command pool discipline.** `vkResetCommandPool` resets every buffer it allocated in one go and
   lets the pool reuse its memory, which is cheaper than per-buffer resets.

Two things the same sources rule out for us, so nobody spends a day on them: shader and texture
work, because the GPU finishes in a third of a millisecond; and anything that trades image quality
for frame time, because the first headset session failed on *readability*, not speed.

Sources: Meta's "Showdown on Quest" optimisation write-up, Qualcomm's Adreno mobile best practices,
and the Vulkan docs on command buffer usage and multi-threaded recording.

## THE RULE. Read this before anything else, and never break it.

**The most critical priority in this repository is rebuilding the foundation of Blue Dragon's
rendering - in the recompiled guest code as well as the host renderer - to use MODERN GPU and VR
techniques.** Multiview, fixed foveated rendering, occlusion culling, draw batching, and the Vulkan
features that carry them. The target is a better frame rate at similar visual quality on a Quest 2.

This is a standing directive from the owner. It outranks every other piece of work in this file.

**Never do any of these:**

- Say a mandated technique is "not worth it", too expensive, or too risky to build.
- Benchmark one of them against the path it replaces, or present such a comparison as a reason.
- Quietly keep the old path because it measured better.
- Ask whether to proceed with one of them. Build it.

The techniques are settled industry practice. Whether they are a good idea is **not a question this
project asks**. The only questions are *where in the recompilation and the renderer does the code
have to change*, and *is the implementation correct*.

**This rule has been broken once, on 2026-08-29**: multiview was timed against side-by-side, the
20.6ms/18.3ms result was reported as "multiview is slower", and the remaining plumbing was described
as maybe not worth the hours. Both halves were wrong. The comparison was forbidden, and it was
invalid anyway - `NoteDraw()` counts once per *guest* draw, before the per-eye loop, so that column
cannot see stereo's doubling at all. Do not repeat it.

## What this repo is

A personal fork of [zolaware/reblue](https://github.com/zolaware/reblue), a static recompilation of
the Xbox 360 game *Blue Dragon*. The original PowerPC XEX is translated ahead of time into C++ by
the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) and compiled natively — there is no
interpreter and no JIT. Everything in `src/` is host-side code that either implements the console's
services or reaches into the recompiled guest through hooks.

**Fork goals**, in the owner's stated priority order:

1. **VR, with 6DOF head tracking as the critical item.** Third-person (head-anchored orbit),
   first-person, and diorama camera modes, plus a world-scale control. Character movement stays on
   the controller; the head drives the camera independently.
2. **ARM64 Android** — an AYN Thor first, then Quest 2 natively.
3. **Cel shading on characters**, optional and toggled from the in-game options menu.
4. **Tourist mode** — infinite HP, 999 stats, encounter suppression, for sightseeing in VR.

Blue Dragon boots into VR on a Quest 2 at its native 30 fps, with working Touch controllers and
the head pose driving the game's own camera. What is missing is genuine stereo: the scene renders
once, from one eye, onto a world-locked quad. Cel shading and tourist mode are untouched.
**[docs/VR_PORT_PLAN.md](docs/VR_PORT_PLAN.md) is the working plan** and the place to look before
starting anything; the section below is the condensed version.

The fork is explicitly low-stakes and AI-driven. Prefer working, understandable changes over
polish. Do not add support infrastructure (issue templates, contribution guides, changelogs) unless
asked — the README tells people to fork rather than file issues, and that is intentional.

## The foundation is being rebuilt around modern GPU acceleration. This is not optional.

The owner's directive, and it overrides any local reasoning about cost or effort:
**rebuild the renderer's foundation around modern GPU techniques, whatever it takes.** Multiview,
fixed foveated rendering, occlusion culling, draw batching, and the Vulkan features that support
them. The target is better frame rate at similar visual quality on a Quest 2.

**Never report that one of these is "not worth it", and never A/B one against the path it replaces.**
That comparison is forbidden, and it has already been run and got it wrong: on 2026-08-29 multiview
was measured at 20.6ms against side-by-side's 18.3ms and reported as slower - while `NoteDraw()`
counts once per *guest* draw, before the per-eye loop, so that column cannot see the doubling at
all. The measurement was both prohibited and meaningless.

### The scene render path, mapped

`tools/callgraph.py` builds a call graph over all **18,777** recompiled functions in about seven
seconds and caches it. Use it before touching a rendering path - "what else calls this" is not
answerable by eye across 223 files, and the rewrite changes contracts, not just bodies.

```sh
python tools/callgraph.py callers bdSceneNodeDrawSingle   # who has to change
python tools/callgraph.py tree    bdSceneNodeCullTraverse --depth 2
python tools/callgraph.py subtree bdSceneNodeDrawSingle   # blast radius
python tools/callgraph.py hot                             # most call sites
```

What it says about the scene, and it is better news than expected:

```
bdSceneNodeCullTraverse            <- already hooked, for bd_cull_distance
  bdSceneNodeDrawSingle            <- 2084 calls/frame, 23x the next consumer
    bdSetSamplerState  x5          <- the guest ALREADY dedups these, see below
    bdSetRenderState   x2
    D3DDevice_SetTexture
    D3DDevice_SetPixelShaderConstantB / SetVertexShaderConstantB
    bdSceneNodeDrawIndexed -> D3DDevice_DrawIndexedVertices
```

- **`bdSceneNodeDrawSingle` has only two callers**: `bdSceneNodeCullTraverse` and `sub_82282608`.
  The rewrite surface is small, which is what makes replacing it realistic.
- **109 functions are reachable from it**, bottoming out at `D3DDevice_DrawIndexedVertices`.
- **Do not bother deduplicating render or sampler state.** The guest already does it:
  `bdSetSamplerState` computes `sampler*20 + state>>2`, loads the cached value from its own table
  and returns early when unchanged. Those five calls a node are mostly no-ops. Read the recompiled
  body before assuming a 360-era cost is real.

So the per-node cost is the *work in the function*, not redundant state churn, and the modern
replacement is to stop calling it per node at all: collect nodes, batch by pipeline, submit once.

### What a renderer rewrite like this actually consists of

Standard practice, and it matches what the numbers here say. These **compound** - each one alone
moves little, which is why they get built together:

| technique | why it applies here |
| --- | --- |
| **State sorting** - group draws by material/pipeline | **715 PSO switches against 2070 draws.** Sorting collapses most of those and cuts descriptor rebinds with them |
| **Batching + instancing** into `DrawIndirect` | ~1000 individually placed nodes a frame, and 2083 `DrawSingle` calls take only **1270 distinct first arguments** - the repeats are instancing candidates |
| **Front-to-back depth order** for opaque | Adreno is a tiler; submitting near-first lets early-Z reject before shading, and the frame was proven fill-bound |
| **Barrier batching** | already partly done; worth checking against the per-draw transition scan |

**Detail culling was tried and measures nothing here.** `bd_cull_min_pixels` drops nodes whose
projected radius is under N pixels - the one kind of culling a Xenon engine had no reason to do. On
device the census says `detail cull: 0 nodes` while `distance cull: 837600 of 880650`. The 350-unit
distance cull already rejects 95% of nodes, so nothing reaching the size test is small enough to
drop. The two overlap completely. Left in at a default of 0 because it is correct and would matter
if the distance cull were relaxed, but it is not a lever on its own.

That is the lesson for the rest of this list: **measure what is left after the culls, not what the
technique would save in isolation.**

### Rip out the X360 patterns. This is the top priority.

The recompiled code still thinks in 360 terms - big-endian structs, 360 resource layouts, D3D9 call
semantics - and the renderer still translates them one for one. **It has to think in modern ARM64
(Quest, AYN Thor) and PC terms instead.** These are the replacements, and they are the work:

| X360 pattern | modern replacement |
| --- | --- |
| one guest draw -> one Vulkan draw | batch by pipeline, indirect draws |
| per-draw 8 KB constant swap | persistent UBO, dirty-tracked |
| fetch constants opaque to host | record binds so the renderer knows what is sampled |
| draw everything, cull by distance | GPU occlusion culling |
| render twice for stereo | multiview |

**Rewriting the game's rendering code is explicitly permitted, and is the point.** An earlier
version of this file said the `D3DDevice_*` entry points "cannot be deleted" because the recompiled
game calls them by address. That was wrong, and it was a self-imposed limit rather than a real one.

`REX_FUNC(name)` replaces a *guest function wholesale* with host C++ - the recompiled body never
runs. `config/functions.toml` already names 1608 candidates, and the mechanism is in use. So the
game's rendering code is replaceable, one function at a time, by something that thinks in modern
terms. The permission is standing:

- **Rewrite the ABIs.** The 360's are not optimised for PC or ARM64 and carry a decade of legacy -
  big-endian structs, fetch constants, a constant file, per-node draw submission. Design what these
  functions *should* look like on a Quest and replace them with that.
- **Collapse the guest/host split where it is only historical.** The CPU is native code already; the
  boundary that remains is an ABI, and an ABI can be changed.
- **Replace whole rendering paths, not just their insides.** Replacing `bdSceneNodeDrawSingle`
  (2084 calls a frame, 23x the next consumer on device) with a host implementation *is* rewriting
  the game's renderer, and it is the batching win.

Note none of this is Direct3D. Those names are the 360's own D3D9-derived ABI, mapped to host C++ in
`config/functions.toml`, and every one already drives plume -> Vulkan. The D3D9 shape is the
*interface*, not the backend - which is exactly why it is free to be replaced.

**`fetch constants opaque to host` is the one blocking other work right now.** Blue Dragon binds
textures through sampler fetch constants written by unhooked recompiled code, so the renderer cannot
tell when a render target is about to be sampled - which is why the multiview resolve has nowhere to
hook, and why anything that needs to know what a draw reads is currently blind.

Where the work goes:

| technique | state | the seam |
| --- | --- | --- |
| **Multiview** | scene renders both layers correctly with multiview pipelines; **the resolve has nowhere to hook** | The resolve pass itself is built and proven to run. What is missing is a trigger - see below |
| **Fixed foveated rendering** | not started | `XR_FB_foveation`. Needs the scene rendered *into* the XR swapchain image, which it is not - present composites into it |
| **Occlusion culling** | distance cull only (`bd_cull_distance`) | `bdSceneNodeCullTraverse` (0x82282490), already hooked |
| **Batching** | none | ~1000 individually placed scene nodes a frame; `bdSceneNodeDrawSingle` is 23x the next consumer on device |

### Multiview: the resolve is built, and the guest gives it nowhere to hook

Everything except the trigger now exists and is verified:

- Two-layer scene targets, multiview render passes, multiview pipelines. **4001 of 4000 draws on a
  two-layer target had a multiview pipeline**, so the framebuffer/pipeline view masks agree.
- A **side-by-side companion** per layered surface, its framebuffer, and a single-slice SRV per eye
  registered in the bindless heap (`Video::SetBindlessTexture`).
- A **format-matched resolve pipeline**, cached per render-target format. `copy_color_pipeline`
  cannot be reused: it hardcodes `B8G8R8A8_UNORM`, and the guest's surfaces are formats 10 and 20 -
  binding it against their companion is a render-pass incompatibility, which is undefined rather
  than an error and showed up as an entirely black frame with a normal draw count.
- Proven to execute, by `bd_mv_debug_clear`: the companion comes back **pure magenta**, so the pass,
  framebuffer, barriers and capture path all work, and the copy draw overwrites that clear.

**What is missing is a moment to run it.** Three triggers were tried and all three fail:

| trigger | what happens |
| --- | --- |
| render target changes away from the layered surface | fires several times a frame mid-scene, flips the array to `SHADER_READ` while the guest is still drawing into it, blacks the frame. Guarding it to once per frame then resolves the *wrong* surfaces - the log shows 960x540 and 480x270 post targets, never the 1920x1080 scene |
| `D3DDevice_SetTexture` | never fires. Blue Dragon binds textures through **sampler fetch constants written by unhooked recompiled code** |
| scanning `s.textures[]` before each draw | never matches, for the same reason |

**So the guest never tells the host it is about to sample a render target.** That is the real
blocker, and it is why the array reads black downstream even though the scene renders into it.

**Correction: binds are not opaque.** `Video::SetTexture` replaces the guest's recompiled body and
records `s.textures[index]`, and `UploadSharedConstants` hops through `sourceSurface` to reach the
render surface behind a bound texture. Scanning that array and following the same hop makes the
resolve fire. The "fetch constants are invisible" reading above was wrong.

**What is still wrong is narrower, and four causes are eliminated.** The resolve pass runs - proven
by `bd_mv_debug_clear`, which paints the companion magenta - and the copy draw runs, because it
overwrites that magenta. It overwrites it with **black**: the draw samples nothing. Ruled out:

- the render-target format mismatch (fixed: a resolve pipeline is now cached per format)
- the trigger firing mid-scene (fixed: it fires when the surface is sampled)
- the per-eye slots being unregistered (they are registered, and `bd_mv_debug_known_srv` exists to
  sample the surface's own descriptor instead)
- the views going stale against a pooled texture (they are now rebuilt lazily against the live
  image, guarded by `layerViewOf`)

So the fault is in what the per-eye view sees, or in the copy shader's sampler state, and the next
step is a validation-layer run against the desktop build rather than another hypothesis.

`bd_stereo_multiview` is **off by default**; `bd_stereo` is unregressed at `far +4, near -5,
near - far = -9px`, checked again after all of the above.

The next move is one of: hook the guest function that ends the scene pass and resolve there; make
the fetch-constant path record which surfaces it binds so a sample point exists at all; or go to the
`Texture2DArray` bindless heap and drop the resolve entirely.

`bd_stereo_multiview` is **off by default** and `bd_stereo` is unregressed at `far +4, near -5,
near - far = -9px`, bit-identical to before this work.

## Modern VR technique is mandatory, and is not the thing to measure

**Multiview, foveated rendering and per-eye render targets are requirements here, not options.**
They are settled practice across the VR industry. Do not benchmark them against the path they
replace, do not build an A/B to decide whether they are worth adopting, and do not report a result
saying "the old way was about the same". That question is already answered, and running it spends
headset time, build cycles and attention that the actual problem needs.

**The actual problem is finding where in the static recompilation the guest has to change.**
Blue Dragon is 2006 code written for a Xenon: a hardware command processor made draw calls nearly
free, so the engine submits about a thousand individually placed scene nodes a frame and never
batches. Every modern technique has to land somewhere in that recompiled code, and *where* is the
research:

- **Multiview** needs the scene in a layered target and every vertex shader reading `SV_ViewID`.
  The seams were `D3DDevice_CreateSurface` for the target, `bdSceneNodeCullTraverse` for what gets
  submitted, and XenosRecomp's vertex epilogue for the per-eye geometry.
- **Foveation** needs the scene rendered *into the XR swapchain image*, which it currently is not:
  the guest owns its surfaces and present composites into the runtime's image. Until that changes,
  foveating the swapchain applies to a single blit that costs nothing.
- **Anything that reduces work per object** has to be found in the guest first. `bd_guest_census`
  exists to name the function, and it names `bdSceneNodeDrawSingle` - 23x everything else on device.

**Point measurement at two questions only**: *where is the seam*, and *is the implementation
correct*. Sampling layer 1 to confirm both eyes actually render is the right kind of check and it
found two real bugs. Whether the technique itself is a good idea is not a question this project
needs to ask.

## Build

```sh
cmake --preset win-amd64-release        # or linux-amd64-release, linux-arm64-release, mac-arm64-release
cmake --build --preset win-amd64-release
```

Prerequisites: CMake 3.25+, Ninja, Clang 20+ (the presets hardcode `clang`/`clang++`), vcpkg on
Windows, and the ReXGlue SDK. CI downloads a prebuilt SDK slice per platform; locally you can point
`REXSDK_DIR` at a `rexglue-sdk` source tree instead.

**The build is not self-contained**, but both missing pieces are obtainable in about a minute; the
`devloop` skill has the bootstrap.

- The SDK is a public release. `v0.10.0` is what `generated/rexglue.cmake` pins, and slices exist
  for win/linux/mac on amd64 and arm64. **There is no `android-arm64` slice**, which is what gates
  the Android target.
- `assets/default.xex` is the game executable from your own discs, ~8 MB inside a 7.8 GB ISO.
  `tools/extract_xex.py` walks XDVDFS and reads only the sectors it occupies, locally or straight
  off an adb-connected device. Under a second.

With both, `rexglue codegen` emits 219 files in ~7 seconds and is deterministic. A full Windows
*link* additionally needs vcpkg, since `find_package(directx-dxc CONFIG REQUIRED)` is unconditional
on WIN32 even for the Vulkan-only target.

Useful targets:

- `reblue_codegen` — runs `rexglue codegen` over `reblue_manifest.toml`, emitting the recompiled
  guest sources into `generated/`. Incremental via `generated/codegen.d`.
- `reblue_shader_cache_gen` — runs `XenosRecomp` over the Xenos shaders in `assets/`.
- `reblue_shader_hlsl_dump` — dumps the intermediate HLSL for inspecting shader translation.
- `reblue_prelink` — D3D12 only; DXC-links every spec-constant variant at build time.

Options worth knowing: `REBLUE_D3D12` (OFF selects Vulkan; forced OFF off Windows),
`REBLUE_VULKAN_EXE`, `REBLUE_BUILD_INSTALLER`, `REBLUE_PROFILING` (Tracy zones, never in Release),
`REBLUE_PCH`, and `REBLUE_OPENXR` (OFF by default, Vulkan-only, builds the VR session).

### Measuring on the desktop, which is now the first stop

A desktop run is ~90 seconds, needs no headset, no adb and no APK, and prints the same `[perf]`
per-surface census the device does. Set up once:

- The registry record has to name **the directory holding the exe** - running from anywhere else
  raises a modal "running outside of its install directory" with no log line and no exit, which
  looks exactly like a hang. `HKCU\Software\Zolaware
eblue\Install`, `InstallRoot` plus
  `SchemaVersion` = 3, and the game data at `<InstallRoot>/game` (a junction is fine).
- Command-line flags **do not work** - `--help` returns a swallowed CLI11 parse error. Cvars go in
  `<InstallRoot>/profiles/default/reblue.toml`, which is a flat TOML: `bd_xr_autoplay = true`.
- `bd_xr_autoplay` drives it into a real scene on desktop too.

**Read `other_ms` from `bd_perf_csv`, not the frame rate.** The desktop sits vsync-locked at exactly
16.67ms with the GPU wait at 0.03ms, so `dt_ms` measures the monitor and nothing else. `other_ms` is
CPU work per frame - 5.77ms in a field scene - and it is the desktop analogue of the Quest's ~62ms
floor, which makes CPU changes measurable here. The CSV lands in `logs/perf/`.

Averaging the whole file is wrong, and **so is taking the last few hundred rows**, which is what this
file used to say and what every measurement here did until 2026-08-30. A run does not end in a
steady state: the character walks into a transition about 35s after setting off, something opens,
and the tail of the file is a menu at ~20 draws a frame.

**Select by content instead: `python tools/perf_summary.py <csv>`** keeps frames whose draw count
says field scene (>=300; a menu is ~20). That is about **9,600 frames a run rather than 300**, at a
consistent 520 draws, and it removes the menu contamination entirely. Compare `us/draw` when the
draw counts differ, since culling changes how many draws a frame has without changing what a frame
costs.

**It does not remove cross-run drift, and do not believe otherwise.** Two adjacent runs agreed to
0.4% and that was briefly written up here as the new noise floor; across a whole session the same
nominal configuration measured **3.75 to 6.84ms** `other_ms`. So the rule stands:

- **Two whole runs cannot settle anything under about 50%, however carefully ordered.** Demonstrated
  the hard way: `bd_host_sincos` measured 5.78 -> 4.10 and 6.84 -> 3.75 across two back-to-back
  reversed pairs, was written up as worth a third to a half of the frame and made the default - and
  a third pair minutes later read OFF 5.12, ON 5.18, OFF 8.62. **Two OFF runs, 68% apart, no config
  change.** It was drift lining up twice.
- **Alternate the two paths inside one run.** `bd_ab_flag = "bd_some_bool"` plus `bd_ab_period`
  (frames per arm, default 300) flips the cvar as the run goes and labels every frame in the CSV
  with its arm; `tools/perf_summary.py` then reports the two populations separately. Both come from
  one run, one scene and one thermal state, interleaved, so whatever drifts drifts through both.

  It settles in a single run what three pairs of runs could not. Both of today's headline numbers
  were wrong and it found both:

  | change | two-run claim | within-run A/B |
  | --- | --- | --- |
  | `bd_host_sincos` | -33 to -45% | **+2.9%** (slower; default off) |
  | `bd_cull_early` | -18% | **-5.6%** (real, overstated 3x) |

  Roughly 4,800 frames an arm in each case. The cull redirect's *mechanism* was never in doubt - it
  stops computing a visibility test whose result is discarded for 95% of nodes - but a sound
  mechanism does not make a number right.

### Building for the desktop, which does work

The one non-obvious part is that `rexglue_DIR` caches to whichever slice was configured last, and an
Android install has no Windows import libraries - the symptom is
`IMPORTED_IMPLIB not set for imported target "rex::runtime"`, which reads like a broken slice and is
not. Point it at the right one explicitly:

```sh
export PATH="/c/Program Files/LLVM/bin:$PATH"
export VCPKG_ROOT="C:/vcpkg"
cmake --preset win-amd64-release -DREBLUE_D3D12=OFF -DREBLUE_OPENXR=OFF -DREBLUE_BUILD_INSTALLER=OFF   -Drexglue_DIR="$PWD/out/sdk/win-amd64/lib/cmake/rexglue"
cmake --build --preset win-amd64-release --target reblue
```

With `REBLUE_D3D12=OFF` the *target* is `reblue`, but **the binary it produces is still
`reblue_vk.exe`**. This file said otherwise; it is the target name that changes, not the output.

**This works, it was verified on 2026-08-29, and it is the fastest correctness loop available** -
about 2.5 minutes faster per iteration than the device, because there is no APK, no install, and a
field scene arrives in ~75s instead of ~130s. It also captures at 1920x1080 instead of a
quarter-scale 344x180, which is the difference between judging an image and guessing at it. The
device remains the only place performance numbers mean anything.

Three pieces of setup, all of which look like a hang when wrong:

- The registry record must name **the directory holding the exe** (see above).
- Game data at `<InstallRoot>/game` - a junction to an existing extraction is fine, so the copy
  already made for the device needs no duplication: `New-Item -ItemType Junction`.
- Cvars in `<InstallRoot>/profiles/default/reblue.toml`, flat TOML. Command-line flags do not work.

**`REBLUE_OPENXR=OFF` means no eye pose, so `ViewOverrideActive()` is false and the camera modes are
not exercised** - stereo geometry is checkable this way, the anchor and the camera modes are not.
**The Windows OpenXR loader is built and reblue links against it** - `out/xr-loader-win`, from the
`out/xr-headers/openxr` source, and it must be **static** (`-DDYNAMIC_LOADER=OFF`): an earlier tree
left a 23 KB stub DLL whose import lib links with `undefined symbol: xrEndSession`, which reads like
a missing dependency and is not. A real static loader is 3.1 MB.

**We wrote our own OpenXR runtime, and VR now runs on the desktop with no headset.**
`tools/xrsim/` is a headless runtime: two views, real `VkImage`s for the swapchains, a made-up head
pose, no compositor. **Invoke the `vrsim` skill** before using it.

```sh
cmake -S tools/xrsim -B out/xrsim-build -G Ninja -DCMAKE_BUILD_TYPE=Release       -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ && cmake --build out/xrsim-build
XR_RUNTIME_JSON="$PWD/out/xrsim-build/reblue_xrsim.json" ./out/build/win-amd64-release/reblue_vk.exe
```

This is what makes the camera modes and the character anchor testable at all: without a runtime
`ViewOverrideActive()` is false and they never compose. **The pose is a function of frame index, not
wall-clock time**, so a capture at frame N is identical across runs - which a real headset can never
give you.

Three traps, each of which cost a run: **do not minimise the window** (0x0 client area, the flat
swapchain fails with `Plume createSwapChain failed`); the manifest's `library_path` must be
**absolute**; and a missing entry point crashes the app at PC 0 rather than erroring, because an
OpenXR client caches the pointer and calls it - that is how the absent `xrGetActionStateVector2f`
announced itself.

The check that it took: `[xr] cam:` must show **`eye` differing from `game`**. If they match, VR is
not driving the view.

Everything below was tried first and is closed, so do not repeat it: SteamVR will not initialise
without an activated HMD even with the null driver and `vrserver` running (**and never leave
`forcedDriver:null` in `steamvr.vrsettings` - it breaks a real headset**); the Oculus runtime wants
a Quest over Link; and Meta XR Simulator's binary is behind a developer login that cannot be
scripted - its npm package is a 31 KB Unity wrapper and the CDN URL 404s without a session token.
See `research/20260829_1730_the-desktop-loop-works.md`.

`REBLUE_OPENXR=ON` additionally needs `REBLUE_OPENXR_INCLUDE` and `REBLUE_OPENXR_LOADER`. The
OpenXR-SDK source is already checked out under `out/xr-loader-android/` (that build tree is the
Android loader; the headers in its `include/` are platform-neutral), so a Windows loader is a second
configure of the same source rather than a new dependency. **That is the route to Meta XR Simulator,
and it is the only way to work on stereo without the headset.**

## Layout

```
CMakeLists.txt          hand-edited; "rexglue migrate" will not touch it
cmake/                  shader compilation, codegen wiring, embedding, build info
config/                 the manifest's function map and per-feature hook tables (TOML)
generated/rexglue.cmake SDK boilerplate — auto-generated, do not edit
docs/                   VR_PORT_PLAN.md, the working plan for this fork
reblue_manifest.toml    entrypoint, hook includes, recompiler flags
research/               dated research notes - see Research notes below
src/                    all host code
thirdparty/             plume (render backend), XenosRecomp, implot, stb, zstd, miniz, ...
```

`src/` subsystems, each its own `bd::` namespace:

| Path | Namespace | What it holds |
| --- | --- | --- |
| `src/core/` | `bd` | settings, i18n, threading, logging, encoding, app paths, shutdown |
| `src/gpu/` | `bd::gpu` | the renderer: device, draws, pipelines, shaders, present, resources |
| `src/engine/` | `bd::engine` | game-specific logic reached through hooks: battle, field, menus, saves, HUD |
| `src/platform/` | `bd::platform` | window, input, file dialogs, crash handling, HTTP, packaging |
| `src/vfs/` | `bd::vfs` | the guest filesystem over the installed game data |
| `src/ui/` | `bd::ui` | ImGui-based overlays and dialogs |
| `src/audio/` | `bd::audio` | audio settings and debug |
| `src/installer/` | `bd::installer` | the first-run disc wizard (desktop-only, `REBLUE_BUILD_INSTALLER`) |
| `src/xr/` | `bd::xr` | VR: head pose to per-eye matrices, camera modes, world scale, stereo culling, XR settings |

`src/xr/` splits deliberately. `xr_math.h`, `xr_camera.*`, `xr_cull.*` and `xr_settings.*` reach no
OpenXR header, so they compile in every configuration and can be reasoned about without a headset —
the coordinate conversion and the camera composition are the parts most likely to be subtly wrong,
and keeping them dependency-free is what makes them checkable. The session, swapchain, frame loop
and input go in `reblue_openxr_only` in `src/CMakeLists.txt` and build only under `REBLUE_OPENXR`.

**Keep it that way.** `xr_camera` and `xr_cull` take their settings pushed in (`CameraTuning`,
a margin argument) rather than reading `bd::xr::Settings`. That is not incidental — it is the only
reason `tools/xr_math_test` can exercise them, and it has already caught two real bugs. Do not
reintroduce a reach into the cvar singleton from either file.

**Handedness lives in one place.** OpenXR is right-handed with -Z forward; Blue Dragon is D3D9-era
left-handed with +Z forward, row-vector, row-major. They differ by a mirror on Z. Everything
crossing that boundary goes through `FromOpenXRPose` in `xr_math.h`. Do not convert at a call site.

## Android / Quest state

The Android target builds and runs on a Quest 2. `tools/build_apk.sh` packages it, game data comes
from `tools/extract_game_data.py`, and `args.txt` beside the game data appends cvars without
rebuilding the APK - use it, a 62 MB reinstall to change a log level is not a dev loop.

It runs: XEX loaded, 154 kernel imports patched, guest heap up, VFS mounting **1673 archives /
119346 record names** of real game data, Vulkan 1.1 on the Adreno 650, the guest rendering, Touch
controllers reaching the guest as a pad, and OpenXR compositing it at 30 fps - the game's native
rate. Those two VFS numbers are the quickest sanity check that the install is complete; see the
game data section above.

The one shortcut still in place is the sun occlusion descriptor set, which is **dropped** on Android
rather than fixed. Adreno exposes `maxBoundDescriptorSets = 4` where a desktop GPU reports 8 or 32,
and this renderer wants five. Dropping it costs four pipelines that still declare bindings there, so
they fail to create. The real fix is to collapse sets 0/1/2, which are one physical descriptor set
bound three times to satisfy three HLSL register spaces. See
`research/20260828_1720_quest-bindless-blocker.md` - and note that note's own correction: the
65536-entry bindless heap was a wrong first hypothesis, since the Adreno 650 does report
`descriptorIndexing`.

Diagnostics that exist now and should be used before guessing: the SDK logs to logcat on Android
(spdlog android_sink), `crash_handler.cpp` unwinds with `_Unwind_Backtrace` and reports the faulting
PC from the signal context, and the on-device log lives under the app's external files directory.

## Git Bash mangles device paths. This has cost hours, three times.

Under MSYS (the Bash tool here), **any argument that looks like a Unix path is rewritten to a
Windows one** before the program sees it. `/storage/emulated/0/...` becomes
`C:/Program Files/Git/storage/emulated/0/...`. `MSYS_NO_PATHCONV=1` disables that - and then
disables it for *every* argument in the command, including the local ones, which is the trap.

Three ways this has already gone wrong, all of which looked like something else:

| Symptom | Actually |
| --- | --- |
| `adb push` reports success, file never changes on device | Local path stayed `/c/Users/...`, adb could not stat it. The `&&` did fire, because the *echo* was unconditional. |
| Three consecutive perf experiments all measure "no change" | Every `args.txt` push had silently failed; the app was reading a stale file |
| `extract_game_data.py --adb-serial` dies with `short read: wanted 2048, got 0` | The device-side ISO path was rewritten to `C:/Program Files/Git/storage/...` |

**The rule: `MSYS_NO_PATHCONV=1` for the command, Windows-style paths for anything local.**

```sh
MSYS_NO_PATHCONV=1 adb push "C:/Users/.../args.txt" /storage/emulated/0/Android/data/com.reblue/files/args.txt
```

And verify, every time, because all three failures above reported success:

- Print the `adb push` result instead of discarding it. It says `1 file pushed` or it says `cannot stat`.
- After a launch, check `adb logcat -s reblue | grep 'args.txt added'` - the count must match the
  file. A stale `args.txt` is indistinguishable from a setting that does nothing.
- `adb shell cat` the file back when it matters.

Two more silent no-ops in the same family, worth knowing before trusting a measurement:

- **`bd_msaa` only accepts 0/2/4/8.** `--bd_msaa 1` is rejected by the validator and leaves it at 4.
  A run that "proves MSAA does not matter" may be a run with MSAA still on.
- **Cvars with `kRequiresRestart` need a `force-stop`**, not just a relaunch of the activity.
- **An APK missing `libopenxr_loader.so` installs cleanly and then cannot start.** It dies in
  `dlopen` before `main()`, so **no reblue log file is written at all** and the newest log on device
  is the *previous* run's - which then reports the old build's numbers under the new build's name.
  The only evidence is one logcat line. `tools/build_apk.sh` now finds the loader itself and refuses
  to package without it, but if a launch ever produces no new log, check logcat for `dlopen` before
  anything else.

## Game data: use `--all`, not the manifest

`tools/extract_game_data.py` was driven by `res/embed/installer/manifest.txt`, the desktop
installer's copy list. **That list does not name a single locale-specific file** - grep it for `_us`
and you get nothing - so a manifest-only extraction produces a build that boots, shows its title
screen, accepts input, and then dies the moment a new game starts:

```
[disc] file-load fatal, failed file: 'D:\database\camp\ene_dic_us.u16'
Fatal: File Load Error - Failed to load a required game file
```

All four of those records live in `pack/packmem_us.ipk`, a 2 MB archive **on disc 1** that the
manifest never mentions - and 1107 files on disc 1 alone are in the same position, mostly locale
packs and locale sound banks. So this is not a "you only extracted one disc" problem, which is what
it looks like at first; extracting all three discs through the manifest still misses it.

**Use `--all`.** It ignores the manifest and takes every file on the disc, still honouring
`--skip-media`. The manifest path only remains for reproducing what the installer does.

Nothing before "new game" touches any of those records, so the install looks completely healthy
until it isn't - which is why this got chased through the renderer first. If a fatal file load
appears, check the VFS counts in the log before suspecting code: a full three-disc `--all` install
mounts **1673 archives / 119346 record names**, where a manifest-only one mounts 1274 / 70008.

`--skip-media` is fine for iterating (it drops `movie/` and `snd_stream*`, most of the bytes) but it
does mean no intro movie.

The discs never have to be copied to the PC. The reader walks XDVDFS and pulls only the sectors it
needs, straight off an adb-connected device:

```sh
MSYS_NO_PATHCONV=1 python tools/extract_game_data.py   "/path/on/device/Blue Dragon ... (Disc 2).iso"   "/path/on/device/Blue Dragon ... (Disc 3).iso"   --adb-serial <serial> -o out/game --all --skip-media
```

**Push directories, never files in a loop.** One `adb push` per file, with a `mkdir -p` round trip
each, managed 3 files in 90 seconds. Pushing the changed subtrees instead moved 5 GB in under a
minute, at 120 MB/s - four orders of magnitude apart, and the only difference is where the loop
lives.

## Forked dependencies

Both patches outgrew `patches/` and now live as real history in the owner's forks. **Push changes
to these, do not add new patch files.**

**Work on `main` in every fork.** No feature branches, matching how this repo itself is worked.

| Submodule | Fork | Branch | Carries |
| --- | --- | --- | --- |
| `thirdparty/plume` | `noeldvictor/plume` | **`main`** | `VulkanInterfaceOptions` so an OpenXR runtime can name its extensions and physical device, plus Vulkan multiview: `viewMask` on the pipeline and framebuffer descs, and the `VkPhysicalDeviceMultiviewFeatures` enable without which a view mask is silently ignored |
| `thirdparty/XenosRecomp` | `noeldvictor/XenosRecomp` | `reblue` (see below) | `SV_ViewID` on every vertex shader and the per-eye off-axis skew that makes multiview stereo work, plus `vs_6_1` because `SV_ViewID` is a shader model 6.1 semantic |
| ReXGlue SDK (not a submodule) | `noeldvictor/rexglue-sdk` | **`main`** | The Android ARM64 port |

**XenosRecomp is the exception, and it needs a decision.** Its fork carries three branches:
`main` is the *original upstream* (hedge-dev), `zolaware-main` is the full history the reblue fork
was taken from, and `reblue` is the working branch our commits sit on. `main` and `reblue` have
**unrelated histories**, so the work cannot simply be merged onto `main` - moving it there means
force-pushing `main` to the reblue lineage, which discards the upstream history that branch
currently holds. `zolaware-main` preserves what matters, so this is safe to do, but it is
destructive and has not been done.

Until then `reblue` is the branch, and it is where the multiview work is pushed.

The ReXGlue SDK is **not** a submodule - it is cloned to `out/rexglue-src` and pointed at with
`REXSDK_DIR`, because no `android-arm64` release slice exists. Its Android port is
`noeldvictor/rexglue-sdk` branch **`android-arm64`**: fibers via vendored libucontext (bionic
aarch64 has no `getcontext` family at all), `ASharedMemory_create` for the guest address space,
an activity harness, an Android filesystem path, and logcat logging.

Two things to know before touching the SDK tree:

- **Never commit `thirdparty/libmspack`.** Its symlinks check out as plain text on Windows, so the
  submodule permanently reads as modified. Committing that corrupts the pointer.
- This repo vendors **XenosRecomp**, the Xenos *shader* recompiler. **XenonRecomp** is a different
  project - the PowerPC *CPU* recompiler - and is also forked, but nothing here uses it. Check which
  one a change actually needs.

## Dev loop

**Invoke the `devloop` skill** (`.claude/skills/devloop/`) before building, running, deploying, or
diagnosing a slow build. The short version:

- **`tools/xrsim/` runs the whole VR path on the desktop with no headset** - our own headless
  OpenXR runtime. It is the only way to exercise the camera modes and the character anchor, because
  without a runtime `ViewOverrideActive()` is false and they never compose. **Invoke the `vrsim`
  skill.**
- **`bash tools/verify_quest.sh` is the whole device measurement in one command.** It picks the
  headset (not the other Android device that is often attached), installs the staged APK, runs
  autoplay into a field scene, and pulls back the frame breakdown, the per-frame CSV, the sampling
  profile, a composited capture and the per-thread CPU split. It encodes the traps that have each
  cost hours here: `MSYS_NO_PATHCONV=1` on every adb call, every push result printed rather than
  assumed, the proximity broadcast re-sent throughout rather than once, a force-stop rather than a
  relaunch, and a check for the `dlopen` failure that writes no log at all and leaves the *previous*
  run's numbers on disk under the new build's name.
- **`tools/stereo_check.py --raw <capture>` runs on a capture already on disk**, so the stereo
  regression test works in the desktop loop with no headset and no device. Verified after the
  2026-08-30 optimisation pass: `far +4, near -2, near - far = -6px`, crossed and correctly signed.
- **RenderDoc captures a frame from inside the app, headlessly.** `bd_renderdoc` loads it before
  the `VkInstance` (it hooks at load time) and `bd_renderdoc_after_s` triggers a capture once
  autoplay is in a field scene - `renderdoccmd capture` waits on a keypress, which an unattended run
  cannot supply. Then `renderdoccmd convert -f cap.rdc -c zip.xml -o frame.zip.xml` and
  `python tools/rdc_outline.py frame.zip.xml` prints one line per render pass: size, **view mask**,
  pipelines, viewports, draws. Khronos publishes **no Windows validation binaries**, so on the
  desktop this is the instrument that exists - and it named a multiview bug in one line after ten
  causes had been eliminated by inference without finding the eleventh.
  **Read the view mask, not the layer count**: `VkFramebufferCreateInfo::layers` must be 1 under
  multiview, so "0 framebuffers are layered" looks damning and means nothing.
- **`tools/stereo_check.py` answers "does stereo have depth" in one command.** `--stacked` does the
  same for a `bd_mv_capture_array` grab, whose two layers are stacked vertically. It captures a frame
  with `bd_capture_after_s`, matches the two eyes band by band and prints a verdict: **FLAT** (the
  eye offset is proportional to `clip.z` and divides out to a constant slide), **INVERTED** (crossed
  the wrong way, so the world renders pseudoscopic), or **OK**. It is checked against the three real
  captures that produced those states, so it is a regression test and not just a report. Run it
  after anything touching the camera, the projection or the shader recompiler.
- **Sweep with `tools/bench_quest.py`, never adb by hand.** Every performance knob reaches the game
  through `args.txt`, so a whole matrix costs no build and no reinstall.
  `python tools/bench_quest.py levers` runs render scale, shadows and reflections one variable at a
  time and prints a comparison table; `fill` runs the scissor sweep; `--config "k=v,k=v"` runs
  anything else and `--targets` adds the per-surface draw census. It encodes the traps that have
  each cost hours here: every push result is checked rather than assumed, the proximity-sensor
  broadcast is sent twice, and it prints the reminder that a difference under ~30% is
  cross-restart noise rather than a result.
- **Know the four loop lengths, and pick the shortest one that answers the question.** Measured
  here: a cvar experiment via `args.txt` is **~30s and builds nothing**; a `src/` change is
  **~10s to compile**, ~15s to package, ~5s to install, ~30s to the title screen; a codegen flag
  change is **~8s for codegen plus ~77s** because all 54 recompiled TUs rebuild; and a full
  disassembly measurement out of `libreblue.so` needs no device at all. Most questions asked in this
  port were answerable at one of the shorter lengths than the one reached for.
- **The device loop is about 60 seconds, and that turned out to be fine.** Measured, on this
  machine: incremental `src/` build **~10s**, `tools/build_apk.sh` **~15s**, `adb install -r`
  **~5s**, launch to title screen **~30s**. The whole VR port was built this way. The advice below
  used to say "do not go to the device, use Meta XR Simulator" - that is still the better loop *if*
  a desktop Vulkan toolchain exists, but this machine has no vcpkg so the desktop targets do not
  link, and the simulator was never needed. Do not treat going to the device as defeat.
- **Change a cvar without building anything.** `args.txt` beside the game data is read at launch,
  one argument per line. A settings experiment is a push and a relaunch, about 30 seconds, with no
  compile and no 62 MB reinstall. Check the `args.txt added N argument(s)` line in logcat, because
  a failed push looks exactly like a setting that does nothing.
- **Push directories, never files in a loop.** One `adb push` per file with a `mkdir -p` round trip
  each managed **3 files in 90 seconds**. Pushing the changed subtrees instead moved **5 GB in under
  a minute**, at 120 MB/s.
- **A XenosRecomp change needs two manual steps or it never reaches the device.** `REBLUE_XENOSRECOMP`
  points at `out/host-xenosrecomp/`, a separate host build tree that the Android build never
  rebuilds - and the shader cache does not depend on that binary either, so `ninja` reports "no work
  to do" and keeps the old `generated/shader_cache.cpp`. A shader-recompiler change therefore
  compiles, commits, deploys, and changes nothing at all. This is how the per-eye stereo skew sat in
  0 of 55 shaders for a day while the C++ around it looked correct.

  ```sh
  cmake --build out/host-xenosrecomp --target XenosRecomp
  rm -f out/build/android-arm64-release/generated/shader_cache.cpp
  cmake --build --preset android-arm64-release --target reblue
  ```

  **Verify in the emitted HLSL, not the source.** `--target reblue_shader_hlsl_dump` then grep
  `out/build/<preset>/hlsl_dump/` for whatever was added. Nothing else can tell the difference.
- **Do not rebuild the guest.** `reblue_recomp` and `reblue_generated` are separate OBJECT libraries
  so a change in `src/` never touches the 54 recompiled TUs or the multi-megabyte shader cache. If
  they rebuild, a codegen input changed — find out which.
- **Never wipe the build directory.** Reconfigure in place.
- **Build one target**, e.g. `--target reblue_vk`, not the default that links two executables.
- **PCH or compiler cache, never both.** They fight — no compiler cache caches a PCH compilation,
  which is why CI sets `REBLUE_PCH=OFF`. Locally: PCH on while editing, cache on while
  reconfiguring.
- **Bootstrap first, then most of this works.** The SDK is a public release and `default.xex` can
  be extracted from a disc image in under a second with `tools/extract_xex.py` — codegen then runs
  in ~7 seconds and is deterministic. See the `devloop` skill. A *full* Windows link additionally
  needs vcpkg. **It is installed** - `C:/vcpkg`, with `vcpkg_installed/` already populated, and
  LLVM lives at `C:/Program Files/LLVM/bin`. The desktop build **was configured, built and run on
  2026-08-29** and it works. This file claimed for weeks that it did not, while also stating further
  down that vcpkg was present - both sentences were in the file at once, and the whole VR port was
  built on-device with printf because nobody tried the route the file said was closed. Never report
  a build success that did not happen, and never leave a "this does not work" claim in place without
  a date and a reason.
- **But the maths is testable.** `tools/xr_math_test/` compiles `xr_math.h` standalone against a
  stub `rex/types.h` and runs assertions on it. It caught the off-centre projection sign on its
  first run. Keep new maths in the dependency-free files so it stays reachable from here, and extend
  the test when it lands.

## Reading the guest: the recompilation already did it

**Do not install a decompiler.** `generated/` holds the entire XEX translated to C++ - 223 files,
about 110 MB, **18,777 functions** - with the original PowerPC interleaved as comments and every
address from `config/functions.toml` applied as a real name. It is exact rather than reconstructed,
it is named, and it answers the question a decompiler is usually opened for: struct offsets.

```sh
grep -rln "DEFINE_REX_FUNC(bdPlayerFieldMovementUpdate)" generated/
```

```c
	// mr r31,r3
	r31.u64 = ctx.r3.u64;                     // r3 is the first argument
	// lwz r11,7224(r31)
	r11.u64 = REX_LOAD_U32(r31.u32 + 7224);   // and 7224 is a field in it
```

`ctx.r3` is the first argument, `r4` the second - the same register names `config/hooks/*.toml`
lists, so a hook body and the generated source read against each other directly. **Invoke the
`guest-source` skill** before going looking for anything in the original binary.

It is a build product: never edit it, never commit it, and if it rebuilds unexpectedly a codegen
input changed.

## Rewriting the recompilation is in scope

The owner has explicitly put the generated guest code on the table for performance work, and that
matters, because **the in-game bottleneck is not the GPU**. A field scene costs ~180ms of CPU
against ~1ms on the GPU fence, and that CPU is the recompiled PowerPC. Shaders, foveated rendering
and texture formats cannot touch it - they are all GPU-side, and the GPU is asleep.

So the expensive options are allowed: changing what codegen emits, hand-writing hot functions as
host `REX_FUNC` implementations (the mechanism exists and `config/functions.toml` already names 1608
candidates), or cutting work a VR port does not need the guest to do at all.

**Measure before rewriting anything.** Two tools exist and both work:

- **`bd_perf_csv`** - a per-frame CSV, no rebuild, set it in `args.txt`. This is the cheapest real
  measurement in the project and went unused for a long time. A field scene reads:
  `dt_ms 182.6 | fence_ms 110.0 | other_ms 72.2 | draws 2957 | pso_switches 1121 | fb_binds 23`.
  **2,957 draws per frame** is the number that reframes everything below.
- **`tools/profile_quest.py`** - simpleperf, no instrumentation, names all 27,080 recompiled
  functions directly.

`gpu_total_ms` was stale until 2026-08-30 and is now correct. `FrameEnd` wrote its closing
timestamp to a reserved high index, leaving unwritten queries in the middle of the pool, and
`vkGetQueryPoolResults` returns `VK_NOT_READY` for a range containing any - so plume bailed and the
previous frame's numbers were read as this frame's, 6913 times a run. Fixed on both sides; the
figure went from a stale ~2.0ms to a real 5.83ms.

**The split is now worth reading, and it names an X360 pattern**: desktop field frames give
`gpu_draw 4.54ms (78%)`, **`gpu_resolve 1.12ms (19%)`**, `gpu_inter 0.16ms`. A fifth of GPU time is
the EDRAM resolve - copying render targets into textures because a Xenon had to.

### What reading the code already found

`research/20260828_2100_guest-cpu-cost.md` has the detail. Three things worth knowing before
touching guest performance:

- **`non_argument_as_local` miscompiles the guest. Do not enable it.** It cuts `ctx.` accesses
  across `generated/` from 2,049,797 to 1,306,101 - a 36% reduction - and builds cleanly, which is
  exactly why it looked like a free win and was briefly reported as one. On device the game dies
  0.2s after the VFS mounts with `[disc] file-load fatal, failed file: '<unknown>'` - no preceding
  read failure, so the guest's own IO path is being miscompiled. Bisected against identical game
  data. **The static metric was completely disconnected from correctness**, which is the lesson: the
  flag is a promise about register liveness, and this codegen has no liveness pass to verify it
  with. `skip_msr` is the remaining unset flag and is left alone for the same reason.
- **Every guest memory access is `volatile`**, which costs real instructions, but unevenly. Built
  both ways and disassembled: `bdBuildViewMatrix` goes 1000 -> 818 instructions and loses **42% of
  its loads**, while `bdCameraRender` moves 1.5% and `bdPlayerFieldMovementUpdate` 0.8%. The win is
  in maths-heavy code with redundant loads to remove, not everywhere. `REBLUE_RELAXED_GUEST_MEMORY`
  turns it off and is **OFF by default**: `volatile` is presumably what stops a guest spin-loop's
  load being hoisted, and nothing has established Blue Dragon has no such loop.
- **Two things that look expensive and are not**, recorded so they are not investigated twice:
  FPSCR flush-mode switching is guarded by a cached compare and only writes FPCR on an actual
  change, and indirect dispatch is a range check plus a table lookup. Neither is a bottleneck.

### The vector unit is the big ARM64 opportunity

`research/20260828_2200_arm64-neon-vector-path.md`. **NEON is mandatory on ARMv8-A**, so the Quest 2
has it and the port already uses it: Xenon's VMX is translated to SSE intrinsics through SIMDe,
which maps to NEON. The problem is not instruction selection, it is that **the vector register file
lives in memory**. Counted out of the shipped `libreblue.so`:

| | instructions |
| --- | --- |
| Vector register memory traffic (q-reg `ldr`/`str`/`stur`/`stp`) | ~129,800 |
| Endian swizzling (`tbl`, `rev32/64/16`, `ext`) | ~18,600 |
| Actual float SIMD arithmetic (`fmul`/`fadd`/`fsub`/`fmla`/`fdiv`) | ~8,600 |

**Roughly fifteen memory operations per useful arithmetic one**, and byte-swizzling costs more than
double the maths. Every VMX op loads from `ctx.vN` and stores straight back, so a value written by
one instruction and read by the next round-trips through memory instead of staying in a register.

The GPRs do not have this problem **because there is a flag for them and it is on**. The SDK's eight
codegen flags cover LR, CTR, XER, CR, reserved, non-argument and non-volatile GPRs, and MSR - **not
one touches VMX**. The same optimisation that removed 743,696 context accesses for integer registers
has never been applied to the vector unit, in a game whose maths is mostly vector.

Three changes, in order, all in the forked SDK's codegen rather than `src/`:

1. **Vector registers as locals** - emit `simde__m128 v12;` and let the compiler allocate it,
   spilling to `ctx.vN` only at call boundaries. The large one.
2. **Hoist the endian conversion** so a register staying in host byte order across a function does
   not get swapped in and out for nothing.
3. **`fmla`** - only 248 fused multiply-adds against 719 `vmaddfp` in the source. SIMDe's
   `add(mul(a,b),c)` is not being contracted; `-ffp-contract=fast` is worth testing, since guest
   results are already not bit-exact with a Xenon.

Ruled out, so nobody re-runs it: **LSE atomics** (only ~11 LSE and ~24 exclusive-loop instructions
in 5.8 million - atomics are not hot, despite the build targeting baseline `armv8-a` with no
`-mcpu`), **fp16 and dotprod** (zero uses, and recompiled PowerPC would not produce them).

**These are static instruction counts, not a profile.** They say where the instructions are, not
where the time goes.

## Start here if you are picking this up

**`research/20260830_0500_what-is-true-now.md`** is the consolidated state as of 2026-08-30. It
lists what this repo believed that turned out to be false (the frame is not fill-bound,
`gpu_total_ms` was stale by 3x, `bdSceneNodeDrawSingle` is ~5% not 23x, encounters do not end
autoplay runs), what was measured and holds, the tools that now exist, and what to do next in order.
Read it before any older note it disagrees with.

## Research notes

Findings from web research go in `research/` as `YYYYMMDD_HHMM_<slug>.md`, dated at the time of
writing, with the sources linked at the end. They are a log, not a wiki: write a new file rather
than editing an old one, so what was believed when a decision was made stays legible. When research
changes the plan, say so in the note *and* update `docs/VR_PORT_PLAN.md`.

## How the guest is patched

Two mechanisms, and it matters which one a change belongs in:

1. **Function replacement.** `config/functions.toml` names guest addresses; the SDK emits
   declarations, and host code defines `REX_FUNC(name)` to take the call over entirely.
   `src/core/hooks.h` adds `BD_NOOP` / `BD_NOOP_RETURN` for calls the host deliberately ignores.
2. **Midasm hooks.** `config/hooks/*.toml` places a callback at a specific instruction address, with
   named registers passed through, optionally redirecting control flow via `jump_address_on_true`.
   Bodies live alongside the feature in `src/`.

Hook TOMLs carry the guest addresses and a comment explaining what the surrounding guest code does.
Read the TOML before touching a hook body — the address comments are the only map of the original
binary you have. `REX_HOOK*` symbols live in `OBJECT` libraries and never `STATIC` archives, because
the linker drops the registration symbols from an archive.

## Render backends

`plume` (`thirdparty/plume`) abstracts D3D12 and Vulkan. Which one is compiled in is a build-time
choice, not runtime:

- Windows builds both `reblue.exe` (D3D12) and `reblue_vk.exe` (Vulkan) from one configure.
- Everything else is Vulkan only. macOS goes through MoltenVK (`REBLUE_MVK`); Linux builds its
  surface through the SDL3 window plume already owns.

`src/CMakeLists.txt` splits sources three ways: `reblue_backend_only` (compiles once per exe),
`reblue_d3d12_only`, and everything else (compiled once into the `reblue_common` object library).
Backend-conditional headers `#error` if included from a common TU — respect that split when adding
files.

**Shaders.** Guest Xenos shaders are translated to HLSL by `XenosRecomp` at build time, then
compiled by DXC to DXIL (D3D12) or SPIR-V (Vulkan, compressed with smol-v). The Vulkan path uses
real spec constants and needs **no runtime shader compiler**. Only D3D12 links shaders at runtime
via `src/gpu/shaders/dxc_link.cpp`, and `reblue_prelink` pre-bakes those variants anyway. This is
load-bearing for the port: a Vulkan-only Android build needs DXC on the *host* toolchain only.

## Conventions

There is no `.clang-format`. Match the surrounding file.

- Headers open with a Doxygen block: `@file` (path-relative), `@brief`, `@copyright`, `@license`.
  Keep the existing copyright lines on files you edit.
- `PascalCase` for functions and types, `snake_case` for locals, trailing underscore for private
  members (`install_root_`), `k`-prefixed constants (`kProgramFiles`).
- Namespaces close with a trailing comment, e.g. `} // namespace bd::gpu`.
- Comments explain *why*, not what — the CMake files are the house style for this, and it is worth
  imitating. A comment that restates the code is noise; a comment that records the guest-side reason
  a workaround exists is the whole value.
- C++23, no compiler extensions. `u32`/`u64` style integer aliases come from `rex/types.h`.
- Byte-swapping guest data uses `__builtin_bswap*` (`_byteswap_ulong` appears once, under MSVC).
  Guest memory is big-endian; host is not. Every read of a guest structure swaps.

## Porting notes: ARM64 / Android / Quest

Research current as of 2026-08. Correct this section when reality disagrees with it.

### Where the port actually is

Blue Dragon runs on a Quest 2 and renders in VR. Most of the section below this one was written
while that was still a plan, and is kept because the reasoning is still worth having — but read this
table first, because it is the part that gets stale.

| Piece | State |
| --- | --- |
| SDK cross-built for android-arm64, APK, install, launch | Works |
| XEX load, kernel imports, guest heap, VFS over real game data | Works |
| Vulkan on Adreno 650, swapchain, pipelines | Works |
| OpenXR session, quad layer, world-locked and correctly placed | Works, seen |
| Touch controllers as a guest gamepad | Works, drives the game |
| Head pose driving the guest's own view matrix | Works, seen moving |
| Full three-disc game data, a new game starts | Works |
| Title screen at 30 fps | Works |
| **In-game frame rate** | **4-8 fps, and ~180ms of it is CPU** |
| Mono projection layer - the "in the world" mode | Works, captured and looked at |
| Character-anchored camera modes | Anchored off the follow camera; unit-tested, untuned |
| Stereo, per-eye render | **Works on device.** far +3 / near -33px crossed, 28-30 fps |
| Cel shading, tourist mode | Not started |
| Sun occlusion descriptor set on Adreno | Dropped, not fixed |

Three of those rows need saying plainly rather than being read past:

- **The VR scale was wrong by 100x, and that is what made the anchored modes look broken.**
  `bd_vr_units_per_metre` defaulted to **1.0** while a Blue Dragon unit is a **centimetre** - the
  field camera sits at y ~ 150 with the ground at 0, which is a 1.5 m eye height. So the head's
  1.6 m became 1.6 cm of game movement, and `bd_vr_third_offset` of `(0, 1.5, -3.0)` put the
  third-person camera **3 cm** behind the character. A capture showed exactly that: the inside of
  the character's head. The offsets' ranges were +/-50 too, half a metre, so it could not even be
  tuned around from a config file. Now 100 units/metre, offsets in game units, ranges +/-2000.
- **`bd_vr_third_offset_y` is 0, not eye height.** The head pose already carries the player's own
  1.6 m above the anchor, so putting eye height here as well stacks them and the camera ends up
  3.7 m up looking down - measured. The anchor belongs on the ground.
- **Battles switch to the diorama camera** (`bd_vr_battle_diorama`, on by default). A battle is a
  stationary set-piece: the ranks do not move, so a follow camera has nothing to follow and spends
  the fight fighting the game's own battle camera. `bd::engine::BattleActive()` reads the same task
  liveness the battle manager does, and deliberately only that half - waiting for the manager to be
  captured would leave the camera in follow mode for the first frames of every battle, which is when
  the transition is most visible.
- **The anchored camera modes now have an anchor**, derived from the game's own follow camera
  (`CharacterFromFollowCamera` in `xr_camera.cpp`, fed from `ComposeView`). Blue Dragon's field
  camera sits behind the party leader and looks at them, so the leader is on the camera's forward
  axis at the follow distance and the camera's yaw is their facing - approximate, but built on data
  that provably updates every frame. Tuned by `bd_vr_anchor_distance` (0 restores the old fall-back)
  and `bd_vr_anchor_eye_height`; **the defaults are estimates and want one pass of tuning against a
  capture.** The direct route still does not work - `xr_player_anchor.cpp` hooks
  `bdPlayerFieldMovementUpdate` and that guest function never fires even with the character walking;
  see `research/20260829_1420_autoplay-walks-and-the-anchor-hook-is-dead.md` for what is already
  ruled out.
- **The projection layer produces an image**, and there is now a capture of it to point at. This
  entry said the opposite for weeks on the strength of one black grab taken before a NaN fix.
- **30 fps is the title screen.** Gameplay is 4-8 fps.

What the player sees is still a flat image on a world-locked screen: the game renders once, from one
eye. The camera modes in `src/xr/xr_camera.cpp` are composed and delivered every frame, so the
plumbing for 6DOF is live — but genuine stereo needs the guest's scene rendered twice per frame, or
per-view matrices reaching shaders whose constants XenosRecomp already baked. That is a renderer
change, not an XR one, and it is the next real piece of work.

**Keeping the headset awake unworn.** The proximity sensor suspends the app the moment nobody is
wearing it - `onResume` then straight to `onPause`/`onStop`, no log file, which looks exactly like a
startup hang. `adb shell am broadcast -a com.oculus.vrpowermanager.prox_close` makes it behave as if
worn, which is what makes unattended measurement possible at all.

**Regenerating `reblue_pch.h` does not invalidate the precompiled header.** Any codegen flag change
then fails with "file has been modified since the precompiled header was built" until the
`cmake_pch.hxx.pch` files are deleted.

**Verify the pixels, not a proxy.** Got wrong repeatedly here, so it is a rule now. "13 input
actions attached" is not "controllers work". "Swapchain format 37" is not "the colour is right". A
clean build is not a working feature. Each of those was reported as success on the strength of a log
line, and each was wrong or unverified. A VR claim is verified when a screenshot has been looked at
or a number read off a run - and `bd_xr_autoplay` plus `bd_capture_after_s` mean that needs nobody
in the headset. **`bd_capture_after_s` writes the composited frame to `logs/capture/`** as raw RGBA
with a one-line header; pull it and convert with PIL. Seconds rather than a bool because `args.txt`
is read once at launch, so a bool only ever catches the title screen - 143 lands in a field scene,
after the field comes up at ~130s and before autoplay starts walking at 150s. It cost one plume fix
(`copyTextureRegion` had no texture-to-buffer case and crashed on a null `dstTexture`) and it has
already paid for itself twice: it confirmed the projection layer, and it showed stereo has no depth.
Note the Quest system-screenshot intents complete and write nothing here, and `adb screencap` does
not see compositor layers, so this is the only route. Say
"built, unverified" otherwise. That is not a weaker claim, it is an accurate one.

Three lessons from getting this far, all of which cost hours and all of which recur:

- **Make it visible before debugging it.** Two multi-hour hunts were output Android discards; both
  fixes were one line once readable. Recorded in `.claude/skills/devloop`.
- **A symmetric test case cannot see a sign error.** Both the projection matrix and the quad's
  facing yaw shipped wrong signs that are zero in the obvious test pose. Test off-axis.
- **An involution that round-trips looks like a no-op and is not.** The OpenXR/game-space mirror is
  its own inverse, so mixing the two conventions in one expression compiles cleanly and is wrong.

See `research/20260828_1900_vr-camera-and-input.md` for the detail, including the hook seams
(`bdBuildViewMatrix` at 0x82286C40, `bdBuildProjectionMatrix` at 0x82168E18) and why Quest
controllers are invisible to SDL.

### Every measurement before 2026-08-29 was taken standing still

`bd_xr_autoplay` pressed START and then A, for ever, and **never touched a stick**. So the fill
sweeps, the culling sweeps and the 28.9 fps result were all measured with the character stationary -
not a representative frame, and the reason every hook on the player object silently never fired.

It now walks a slow circle. Two things follow, and both bite:

- **`kWalkStart` is 150s and has to be late.** At 26s the file menu is still up and a stick
  deflection moves the selection; the run then sits in a menu for ever, measuring 18 draws/frame
  with the camera pinned at (0, 1000, 500), which reads exactly like a hang. Launch to a field
  scene is ~130s on a Quest 2.
- **Walking triggers random encounters**, so a run leaves the field within about a minute and a
  longer settle makes it *more* likely to have wandered into a battle or a menu. Check the draw
  count before believing any number: a field scene is ~500-600 draws, a menu is 18. This is the
  first concrete reason to want tourist mode's encounter suppression as a *measurement tool*.

### The `bdPlayerField*` family never runs. Do not hook it.

Two features have now stalled on this. `bdPlayerFieldMovementUpdate` never fires with the character
walking (the VR anchor), and `bdPlayerFieldCheckEncounter` never fires either - measured with an
unconditional entry counter over a 200s run covering the whole autoplay walking phase, **zero
entries**. The callers chain up to `bdPlayerFieldUpdateMain`, which is itself named, so this is not
an unnamed indirect-dispatch edge the callgraph cannot see; that subsystem simply is not the one
driving. See `research/20260830_0300_the-player-field-family-is-dead.md`.

It blocks encounter suppression - wanted for VR sightseeing *and* as a measurement tool, since
autoplay walks, walking rolls encounters, and 26% of frames after the walk begins came in under 100
draws against a field scene's 500-600 - and it is why the character anchor derives position from the
follow camera instead.

**Find the seam that runs before writing the hook**, and search by *address range*, not by name: a
profiler ranks by cost and a per-frame player update for one character is cheap, so grepping a
profile for "player" proves nothing. What it does show is that **no function in `0x8220xxxx` is
sampled at all** during the walking phase while `0x8221xxxx` is busy - sixteen distinct functions,
of which `sub_82215050` has no direct caller and is therefore reached through a vtable or jump
table, the shape of a task entry point. Start there. Confirm liveness with an unconditional entry
counter before building on it. A hook on a function nobody has watched fire is a guess, and the cost
of checking is one counter and one run.

### Look at the threads before theorising about the GPU

`adb shell top -H -b -n 1 -p $(pidof com.reblue)` costs nothing, needs no build, and had never been
run before 2026-08-29. It immediately showed the guest main thread saturated and the renderer nearly
idle, which is the opposite of what three sessions of GPU work had assumed. Read
`/proc/<pid>/task/<tid>/status` alongside it for `Cpus_allowed_list`: the runtime pins our render
thread to cores 4-6 (the big cluster) while unpinned guest threads roam all eight and compete for
exactly those three.

`bd_thread_policy` (on by default, Android only) acts on what that shows: it walks
`/proc/self/task` every ~120 frames and puts the guest main thread on the performance cores and the
guest workers on the efficiency cores, leaving the render thread where the runtime pinned it. The
clusters are **derived from `cpuinfo_max_freq`, never hardcoded** - a Quest 2 is 4+3+1 (masks
`0x0f`/`0xf0`) and an AYN Thor is 3+4+1 (`0x07`/`0xf8`), so a fixed mask is wrong on one of them.
`taskset` from adb cannot do this: the shell may not repin another app's threads, but a process may
always repin its own.

**The first profile, desktop, field scene, 17,332 samples, 99.4% resolved.** Nothing dominates -
which is the headline, because the plan assumed one function did:

```
__imp__bdSceneNodeDrawSingle    5.3%     rex::ppc::CRRegister::compare  4.6%
__imp__sub_82287788             4.6%     std::_Atomic_integral::fetch_  3.9%
CopyByteSwap32Impl              3.2%     simde_mm_shuffle_epi8          3.0%
InsetQuadUVs                    2.2%     __imp__sub_82281D28            2.2%
__imp__sub_8272BE80             2.1%     __imp__bdSceneNodeCullTraverse 2.0%
```

`bdSceneNodeDrawSingle` is **5.3%, not the 23x-everything-else the call census implied** - a census
counts calls, and these are cheap calls made often. Expect a rewrite of it to buy single digits, not
a frame. `rex::ppc::CRRegister::compare` at 4.6% is recompiler *runtime*, not guest code, and
`cr_as_local` is already on, so that is the cost of PowerPC condition registers existing at all.
`sub_82287788` and `sub_82281D28` are unnamed and worth naming in `config/functions.toml`.

**The first thing the profiler found was ours, not the guest's.** `Sleep_hook` was 15.9% of all
samples - more than three times the hottest guest function - busy-waiting out a 1.5ms guard band for
precision nothing needs. Removing the spin took `other_ms` from **8.49ms to 7.79ms** at an identical
draw count and unchanged `logic_tps`. `bd_sleep_spin` restores the old behaviour.

**There is now an in-process sampling profiler, and it is the only one that works on a Quest.**
`bd_sample_profiler=true` (plus `bd_sample_hz`, default 1000) makes a timer thread send `SIGPROF`
to the guest threads and records the interrupted PC - a process may always signal its own threads,
which is the whole trick. Nothing is symbolised on device: PCs are stored as offsets into
`libreblue.so` and resolved on the host, so the on-device cost is a signal and a store.

It works on the **desktop** too, which is the fast loop: `SuspendThread`/`GetThreadContext` in
place of `SIGPROF`, and a run is 170s with no APK, no install and no headset. That is where the
profile below was taken, and the guest code being profiled is the same code.

```sh
# device
adb pull /sdcard/Android/data/com.reblue/files/logs/guest_profile.txt
python tools/symbolize_profile.py guest_profile.txt

# desktop - reads the PDB through llvm-symbolizer, since a PE keeps no symtab
python tools/symbolize_profile.py out/build/win-amd64-release/logs/guest_profile.txt        --so out/build/win-amd64-release/reblue_vk.exe
```

It reads the **unstripped** `out/build/android-arm64-release/libreblue.so` (50,651 text symbols,
17,054 of them recompiled guest functions) and prints demangled names ranked by share. A name it
prints is a real function in `generated/` - grep `DEFINE_REX_FUNC(<name>)` to read its body.

`simpleperf` would be better and does not work here: Horizon OS refuses shell perf on this device
regardless of `perf_event_paranoid`. The app manifest now declares `profileable android:shell`,
which was genuinely missing and is why `tools/profile_quest.py` had never produced a profile - but
it is not sufficient on a Quest 2.

### Vulkan validation layers run on device, and should be used before guessing

**`bash tools/validate_quest.sh` is that, in one command**: it packages the layer via `EXTRA_LIBS`,
enables the debug-layer settings for the app, runs autoplay, prints the VUIDs sorted by frequency,
and **turns the layers back off again** - left on they change what every later frame time means.
Khronos publishes Android binaries and no Windows ones, so the Quest is the only place this runs;
fetch the `.so` once into `out/vvl` (the script's header has the URL).

There are none in the NDK. Fetch the Khronos Android build from the Vulkan-ValidationLayers
releases and package it - Android only loads layers from the app's own lib directory, which is what
`EXTRA_LIBS` in `tools/build_apk.sh` is for (also the route for a replacement Turnip driver).

```sh
EXTRA_LIBS=/path/to/libVkLayer_khronos_validation.so bash tools/build_apk.sh
adb shell settings put global enable_gpu_debug_layers 1
adb shell settings put global gpu_debug_app com.reblue
adb shell settings put global gpu_debug_layers VK_LAYER_KHRONOS_validation
adb shell settings put global gpu_debug_layer_app com.reblue
```

**Turn it off again** (`enable_gpu_debug_layers 0`) - it is slow enough to change what a frame time
means. It settled the multiview question in one run after three sessions of inference, and it
immediately surfaced an unrelated live bug: `Int64` is declared by every shader (the constant path
uses `vk::RawBufferLoad` at a `uint64_t` address) while `shaderInt64` is not enabled, which makes
the renderer's hottest path formally undefined on this device.

### Measurement precision: +/-30% across restarts. Read this before trusting an A/B.

Within a single run the frame is stable to about 3% (200.5-207.1ms observed). **Across restarts it
is not**: two runs at essentially the same draw count (2848 and 2834) measured 159ms and 205ms.
`bd_xr_autoplay` presses buttons on a fixed schedule, but load times vary, so the character is not
in the same place at the same elapsed time and the scene genuinely differs.

So any effect smaller than ~30% cannot be resolved by comparing two runs, and several results in
this session sit inside that band and must be treated as inconclusive rather than negative. The
conclusions that survive are the ones with effects far larger than the noise: capping draws
(112.8ms -> 0.1ms), VR on versus off, resolution 720p versus 360p, and capping PSO switches
(90ms -> 235ms).

To measure something smaller, do it **within one run** - a cvar that can be toggled live, or two
code paths alternating per frame - not by restarting.

### The bottleneck: guest shader constants are global memory loads

**This is the answer to why the port is slow**, and it is a property of the shader translation, not
of Blue Dragon and not of VR. See `research/20260829_0030_shader-constants-are-global-loads.md`.

`XenosRecomp/shader_recompiler.cpp:1345` emits every guest constant register as

```cpp
#define cN vk::RawBufferLoad<float4>(g_PushConstants.VertexShaderConstants + off, 0x10)
```

`VertexShaderConstants` is a `uint64_t` device address in a push constant, so **every `c[n]` read is
a raw global memory load, per invocation**. The indexed form used by skinning (line 1340) adds a
compare and a `min` on top. On a Xenos these were constant registers - the fastest read a shader
had. On an Adreno 650 a UBO read goes through the constant path and is usually hoisted at wave
launch, while a buffer-device-address load is ordinary global memory, per vertex.

A skinned vertex shader reading four bone matrices plus a WVP is 20-40 global loads per vertex. At
~400,000 vertices a frame that is 8-16 million uncached loads, and it measures as **~225ns per
vertex** where single-digit nanoseconds would be normal.

Measured from the dumped HLSL (`--target reblue_shader_hlsl_dump`): **86-89 raw loads per vertex
shader**, and in `bd_toon_vs_env` **24 of them are `a0`-relative** - `exMatrix(0 + a0)` and friends,
which is bone-matrix lookup for skinning. Those are the expensive ones: the bounds check cannot
fold, and the address is *divergent across the wave*, so the driver cannot use scalar loads and must
gather per-lane from uncached global memory. On a Xenos these hit the constant file and were nearly
free.

**The fix** is to bind the constant blocks as a uniform buffer instead of pushing a device address:
`shader_recompiler.cpp` (lines ~1340-1395) and `shader_common.h` on the fork
`noeldvictor/XenosRecomp`, plus `src/gpu/constant_buffers.cpp` and `src/gpu/draw.cpp` to bind a UBO
descriptor rather than passing `gpuAddress` through `setGraphicsPushConstants`. The data, the upload
heap and the alignment all stay; only how the shader reaches it changes. Needs the shader cache
rebuilt.

**Scope it before starting.** The constants change every draw, which is exactly why a device address
in a push constant was chosen - it needs no descriptor update. A UBO needs dynamic offsets (plume
does not expose them yet) or a per-draw descriptor write, which would be worse than today. And there
is no spare set: Adreno allows 4 and sets 0-3 are taken, so the UBO has to become a *binding* on the
shared texture set, with DXC shift flags to dodge `t0` and the variable-count array kept last.

### The bottleneck is not VR

**Measured, VR switched off entirely**: the same field scene costs 2925 draws, 108.9ms on the GPU
fence, 183ms a frame - identical within noise to VR on. **Blue Dragon runs at 5.5 fps on a Quest 2
with the flat renderer.** The session, the layer, the camera composition and the pad cost
essentially nothing. The port is slow; VR was never the reason.

### The bottleneck is fragments — **corrected 2026-08-29, it is not**

**Read `research/20260829_2300_the-gpu-is-not-the-bottleneck.md` before acting on this section.**
Re-measured with actual GPU timers and the conclusion below does not hold: on a Quest 2 the GPU
executes the whole command buffer in **~2ms of a ~100ms frame**, and five experiments that would
each have shown a GPU cost all came back negative. The decisive one is `bd_cull_distance=1` -
almost the entire scene culled - which still costs **73ms against 12ms of our own work**. Drawing
nothing does not make the frame fast, so the frame is not bound by what we draw.

Also closed, so it is not tried again: **forcing every colour tile load and every depth store to
`DONT_CARE`** - the upper bound on every tile-traffic optimisation at once, rendering deliberately
incorrectly - changed nothing (102-117ms against 100-120ms). `XR_EXT_performance_settings`
SUSTAINED_HIGH and `XR_KHR_android_thread_settings` are both now requested and both accepted by the
runtime, and neither moved the frame either. They are kept because they are correct.

What the frame *is* bound by is the recompiled guest: `top -H` shows the guest main thread
(`SDLThread`) saturated and up to five guest worker threads at ~70% each, against a renderer
(`Draw Thread`) at ~10%. See `research/20260829_2200_where-the-cpu-actually-is.md`.

Two traps found while measuring: **`gpu_busy_percentage` reads 99% with the app force-stopped**, so
it is useless on this device; and the guest worker threads are **transient per scene**, so an A/B
across restarts can straddle two entirely different loads.

The original section follows, kept because the method is still the right method:

**Proven fill-bound.** `bd_debug_fill_scale` shrinks the *scissor* to N percent of the viewport
without touching the viewport, so vertex work, draw count, pipeline state and every upload stay
bit-identical and only the surviving fragment count moves. A field scene, MSAA off:

| `bd_debug_fill_scale` | fragments | fence | frame | draws |
| --- | --- | --- | --- | --- |
| 100 | 1.0x | **141ms** | 210ms | 2812 |
| 50 | 0.25x | **17ms** | 98ms | 2902 |
| 25 | 0.0625x | **0.1ms** | 71ms | 3095 |

**3095 draws and 450,000 vertices cost the GPU 0.1ms.** Draws and geometry are free; fragments are
the entire GPU cost. The fall is super-linear because a tiler skips tiles that contain nothing.

**Two earlier conclusions were wrong, and both failed the same way - one experiment, two variables.**

- *"Draw-bound"*, from `bd_debug_max_draws` taking the fence from 112.8ms to 0.1ms. Removing a draw
  removes its fragments too, so that test can never separate the two. The scissor test holds draws
  fixed and still empties the GPU. There is no 60µs-per-draw anomaly and no binning problem.
- *"Insensitive to resolution"*, from `bd_max_render_height` moving nothing. It was resizing the
  wrong surface. A per-target draw census (`NoteDrawTarget`, keyed on the surface, in the `[perf]`
  line) at `bd_max_render_height=360` shows `1280x720: 2434 draws` **unchanged** beside
  `688x360: 22 draws`. The scene renders into a surface pinned to the design canvas
  (`kDesignCanvasWidth/Height`), created by the guest through `D3DDevice_CreateSurface`; the cvar
  sizes the output fit. Foveated rendering was dismissed on this evidence and should be reassessed.

`bdOutputResViewScaleHook` only ever scaled *up*, so the canvas was a floor - but removing that
guard is **inert**, the scene surface does not come through that hook. The seam is
`D3DDevice_CreateSurface_hook` (`src/gpu/hooks/resource.cpp`), which is where `bd_render_scale`
acts.

**There is also a hard CPU floor of ~62ms.** At `fill_scale=25` the GPU is idle and the frame is
still 71ms, so the CPU alone caps the port near **14 fps**: ~46ms guest simulation plus ~14ms draw
recording. Fixing fill completely gets a field scene to roughly 10-14 fps, not to 72. Both halves
have to be solved, and only the GPU half currently has a proven lever.

**Do not trust a static count of the emitted HLSL.** A pixel shader body reads like 143-158 global
loads per fragment because the constants are `#define`s that re-expand at every use. Hoisting them
to entry-initialised statics was implemented and **changed the compiled SPIR-V by +0.2%** - DXC
already CSEs them. Measured by rebuilding the shader cache and comparing size, no device needed.
Reverted. The same idea was separately tried and reverted for vertex shaders.

### Measured on a Quest, 2026-08-29

`python tools/bench_quest.py all`, which is the first thing to run and builds nothing:

```
configuration                            frame    fence     else    fps   draws
baseline                               170.1ms  108.6ms   53.2ms    5.9    2834
render_scale=50, reflections=false     129.0ms   68.7ms   52.0ms    7.8    2761
  + shadows=false                      129.4ms   65.4ms   55.6ms    7.7    2777
  + stereo (sep 0.06, conv 0.03)       164.3ms   91.5ms   63.9ms    6.1    2766
```

Four things this settles:

- **The fill levers transfer.** 37% off the GPU fence and 5.9 -> 7.8 fps, outside the +/-30% band.
  The desktop verification loop was worth building.
- **`bd_shadows` is not a lever.** 65.4ms against 68.7ms is inside noise. It reached the device
  unverified because the census hooks the colour target and cannot see a depth-only pass, and it has
  now measured as approximately nothing.
- **The CPU floor is real computation, not back-pressure.** 43ms of GPU time was freed and
  `elsewhere` moved 53.2ms to 52.0ms. It caps the port near **19 fps** on its own, so the
  node-submission cost has to be attacked directly and culling is worth building.
- **Stereo runs on the headset and costs what it should**: a second view of a half-scale scene plus
  ~8ms of doubled draw recording. **Stereo at half scale is cheaper than mono at full scale** -
  91.5ms of fence against the baseline's 108.6ms - which is what `bd_render_scale` is for. Not yet
  looked at through the lenses.

Best mono is **7.8 fps**, best stereo **6.1 fps**, against 13.9ms for 72 fps. Foveated rendering was
dismissed on the since-disproved reading that the frame was not fill-bound, and should be
reassessed.

### The CPU floor, attributed

At `bd_render_scale=25` the GPU fence is **0.1ms** and the frame is 71ms, so the CPU is the entire
port. Measured on device at that configuration:

| | of ~56ms |
| --- | --- |
| our draw recording | ~13ms (mutex 0.4, bindFB 2.0, flushState 10.5) |
| guest code | **~43ms**, overwhelmingly one function |

`bd_guest_census` counts calls into named guest functions per frame. On device:

```
bdSceneNodeDrawSingle   2084 calls   16130160 bytes of guest code
bdAnimBoneEvaluate       126 calls     706608
bdAnimationUpdate        112 calls     203840
everything else          0-3 calls    negligible
```

**23x the next consumer**, where the desktop build showed 1.9x - so tune this on the device, not on
the desktop. And the scene is **about a thousand individually placed objects**, not instanced
geometry: 2083 calls take 1270 distinct first arguments. So the fix is removing objects, and the
axis is distance.

Two culling knobs exist, both hooked into `bdSceneNodeCullTraverse` (`0x82282490`) so the guest
skips the draw on its own path with no control flow redirected:

| cvar | how it selects | measured |
| --- | --- | --- |
| `bd_cull_distance` | view-space distance; the centre at `r3` is view space with the camera at the origin, so it is a plain length | the intended lever |
| `bd_cull_bias` | bounding radius | 11% CPU, inside noise - it culls small objects and the cost is per object |

### The three fill levers

All three scale a surface the guest asks for, on the seam `bd_supersampling` already uses, so the
guest computes its own viewports, resolve rects and post-process chain from the smaller size. None
of them suppress draws - draws are free, fragments are not.

| cvar | default | what it does |
| --- | --- | --- |
| `bd_render_scale` | 100 | Scene colour and depth at N% per axis. 50 quarters the fragment cost. |
| `bd_shadows` | true | Off renders the sun shadow map at 64x64 instead of 1024x1024. |
| `bd_reflections` | true | Off pins the planar reflection to its 128-wide floor. The reflection re-renders the scene, so this is the largest single pass to remove. |

Bodies in `src/gpu/hooks/tweaks.cpp`, addresses in `config/hooks/render_tweaks.toml`. **Off forces
the smallest legal surface rather than skipping the pass**, because the receivers sample those
textures and a pass that never runs leaves them reading whatever was there before.

Do not reach for `D3DDevice_CreateSurface_hook` for this. Scaling there means lying to the guest
about a size it still computes viewports and texel offsets from, which needs a ratio fixup at every
site that reads back. It was written that way first and replaced.

See `research/20260829_0230_the-frame-is-fill-bound.md` for the full trail and
`research/20260828_2330_the-real-bottleneck.md` for the eliminations it corrects. Note that
`src/xr/xr_cull.cpp` is **dead code** - written, unit-tested, never connected to anything.

### The Quest frame budget

**The GPU is 97% idle.** At the Android defaults a frame is 32ms, of which 1ms is the GPU fence and
17ms is guest simulation and command recording. This inverts the usual mobile-VR advice, so before
optimising anything, read `research/20260828_2010_quest-frame-budget.md`:

- Shaders, fixed foveated rendering, ASTC transcoding and MSAA replacements all save **nothing**
  right now. They are GPU-side, and the GPU finishes in 1ms.
- LOD and culling are the exception, because they cut draw calls, which is CPU.
  `bdCameraViewFrustumTest` (0x82135030) is the seam and is already named.
- Fixed foveation becomes worth having *after* stereo, when the scene is drawn twice. Quest 2 has no
  eye tracking, so `XR_FB_foveation` is fixed-only.

Two defaults exist because of that note, and both are Android-only: `bd_max_render_height` 720 (the
renderer otherwise sizes the scene to the 3664x1920 headset panel, costing 119ms a frame for a
1280x720 game) and `bd_shadow_dimension` 1024.

**Measure on gameplay, not on the title screen.** Every number in that note was taken at the title
screen, and the title screen is not representative of anything - it is a static 2D image with the
GPU 97% idle. In an actual field scene, at the same 720p cap, the same build costs:

```
frame 237ms = xrWait 7.3 + submit 0.2 + present 0.0 + fence 108.3 + drain 1.6 + elsewhere 119.3
```

**Both ends are loaded**: ~108ms GPU and ~119ms CPU, against 1ms and 17ms at the title. So the
conclusion drawn there - that the GPU is idle and only CPU work matters - is true of the title
screen and false of the game. The MSAA default was chosen on that basis and is the first thing to
re-test: a real scene runs a multi-pass post chain that a static title screen never touches, and 4x
MSAA on a tiler pays for every one of those passes.

The honest state: the title screen runs at its native 30 fps, and gameplay does not. Do not quote
the title-screen numbers as the port's performance.

**A near-zero GPU fence does not mean the GPU is idle.** It means the GPU was not the last thing
waited on. A blocking present upstream hides the entire cost.

### What already works

ARM64 is not a new frontier here. Upstream ships a `linux-arm64` AppImage from CI and a `mac-arm64`
app, the ReXGlue SDK publishes a `linux-arm64` slice and has a dedicated ARM64 contributor, and the
recompiled guest code is ordinary architecture-neutral C++. The CPU side of "make it run on ARM" is
already done. `src/core/threading.cpp` is the only file with an x86 intrinsic (`_mm_pause`), and it
already falls back to `std::this_thread::yield()`.

**So the AYN Thor problem is not an ARM problem — it is an Android problem.** The Thor runs
Android 13 on a Snapdragon 8 Gen 2 (Adreno 740; the Lite is an SD865/Adreno 650). There is no
official Linux for it.

**Decision: native Android APK.** The proot/Termux route (running the existing `linux-arm64`
AppImage under a Debian rootfs) was considered and set aside — Vulkan on Adreno inside proot means
Turnip/AdrenoTools or Zink, and both performance and driver stability are a coin flip. The APK path
is also the one that shares work with Quest.

### What an Android target actually needs

Roughly in dependency order:

1. **ReXGlue SDK cross-built for `android-arm64`.** No such release slice exists;
   `.github/scripts/fetch-deps.sh` knows only `win-amd64`, `linux-amd64`, `linux-arm64`,
   `mac-arm64`, `mac-amd64`. The SDK source is public and already builds for aarch64 Linux, so the
   delta is bionic vs glibc, no GTK, Android's signal and threading rules, and its SDL3/ImGui/Tracy
   dependencies. This is the gate on everything else. Build it with `REXSDK_DIR` pointed at a source
   tree rather than chasing a release.
2. **Guest address space.** Xbox 360 recompilations reserve a fixed large virtual region so guest
   pointers are base + offset. Verify the reservation succeeds under bionic, and check page-size
   assumptions: Android 15+ requires 16 KB page support, while Horizon OS on Quest 2 is Android 12
   era at 4 KB.
3. **Vulkan through plume, on Adreno.** Prior art (`SansNope/UnleashedRecomp-Android`, an AI-built
   Android port of Unleashed Recompiled) needed a replacement Mesa Turnip driver for several Adreno
   generations, sysmem render mode on Adreno 6xx to dodge corruption and hangs, and MSAA disabled on
   Adreno 750. Expect the same class of problems. **Check BC texture support first** — Xbox 360
   textures are DXT1/3/5, and if `vkGetPhysicalDeviceFormatProperties` says no BCn, you need CPU
   decompression or an ASTC transcode, which is a real perf and VRAM cost.
4. **Toolchain.** NDK r29-ish, a CMake toolchain file, and a new `android-arm64` preset. Note that
   host tools (`rexglue`, `XenosRecomp`, `dxc`, `reblue_prelink`) must build for the *host* and run
   there; only the runtime cross-compiles. `REBLUE_D3D12` is already forced OFF off Windows, so the
   Vulkan-only path comes free.
5. **Platform layer.** `src/platform/` assumes a desktop: GTK/SDL file dialogs, desktop shortcuts, a
   crash handler, WinHTTP/libcurl. Android needs SAF for file access, an activity/APK harness, and
   probably Oboe for audio latency even though SDL3 audio should function.
6. **The installer.** `src/installer/` is a desktop wizard that wants three DVD images and produces
   roughly 15 GB of extracted output. On Android this becomes either a SAF-driven import flow or a
   "put the files here yourself" path under `Android/data/`. Set `REBLUE_BUILD_INSTALLER=OFF` early
   and side-load a pre-extracted `game/` directory to unblock the rest.
7. **Input.** SDL3 gamepad covers handhelds with physical controls. Quest has none — Touch
   controllers are OpenXR input, not HID gamepads, so Quest needs either a paired Bluetooth pad or
   an OpenXR-to-action-map shim.

### Hardware acceleration on device

Not a footnote — an Android build that ignores this is unplayable regardless of how good the camera
is. Prior art (`SansNope/UnleashedRecomp-Android`) needed a replacement Mesa Turnip driver for
several Adreno generations, sysmem render mode on Adreno 6xx to dodge corruption and hangs, and MSAA
disabled on Adreno 750. Plan for driver selection (bundled Turnip plus AdrenoTools import) rather
than trusting the stock blob.

**Check BC texture support before anything else.** Xbox 360 textures are DXT1/3/5. If
`vkGetPhysicalDeviceFormatProperties` reports no BCn, you need CPU decompression or an offline ASTC
transcode — a real cost in VRAM, load time, and install flow. `src/gpu/format.cpp` and
`src/gpu/texture_upload.cpp` are where it lands.

Also on the list: MediaCodec hardware video decode behind `bdMovieFrameConvert` / `bdMovieYuvQuadDraw`,
Oboe for audio latency, XMA decode cost (`XMAPlaybackConsumeDecodedData`), and on Quest specifically
fixed foveated rendering (`XR_FB_foveation`, large and nearly free) and Application SpaceWarp
(`XR_FB_space_warp`, Vulkan-only, needs motion vectors — and the game is natively 30 fps against a
72 Hz display, so this may be the difference between shipping and not).

### The VR camera

**The guest camera pipeline is already fully named in `config/functions.toml`.** This is the single
luckiest fact about the project — these addresses do not need finding:

- `bdCameraViewSetMatrices` (`0x82135228`) — the interception point; sets view and projection together
- `bdCameraViewFrustumTest` (`0x82135030`) — culling. Test once against a volume enclosing *both*
  eyes plus a head-motion margin, not twice per eye and not a single eye's frustum scaled up; that
  is what the VR industry converged on and it costs about half what per-eye queries do
- `bdBuildProjectionMatrix` (`0x82168E18`) — already hooked by `config/hooks/output_resolution.toml`
- `bdFieldCameraSetupFollow` (`0x821B1A58`), `bdCameraLookAt*`, `bdScriptOpCameraControl` — the
  game's own camera logic, which third-person mode suppresses
- `bdCameraRenderMotionBlur` (`0x8213CE90`) — disable in VR, non-negotiable
- `bdPlayerFieldMovementUpdate` (`0x82207858`) — locomotion, must become head-relative

Hardware reality: Quest 2 is an Adreno 650 (roughly SD865 class) with ~6 GB RAM and a practical
per-app ceiling around 4 GB, weaker than the Thor's 8 Gen 2, and stereo doubles raster cost at
72–90 Hz. The game being natively 1280x720 at 30 fps is the saving grace. Quest 2 was discontinued in
September 2024 with support running to roughly 2026–2027 — a fixed, non-improving target, which at
least means optimising for one known device.

### The key scheduling insight, and how it actually went

The original plan said: VR is the priority but Android is the blocker, so build and debug the VR
camera on desktop Vulkan against a PC OpenXR runtime, with a real debugger and RenderDoc, and port
to Android only once the camera is right. **That is still the right instinct, and it is not what
happened** - this machine has no vcpkg, so the desktop targets never linked, and the entire port was
built on-device with a 60-second loop and printf.

What made that survivable was not tooling, it was making the machine answer questions instead of
guessing. Every hard bug this port has hit was found by adding one line of output and reading it:

- 124ms of a 150ms frame in the flat present - found by printing a frame breakdown, which also
  killed three plausible hypotheses in the same line.
- A fatal file load 25 minutes into a renderer investigation - found by reading the log rather than
  the renderer.
- A camera seam that was reached but composing wrongly - found by logging the head pose and seeing
  it move.

So the rule that generalises is not "use the simulator". It is **make it visible before debugging
it**, which is the same lesson recorded in the devloop skill.

### Order of work

Items 1-5 of the original plan are **done**: the plume OpenXR seam compiles and runs, the SDK
cross-builds for `android-arm64`, `src/xr/` is complete on both sides of the OpenXR line, and the
game composites into a headset at its native frame rate with working controllers. What is left:

0. **Measured on the Quest, 2026-08-29, with everything from that day in.**

   ```
   [xr] 29.7 fps | frame 33.7ms = xrWait 13.1 + fence 0.2 + drain 0.5 + elsewhere 19.8
   stereo disparity: far +3px, near -33px, near - far = -36px  -> crossed, correct
   ```

   **Stereo, on hardware, at the game's native rate**, in the 60/2 tier - `bd_xr_refresh_rate=60,
   bd_render_scale=25, bd_reflections=false, bd_cull_distance=350, bd_stereo=true,
   bd_stereo_separation=0.02`. Stereo used to cost a whole tier (20 fps / 50ms); it no longer does.
   Frame rate still varies 20-30 fps with what is on screen, so quote the range, not the best.

   **`REBLUE_RELAXED_GUEST_MEMORY` hangs the game on ARM64. Do not enable it.** This file used to
   say `volatile` was "presumably what stops a guest spin-loop's load being hoisted, and nothing has
   established Blue Dragon has no such loop". Now something has: with the flag on, the app starts,
   logs 67 lines, and sits there alive and stuck - no crash, no error. On x86 it was worth exactly
   **0%** (17.9ms either way), so there is nothing to trade for the risk. Two traps if you try it
   anyway: the relax step is a POST_BUILD on `reblue_codegen`, so it does nothing unless codegen
   actually re-runs; and it rewrites the **shared** `generated/` header, so a desktop configure
   silently relaxes the Android build too.

1. **Two things a user spotted immediately, both real, both open.**
   - **2D overlays land across the eye seam, not in each eye**, and this is partly fixed. The
     mechanism: 2D draws fail the `scene_pass` gate, so they were emitted once at full viewport
     width and straddled the join. `VideoState::overlay2D` now marks the guest's glyph batch and
     the stereo path submits it to both half viewports **with no eye offset** - an overlay belongs
     at the same place in both eyes, and parallax would push the HUD into the world.

     **The discriminator has to be the glyph batch's fixed EA and nothing looser.** Two wider tests
     were tried and both quadrupled the frame: "came through the user-pointer path", and
     `IsScreenSpriteQuad`. A full-screen *post* blit is also a four-vertex triangle strip at the
     sprite stride, and doubling one squashes the whole source into half the target. Only
     `pVertexData == kTextQuadBatchEA` separates them.

     **The right discriminator is known and is not a vertex shape.** The call graph names the
     guest's own 2D path: `Visual__DrawSortedQueues` -> `Visual__DrawVerticesUP` (0x82425710, only
     two callers) is where Blue Dragon flushes its sorted 2D queues - sprites, the intro credits,
     the HUD - and it wraps `D3DDevice_BeginVertices`/`EndVertices`. Bracketing that call marks
     every draw inside it as an overlay. `VideoState::overlay2DScope` exists for it and is read.

     **What blocks it is one unexplained symbol, and three explanations are already ruled out.**
     `REX_HOOK_RAW(Visual__DrawVerticesUP)` fails to link with `duplicate symbol`, while
     `bdSoundBankPlayCue` is replaced by the identical macro and links. Eliminated: it is **not**
     COFF-versus-ELF (`ld.lld` rejects it on Android too), **not** the anonymous namespace (moving
     the hook to file scope changes nothing), and **not** a double definition (`Visual__DrawVerticesUP`
     appears once in `generated/`, in exactly the same three places as the function that works -
     `reblue_recomp`, `reblue_init`, `reblue_register`).

     `DEFINE_REX_FUNC` emits `name` as `__attribute__((weak, alias("__imp__" name)))`, which a
     strong host definition is supposed to override. Why that override takes for one symbol and not
     the other is the open question - and worth answering, because it gates every function
     replacement, which is the mechanism the whole renderer rewrite depends on.

     **The route that does not need it**: a pair of midasm hooks on the entry (0x82425710) and the
     return, using the mechanism `config/hooks/*.toml` already uses. No symbol replacement, no
     linker semantics.

     Still open until then: the credits text ("Akira Toriyama") lands in one eye. The proper VR
     answer for all of it is a head-locked layer - `bd_vr_hud_mode` is defined, stored in
     `xr::Settings`, and **read by nothing that renders**, so that is dead config waiting on the
     same discriminator.
   - **The CPU cost is the recompiled guest, not the renderer and not draw submission.** Measured on
     the desktop, RTX 3060, 1920x1080, VR + stereo: `dt 17.9ms, fence 0.35ms, draws 2070`, and the
     renderer's own share of that frame is `mutex 0.1ms, bindFB 0.3ms, flushState 2.1ms` - about
     **2.5ms of 17.9**. The GPU is idle and the renderer is cheap; the remaining ~15ms is guest
     simulation, which matches the Quest's ~46ms of guest time.

     Two things were tried against it and **neither is a result**, so do not repeat them:
     **multiview** measured *slower* than side-by-side (20.6ms against 18.3ms), and note
     `NoteDraw()` is called once per *guest* draw before the per-eye loop, so the `draws` column
     cannot see stereo's doubling at all - any argument from that number is void. **Vectorising the
     per-draw constant byte-swap on x86** (it had no SIMD path where ARM64 did) moved 18.3ms to
     17.9ms, 2%, which is noise. It is kept because it is strictly correct, not because it helped.

     **So the performance work is the guest CPU** - the "Rewriting the recompilation is in scope"
     section - and not the renderer, not draw submission, and not the GPU.

2. **Stereo works, on the side-by-side path.** `bd_stereo=true` gives genuine depth: measured disparity far +21px, near +5px,
   **near - far = -16px** - crossed, correctly signed, monotone with depth. Before it was flat to
   2px. Verified from a capture with `bd_capture_after_s`; nobody has to wear the headset to check.

   The bug was that both paths applied the eye offset as `sep * clip.z`, and `clip.z ~= clip.w` past
   a few metres, so it divided out to a constant sideways slide with no depth at all. A lateral eye
   translation is a **constant** added to `clip.x`. Two more things had to be right at once: the
   shared constants must be multiview-only (`SV_ViewID` is 0 in the side-by-side path, so the shader
   was adding `-sep` to *both* eyes on top of the host patch), and the **left eye takes the positive
   constant** - backwards renders the scene pseudoscopic, and that is invisible in a symmetric test.

   **Multiview: the per-eye skew was unreachable code, and that is fixed. It now applies to the
   wrong draws.** Two findings, both from capturing *both array slices in one frame* -
   `bd_capture_after_s` does that now when the present source is a multiview target, which is the
   only way to compare eyes without a cross-run confound.

   First: the skew was emitted at the **end** of the generated vertex shader, after the guest's own
   `return;`. Dead code. It compiled, DXC dropped it, and the SPIR-V declared `OpCapability
   MultiView` while carrying **no `BuiltIn ViewIndex`** - healthy-looking and completely inert.
   Fixed in the XenosRecomp fork by emitting it before the return; the two views of one frame went
   from bit-identical to a mean difference of 14.6.

   Second: the disparity was then a **uniform +38px at every depth** - ~0.04 NDC, which is
   `2 * separation` at `w = 1`, so what was being skewed was a **full-screen post quad**. The shader
   skew ran in every vertex shader, and a post pass drawing at `w = 1` slides the finished image
   instead of adding parallax. Fixed: `VideoState::stereoEligible` is set per draw before the
   constants flush, and the shared stereo constants are zero unless the draw is scene geometry -
   the same `scene_pass` gate the host's side-by-side patch always had.

   Third, and **this is the remaining blocker, and it is architectural rather than a bug**: the post
   chain writes both layers but **reads one**. `surface_pool` builds each surface's sampling view as
   a plain 2D view of layer 0, because the bindless heap it is registered in is declared
   `Texture2D`. So the scene renders in stereo and the first pass that reads it flattens the pair.

   Doing it properly needs a second bindless heap declared `Texture2DArray` sampled by `ViewIndex`,
   which reaches into XenosRecomp's texture declarations, the descriptor set layout, and every
   `tfetch` - and on Adreno has to fit inside a `maxBoundDescriptorSets` of 4 that is already full.
   **Do not start that casually.**

   **The cheap way round gets the win anyway**: the expensive part is the scene (~2000 draws against
   a couple of dozen full-screen passes), so render the scene with multiview - one draw, two layers -
   then **resolve the two slices into a side-by-side image on a single-layer target** before the post
   chain, which then runs mono exactly as it does for `bd_stereo` today. One blit, no descriptor
   changes, and `xr_session` already splits a side-by-side image into per-eye `imageRect`s. The seam
   to insert it at is the render-target change from the two-layer scene surface to a single-layer
   post surface, which `NoteDrawTarget` already observes. See
   `research/20260829_1900_multiview-needs-a-resolve-not-an-array-heap.md`.

   **Until then use `bd_stereo`**, which works and is unregressed: `far +4, near -5,
   near - far = -9px` on the flat desktop, bit-identical to the measurement taken before any of the
   multiview work.

   **Multiview: the pipelines were never the problem, the resolve runs on the wrong surface.**
   Measured 2026-08-30 on the desktop, no headset:
   `MULTIVIEW pipeline created, viewMask=3` (they are created), framebuffers get `viewMask=3`,
   `SetRenderTarget` reports `mv=true` for the layered 1920x1080 surface, and there are no errors -
   but the resolve fires **501 times on a 120x67 bloom target and never on the scene target**. The
   scene renders into two layers correctly and the pass that flattens them never touches it. Note
   that `[mv] N mono pipelines so far, 0 multiview` was read as proof pipelines were mono; that
   counter only prints at its 40th and 200th, both during startup before a layered target is bound.
   See `research/20260830_0400_multiview-resolves-the-wrong-surface.md` for the trigger site and the
   three candidate causes.

   **Multiview stereo works, and has verified depth.** `bd_stereo_separation=0.2`,
   `bd_stereo_multiview=true`: `tools/stereo_check.py --stacked` reads **far -2, near -8** -
   crossed, monotone with depth, correct. Two bugs, both two-line:

   - **`bd_stereo` and `bd_stereo_multiview` were both on and nothing stopped that.** They are two
     implementations of one thing. Together, each draw is submitted twice into half-width viewports
     *and* replicated into both layers, so both layers carry the same side-by-side pair - the
     "identical layers" symptom - and every triangle rasterises four times, which is why multiview
     once measured *slower* than the path it replaces. **That number is void**, as is every other
     multiview measurement taken here: the desktop profile had both set.
   - **The per-eye sign was inverted.** View 0 is the left eye and takes the *positive* constant.

   And three instruments were lying, which is why it took two sessions:
   **`bd_mv_capture_array` decoded an `R16G16B16A16_FLOAT` scene target as RGBA8** (and overran its
   readback buffer, and stacked the second slice half a slice early), so the standing figure
   "the layers differ by mean 3.694, 23.2% of pixels" was misaligned halves of misinterpreted bytes;
   a `[mv] SetRenderTarget` log sampled **every 4000th call of a per-slot function**, so it never
   moved off one slot and reported `mv=false` for ever; and `[mv] resolved ... (N times)` prints at
   the 1st and 501st, so two lines meant *501+* resolves, not two.

   **A periodic sample whose period shares a factor with the thing sampled is not a sample** - the
   companion rule to "a bounded log answers what happened first, never what happens".

   **The stock default now gives correct stereo on the multiview path**, no tuning:
   `far -4, near -26`. `bd_stereo_separation` is calibrated on the side-by-side path and the
   multiview shader needs **23.3x** the same number for the same parallax, so it is converted at the
   multiview seam - one knob, one meaning. Measured in one field scene, one build, both paths, which
   is the only comparison this workload supports: side-by-side 0.03 gives 11px of a 960 eye,
   multiview 0.7 gives 22px of a 1920 layer, the same angle, and the response is linear in between.

   Still open: **why the ratio is 23 and not 1.** Both paths add a constant to `clip.x` and a
   multiview layer is twice the width of a side-by-side eye, so it should need *half*. Likeliest
   suspect is the host's constant landing on a coefficient of a position component that is not `w`,
   which would scale it by a typical guest coordinate - and a Blue Dragon unit is a centimetre,
   which is the right order of magnitude. See `research/20260830_0700_multiview-has-depth.md`.

   **Superseded, kept for the reasoning only:** The post chain was mono
   because `surface_pool` only gave two layers to surfaces at or above a quarter of the design
   canvas - so the bloom and downsample targets were single-layer, took single-view pipelines, wrote
   layer 0 and collapsed the pair on the first post pass. That threshold is gone: every render
   target is two-layer under `bd_stereo_multiview`, confirmed on desktop as `1920x1080`, `960x540`,
   `480x270`, `240x135`, `120x67` all at `viewMask=3`, and **layer 1 is now populated where it used
   to be black**. But the two layers come out *identical* - `SV_ViewID` is still not varying the
   skew, with a fresh shader cache and validation clean. That is the remaining multiview bug. The
   side-by-side path is unaffected and is what works.

   **Cost is on the GPU and `fence` does not show it.** CPU only moves 19.0 -> 20.2ms, but the frame
   goes 34.6 -> 50.0ms because the compositor drops a pacing tier. `render_scale=20, cull 250,
   shadows off` claws back to 40.8ms / 24.5 fps. Getting stereo into the 33.3ms tier is the next
   performance job, and it is a fill problem with proven levers.
   See `research/20260829_1700_stereo-has-depth.md`.

2. **The occlusion descriptor set**, which is dropped rather than fixed on Android and costs four
   pipelines. Collapse sets 0/1/2 - one physical set bound three times to satisfy three HLSL
   register spaces - and the layout fits Adreno's four without dropping anything.
3. **Fixed foveated rendering** (`XR_FB_foveation`; Quest 2 has no eye tracking, so fixed only).
   Worth nothing today and genuinely useful the moment stereo lands.
4. **Tourist mode** whenever a quick win is wanted. It touches nothing else, and
   `src/engine/character.h` already exposes `PlayableCharacter::SetHP()` with
   `src/engine/stat_breakdown.h` documenting exactly which block to write for maxed stats.
5. **Cel shading last**, and only once there is a real frame budget to measure against.

Do not start with cel shading. Do not optimise the GPU before re-measuring - see the frame budget
section, where the intuitive moves all save nothing.
