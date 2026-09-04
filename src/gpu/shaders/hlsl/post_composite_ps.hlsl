// Host post chain: the depth-of-field and bloom composite in one full-screen
// pass, replacing the guest's two (bd_pe_ps_dof, then a resolve, then
// bd_pe_ps_ms_tex). Written from the recompiled bd_pe_ps_dof (2026-09-02):
//
//   a = 1 - (1 - focus)^(1/8),  b = 1 - (1 - depth)^(1/8)
//   level = 711.11 * dofY^2 * |b - a| / (dofX * a * b)
//   level < 1  : lerp(scene, L1, level)
//   1 ..  2    : lerp(L1, L2, log2 level)
//   2 ..  4    : lerp(L2, L3, frac(log2 level))     ... up to L5, then L5
//
// and bd_pe_ps_ms_tex: saturate(w0 * that + w1 * bloom). The guest's
// parameters come through the host-filled pixel constant block:
//   g_PSC[0] = (dofX, dofY, focus, 0)   the dof draw's c27.x, .y, .w
//   g_PSC[1] = w0, g_PSC[2] = w1        the ms_tex draw's c13, c14
//   g_PSC[3] = (depth, L1, L2, L3)      bindless indices as uint bits
//   g_PSC[4] = (L4, L5, 0, 0)
// Scene and bloom are the push block's two indices.
#include "copy_common.hlsli"

[[vk::binding(0, 0)]] Texture2DArray<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);
[[vk::binding(0, 1)]] SamplerState     g_SamplerDescriptorHeap[]   : register(s0, space3);
[[vk::binding(1, 2)]] cbuffer PixelShaderConstantsBuf : register(b1, space4) { float4 g_PSC[224]; };

static uint g_ViewId = 0u;

float4 Tap(uint index, float2 uv)
{
    return g_Texture2DDescriptorHeap[index].SampleLevel(g_SamplerDescriptorHeap[0], float3(uv, float(g_ViewId)), 0.0);
}

float4 main(in float4 position : SV_Position, in float2 texCoord : TEXCOORD,
            in uint viewId : SV_ViewID) : SV_Target
{
    g_ViewId = viewId;
    const float dofX = g_PSC[0].x;
    const float dofY = g_PSC[0].y;
    const float focus = g_PSC[0].z;
    const uint depthIdx = asuint(g_PSC[3].x);
    const uint level[5] = { asuint(g_PSC[3].y), asuint(g_PSC[3].z), asuint(g_PSC[3].w),
                            asuint(g_PSC[4].x), asuint(g_PSC[4].y) };

    // g_PSC[0].w: the scene's resolve scale when it is an alias of the
    // unscaled surface, 0 = 1.
    const float sceneScale = g_PSC[0].w > 0.0 ? g_PSC[0].w : 1.0;
    const float4 scene = Tap(g_PushConstants.ResourceDescriptorIndex, texCoord) * sceneScale;
    const float depth = Tap(depthIdx, texCoord).x;

    // The guest's pow(x, 1/8) through log2/exp2, with its clamps.
    const float a = 1.0 - exp2(clamp(log2(abs(1.0 - focus)), -126.0, 126.0) * 0.125);
    const float b = 1.0 - exp2(clamp(log2(abs(1.0 - depth)), -126.0, 126.0) * 0.125);
    const float denom = max(dofX * a * b, 1e-20);
    const float lvl = 711.11 * (dofY * dofY * abs(b - a)) / denom;

    float4 dof;
    if (lvl < 1.0)
    {
        dof = lerp(scene, Tap(level[0], texCoord), lvl);
    }
    else
    {
        const float l2 = log2(lvl);
        if (l2 < 1.0)
            dof = lerp(Tap(level[0], texCoord), Tap(level[1], texCoord), l2);
        else if (l2 < 2.0)
            dof = lerp(Tap(level[1], texCoord), Tap(level[2], texCoord), frac(l2));
        else if (l2 < 3.0)
            dof = lerp(Tap(level[2], texCoord), Tap(level[3], texCoord), frac(l2));
        else if (l2 < 4.0)
            dof = lerp(Tap(level[3], texCoord), Tap(level[4], texCoord), frac(l2));
        else
            dof = Tap(level[4], texCoord);
    }

    // Bloom: the mask texture, or (folded) the bright pass of dof level 2.
    float4 bloom;
    if (g_PSC[5].z > 0.5)
    {
        const float3 rgb = Tap(level[2], texCoord).xyz;
        const float threshold = g_PSC[5].x;
        const float intensity = g_PSC[5].y;
        const float3 over = max(rgb - threshold, 0.0);
        const float luma = dot(over, float3(0.2125, 0.7154, 0.0722)) * intensity;
        bloom = float4(saturate(min(rgb, 0.25) * 4.0 * luma), 1.0);
    }
    else
    {
        bloom = Tap(g_PushConstants.ResourceDescriptorIndex2, texCoord);
    }
    // Param0: 1 shows depth, 2 shows the level over 8, 3 the scene alone.
    const int debug = int(g_PushConstants.Param0 + 0.5);
    if (debug == 1)
        return float4(depth.xxx, 1.0);
    if (debug == 2)
        return float4(saturate(lvl / 8.0).xxx, 1.0);
    if (debug == 3)
        return saturate(scene * g_PSC[1]);
    return saturate(dof * g_PSC[1] + bloom * g_PSC[2]);
}
