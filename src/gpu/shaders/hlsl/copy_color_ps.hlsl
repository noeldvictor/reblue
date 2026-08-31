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
[[vk::binding(3, 0)]] Texture2DArray<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);
SamplerState     g_SamplerDescriptorHeap[]   : register(s0, space3);
// Which array layer this pass reads. The heap is Texture2DArray now; these are
// mono passes over a single-layer surface, so layer 0. A multiview-aware
// present would pick the eye here instead.
// The layer this invocation reads. The guest's EDRAM resolve is a full-screen
// draw through this shader, and under multiview it runs as a multiview pass -
// so each view must copy its own eye. Reading layer 0 here is what silently
// flattened the stereo pair before the post chain ever saw it: the scene array
// carried correct depth and everything downstream was mono.
//
// A pipeline with no view mask reports view 0, and a one-layer source clamps to
// layer 0 anyway, so the non-multiview path is unaffected.
static uint g_ViewIndex = 0;
#define BD_L0(uv) float3((uv), float(g_ViewIndex))

static const uint kMaxBoxTaps = 8u;

float4 main(in float4 position : SV_Position, in float2 texCoord : TEXCOORD,
            in uint viewId : SV_ViewID) : SV_Target
{
    g_ViewIndex = viewId;

    Texture2DArray<float4> tex = g_Texture2DDescriptorHeap[g_PushConstants.ResourceDescriptorIndex];
    uint ratio = (uint)(g_PushConstants.Param1 + 0.5);
    float3 rgb;
    float a;
    if (ratio >= 2u)
    {
        uint taps = min(ratio, kMaxBoxTaps);
        int2 base = int2(position.xy) * (int)ratio;
        float4 acc = 0.0;
        [loop] for (uint y = 0u; y < taps; ++y)
        {
            int oy = (int)((y * ratio + ratio / 2u) / taps);
            [loop] for (uint x = 0u; x < taps; ++x)
            {
                int ox = (int)((x * ratio + ratio / 2u) / taps);
                acc += tex.Load(int4(base.x + ox, base.y + oy, int(g_ViewIndex), 0));
            }
        }
        rgb = acc.rgb / (float)(taps * taps);
        a = acc.a / (float)(taps * taps);
    }
    else
    {
        float4 s = tex.Sample(g_SamplerDescriptorHeap[0], BD_L0(texCoord));
        rgb = s.rgb;
        a = s.a;
    }
    return float4(rgb * g_PushConstants.Param0, a);
}
