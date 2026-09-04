# Blue Dragon VR port plan

> Historical plan. The active scope and completion gate are in
> [HOST_RENDERER_TRANSITION.md](HOST_RENDERER_TRANSITION.md), and shared agent
> instructions are in [AGENTS.md](../AGENTS.md). The dated status and phase order
> below are retained for reference, not current priorities or permission to
> resume Quest testing.

## Current state, 2026-08-30: the frame is GPU-bound and two X360 patterns own it

Stereo works and is verified on the headset. Speed is the whole remaining problem, and the
bottleneck moved: the CPU work landed and the GPU is now 96% of the frame.

```
dt_ms 158.74 | gpu_draw_ms 152.94 | fence_ms 131.16 | other_ms 27.03 | draws 522.87
```

Two X360 patterns account for it, and both are removable.

### 1. Shader constants are uncached global loads on Vulkan only

`shader_recompiler.cpp:1291` emits two constant paths behind `#ifdef __spirv__`. DXIL gets a
`cbuffer`; SPIR-V gets `vk::RawBufferLoad` from a 64-bit device address - 143-158 uncached global
loads per fragment on Adreno, where a UBO read would be hoisted at wave launch. Measured cost:
1.24 Gpix/s against the Adreno 650's 4-8, i.e. ~5x under the hardware floor, corroborated by a
5-7x gap against the desktop build of the same scene.

It is also why all 141 shaders declare `OpCapability Int64`, which an Adreno 740 cannot compile -
so **one change unblocks the AYN Thor and fixes the Quest's frame rate.** The replacement is
already written and shipping on D3D12.

Blocking work, both known: plume exposes no dynamic UBO offsets, and Adreno's four descriptor sets
are full, so the buffer must become a binding on an existing set. Collapsing HLSL spaces 0/1/2 -
one physical set bound three times - frees the slot and is the same prerequisite as the dropped
sun-occlusion set.

### 2. The surface pool is EDRAM semantics

`targets: 64 rows, 190.4 Mpix/frame`, with **sixteen live `1376x720x2L` targets** - four scene
alternates plus twelve more - over ~29 small ones each touched a quarter of a frame. They are
pooled aliases of a handful of logical surfaces, because the guest asks for a fresh surface per
pass the way a Xenon took a fresh EDRAM tile. ~127 MB of render targets for a 4 MB framebuffer.

### What this retires

`bd_debug_fill_scale` was read as proving the frame is bound by fragment *count*. It cannot
separate count from per-fragment cost - the two are multiplied, so both hypotheses give the same
curve. Resolution and foveation levers were chosen on that reading and should be re-argued against
the Gpix/s figure, which says cost.

---

Working document for this fork. Research current as of 2026-08. Nothing here is built yet.

Stated priorities, in the order given:

1. **VR camera. 6DOF head tracking is the critical item.** Third-person and first-person modes,
   plus diorama and a world-scale control.
2. **ARM64 Android** — AYN Thor first, Quest 2 native after.
3. **Cel shading on characters**, optional and toggled from the options menu.
4. **Tourist mode** — infinite HP, 999 stats, encounter suppression. Independent of everything
   else and the cheapest item here; can be built at any point.

---

## The scheduling problem, and the way around it

Those priorities are the reverse of the dependency order. VR runs on Quest; Quest is Android;
Android is blocked on cross-building the ReXGlue SDK, which nobody has published a slice for. Taken
literally, the most important work is behind the longest pole.

**It does not have to be.** re:Blue already builds a Vulkan executable on Windows (`reblue_vk.exe`)
and on Linux. OpenXR has desktop runtimes — SteamVR and Monado — and a Quest 2 on Link or Air Link
presents to a PC OpenXR runtime like any other headset. So:

> Build and debug the entire VR camera system on desktop Vulkan against **Meta XR Simulator** — a
> virtual Quest presented as an OpenXR runtime on the PC — with a real debugger and RenderDoc
> attached. Port to native Android only once the camera is right.

The simulator removes the headset from the loop as well as the device. It implements OpenXR at the
API level and supports native development, so the same code path runs unchanged in the simulator,
over Quest Link, and on-device — selected by the `XR_RUNTIME_JSON` environment variable, with no
rebuild and no registry edit. Input is simulated from keyboard, mouse, or an Xbox pad. Reach for the
actual Quest for comfort checks, real performance numbers, and driver bugs; nothing else.

This is the single most important decision in this document. It gets the critical work started
immediately, it front-loads all the hard rendering and camera questions onto the platform where
they are debuggable, and it means the Android port later inherits a working VR renderer instead of
trying to invent one on a device with no debugger and a 40-minute iteration loop.

Phases 0–5 are desktop. Phases 6–8 are the device.

---

## What the guest gives us

The single luckiest thing about this project: `config/functions.toml` already names the entire
camera pipeline. These are not addresses that need finding — they are already mapped, and the
recompiler will emit declarations for them.

