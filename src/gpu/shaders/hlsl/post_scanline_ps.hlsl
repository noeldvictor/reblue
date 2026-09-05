// Native four-tap scanline filter after optical adjustments. One layered pass.
#include "copy_common.hlsli"
#include "src/gpu/post_scanline.h"

[[vk::binding(0, 0)]] Texture2DArray<float4> g_Images[] : register(t0, space0);
[[vk::binding(0, 1)]] SamplerState g_Samplers[] : register(s0, space3);

float4 main(float4 position : SV_Position, float2 uv : TEXCOORD,
            uint view_id : SV_ViewID) : SV_Target {
    Texture2DArray<float4> source = g_Images[g_PushConstants.ResourceDescriptorIndex];
    uint width, height, layers;
    source.GetDimensions(width, height, layers);
    const float strength = g_PushConstants.Param0;
    const float wave = ScanlineWave(uv.y, float(height), strength, g_PushConstants.Param1);
    const float top = ScanlineOffset(wave, strength, 235.0);
    const float right = ScanlineOffset(wave, strength, 159.0);
    const float left = ScanlineOffset(wave, strength, 33.0);
    const float bottom = ScanlineOffset(wave, strength, 87.0);
    const float2 pixel = 1.0 / float2(width, height);
    float4 a = source.SampleLevel(g_Samplers[0], float3(uv + top + float2(0, pixel.y), view_id), 0);
    float4 b = source.SampleLevel(g_Samplers[0], float3(uv + right + float2(pixel.x, 0), view_id), 0);
    float4 c = source.SampleLevel(g_Samplers[0], float3(uv + left - float2(pixel.x, 0), view_id), 0);
    float4 d = source.SampleLevel(g_Samplers[0], float3(uv + bottom - float2(0, pixel.y), view_id), 0);
    return ((c + d) + b + a) * 0.25; // filter alpha with the same four taps
}
