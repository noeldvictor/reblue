#include "copy_common.hlsli"

// Binding 3, not the default 0: the guest constant blocks took bindings 0-2
// of this set when they became dynamic uniform buffers, and the texture
// array moved up to make room. DXC maps register(t0, space0) to binding 0
// without this, so the sample silently reads a uniform buffer and returns
// black - which is exactly how the desktop present went black.
// Ignored when DXC targets DXIL, so the D3D12 path is unaffected.
// The bindless 2D heap is declared Texture2DArray under Vulkan so a multiview
// target can be sampled per eye without being flattened first. Every descriptor
// in it is an array view, so this host shader has to agree on the type. Layer 0
// unless the shader has a reason to pick another - these are mono passes.
[[vk::binding(0, 0)]] Texture2DArray<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);
[[vk::binding(0, 1)]] SamplerState     g_SamplerDescriptorHeap[]   : register(s0, space3);
// Present flattens the multiview pair here instead of in a separate pass.
//
// ResourceDescriptorIndex2 is 0 for an ordinary single-layer source and 1 when
// the source is a two-layer multiview target. In that case the output is a
// side-by-side pair: the left half of the screen reads array layer 0 and the
// right half layer 1, each stretched back to full width.
//
// This is what replaced the resolve chain. Flattening used to need its own
// full-resolution render pass per layered surface - five of them in a field
// frame, 79.5 MB of tile traffic on a Quest 2 - because the bindless heap was
// declared Texture2D and nothing downstream could read an array at all. With an
// array heap the scene is sampled directly and the flatten costs one branch in
// the blit that was already happening.
// ResourceDescriptorIndex2 == 3 is the layered swapchain: the pass itself has
// two views, so each output layer reads the source layer of the same index and
// the flatten does not happen at all. That is what the compositor wants (one
// projection view an array layer) and what XR_FB_foveation attaches to.
float4 main(in float4 position : SV_Position, in float2 texCoord : TEXCOORD,
            in uint viewId : SV_ViewID) : SV_Target
{
    float3 srcCoord = float3(texCoord, 0.0);
    if (g_PushConstants.ResourceDescriptorIndex2 == 3)
    {
        srcCoord = float3(texCoord, float(viewId));
    }
    else if (g_PushConstants.ResourceDescriptorIndex2 != 0)
    {
        const float eye = texCoord.x < 0.5 ? 0.0 : 1.0;
        srcCoord = float3(saturate(texCoord.x * 2.0 - eye), texCoord.y, eye);
    }

    float4 sampled = g_Texture2DDescriptorHeap[g_PushConstants.ResourceDescriptorIndex]
                       .Sample(g_SamplerDescriptorHeap[0], srcCoord);

    // Probe (ResourceDescriptorIndex2 == 2): show |layer1 - layer0| amplified.
    // Two fixes for "present emits identical halves" were guesses and both
    // missed, so this answers the actual question - does the surface present
    // samples have two different layers at all? Black means it does not, and
    // the flatten is innocent.
    if (g_PushConstants.ResourceDescriptorIndex2 == 2)
    {
        const float3 l0 = g_Texture2DDescriptorHeap[g_PushConstants.ResourceDescriptorIndex]
                            .Sample(g_SamplerDescriptorHeap[0], float3(texCoord, 0.0)).rgb;
        const float3 l1 = g_Texture2DDescriptorHeap[g_PushConstants.ResourceDescriptorIndex]
                            .Sample(g_SamplerDescriptorHeap[0], float3(texCoord, 1.0)).rgb;
        return float4(saturate(abs(l1 - l0) * 8.0), 1.0);
    }
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
