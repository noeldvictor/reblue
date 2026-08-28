# Research: the Quest 2 blocker is bindless, not the port

Date: 2026-08-28 17:20
Topic: why the runtime crashes on a Quest 2 after Vulkan comes up, and what it means for the port.

The Android port is essentially done — the runtime starts, the VFS mounts the real game data, and
Vulkan initialises on the headset's own GPU. What stops it is not a porting gap. It is an
architectural mismatch between how re:Blue's renderer binds textures and what a mobile Vulkan 1.1
driver offers.

---

## 1. How far it gets

On a Quest 2, from the on-device log:

```
XEX image loaded successfully
created symbols for import library xboxkrnl with 154 imports
rexcrt_heap: initialized with 1 segment(s), capacity=255MB
[vfs] 1274 shipped archive(s), 70008 record name(s) ... (cached index)
GPU caps: Vulkan 1.1.284 on Adreno (TM) 650 | MSAA color=0x7 depth=0x7 usable=0x7
[device] creating swap chain (window 0xb400007ab281e810)
[device] swap chain created
[device] BuildFramebuffers
[device] BuildPresentSemaphores
[device] BuildPipelineLayout
================ reblue host crash ================
```

Everything before that line works: the guest executable loads, kernel imports patch, the guest heap
comes up, the game's own archives mount, and plume builds a real Vulkan swapchain against a valid
`ANativeWindow`. Windowing, surface creation and device selection on Android are not the problem.

## 2. Where it dies

Inside `BuildPipelineLayout` (`src/gpu/device_pipelines.cpp:111`), whose first job is the bindless
texture descriptor set:

```cpp
tex_set_builder.addTexture(0, kBindlessTextureCount);
tex_set_builder.end(true, kBindlessTextureCount);
s.texture_descriptor_set = tex_set_builder.create(s.device.get());
```

And in `src/gpu/bindless_allocator.h`:

```cpp
constexpr u32 kBindlessTextureCount = 65536;
constexpr u32 kBindlessSamplerCount = 1024;
```

**A 65,536-entry bindless texture array.** That is an entirely reasonable number on a desktop GPU
with `VK_EXT_descriptor_indexing` and update-after-bind, which is what this renderer was designed
against. It is not a reasonable number on an Adreno 650.

## 3. Why that is fatal here rather than merely slow

Two things line up badly:

- **The Quest 2 reports Vulkan 1.1.284.** Descriptor indexing is core in Vulkan **1.2**. At 1.1 it is
  the optional `VK_EXT_DESCRIPTOR_INDEXING`, and plume treats it as exactly that — it appears in
  plume's *optional* extension list, and `capabilities.descriptorIndexing` is only set when
  `descriptorBindingPartiallyBound`, `descriptorBindingVariableDescriptorCount` and
  `runtimeDescriptorArray` are all present (`plume_vulkan.cpp:3952`).
- **Mobile descriptor limits are far smaller.** Even where the extension exists, per-set sampled
  image limits on Adreno are orders of magnitude below 65,536.

The crash itself is consistent with a rejected or unsupported path rather than a clean failure: the
faulting PC equals the fault address, and an odd address is impossible on AArch64, where
instructions are 4-byte aligned. That is a jump through a pointer holding a small integer, not a bad
data access — the shape of calling an entry point that was never loaded, or dereferencing a handle
the driver refused to create.

## 4. What this means for the port

The remaining work is a **renderer** change, not a platform one. Options, cheapest first:

1. **Scale the bindless heap by device capability.** Query
   `maxDescriptorSetUpdateAfterBindSampledImages` (or the non-update-after-bind limit when the
   extension is absent) and size `kBindlessTextureCount` from it instead of hardcoding 65,536.
   Cheap, and probably enough to get a first frame.
2. **Check `capabilities.descriptorIndexing` before building the bindless set at all**, and fail
   with a clear message rather than a jump into hyperspace. Worth doing regardless — a driver
   without the feature should say so.
3. **A non-bindless fallback path.** Conventional descriptor sets rebound per draw. Significant work
   and slower, but it is what a Vulkan 1.1 device actually wants.

Worth knowing before choosing: Blue Dragon is an Xbox 360 title with a 512 MB unified memory budget.
Whatever the real working set of simultaneously-bound textures is, it is nowhere near 65,536, so a
much smaller heap is very likely sufficient.

## 5. What is now known to work on the device

Recorded because it is easy to lose track of, and because none of it was true this morning:

| Piece | State |
| --- | --- |
| SDK cross-built for android-arm64 | Works |
| `libreblue.so`, 140 MB AArch64 | Works |
| APK build, install, launch on Quest 2 | Works |
| App root, argv, cvars, file logging | Works |
| XEX load, 154 kernel imports, guest heap | Works |
| VFS over real game data, 70008 records | Works |
| Vulkan device + swapchain on Adreno 650 | Works |
| Bindless pipeline layout | **Crashes — this note** |
| Stereo rendering / OpenXR session wired up | Not started |
