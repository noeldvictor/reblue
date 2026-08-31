# Depth rejection removes half the GPU work, and the Quest may not be getting it

2026-08-31. Measured on the desktop (RTX 3060), because the Quest disconnected - and the desktop
loop turned out to work all along.

## The measurement

`bd_debug_depth_always` forces every pipeline's depth compare to `ALWAYS`, so no fragment is ever
rejected by depth. Same scene, same draw count, ~3600 frames selected by draw count each:

| | `gpu_total_ms` | draws |
| --- | --- | --- |
| baseline | **4.63** | 839 |
| depth compare ALWAYS | **9.36** | 835 |

**Depth rejection is worth half the frame.** The scene carries roughly 2x overdraw, and early-Z is
currently throwing it away.

## Why this is a Quest finding despite being measured on a desktop

Overdraw is a property of the **content** - how many times the game draws over the same pixel - not
of the GPU. The same scene submits the same geometry in the same order on both. What differs is
whether the hardware rejects it.

And on the Quest there is direct evidence it does not:

- The frame is fragment-bound: a quarter of the fragments halves `gpu_total` (56.18 -> 27.88ms).
- Front-to-back ordering buys **exactly zero** - 55.98 against 55.95 - with the sort provably firing
  (pipeline binds 14 to 39).

A fragment-bound pass with 2x overdraw where depth ordering changes nothing is a pass where
low-resolution Z is not rejecting. If the Quest is paying the full 2x on a ~45ms scene pass, then
**roughly 20ms of the 56ms frame is overdraw that early rejection should be removing.**

20ms crosses two pacing tiers: 56.18 -> ~36ms is past the 50.0ms boundary (20fps) and close to the
33.3ms one (30fps).

## Why this is the best lead on the board

It is the only candidate with all three properties:

- **It is large.** ~20ms against a 6.2ms tier boundary. Every other measured lever was 0, +17, or 3.
- **It costs no image quality.** Unlike resolution or foveation, rejecting a fragment that would
  have been overwritten anyway changes nothing on screen.
- **The machinery to exploit it is already shipped.** `bd_draw_sort` does front-to-back ordering
  today and earns nothing; it starts earning the moment LRZ engages.

## What is still unknown, and how to close it

*Why* Adreno's LRZ is not rejecting. Two causes have been tested and eliminated - the depth load op
(state is reusable when depth is stored then loaded unchanged) and the alpha-test discard
(`bd_debug_no_alpha_test` moved `gpu_total` 56.18 -> 56.12, i.e. not at all).

Remaining, from Qualcomm's and Mesa's lists: **blending** - only 166 of 562 draws are opaque, and
writing depth with blend enabled forces invalidation, so one such draw early in a pass poisons the
rest - a **changing depth compare operator**, and **shader depth writes**.

The confirming run on a Quest is one command, and the probe is already built and committed:

```
bash tools/verify_quest.sh "bd_stereo=false,bd_stereo_multiview=true,bd_debug_depth_always=true"
```

If `gpu_total` does **not** move from 56.18ms, nothing was being rejected and the 20ms is there to
take. If it rises toward 100ms the way the desktop doubled, rejection is already working and this
lead is dead - which would itself be worth knowing, because it would mean the fragment cost is
something else entirely.

## Method note

This was measurable only because the desktop loop turned out to work - a claim this session
withdrew three times before photographing a field scene properly. Ninety seconds a run against the
device's four minutes, and it does not disconnect.
