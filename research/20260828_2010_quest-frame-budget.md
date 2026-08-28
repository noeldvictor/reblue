# Research: finding the 150ms, and where the Quest 2 frame budget actually goes

Date: 2026-08-28 20:10
Topic: 6.7 fps to 31 fps, what the bottleneck really was, and what is worth optimising next.

The VR build ran at 6.7 fps and looked "flickery and choppy". Both symptoms had the same cause, and
it was not the one any of the obvious readings suggested. This note records the measurements,
because the conclusions are counter-intuitive enough that they will otherwise get re-guessed.

---

## 1. Instrument first

Two symptoms that look identical inside a headset:

- a **dropped layer** — some frames reach `xrEndFrame` with nothing to composite, so the screen
  blanks and it reads as flicker;
- a **slow frame** — the content updates rarely, and the compositor reprojects a stale quad.

They have nothing to do with each other and completely different fixes, so the first change was a
per-frame accounting line rather than a fix:

```
[xr] 6.7 fps | 34 begun, 34 layered, 0 no-render, 0 no-image
     | frame 149.8ms = xrWait 8.2 + acquire 0.0 + submit 0.1
     + present 124.2 + fence 0.1 + drain 2.5 + elsewhere 14.5
```

That one line killed three hypotheses immediately:

- **0 no-image, 0 no-render, 34 of 34 frames carrying a layer.** Nothing was ever dropped, so the
  flicker was not a missing quad. It was just content arriving 6 times a second.
- **xrWait 8.2ms.** The compositor was not throttling us.
- **fence 0.1ms.** The GPU was finishing instantly. It looked entirely CPU-bound.

The last one was a lie, and believing it would have cost hours. See §3.

## 2. The Android surface costs 124ms a frame

`present` was 124ms of a 150ms frame. 1/0.124 is almost exactly **8Hz**.

On a Quest, an OpenXR application's own `ANativeWindow` surface is not what the user sees — the
compositor draws submitted layers instead. The surface still exists and still has a consumer, but it
is serviced at something like 8Hz, and a FIFO present against an 8Hz consumer paces the entire
engine at 8Hz. Everything upstream — guest simulation included — was being throttled by a surface
nobody was looking at.

`setVsyncEnabled(false)` does not fix it. In plume that only takes effect on the next resize, and
even in IMMEDIATE mode the buffer queue is the constraint.

**The fix is to stop using the surface.** In VR the swapchain is never acquired, never rendered to
and never presented; the present pass targets a texture the renderer owns, and the only consumer is
the copy into the runtime's swapchain image. Two details that bite:

- Nothing was acquired, so the acquire semaphore never signals. Submitting a wait on it deadlocks
  on the first frame. Wait and signal counts drop to zero in VR.
- The offscreen target must be restored to `COLOR_WRITE`, not `PRESENT`. It is not a swapchain
  image, and next frame's render pass would find it in an undefined layout.

This is also the shape stereo needs, since that wants one target per eye and a swapchain has no
concept of eyes.

## 3. The fence was lying, and the real cost was resolution

Removing the present moved the 124ms wholesale onto `fence`. Nothing got faster.

That is the trap worth remembering: **`present` was blocking first, and the GPU finished during the
block**, so by the time the fence was waited on it had already signalled. A near-zero fence means
"the GPU was not the last thing we waited for", *not* "the GPU is idle". Removing the earlier block
is what exposed the real 125ms of GPU work.

And that work was resolution:

| Render size | fps | fence |
| --- | --- | --- |
| 3664x1920 | 6.9 | 119ms |
| 1600x900 | 24.0 | 0.1ms |
| 1280x720 | 26.2 | 1.5ms |
| 640x360 | 25.6 | 0.2ms |

The headset panel is 3664x1920 across both eyes and the renderer sizes the scene to the window, so a
Quest 2 was drawing a **1280x720 game at seven megapixels**. Blue Dragon is natively 1280x720/30fps.
Capping at 720 is the game's own resolution, not a compromise — and the result is resampled onto a
quad the compositor draws at arm's length regardless.

