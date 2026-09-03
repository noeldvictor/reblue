// Host post chain: box downsample. The target is the source divided by
// Param0 (2 or 4); each output texel averages the ratio x ratio block under
// it. Integer loads, so no sampler state is involved.
//
// Replaces bd_pe_ps_quoter, which drew one bilinear tap per target texel
// through the EDRAM tile and a resolve for every pyramid level (2026-09-02).
#include "copy_common.hlsli"

[[vk::binding(0, 0)]] Texture2DArray<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);

float4 main(in float4 position : SV_Position, in float2 texCoord : TEXCOORD,
            in uint viewId : SV_ViewID) : SV_Target
{
    Texture2DArray<float4> src = g_Texture2DDescriptorHeap[g_PushConstants.ResourceDescriptorIndex];
    uint w, h, layers;
    src.GetDimensions(w, h, layers);
    const int ratio = max(1, int(g_PushConstants.Param0 + 0.5));
    const int2 base = int2(position.xy) * ratio;
    float4 acc = 0.0;
    for (int y = 0; y < ratio; ++y)
    {
        for (int x = 0; x < ratio; ++x)
        {
            const int2 p = int2(min(base.x + x, int(w) - 1), min(base.y + y, int(h) - 1));
            acc += src.Load(int4(p, viewId, 0));
        }
    }
    return acc / float(ratio * ratio);
}
