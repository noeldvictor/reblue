# Multiview stereo runs on a Quest 2, with real depth

2026-08-30. The headset was attached for the first time this session, and everything built and
verified on the desktop today went to it unchanged.

## It works

`bd_stereo_multiview=true`, autoplay into a field scene, composited frame captured at 150s:

```
far +57, near -80  ->  near - far = -137 px, spread 137 px
OK: crossed disparity, near separating more than far.
```

Monotone across all eight bands. The capture is the compositor's own 3664x1920 panel image with
both eyes in it - a real Blue Dragon field scene, the party leader on a rock, desert, shadows,
foliage - so this is what a person wearing it sees, not a proxy.

The exclusivity fix fires on device exactly as designed:

```
[mv] bd_stereo and bd_stereo_multiview are both set; they are alternatives, not layers.
     Multiview wins and the side-by-side eye loop is suppressed.
[mv] MULTIVIEW pipeline created, viewMask=3
[mv] resolved 344x180 to side-by-side (501 times)
```

**One thing to tune, not a bug**: far sits at +57 rather than near 0, a uniform offset between the
two eyes. That is convergence - `bd_stereo_convergence` defaults to 0, which puts the convergence
plane at infinity - and it is a comfort setting, not a depth error. The depth itself is correct and
correctly signed.

## The Thor's blocker is not the Quest's

**6 pipeline failures on the Quest against 21,615 on the AYN Thor.** Six is the known sun-occlusion
descriptor set that Android drops deliberately. So the `shaderInt64=0` finding is specific to the
Adreno 740's newer driver, and the caution recorded in CLAUDE.md - *a Thor failure is not
automatically a Quest failure* - was right. The Adreno 650 compiles the `Int64` declaration and
renders.

## What it costs

```
dt_ms 100.18 | other_ms 99.62 | gpu_total_ms 1.69 | draws 420
```

**10 fps, and 99.6 of the 100ms is CPU.** The GPU does 1.7ms. That is the same shape every desktop
measurement has shown all day and the same conclusion the port reached in August: the cost is the
recompiled guest, not the renderer and not VR. Multiview halves the draw *submissions*, which is
real, and it does not touch the guest simulation that dominates the frame.

So the honest headline is two sentences, not one: **Blue Dragon renders in correct stereo VR on a
Quest 2, and it runs at 10 fps.** Depth is solved. Speed is not.

## Next, in order

1. **`bd_stereo_convergence`** - one cvar, no rebuild, fixes the uniform eye offset above.
2. **The guest CPU**, which is 99.6% of the frame. `bd_sample_profiler` wrote an empty
   `guest_profile.txt` on this run and that needs chasing first, because it is the only instrument
   that names where the time goes on ARM64.
3. The constant-heap rewrite stays worth doing for the Adreno 740 and for GPU cost, but it is
   explicitly *not* what makes this port slow.
