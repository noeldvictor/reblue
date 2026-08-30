# The AYN Thor runs now, and the flat renderer is what is black

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
