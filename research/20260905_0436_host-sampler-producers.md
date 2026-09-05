# Host sampler producers

2026-09-05, Windows Vulkan desktop.

## Source and change

The guest-source guide directed inspection of `render_tweaks.toml` and the
complete translated scene sampler initializer `sub_82184A88` (generated file
70), `bdSetSamplerState` (98) and seven D3D sampler setters (14, 20, 23, 34,
47, 50, 56). The device initialization adapter copies the sampler dispatch
table from 0x827521F8 to device +444.

The complete scene initializer now executes a host plan. It resets min/mag/mip
filters and U/V addressing on slots 0-4, in the original order. Slot zero uses
the three settings at +7052/+7056/+7048; settings 1/2/3 map to boundary values
0/4/2, and other values to 1. Other reset slots use filter value 1. W, border,
other fields and later slots inherit. Unchanged cache entries do not republish
fetch bits or clear an existing dirty flag.

Seven direct setters (U/V/W, border, mag/min/mip), plus the supported changed
path of `bdSetSamplerState`, now execute on the host. The previous hook replaced
only the unchanged early-out and defaulted off; `bd_host_sampler_state` now
defaults on. Unknown offsets, unsupported changed fields and changed dispatch
entries retain a counted compatibility path. Full preflight precedes publication
for changed supported setters and the scene plan. The known-cache early-out
retains the existing render-thread accessor, with explicit slot/offset bounds.

`sampler_import.h` isolates the temporary packed publication ABI. It preserves
anisotropy lookup coupling, separate Z-filter bits, unrelated fetch bytes, dirty
bits, cache writes and the observed return register. It is deliberately **not**
a native material format or an authoritative live sampler store. Several
material functions still write cache/fetch fields inline; claiming setter-only
ownership would miss those later overrides. Ordinary draws therefore still
import fetch state. Native retained recipes already use `RenderSamplerDesc`.
Remaining sampler writers, scene-begin execution and independent pass/material
sampler production still need conversion. This checkpoint removes guest
producer bodies; it does not claim fully host-owned sampling or frames.

## Verification contract

- The new standalone test compares 112000 randomized publications against an
  independent PPC mask/rotate oracle, including all 32 slots, all seven fields,
  full-width input values, every Z-selector byte and unrelated neighbors.
- Writer tests check the exact modified bytes. Scene-plan tests check ordering,
  all 216 selected settings combinations (including unknown/signed bit patterns),
  repeated unchanged resets and inheritance of all untouched fields/slots.
- All 17 texture/state/pass/sampler CTests, the separate material CTest and
  three reflection/scene lock-boundary guards pass: 18 CTests total.
- The host-only `reblue` target linked at 04:36:19 EDT, 47,265,792 bytes,
  source revision `73bdcb7` dirty. Codegen wrote/deleted nothing and no guest
  objects rebuilt.

`bd_host_sampler_verify=true` executes the original once, bypassing nested
setter replacements, and compares the entire device allocation plus relevant
cache/dirty regions and return register with the prepared host publication.
It throws on a mismatch; native writes do not overwrite evidence. Unchanged
engine calls take the direct early-out and are not included in that comparison
count. Comparison runs are not normal guest-free execution or performance tests.

## Initial live comparison

Desktop process 4936 started at 04:37:03 EDT, log `reblue_711.log`, with the
04:36:19 binary. The original five profile settings were retained, with explicit
`bd_host_sampler_state=true` and `bd_host_sampler_verify=true`; all seven audited.
At 04:37:55 the run has 43819 matching publications, 5697 scene default calls,
20136 default changes and 2626 changed engine calls, with no compatibility or
refusals. Direct U/V/min/mag are exercised; direct W/border/mip are not yet
exercised (mip is included in scene defaults). Pixel inspection, the normal
comparison-off run and final-eye checks are pending at this initial checkpoint.

Known later rock-wall/background popping, text qualification, VR blur/letterbox
and inconclusive depth remain open. No headset runs or performance claims.

## Completed comparison sequence

Process 4936 stopped at 04:39:28. Last totals: 146571 matching publications,
21921 default calls, 74216 default changes, 13442 changed engine calls,
3961717 unchanged calls and zero compatibility/refusals/mismatches. The full
1673 archives / 119346 records mounted. No error/critical/VK_ERROR or upload
exhaustion entries were found.

The isolated `out/verification/host_sampler_compare` sequence contains 120
1920x1080 frames, 2837-2956, from `frame_1788597486_0.raw` through
`frame_1788597489_119.raw` (04:38:06.293-04:38:09.583). All 119 pairs remain
below 6%; no frame exceeds 0.30% cyan, with zero patches, median 0.011% and
maximum 0.02%. Actual first/last previews show Shu and the field scenery with
no broad banding or cyan patch. The preview tool's `--mono` flag does not
disable replay or act as a sampler-off control.

The comparison-off process 25480 started at 04:39:41, log `reblue_712.log`,
with the same binary and seven audited settings, changing only
`bd_host_sampler_verify=false`. Its initial host defaults/setters have zero
fallbacks. Normal-path pixels and desktop final eyes remain pending here.
