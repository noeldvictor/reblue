# Native sun camera math; integration not yet qualified

2026-09-05, Windows desktop. This checkpoint publishes only the dependency-free
camera builder and its test target. It does not enable the renderer integration.

`native_sun_camera.h` builds a current-view directional light camera using
double-precision inverse/unprojection, finite or infinite far-plane rays,
bounded receiver reach, a spherical coverage fit, a texel-snapped light-space
centre and an orthographic depth volume with caster padding. It publishes
native view/projection matrices and six outward clip planes. No rendering
engine functions, addresses, shader-register files or graphics SDK are inputs.

Tests cover right/left-handed, infinite-far, off-centre and orthographic scene
projections; translated cameras; authored sun rotation including the pole;
receiver coverage and plane inclusion; sub-texel stabilization; and invalid
inputs. Float output precision is checked against its summed-term rounding
bound, not an unrealistic double-precision absolute threshold. The initial
test exposed that threshold error. The Windows host build also caught `near`
and `far` macro collisions; local names now avoid them.

All 23 CTests pass (22 native texture/state/camera tests plus one material
test). The new camera executable is also tested independently. This is math
evidence, not independent GPU/caster or original-camera equivalence evidence.

## Integration work deliberately excluded from this checkpoint

The guest-source skill directed inspection of the translated sun fitter
`sub_821752E8`, snapshot `sub_82283068`, their math helpers and the render-tweak
hook configuration. The in-progress integration replaces automatic fitting
and snapshot execution; temporary authored-input/getter adapters remain.
The devloop skill kept builds host-only and testing on desktop.

The initial uncommitted integration binary (08:20:01, 47,381,504 bytes,
`11e3e97a1` dirty) ran as PID 21980, log 736, 08:20:23-08:22:13.995.
Its 120-frame flat sequence in `out/verification/native_sun_flat` has no large
frame jumps or cyan patches. It records 5401 native fits/snapshots, zero
refusals and zero original snapshot/fitting calls; attachment checks match.
However, full-resolution pixel inspection against the preceding lifecycle
checkpoint shows Shu's cast silhouette missing. Stable counters and frames
therefore do **not** qualify this integration.

A diagnostic rebuild (08:34:32, 47,385,600 bytes) ran as PID 23812, log 737,
08:34:56-08:37:34. All six profile settings audited: the original five plus
`bd_shadow_fit_diag=true`. It reports pitch/yaw (-0.87266, 0.87266), half extent
346.839, world texel 0.16935, depth range 1693.677 and target clip coordinates
(0.34923, 0.12439, 0.50839). Lighting bias is 0.00025. These values alone do
not isolate the regression. The native light-eye write into the authored
position getter is another suspect because the original automatic fitter
does not update it. Investigation continues; no GPU equivalence, full desktop
acceptance or Quest result is claimed.

The first restricted standalone-test Ninja process stalled; only its verified
owned PID 21620 was stopped, and the existing tree then built and tested
successfully with approval. No build tree, profile, game data or captures were
deleted. A mistyped reflection-test filename did not run that guard; its actual
test path must be checked separately.
