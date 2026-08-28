# Research: where Blue Dragon's shaders actually live

Date: 2026-08-28 16:20
Topic: why the generated shader cache is empty, and what it would take to fill it.

The Android build links but `g_shaderCacheEntries[] = {}`. A build in that state runs and draws
nothing, so this is the gap between "it compiles" and "it is a game". This note is what was ruled
in and out.

---

## 1. What XenosRecomp is actually looking for

Not a file extension. `XenosRecomp/main.cpp` recursively walks the input directory and scans the
**raw bytes of every file** for a `ShaderContainer` header:

```cpp
if ((shaderContainer->flags & 0xFFFFFF00) == 0x102A1100 &&
    dataSize <= (fileSize - i) &&
    shaderContainer->field1C == 0 && shaderContainer->field20 == 0)
```

It then hashes the container and names the shader after the file's stem. The `*.vso *.pso *.xex`
glob in `cmake/shader_cache.cmake` is only a *dependency list* for the build system — it does not
filter what gets scanned. Anything in `assets/` is fair game.

This matters because it means the assets are reproducible from the game itself. Nothing here needs
a file that only exists in someone else's repository, provided the shader containers can be found
in a form that is not compressed.

## 2. What was ruled out

**Xenia's shader dumps are the wrong artifact.** The Thor carries 118 files under
`Android/data/jp.xenia.emulator.github.debug/files/bd_shaders`, of which 37 are `.ucode.bin.vert`
and `.ucode.bin.frag`. These are *decoded microcode* that Xenia captured from GPU memory at
runtime, not the container the game shipped. Staged as `.vso`/`.pso` and fed to XenosRecomp they
produce zero entries, exactly as the magic check predicts. They are still useful as a reference for
what a translated shader should look like, but they are not inputs.

**The `pack/*.ipk` archives do not contain them.** `IPK1` is an uncompressed container with a plain
file table — `battle.ipk` opens with `camera\bcm01_bt_dd01.xsca` — so a byte scan would find
containers if they were there. Scanned `!necessity.ipk` (3 MB), `battle.ipk` (2 MB) and
`game_start.ipk` (15 MB): zero, zero, and one incidental three-byte hit that fails the rest of the
header check. These archives hold camera, animation and event data.

**`default.xex` scanned raw finds nothing**, which is the clue rather than a dead end.

## 3. What is almost certainly true

The shaders are inside `default.xex`, and the raw file cannot show them because **an XEX2 basefile
is compressed** (LZX, optionally encrypted). XenosRecomp scanning the file as it sits on disc is
looking at compressed bytes.

Two things support this:

- `rexglue codegen` reads the same file happily and produces 219 files of recompiled PowerPC, so it
  is decompressing the basefile internally.
- The SDK vendors **libmspack** — an LZX implementation — which is not something a recompiler needs
  for any other reason. It is there to unpack XEX basefiles.

So the capability exists in the tree; it just is not exposed. `rexglue --help` lists only `codegen`,
`init` and `recompile-tests`, and codegen leaves no decompressed image behind in `generated/`.

## 4. The path forward, in preference order

1. **Dump the decompressed basefile.** The cleanest fix, and it makes the shader cache reproducible
   from the disc alone with no private repository involved. Either add a `dump-xex` subcommand
   upstream, or write `tools/dump_xex.cpp` against the host SDK in `out/sdk/win-amd64` — the headers
   and libraries are already there. Feed the result to XenosRecomp and the cache fills.
2. **Get the existing dumps.** `zolaware/reblue-assets` is what CI clones into `assets/`. Private,
   so this needs upstream's cooperation, but it is the zero-effort answer if it is offered.
3. **Capture at runtime.** The Thor already has a `bd_frames_*.gfxr` GFXReconstruct capture and a
   working XenDroid. A capture records the translated shaders, not the originals, so this reproduces
   Xenia's output rather than the game's input — useful for verification, not for building.

Option 1 is the one worth doing. It is a small tool against an SDK that is already built, and it
turns "blocked on a repository we cannot read" into "a build step".

## 5. Side note: the extractor now walks subdirectories

`tools/extract_xex.py` grew path support while investigating this, so any file on the disc is
reachable without copying it:

```sh
python tools/extract_xex.py --list --name pack "…/Blue Dragon (Disc 1).iso"
python tools/extract_xex.py --name pack/game_start.ipk -o out/gamedata/game_start.ipk "…"
```

Pulling `game_start.ipk` (15 MB) out of a 7.8 GB image over WiFi adb took a few seconds. The disc
layout, for reference: `pack/` holds the `.ipk` archives, plus `map/`, `movie/`, and four
`snd_stream*`/`snd_memory*` trees per language.
