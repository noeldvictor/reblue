# Native scene-image draw recipes

2026-09-05, Windows Vulkan desktop. Follow-up to
`20260905_0301_native-scene-textures.md`.

## Source and ownership

The guest-source guide directed inspection of the render hook TOML and exact
translated source. `bdSceneNodeDrawSingle` (generated file 40) calls the active
draw callback at `((uint32_t(-32036) << 16) - 22280) + 36` before direct indexed
draws, conditionally on its material-change flag. The callback chain through
`sub_8221D530` / `sub_8221DB00` invokes visual vtable +32 after global callbacks.
`sub_8221E618` selects and binds current/next scene images, directly or through
the blend/constant wrapper `sub_82454C08`. Model reflection selection alone
cannot describe the final slot-5 input. The node also has ordinary and animated
texture writes, and can change active render-target selection between commands.

The new `SceneTextureRecipe` records named current/next roles and producer kind,
not an image address, old image handle or guessed match against another draw.
The host producer emits each semantic event only after its non-null texture
publication. An ordinary texture write clears that slot's semantic role even
if the image pointer does not change. Null selection remains a no-op. Each
draw snapshots its current role recipe; later writes do not mutate that draw.
New nodes do not infer roles from the previous node's bindings.

During capture, actual bindings are compared with the already prepared native
producer inputs. No texture-registry lookup occurs under the draw/video lock.
The node-entry logical selections must agree with the publication-time values;
otherwise the whole node remains explicitly unsupported until its intra-node
pass changes have a native sequence. Unknown callback owners are also counted
as unsupported, not silently converted by image-identity guessing.

After source verification and compatibility visual-history updates, converted
slots discard their retained native handles, wrappers, addresses and surface
inheritance flags. Replay resolves the current pair outside video/store locks
and preflights all draws before dispatch. It checks today's producer kind,
composes the requested live inputs, and supplies them to sampler composition,
packet overrides and the replay comparator. A requested null or unresolved
input refuses the complete node. Merge and material-census identity includes
the role recipe; different producers/roles cannot become the same recipe.

This removes retained scene-image selection from converted draw templates,
not all replay/template dependencies. Scene-table and persistent association
production, dynamic image ownership, native pass sequences, the wrapper's
blend/constants, shader ABI, other material inputs and full-frame scheduling
remain unconverted. Callback/global state inherited by later interpreted nodes
still requires independent qualification. Model reflection enable remains its
own contract; a scene image callback is not treated as a new enable producer.

## Initial verification

- All 15 texture/upload/state/recipe CTests pass (0.54 seconds), including the
  new SDK-independent semantic-role test. The material CTest also passes.
- Tests cover current/next mapping, repeated publication, ordinary overrides,
  null no-op, unrelated slots, independent draw/node snapshots, changing live
  native/dynamic inputs, atomic missing-input refusal and callback decoding.
- All three reflection/scene source-boundary guards pass. These are not a
  runtime concurrency proof.
- Host-only Vulkan build linked at 03:17:58 EDT. Codegen wrote/deleted nothing,
  and no guest objects rebuilt. Logged source revision is `3520ba5` dirty.

The first run uses autoplay/perf, capture delay 180, minimum 30 draws, 120
frames and `bd_host_draw_verify_every=8`. Scene producer comparison is off.
Runtime coverage and pixel inspection are pending at this implementation
checkpoint; tests alone do not qualify replay, later scenes or VR.
