// One level of the depth-of-field pyramid, sampled straight from the scene.
//
// The guest's chain (and the host's until 2026-09-04) built five levels each
// from the one before, a render pass per level. Adreno charges a pass
// boundary for each, so the five are now one pass into a level atlas, every
// level filtered from the scene directly with a kernel scaled to its level:
// a 5x5 grid of taps at half the level's radius, which for level k stands in
// for k+1 rounds of the dual downsample. Approximate visuals are the owner's
// call (2026-09-02).
//
// Push block: ResourceDescriptorIndex = the scene, ResourceDescriptorIndex2 =
// the level, Param0 = the blur scale (bd_host_post_blur), Param1 = the
// scene's resolve scale (0 for 1).
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
    const uint level = g_PushConstants.ResourceDescriptorIndex2;
    const float blur = max(g_PushConstants.Param0, 0.25);
    // Level k of the chain: the dual downsample's 2-texel reach at each of
    // k+1 halvings is 2^(k+1) scene texels.
    const float radius = float(2u << level) * blur;
    const float2 step = (radius * 0.5) / float2(max(w, 1u), max(h, 1u));
    float4 acc = 0.0;
    [unroll]
    for (int y = -2; y <= 2; ++y)
    {
        [unroll]
        for (int x = -2; x <= 2; ++x)
        {
            const float2 uv = texCoord + float2(x, y) * step;
            // Tent weights: the centre counts most, the ring least.
            const float wgt = (3.0 - abs(float(x))) * (3.0 - abs(float(y)));
            acc += src.SampleLevel(smp, float3(uv, float(viewId)), 0.0) * wgt;
        }
    }
    acc /= 81.0; // sum of (3-|x|)(3-|y|) over the 5x5 grid
    const float scale = g_PushConstants.Param1 > 0.0 ? g_PushConstants.Param1 : 1.0;
    return acc * scale;
}
