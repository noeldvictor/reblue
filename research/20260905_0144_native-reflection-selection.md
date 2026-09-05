# Explicit reflection texture selection

2026-09-05, Windows desktop. Follow-up to the stale slot-5 assets reported in
`20260905_0053_recurring-draw-verification.md` and still unconverted in the
lighting checkpoint.

## Source and first checkpoint

The guest-source guide kept the investigation in exact generated C++ after
reading `config/hooks/render_list.toml`. In `bdSceneNodeDrawSingle`
(`generated/reblue_recomp.40.cpp:9901`), the node initially disables reflection
and binds the current pass default from render-list state +68. Model command
`06xx` selects reflection policy at `loc_82281250`: 0..253 select a current-table
texture, 254 selects the pass default, and 255 disables sampling **without
unbinding** the previous image. A repeated command is wholly elided, including
after a different command changed the slot. Nonzero phases force the command
value to zero; the new decoder is explicitly phase-0 only.

The native material decoder now records selection and enable independently.
Ordinary `65xx` material texture overrides mark the selection unknown until a
non-elided explicit reflection selection replaces it. They are not silently
classified as pass-default bindings. Records have no guest addresses, but their
table indices remain import recipes, not persistent native texture identities.
The existing `.bdmat` property format is unchanged.

Standalone tests cover initial default, explicit table indices including 0/253,
default selection, disable without unbind, repeated-command elision, unknown
slot-5 overrides, unrelated slots, independent model resets and transactional
truncation. The existing material test target builds and its CTest passes
(0.04 seconds). This first checkpoint does not yet change runtime replay.

The colour-mask trace reached material callback dispatch in `sub_8221DB00`
and `sub_8221D548`. The direct draw body itself only sets alpha reference, but
that does not prove the callbacks cannot change colour write. No blanket
live-mask override was made; this remains a separate conversion boundary.

## Runtime integration

The import adapter follows `bdLookupCurrentTableTexture` exactly for table
selection: missing table returns null, table offset plus model index is compared
as a 32-bit unsigned value, in-range records use their texture at +24 (28-byte
stride), and out-of-range selection uses the table fallback. Checked address
overflow/unreadable input is refused, not confused with an explicit null texture.
Standalone tests cover those cases and independently changing pass inputs.

Supported direct phase-0 draws now retain the selection recipe, not slot 5's
captured native handle or pooled resource pointer. Before replay, the host
resolves all selected bindings against the current pass/table inputs and
preflights the entire node before any draw. Native handles feed the existing
sampler/descriptor boundary directly. A live dynamic-resource adapter and
refused null selections are distinguished and counted. Reflection enable is composed
from its recipe rather than a sibling visual's last bool state.

Captured draws independently compare the decoded enable and selected binding
against actual interpretation. A mismatch refuses capture. Verification and
compatibility visual history consume the actual capture before slot 5 is
discarded from retained templates. Recipe changes prevent template merging;
the material/merge censuses include the new distinction. The final integration
also releases an earlier node's temporary native handle when that preflight
slot no longer uses the reflection recipe.

`bd_native_reflection_inputs` defaults on and requires native texture bindings.
Engine table/pass association production, discovery through model geometry,
null-selection inheritance, ordinary slot-5 material/animation overrides,
scene-target material callbacks, technique 11, nonzero phases and
deferred-entry recipes remain explicit conversion boundaries. Table selection
is not yet a persistent native scene/material association. This component
does not replace full reflection pass construction or the guest draw producer.

## First diagnostics: a null-handling error and a separate binding mismatch

`reblue_696.log`, PID 24764, 01:50:17-01:57:39 EDT, six settings audited:
autoplay/perf on, capture delay 270 seconds/minimum 30 draws/120 frames,
`bd_host_draw_verify_every=31`. The first integration built and linked without
guest-object compilation. All 13 existing texture/upload/state tests passed
(0.63 seconds), as did the extended material test (0.04 seconds).

The short field initially matched, but at 01:53:10 some disabled, pass-default
bindings differed. Last reflection report: 513113 checked, 6386 mismatches,
1421213 composed inputs, all native handles, no lookup refusals. The source
trace found `Video::SetTexture` in `src/gpu/draw_bindings.cpp:35`: a null
selection is a **no-op**, preserving the previous non-null image. A final
selector alone cannot reconstruct earlier commands if that selector resolves
to null. The initial bridge had incorrectly treated null as an unbind.

The adapter now refuses null-selection recipes before replay effects,
and counts them separately. It does not approximate their inherited texture
with a sibling draw or silently change the existing adapter's semantics.
Converting their complete binding sequence remains work to do. The owned
diagnostic process was stopped for this confirmed mismatch; no capture files
had been produced, so this run is not visual qualification. The final rebuild
compiled only the host draw object and linked; codegen again wrote/deleted
nothing and no guest objects rebuilt. It also includes the compatibility
visual-history handoff and source-kind coverage counters described above.

