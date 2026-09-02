// Lens-flare sun-visibility counter (128x128 quad). [earlydepthstencil] counts only
// depth-passing fragments, so the atomic == visible sun pixels (0..16384).
// D3D12: root UAV u0/space5. Vulkan: main-layout descriptor set 3
// (kOcclusionDescriptorSetIndex; the layout note in bindless_allocator.h).
#ifdef __spirv__
[[vk::binding(0, 3)]]
#endif
RWByteAddressBuffer g_OcclusionCounter : register(u0, space5);

[earlydepthstencil]
float4 main(float4 pos : SV_Position) : SV_Target
{
    uint old;
    g_OcclusionCounter.InterlockedAdd(0, 1u, old);
    return float4(0.0, 0.0, 0.0, 0.0);
}
