# VR options belong in the game's own Config screen, and here is the seam

2026-08-31. Groundwork only - nothing built, because there is no headset attached to verify against.

## Why this rather than the ImGui panel

An ImGui panel exists (`src/ui/vr_options.cpp`, 13 rows, opened by a grip+trigger gesture or
`bd_vr_menu`) and it is verified to draw. It is also the wrong answer, and the owner said so
directly: the options should be in Blue Dragon's own menu.

That is not just aesthetics. The game's menu already has the cursor, the input handling, the fonts,
the localisation and the look. An overlay has to reimplement all of it and steal the pad while it is
open.

## The seam

The menu system is **already named** - 45 `AnimeMenu_*` functions in `config/functions.toml`,
including `AnimeMenu_AddEntryData`, `AnimeMenu_SetItemWideStringVar`,
`AnimeMenu_CheckConfirmInput`, `AnimeMenu_CheckDirectionInput`, `AnimeMenu_CursorMoveUpdate` and
`AnimeMenu_Draw`.

And the Config screen has a single entry point:

```
0x822D5988 = Camp__Config__MainTask__BuildRows
```

with `bdCampConfigDataLoad` and `bdCampConfigSetState` beside it.

## What the row table looks like

Read out of `generated/` - the recompilation is the decompiler, no tooling needed.

The function builds a table on its own stack and the body contains `r31 = 11`, which matches the
eleven rows the retail Config screen shows. The stores land on a **12-byte stride** from `r1+156`:

```
+156  r10   +160  r31   +164  r11      <- row 0
+168  r10   +172  r31   +176  r31      <- row 1
+180  r9    +184  r31   +188  r31      <- row 2
+192  r8    +196  r31   +200  r31      <- row 3
+204  r7    +208  r31   +212  r31      <- row 4
```

So each row is three `u32`s: a pointer, and two values. The pointers come from a run of
`addi rN, r31, -166xx` against a `lis r31, -32248` base - twelve distinct string addresses in
`0x8200xxxx` - and the second and third fields are mostly the constant 11.

**That last part is not decoded.** 11 is both the row count and the value stored in most fields,
which is exactly the kind of coincidence that has cost time today. Whether the fields are a type
tag, an initial value, a string id or something else is unknown, and guessing would be the fourth
unverified theory of the session.

## How to do it safely, when there is a device

Two mechanisms exist and the choice matters:

1. **A midasm hook at the function's return** (`config/hooks/*.toml`), reading the finished table.
   Needs the struct decoded, but changes no control flow.
2. **`REX_FUNC(Camp__Config__MainTask__BuildRows)`** replacing it wholesale, building all eleven
   original rows plus the VR ones in host C++. Cleaner and it is the "rewrite it as host code"
   direction - but it has to reproduce the original eleven exactly, so it needs the struct decoded
   too, and more of it.

Either way the first step is the same and it is cheap: **an entry counter and a dump of the
finished table**, run once on a headset, printing the eleven rows' three fields. That turns every
open question above into a fact in one run. Do not write the hook first.

## What to put in it

`src/xr/xr_settings.cpp` already defines everything worth exposing: `bd_vr_camera_mode` (first
person / third / diorama / flat screen), `bd_vr_world_scale`, `bd_vr_battle_diorama`,
`bd_vr_diorama_height`, `bd_vr_snap_turn`, `bd_vr_turn_degrees`, `bd_vr_comfort_vignette`,
`bd_vr_hud_mode`, `bd_vr_hud_distance`, `bd_vr_hud_scale`, `bd_vr_cutscene_policy`, and the
third-person offsets. The ImGui panel's row table (`src/ui/vr_options.cpp`) already has the labels,
ranges, steps and one-line explanations written; it can be lifted straight across.
