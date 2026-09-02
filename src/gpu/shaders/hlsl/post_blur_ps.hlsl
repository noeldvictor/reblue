// Host post chain: one direction of a separable 9-tap gaussian. Param0/Param1
// are the step in texels (1,0) or (0,1); the pass runs twice, through a
// scratch texture, for a full blur.
//
// Replaces bd_pe_ps_ms_weight, a 13-tap weighted gather the guest ran once
// per pyramid level through the tile and a resolve (2026-09-02). The kernel
// is wider and cheaper here; the levels it blurs are 1/2 to 1/16 of the scene.
#include "copy_common.hlsli"

[[vk::binding(0, 0)]] Texture2DArray<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);

static const float kWeights[5] = { 0.227027, 0.1945946, 0.1216216, 0.054054, 0.016216 };

float4 main(in float4 position : SV_Position, in float2 texCoord : TEXCOORD) : SV_Target
{
    Texture2DArray<float4> src = g_Texture2DDescriptorHeap[g_PushConstants.ResourceDescriptorIndex];
    uint w, h, layers;
    src.GetDimensions(w, h, layers);
    const int2 limit = int2(int(w) - 1, int(h) - 1);
    const int2 p = int2(position.xy);
    const int2 dir = int2(int(round(g_PushConstants.Param0)), int(round(g_PushConstants.Param1)));
    float4 acc = src.Load(int4(min(p, limit), 0, 0)) * kWeights[0];
    [unroll]
    for (int i = 1; i < 5; ++i)
    {
        const int2 a = clamp(p + dir * i, int2(0, 0), limit);
        const int2 b = clamp(p - dir * i, int2(0, 0), limit);
        acc += (src.Load(int4(a, 0, 0)) + src.Load(int4(b, 0, 0))) * kWeights[i];
    }
    return acc;
}
