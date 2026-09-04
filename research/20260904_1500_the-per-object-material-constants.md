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

## Materials are shared objects, not per-mesh data (17:10)

`bd_material_source` runs the same search on a tree draw, which unlike a list draw has a
real mesh address. Searched: the mesh, the object its first word points at, and the object
at `mesh + 0x10` (the VB/declaration record table), 4 KB each. Not found in any of them.

Two things fell out that matter more than the negative:

- The first samples all carried `c3 = (1, 1, 1, 1)`, the default white diffuse - a needle of
  only zeros and ones matches anything and finds nothing, so the search now waits for a
  material with a real colour in it. Worth remembering for the next search of this kind.
- With that filter on, **the same material `(0.149, 0.122, 0.051, 12.0)` appears across
  several different meshes**. So a material is a *shared* object referenced by the token
  stream, not data inline in the mesh.

That is the shape of the thing to cook, and it agrees with the census: 121 distinct
materials behind 579 node draws a frame is a table with references into it, not a value per
draw. So the cook is a material table plus a per-sub-draw index, which is also what makes it
small enough to ship.

What is left to find is the table's base and how the token stream indexes it, and that is a
read of the interpreter's material selection rather than a search - every structure reachable
from the draw has now been scanned and ruled out.

## The shortcut is closed: identity does not determine the colours (17:40)

Before decoding the guest's material table, the cheaper question: can the host build its own?
If a material's *identity* - both shaders, the blend and depth state, the textures, with no
constants in it - determined its colours, the host could key a table on that and stop asking
the interpreter entirely.

It does not. A village scene has **32 distinct identities, and 38 further colour sets
carried under them** - so a majority of identities appear with more than one material
colour. The same shader, state and textures are used with different diffuse and specular.

That closes the shortcut by measurement rather than argument. The host cannot infer the
material from anything it already captures; it has to read the guest's material table, which
means decoding the interpreter's material selection. Together with the earlier negatives -
not in the entry, not in the visual, not in the mesh, shared across meshes - the shape is
fully pinned: a table of materials somewhere reachable only through the token stream, indexed
per sub-draw, with 121 distinct entries behind 579 draws a frame.

## The chain, traced (18:10)

Read out of the recompilation rather than searched for, and it ends the guesswork about
where these constants live.

**A global staging struct at 0x82DE80D8** (formed everywhere as `lis 0x82DF0000` then
`addi -32552`) holds the shader constants the renderer is about to upload:

| offset | contents |
| --- | --- |
| +0 | vertex constants c0..c4 |
| +80 | **pixel constants c0..c13** - so c3 is +128 and c4 is +144 |
| +304 | vertex bool constants |
| +372 / +376 / +380 | dirty flags for the vertex, pixel and bool blocks |
| +408 | a write counter |

`sub_821981E0` is the flush: called once by `bdSceneNodeDrawSingle`, it uploads each block
whose dirty flag is set - `SetPixelShaderConstantFN(device, 0, struct + 80, 14)`. Fifteen
functions form this pointer, and the interesting ones are the interpreter itself and the
`sub_82173960` / `sub_821739B0` / `sub_821739F0` family it calls.

The interpreter writes the diffuse at `struct + 128` and sets the pixel dirty flag, and the
values come **from its own stack frame** (`r1 + 336`, `r1 + 340`, and a third in `f13`). So
the chain is:

```
mesh token stream -> (decode, upstream in the interpreter) -> interpreter stack
                  -> staging struct +128 -> flush -> PS c3
```

What that settles: the host cannot shortcut this by reading a structure, because the value
only exists in the staging struct *after* the interpreter has run for that node - which is
the staleness the whole problem started with. The cook has to do the decode the interpreter
does, from the token stream to the colour, and the remaining unknown is one hop: what writes
`r1 + 336`/`+340` upstream.

## The elimination is complete (18:40)

The interpreter loads the material float4 from `r23 + 4932 + 108`, and `r23` is `ctx[0]`,
which `guest_scene.h` documents as the visual. That predicts `visual + 5040`. It reads zero,
and a search of every structure reachable from a tree draw - **visual, mesh, the object the
mesh's first word points at, the record table at mesh + 0x10, and the traverse context, all
to 16 KB** - finds the captured colour in none of them.

Two readings survive, and they are worth stating so the next attempt starts in the right
place:

1. The load I traced is **one branch of several**. Immediately above it is
   `compare(r3, 3)`, which looks like a material-type switch, so the draws sampled here are
   probably served by a different branch with a different base.
2. The colour may be **computed rather than stored** - blended from a token and a
   modulator - in which case there is no address to read at all and the cook must reproduce
   the arithmetic.

Either way the answer is in the interpreter's material branches, not in memory, and no
further searching will help: everything a draw can reach has now been scanned.

## Correction (19:10): every search above was reading garbage

