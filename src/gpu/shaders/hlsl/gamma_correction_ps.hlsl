#include "copy_common.hlsli"

// Binding 3, not the default 0: the guest constant blocks took bindings 0-2
// of this set when they became dynamic uniform buffers, and the texture
// array moved up to make room. DXC maps register(t0, space0) to binding 0
// without this, so the sample silently reads a uniform buffer and returns
// black - which is exactly how the desktop present went black.
// Ignored when DXC targets DXIL, so the D3D12 path is unaffected.
[[vk::binding(3, 0)]] Texture2D<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);
SamplerState     g_SamplerDescriptorHeap[]   : register(s0, space3);

float4 main(in float4 position : SV_Position, in float2 texCoord : TEXCOORD) : SV_Target
{
    float4 sampled = g_Texture2DDescriptorHeap[g_PushConstants.ResourceDescriptorIndex]
                       .Sample(g_SamplerDescriptorHeap[0], texCoord);
    // max() guards pow() against NaN on negative HDR values (source RT may be R16F).
    float3 ramp = pow(max(sampled.rgb, 0.0), g_PushConstants.Param0);
    // X360 scanout: the system reshapes the app ramp for the display before upload
    // (D3D::GammaRampCorrected 0x8246F538): for display-gamma type 2 (TV) the
    // hardware LUT is Rec709_encode(sRGB_decode(app_ramp)). Xenia measurably
    // displays the UNCORRECTED front buffer (its DC LUT stays linear), so
    // Param1 (display correction strength) blends between the two references.
    float3 lin = lerp(ramp / 12.92, pow((ramp + 0.055) / 1.055, 2.4),
                      step(0.04045, ramp));
    float3 corrected = lerp(lin * 4.5, 1.099 * pow(lin, 0.45) - 0.099,
                            step(0.018, lin));
    sampled.rgb = lerp(ramp, corrected, g_PushConstants.Param1);
    return float4(sampled.rgb, 1.0);
}