| Guest symbol | Address | Why it matters |
| --- | --- | --- |
| `bdCameraViewSetMatrices` | `0x82135228` | **The interception point.** Sets view and projection together. |
| `bdCameraViewSetMatrix` | `0x82135128` | View matrix alone. |
| `bdCameraViewSetProjMatrix` | `0x821351A8` | Projection alone. |
| `bdBuildProjectionMatrix` | `0x82168E18` | Already hooked by `config/hooks/output_resolution.toml` for aspect and FOV. |
| `bdCameraViewFrustumTest` | `0x82135030` | Culling. Must be widened, or head-turning pops geometry. |
| `bdCameraComputeViewVectors` | `0x82168AB8` | Derived forward/right/up. Consumers of these need to agree with our view matrix. |
| `bdCameraLookAtDefault` / `LookAtTarget` / `LookFromBehind` | `0x82169060` / `0x821691B8` / `0x82169300` | The game's own framing modes. Third-person mode overrides these. |
| `bdCameraTranslate` | `0x82169458` | Camera position updates. |
| `bdFieldCameraSetupFollow` | `0x821B1A58` | Field camera follow logic — the thing that fights you in third person. |
| `bdScriptOpCameraControl` | `0x821BC438` | Scripted camera during events. Needs a policy (see Phase 4). |
| `bdCameraRender` / `bdCameraRenderSetup` | `0x82142D30` / `0x8213C8E0` | Per-camera render entry — the natural per-eye loop boundary. |
| `bdCameraRenderMotionBlur` | `0x8213CE90` | Disable in VR. Non-negotiable for comfort. |
| `bdPlayerFieldMovementUpdate` | `0x82207858` | Character locomotion. Needs to become camera-relative. |
| `bdMovieYuvQuadDraw` | `0x82130020` | FMV is decoded to YUV and drawn as a quad. Becomes a world-locked screen. |

The existing hook infrastructure (`config/hooks/*.toml` + `REX_FUNC` / `REX_HOOK_RAW`) is exactly
the right shape for all of this. See `CLAUDE.md` for how the two patch mechanisms differ.

---

## Phase 0 — XR subsystem skeleton

**Goal:** an OpenXR session opens, runs a frame loop, and presents black. No game rendering yet.

New subsystem `src/xr/`, namespace `bd::xr`, mirroring how `src/gpu/` and `src/platform/` are laid
out:

```
src/xr/xr_session.cpp/h    instance, system, session, reference spaces, lifecycle
src/xr/xr_swapchain.cpp/h  per-eye colour + depth swapchains, plume interop
src/xr/xr_frame.cpp/h      xrWaitFrame / xrBeginFrame / xrEndFrame, view acquisition
src/xr/xr_camera.cpp/h     pose -> view matrix, camera modes, world scale
src/xr/xr_input.cpp/h      action sets, controller poses, haptics
src/xr/xr_settings.cpp/h   cvars, mirroring src/gpu/settings.cpp
src/xr/xr_stub.cpp         no-op implementations for non-XR builds
```

Build wiring: XR is Vulkan-only and backend-dependent, so `src/xr/` sources go in
`reblue_backend_only` in `src/CMakeLists.txt`, not the common object library. Add a
`REBLUE_OPENXR` option, defaulting OFF, so the normal desktop build is untouched.

Vulkan interop is the fiddly part. OpenXR wants to create the `VkInstance`/`VkDevice` (or at least
dictate extensions and the physical device) via `xrGetVulkanGraphicsRequirements2KHR` and friends.
plume creates the device today. Two options, and the second is strongly preferred:

- Let plume create the device and hope it matches what the runtime demands. Fragile.
- **Add a device-creation injection point to plume** so the XR layer supplies the required instance
  extensions, device extensions, and physical device. `thirdparty/plume` is a submodule pointing at
  `zolaware/plume`; this fork will need its own plume branch. Budget for that.

**Done when:** the headset shows a black stereo view, the session survives focus loss and resume,
and quitting does not hang.

**Risk:** plume interop. This is the phase most likely to take three times its estimate.

---

## Phase 1 — Stereo rendering

**Goal:** the game renders twice, once per eye, into the XR swapchains.

**Superseded, 2026-08-29. Do not drive the guest twice.** This section used to say
`bdCameraRender` (`0x82142D30`) was the natural boundary and to call it twice per frame. It was
tried, at two levels, and it does not work:

| seam re-entered | draws/frame |
| --- | --- |
| off | 826 |
| `bdCameraRender` | 965 (+15%) |
| `sub_822D3598`, the whole view driver | 997 (+21%) |

Neither doubles, because **the render list is built once per frame above both seams** and a replay
finds only the remainder. Forcing it means re-running visibility and sorting per eye, on a frame
that already carries a ~62ms CPU floor. Re-entering a guest function mid-frame is safe and does work
- neither experiment crashed - it just does not produce a second scene.

