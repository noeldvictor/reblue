# Native nested pass scopes

2026-09-05, Windows Vulkan desktop.

## Source and change

The guest-source guide directed inspection of `render_tweaks.toml`,
`render_list.toml` and the exact translated functions. Despite its name,
`bdSurfaceSetMSAA` (0x82273080, generated file 67) saves four colour handles
and one depth handle, increments their references, binds colour zero and
depth, clears the other colour slots and pushes a logical content extent.
It does not select a multisample count. `bdDestroySurface` (0x82273240,
generated file 94) restores those attachments and releases the saved references;
it is a pop, not surface allocation destruction. The getter wrappers
`sub_824739F0` / `sub_82473A38` read device +12168 / +12184 and add references.
`sub_82474388` tails the already replaced render-target setter.

The complete supported push/pop bodies are now host replacements. The
SDK-independent `NativePassStack` holds native attachment references and
logical extents. It saves the actual live targets at each entry, including
changes made by another rendering entry point. Depth-only/null passes inherit
logical dimensions; physical viewport dimensions still follow the bound image.
The native stack is not limited to seven levels. The temporary engine adapter
preserves its seven-level overflow no-op and empty-pop behavior.

Shared host attachment binders own the format, sample-count, multiview,
foveation, framebuffer-invalidating and viewport changes. Compatibility D3D
setters delegate to these same binders. No queue flush is introduced at a
target change: that boundary can run without a command list, so the outgoing
draw queue still flushes at `BindDrawFramebuffer` before the next framebuffer.

The adapter maintains big-endian getter shadows and saved-reference mirrors
for remaining engine readers, through the existing host reference/lifetime
implementation. Native pop restores its saved host references, not handles
re-read from the engine stack. A mirror check detects foreign stack writes;
it is not an independent comparison with original execution. Resource lookup
and reference release stay outside the video lock. Unsupported inputs or
additional colour attachments refuse before effects; original scopes above
native scopes unwind separately. The `bd_native_passes` correctness switch
defaults on; an already native-owned nesting chain finishes natively even if
the setting is changed mid-chain.

This removes two guest rendering bodies from supported pass entry/exit. It
does **not** yet replace the engine's traversal, pass scheduling, scene-begin
sampler/camera/effect producers, allocation classification, resolve/alias
adapters, surface wrappers/reference headers or frame-wide guest dependencies.
Multiple colour attachment rendering and all representative scenes remain work.

## Initial verification

- All 16 texture/upload/state/recipe/pass CTests pass; the separate material
  CTest passes, for 17 total. Three reflection/scene source-boundary guards pass.
- New tests cover nested restoration, independent intervening target changes,
  depth/null logical-extent inheritance, explicit zero extents, underflow,
  root-reset rejection during a scope, 24-level native nesting and retained
  image lifetime. They do not simulate engine refcount memory or a Vulkan device.
- The host-only `reblue` Vulkan target linked at 03:55:48 EDT, 47,247,360 bytes,
  source revision `cb4009c` dirty. Codegen wrote/deleted nothing; no guest
  objects rebuilt.
- Initial desktop process 25576 started 03:56:26 EDT, log `reblue_708.log`.
  Original five profile settings were used and all five audited successfully:
  autoplay/perf true, capture delay 60, minimum 600 draws, 120 frames.
  Early coverage has 18867 native pushes, 18866 pops, 901 depth-only and 15232
  null passes, peak nesting 1, no compatibility/refusals and 37733 matching
  getter-shadow checks. This does not qualify pixels or deeper GPU nesting.

Capture inspection and later-scene/final-eye verification are pending at this
initial implementation checkpoint. Known later scenery/text and VR
letterboxing/blur/depth limitations remain open.

## Normal short desktop sequence

Implementation committed/pushed as `9a39837`. Process 25576 was stopped at
03:59:38 after its complete sequence was preserved in
`out/verification/native_passes_flat`. Captures are 1920x1080, frames 2843-2962,
from `frame_1788595048_0.raw` through `frame_1788595051_119.raw`
(03:57:28-03:57:31 EDT). All 119 pairs stay below 6%; none of 120 frames exceed
0.30% cyan, with zero 2-60% cyan patches, median 0.011% and maximum 0.02%.
Actual first/last previews show Shu and the field scenery without broad
banding or a cyan patch. The `--mono` preview command does not make this a
replay-off or pass-off control: all native paths remained enabled.

