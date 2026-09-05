// Optical texture alpha remains coverage; authored intensity scales RGB only.
#include "src/gpu/lens_flare_uv.h"
[[vk::binding(0, 0)]] Texture2DArray<float4> images[] : register(t0, space0);
[[vk::binding(0, 1)]] SamplerState samplers[] : register(s0, space3);
float4 main(float4 position : SV_Position, float2 uv : TEXCOORD0,
            nointerpolation float4 color : COLOR0,
            nointerpolation uint image : TEXCOORD1) : SV_Target
{
    // Optical assets have one layer, shared by both multiview eyes. The target
    // is layered; the texture is not a flattened eye image.
    const float2 optical_uv = float2(LensFlareU(uv.x), LensFlareV(uv.y));
    const float4 texel = images[NonUniformResourceIndex(image)].SampleLevel(samplers[0], float3(optical_uv,0), 0);
    return float4((color.rgb * color.a) * texel.rgb, texel.a);
}
