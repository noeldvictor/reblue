// Native grain -> discolor -> colour correction, after optical/scanline passes.
#include "copy_common.hlsli"
#include "src/gpu/post_grade.h"
[[vk::binding(0, 0)]] Texture2DArray<float4> g_Images[] : register(t0, space0);
[[vk::binding(0, 1)]] SamplerState g_Samplers[] : register(s0, space3);
[[vk::binding(1, 2)]] cbuffer GradeConstants : register(b1, space4) {
  float4 g_GainGamma;
  float4 g_BiasSaturation;
  float4 g_TargetBlend;
  float4 g_StrengthPhase;
  uint4 g_EnabledSampler;
};
float4 main(float4 position : SV_Position, float2 uv : TEXCOORD,
            uint view_id : SV_ViewID) : SV_Target {
  float2 phase = g_StrengthPhase.zw * 2.0 - 1.0;
  float2 sample_uv = uv;
  if (g_EnabledSampler.y != 0) sample_uv += phase * (.0025 * g_StrengthPhase.y);
  float4 color = g_Images[g_PushConstants.ResourceDescriptorIndex].SampleLevel(
      g_Samplers[0], float3(sample_uv, view_id), 0);
  if (g_EnabledSampler.y != 0) {
    float3 noise = g_Images[g_PushConstants.ResourceDescriptorIndex2].SampleLevel(
        g_Samplers[g_EnabledSampler.w], float3((uv + phase) * 2.2, 0), 0).rgb;
    color.rgb += (noise * .6 - color.rgb) * g_StrengthPhase.y;
  }
  GradeColor rgb = {color.r, color.g, color.b};
  if (g_EnabledSampler.x != 0) rgb = GradeDiscolor(rgb, g_StrengthPhase.x);
  if (g_EnabledSampler.z != 0) {
    GradeColor gain = {g_GainGamma.x, g_GainGamma.y, g_GainGamma.z};
    GradeColor bias = {g_BiasSaturation.x, g_BiasSaturation.y, g_BiasSaturation.z};
    GradeColor target = {g_TargetBlend.x, g_TargetBlend.y, g_TargetBlend.z};
    rgb = GradeCorrect(rgb, g_GainGamma.w, g_BiasSaturation.w, gain, bias, target, g_TargetBlend.w);
  }
  return float4(rgb.r, rgb.g, rgb.b, color.a);
}