The repeat test (`reblue_697.log`, PID 23548, 01:58:33-02:04:04 EDT) still found binding mismatches,
with **zero null selections**. Null-as-no-op was a real independent adapter
error, but was **not** the cause of the observed source mismatch. The first
diagnosis did not establish that causal link. Exact expected/actual identity
and callback-state tracing is needed; these failed checks are not qualification.
Its last report was 510715 checked / 4328 wrong, 1425138 composed native
bindings, with no refusals/null selections; no captures had been produced.

## Next source boundary

The decisive identity trace is `reblue_698.log`, PID 21980,
02:04:44-02:08:35 EDT, the same six settings except capture delay 180 seconds.
At 02:07:39, technique-6 mesh `27C0568C`, range 0/30, expected native asset
`4E68548CBCA3BB75` but actually bound `C7FA987FEB91D6BE`. The pass default
was unchanged before/after (`23ADD8E0`), and enable was false on both sides.
Slot mask `0421` includes slots 0, 5 and 10.

The exact extra writer is `sub_8221E618` (generated file 91), which binds
`bdGetCurrentRenderTarget()` to slot 5 and `bdGetNextRenderTarget()` to slot 10.
Its direct caller is material begin callback `sub_82454C08` (file 99).
`sub_8221DB00` (file 43) invokes the visual's virtual +32 callback after model commands.
The importer now explicitly excludes both callback entry points; they need their own native
scene-target recipe and sub-draw scheduling. Excluding all technique-6/7/8
objects, or pretending that their model commands own the final slot, was not
used as a substitute for identifying the writer.

That diagnostic produced 120 isolated 1920x1080 captures in
`out/verification/native_reflection_transition_diagnostic`, frames 10002-10121,
02:07:47.095-02:07:50.935 (log write times). Sequence analysis: 109/119 changes
over 6%; no cyan patches, median 0%, maximum 1.11%. Inspected pairs 1/2 and
43/44: character and surrounding terrain are visible; the moving camera means
the jump count alone is not a flicker diagnosis. This failed-input diagnostic
is not normal-path or later-scene qualification. Last reflection report:
510609 checked, 4269 wrong, 1440028 composed native bindings, no null selections.
The process was stopped only after all 120 captures were present.

The first guard checked only the direct wrapper (`82454C08`). Another run,
`reblue_699.log`, PID 24052, 02:12:10-02:16:43 EDT, exposed the actual virtual
entry **8221E618** on those technique-6 visuals. Last report: 510393 checked,
5079 wrong, no unsupported/refusals, 1434333 composed bindings. Its 120 captures
are isolated in `out/verification/native_reflection_callback_diagnostic`;
this failed-input run is not qualification. The guard now tests both the helper
and wrapper, with standalone regressions for both and an absent callback.
The host renderer and extended material tests build/pass after that correction.

## Corrected transition comparison

`reblue_700.log`, PID 21300, 02:17:16-02:21:22 EDT, all six settings audited:
autoplay/perf on, capture delay 180 seconds/minimum 30 draws/120 frames,
replay sampling every 31 candidates. **490655 source checks, zero mismatches**;
179 unsupported callback-driven draws, zero lookup/null refusals; 1373043
composed bindings, all native handles. All observed supported selections are
pass-default with reflection disabled. Table-selected, enabled and dynamic
reflection paths have not gained GPU qualification from these counts.

Full replay verification still reports other differences: 112701 nodes /
154060 draws, 56287 flagged draws, declared VS/PS 311/1892, geometry 130,
pipeline state 0 and draw-count nodes 0. No logged slot-5 difference remains
in this sampled run. That does not establish that every texture or shader
recipe is native/correct. No error/critical, VK_ERROR, overflow or exhaustion
matches were found. Lighting has 492606 direct source checks with zero wrong;
the native consumer has no fallback/refusals in its final report.

The isolated `out/verification/native_reflection_transition_verify` contains
120 1920x1080 captures, frames 10021-10140, 02:20:19.396-02:20:23.195.
Sequence analysis: 114/119 changes over 6%. No cyan patches (median 0.003%,
max 1.23%). Inspected pairs 1/2 and 43/44:
characters, terrain and the title graphic are readable during the camera move.
This is a sampled comparison, not normal-path or full-game qualification.

The final integration additionally validates the **current visual's** callback
before replay, not only the visual that supplied a shared mesh template. The
host-only rebuild compiled material/draw objects and linked without rebuilding
guest objects. Normal-path late-scene and desktop VR checks follow separately.
The normal late run subsequently exposed a capture-time registry/video lock
inversion, despite zero source mismatches; see
`20260905_0235_reflection-validation-lock-order.md` for the thread evidence and
correction. The earlier source comparison did not establish loading safety.

### Colour-write follow-up

A bounded search for literal `li r3,212` followed by render-state calls finds
13 generated functions, including `sub_82186BA0` (file 53),
`sub_82188300` (34), `sub_821885A0` (100), `bdCameraRender` (8) and
their shadow/resolve companions. In `sub_82186BA0`, the colour mask is 7 or 15
according to the current slot at `(uint32_t(-32035) << 16) - 26424`;
`sub_82188300` and `sub_821885A0` explicitly select 15. This is a concrete
pass-producer lead for the remaining stale colour mask. The literal search
alone does not prove that no indirect or dynamically indexed writer exists,
and no colour-mask runtime change is part of this checkpoint.