**What does work: record once, submit twice.** `bd_stereo` submits every scene *geometry* draw twice
in `DispatchDraw`, into left and right viewports, with its own per-eye constant upload. One guest
frame, one render list, two views - the guest, the traversal, the sort and the draw recording are
all untouched, and only GPU fill doubles. It renders, with parallax and convergence, verified by
screenshot on the desktop build.

Three things this cost, all of which a draw count called "working":

- **Doubling every draw** subdivides the frame into vertical stripes: the post chain reads the target
  it is doubling, so each pass halves again. Restrict to scene geometry.
- **Restricting by target size** is not enough - the full-resolution post passes render to the scene
  surface. The discriminator is vertex count: a post pass is a full-screen quad of 3-4 vertices.
- **The scene-pass threshold must scale with `bd_render_scale`**, or the two features are mutually
  exclusive: at 50 the scene target falls under a fixed 1280x720 gate and stereo silently applies to
  nothing.

Per-eye geometry is a skew of the view-projection at VS registers 32-35, which are its **columns**:
`register32 += separation * register34 + convergence * register35`, i.e.
`clip.x' = clip.x + sep*clip.z + conv*clip.w`. Reading them as rows mixes components and sends the
two eyes to different viewpoints. Still to do: per-eye OpenXR views rather than half-viewports, and
deriving the two constants from a real IPD and the runtime's per-view fov instead of clip-space
units. `bd::xr::LastGuestProjection()` captures the guest's own projection - 45 degree horizontal
fov, right-handed, -Z forward, the same handedness OpenXR uses.

Touches:

- `src/gpu/frame.h`, `src/gpu/frame_ring.cpp` — the frame ring becomes per-eye. Constant buffer
  and descriptor allocation doubles.
- `src/gpu/present.cpp` — presentation moves from the SDL swapchain to `xrEndFrame` when XR is
  active. Keep an SDL mirror window; you will want it for debugging and it costs almost nothing.
- `src/gpu/output.cpp` — `Output::ProjectionAspect()` and the aspect-fit logic assume one flat
  rectangle. XR supplies per-eye FOV as four tangent half-angles, not an aspect ratio. This needs a
  parallel path, not a patch to the existing one.
- `src/gpu/resolve.cpp`, `src/gpu/occlusion.cpp` — anything that assumes one back buffer.
- `src/gpu/screenshot.cpp` — capture one eye.

**Watch for:** anything caching per-frame state that is now per-eye. The PSO predictor
(`src/gpu/pipeline/pso_predictor.cpp`) and the occlusion queries are the likely offenders — both
will see double the draws and may mispredict.

**Get the MSAA resolve right here, not in Phase 8.** On a tile-based GPU the MSAA samples never need
to leave on-chip memory — but only if the render pass declares a 4x colour and depth attachment with
a non-MSAA image in `pResolveAttachments`. Meta measure the alternative, resolving through memory, at
roughly 3 ms on the GPU, which is over a fifth of a 72 Hz frame.

`src/gpu/resolve.cpp` currently resolves with a full-screen shader pass reading a `Texture2DMS`,
which forces the MSAA buffer to main memory. That is the right design for what it does — emulating
the Xbox 360's guest-driven EDRAM resolve, which is not a render-pass-end event — so **do not replace
it. Separate it.** The guest EDRAM path keeps the shader; the XR eye buffers get a render pass with
resolve attachments. They are two different problems sharing one code path today, and splitting them
after the render pass structure is built is far worse than splitting them now.

**Done when:** the scene appears in both eyes with correct stereo separation and no per-eye
divergence in effects.

---

## Phase 2 — 6DOF camera and modes (the critical phase)

**Goal:** head pose drives the camera. Character stays on the stick.

### Interception

`bdCameraViewSetMatrices` (`0x82135228`) is the choke point. Implement a `REX_FUNC` that:

1. Reads the game's intended view matrix and camera position.
2. Passes them to `bd::xr::Camera` as the *anchor*.
3. Composes the final per-eye view matrix from anchor + head pose + mode offset + world scale.
4. Writes the result back, and writes the OpenXR per-eye projection over the game's projection.

Everything downstream — culling, lighting, post-processing, the HUD — keeps working because it
reads the matrices the game thinks it set.

### Modes

Exposed as `bd_vr_camera_mode`:

**0 — First person.** Anchor at the player character's head bone. The character model needs hiding
or the inside of Shu's skull fills your view; check whether the renderer can skip the player draw
without breaking shadows. Blue Dragon's animations are authored for a third-person camera, so
expect the head to whip around during attacks. A pose-smoothing filter on the anchor is mandatory,
not optional.

**1 — Third person (default).** A virtual anchor point floats behind and above the character.
Head 6DOF offsets freely from that anchor, so you can lean in, look around, and peer over things.
`bdFieldCameraSetupFollow` (`0x821B1A58`) and the `bdCameraLookAt*` family are suppressed in this
mode — the game's own follow logic is exactly what you are replacing. Offsets are tunable via
`bd_vr_third_offset_x/y/z`.

