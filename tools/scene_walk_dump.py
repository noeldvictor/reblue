#!/usr/bin/env python3
"""Read a scene-walk recording (.bdsw) and say what the guest drew.

    python tools/scene_walk_dump.py <walk.bdsw> [--meshes N] [--materials N]

Prints, per recorded frame and pass, the node draws and the distinct meshes
and materials, then the meshes drawn most often across the window (the
instancing candidates) and the materials with the most meshes.

The format is written by src/gpu/scene/scene_recorder.cpp: a header and four
flat POD arrays in host byte order. Struct layouts here mirror that file's
structs field for field; bump both together.
"""
import argparse
import collections
import struct
import sys

HEADER = struct.Struct("<4sIIIIIII")

# NodeDrawRecord
NODE = struct.Struct("<IIIIIIII16fIIQQfI12f")
# MeshRecord: key, block_hash, block_size, stream_count, 16 x (slot, offset,
# stride, size), ib_offset, ib_bytes, ib_format, indexed, count, start_index,
# base_vertex, start_vertex, decl_hash, centre[3], radius
MESH = struct.Struct("<QQII" + "IIII" * 16 + "IIIIIIiIQ3ff")
# MaterialRecord: key, vs, ps, decl, state, ps_block, tex_key[16], fetch[16][6],
# tech, spec
MATERIAL = struct.Struct("<QQQQQQ" + "Q" * 16 + "I" * 96 + "II")
# TextureRecord
TEXTURE = struct.Struct("<QIIIIIIII32s")


def read(path):
    data = open(path, "rb").read()
    magic, version, first, frames, n_node, n_mesh, n_mat, n_tex = HEADER.unpack_from(data, 0)
    if magic != b"BDSW":
        sys.exit(f"{path}: not a scene walk (magic {magic!r})")
    if version != 2:
        sys.exit(f"{path}: version {version}, this tool reads 2")
    off = HEADER.size
    nodes = [NODE.unpack_from(data, off + i * NODE.size) for i in range(n_node)]
    off += n_node * NODE.size
    meshes = [MESH.unpack_from(data, off + i * MESH.size) for i in range(n_mesh)]
    off += n_mesh * MESH.size
    mats = [MATERIAL.unpack_from(data, off + i * MATERIAL.size) for i in range(n_mat)]
    off += n_mat * MATERIAL.size
    texs = [TEXTURE.unpack_from(data, off + i * TEXTURE.size) for i in range(n_tex)]
    return (first, frames), nodes, meshes, mats, texs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("path")
    ap.add_argument("--meshes", type=int, default=15)
    ap.add_argument("--materials", type=int, default=10)
    a = ap.parse_args()
    (first, frames), nodes, meshes, mats, texs = read(a.path)
    print(f"{a.path}: frames {first}..{first + frames - 1}, {len(nodes)} node draws, "
          f"{len(meshes)} meshes, {len(mats)} materials, {len(texs)} textures")

    mesh_by_key = {m[0]: m for m in meshes}
    mat_by_key = {m[0]: m for m in mats}

    # Per frame and pass.
    per = collections.defaultdict(lambda: [0, set(), set(), 0])
    for n in nodes:
        frame, pass_id, view, seq, visual, node_index, tech, blended = n[:8]
        mesh_key, material_key = n[26], n[27]
        row = per[(frame, pass_id, view)]
        row[0] += 1
        row[1].add(mesh_key)
        row[2].add(material_key)
        row[3] += blended
    print("\nframe  pass  view  draws  meshes  materials  blended")
    for (frame, pass_id, view), (draws, ms, mts, bl) in sorted(per.items()):
        print(f"{frame:5}  {pass_id:4}  {view:4}  {draws:5}  {len(ms):6}  {len(mts):9}  {bl:7}")

    # The instancing candidates: (mesh, material) pairs drawn more than once
    # in one frame and pass.
    pairs = collections.Counter()
    for n in nodes:
        pairs[(n[0], n[1], n[26], n[27])] += 1
    repeats = collections.Counter()
    for (frame, pass_id, mk, mt), c in pairs.items():
        if c > 1:
            repeats[(mk, mt)] += c - 1
    if frames:
        saved = sum(repeats.values()) / frames
        print(f"\nmergeable draws per frame (same mesh and material, same pass): {saved:.1f}")
    print(f"\ntop {a.meshes} repeated (mesh, material) pairs, draws saved over the window:")
    for (mk, mt), c in repeats.most_common(a.meshes):
        m = mesh_by_key.get(mk)
        desc = "?"
        if m:
            block_hash, block_size, streams = m[1], m[2], m[3]
            count, indexed = m[72], m[71]
            desc = f"block {block_hash:016x} {streams} streams {'indexed ' if indexed else ''}{count} verts"
        print(f"  {c:5}  mesh {mk:016x}  mat {mt:016x}  {desc}")

    # Materials by how many meshes wear them.
    meshes_per_mat = collections.defaultdict(set)
    for n in nodes:
        meshes_per_mat[n[27]].add(n[26])
    print(f"\ntop {a.materials} materials by distinct meshes:")
    for mt, ms in sorted(meshes_per_mat.items(), key=lambda kv: -len(kv[1]))[: a.materials]:
        m = mat_by_key.get(mt)
        if m:
            vs, ps, tech = m[1], m[2], m[118]
            n_tex = sum(1 for k in m[6:22] if k)
            print(f"  {len(ms):5} meshes  vs {vs:016x} ps {ps:016x} tech {tech} {n_tex} textures")
        else:
            print(f"  {len(ms):5} meshes  mat {mt:016x}")

    # How c20..c22 relate to the palette slot: identical rows, transposed, or
    # neither (composed with something else).
    same = transposed = other = 0
    example = None
    for n in nodes[:2000]:
        w = n[8:24]
        c = n[30:42]
        as_is = all(abs(w[r * 4 + k] - c[r * 4 + k]) < 1e-4 for r in range(3) for k in range(4))
        col = all(abs(w[k * 4 + r] - c[r * 4 + k]) < 1e-4 for r in range(3) for k in range(3)) and \
              all(abs(w[12 + r] - c[r * 4 + 3]) < 1e-4 for r in range(3))
        if as_is:
            same += 1
        elif col:
            transposed += 1
        else:
            other += 1
            if example is None:
                example = n
    print(f"\nc20..c22 against the palette slot (first {min(2000, len(nodes))} draws): "
          f"as-is {same}, transposed (rows = columns of the slot) {transposed}, neither {other}")
    if example is not None:
        print("  example palette:", [round(v, 3) for v in example[8:24]])
        print("  example c20-22 :", [round(v, 3) for v in example[30:42]])

    print(f"\ntextures: {len(texs)}")
    for t in texs[:20]:
        key, fmt, w, h, d, mips, arr, dim = t[:8]
        name = t[9].split(b"\0", 1)[0].decode("ascii", "replace")
        print(f"  {key:016x} {w}x{h}x{d} mips {mips} array {arr} fmt {fmt} {name}")


if __name__ == "__main__":
    main()
