# Native nested pass scopes

2026-09-05, Windows Vulkan desktop.

## Source and change

The guest-source guide directed inspection of `render_tweaks.toml`,
`render_list.toml` and the exact translated functions. Despite its name,
`bdSurfaceSetMSAA` (0x82273080, generated file 67) saves four colour handles
and one depth handle, increments their references, binds colour zero and
depth, clears the other colour slots and pushes a logical content extent.
It does not select a multisample count. `bdDestroySurface` (0x82273240,
generated file 94) restores those attachments and releases the saved references;
it is a pop, not surface allocation destruction. The getter wrappers
`sub_824739F0` / `sub_82473A38` read device +12168 / +12184 and add references.
`sub_82474388` tails the already replaced render-target setter.

The complete supported push/pop bodies are now host replacements. The
SDK-independent `NativePassStack` holds native attachment references and
logical extents. It saves the actual live targets at each entry, including
changes made by another rendering entry point. Depth-only/null passes inherit
logical dimensions; physical viewport dimensions still follow the bound image.
The native stack is not limited to seven levels. The temporary engine adapter
preserves its seven-level overflow no-op and empty-pop behavior.

Shared host attachment binders own the format, sample-count, multiview,
foveation, framebuffer-invalidating and viewport changes. Compatibility D3D
setters delegate to these same binders. No queue flush is introduced at a
target change: that boundary can run without a command list, so the outgoing
draw queue still flushes at `BindDrawFramebuffer` before the next framebuffer.

The adapter maintains big-endian getter shadows and saved-reference mirrors
for remaining engine readers, through the existing host reference/lifetime
implementation. Native pop restores its saved host references, not handles
re-read from the engine stack. A mirror check detects foreign stack writes;
it is not an independent comparison with original execution. Resource lookup
and reference release stay outside the video lock. Unsupported inputs or
additional colour attachments refuse before effects; original scopes above
native scopes unwind separately. The `bd_native_passes` correctness switch
defaults on; an already native-owned nesting chain finishes natively even if
the setting is changed mid-chain.

This removes two guest rendering bodies from supported pass entry/exit. It
does **not** yet replace the engine's traversal, pass scheduling, scene-begin
sampler/camera/effect producers, allocation classification, resolve/alias
adapters, surface wrappers/reference headers or frame-wide guest dependencies.
Multiple colour attachment rendering and all representative scenes remain work.

## Initial verification

- All 16 texture/upload/state/recipe/pass CTests pass; the separate material
  CTest passes, for 17 total. Three reflection/scene source-boundary guards pass.
- New tests cover nested restoration, independent intervening target changes,
  depth/null logical-extent inheritance, explicit zero extents, underflow,
  root-reset rejection during a scope, 24-level native nesting and retained
  image lifetime. They do not simulate engine refcount memory or a Vulkan device.
- The host-only `reblue` Vulkan target linked at 03:55:48 EDT, 47,247,360 bytes,
  source revision `cb4009c` dirty. Codegen wrote/deleted nothing; no guest
  objects rebuilt.
- Initial desktop process 25576 started 03:56:26 EDT, log `reblue_708.log`.
  Original five profile settings were used and all five audited successfully:
  autoplay/perf true, capture delay 60, minimum 600 draws, 120 frames.
  Early coverage has 18867 native pushes, 18866 pops, 901 depth-only and 15232
  null passes, peak nesting 1, no compatibility/refusals and 37733 matching
  getter-shadow checks. This does not qualify pixels or deeper GPU nesting.

Capture inspection and later-scene/final-eye verification are pending at this
initial implementation checkpoint. Known later scenery/text and VR
letterboxing/blur/depth limitations remain open.
