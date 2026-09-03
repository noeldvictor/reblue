// Host post chain: the bloom bright mask at a quarter of the scene, in one
// pass. ResourceDescriptorIndex2 is the downsample ratio (4); Param0 is the
// threshold and Param1 the intensity, read by the host from the guest's
// pixel constant c27 at the composite draw that consumes this texture.
//
// The mask math is bd_pe_ps_brightpass verbatim (see
// bd_pe_ps_brightpass_clamp.hlsl): luma of what exceeds the threshold, scaled
// by the intensity, modulating the colour clamped to 0.25 and scaled by 4.
// Clamped to [0,1] on export like the substituted guest shader, because
// unclamped FP16 masks blur into white blobs on maps that author a large
// intensity (2026-09-02).
#include "copy_common.hlsli"

[[vk::binding(0, 0)]] Texture2DArray<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);

float4 main(in float4 position : SV_Position, in float2 texCoord : TEXCOORD,
            in uint viewId : SV_ViewID) : SV_Target
{
    Texture2DArray<float4> src = g_Texture2DDescriptorHeap[g_PushConstants.ResourceDescriptorIndex];
    uint w, h, layers;
    src.GetDimensions(w, h, layers);
    const int ratio = max(1, int(g_PushConstants.ResourceDescriptorIndex2));
    const int2 base = int2(position.xy) * ratio;
    // Average the block, as the guest's single bilinear tap at the block's
    // centre roughly did, then threshold the average.
    float3 rgb = 0.0;
    for (int y = 0; y < ratio; ++y)
    {
        for (int x = 0; x < ratio; ++x)
        {
            const int2 p = int2(min(base.x + x, int(w) - 1), min(base.y + y, int(h) - 1));
            rgb += src.Load(int4(p, viewId, 0)).xyz;
        }
    }
    rgb /= float(ratio * ratio);

    const float threshold = g_PushConstants.Param0;
    const float intensity = g_PushConstants.Param1;
    const float3 over = max(rgb - threshold, 0.0);
    const float luma = dot(over, float3(0.2125, 0.7154, 0.0722)) * intensity;
    const float3 mask = min(rgb, 0.25) * 4.0 * luma;
    return float4(saturate(mask), 1.0);
}
