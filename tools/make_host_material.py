"""make_host_material.py <dump stem> <out stem>

Generate src/gpu/shaders/hlsl/<out>.hlsl from a recompiled pixel shader in the
desktop build's hlsl_dump (the common header #included instead of inlined),
swap its shadow block for the host's four-gather kernel when the block is
byte-identical to bd_normal_ps's, and register the shader in
cmake/generated.cmake and guest_shaders.cpp by its guest hash.

Refuses a shader whose shadow block differs from bd_normal_ps's: those need
a per-shader decode of the kernel's registers and constant slots
(research/20260903_1230_the-host-shadow-kernel-and-what-a-host-material-is-not.md).
"""
import hashlib
import os
import re
import sys

R = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
DUMP = os.path.join(R, "out", "build", "win-amd64-release", "hlsl_dump")
HLSL = os.path.join(R, "src", "gpu", "shaders", "hlsl")
HEADER_LINES = 1 + 496  # the hash comment line, then shader_common.h inlined

# The host shadow kernel: the recompiled projection, biases and edge rule,
# four GatherRed calls instead of six fetches plus six four-load compares.
SHADOW = """\tif (g_bShadowMap)
\t{
\t\t// The host shadow kernel (2026-09-03). The guest's was six depth fetches
\t\t// and six four-load compares, thirty texture operations a fragment, on
\t\t// taps spread +-1.3/1024 of the map times g_ShadowPcfScale (the host
\t\t// holds that penumbra constant in world space, constant_buffers.h).
\t\t// Four GatherRed calls of the D32 map at the corners of a quad half that
\t\t// wide, each a bilinear compare, cover the same penumbra with sixteen
\t\t// texels for four fetches. The projection (uv from the second set, v
\t\t// flipped as D3D does), the depth-proportional and slope-scaled biases
\t\t// and the "outside the map is lit" rule are the recompiled ones; r7.y
\t\t// leaves this block as the lit fraction, which the diffuse block consumes.
\t\tps = clamp(rcp(r5.w), FLT_MIN, FLT_MAX);
\t\tr7.x = ps;
\t\tr3.yzw = r7.xxx * r5.zyx;
\t\tr7.y = saturate(dot(r8.xzy, -g_vLightDir1.zxy));
\t\tps = clamp(rcp(r6.w), FLT_MIN, FLT_MAX);
\t\tr7.x = ps;
\t\tr0.zw = r6.yx * c250.yy * r7.xx;
\t\tr3.x = c252.x - r7.y;
\t\tr5.xyzw = r3.xyzw * g_vShadowEpsilon.xxwz;
\t\tfloat shadow_ref = r7.x * r6.z - r5.y - r5.x * c251.y;
\t\tfloat2 shadow_uv = float2(c250.y + r0.w, c250.y - r0.z);
\t\tBD_TEX2D shadow_tex = g_Texture2DDescriptorHeap[ShadowTexture_Texture2DDescriptorIndex];
\t\tfloat2 shadow_dim = float2(getTexture2DDimensions(shadow_tex));
\t\tfloat shadow_o = 0.65 * c252.z; // c252.z is (1/1024) * g_ShadowPcfScale
\t\tr7.y = 0.0;
\t\t[unroll] for (int shadow_i = 0; shadow_i < 4; ++shadow_i)
\t\t{
\t\t\tfloat2 tap_uv = shadow_uv + float2((shadow_i & 1) ? shadow_o : -shadow_o, (shadow_i & 2) ? shadow_o : -shadow_o);
\t\t\tfloat4 shadow_taps = shadow_tex.GatherRed(g_SamplerDescriptorHeap[ShadowTexture_SamplerDescriptorIndex], BD_UV(tap_uv));
\t\t\tfloat4 shadow_lit = select(shadow_taps > shadow_ref.xxxx, float4(1.0, 1.0, 1.0, 1.0), float4(0.0, 0.0, 0.0, 0.0));
\t\t\tfloat2 shadow_f = frac(tap_uv * shadow_dim - 0.5);
\t\t\tr7.y += 0.25 * lerp(lerp(shadow_lit.w, shadow_lit.z, shadow_f.x), lerp(shadow_lit.x, shadow_lit.y, shadow_f.x), shadow_f.y);
\t\t}
\t\tif (any(shadow_uv < 0.0) || any(shadow_uv > 1.0))
\t\t\tr7.y = c252.x;
\t}"""


