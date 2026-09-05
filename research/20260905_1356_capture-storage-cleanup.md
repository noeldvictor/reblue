# Lossless capture storage cleanup

2026-09-05, Windows desktop, EDT. Completes the archive job left running in
`20260905_1344_native-post-scheduling.md`. The owner requested disk cleanup
and durable storage discipline. `AGENTS.md` was updated and pushed in
`67eba28`; the separately verified renderer checkpoint is `b60c9c1`.

## Scope and preservation

Only identified historical raw captures were losslessly NTFS-compressed.
No file was deleted, renamed or moved. Game/disc data, saves, profiles,
dependencies, source and build artifacts were not cleanup targets. Existing
capture hard links remain usable and refer to the same compressed content.
Logical directory totals can double-count these links.

The initial set was exactly 120 raw frames in
`out/verification/scoped_camera_vr`. Every SHA-256 matched before/after.
The first-file trial also verified its shared capture/verification hard links.
Observed free space during the sequence increased from 619442176 to
1667072000 bytes, excluding the already completed first-file trial.

The bulk job selected exactly 7381 top-level files under
`out/build/win-amd64-release/logs/capture`: names matching
`^frame_\d+_\d+\.raw$`, last-write time before local 2026-09-05, initially
not NTFS-compressed. No renderer was running when this set was selected.
Each resolved absolute path was checked against that exact directory and
reparse points were rejected. Each file was hashed, processed with
`compact.exe /C /Q` using its exact path, then hashed again. No recursive
directory command or directory-wide compression setting was used.

The job completed successfully by 13:55:57 with all 7381 before/after SHA-256
comparisons matching and no compression failure. A fresh inventory confirmed
7381 selected files and 7381 compressed attributes. New September-5 native
post flat/VR captures were outside the bulk selection and remain available.

## Measured storage

Stored bytes below were queried after completion with Windows
`GetCompressedFileSizeW`, once per selected capture name, not by summing both
the capture directory and its verification hard links.

| Set | Files | Logical bytes | Stored bytes |
| --- | ---: | ---: | ---: |
| Scoped-camera VR | 120 | 2189724000 | 1133514752 |
| September-4 archive | 7381 | 60568619300 | 30808399872 |

Together, stored capture data is 30816428676 bytes (28.70 GiB) smaller than
its logical payload. This logical-versus-stored difference is not a precise
before/after whole-volume allocation measurement.

During the bulk job, actual volume free space rose from 35776147456 to
62299176960 bytes, an observed increase of 24.70 GiB. New verification
captures and other disk activity happened during that interval, so the
whole-volume change cannot be attributed exclusively to compression.
The subsequent sample was 62299144192 bytes, or 58.02 GiB free.
An earlier unrelated rise in available space before the bulk job is not
claimed as a result of this cleanup.

## Ongoing rule and limitations

The new canonical policy checks output estimates and real volume free space,
warns below 20 GiB, requires reclaiming space before large jobs below 10 GiB,
reuses existing builds/data and bounds active capture evidence around 10 GiB
with explicit correctness exceptions. It preserves required failure evidence
and protects valuable data from speculative deletion. `CLAUDE.md` remains a
thin import of `AGENTS.md`.

Compression preserves all bytes and can be reversed with `compact /U` on
the exact files, provided sufficient expansion space is available. No restore
is needed to read the captures normally. This cleanup does not resolve any
renderer correctness issue. Compression ran concurrently with the later
native-post correctness checks; those runs do not establish GPU performance.
