// Native fisheye then colour inversion. One layered pass, explicit input;
// no engine registers, depth binding, intermediate console target or resolve.
#include "copy_common.hlsli"
#include "src/gpu/post_adjustments.h"

[[vk::binding(0, 0)]] Texture2DArray<float4> g_Images[] : register(t0, space0);
[[vk::binding(0, 1)]] SamplerState g_Samplers[] : register(s0, space3);

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD,
            uint view_id : SV_ViewID) : SV_Target {
    Texture2DArray<float4> source = g_Images[g_PushConstants.ResourceDescriptorIndex];
    uint width, height, layers;
    source.GetDimensions(width, height, layers);
    const float2 delta = uv - 0.5;
    const float radius = length(delta * float2(1.0, float(height) / float(width)));
    const float2 sample_uv = uv + delta * FisheyeOffsetScale(radius, g_PushConstants.Param0);
    float4 color = source.SampleLevel(g_Samplers[0], float3(sample_uv, view_id), 0);
    // This pass's second push lane is a float pivot, not another image index.
    const float pivot = asfloat(g_PushConstants.ResourceDescriptorIndex2);
    color.r = ReverseColor(color.r, g_PushConstants.Param1, pivot);
    color.g = ReverseColor(color.g, g_PushConstants.Param1, pivot);
    color.b = ReverseColor(color.b, g_PushConstants.Param1, pivot);
    return color; // original alpha survives both effects
}
