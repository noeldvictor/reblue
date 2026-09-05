# Explicit reflection texture selection

2026-09-05, Windows desktop. Follow-up to the stale slot-5 assets reported in
`20260905_0053_recurring-draw-verification.md` and still unconverted in the
lighting checkpoint.

## Source and first checkpoint

The guest-source guide kept the investigation in exact generated C++ after
reading `config/hooks/render_list.toml`. In `bdSceneNodeDrawSingle`
(`generated/reblue_recomp.40.cpp:9901`), the node initially disables reflection
and binds the current pass default from render-list state +68. Model command
`06xx` selects reflection policy at `loc_82281250`: 0..253 select a current-table
texture, 254 selects the pass default, and 255 disables sampling **without
unbinding** the previous image. A repeated command is wholly elided, including
after a different command changed the slot. Nonzero phases force the command
value to zero; the new decoder is explicitly phase-0 only.

The native material decoder now records selection and enable independently.
Ordinary `65xx` material texture overrides mark the selection unknown until a
non-elided explicit reflection selection replaces it. They are not silently
classified as pass-default bindings. Records have no guest addresses, but their
table indices remain import recipes, not persistent native texture identities.
The existing `.bdmat` property format is unchanged.

Standalone tests cover initial default, explicit table indices including 0/253,
default selection, disable without unbind, repeated-command elision, unknown
slot-5 overrides, unrelated slots, independent model resets and transactional
truncation. The existing material test target builds and its CTest passes
(0.04 seconds). This first checkpoint does not yet change runtime replay.

The colour-mask trace reached material callback dispatch in `sub_8221DB00`
and `sub_8221D548`. The direct draw body itself only sets alpha reference, but
that does not prove the callbacks cannot change colour write. No blanket
live-mask override was made; this remains a separate conversion boundary.
