# Multiview after the per-draw cut: the host post chain goes layered

2026-09-02, 23:50. Desktop (flat window, `bd_msaa=0`, `bd_stereo_multiview=true`,
`bd_mv_layered_textures=true`) and one Quest run of the morning build.

## Where multiview stood at 22:40

Quest, `verify_quest.sh "bd_stereo_multiview=true,bd_mv_layered_textures=true"`, the
build with direct present and the seed skips: **23.5 ms GPU, 24.3 ms CPU, 379 draws**, two
60 Hz slots - against 59 ms the previous evening. The resolve chain (`bd_mv_resolve`, Android
default on) was still in the frame, the host post chain did not run (it refused two-layer
targets), so the guest's fifteen post quads ran too, and the capture came back black (the
resolved-companion capture site; the panel was not looked at).

## The desktop multiview frame, and three bugs in it

1. **Crash in `HostPostIntercept`**: the new "composite reads the scene the dof draw saw"
   code compared a null scene against a null `scene_src` and dereferenced it. Guarded.
2. **Washed out**: without the host chain the guest's scaled resolve into a two-layer
   texture never delivered the x0.25 exposure to its composite. Fixed by running the host
   chain on layered targets: scratch textures and framebuffers with `viewMask 3`, a layered
   pipeline variant per post shader, the five post pixel shaders at `ps_6_1` picking the
   source layer by `SV_ViewID`, `Readable`/`BeginGuestTarget`/`HostComposite` accepting two
   layers. The pair then has the flat path's exposure, depth of field and bloom.
3. **Sun shadow in the left eye only**: the pipeline state's multiview flag followed the
   colour target; the shadow pass has none, so its pipelines were mono inside the two-layer
   depth framebuffer (viewMask 3) and layer 1 of the map was never drawn. The flag now
   follows the depth target for a depth-only pass. Both eyes carry the shadow.

Screenshots: `out/shot_layered_mv3.png` (pair, correct), `out/shot_layered_flat.png` (flat,
unchanged).

## Next on this path

- Quest run of this build under multiview (in flight as this is written).
- `bd_mv_resolve=false`: the five full-resolution resolve passes exist only for the 277 ms
  mystery of 2026-08-31, which smells like the per-bind descriptor copy fixed on 09-01.
- The present: direct present under multiview means the gamma pass reading both layers into
  the eye images, or a two-layer XR swapchain (`XR_FB` array swapchains).

## Quest, layered post chain (00:15)

`bd_stereo_multiview=true,bd_mv_layered_textures=true`, resolve chain still on: the host
composite runs on the two-layer frame ("composite into 1376x720"), direct present is active,
**GPU 23.0 ms, CPU 24.2 ms, 378 draws** - the same as before the chain went layered, so the
guest's post quads were not where multiview's extra 9 ms over side-by-side sit. The capture
is black again (the resolved-companion site). Next run: `bd_mv_resolve=false` with
`bd_mv_capture_array` for a stacked capture.

## Quest, resolve chain off (00:30)

`bd_mv_resolve=false,bd_mv_capture_array=true`: **GPU 23.0 ms** again, no crash, no 277 ms -
the 2026-08-31 catastrophe was the per-bind descriptor copy, fixed 09-01. The stacked capture
(RGBA16F 1376x1440) has both layers non-black at the same exposure; the best horizontal
alignment between them is 2 px, on a depth-of-field-blurred close-up, so the device stereo
verdict is still open (the desktop pair is unmistakably stereo with the same code).

Where multiview's 9 ms over side-by-side come from: each layer is the full 1376x720, so a
multiview frame shades twice the pixels per eye that the side-by-side frame does (688x720 an
eye). The per-eye size is the next knob, and the target is 1440x1584 an eye either way.

## Later, with the headset offline (01:00-01:40, desktop only)

The Quest dropped off adb after the half-width multiview run started (the script sat in
adb for an hour; killed). Two desktop-only pieces followed:

- **Cel shading on the characters** (`a89cbfe`, XenosRecomp `b03821d`): `SPEC_CONSTANT_CEL`
  makes every recompiled pixel shader band its lit colour before export; the host sets the
  bit on skinned draws under `bd_cel_characters`. The character is banded, the world is not
  (`out/shot_cel.png`). Four bands, no outline, no options-menu switch yet.
- **Host-built render-list entries** (`1da7457`): the interpreter runs that only built
  entries (274 a frame) are replaced by re-emitting recorded entry images through the
  guest's own allocator with a fresh matrix and palette. 564 of 639 node runs a frame are
  the host's now. CPU effect unmeasured until the headset is back.

## Half width, desktop (03:00)

`bd_mv_half_width=true` with the layered host post chain: a correct stereo pair at 960x1080
a layer (`out/shot_mv_half.png`) - exposure, depth of field, bloom and the sun shadow in both
eyes. The 2026-08-31 note that "the present chain does not follow" half width described the
guest's post chain on layered targets; the host chain follows. Android default now; the Quest
run is the parity measurement against side-by-side's 13.2 ms.

## The host-built render list, desktop A/B (03:30)

`bd_perf_csv=true, bd_ab_flag="bd_host_list_build", bd_ab_period=240`, one 300 s run, 9719
field frames: arm 0 (interpreter builds the entries) 6.41 us of CPU per draw, arm 1 (host)
5.84 - **-8.8%**. The desktop CPU is ~3x a Quest core, so the ratio is what transfers.
