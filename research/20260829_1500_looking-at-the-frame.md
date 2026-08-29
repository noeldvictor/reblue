# Looking at the frame, at last - and what it shows about stereo

2026-08-29. The port can now write the composited frame to disk, and the first thing that came back
contradicted a working assumption.

## Why this had to be built

"Verify the pixels, not a proxy" is a rule in `CLAUDE.md` and it was, in VR, impossible to keep.
Everything available was a proxy:

- Quest system-screenshot intents (`systemux://screenshot`, both the broadcast and the activity
  form) **complete and write nothing** on this Horizon build.
- `adb shell screencap` does not see compositor layers.
- So every VR claim rested on a log line, and "swapchain format 37" has been wrong before.

`bd_capture_after_s` writes the finished frame - the same image handed to the XR swapchain - to
`logs/capture/` as raw RGBA with a one-line header, then latches off.

**Seconds, not a bool.** `args.txt` is read once at launch, so a bool could only ever capture the
title screen. Autoplay reaches a field scene on a fixed schedule, so "capture at t=143s" is the
entire interface. 143 is deliberate: the field is up by ~130s and autoplay starts walking at 150s,
so it catches the character standing in a field rather than mid-encounter.

Raw rather than PNG because the tree vendors `stb_image` but not `stb_image_write`, and the host
converts it in three lines. Not worth a dependency.

## plume could not read a texture back at all

The first attempt crashed in `vkCmdCopyImage`. `VulkanCommandList::copyTextureRegion` implemented
buffer-to-texture (upload) and had **no case for the reverse**: a readback fell through to the
texture-to-texture `else` branch, which dereferences `dstTexture` - null when the destination is a
buffer.

Fixed on the plume fork (`noeldvictor/plume`, `main`) with the mirror branch, using
`vkCmdCopyImageToBuffer` and the same block-alignment maths as the upload path. Nothing in reblue
had ever read a rendered image back, which is why a whole direction of the API was missing.

## The first picture

A field scene, `bd_render_scale=25`, 3664x1920, mono: sky, cloud, terrain, a structure, distant
hills. It is **Blue Dragon, in VR, on a Quest 2**, and it is the first time this project has looked
at one of its own VR frames instead of reading about it.

Two things are visible that were only ever theoretical:

- **The projection layer produces an image.** `CLAUDE.md` recorded it as built and never observed
  rendering. It renders.
- **`render_scale=25` is very blurry.** A quarter-scale scene upscaled to a 3664-wide panel is soft
  in a way no frame-time number conveys. The 28.9 fps configuration has a real cost and it is now
  possible to see it and trade against it deliberately.

## Stereo reaches both eyes and carries no depth

Captured with `bd_stereo=true`, the frame is two side-by-side eye views. Both render, neither is
black. That alone was worth having.

But the disparity is flat. Sliding one eye against the other and minimising SAD, per horizontal
band:

| band (y%) | 30 | 45 | 55 | 70 | 85 | 92 |
| --- | --- | --- | --- | --- | --- | --- |
| shift (px) | +59 | +59 | +58 | +58 | +57 | +58 |

**+59 at the sky and +57 at the near ground.** Two pixels of variation across the entire depth range
of the scene, with strong contrast in the near bands so the correlation is trustworthy. Real stereo
here - near ground metres away, hills hundreds of metres - would be tens to hundreds of pixels
apart. This is one image shifted sideways, which is the classic "looks like stereo, has no volume"
result.

The measurement then agreed with the code, which is the reassuring part: `bd_stereo`'s own
description already says it plainly - *"Step one of stereo: no per-eye matrices yet, so both halves
show the same view."* The capture found the documented behaviour rather than a surprise, which is a
decent first test of the method.

**With `bd_stereo_multiview=true` as well, the numbers do not move**: +58, +59, +58, +58, +57, +58.
The log says why:

```
[mv] MULTIVIEW pipeline created, viewMask=3
[mv] 201 mono pipelines so far, 12 multiview
```

The per-eye skew lives in the vertex shader, driven by `SV_ViewID`:

```hlsl
oPos.x += eyeSign * (g_StereoSeparation * oPos.z - g_StereoConvergence * oPos.w);
```

That is correct - `oPos.z`, so the shift scales with depth, which is parallax and not a slide. But
it only runs in a multiview pipeline, and **12 of 213 pipelines are multiview**. The scene geometry
is drawn by the other 201, so the skew never touches it.

**So the next piece of stereo work is not the maths and not the shader. It is why 94% of pipelines
are still mono.** That is a concrete, findable thing, and it replaces "stereo is not started" with a
number.

## What the capture facility is worth

Four minutes, no headset, no wearer: push `args.txt`, launch, pull one file, look at it. It found a
crash in plume, confirmed the projection layer, made the render-scale trade visible, and turned
"stereo has never been visually confirmed" into a table of numbers with a named next step - in one
session.

Every VR claim from here can be checked this way, and should be.
