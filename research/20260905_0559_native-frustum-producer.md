# Native frustum construction and current-view culling ownership

Date: 2026-09-05, desktop EDT. This converts the six-plane producer and its
default-view host-walk consumer, not whole scene/pass or frame ownership.

## Source and correction

The exact translated bodies, not a decompiler reconstruction, establish:

- `sub_821CCF48` (`generated/reblue_recomp.26.cpp`) takes a 13-float shape:
  origin xyz, quaternion xyzw, right/left/top/bottom slopes, near and far.
  It constructs near/far/right/left/top/bottom planes, rotates each normal
  by `q * (n,0) * conjugate(q)`, subtracts the translated normal's dot with
  the origin from d, and normalizes the four coefficients.
- Its six calls to `sub_821CCA50` (`reblue_recomp.47.cpp`) use vector
  arguments, not a matrix pointer. Quaternions are not normalized first.
  Normalization uses reciprocal square root plus one non-contracted Newton
  step. Zero squared normal length selects an all-zero plane.
- `sub_82186840` (`reblue_recomp.24.cpp`) is a per-view **frustum** cache and
  producer. `sub_822873E0` derives shape slopes/distances from projection;
  `sub_82287478` derives origin/orientation from view data. Scene begin
  (`sub_82186BA0`, `reblue_recomp.53.cpp`) may replace the shape with a
  second cached view before rebuilding the planes. Earlier active work
  described this cluster as fog/lighting; the source corrects that label.
- Three call sites in the cache/scene-begin bodies publish canonical scene
  planes; `reblue_recomp.16.cpp` also constructs caller-local planes at
  stack +832..+912 from a shape at +1008. That noncanonical call must not
  replace the native default-view volume.
- `sub_82287788` and the adjacent visibility consumer in
  `reblue_recomp.48.cpp` read the canonical six-plane table. The existing
  host walk previously reimported its 96 bytes on each default-view walk.
  Other render views have separate tables and still use visibility adapters.

Generated source and hook TOML were not edited. The generic vector helper
is not globally replaced for unrelated gameplay/math callers.

## Implementation and boundary

`native_frustum.h` is SDK-independent arithmetic and a current-frame native
volume holder. It retains pairwise float arithmetic, non-unit quaternion
scaling, zero normals and IEEE exceptional transitional inputs. Native
camera asset validation remains an ingestion responsibility.

`host_frustum_bridge.cpp` replaces the complete plane-builder body. Supported
normal execution uses no original builder or its six vector-helper calls.
Shape inputs and engine getter publications are still temporary adapters.
The bridge preserves input r3, snapshots before stores, rounds output
addresses as STVX does, and supports repeated output destinations with
ordered last-store semantics. Invalid memory, unusual aliases of callee
scratch/argument spills and changed engine constants refuse before effects.

Canonical publication owns the current frame's native scene volume on the
submitting thread. Noncanonical destinations cannot select it by matching
values. Any output overlapping the canonical plane region invalidates its
previous native volume before publication; a complete canonical publication
replaces it. A frame without publication cannot reuse the previous frame's
volume. The host walk reads this native volume, not engine planes. Missing
producers and other-view visibility adapters remain explicit dependencies.

`bd_host_frustum=true` is the default. With `bd_host_frustum_verify=true`,
native results are predicted and the original executes exactly once; only
the original writes engine outputs. All 24 coefficients and return r3 are
compared (1e-5 relative/absolute tolerance, matching NaNs accepted). Each
native walk additionally compares the current engine getter shadow to catch
an intervening writer. This diagnostic import is absent from normal culling.
`bd_host_cull_diag` separately compares per-node visibility decisions.
Stack scratch/caller-clobbered registers and NaN payload bits are not compared.

## Build and standalone verification

Final desktop Vulkan executable: `out/build/win-amd64-release/reblue_vk.exe`,
47,300,096 bytes, 05:59:12 EDT. The existing `reblue` target built successfully
with four jobs. Codegen reported the module up to date; no guest translation
unit rebuilt and no dependency changed. Log version is `39f46df2e` plus local
changes (the earlier configure timestamp is retained by incremental builds).

All 19 texture/state/upload/scene CTests, the material CTest, and three
reflection/source lock-boundary guards pass. New standalone coverage includes
all six faces, inside/outside/tangent spheres, translated/rotated volumes,
non-unit and zero quaternions, asymmetric/signed slopes and distances,
10,000 varied inputs against an independent double rotation-matrix oracle,
infinity/NaN arithmetic and current-frame invalidation including frame wrap.
Engine memory alias handling has source review, not dedicated live alias
injection coverage.

## Desktop runs

Exploratory PID 20852, 05:57:26-05:59:10, `reblue_718.log`, used the earlier
05:56:46 binary (47,298,048 bytes). It matched ordinary plane publications
and culling decisions but retained 13 early compatibility calls under its
finite-input/output restriction. That restriction was removed: exceptional
inputs now follow native arithmetic too, with explicit counting and matching
NaNs in the comparison. Its captures are not final-build qualification.

Final-build comparison PID 24724 started 05:59:33, `reblue_719.log`. All eight
settings audited: the preserved autoplay/perf/60-second/600-minimum-draw/
120-frame capture settings, plus host frustum, original comparison and
culling diagnostics enabled. Field comparisons include exceptional startup
inputs, not just finite camera data.

This run stopped at 06:02:16. Last counters: 33,939 matching publications,
25,239 canonical scene publications, 869,630 native walks/getter-shadow
checks, 12 exceptional inputs, zero compatibility/refused/missing/wrong.
All logged culling diagnostics report zero disagreements. Mounted 1673
archives / 119346 names; RTX 3060 Vulkan 1.4.341, MSAA 4. No error/critical,
VK_ERROR or upload-exhaustion messages.

`out/verification/native_frustum_compare`: 120 1920x1080 captures,
`frame_1788602435_0.raw` through `frame_1788602439_119.raw`,
06:00:35.789-06:00:39.078. Sequence analysis reports 0/119 jumps over 6%;
cyan analysis reports zero threshold/patch frames, median 0.011%, max 0.02%.
Actual first/last previews show Shu in the village and moving waterwheel
shadows without a broad missing band or cyan patch. `--mono` only decoded
flat pixels; it is not a replay-disabled control. This comparison run does
not qualify the normal comparison-off path, later scenes or final eyes.

## Remaining work

Native view/projection sources and per-view cache, whole scene begin and
target lifetime, other-view culling tables, native camera/scene associations,
and engine getter removal remain. This is not a native camera registry or
full frame scheduler. The known late-scene rock-wall/text failure and
blurred/letterboxed, depth-inconclusive VR remain open until separately
qualified. Full desktop fields/battles/cutscenes/menus/transitions/reloads/
both-eyes acceptance remains required before Quest 2 work.