**2 — Diorama.** The anchor detaches from the character and sits high and back, world scaled down
hard (`bd_vr_world_scale` around 0.05–0.15). You are a giant looking into a tabletop version of the
scene; the party runs around below you. This is the most comfortable mode by a wide margin and the
one most likely to look genuinely good, because it sidesteps every problem caused by the game's
authored camera framing. Worth building early as the fallback that always works.

**3 — Cinema.** Flat 2D on a large world-locked screen. Trivially correct, zero comfort risk, and
the guaranteed-working baseline. Ship this first even though it is the least interesting.

### World scale

`bd_vr_world_scale` maps game units to metres. Implemented by scaling the head-pose translation
before composing it into the view matrix — **not** by scaling the world matrix, which would break
physics, culling, and the skybox. Scale also has to feed IPD-relative eye offsets, or stereo depth
and world size disagree and the scene reads as a diorama when you wanted a room.

Blue Dragon's world unit is unknown; measure it in-game against a character of known height and
write the constant down here once it is known.

### Culling

`bdCameraViewFrustumTest` (`0x82135030`) tests against the game's frustum. Once the head can look
away from where the game thinks the camera points, geometry pops out of existence at the edges.

**Test once against a volume enclosing both eyes**, plus a head-motion margin
(`bd_vr_cull_expand`) — not twice per eye, and not a single eye's frustum scaled up. An object
occluded for one eye can be visible to the other, so per-eye testing is the only *correct*
alternative and it costs twice as much; a combined volume is correct for both viewpoints at roughly
half the price. This is what Umbra's stereo camera does and what the industry converged on. Start by
forcing the test to pass, to prove the camera works, then tighten to the combined volume.

Two follow-ups from the same research, both for Phase 8: **round-robin occlusion** (alternate the
queried eye each frame, trading a frame of visibility latency for half the queries) as a fallback if
combined volumes prove too conservative, and **hybrid mono rendering** (render distant geometry once
monoscopically, since stereo separation is imperceptible past a certain distance) which for a JRPG
with large outdoor areas and a skybox could be a significant win.

Watch `src/gpu/occlusion.cpp` and the guest's own queries (`bdCameraCheckAllPointsVisible`,
`pfx_occlusion_count_ps`) — those double under two-pass stereo, and if the eyes disagree about
whether a lens flare is occluded it will flicker.

Same class of problem for LOD selection and shadow cascade fitting — both are camera-derived.

### Comfort

Non-negotiable list: disable `bdCameraRenderMotionBlur` (`0x8213CE90`), disable or heavily reduce
depth-of-field (`bd_dof_*` already exists in `src/gpu/settings.cpp`), suppress camera shake, add an
optional comfort vignette during locomotion, and offer snap turn as well as smooth turn.

**Done when:** you can stand in a field, look around freely with your head, and walk the character
around with the stick without the camera fighting you.

---

## Phase 3 — Input and locomotion

**Goal:** controller drives the character, head drives the camera, and the two are properly
decoupled.

- OpenXR action sets in `src/xr/xr_input.cpp`, bridged into the existing `src/engine/action_map.cpp`
  so the game's rebinding UI keeps working. The action map is already an abstraction over SDL
  gamepad input, which is the right seam.
- **Camera-relative locomotion.** `bdPlayerFieldMovementUpdate` (`0x82207858`) currently resolves
  stick direction against the game camera. It must resolve against the *head* forward vector
  projected onto the ground plane, or walking "forward" means whatever the game camera last decided.
- Touch controllers are OpenXR poses, not HID gamepads. A paired Bluetooth pad is the pragmatic
  option and probably the better experience for a JRPG; support both.
- Menus: `src/engine/menus/` and the existing mouse cursor support (`src/engine/mouse_cursor.cpp`)
  give a head start on a controller laser pointer. The mouse cursor path is already wired through
  the menus, so a ray-cast that emits mouse-cursor events is likely the cheapest route.

---

## Phase 4 — HUD, 2D, menus, and cutscenes

This is where flat-game VR ports usually die. Budget accordingly.

**HUD.** Blue Dragon's HUD is a 2D layer composited in screen space. `src/engine/hud_anchor.cpp`
and `config/hooks/hud_anchor.toml` already exist to reposition it for non-16:9 aspects, which is a
real head start. In VR it needs to become either a head-locked quad at a comfortable distance
(`bd_vr_hud_distance`, ~2m, `bd_vr_hud_scale`) or a world-locked panel. Head-locked is easier and
less nauseating; make it the default. Screen-space effects that assume they can cover the whole
frame — fades, flashes, letterboxing — need individual decisions.

**Menus.** Full-screen 2D. Render to a texture, present as a quad. The camp menu and config menu
are the most-used screens in the game, so this is not optional polish.

**Cutscenes.** Two kinds, and they need different answers:

- *Scripted in-engine scenes* driven by `bdScriptOpCameraControl` (`0x821BC438`) and the
  `bdCutscene*` family. These deliberately move the camera, which is exactly what VR must not do.
  Policy options: let them play with head offset only (safest); force diorama mode for their
  duration (most comfortable); or fully honour them and accept the nausea. Make it a setting,
  default to head-offset-only.
- *FMV*. `bdMoviePlayerInit` / `bdMovieYuvQuadDraw` (`0x8212F260` / `0x82130020`) decode SFD video
  to YUV and draw a quad. In VR this becomes a world-locked cinema screen — genuinely one of the
  easier wins here, and it is also where Android hardware video decode pays off (Phase 7).

---

## Phase 5 — Cel shading

**Goal:** toon outlines and banded lighting on characters. Post-process, masked — no guest shader
surgery.

Blue Dragon is Akira Toriyama character design over a fairly flat anime-styled renderer, so this is
working with the art direction rather than against it.

**Approach.** Two host shaders in `src/gpu/shaders/hlsl/`, registered in `cmake/generated.cmake`
alongside the existing `reblue_host_shader` entries:

- `toon_outline_ps.hlsl` — Sobel or Roberts-cross edge detection over depth and reconstructed
  normals, producing inked outlines. Width in `bd_toon_outline_width`.
- `toon_quantize_ps.hlsl` — luminance banding into N steps (`bd_toon_bands`, default 3–4) with a
  soft threshold so it does not shimmer under animation.

New host code in `src/gpu/toon.cpp/h`, driven from the existing post-process chain.

**The masking question is the whole problem.** Applying this to the entire scene will fight the
skybox, the 2D layer, particle effects, and the FMV quad. Character draws have to be identified.
Three candidate routes, in order of preference:

1. **Stencil.** Tag character draws with a stencil bit during their draw calls, then run the
   post-process only where that bit is set. Cleanest if a free stencil bit exists.
2. **Draw-call classification.** `src/gpu/hooks/draw.cpp` and the vertex declaration and shader
   hashing in `src/gpu/vertex_declaration.cpp` and `src/gpu/pipeline/` already fingerprint draws for
   the PSO predictor. Character draws likely share a recognisable signature — skinned vertex
   declaration, specific shader hashes. Reuse that machinery.
3. **A separate character-only depth prepass.** Most expensive, most reliable.

Investigate (1) first; fall back to (2). `bd_toon_mask_mode` selects, so all three can coexist while
you work out which actually holds up across the whole game.

**Interaction with VR:** outlines are view-dependent and must be computed per-eye, or they swim.
This is a real cost — do it after Phase 2 so you are measuring against the real frame budget.

**It must be optional and live in the options menu.** `bd_toon_enabled` off by default, exposed as a
row in the visuals section of the config menu next to the existing graphics toggles —
`src/core/settings_rows.cpp` and `src/engine/menus/config_menu_visuals.cpp`. Outline width, band
count, and mask mode go in the same group. Toggling it must take effect immediately without a
restart, like the other visual settings already do.

---

## Phase 5b — Tourist mode

**Goal:** wander the world in VR without the game killing you. Invincibility, maxed stats, and
optional encounter suppression. Independent of every other phase — buildable at any point, and by
far the cheapest thing in this document.

The point is not cheating for its own sake. A 6DOF camera in a JRPG is a sightseeing device, and
sightseeing goes badly when a random encounter drags you into a battle every forty seconds.

**re:Blue has already done most of the work.** `src/engine/character.h` exposes a typed model of the
guest character struct — `Character::HP()`, `MaxHP()`, `Stats()`, `StatusFlags()`, `HasStatus()`,
`StatusResist()` — and, critically, `PlayableCharacter::SetHP()` is already a working setter.
`src/engine/stat_breakdown.h` goes further and decomposes each derived stat into its base block,
best-of-class bonus, and permanent bonus block, documenting exactly how the guest's
`Player_CalcBattleParams` sums them.

New code in `src/engine/tourist.cpp/h`, `bd::engine`.

**Invincibility.** Two possible routes:

1. **Re-assert HP.** Watch HP each tick and call `SetHP(MaxHP())` whenever it drops. Trivial,
   robust, and works regardless of damage source. Downside: damage numbers still pop and death
   animations may briefly trigger.
2. **Suppress the damage write.** Cleaner visually, but needs the damage application site located in
   the guest — it is not currently named in `config/functions.toml`, so it means a reversing session.

Start with (1). It is a dozen lines against an API that already exists. Move to (2) only if the
visual noise is annoying.

**999 stats.** Do *not* write the derived `CharaBattleParams_t` block directly — `Player_RecalcParams`
will overwrite it on the next recalculation. Write the **permanent bonus block** that
`stat_breakdown.h` already identifies, and let the game's own `Player_CalcBattleParams` sum it up.
The stats come out maxed through the game's normal path, they survive recalculation, they display
correctly in the menus, and nothing has to be fought. This is the whole reason
`stat_breakdown.h` existing is such a gift.

