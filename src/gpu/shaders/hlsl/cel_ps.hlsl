#include "copy_common.hlsli"

// Cel shading, applied at present over the finished frame.
//
// Deliberately a post-process rather than a change to the guest's material
// shaders. Blue Dragon already ships toon shaders - bd_toon_ps and friends - so
// the characters are lit with a ramp; what the Toriyama art actually wants on
// top is ink lines and flatter colour, and doing that at present means it works
// on every material the game has without touching XenosRecomp, without a shader
// cache rebuild, and without knowing which draws are characters.
//
// It costs one full-screen pass, and it replaces the gamma pass rather than
// adding to it - the gamma maths below is the same as gamma_correction_ps, so
// the pipeline swaps in where that one binds and nothing else in present moves.
//
// Param0/Param1 keep their present-pass meaning (guest ramp exponent, display
// correction strength). The cel constants are compile-time: the push constant
// block has no room left, and the useful range is narrow enough that a cvar
// per knob would be more dial than anyone wants before seeing it in a headset.

Texture2D<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);
SamplerState     g_SamplerDescriptorHeap[]   : register(s0, space3);

// Luminance bands. Four reads as animation cel, eight is closer to the game's
// own toon ramp and much more forgiving of gradients.
static const float kBands = 6.0;
// How far to pull each pixel toward its band. 1.0 is full posterisation, which
// bands the sky badly; this keeps some gradient.
static const float kBandStrength = 0.55;
// Ink line darkness at a full-strength edge.
static const float kInkStrength = 0.85;
// Luminance gradient that counts as a full-strength edge. Lower catches more
// interior detail and starts drawing lines on textures rather than on shapes.
static const float kEdgeThreshold = 0.28;

float Luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }

float4 main(in float4 position : SV_Position, in float2 texCoord : TEXCOORD) : SV_Target
{
    Texture2D<float4> tex = g_Texture2DDescriptorHeap[g_PushConstants.ResourceDescriptorIndex];
    SamplerState samp = g_SamplerDescriptorHeap[0];

    float4 sampled = tex.Sample(samp, texCoord);

    uint w, h;
    tex.GetDimensions(w, h);
    const float2 texel = 1.0 / float2(max(w, 1u), max(h, 1u));

    // Roberts cross on luminance: two diagonal differences, four taps. A Sobel
    // is nine taps for a line that is barely different once it has been
    // thresholded, and this pass runs over every pixel of a fill-bound frame.
    const float l00 = Luma(tex.Sample(samp, texCoord).rgb);
    const float l10 = Luma(tex.Sample(samp, texCoord + float2(texel.x, 0.0)).rgb);
    const float l01 = Luma(tex.Sample(samp, texCoord + float2(0.0, texel.y)).rgb);
    const float l11 = Luma(tex.Sample(samp, texCoord + texel).rgb);
    const float edge = saturate((abs(l00 - l11) + abs(l10 - l01)) / kEdgeThreshold);

    // Posterise toward the band, preserving hue by scaling rather than
    // quantising each channel - per-channel banding shifts colours and looks
    // like a broken palette rather than like cel animation.
    const float luma = max(Luma(sampled.rgb), 1e-4);
    const float banded = floor(luma * kBands + 0.5) / kBands;
    sampled.rgb *= lerp(1.0, banded / luma, kBandStrength);

    // Ink last, so the lines are not themselves posterised.
    sampled.rgb *= 1.0 - edge * kInkStrength;

    // --- unchanged from gamma_correction_ps below this line ---
    float3 ramp = pow(max(sampled.rgb, 0.0), g_PushConstants.Param0);
    float3 lin = lerp(ramp / 12.92, pow((ramp + 0.055) / 1.055, 2.4),
                      step(0.04045, ramp));
    float3 corrected = lerp(lin * 4.5, 1.099 * pow(lin, 0.45) - 0.099,
                            step(0.018, lin));
    sampled.rgb = lerp(ramp, corrected, g_PushConstants.Param1);
    return float4(sampled.rgb, 1.0);
}
