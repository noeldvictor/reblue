# Native blend production and draw intent

Date: 2026-09-04. Desktop Vulkan only. No Quest or Thor run.

## Change and source boundary

`BlendState` holds named host enable, RGB and alpha factors/operations.
`ApplyBlendState` restores live intent independently of the last bound or
replayed pipeline, preserving raster, shader, target and multiview fields.
Normal `ApplyNativeRenderState` no longer reads or converts device blend words.
Raster/depth/stencil had already crossed this draw-time boundary.

`native_blend_bridge` replaces eight setters reached through `bdSetRenderState`.
The bridge owns requested/effective blend shadows and publishes the engine's
getter/cache/dirty side effects without executing the original setter normally.
It imports once at bootstrap, resets at device creation/engine initialization,
and explicitly reimports after a disabled or unsupported compatibility call.
The normal draw path copies host state, with no per-draw device import. Native
production defaults on (`bd_native_blend`); comparison defaults off.

This is still a producer bridge, not native material/pass asset completion.
Packed requested/effective words, getter storage and `bdSetRenderState` inputs
remain explicit temporary engine adapters. Replay still retains pipeline recipes.
Alpha test/ref/function, blend constants, sampler/other-state execution and
independent material/pass producers remain follow-up work. Constant blend factors
retain the previous ZERO fallback, with an explicit unmapped counter/warning;
this change does not claim support for them or additional render targets.

Sources read before replacement:

| Engine byte offset | Setter | Generated module |
| --- | --- | --- |
| 60 | AlphaBlendEnable, `0x824713C0` | 49 |
| 64 | SeparateAlphaBlendEnable, `0x82471750` | 48 |
| 72 | SrcBlend, `0x824714E0` | 23 |
| 76 | DestBlend, `0x82471570` | 69 |
| 80 | BlendOp, `0x82471450` | 92 |
| 84 | SrcBlendAlpha, `0x82471670` | 52 |
| 88 | DestBlendAlpha, `0x824716E0` | 59 |
| 92 | BlendOpAlpha, `0x82471600` | 73 |

The local extracted image's table sequence (file offset `0x749D68`) confirms
these identities next to the previously verified raster entries. The live
dispatch table at device+56+offset must match before any native effects.
Offset 68 is the blend constant setter, **not** BlendOp.

The setters maintain requested state at device+11576, enable/separate flags at
+11580, four effective words at +10424/+10456/+10460/+10464 and dirty bits
`0x407` at +16. RGB changes retain gated requests; alpha-only changes publish
only when both master and separate-alpha are enabled. Shared-alpha folding
preserves the SDK's saturate-to-ONE alpha rule. Noncanonical boolean input keeps
the SDK's distinction between its low-bit flag and zero/nonzero control flow.

Lower flag bits are shared with other engine setters. `HiZEnable` (`0x82472A50`,
module 76) changes bits 20..22; `sub_82472B48` (module 56) changes bits 23..29.
Publication preserves these and current dirty marks. Blend inputs themselves
come from the host shadow, not a repeated device read.

Correction to the raster checkpoint's active commentary: the claim that blend
necessarily requires a per-draw import because of inline engine writers was
not substantiated by this trace. Literal blend-word writers found were the eight
SDK setters and host device initialization. Other matching offsets in modules
19/20/46/48 were object fields, and address constructions in 15/29/104 were
static data addresses, not D3D device aliases. This is not a proof against every
possible indirect writer; diagnostic update/draw checks detect tracked-word
drift without silently repairing native intent.

## Verification method

The standalone suite has nine passing tests. The added test covers all 65536
shared-alpha patterns against an independent bit-by-bit reference, 32000
randomized setter publications, gated requests, re-enable/separate sequences,
all effective lanes, unrelated bits/dirty preservation, invalid offsets and
native pipeline copy/replay independence. Assertions remain active in Release.

The configured `win-amd64-release` `reblue` target linked `reblue_vk.exe`.
Codegen reported zero writes and its module up to date; guest objects were not
rebuilt. The first standalone sandbox build stalled before compiler launch and
was interrupted; the approved build/test retry passed all nine tests in 0.47 s.

`bd_native_blend_verify` predicts the full first 12188 device bytes, executes the
original engine state call once, then compares every device byte and the updated
engine cache word. It separately compares requested/effective words and the two
owned flag bits before updates and ordinary draw flushes. Other flag bits/dirty
marks legitimately change outside this bridge and are excluded from drift
checks. This is correctness-only execution, not the production path or an FPS
comparison. Final frame captures are still required.

## Runtime evidence

### Comparison: log 675

Process 24752 started at 23:02:39 EDT and was stopped, by verified executable
path, at 23:04:26. The original five-setting profile was temporarily extended
with `bd_native_blend_verify = true`; the audit confirmed all six applied:

```toml
bd_xr_autoplay = true
bd_perf_csv = true
bd_capture_after_s = 60
bd_capture_min_draws = 600
bd_capture_frames = 120
bd_native_blend_verify = true
```

Final periodic counters at 23:04:21.399:

- 4002268 native updates, including 3807552 unchanged engine cache values;
  4002268 original-execution publication checks, zero mismatches.
- 3073105 ordinary native draw flushes/checks and 4002268 update drift checks;
  zero drift, refused calls, compatibility calls, legacy draw imports or
  unmapped factors/operations. One bootstrap import.
- Setter coverage: enable 1468426, RGB src/dst 1266921 each. Separate enable,
  RGB op and all three alpha setters were **not exercised** by this field run.
- The raster bridge still recorded 2334492 compatibility calls for other
  engine states. The blend-specific zero is not zero guest rendering.

Capture sequence 119 was written at 23:03:45.518, frame 2945, 1920x1080.
The 120 raw frames from this run alone were copied after stopping into
`out/verification/native_blend_verify`. Sequence analysis: 0/119 jumps over 6%;
cyan analysis: zero patch frames, median 0.011%, max 0.02%. Actual sequence 0
and 119 images were inspected: character, terrain, vegetation, structures and
moving shadows remain visible, with no new flat-view break seen.
The log had no error/critical, Vulkan error/failed, overflow, exhausted or
retirement-race matches. That absence is not full-game qualification.

The temporary comparison setting was removed before the normal-path run.

### Normal flat: log 676

Same executable, native blend on, comparison off. Process 24876 ran from
23:04:47 to verified stop at 23:06:33 EDT. The original five settings above
(without the comparison line) all applied. Final periodic counters at
23:06:29.399: 4007188 native updates, 3812546 unchanged, 3074842 ordinary native
draw flushes, one bootstrap import. Zero comparison, compatibility, refused,
legacy-draw or unmapped counts. Drift checks were off, not passing checks.
Coverage remained enable/RGB factors only. Other-state compatibility remained
2336939 calls in the raster bridge.

Sequence 119: 23:05:53.702, frame 2956, 1920x1080. The 120 current-run raw files
were isolated after stopping in `out/verification/native_blend_flat`.
0/119 jumps over 6%; zero cyan patch frames, median 0.012%, max 0.02%.
Actual sequence 0/119 images were inspected: character, terrain, vegetation,
structures and shadows were intact in this short field sample. No matching
error/critical, Vulkan error/failed, overflow, exhausted or retirement-race log
messages. The nine standalone tests were rerun: all passed in 0.51 s.

## Qualification limits

The earlier 64-frame multiview flash, banded/blurred eyes, inconclusive stereo
depth and damaged later flat scene are not fixed by this state conversion.
Representative fields, battles, cutscenes, menus, transitions, reloads, animated
effects and both eyes still require qualification. No full host-owned frame or
Quest readiness is claimed.