Cap at the field width rather than a literal 999 where the struct allows more; clamp where it does
not, to avoid overflow into adjacent fields.

**Encounter suppression.** The random encounter trigger is not named in `config/functions.toml` yet
and will need locating — likely near `bdPlayerFieldMovementUpdate` (`0x82207858`), since encounter
steps usually accumulate on movement. A midasm hook that zeroes the step accumulator is the
expected shape. Offer three states: normal, suppressed, and forced-on, because the last one is
useful for testing battle rendering in VR.

**Also worth having:** infinite MP, one-hit kills (the inverse — for when you *do* want a battle to
end), and a no-clip / free-fly camera. No-clip pairs naturally with the diorama and third-person
camera work in Phase 2, since the camera anchor is already decoupled from the character by then.

**Presentation.** One `bd_tourist_mode` master toggle in the options menu that flips the group on,
plus individual cvars underneath for people who want only some of it. Saves and achievements are the
open question — Blue Dragon has achievements and re:Blue adds eight of its own
(`src/engine/achievements/`). Decide whether tourist mode disables achievement unlocks. It probably
should, and it should say so in the menu.

| Cvar | Default | Notes |
| --- | --- | --- |
| `bd_tourist_mode` | 0 | Master toggle for the group. |
| `bd_tourist_invincible` | 1 | Re-assert HP each tick. |
| `bd_tourist_max_stats` | 1 | Via the permanent bonus block. |
| `bd_tourist_infinite_mp` | 1 | |
| `bd_tourist_encounters` | 0 | 0 normal, 1 suppressed, 2 forced. |
| `bd_tourist_noclip` | 0 | Free-fly. Pairs with the Phase 2 camera work. |
| `bd_tourist_block_achievements` | 1 | |

---

## Phase 6 — Android ARM64

**Goal:** the thing runs on an AYN Thor.

ARM64 itself is a solved problem here: upstream ships a `linux-arm64` AppImage, the SDK has a
`linux-arm64` slice and an ARM64 contributor, and the recompiled guest code is architecture-neutral
C++. `src/core/threading.cpp` holds the only x86 intrinsic and already has a portable fallback.

**The Thor problem is Android, not ARM.** It runs Android 13 on a Snapdragon 8 Gen 2 (Adreno 740;
the Lite is SD865/Adreno 650). Work, in dependency order:

1. **Cross-build the ReXGlue SDK for `android-arm64`.** No release slice exists, so it builds from
   source. **In progress and much smaller than feared** — see
   `research/20260828_1600_android-arm64-bringup.md` and `patches/rexglue-sdk-android.patch`. The
   SDK's own platform detection already classifies Android as `linux-arm64`, the NDK clears its
   Clang floor, and SDL3 configures itself natively with aaudio, opensles, android hidapi and both
   the Vulkan and OpenXR backends. What actually needed patching: the X11/Wayland pkg-config
   requirement, `clock_time_conversion` (libc++ gap the SDK already handles for Apple), the missing
   ucontext family (bionic removed it; `libucontext` supplies it in aarch64 assembly and the fiber
   backend links unmodified), and `librt`. Use **NDK r30 or newer** — r29's libc++ has no
   floating-point `from_chars`.

   **Watch the host-tools split.** `rexglue`, `XenosRecomp`, `dxc` and `reblue_prelink` must build
   for and run on the *host*. Pointing `REXSDK_DIR` at the source tree makes CMake build the SDK as
   a subdirectory, which would cross-compile `rexglue` and leave codegen with an unrunnable
   binary. Codegen has to come from a host SDK; only the runtime cross-compiles.
2. **Guest address space.** The recompiler reserves a large fixed virtual region so guest pointers
   are base + offset. Verify the reservation succeeds under bionic. Check page-size assumptions:
   Android 15+ mandates 16 KB page support; Horizon OS on Quest 2 is Android 12 era at 4 KB.
3. **Toolchain and preset.** NDK r29-ish, `cmake/android-arm64.toolchain.cmake`, and an
   `android-arm64` preset in `CMakePresets.json`. Host tools (`rexglue`, `XenosRecomp`, `dxc`,
   `reblue_prelink`) build for and run on the *host*; only the runtime cross-compiles. `REBLUE_D3D12`
   is already forced OFF off Windows, so the Vulkan-only path comes free — and because the Vulkan
   backend uses real spec constants rather than runtime DXC linking, **no shader compiler is needed
   on the device.** That is a large problem this project simply does not have.
