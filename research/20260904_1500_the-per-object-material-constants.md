# The per-object material constants, and why the host still needs the interpreter for them

2026-09-04, 15:00. Desktop. Written because two ways of removing the interpreter runs they
cause were built, measured and reverted, and the next person should not spend the afternoon
on a third.

## What is left of the guest in a frame

Measured, not estimated (`bd_guest_census`, `[node] host-issued`):

| | |
| --- | --- |
| node draws a frame | 579 |
| host-issued | 450-484 |
| still interpreted | 95-129 |
| refused: fresh values / no template / refresh / never / drift | 18 / 18 / 4-19 / 1 / 29 |

Plus effects, particles and UI, which never leave the per-draw ABI at all, and the guest's
own frame assembly (`bdRenderViewSubmit`, `bdCameraRender`, `bdFrameSubmitAndDebugHUD`).

## The 29 drift refusals, named

A new `[node] drift by register` line says the template recaptures are almost entirely two
pixel-shader registers: **ps c4 twenty-three times a frame and ps c3 six**. The recompiled
shader names them itself:

```
#define g_vObjectDiffuse  g_PSC[3]
#define g_vObjectSpecular g_PSC[4]
```

Per-object material colours. `bd_material_diag` prints what they hold when they drift:

```
ps c4 drift: template (0.149 0.149 0.149 25.0) fresh (0.200 0.200 0.200 25.0)
ps c4 drift: template (0.498 0.498 0.498 25.0) fresh (0.149 0.149 0.149 25.0)
ps c4 drift: template (0.098 0.098 0.098 25.0) fresh (0.498 0.498 0.498 25.0)
```

Each line's *fresh* is the next line's *template*. The values are not changing over time -
they are **different sub-draws' materials**, rotating through the visual's single cached
slot. A visual is a model with many meshes and the meshes have different specular; the
host's per-visual register cache is the wrong granularity for them. (`visual+0xD4C` and
`visual+0xBBC`, the guest's per-draw material colour copy, read all zeros for these, so they
are not the source either.)

## Attempt one: reclassify the drifting register as moving

Take the visual's fresh value instead of recapturing the template. Host-issued draws
483 -> 511 of 579, drift 29 -> 0.

Wrong, and the replay verifier said so at once: **ps c4 wrong on 44,924 draws and c3 on
19,627**, against 1,354 and none with it reverted. The visual's value is another mesh's, so
the replay painted this mesh with that one's specular.

Worth noting how nearly this passed: a 120-frame capture sequence showed 30 neighbour jumps
over 6%, which looked like the tell, and was not - the pairs are an autoplay camera pan and
the diff is a coherent shift rather than localised popping. The verifier is what settled it,
because it composes what the replay would issue and diffs it against the interpreter's own
draw inside one run.

## Attempt two: stop drift-checking per sub-draw registers

Track, per visual, which registers two sub-draws wrote differently in one frame, and skip
the drift check for those - the template's value came from this node's own run and is right
by construction, so the comparison against a sibling's is meaningless. Drift 29 -> 0,
host-issued 450 -> 474.

Also wrong, though far less so: **ps c4 wrong on 5,718 draws against 1,351**. The check was
spurious *and* load-bearing. It fires on the sibling mismatch, but the same trip also
catches genuine material animation, which the 16-frame refresh would otherwise let sit stale
for up to sixteen frames. Removing it trades 29 interpreter runs a frame for about 4,400
draws carrying a stale material colour.

Both reverted. The tracking flags are kept, unused, with a pointer here.

## What would actually work

The host has to know where a sub-draw's diffuse and specular *come from* rather than
watching the interpreter set them - which is the material cook. Each sub-draw's material is
a fixed property of its mesh, modulated by whatever animates it; cooked into a host material
record, the replay would compose c3 and c4 itself, the drift check would have nothing to
catch, and those 29 draws a frame would join the host's.

That is the same conclusion the effects and UI paths reach from the other direction, and it
is why the cook is the next structural piece rather than another inference on the seam.

## The cook's size, measured (15:40)

`bd_material_census` keys every captured sub-draw by content rather than by the node that
carries it: the pixel and vertex shader, the blend and depth state, the textures by guest
address, and the pixel constants the run set. Two sub-draws with the same key want the same
cooked record whichever visual they belong to.

**A village scene has 121 distinct sub-draw materials**, against 579 node draws a frame and
612 node templates. So the cook is small - a hundred-odd records, not thousands - and the
reason the host currently re-learns them every sixteen frames is that it keys them by node
instead of by content.

(The census deadlocked on its first run by taking the store's mutex inside the capture path,
which already holds it. Non-recursive; the app froze on frame one and wrote a 330-line log.)

## Where the material comes from: narrowed, not found (16:10)

`bd_material_diag` labels each drift line list or tree, and **every one is a render-list
draw**. So the whole of the drift problem lives in the deferred path, not the tree walk.

The render-list loop uploads the pixel material constants itself, in one call:

```
D3DDevice_SetPixelShaderConstantFN(device, start = 0, data = r30 + 80, count = 14)
```

That is PS c0..c13 per entry, so a list draw's own diffuse and specular are at that buffer
+ 48 and + 64. Two guesses at where the buffer is were tried and are wrong, and are recorded
in the code so a third is not made blindly:

| guess | result |
| --- | --- |
| `entry + 80 + N*16` | all zeros |
| `entry + 468 + N*16` (from `r30 = r31 + 388` further up the loop) | garbage; a different r30 |

Finding it means reading `sub_8227F360` around its upload rather than probing offsets. Once
it is found, a list draw's material is readable directly and those 29 interpreter runs a
frame become host-issued without any inference from siblings.

## Where the material actually comes from (16:40)

The offset guessing was replaced by a search: scan a structure for the float4 the node's own
run captured, and report where it is. Two corrections were needed to make the search mean
anything - the needle must be the **template's** value, since "fresh" is a sibling mesh's
material, and an all-zero needle matches any hole so it is skipped.

With that, the answer is a clean negative:

| searched | result |
| --- | --- |
| render-list entry + 0..8192 | not there |
| visual + 0..8192 | not there |

So a sub-draw's diffuse and specular are in neither the per-frame entry nor the visual. That
leaves the place `bdSceneNodeDrawSingle` is documented to read: **the mesh's own token
stream**, which the interpreter walks for stream, declaration, index buffer, texture, shader
and material-colour selection.

That is the useful result, and it is what makes the cook possible rather than merely
desirable. The token stream is a fixed asset, not per-frame state: decoded once per mesh
offline it yields a material record the host can compose c3 and c4 from directly, with no
interpreter, no sibling inference and nothing to drift. It also explains the rotation seen
earlier - the values never changed over time, the host was simply reading whichever sibling
mesh ran last.

The next step is concrete: decode the material-colour tokens out of the mesh stream, which
is a read of the interpreter around its material selection rather than another probe.

## Instruments this added

- `[node] drift by register a frame: psc4x23 psc3x6` - which registers cost recaptures.
- `bd_material_diag` - what a drifting material constant holds, beside the guest's
  per-draw material colour, so the source can be identified.
- `bd_material_census` - distinct sub-draw materials by content, which sizes the cook.
- The search in `bd_material_diag`: given a value, find it in a guest structure, or say it
  is not there. Two structures were ruled out with it in one run each.

Sources: `src/gpu/scene/host_draw.cpp` (`drifted`, the visual register publish, the drift
counters), `src/gpu/shaders/hlsl/bd_normal_lit.hlsl` (the constant names),
`src/gpu/scene/guest_scene.h` (`kVisualMaterialColor`).