`bd::mem::try_load<T>` reads through `be<T>` and **already converts**. All the searches in
this note were written as `__builtin_bswap32(try_load<u32>(...))`, which double-swaps, so
every needle was compared against byte-reversed nonsense. The searches were not evidence of
anything.

The tell arrived by accident: a probe printed the traverse context's first word as
`20ab2c28` beside the host's visual `282cab20` - the same value byte-reversed. That reads as
"two different objects" and is actually "one object, read wrongly", which is worse, because
the first reading would have sent the next attempt somewhere useless.

Corrected and re-run. The context's first word is now `282cab20`, equal to the host's
`visual_va`, which confirms `r23 == ctx[0] == visual` as `guest_scene.h` says. And with
correct reads the search still finds nothing: not in the visual, mesh, `mesh[0]`,
`mesh + 0x10` or the traverse context, to 16 KB, and `visual + 5040` is zero.

So the conclusion survives its evidence being rebuilt - but it had to be rebuilt, and the
note above stood for half an hour on a swap bug. `try_at` hands back raw guest bytes and
needs a manual swap; `try_load` does not. Mixing them is silent.

## Why no structure held it: the colour is a product (19:30)

The interpreter has two stores into the staging struct's diffuse slot, on different branches,
and the one the sampled draws take is `loc_82280824`:

```
f11 = f10 * f11     with loads from r31 + 396, + 400, + 404 feeding the operands
f12 = f10 * f12
f13 = f10 * f13
store f11,f12,f13 -> r31 + 128, +132, +136   (which is pixel constant c3)
```

So `g_vObjectDiffuse` is **computed, not stored**: a base triple times a scalar. That is why
sixteen kilobytes of every structure a draw can reach held nothing matching - the value never
exists in memory until the multiply, and then only in the staging struct the flush reads.

The base is in the staging struct itself, at `+396`, `+400`, `+404` - immediately after the
`+392` gate that chooses between the two branches. The struct is a global, so the host can
read the base; what it still needs is `f10`, the per-draw modulator, and that is one more
hop back through the interpreter.

This also retires the search approach for good. A value that is the product of two others is
not findable by scanning for it, however correct the byte order, and three of today's dead
ends were spent looking for something that was never there.

## The formula, from the PowerPC (19:50)

The recompilation carries the original instructions as comments, and they settle it:

```
addi r11, r23, 4932
lfs  f11, 108(r11)      ; base.r   -> visual + 5040
lfs  f12, 112(r11)      ; base.g   -> visual + 5044
lfs  f13, 116(r11)      ; base.b   -> visual + 5048
lfs  f0,  120(r11)      ; base.a   -> visual + 5052
...
lfs  f10, 396(r31)      ; modulator.r  (the staging struct)
fmuls f11, f10, f11
lfs  f10, 400(r31)      ; modulator.g
fmuls f12, f10, f12
lfs  f10, 404(r31)      ; modulator.b
fmuls f13, f10, f13
stfs f11, 128(r31)      ; -> pixel constant c3
```

So **`g_vObjectDiffuse` is a component-wise product**: an object base at `visual + 5040` times
a modulator at the staging struct's `+396`. Both branches of the type switch use the same
base; the switch only chooses whether the modulator is applied.

That is the formula the cook needs, and both operands are addressable - one in the visual,
one in a global.

### The loose end, stated rather than buried

A probe reading `visual + 5040` at capture time returns `(0, 0, 0, 0)`, while the constant
the same draw produced is `(0.498, 0.498, 0.498, 1.0)`. Zero times anything is zero, so the
probe and the assembly disagree and **the trace is not finished**. The probe reads at capture
time, after the interpreter has run for every sub-draw of the node; the interpreter reads
during its own run. The next step is a read taken inside the DrawSingle hook before the
interpreter is entered, plus a dump of the window around `visual + 5040`, which will say
whether the offset is right and the field is transient, or the offset is wrong.

Not claiming the source is found until those agree - three earlier conclusions in this note
were withdrawn for less.

## Instruments this added

- `[node] drift by register a frame: psc4x23 psc3x6` - which registers cost recaptures.
- `bd_material_diag` - what a drifting material constant holds, beside the guest's
  per-draw material colour, so the source can be identified.
- `bd_material_census` - distinct sub-draw materials by content, which sizes the cook.
- The search in `bd_material_diag` and `bd_material_source`: given a value, find it in a
  guest structure or say it is not there. Four structures were ruled out with it, one run
  each. Its two traps: the needle must be the value this node's own run captured, not a
  sibling's, and a needle of only zeros and ones matches anything.

Sources: `src/gpu/scene/host_draw.cpp` (`drifted`, the visual register publish, the drift
counters), `src/gpu/shaders/hlsl/bd_normal_lit.hlsl` (the constant names),
`src/gpu/scene/guest_scene.h` (`kVisualMaterialColor`).
