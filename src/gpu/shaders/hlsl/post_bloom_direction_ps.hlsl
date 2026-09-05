// Two independent 13-tap directions packed side by side in a native atlas.
// A pair of viewports shares one render pass; SV_ViewID selects the eye.
#include "copy_common.hlsli"
[[vk::binding(0, 0)]] Texture2DArray<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);
[[vk::binding(1, 2)]] cbuffer BloomKernel : register(b1, space4) {
    float4 g_Weights0;
    float4 g_Weights1;
};
float Weight(uint i) { return i < 4 ? g_Weights0[i] : g_Weights1[i - 4]; }
float4 main(in float4 position : SV_Position, in float2 texCoord : TEXCOORD,
            in uint viewId : SV_ViewID) : SV_Target {
    Texture2DArray<float4> src = g_Texture2DDescriptorHeap[g_PushConstants.ResourceDescriptorIndex];
    uint width, height, layers;
    src.GetDimensions(width, height, layers);
    const int half_width = int(width / 2);
    const int origin = int(g_PushConstants.ResourceDescriptorIndex2) * half_width;
    const int2 lo = int2(origin, 0), hi = int2(origin + half_width - 1, int(height) - 1);
    const int2 p = int2(int(position.x) % half_width + origin, int(position.y));
    const int2 direction = g_PushConstants.Param0 == 0 ? int2(1, 0) : int2(0, 1);
    float3 color = src.Load(int4(clamp(p, lo, hi), viewId, 0)).rgb * Weight(0);
    [unroll] for (uint i = 1; i <= 6; ++i) {
        color += (src.Load(int4(clamp(p + direction * int(i), lo, hi), viewId, 0)).rgb +
                  src.Load(int4(clamp(p - direction * int(i), lo, hi), viewId, 0)).rgb) * Weight(i);
    }
    return float4(color, 1);
}
