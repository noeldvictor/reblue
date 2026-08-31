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
[[vk::binding(3, 0)]] Texture2DArray<float> g_Texture2DDescriptorHeap[] : register(t0, space0);
SamplerState     g_SamplerDescriptorHeap[]   : register(s0, space3);
// Which array layer this pass reads. The heap is Texture2DArray now; these are
// mono passes over a single-layer surface, so layer 0. A multiview-aware
// present would pick the eye here instead.
// Per eye, like copy_color_ps. A hardcoded layer 0 here copies the left eye's
// depth into both layers of a multiview destination, which flattens the pair
// just as surely as a mono colour copy does.
static uint g_ViewIndex = 0;
#define BD_L0(uv) float3((uv), float(g_ViewIndex))

float main(in float4 position : SV_Position, in float2 texCoord : TEXCOORD,
           in uint viewId : SV_ViewID) : SV_Depth
{
    g_ViewIndex = viewId;

    // Sample (not Load) so dim-mismatched depth resolves (672x720 -> 1280x720) stretch instead of pasting 1:1.
    return g_Texture2DDescriptorHeap[g_PushConstants.ResourceDescriptorIndex]
        .Sample(g_SamplerDescriptorHeap[0], BD_L0(texCoord));
}