Below 720p nothing improves, because the GPU has stopped being the constraint.

## 4. What the settings are actually worth

Measured one at a time, on the title screen:

| Change | Effect |
| --- | --- |
| 3664x1920 -> 1280x720 | **6.9 -> 26.2 fps** |
| shadows 4096 -> 1024 | 26.2 -> 31.5 fps |
| MSAA 4x -> off | nothing measurable at 720p |

So the Android defaults are: `bd_max_render_height` 720, `bd_shadow_dimension` 1024, and **MSAA
stays at 4x** — at 720p the GPU finishes in 1.0ms and there is nothing to buy by turning it off.
Turning MSAA off first would have been the intuitive move and would have achieved nothing.

Result: **30-31 fps with no overrides**, which is the game's native frame rate.

## 5. Where the remaining budget goes, and what NOT to optimise

```
frame 32.3ms = xrWait 13.8 + submit 0.1 + present 0.0
             + fence 1.0 + drain 0.3 + elsewhere 17.1
```

**The GPU is 97% idle.** The remaining cost is `elsewhere` — 17ms of guest simulation and command
recording, on the CPU, in recompiled PowerPC.

This matters because it inverts the usual mobile-VR advice. On this workload, right now:

- **Optimised shaders** save nothing. The GPU finishes in 1ms.
- **Fixed foveated rendering** (`XR_FB_foveation` — Quest 2 has no eye tracking, so fixed only)
  saves nothing, for the same reason. It becomes interesting *after* stereo, when the scene is drawn
  twice.
- **ASTC/BCn asset transcoding** saves VRAM, bandwidth and load time, not frame time. The Adreno 650
  is already sampling these textures happily.
- **An MSAA replacement** saves nothing, because MSAA already costs nothing here.
- **LOD and culling** are the only items on that list that would help, because they cut *draw
  calls*, which is CPU. `bdCameraViewFrustumTest` (0x82135030) is already named and is the seam.

The honest summary: this is a 2007 console game on hardware two generations ahead of the console.
The GPU was never going to be the problem once it stopped being asked to draw at 20x the necessary
resolution. The CPU cost of static recompilation is the real budget.

## 6. 72Hz is a different question from performance

Blue Dragon's logic is a fixed 30Hz step — the gates in `config/hooks/frame_interp.toml` exist
precisely because so much of the engine assumes it. Running the *simulation* at 72Hz is not a
performance problem, it is a correctness one.

Two things make this less bad than it sounds:

- **The compositor already reprojects the quad at 72Hz.** Head motion is smooth today, at 30fps
  content. That is why a quad layer is comfortable and why "choppy" was about content, not tracking.
- This port already has frame interpolation, so presenting above 30 is a solved problem in
  principle.

## 7. A dev-loop trap that invalidated three experiments

`args.txt` beside the game data appends cvars with no rebuild, which is the whole fast loop. Three
consecutive experiments — MSAA, shadows, resolution — all showed "no effect", and the reason was
that none of them had been applied:

```
adb: error: cannot stat '/c/Users/.../args.txt': No such file or directory
```

`MSYS_NO_PATHCONV=1` is needed so the *device* path is not mangled, but it also stops the **local**
path being converted from `/c/...` to `C:/...`. Every push failed. Use a Windows-style local path
and an unmangled remote one:

```sh
MSYS_NO_PATHCONV=1 adb push "C:/path/args.txt" /storage/emulated/0/Android/data/com.reblue/files/args.txt
```

Two independent checks that would have caught it sooner, both now habit: print the push result
rather than discarding it, and confirm the count the app reports — `args.txt added N argument(s)` in
logcat — matches the file.

Also worth knowing: `bd_msaa` validates to 0/2/4/8, so `--bd_msaa 1` is silently rejected. A
measurement that "showed MSAA does not matter" was actually a measurement with MSAA still at 4x.
