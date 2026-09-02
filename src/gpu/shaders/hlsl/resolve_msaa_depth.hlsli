#pragma once
#include "copy_common.hlsli"

// Plume picks the depth-plane format (R32_FLOAT_X8X24_TYPELESS) for a
// D32_FLOAT_S8_UINT MS texture, so a Texture2DMS<float> reads depth.
// Binding 3, not the default 0: the guest constant blocks took bindings 0-2
// of this set when they became dynamic uniform buffers, and the texture
// array moved up to make room. DXC maps register(t0, space0) to binding 0
// without this, so the sample silently reads a uniform buffer and returns
// black - which is exactly how the desktop present went black.
// Ignored when DXC targets DXIL, so the D3D12 path is unaffected.
[[vk::binding(0, 0)]] Texture2DMS<float, SAMPLE_COUNT> g_Texture2DMSDescriptorHeap[] : register(t0, space0);

float main(in float4 position : SV_Position, in float2 texCoord : TEXCOORD) : SV_Depth
{
    Texture2DMS<float, SAMPLE_COUNT> tex =
        g_Texture2DMSDescriptorHeap[g_PushConstants.ResourceDescriptorIndex];
    uint w, h, samples;
    tex.GetDimensions(w, h, samples);
    int2 coord = min(int2(texCoord * float2(w, h)), int2(w - 1, h - 1));
    // min() across samples = nearest-surface depth (matches UR's depth resolve
    // and is correct for the posteff DOF/fog that samples this). No scale.
    float result = tex.Load(coord, 0);
    [unroll] for (int i = 1; i < SAMPLE_COUNT; i++)
        result = min(result, tex.Load(coord, i));
    return result;
}
