# Native alpha policy and publication

2026-09-04, Windows Vulkan desktop. This is another rendering boundary conversion,
not full host frame ownership or Quest qualification.

## Source and contract

Read the translated SDK functions and PPC comments, not a decompiler inference:

| Engine offset | Setter | Native field / temporary device shadow |
| --- | --- | --- |
| 96 | `0x82471380` (`generated/reblue_recomp.87.cpp`) | Enabled low bit; control `+10428` bit 3; dirty `+16` bits 9 and 50 |
| 100 | `0x824717E8` (`generated/reblue_recomp.100.cpp`) | Reference float at `+10372`; dirty `+24` bit 8 |
| 104 | `0x82471850` (`generated/reblue_recomp.0.cpp`) | Eight compare functions; control low three bits; dirty `+16` bit 9 |
| 336 | `0x82472930` (`generated/reblue_recomp.32.cpp`) | Alpha-to-coverage request; control bit 4; dirty `+16` bit 9 |

The setter table in the local extracted image confirms these addresses. Offset
308 is high-precision blending, not alpha-to-coverage. Alpha-to-mask offsets at
340 remain a compatibility setter; custom sample-position behavior is not newly
implemented or qualified here. Other control bits must survive publication.

`0x8200167C` contains float bits `0x3b808081` (the SDK's 1/255 scale).
The native reference reproduces unsigned-int-to-float then float multiplication.
This corrects the former host hook's 1/256 scale: reference 255 now means 1.0,
not 0.99609375. This is an intentional cutoff correction, not bit-identical
preservation of the old host shader behavior. Enabled/coverage values use the
SDK low bit, rather than treating every nonzero integer as true.

`native_alpha.h` defines named enabled/compare/threshold/coverage intent.
`alpha_import.h` isolates engine enum decoding and exact shadow publication.
`native_alpha_bridge.cpp` owns the live intent, bootstraps once, and replaces the
four normal setter executions. It preflights mapped ranges, dispatch identities
and the reference-scale constant before effects. Unsupported inputs or the
explicit off switch retain counted compatibility calls. Engine cache/getters
remain temporary adapters, not the native source on each ordinary draw.

Ordinary flush composes current alpha intent, not a previously replayed pipeline.
Alpha-to-coverage is enabled only when requested and the current target is
multisampled. Cutout suppression remains an explicit diagnostic. Replay still
captures pipeline specialization and cutoff, temporarily overrides the binding,
and restores it; its retained material/pass recipes are not converted here.

## Shared shader predicate

XenosRecomp defines host compare modes in specialization bits 6..8. Zero is
greater-equal, preserving the previous default for finite values. The eight
functions are explicit; numeric modes reject unordered values, Always accepts
them, and a disabled alpha test accepts every fragment. Shared C++/HLSL code
spells ordered not-equal as `< || >`: inspecting the initial compiled SPIR-V
caught that HLSL/DXC `!=` was ordered while C++ `!=` accepts NaN. CPU tests now
also cover NaN reference and both operands NaN. These are native semantics,
not a claim of complete Xenos exceptional-value emulation.

The emitter publishes the full specialization mask and calls this predicate
before discard. The four host normal/wind/bloom overrides use the same helper.
The existing PSO key/mask carries the compare mode. SharedConstants remains
352 bytes and PipelineState remains 158 bytes; no packing ABI was expanded.

## Build and shader verification

- Standalone `out/native_texture_test`: all 10 tests pass, 0.64 seconds in the
  final semantic test run. Alpha tests cover every compare boundary, infinities,
  NaNs, target sample count, replay-like destination contamination, and 16000
  randomized setter/shadow cases including the full 0..255 reference range.
- Built the configured `reblue` target in `out/build/win-amd64-release`, Vulkan
  only, OpenXR/PCH on; exit 0. XenosRecomp and the generated shader cache rebuilt,
  all four host overrides compiled and `reblue_vk.exe` linked. Codegen reported
  zero files written/deleted and the guest module up to date. No guest rebuild
  or build-tree wipe.
- Isolated final dump: `out/verification/native_alpha_final_shaders`, 141
  original SPIR-V outputs and 86 emitted HLSL shaders calling `BD_AlphaPass`.
  `bd_normal_ps` reports mask `0x1E2`; assembly has SpecId 0, mask 448, shift 6,
  all eight switch destinations, ordered comparisons and `OpKill`.
- An independent DXC `ps_6_1` / HLSL 2021 / Vulkan 1.1 compile produced identical
  bytes to the final tool's normal shader. SHA-256:
  `DD0C22A8EA03E966C3FFA5D0A1D10F593FBA9EBC796C2AC2F894668DA309FD9B`.
  Inspected not-equal uses ordered less/greater with control flow, not C++'s
  unordered `!=` semantics. Dumps/probes/game data remain local and untracked.
- Dependency foundation `c47235b` and final emitter/predicate `411e2f1` pushed
  to `noeldvictor/XenosRecomp:reblue` before the parent gitlink.

## Desktop publication comparison

PID 21320, 23:27:25-23:31:40 EDT, `reblue_678.log`. Profile audit accepted all
six settings: autoplay/perf CSV on, capture after 60 seconds, minimum 600 draws,
120 frames, and `bd_native_alpha_verify=true`. Native alpha defaults on.
The original setter executes only for this correctness comparison; predicted
publication is checked against all 12188 device bytes and the engine cache word.
Draw checks detect untracked cache writers without silently repairing intent.

Final report: 7196829 setter checks and 7108657 ordinary draw-intent checks,
zero publication mismatches, cache drift, refusals, compatibility or legacy
draws; one bootstrap import. Setter coverage: 3483900 enable, 3584419 reference,
128510 function, zero coverage. All 2710070 enabled draw intents were GE.
Counts are CPU publication/flush coverage, not fragment execution counts.

Isolated `out/verification/native_alpha_verify`: 120 captures at 1920x1080,
last sequence 119 at frame 2923. There are 0/119 jumps above 6% and no cyan
patches (maximum cyan 0.02%). Inspected actual sequence 0 and 119: character,
cutout foliage, terrain and moving shadows are visible. This comparison preceded
the ordered-NE correction; that mode was not exercised. Normal final-binary
verification is recorded below. No errors/critical messages, Vulkan failures,
overflow, exhaustion or retirement-race messages were found in the run log.

## Final normal flat path

PID 25420, 23:34:24-23:36:13 EDT, `reblue_679.log`. All five original profile
settings took effect; alpha verification was removed, and native alpha remained
on by default. Final report: 2274942 native updates, 3294769 ordinary draw-intent
applications, zero comparison/compatibility/legacy calls or refusals, one import.
All 1142965 enabled draw intents used GE; no coverage requests occurred.

`out/verification/native_alpha_flat` contains 120 captures at 1920x1080, ending
at frame 2898. There are 0/119 jumps above 6% and no cyan patches (maximum 0.01%).
Inspected actual sequence 0 and 119, including the character, cutout foliage,
scenery and changing shadows. This is the final ordered-NE binary, though the
field still only exercises GE. The run log has none of the error/failure/storage
messages checked above. This short field slice is not later-scene qualification.

## Remaining ownership and coverage

Engine cache/getter shadows, pass/material producers, remaining other-state and
sampler paths, blend constants, replay recipes and shader-register packing remain.
The six non-GE numeric/never modes and Always have CPU/SPIR-V coverage but no
field GPU exercise here; alpha-to-coverage likewise has no field requests.
Custom coverage offsets and actual multisample coverage output still need work.
Short field captures do not requalify the previously broken later scene, menus,
battles, cutscenes, transitions or both-eye correctness. The complete host-frame
and full desktop gate remain open; no Quest or Thor was used.