4. **The Android target is a shared library, not an executable.** `rex/ui/windowed_app.h` defines
   `XE_UI_WINDOWED_APPS_IN_LIBRARY` when `REX_PLATFORM_ANDROID`, swapping the entry point from a
   `GetWindowedAppCreator()` that an executable's `main` calls, to a registration table a library
   exports. Correct for Android — an APK loads a `.so` from an Activity rather than exec-ing a
   binary. So `add_executable` becomes `add_library(... SHARED)` there, the SDK's
   `windowed_app_main_sdl.cpp` template must not be compiled, and the app registers itself with
   `REX_DEFINE_APP`. **This is the current stopping point; everything else compiles.**

5. **The shader cache is empty without the asset dumps.** XenosRecomp globs `assets/` for
   `*.vso`/`*.pso`/`*.xex`. With only `default.xex` present it emits
   `g_shaderCacheEntries[] = {}` — the build links but has no shaders to draw with. The dumps live
   in the private `zolaware/reblue-assets` repo. Extracting them from the discs, or from a running
   capture, is its own task and is required before anything renders.

6. **Platform layer.** `src/platform/` assumes a desktop: GTK/SDL file dialogs, desktop shortcuts,
   crash handler, WinHTTP/libcurl. Needs SAF for file access and an activity/APK harness.
7. **Installer off.** `src/installer/` wants three DVD images and produces ~15 GB. Build with
   `REBLUE_BUILD_INSTALLER=OFF` and side-load a pre-extracted `game/` directory under
   `Android/data/` to unblock everything else. A proper SAF import flow comes later or never.

---

## Phase 7 — Hardware acceleration on device

This deserves its own phase rather than a footnote. An Android build that ignores it will be
unplayable regardless of how good the VR camera is.

**GPU driver.** The prior art here is `SansNope/UnleashedRecomp-Android`, an AI-assisted Android
port of Unleashed Recompiled, and its scars are instructive: it needed a replacement Mesa Turnip
driver for several Adreno generations, sysmem render mode on Adreno 6xx to dodge corruption and
hangs, and MSAA disabled on Adreno 750. Plan for driver-selection support (bundled Turnip plus
AdrenoTools `.so`/`.zip` import) rather than trusting the stock blob.

**Texture formats — check this first, it can sink the schedule.** Xbox 360 textures are DXT1/3/5.
Query `vkGetPhysicalDeviceFormatProperties` for BC1/BC3 at startup. If BCn is absent you need CPU
decompression or an offline ASTC transcode, and both cost real VRAM and load time. `src/gpu/format.cpp`
and `src/gpu/texture_upload.cpp` are where this lands. An offline transcode during install is much
better than per-load CPU decompression, but it changes the install flow.

**Video decode.** FMV currently decodes on CPU to YUV. Android MediaCodec gives hardware decode, and
`bdMovieFrameConvert` (`0x8212FD60`) plus `bdMovieYuvQuadDraw` (`0x82130020`) are a clean seam to
swap the decoder behind. Worth doing on Quest, where CPU headroom is scarce and dropped FMV frames
are very visible.

**Audio.** SDL3 audio will function; Oboe gives materially better latency on Android and is what the
prior art uses. Latency matters more in VR than on a handheld.

**Xbox 360 audio.** `XMAPlaybackConsumeDecodedData` (`0x8268B1B8`) — XMA decode is CPU work the SDK
handles today. Profile it before assuming it is fine on an Adreno 650-class device.

**VR-specific acceleration on Quest:**

- **Fixed foveated rendering** (`XR_FB_foveation`) **together with subsampled layout**. Large, nearly
  free win on Adreno for anything fill-bound, which a stereo JRPG at 72 Hz will be. Two caveats:
  FFR works best rendering directly into the eye textures, so re:Blue's composite chain
  (`src/gpu/output.cpp`, `present.cpp`) gives up part of the benefit; and enabling FFR *without*
  subsampled layout leaves most of the win on the table — that is the explanation behind most "FFR
  did nothing for me" reports. Never ship one without the other.
- **Prefer MSAA to render scale.** If fill-bound, scale resolution down and add MSAA rather than the
  reverse — MSAA is close to free through resolve attachments, extra pixels never are.
- **Application SpaceWarp** (`XR_FB_space_warp`). Renders at half rate and synthesises intermediate
  frames from a motion vector buffer plus depth. Vulkan-only, which suits us. It requires generating
  motion vectors, which the guest renderer does not produce — but `src/engine/frame_interp.cpp` and
  `config/hooks/frame_interp.toml` already exist for frame interpolation, so some of the thinking is
  done. Given the game is natively 30 fps and Quest 2 wants 72 Hz, this is arguably the difference
  between shipping and not.
- **Multiview** (`VK_KHR_multiview`) to halve draw-call submission. Requires the translated shaders
  to index a per-view matrix, which means touching XenosRecomp output. Defer until measurements say
  submission is the bottleneck.

**Memory.** Quest 2 has 6 GB LPDDR4X and developer reports put the practical per-app ceiling around
4 GB. Guest RAM is only 512 MB, but host-side textures, the shader cache, and doubled per-eye
resources are the real consumers. Measure early.

---

## Phase 8 — Performance and shipping

