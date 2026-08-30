# The AYN Thor runs now; the flat renderer is black on the desktop and UNCONFIRMED on the Thor

**CORRECTION, appended below.** The Thor half of the "flat path is black" claim does not hold up -
its screenshot was taken on a dual-screen device where another app held focus. Read the correction
before the table.

2026-08-30.

## The Thor is unblocked

The Adreno 740 could not compile a single shader: every module declared
`OpCapability Int64` because guest constants were read through `vk::RawBufferLoad` at a 64-bit
device address, and the device reports `shaderInt64 = 0`. The driver said only
`Shader compilation failed for shaderType: 0` and validation had nothing to flag.

Removing that capability - the constant rewrite - fixed it:

| | pipeline failures | shader compilation failures |
| --- | --- | --- |
| first ARM64 run | 21,615 | many |
| after the vertex-format fix | 5,449 | many |
| **now** | **245** | **0** |

And it runs, fast, on `Vulkan 1.3.128 on Adreno (TM) 740`:

```
2962 of 3873 frames are field scenes
dt_ms 33.75 | gpu_total_ms 21.50 | draws 833 | us/draw 34.19
```

**29.6 fps**, against the Quest 2's 15.0 - which is what a newer, much larger GPU should look like.

## But nothing is on the screen, and that is a different bug

`adb exec-out screencap` - an instrument entirely outside this repo's capture path - returns a
frame that is **0.0% non-black**, while the app is alive, recording 833 draws a frame and spending
21.5ms on the GPU.

The log settles what path it is on: `OpenXR: no usable runtime (-51), staying on the flat
renderer`.

## The pattern across three targets

| target | present path | renders |
| --- | --- | --- |
| Quest 2 | OpenXR, XR swapchain | **yes**, verified from a two-layer capture |
| AYN Thor | flat swapchain | **no**, black (screencap and in-app capture agree) |
| Desktop Vulkan | flat swapchain | **no**, black (window capture and in-app capture agree) |

**It is the flat present path, not the desktop.** Two very different GPUs - an Adreno 740 and an
NVIDIA desktop part - and three independent instruments agree. This reframes
`research/20260830_1630_the-desktop-3d-was-already-broken.md`, which correctly established the
fault predates the constant rewrite but wrongly scoped it to "the desktop".

It also explains why nobody noticed: the port is VR-first, the Quest path works, and the flat
capture that would have shown this was itself broken (plume created swapchain images without
`TRANSFER_SRC`, so every readback was zero).

## Why this matters more than it looks

The flat path is not a side feature:

- It is the only way to run on the Thor, which is a stated target of this fork.
- It is the desktop correctness loop, which is 90 seconds against the device's four minutes.
- `RecordPresentPass` is the code Track B1 replaces when the scene starts rendering directly into
  the XR swapchain, so it has to be understood before that lands.

## Where to start

`RecordPresentPass` in `src/gpu/present.cpp` draws the guest's final surface into `back` through
the gamma/cel blit, then the overlay draws on top. On the Quest, `RecordXrQuad` copies `back` into
the runtime's image and the result is correct - **so `back` is right on the VR path**. On the flat
path the same `back` is presented and comes out black.

That narrows it to what differs between "copy `back` into an XR image" and "present `back`": the
swapchain acquire/present pairing, the image index, and the layout transitions around it. Note
`have_rt_blit` skips the blit entirely for an MSAA render target and only logs at `BD_ERROR` five
times - worth checking that it is not silently skipping.


---

## Correction: the Thor's black screen is not established

`adb exec-out screencap` returned 0.0% non-black, and that was written up above as a second
independent confirmation that the flat present path renders nothing. It is not one.

`dumpsys window` says:

```
mCurrentFocus=Window{... com.odin.dualscreen.assistant}
mFocusedApp=ActivityRecord{... com.android.launcher3/.secondarydisplay.SecondaryDisplayLauncher}
```

The AYN Thor is a **dual-screen** device with two HWC displays, and reblue - on `mDisplayId=0`,
alive, window present - never takes focus. `am start` does not bring it forward, and
`screencap -d 0` is rejected because the displays have 64-bit IDs rather than small indices. So the
capture is of a display state that another app is driving, and a black result cannot be attributed
to reblue's renderer.

Pure black is *odd* for a launcher overlay, so this is not evidence the Thor renders fine either.
It is simply not evidence in either direction.

**What survives, and is solid** - it comes from the log rather than a screenshot:

- `Int64` is gone, shader compilation failures are **0** (they were the blocker), and pipeline
  failures went 21,615 -> 5,449 -> **245**.
- The Thor runs a field scene at `dt 33.75ms | gpu_total 21.50ms | draws 833` - **29.6 fps**.
- `OpenXR: no usable runtime (-51), staying on the flat renderer`, so that frame rate is the flat
  path doing real work.

**What is now single-source**: the flat path rendering black is established on the **desktop** only,
where the instrument photographs reblue's own window by process handle and the in-app capture
agrees. The claim "two very different GPUs agree" was overstated and is withdrawn.

**To settle the Thor**, get a capture that cannot be confused by another display: run with
`bd_capture_after_s` and `bd_capture_min_draws` and read the in-app capture, which photographs the
back buffer directly and does not care what the compositor is doing. That run was started and the
Quest disconnection ended the session before it completed.


---

## Second correction: the Thor RENDERS. Confirmed from its scene target.

`bd_mv_capture_array` with multiview off could not fire at all - the array branch needs
`layers > 1`, and the in-pass composited capture stands aside whenever a guest surface was asked
for - so the run produced no capture while looking healthy. Fixed: a single-layer scene target is
now captured directly.

With that, the Thor's scene target reads **`RGBA16F 1280x720, mean 128.5, 100% non-black`**, and the
image is unmistakably a Blue Dragon field scene - sky, terrain, structures, correct colours.

**So the Adreno 740 renders the game.** Taken with the frame numbers, the picture on the Thor is:

```
Int64 gone -> shader compilation failures 0, pipeline failures 21,615 -> 245
dt_ms 33.75 | gpu_total_ms 21.50 | draws 833   = 29.6 fps
scene target: correct
```

That closes the blocker recorded in
`research/20260830_0820_arm64-the-thor-renders-nothing.md`. The Thor rendered nothing; it now
renders a field scene at twice the Quest's frame rate.

**Known artefact, not a rendering bug:** the captured image is sheared into vertical bands. The
content is correct, so this is a row-stride mismatch in the readback for this width/format
combination - the same path is clean for the Quest's 1920-wide two-layer RGBA16F grab. Worth fixing
before the capture is used to judge fine detail, but it does not affect "does it render".

**What remains genuinely open:** whether the flat path reaches the *display*. The scene target is
correct and `screencap` reads black, but the Thor is dual-screen with another app holding focus, so
that reading is not attributable. The desktop's flat-path black stands on its own evidence.
