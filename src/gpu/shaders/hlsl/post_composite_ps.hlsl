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
// Native heat coordinate selection follows DoF and precedes weighted bloom.
// Its depth veto prevents foreground bleeding. Bloom stays at original UV.
// Explicit 224-byte native layout, not a console pixel-register array.
#include "copy_common.hlsli"
#include "src/gpu/post_heat.h"

[[vk::binding(0, 0)]] Texture2DArray<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);
[[vk::binding(0, 1)]] SamplerState     g_SamplerDescriptorHeap[]   : register(s0, space3);
[[vk::binding(1, 2)]] cbuffer CompositeConstants : register(b1, space4) {
    float4 g_Dof;
    float4 g_SceneWeight;
    float4 g_BloomWeight;
    uint4 g_Indices0; // depth, first three levels
    uint4 g_Indices1; // last two levels, DoF atlas, directional bloom atlas
    float4 g_Bloom; // threshold, intensity, mask mode (0 external/1 folded/2 paired/3 shared), DoF atlas
    float4 g_Rects[5];
    float4 g_Heat; // amplitudes xy, noise scale, depth exponent
    float4 g_HeatAnimation;
    uint4 g_HeatImage; // enabled, image, sampler, reserved
};

static uint g_ViewId = 0u;

float4 Tap(uint index, float2 uv)
{
    return g_Texture2DDescriptorHeap[index].SampleLevel(g_SamplerDescriptorHeap[0], float3(uv, float(g_ViewId)), 0.0);
}

float4 Bright(float3 rgb) {
    const float3 over = max(rgb - g_Bloom.x, 0.0);
    const float luma = dot(over, float3(0.2125, 0.7154, 0.0722)) * g_Bloom.y;
    return float4(saturate(min(rgb, 0.25) * 4.0 * luma), 1.0);
}

float4 DirectionalBloom(float2 uv) {
    uint width, height, layers;
    g_Texture2DDescriptorHeap[g_Indices1.w].GetDimensions(width, height, layers);
    const float2 inset = 0.5 / float2(width, height);
    const float2 first = clamp(float2(uv.x * 0.5, uv.y), inset, float2(0.5, 1) - inset);
    const float2 second = first + float2(g_Bloom.z < 2.5 ? 0.5 : 0, 0);
    // Each original mask has weight 1; g_BloomWeight carries their sum (2).
    return (Tap(g_Indices1.w, first) + Tap(g_Indices1.w, second)) * 0.5;
}

// A dof level: its own texture, or its rect of the level atlas,
// the tap kept half a texel inside the rect so levels do not bleed.
float4 TapLevel(uint i, uint level_index, float2 uv)
{
    if (g_Bloom.w > 0.5)
    {
        const uint atlas = g_Indices1.z;
        const float4 r = g_Rects[i];
        uint w, h, layers;
        g_Texture2DDescriptorHeap[atlas].GetDimensions(w, h, layers);
        const float2 inset = 0.5 / float2(max(w, 1u), max(h, 1u));
        const float2 t = clamp(r.xy + uv * r.zw, r.xy + inset, r.xy + r.zw - inset);
        return Tap(atlas, t);
    }
    return Tap(level_index, uv);
}

float2 HeatSceneUV(float2 uv) {
    if (g_HeatImage.x == 0) return uv;
    float2 noise_sum = 0;
    [unroll] for (uint i = 0; i < 4; ++i) {
        const HeatUV noise_uv = HeatShimmerNoiseUV(uv.x, uv.y, g_Heat.z, g_HeatAnimation.x, float(i));
        noise_sum += g_Texture2DDescriptorHeap[g_HeatImage.y].SampleLevel(
            g_SamplerDescriptorHeap[g_HeatImage.z], float3(noise_uv.u, noise_uv.v, 0), 0).xy;
    }
    const float original_depth = Tap(g_Indices0.x, uv).x;
    const HeatUV displaced = HeatShimmerDisplace(uv.x, uv.y, noise_sum.x, noise_sum.y,
        HeatShimmerDepthWeight(original_depth, g_Heat.w), g_Heat.x, g_Heat.y);
    const float2 displaced_uv = float2(displaced.u, displaced.v);
    return HeatShimmerAcceptDepth(original_depth, Tap(g_Indices0.x, displaced_uv).x)
        ? displaced_uv : uv;
}

float4 main(in float4 position : SV_Position, in float2 texCoord : TEXCOORD,
            in uint viewId : SV_ViewID) : SV_Target
{
    g_ViewId = viewId;
    const float dofX = g_Dof.x;
    const float dofY = g_Dof.y;
    const float focus = g_Dof.z;
    const uint depthIdx = g_Indices0.x;
    const uint level[5] = {g_Indices0.y, g_Indices0.z, g_Indices0.w, g_Indices1.x, g_Indices1.y};
    const float2 scene_uv = HeatSceneUV(texCoord);

    // g_Dof.w: the scene's resolve scale when it is an alias of the
    // unscaled surface, 0 = 1.
    const float sceneScale = g_Dof.w > 0.0 ? g_Dof.w : 1.0;
    const float4 scene = Tap(g_PushConstants.ResourceDescriptorIndex, scene_uv) * sceneScale;
    const float depth = Tap(depthIdx, scene_uv).x;

    // The guest's pow(x, 1/8) through log2/exp2, with its clamps.
    const float a = 1.0 - exp2(clamp(log2(abs(1.0 - focus)), -126.0, 126.0) * 0.125);
    const float b = 1.0 - exp2(clamp(log2(abs(1.0 - depth)), -126.0, 126.0) * 0.125);
    const float denom = max(dofX * a * b, 1e-20);
    const float lvl = 711.11 * (dofY * dofY * abs(b - a)) / denom;

    float4 dof;
    if (lvl < 1.0)
    {
        dof = lerp(scene, TapLevel(0, level[0], scene_uv), lvl);
    }
    else
    {
        const float l2 = log2(lvl);
        if (l2 < 1.0)
            dof = lerp(TapLevel(0, level[0], scene_uv), TapLevel(1, level[1], scene_uv), l2);
        else if (l2 < 2.0)
            dof = lerp(TapLevel(1, level[1], scene_uv), TapLevel(2, level[2], scene_uv), frac(l2));
        else if (l2 < 3.0)
            dof = lerp(TapLevel(2, level[2], scene_uv), TapLevel(3, level[3], scene_uv), frac(l2));
        else if (l2 < 4.0)
            dof = lerp(TapLevel(3, level[3], scene_uv), TapLevel(4, level[4], scene_uv), frac(l2));
        else
            dof = TapLevel(4, level[4], scene_uv);
    }

    // Quarter-size native preparation is before heat and weighted composition.
    // Its caller supplies disabled heat; no full-size intermediate is needed.
    if (g_PushConstants.Param1 == 1.0)
        return Bright(dof.xyz);

    // Bloom: independent directional masks, external mask, or folded bright level.
    float4 bloom;
    if (g_Bloom.z > 1.5)
    {
        bloom = DirectionalBloom(texCoord);
    }
    else if (g_Bloom.z > 0.5)
    {
        const float3 rgb = TapLevel(2, level[2], texCoord).xyz;
        bloom = Bright(rgb);
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
        return saturate(scene * g_SceneWeight);
    return saturate(dof * g_SceneWeight + bloom * g_BloomWeight);
}
