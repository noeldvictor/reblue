/**
 * @file    gpu/host_mips.h
 * @brief   Host-generated mip chains for guest textures that ship without one.
 *
 * Two thirds of Blue Dragon's texture data - the 1024x1024 and 2048x1024
 * world textures in DXT1/3/5 - comes with no mip chain (measured 2026-09-02:
 * 43.1 M texels without against 21.0 M with), so every fragment sampled the
 * base level. The legacy upload adapter is declared here; the SDK-independent
 * cooker is in host_mips.cpp and native_texture_data.h. Native texture assets
 * persist its output and reuse it without generation on later loads.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#pragma once
#include <vector>
#include <rex/types.h>
#include "gpu/texture_upload.h"

namespace bd::gpu {

struct HostMipChain {
  // Level 0 is the caller's base; levels 1..N are owned here.
  std::vector<std::vector<u8>> storage;
  std::vector<BCMipLevel> levels;
};

// True when the format is one the generator handles (DXT1, DXT3, DXT5).
bool HostMipsSupported(u32 xe_format);

// Builds levels 1..N from an untiled base of block rows (row stride in bytes
// given), appending the base as level 0 first. Returns false with an empty
// chain when the format is unsupported or the base is too small.
bool GenerateHostMips(u32 xe_format, u32 width, u32 height,
                      const u8 *base_blocks, size_t base_size,
                      u32 base_row_bytes, u32 base_row_width_texels,
                      HostMipChain &out);

} // namespace bd::gpu
