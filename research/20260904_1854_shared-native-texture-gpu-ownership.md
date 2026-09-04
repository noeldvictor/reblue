# Shared native GPU texture ownership

2026-09-04. This removes per-guest-wrapper GPU texture ownership for converted
static assets. It is progress toward the full host renderer, not completion of
the scene/frame migration or VR qualification.

## Inspected boundary

Read the canonical instructions, transition plan and repository devloop,
guest-source and vrsim skills. Inspected `texture_upload`, `resources`,
`native_texture_mirror`, `bindless`, `graveyard`, `frame_ring`, device lifetime,
the existing native asset libraries and `config/hooks/physical_buffers.toml`.
No guest source, generated output, hook configuration or dependencies changed.

Previously, CPU texture assets were shared but every `BuildNativeTexture`
created a new `GuestTexture` owning an image, view and bindless slot. Its
destruction path retired that slot regardless of any potential native owner.
Sharing just the image would therefore have been insufficient.

## Implemented

`scene/native_texture_gpu` accepts a native CPU asset and returns a shared
`NativeTextureGpu` with image, view and descriptor. It contains no guest header,
format or allocation address. `VideoState` owns the content-keyed GPU store.
Matching content reuses the same GPU allocation and descriptor without another
upload. Cubes retain all six faces; ordinary 2D assets use the one-layer array
view required by the existing shader heap; volume and mip footprints derive
only from native data. CPU staging is bounded to one image at a time and at most
64 MiB per padded subresource. No mapped GPU buffer is read as CPU source data.

`BuildNativeTexture` is now only a compatibility adapter holding a native GPU
handle and borrowed pointers/descriptor. `BindTextureSRVLocked` uses the native
binding directly. Wrapper destruction invalidates only its borrowed slot field;
it does not null/free the shared descriptor. Dropping the final adapter does not
destroy the image if another native handle still exists.

`FencedAssetCache` implements the production lifetime policy independently of
the SDK/GPU. Unpinned entries are marked only after the slot-entry drain. Only
that slot's next completed fence can reclaim them. Reacquisition cancels the
mark, and still-pinned entries cannot be retired. The callback nulls/frees the
descriptor before destroying the view and image. Pending entries remain inside
the 256 MiB payload/8192-entry budgets; a pending fence is not available memory.
Those bytes exclude driver alignment/metadata and are not a measured headset
allocation. Access/reacquisition is serialized with the renderer lock; arbitrary
concurrent weak-handle promotion is not supported.

The store follows the device's existing process-lifetime ownership/shutdown
contract. Dynamic textures, render targets and unconverted imports retain their
separate, explicitly tracked compatibility lifetime. Native asset-level scene
bindings, samplers, draw/pass producers and shader ABI replacement still remain.

## Build and automated tests

Used existing `out/build/win-amd64-release`, target `reblue`, Clang 22.1.8,
D3D12 off and OpenXR on. Linked successfully. The shared resource headers
rebuilt dependent host translation units; codegen reported the module up to
date and no guest translation units rebuilt. Existing unrelated compiler
deprecation/designator warnings remain.

The first sandboxed standalone Ninja build stopped producing output. It was
explicitly stopped; inspection confirmed its processes gone before retrying
outside the sandbox. The escalated test and renderer builds succeeded.

- Texture CTest: 2/2 passed (format/persistence and new GPU lifetime test).
- Material CTest: 1/1 passed.
- Mesh CTest: 1/1 passed.
- Stereo Python tests: 2/2 passed.

The new test runs the production cache with instrumented image destruction.
It covers two owners sharing one upload, a native owner outliving adapters,
wrong-fence refusal, reacquisition before retirement, no premature release of
unsubmitted references, pending-byte and entry budgets, upload failure,
descriptor-before-image destruction, and exactly-once retirement. It is a
lifetime-policy test, not a Vulkan driver validation test.

## Desktop pixels and runtime evidence

Both runs used the new default native path, not a performance A/B. Captures
were copied from each run into separate directories under `out/verification`.

| Run | Capture and verdict |
| --- | --- |
| Flat `reblue_637.log`, `native_texture_gpu_flat` | 120 final 1920x1080 frames, 119 pairs, zero jumps above 6%, zero cyan patch frames; median cyan 0.011%, max 0.02%. Inspected village/character/foliage/terrain textures and shadows |
| Multiview `reblue_638.log`, `native_texture_gpu_vr` | 120 final stacked 936x2060 frames, 119 pairs, 10 jumps at 29/30/32/33/35 and 93/94/96/97/99; zero cyan patches. Inspected both eyes and `jump_029.png`; blur/lighting changes repeat exactly 64 frames apart |

Both runs' last periodic GPU reports show **615 uploads, 25 reuses, 10 retired,
605 resident, 91,512,512 payload bytes; zero refusals or failures**. These are
periodic snapshots, not asserted whole-run final counts. CPU reports show
cached assets/mips, with no mip generation or cache write errors. No error,
critical, Vulkan-error, device-loss or retire-race message was found in either
log. This does not establish full-game coverage, GPU memory savings or speed.

Flat used the original five-line profile: autoplay, perf CSV, capture after
60 seconds, at least 600 draws, 120 frames. Multiview used the previously
recorded diorama settings: VR on, legacy stereo off, multiview/layered textures
on, final-eye capture, mirror off, camera mode 2, diorama height 0; minimum 450
draws. All 13 profile settings audited successfully. The process-local xrsim
manifest used its checked absolute DLL path; recommended eye size was
1440x1584, head height 0. The actual capture size above is not native target
resolution. Logged eye and game cameras differed, confirming XR composition.

The stereo checker exits 2: **INCONCLUSIVE**, with fewer than two textured
bands. Distant blurred geometry and letterboxing do not qualify depth. The
64-frame defect was already recorded before texture GPU sharing, including
with native materials/meshes disabled. This run reproduces its cadence but does
not isolate the exact retained-state cause or clear the VR renderer.

Both app processes were stopped and the original profile restored. No Quest
or Thor run took place, no game-derived assets were committed, and no further
push was attempted while the existing source-upload approval remains pending.