Quest 2 is an Adreno 650, roughly SD865 class — weaker than the Thor's 8 Gen 2 — with stereo
doubling raster cost at 72–90 Hz. The game being natively 1280x720 at 30 fps is the saving grace.
Quest 2 was discontinued in September 2024 and its support window runs to roughly 2026–2027, so it
is a fixed, non-improving target. That is actually convenient: optimise for one known device.

Order of attack: fixed foveation, then resolution scaling, then AppSW, then multiview, then draw
submission. Tracy is already wired in (`REBLUE_PROFILING`) — use it rather than guessing.

---

## New settings

Registered alongside the existing cvars in `src/gpu/settings.cpp` and surfaced through
`src/core/settings_rows.cpp` and `src/engine/menus/config_menu_visuals.cpp`.

| Cvar | Default | Notes |
| --- | --- | --- |
| `bd_vr_enabled` | 0 | Master switch. |
| `bd_vr_camera_mode` | 1 | 0 first, 1 third, 2 diorama, 3 cinema. |
| `bd_vr_world_scale` | 1.0 | Game units to metres. Diorama wants ~0.05–0.15. |
| `bd_vr_eye_height` | 1.6 | Metres, for standing reference space. |
| `bd_vr_third_offset_x/y/z` | 0 / 1.5 / -3 | Third-person anchor offset. |
| `bd_vr_snap_turn` | 1 | Snap vs smooth. |
| `bd_vr_turn_degrees` | 30 | Snap increment. |
| `bd_vr_comfort_vignette` | 1 | During locomotion. |
| `bd_vr_hud_mode` | 0 | 0 head-locked, 1 world-locked, 2 off. |
| `bd_vr_hud_distance` | 2.0 | Metres. |
| `bd_vr_hud_scale` | 1.0 | |
| `bd_vr_cull_expand` | 1.5 | Frustum expansion factor. |
| `bd_vr_cutscene_policy` | 0 | 0 head-offset only, 1 force diorama, 2 honour fully. |
| `bd_toon_enabled` | 0 | |
| `bd_toon_outline_width` | 1.0 | |
| `bd_toon_bands` | 4 | |
| `bd_toon_mask_mode` | 0 | 0 stencil, 1 draw classification, 2 depth prepass. |

---

## Risk register

| Risk | Severity | Note |
| --- | --- | --- |
| plume/OpenXR Vulkan device-creation interop | **High** | Needs a plume fork. Gates Phase 0. |
| ReXGlue SDK will not cross-build for Android | **High** | Gates all of Phase 6+. Unknown until attempted. |
| No BCn on target Adreno | **High** | Forces CPU decode or an offline transcode; changes install flow. |
| Game camera logic fights 6DOF override | Medium | `bdFieldCameraSetupFollow` and scripted camera ops. Mitigated by diorama fallback. |
| Character masking for cel shading is unreliable | Medium | Three fallback routes planned. |
| Quest 2 performance simply insufficient | Medium | Foveation + AppSW + res scaling are the levers. Cinema and diorama modes are cheaper. |
| First-person mode is unusable | Low | Animations are authored third-person. Acceptable to ship it as a novelty. |
| Quest 2 support window closes | Low | Fixed target, already known. |

---

## Immediate next actions

Rewritten 2026-08-29. Everything the old list asked for is done: plume is forked with the seam,
`src/xr/` is up, the SDK cross-builds for android-arm64, and tourist mode works.

**1. Put the Quest on USB and run `python tools/bench_quest.py all`.** One command, no build. It
sweeps the levers verified on desktop, one variable at a time, and prints a table. Everything below
is gated on what it says.

**2. Read `elsewhere` in that run, not just the fence.** The frame's two halves may not be
independent. `bd_debug_max_draws=500` once took the Quest's CPU from 60.7ms to 29.5ms - 31ms across
~2400 draws, where the renderer cost measured on desktop predicts about 10ms. The factor of three
suggests part of the "CPU floor" is the guest waiting on the render thread, in which case
**`bd_render_scale=50` will shrink it as a side effect** and the floor is partly a symptom of the
fill problem. If `elsewhere` does not move, it is real computation.

**3. Only then choose the CPU work.** If the floor is real, the census
(`bd_guest_census`) says `bdSceneNodeDrawSingle` dominates - 420 calls a frame against
`bdAnimBoneEvaluate`'s 63 - so the lever is submitting fewer nodes, and `src/xr/xr_cull.cpp` is
written, unit-tested and connected to nothing. Run the census in a battle first: it has already
changed its own answer once by looking somewhere new.

**4. Stereo on the headset.** `bd_stereo` renders correctly on desktop. The device step is per-eye
OpenXR views instead of half-viewports; the present path already owns its target, which is what
makes it small.

Do not hand-write `bdAnimBoneEvaluate` - the census says it is a tenth of the cost the plan assumed.
Do not build culling before step 2. Do not start with cel shading.
