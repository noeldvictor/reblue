// Native kernel and paired-atlas dependencies against separate scalar images.
#include "gpu/post_bloom.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <algorithm>
#include <cassert>
#include <limits>
#include <numbers>
#include <vector>
using namespace bd::gpu;

void Same(float a, float b) {
  assert(std::isfinite(a) && std::isfinite(b));
  assert(std::abs(a - b) <= 3e-6f * std::max(1.0f, std::abs(b)));
}

// Literal 13 samples of the Gaussian helper, including its prefactor. This
// reference neither uses the packed native kernel nor its pair normalization.
std::array<double, 13> ReferenceKernel(double sigma, double gain) {
  std::array<double, 13> result{};
  double total = 0;
  for (int i = -6; i <= 6; ++i) {
    const double w = sigma == 0 ? (i == 0 ? 1 : 0) :
        std::exp(-double(i * i) / (2 * sigma * sigma)) /
            std::sqrt(2 * std::numbers::pi * sigma * sigma);
    result[i + 6] = w;
    total += w;
  }
  for (auto &value : result) value = value * gain / total;
  return result;
}

using Image = std::vector<float>;
Image ReferenceBlur(const Image &source, int width, int height, int direction,
                    const std::array<double, 13> &kernel) {
  Image result(source.size());
  for (int eye = 0; eye < 2; ++eye) {
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        double value = 0;
        for (int tap = -6; tap <= 6; ++tap) {
          const int sx = std::clamp(x + (direction == 0 ? tap : 0), 0, width - 1);
          const int sy = std::clamp(y + (direction == 1 ? tap : 0), 0, height - 1);
          value += source[(eye * height + sy) * width + sx] * kernel[tap + 6];
        }
        result[(eye * height + y) * width + x] = float(value);
      }
    }
  }
  return result;
}

void CheckAtlas(int width, int height, uint32_t iterations) {
  const auto kernel = MakeBloomKernel(2.5f, .9f);
  const auto reference_kernel = ReferenceKernel(2.5, .9f);
  // Both directions start from the same bright image; the eyes intentionally
  // differ. NaN poison detects reads from unwritten halves, even at zero gain.
  Image bright(width * height * 2);
  for (size_t i = 0; i < bright.size(); ++i)
    bright[i] = float((i * 71 + i / width * 13) % 257) / 256.0f;
  const float poison = std::numeric_limits<float>::quiet_NaN();
  std::array<Image, 2> atlas{Image(bright.size() * 2, poison), Image(bright.size() * 2, poison)};
  for (int eye = 0; eye < 2; ++eye)
    for (int y = 0; y < height; ++y)
      for (int x = 0; x < width; ++x)
        atlas[0][(eye * height + y) * width * 2 + x] = bright[(eye * height + y) * width + x];
  for (uint32_t iteration = 0; iteration < iterations; ++iteration) {
    for (uint32_t direction = 0; direction < 2; ++direction) {
      const auto step = MakeBloomAtlasStep(iteration, direction);
      assert(step.input != step.output && step.input < 2 && step.output < 2);
      assert(step.source_half == (iteration == 0 ? 0 : direction));
      for (int eye = 0; eye < 2; ++eye) {
        for (int y = 0; y < height; ++y) {
          for (int x = 0; x < width; ++x) {
            // Mirror the shader's absolute viewport coordinate and half-local
            // texel clamp, not the independent scalar reference's indexing.
            const int px = (x + int(direction) * width) % width + int(step.source_half) * width;
            const int lo = int(step.source_half) * width;
            const auto sample = [&](int tap) {
              const int sx = std::clamp(px + (direction == 0 ? tap : 0), lo, lo + width - 1);
              const int sy = std::clamp(y + (direction == 1 ? tap : 0), 0, height - 1);
              return atlas[step.input][(eye * height + sy) * width * 2 + sx];
            };
            float value = sample(0) * kernel[0];
            for (int tap = 1; tap <= 6; ++tap)
              value += (sample(tap) + sample(-tap)) * kernel[tap];
            atlas[step.output][(eye * height + y) * width * 2 + int(direction) * width + x] = value;
          }
        }
      }
    }
  }
  for (int direction = 0; direction < 2; ++direction) {
    auto expected = bright;
    for (uint32_t iteration = 0; iteration < iterations; ++iteration)
      expected = ReferenceBlur(expected, width, height, direction, reference_kernel);
    const int half = iterations == 0 ? 0 : direction;
    for (int eye = 0; eye < 2; ++eye)
      for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
          Same(atlas[iterations & 1u][(eye * height + y) * width * 2 + half * width + x],
               expected[(eye * height + y) * width + x]);
  }
}

int main() {
  assert(!DirectionalBloom{}.enabled);
  for (float sigma : {0.0f, .001f, .25f, 1.0f, 3.0f, 6.0f, 100.0f, -3.0f, 1e30f}) {
    for (float gain : {0.0f, .25f, 1.0f, 2.0f, -1.0f}) {
      const auto actual = MakeBloomKernel(sigma, gain);
      const auto expected = ReferenceKernel(sigma, gain);
      float energy = actual[0];
      for (int i = 0; i < 7; ++i) {
        Same(actual[i], float(expected[6 + i]));
        Same(actual[i], float(expected[6 - i]));
        if (i) energy += 2 * actual[i];
      }
      Same(energy, gain);
      assert(actual[7] == 0); // explicit 32-byte shader block's unused lane
    }
  }
  for (int width : {1, 7, 33})
    for (int height : {1, 5, 31})
      for (uint32_t iterations = 0; iterations < 5; ++iterations)
        CheckAtlas(width, height, iterations);
  // A point remains on its direction's line. A separable H->V implementation
  // would light the off-axis diagonal and destroy the two-mask cross.
  Image impulse(33 * 33 * 2);
  impulse[16 * 33 + 16] = 1;
  const auto kernel = ReferenceKernel(3, 1);
  const auto horizontal = ReferenceBlur(impulse, 33, 33, 0, kernel);
  const auto vertical = ReferenceBlur(impulse, 33, 33, 1, kernel);
  assert(horizontal[16 * 33 + 17] > 0 && vertical[17 * 33 + 16] > 0);
  assert(horizontal[17 * 33 + 17] == 0 && vertical[17 * 33 + 17] == 0);
  assert(ReferenceBlur(horizontal, 33, 33, 1, kernel)[17 * 33 + 17] > 0);
}
