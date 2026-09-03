// Host post chain: downsampling blur, one pass per pyramid level. The target
// is half the source. Thirteen bilinear taps (Jimenez, "Next generation
// post processing in Call of Duty", SIGGRAPH 2014): the four inner taps at
// half a target texel carry half the weight, the 3x3 outer ring at one target
// texel the other half - a downsample that blurs over a 4x4 source
// footprint as it goes. Param0 scales the tap distance (1 = as described).
//
// Replaces the guest's separate quarter copy and 13-tap blur at each level,
// and the five-tap dual filter tried first, whose levels were too sharp for
// the guest's depth-of-field composite to show any distance blur
// (2026-09-02, desktop captures).
#include "copy_common.hlsli"

[[vk::binding(0, 0)]] Texture2DArray<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);
[[vk::binding(0, 1)]] SamplerState     g_SamplerDescriptorHeap[]   : register(s0, space3);

float4 main(in float4 position : SV_Position, in float2 texCoord : TEXCOORD,
            in uint viewId : SV_ViewID) : SV_Target
{
    Texture2DArray<float4> src = g_Texture2DDescriptorHeap[g_PushConstants.ResourceDescriptorIndex];
    SamplerState smp = g_SamplerDescriptorHeap[0];
    uint w, h, layers;
    src.GetDimensions(w, h, layers);
    // One target texel is two source texels; the scale widens the kernel.
    const float2 t = (2.0 / float2(max(w, 1u), max(h, 1u))) * max(g_PushConstants.Param0, 0.25);
    const float2 uv = texCoord;

    float4 a = src.SampleLevel(smp, float3(uv + float2(-t.x, -t.y), float(viewId)), 0.0);
    float4 b = src.SampleLevel(smp, float3(uv + float2( 0.0, -t.y), float(viewId)), 0.0);
    float4 c = src.SampleLevel(smp, float3(uv + float2( t.x, -t.y), float(viewId)), 0.0);
    float4 d = src.SampleLevel(smp, float3(uv + float2(-t.x,  0.0), float(viewId)), 0.0);
    float4 e = src.SampleLevel(smp, float3(uv, float(viewId)), 0.0);
    float4 f = src.SampleLevel(smp, float3(uv + float2( t.x,  0.0), float(viewId)), 0.0);
    float4 g = src.SampleLevel(smp, float3(uv + float2(-t.x,  t.y), float(viewId)), 0.0);
    float4 hh = src.SampleLevel(smp, float3(uv + float2( 0.0,  t.y), float(viewId)), 0.0);
    float4 i = src.SampleLevel(smp, float3(uv + float2( t.x,  t.y), float(viewId)), 0.0);
    const float2 ht = t * 0.5;
    float4 j = src.SampleLevel(smp, float3(uv + float2(-ht.x, -ht.y), float(viewId)), 0.0);
    float4 k = src.SampleLevel(smp, float3(uv + float2( ht.x, -ht.y), float(viewId)), 0.0);
    float4 l = src.SampleLevel(smp, float3(uv + float2(-ht.x,  ht.y), float(viewId)), 0.0);
    float4 m = src.SampleLevel(smp, float3(uv + float2( ht.x,  ht.y), float(viewId)), 0.0);

    float4 acc = (j + k + l + m) * 0.125;
    acc += (a + b + d + e) * 0.03125;
    acc += (b + c + e + f) * 0.03125;
    acc += (d + e + g + hh) * 0.03125;
    acc += (e + f + hh + i) * 0.03125;
    // Param1: the source's resolve scale (the HDR scene aliased unscaled), 0 = 1.
    const float scale = g_PushConstants.Param1 > 0.0 ? g_PushConstants.Param1 : 1.0;
    return acc * scale;
}
