// The occlusion proxy: a view-space cube bounding a node's sphere, drawn
// under an occlusion query at the end of the scene pass (gpu/occlusion_cull.h).
// No vertex buffers: SV_VertexID picks the cube corner, SV_InstanceID the
// sphere. The block is bound through the guest's vertex constant window:
// registers 0-3 are the projection rows (clip = P * v, the guest's own
// c32-35 of the scene pass), registers 4.. one sphere each (xyz centre in
// view space, w radius).
#include "thirdparty/XenosRecomp/XenosRecomp/shader_common.h"

#ifndef __spirv__
uint g_SpecConstants() { return 0u; }
#endif

static const float3 kCubeCorners[8] = {
    float3(-1, -1, -1), float3(1, -1, -1), float3(1, 1, -1), float3(-1, 1, -1),
    float3(-1, -1, 1),  float3(1, -1, 1),  float3(1, 1, 1),  float3(-1, 1, 1)};
// 12 triangles, both windings drawn (cull off), so the order only has to
// cover every face.
static const uint kCubeIndices[36] = {
    0, 1, 2, 0, 2, 3, 4, 6, 5, 4, 7, 6, 0, 4, 5, 0, 5, 1,
    2, 6, 7, 2, 7, 3, 1, 5, 6, 1, 6, 2, 0, 3, 7, 0, 7, 4};

void main(in uint vertexId : SV_VertexID, in uint instanceId : SV_InstanceID,
          out float4 oPos : SV_Position)
{
    const float4 sphere = g_VSC[4u + instanceId];
    // A margin over the sphere: the proxy must be conservative, and under
    // multiview the eye skew is not applied here.
    const float3 corner = sphere.xyz + kCubeCorners[kCubeIndices[vertexId % 36u]] * (sphere.w * 1.15);
    const float4 p = float4(corner, 1.0);
    oPos.x = dot(g_VSC[0], p);
    oPos.y = dot(g_VSC[1], p);
    oPos.z = dot(g_VSC[2], p);
    oPos.w = dot(g_VSC[3], p);
}
