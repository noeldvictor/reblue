# 61% of pixel shaders can discard, which is why sorting bought nothing

2026-08-31. Quest 2, Adreno 650.

## Three measurements that only make sense together

1. **The frame is fragment-bound.** `bd_debug_fill_scale=50` - a quarter of the fragments, identical
   draw count - takes `gpu_total` from 56.18ms to 27.88ms.
2. **Front-to-back sorting bought exactly zero.** 55.98ms sorted against 55.95ms unsorted, with the
   sort provably firing (pipeline binds 14 to 39).
3. A fragment-bound pass with overdraw that gains nothing from depth ordering is a pass where the
   tiler's **low-resolution Z is not rejecting anything.**

## Why LRZ is off

Qualcomm's and Mesa's documentation agree on what disables it: blending, stencil, writing depth from
the shader, UAV writes, alpha-to-coverage, changing the depth comparison operator, reading the
framebuffer, and **`discard`**.

Note what is *not* on that list: loading rather than clearing the depth attachment. LRZ state is
explicitly reusable when the depth attachment was stored by a previous pass and is loaded unchanged.
The `VK_ATTACHMENT_LOAD_OP_LOAD` theory this note was going to test is wrong, and worth recording as
wrong so nobody spends a day on it.

**86 of 141 compiled pixel shaders contain `OpKill`** - 61%. Scanned straight out of the SPIR-V.

They contain it because `shader_recompiler.cpp:2299-2307` emits Xenos alpha test as

```hlsl
[branch] if (g_SpecConstants() & SPEC_CONSTANT_ALPHA_TEST) { clip(oC0.w - g_AlphaThreshold); }
```

`clip()` is a discard, and `g_SpecConstants` is a Vulkan specialization constant - so **one module
serves both the alpha-tested and the opaque variant, and the `OpKill` is in the module either way.**
Whether it survives into a given pipeline is up to the driver's specialisation. A conservative
implementation that decides LRZ from what the module *can* do, rather than what this specialisation
*will* do, turns LRZ off for every draw that uses one of those 86 shaders.

That is a X360-era shape leaking through: on a Xenos, alpha test was fixed-function render state,
free to toggle per draw. Translating it into a branch inside the pixel shader is what makes an
opaque draw indistinguishable from a cutout one.

## What to do about it

Emit alpha test as a **real variant**, not a spec-constant branch: two compiled modules, one with
the `clip` and one without, selected by the same pipeline key that already carries `specConstants`.
The opaque variant then contains no `OpKill`, and LRZ can reject for it.

This is a recompiler change - `thirdparty/XenosRecomp/XenosRecomp/shader_recompiler.cpp` around
2299 - and it is worth doing before anything else, because:

- The frame is fragment-bound, so anything that stops fragments being shaded pays directly.
- Front-to-back sorting is **already built and shipped** and currently earns nothing. It starts
  earning the moment LRZ engages.
- It costs no image quality at all, unlike every resolution or foveation lever.

The budget: `gpu_total` needs to fall 6.2ms for 20fps and 22.9ms for 30fps. Overdraw rejection on a
fragment-bound pass is the right shape of lever for numbers that size.

## Verifying it

`python tools/spv_caps.py` already decodes SPIR-V; the scan used here is the same idea and belongs
beside it. **The check is `OpKill` count in the android dump going from 86 to near zero for the
opaque variants**, followed by a within-run A/B of `bd_draw_sort`, which should stop being neutral.

## Sources

- Qualcomm, *Adreno GPU on Mobile: Best Practices* - the list of state that disables LRZ / early-Z.
- Mesa, *Low Resolution Z Buffer* (freedreno) - LRZ reuse across passes, and what invalidates it.
- Danylo Piliaiev, *Low-resolution-Z on Adreno GPUs*.
