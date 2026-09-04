# Native mesh assets and capture ownership

2026-09-04, desktop only. No Quest build, deployment or measurement.

The owner clarified the active goal: all rendering becomes host-owned while
gameplay stays recompiled. Art style and readability stay; materials, lighting,
geometry and assets may change. `docs/HOST_RENDERER_TRANSITION.md` preserves the
full completion requirements. This entry is an intermediate conversion.

## Native geometry

`native_mesh_data.*` defines a versioned, checked little-endian `.bdmesh` file:
layout identity, base vertex, triangle indices and GPU-ready vertex streams.
There are no guest VAs, host pointers or X360 resource structs in the payload.
The importer retains the packed vertex layout the current shaders understand;
native materials and independently loaded layout definitions remain required.

`native_mesh.cpp` imports loaded model geometry at the replay boundary, excluding
host-owned lock scratch buffers. Strip restarts reset winding parity; degenerate strip indices advance
it. Triangle lists retain ordinary 65535 indices. Source bytes, layout and draw
parameters form the content key, so reloading a model at new addresses resolves
the same disk asset. Files have bounded counts and sizes plus a payload checksum;
invalid or partial files are rebuilt. This is a trusted local derived-asset
cache, not a cryptographic content-authentication format.

The GPU allocation is a shared vertex/index/storage arena. All draws in a
chunk bind one R32 index view and carry their index range in the draw command,
which keeps distinct meshes eligible for indirect grouping. Each asset's
streams point at its own copied bytes, independent of physical-block lifetime.
Existing scene/shadow/reflection LODs feed their CPU triangle lists into this
same importer; no upload memory is read back. Geometry has a bounded 256 MiB
arena budget. Eviction/streaming and asset loading independent of draw discovery
remain necessary for the full game.

`bd_native_meshes` is on by default. Coverage counters explicitly count indexed
replays. Interpreted draws, dynamic geometry and the guest material/constant
producer are still outside this conversion. No frame-time gain is claimed.

## Checks and observations

- Main desktop target compiled and linked; codegen reported the guest up to
  date and compiled no guest translation units.
- Standalone CMake/CTest: 1/1 passed in `out/native_mesh_check`. Tests cover strip
  winding, restart parity, degenerates, 16/32-bit input, base-vertex bounds,
  multi-stream round trips, every truncation boundary and every single-byte
  corruption of a fixture, duplicate streams and trailing bytes.
- Flat alternating-frame geometry check: `reblue_622.log`, evidence in
  `out/verification/native_mesh_flat`. 657 cooked assets / 6,805,672 bytes,
  one 32 MiB GPU arena, zero import or budget refusals. 120 captures, 119
  neighbouring pairs, zero jumps over 6%, zero cyan patches. The village,
  character, foliage and shadows were inspected in the image.
- Desktop OpenXR, 1440x1584 runtime eye recommendation: the first run reused
  630 disk assets and cooked 273 additional meshes (`reblue_623.log`). A later
  run reused all 729 meshes it encountered without cooking (`reblue_625.log`).
  This proves persistent reuse for the tested content, not full-game coverage.
- Corrected presented capture: `out/verification/native_mesh_presented`,
  120 frames at 936x2060 (two 936x1030 eyes), with exactly one recorded readback
  and one written frame per sequence entry.
- Final build/default check: the original five-line profile was restored, with
  no native-mesh override. `out/verification/native_mesh_default.log` records
  652 meshes loaded, zero cooked, zero import refusals and native indexed
  replays active. All test game processes were stopped after verification.

## Two verification bugs found

1. A sequence made `CaptureDue()` return true at both the in-pass presented
   capture and the later guest-surface capture. The second resized/deleted the
   first readback buffer before submission, and wrote the guest image instead.
   The later site now stands aside when `g_captured_in_pass` is true. Before the
   fix, a 120-entry request wrote about 60 files; after it, all 120 final-eye
   captures are present. This also removes the outstanding-copy lifetime error.
2. `stereo_check.py` matched featureless black letterbox bands at the search
   boundary, reported -90 px for them, and called that correct depth. It now
   excludes horizontally untextured bands and search-boundary matches, and
   reports insufficient evidence rather than a flat-image diagnosis when
   useful variation is absent. Two regression tests pass (blank layers and a
   textured pair with known crossed disparity).

## Remaining VR state defect

The presented diorama view shows periodic lighting strips. The native/original
alternating run has 12 jumps over 6% in 119 pairs, in groups at sequence 6..12
and 70..76. A control with native meshes disabled has the same 12 jumps and the
same visible strip, at 23..29 and 87..93. The exact 64-frame recurrence matches
the existing template refresh cadence. This points to that retained state path;
the responsible constant or binding has not yet been isolated.

Control evidence: `out/verification/native_mesh_control/reblue.log` and the
inspected `jump_029.png`, versus presented `jump_012.png`. Do not label the new
mesh importer as the cause, or mark the VR renderer clean. The distant diorama
view also cannot establish stereo depth once letterboxes are excluded. The
windmill close-up's automatic near/far ordering was not a valid depth test
either. A useful near/far capture remains required.

Next architectural work: replace template-derived material/constant sources
and the guest draw producer with explicit native scene/material records, then
complete frame/pass ownership, effects/UI and animation. The native geometry
boundary makes those records able to name persistent meshes. Quest work still
waits for the full desktop gate.