def block_span(ls):
    """The consecutive `if (g_bShadowMap)` blocks: [start, end) line indices."""
    start = next(i for i, l in enumerate(ls) if l == "\tif (g_bShadowMap)")
    i = start
    while i < len(ls) and ls[i] == "\tif (g_bShadowMap)":
        i += 1
        while i < len(ls) and ls[i] != "\t}":
            i += 1
        i += 1
    return start, i


def patch(path, old, new):
    raw = open(path, "rb").read().decode("utf-8")
    crlf = "\r\n" in raw
    t = raw.replace("\r\n", "\n")
    if new in t:
        print("already registered in", os.path.basename(path))
        return
    assert t.count(old) == 1, (path, old)
    t = t.replace(old, new)
    open(path, "wb").write((t.replace("\n", "\r\n") if crlf else t).encode("utf-8"))
    print("patched", os.path.basename(path))


def main():
    stem, out = sys.argv[1], sys.argv[2]
    lines = open(os.path.join(DUMP, stem + ".hlsl"), encoding="utf-8").read().split("\n")
    hash_hex = re.search(r"hash=0x([0-9A-F]+)", lines[0]).group(1)
    ref = open(os.path.join(DUMP, "bd_normal_ps.hlsl"), encoding="utf-8").read().split("\n")
    rs, re_ = block_span(ref)
    ref_md5 = hashlib.md5("\n".join(ref[rs:re_]).encode()).hexdigest()
    s, e = block_span(lines)
    if hashlib.md5("\n".join(lines[s:e]).encode()).hexdigest() != ref_md5:
        print("shadow block differs from bd_normal_ps; not transplanting")
        sys.exit(1)
    body = lines[HEADER_LINES:s] + SHADOW.split("\n") + lines[e:]
    head = [
        "// %s (0x%s), as a host shader substituted at link time" % (stem, hash_hex),
        "// (guest_shaders.cpp, bd_host_materials). The recompiled body (dump of",
        "// 2026-09-03) with the host's shadow kernel: four gathers instead of thirty",
        "// texture operations; the block was byte-identical to bd_normal_ps's, whose",
        "// host copy (bd_normal_lit.hlsl) explains the kernel. Everything else stays",
        "// as recompiled: uniform branches cost nothing when untaken.",
        "// Generated by tools/make_host_material.py.",
        "",
        '#include "thirdparty/XenosRecomp/XenosRecomp/shader_common.h"',
        "",
    ]
    open(os.path.join(HLSL, out + ".hlsl"), "w", encoding="utf-8", newline="\n").write(
        "\n".join(head + body))
    print("wrote", out + ".hlsl", len(head + body), "lines, hash", hash_hex)

    patch(os.path.join(R, "cmake", "generated.cmake"),
          "reblue_host_shader(bd_normal_lit ps_6_1 -D REBLUE_RECOMP)\n",
          "reblue_host_shader(bd_normal_lit ps_6_1 -D REBLUE_RECOMP)\n"
          "reblue_host_shader(%s ps_6_1 -D REBLUE_RECOMP)\n" % out)
    gs = os.path.join(R, "src", "gpu", "shaders", "guest_shaders.cpp")
    patch(gs, '#include "src/gpu/shaders/hlsl/bd_normal_lit.hlsl.dxil.h"\n',
          '#include "src/gpu/shaders/hlsl/bd_normal_lit.hlsl.dxil.h"\n'
          '#include "src/gpu/shaders/hlsl/%s.hlsl.dxil.h"\n' % out)
    patch(gs, '#include "src/gpu/shaders/hlsl/bd_normal_lit.hlsl.spirv.h"\n',
          '#include "src/gpu/shaders/hlsl/bd_normal_lit.hlsl.spirv.h"\n'
          '#include "src/gpu/shaders/hlsl/%s.hlsl.spirv.h"\n' % out)
    patch(gs, "    size = sizeof(REBLUE_BLOB_SYMBOL(bd_normal_lit));\n    return true;\n",
          "    size = sizeof(REBLUE_BLOB_SYMBOL(bd_normal_lit));\n    return true;\n"
          "  case 0x%sull: // %s\n"
          "    if (!REXCVAR_GET(bd_host_materials))\n"
          "      return false;\n"
          "    blob = REBLUE_BLOB_SYMBOL(%s);\n"
          "    size = sizeof(REBLUE_BLOB_SYMBOL(%s));\n"
          "    return true;\n" % (hash_hex, stem, out, out))


if __name__ == "__main__":
    main()