Last reported pass totals: 211108 pushes / 211107 pops, 10501 depth-only,
159473 null scopes, peak nesting 1; 422215 matching getter-shadow checks,
zero compatibility/refusals/overflow/empty pops. The one-entry difference is
a mid-scope periodic report, not proof of a leak or a balanced shutdown audit.
No error/critical/VK_ERROR or upload-exhaustion entries were found. The full
1673 archives / 119346 record names were mounted. Later scenes, deeper GPU
nesting and stereo are not covered by this short sequence.

## Normal late desktop run

Process 25444 ran 03:59:42-04:06:51 EDT with the same 03:55:48 binary and native
passes enabled; log `reblue_709.log`. The temporary five-setting profile used
autoplay/perf true, delay 270, minimum 30 draws and 120 frames, with all five
audited. Capture waited through a 20-draw loading screen and produced frames
14195-14314 at 04:04:59.092-04:05:02.522. The isolated sequence is
`out/verification/native_passes_late_flat`, from `frame_1788595499_0.raw`
through `frame_1788595502_119.raw`, all 1920x1080.

Last reported pass totals: 429247 pushes / 429246 pops, 21928 depth-only,
330865 null scopes, peak nesting 1; 858493 matching getter-shadow checks,
zero compatibility/refusals/overflow/empty pops. No error/critical/VK_ERROR or
upload-exhaustion entries were found. These are normal native-path counters,
not an original-producer or draw-state comparison.

All 120 frames were analyzed: 110/119 pairs exceed 6%, with zero detected
cyan frames/patches (median and maximum 0%). The first image is a dark
transition; the last shows villagers on the wooden platform. Actual pair
previews 080 and 104 show the recurring rock-wall/background surfaces
appearing/disappearing between frames. Some sequence changes also include
the transition and camera motion; 110 is not a count of independently
classified rendering bugs. This pass conversion does not fix the later
scenery defect, and the window does not requalify text rendering.

## Full-size desktop final-eye setup

Process 25088 started at 04:07:19 EDT, log `reblue_710.log`, same binary.
The absolute local xrsim manifest and its 31232-byte runtime DLL were verified.
Process-only environment: runtime manifest, width 1440, height 1584, head height
0. All 14 temporary profile entries were audited:

```toml
bd_xr_autoplay = true
bd_perf_csv = true
bd_capture_after_s = 60
bd_capture_min_draws = 450
bd_capture_frames = 120
bd_vr_enabled = true
bd_stereo = false
bd_stereo_multiview = true
bd_mv_layered_textures = true
bd_mv_capture_array = false
bd_xr_mirror = false
bd_vr_camera_mode = 2
bd_vr_diorama_height = 0
bd_xr_render_scale = 1.0
```

The log confirms a 1440x1584x2 runtime swapchain and direct final presentation.
The content remains 1440x808 under the existing aspect-fit policy. The normal
late-sequence analyzer overlaps this VR run; no timings or performance claims
are inferred from either process.

## Final-eye result and handoff

Process 25088 was stopped at 04:09:27 EDT after its complete sequence was
preserved in `out/verification/native_passes_vr_fullsize`. Captures are final
stacked 1440x3168 images (1440x1584 per eye), frames 12404-12523, from
`frame_1788595701_0.raw` through `frame_1788595708_119.raw`, recorded
04:08:21.735-04:08:28.930. All 119 pairs stay below 6%. Cyan analysis reports
zero flagged frames/patches, median and maximum 0%.

Both eyes in the actual first/last images were inspected: no broad banding,
but the familiar blur and large black bars remain. Both disparity checks are
**inconclusive**: only 44%/52% bands are usable, at -1/-2 pixels, with a 1-pixel
spread. Full-size eye allocation is not correct native 3D framing or proof of
headset depth/comfort/performance. No pass-off/original-producer image control
was run for this checkpoint.

Last reported VR pass totals: 327532 pushes / 327531 pops, 15901 depth-only,
257562 null scopes, peak nesting 1; 655063 matching getter-shadow checks and
zero compatibility/refusals/overflow/empty pops. No error/critical/VK_ERROR or
upload-exhaustion entries were found. Multiple native nesting levels, overflow,
unwind through unsupported scopes and additional colour attachments have not
been GPU-qualified by these runs.

Both analysis sessions completed; all three owned renderer processes were
stopped. The exact original five-setting profile was restored. No game assets
or saves were manually modified or staged, and no headset/device runs occurred.
Scene-begin producers and native frame scheduling remain the next ownership
boundary; the known later scenery and VR framing defects remain open.
