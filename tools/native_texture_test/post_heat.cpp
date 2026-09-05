// Original heat shader's literal math, coordinate ordering and depth veto.
#include "gpu/post_heat.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <limits>
using namespace bd::gpu;

float Literal(uint32_t bits) { return std::bit_cast<float>(bits); }
float ReferencePower(float value, float exponent) {
  return std::exp2(std::clamp(std::log2(std::abs(value)),
      std::numeric_limits<float>::lowest(), std::numeric_limits<float>::max()) * exponent);
}
void Same(float a, float b) {
  assert(a == b || (std::isfinite(a) && std::isfinite(b) &&
      std::abs(a-b) <= 2e-5f * std::max(1.0f, std::abs(b))));
}
int main() {
  assert(!HeatShimmerParameters{}.enabled);
  // Reference follows the original y/x register swizzles and literal bits,
  // not a call to the native UV helper. All four noise coordinates are checked.
  for (uint32_t sample = 0; sample < 4096; ++sample) {
    const float phase = float(sample) * .017f - 4.0f;
    const float scale = float(sample % 9) * .25f;
    const float u = float(sample % 127) / 126.0f;
    const float v = float(sample % 53) / 52.0f;
    for (uint32_t tap = 0; tap < 4; ++tap) {
      const std::array<float, 2> weights{phase*Literal(0x3fc90e56), phase*Literal(0x3e4ccccd)};
      const std::array<float, 2> offsets{float(tap)*Literal(0x3f400000), float(tap)*Literal(0x3ca3d70a)};
      const std::array<float, 2> yx{weights[1]+offsets[1], weights[0]+offsets[0]};
      const float radians = (yx[1]*Literal(0x3e22f983)+Literal(0x3f000000));
      const float wrapped = (radians-std::floor(radians))*Literal(0x40c90fdb)+Literal(0xc0490fdb);
      const float horizontal = std::sin(wrapped)*Literal(0x3c23d70a)+u;
      const HeatUV actual = HeatShimmerNoiseUV(u,v,scale,phase,float(tap));
      Same(actual.u, (horizontal*Literal(0x40400000))*scale);
      Same(actual.v, (yx[0]+v)*scale);
    }
  }
  for (float exponent : {0.0f,.5f,1.0f,5.0f}) {
    for (uint32_t i = 0; i <= 512; ++i) {
      const float depth = float(i)/512;
      const float weight = HeatShimmerDepthWeight(depth,exponent);
      Same(weight, ReferencePower(1-ReferencePower(1-depth,Literal(0x3e4ccccd)),exponent));
      for (const auto sum : {std::array<float,2>{0,4}, {2,2}, {3,1}}) {
        const auto displaced = HeatShimmerDisplace(.4f,.6f,sum[0],sum[1],weight,.03f,-.02f);
        // Sum is swapped twice by the original shader; XY amplitudes stay XY.
        const std::array<float,2> r1{sum[1]*.5f-1, sum[0]*.5f-1};
        Same(displaced.u, (r1[1]*weight)*.03f+.4f);
        Same(displaced.v, (r1[0]*weight)*-.02f+.6f);
      }
    }
  }
  assert(HeatShimmerDepthWeight(0,5)==0);
  assert(HeatShimmerDepthWeight(0,0)==1);
  assert(HeatShimmerDepthWeight(1,5)==1);
  assert(HeatShimmerAcceptDepth(.5f,.5f));
  assert(HeatShimmerAcceptDepth(.5f,std::nextafter(.5f,1.0f)));
  assert(!HeatShimmerAcceptDepth(.5f,std::nextafter(.5f,0.0f)));
  const float nan = std::numeric_limits<float>::quiet_NaN();
  assert(!HeatShimmerAcceptDepth(nan,.5f));
  assert(!HeatShimmerAcceptDepth(.5f,nan));
  for (uint32_t frame : {0u,1u,300u,65535u,UINT32_MAX}) {
    const float left = HeatShimmerFramePhase(frame), right = HeatShimmerFramePhase(frame);
    assert(std::isfinite(left) && left==right);
  }
  assert(HeatShimmerFramePhase(301)>HeatShimmerFramePhase(300));
}
