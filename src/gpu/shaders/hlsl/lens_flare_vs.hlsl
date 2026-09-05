// Native optical sprites: one instanced draw, no vertex buffer or register file.
struct Sprite { float4 rect; float4 color; uint4 image; };
[[vk::binding(0, 2)]] cbuffer LensParameters : register(b0, space4) { Sprite sprites[15]; };
struct Output {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    nointerpolation float4 color : COLOR0;
    nointerpolation uint image : TEXCOORD1;
};
Output main(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
    static const float2 corners[6] = {
        float2(0,0), float2(1,0), float2(0,1),
        float2(0,1), float2(1,0), float2(1,1)
    };
    const Sprite sprite = sprites[instanceId];
    Output output;
    output.uv = corners[vertexId];
    const float2 screen = sprite.rect.xy + output.uv * sprite.rect.zw;
    output.position = float4(screen * float2(2,-2) + float2(-1,1), 0, 1);
    output.color = sprite.color;
    output.image = sprite.image.x;
    return output;
}
