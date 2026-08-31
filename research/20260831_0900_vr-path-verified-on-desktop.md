# The VR path runs on the desktop, and diorama mode composes

2026-08-31. No headset, no device. `tools/xrsim/` - this project's own headless
OpenXR runtime - plus the `REBLUE_OPENXR=ON` desktop build.

## What is verified

```
OpenXR: instance up, per-eye 1024x1024, 0 instance / 0 device extensions required
OpenXR: 13 input actions attached
[xr] cam: game (-0.0, -0.0, -0.0)  head m (-0.032, 1.600, 0.000)  eye (-3.2, 960.0, 0.0)
```

**`eye` differs from `game`**, which is the check that matters: it means
`ViewOverrideActive()` is true and the camera composition is actually driving
the view rather than being computed and discarded. With `bd_vr_camera_mode = 2`
that is **diorama mode composing**, on the desktop, reproducibly.

The head is at the simulator's default 1.6 m and the composed eye sits at
y = 960 game units - 9.6 m at the measured 100 units/metre - which is the
diorama's camera height above the scene.

## The trap: `bd_vr_enabled` is a separate switch

A run with the runtime correctly attached, `XR_RUNTIME_JSON` set, and the
OpenXR build in place still logged **nothing** about XR and rendered flat. The
only evidence was one line:

```
bd_vr_enabled is off; flat renderer
```

Setting `XR_RUNTIME_JSON` and building with `REBLUE_OPENXR=ON` is not enough -
`bd_vr_enabled = true` has to be in the profile TOML as well. Without it the
whole VR path is compiled in, the runtime is present, and none of it runs. The
`vrsim` skill's checklist does not mention it; it should.

## What is NOT verified

**No picture of diorama mode.** Two runs at 140 s and 205 s both captured
nothing: `bd_capture_min_draws = 300` never triggered because the run was still
in a menu (`26 draws/frame attributed of 55 counted`). Autoplay is slower under
XR - stereo plus the XR frame loop - and does not reach a field scene on the
schedule the flat build uses. So the *geometry* of diorama mode is confirmed to
compose and its *appearance* is not.

The fix for next time is a longer `bd_capture_after_s` under XR, or gating the
capture on draw count alone with a much later deadline. Do not read a capture
that did not fire as a failure of the camera mode.

## Where this leaves VR

| piece | state |
| --- | --- |
| OpenXR session, swapchains, input actions | works, desktop, no headset |
| camera modes composing (`eye != game`) | **verified**, diorama |
| side-by-side stereo through present | **works**, `far +4, near -7` |
| multiview stereo through present | **black** - see the 0700 note |
| diorama framing and tuning | **unverified** - never photographed |
