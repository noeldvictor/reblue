/**
 * @file    gpu/host_mips.cpp
 * @brief   SDK-independent native BC1/2/3 mip cooker; see native_texture_data.h.
 *
 * The block decoders are written here; the encoders are stb_dxt for the
 * colour and DXT5 alpha blocks, with two cases stb does not cover done by
 * hand: DXT1's punch-through alpha (the 3-colour mode, index 3 transparent -
 * foliage and fences use it, and dropping it would fill their cutouts), and
 * DXT3's explicit 4-bit alpha block.
 *
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/scene/native_texture_data.h"

#include <algorithm>
#include <cstring>


#define STB_DXT_IMPLEMENTATION
#define STB_DXT_STATIC
#include <stb_dxt.h>

namespace bd::gpu::scene {
using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
namespace {

constexpr u32 kDxt1 = u32(NativeTextureFormat::BC1);
constexpr u32 kDxt3 = u32(NativeTextureFormat::BC2);
constexpr u32 kDxt5 = u32(NativeTextureFormat::BC3);

u32 BlockBytes(u32 format) { return format == kDxt1 ? 8u : 16u; }

void Rgb565(u16 c, u8 out[3]) {
  const u32 r = (c >> 11) & 31u, g = (c >> 5) & 63u, b = c & 31u;
  out[0] = u8((r << 3) | (r >> 2));
  out[1] = u8((g << 2) | (g >> 4));
  out[2] = u8((b << 3) | (b >> 2));
}

// A 4x4 block of RGBA8, row-major, from a DXT1 colour block; with_alpha
// honours the 3-colour mode's transparent index.
void DecodeColorBlock(const u8 *b, u8 *rgba, bool with_alpha) {
  const u16 c0 = u16(b[0] | (b[1] << 8));
  const u16 c1 = u16(b[2] | (b[3] << 8));
  u8 pal[4][4];
  Rgb565(c0, pal[0]);
  Rgb565(c1, pal[1]);
  pal[0][3] = pal[1][3] = 255;
  const bool four = !with_alpha || c0 > c1;
  for (int k = 0; k < 3; ++k) {
    if (four) {
      pal[2][k] = u8((2 * pal[0][k] + pal[1][k] + 1) / 3);
      pal[3][k] = u8((pal[0][k] + 2 * pal[1][k] + 1) / 3);
    } else {
      pal[2][k] = u8((pal[0][k] + pal[1][k]) / 2);
      pal[3][k] = 0;
    }
  }
  pal[2][3] = 255;
  pal[3][3] = four ? 255 : 0;
  const u32 bits = u32(b[4]) | (u32(b[5]) << 8) | (u32(b[6]) << 16) | (u32(b[7]) << 24);
  for (int i = 0; i < 16; ++i) {
    const u32 idx = (bits >> (2 * i)) & 3u;
    std::memcpy(rgba + i * 4, pal[idx], 4);
  }
}

void DecodeDxt3Alpha(const u8 *a, u8 *rgba) {
  for (int i = 0; i < 16; ++i) {
    const u32 nib = (a[i / 2] >> ((i & 1) * 4)) & 15u;
    rgba[i * 4 + 3] = u8(nib * 17u);
  }
}

void DecodeDxt5Alpha(const u8 *a, u8 *rgba) {
  u8 pal[8];
  pal[0] = a[0];
  pal[1] = a[1];
  if (pal[0] > pal[1]) {
    for (int i = 1; i < 7; ++i)
      pal[i + 1] = u8(((7 - i) * pal[0] + i * pal[1] + 3) / 7);
  } else {
    for (int i = 1; i < 5; ++i)
      pal[i + 1] = u8(((5 - i) * pal[0] + i * pal[1] + 2) / 5);
    pal[6] = 0;
    pal[7] = 255;
  }
  u64 bits = 0;
  for (int i = 0; i < 6; ++i)
    bits |= u64(a[2 + i]) << (8 * i);
  for (int i = 0; i < 16; ++i)
    rgba[i * 4 + 3] = pal[(bits >> (3 * i)) & 7u];
}

void DecodeBlock(u32 format, const u8 *block, u8 *rgba) {
  if (format == kDxt1) {
    DecodeColorBlock(block, rgba, true);
  } else if (format == kDxt3) {
    DecodeColorBlock(block + 8, rgba, false);
    DecodeDxt3Alpha(block, rgba);
  } else {
    DecodeColorBlock(block + 8, rgba, false);
    DecodeDxt5Alpha(block, rgba);
  }
}

// Whole level to RGBA8 (width x height, row-major).
void DecodeLevel(u32 format, const u8 *blocks, u32 row_bytes, u32 width,
                 u32 height, std::vector<u8> &rgba) {
  rgba.assign(size_t(width) * height * 4u, 0u);
  const u32 bw = (width + 3u) / 4u, bh = (height + 3u) / 4u;
  const u32 bb = BlockBytes(format);
  u8 tmp[64];
  for (u32 by = 0; by < bh; ++by) {
    for (u32 bx = 0; bx < bw; ++bx) {
      DecodeBlock(format, blocks + size_t(by) * row_bytes + size_t(bx) * bb, tmp);
      for (u32 y = 0; y < 4u; ++y) {
        const u32 py = by * 4u + y;
        if (py >= height)
          break;
        for (u32 x = 0; x < 4u; ++x) {
          const u32 px = bx * 4u + x;
          if (px >= width)
            break;
          std::memcpy(&rgba[(size_t(py) * width + px) * 4u], tmp + (y * 4 + x) * 4, 4);
        }
      }
    }
  }
}

// 2x2 box filter; odd edges repeat the last row/column.
void Downsample(const std::vector<u8> &src, u32 sw, u32 sh,
                std::vector<u8> &dst, u32 &dw, u32 &dh) {
  dw = std::max(sw / 2u, 1u);
  dh = std::max(sh / 2u, 1u);
  dst.assign(size_t(dw) * dh * 4u, 0u);
  for (u32 y = 0; y < dh; ++y) {
    const u32 y0 = std::min(y * 2u, sh - 1u), y1 = std::min(y * 2u + 1u, sh - 1u);
    for (u32 x = 0; x < dw; ++x) {
      const u32 x0 = std::min(x * 2u, sw - 1u), x1 = std::min(x * 2u + 1u, sw - 1u);
      for (u32 c = 0; c < 4u; ++c) {
        const u32 sum = src[(size_t(y0) * sw + x0) * 4u + c] +
                        src[(size_t(y0) * sw + x1) * 4u + c] +
                        src[(size_t(y1) * sw + x0) * 4u + c] +
                        src[(size_t(y1) * sw + x1) * 4u + c];
        dst[(size_t(y) * dw + x) * 4u + c] = u8((sum + 2u) / 4u);
      }
    }
  }
}

u32 Dist2(const u8 *a, const u8 *b) {
  u32 d = 0;
  for (int k = 0; k < 3; ++k) {
    const int e = int(a[k]) - int(b[k]);
    d += u32(e * e);
  }
  return d;
}

// DXT1 with punch-through: stb picks the endpoints, and when any texel is
// transparent the block is rewritten in the 3-colour mode with index 3 for
// those texels.
void EncodeDxt1(const u8 *rgba, u8 *out) {
  bool transparent = false;
  for (int i = 0; i < 16; ++i)
    transparent |= rgba[i * 4 + 3] < 128;
  if (!transparent) {
    stb_compress_dxt_block(out, rgba, 0, STB_DXT_NORMAL);
    return;
  }
  // Endpoints from the opaque texels, transparent ones filled with the
  // opaque mean so they do not pull the endpoints.
  u8 filled[64];
  std::memcpy(filled, rgba, 64);
  u32 mean[3] = {0, 0, 0}, n = 0;
  for (int i = 0; i < 16; ++i)
    if (rgba[i * 4 + 3] >= 128) {
      for (int k = 0; k < 3; ++k)
        mean[k] += rgba[i * 4 + k];
      ++n;
    }
  if (n)
    for (int i = 0; i < 16; ++i)
      if (rgba[i * 4 + 3] < 128)
        for (int k = 0; k < 3; ++k)
          filled[i * 4 + k] = u8(mean[k] / n);
  u8 tmp[8];
  stb_compress_dxt_block(tmp, filled, 0, STB_DXT_NORMAL);
  u16 c0 = u16(tmp[0] | (tmp[1] << 8));
  u16 c1 = u16(tmp[2] | (tmp[3] << 8));
  if (c0 > c1)
    std::swap(c0, c1); // c0 <= c1 selects the 3-colour mode
  u8 pal[3][3];
  Rgb565(c0, pal[0]);
  Rgb565(c1, pal[1]);
  for (int k = 0; k < 3; ++k)
    pal[2][k] = u8((pal[0][k] + pal[1][k]) / 2);
  u32 bits = 0;
  for (int i = 0; i < 16; ++i) {
    u32 idx = 3;
    if (rgba[i * 4 + 3] >= 128) {
      u32 best = ~0u;
      for (u32 p = 0; p < 3; ++p) {
        const u32 d = Dist2(rgba + i * 4, pal[p]);
        if (d < best) {
          best = d;
          idx = p;
        }
      }
    }
    bits |= idx << (2 * i);
  }
  out[0] = u8(c0);
  out[1] = u8(c0 >> 8);
  out[2] = u8(c1);
  out[3] = u8(c1 >> 8);
  out[4] = u8(bits);
  out[5] = u8(bits >> 8);
  out[6] = u8(bits >> 16);
  out[7] = u8(bits >> 24);
}

void EncodeBlock(u32 format, const u8 *rgba, u8 *out) {
  if (format == kDxt1) {
    EncodeDxt1(rgba, out);
  } else if (format == kDxt3) {
    for (int i = 0; i < 8; ++i) {
      const u32 a0 = (rgba[(2 * i) * 4 + 3] + 8u) / 17u;
      const u32 a1 = (rgba[(2 * i + 1) * 4 + 3] + 8u) / 17u;
      out[i] = u8(std::min(a0, 15u) | (std::min(a1, 15u) << 4));
    }
    stb_compress_dxt_block(out + 8, rgba, 0, STB_DXT_NORMAL);
  } else {
    stb_compress_dxt_block(out, rgba, 1, STB_DXT_NORMAL);
  }
}

// RGBA8 level to block rows with a 256-byte-aligned stride.
void EncodeLevel(u32 format, const std::vector<u8> &rgba, u32 width, u32 height,
                 std::vector<u8> &blocks, u32 &row_bytes) {
  const u32 bw = (width + 3u) / 4u, bh = (height + 3u) / 4u;
  const u32 bb = BlockBytes(format);
  row_bytes = ((bw * bb) + 255u) & ~255u;
  blocks.assign(size_t(row_bytes) * bh, 0u);
  u8 tmp[64];
  for (u32 by = 0; by < bh; ++by) {
    for (u32 bx = 0; bx < bw; ++bx) {
      for (u32 y = 0; y < 4u; ++y) {
        const u32 py = std::min(by * 4u + y, height - 1u);
        for (u32 x = 0; x < 4u; ++x) {
          const u32 px = std::min(bx * 4u + x, width - 1u);
          std::memcpy(tmp + (y * 4 + x) * 4, &rgba[(size_t(py) * width + px) * 4u], 4);
        }
      }
      EncodeBlock(format, tmp, blocks.data() + size_t(by) * row_bytes + size_t(bx) * bb);
    }
  }
}

} // namespace

bool GenerateNativeTextureMips(const NativeTextureData &base, NativeTextureData &out) {
  if (!ValidateNativeTexture(base) || base.dimension != NativeTextureDimension::Image2D ||
      base.mip_levels != 1 || base.format == NativeTextureFormat::RGBA8 ||
      base.width < 8 || base.height < 8 || uint64_t(base.width) * base.height > (16u << 20))
    return false;
  NativeTextureData result = base;
  const u32 format = u32(base.format);
  const u32 base_row_bytes = ((base.width + 3) / 4) * BlockBytes(format);
  std::vector<u8> rgba, next;
  DecodeLevel(format, base.images[0].data(), base_row_bytes, base.width, base.height, rgba);
  u32 w = base.width, h = base.height;
  // Down to 4x4: a block is the smallest thing these formats store.
  while (w > 4u || h > 4u) {
    u32 dw, dh;
    Downsample(rgba, w, h, next, dw, dh);
    rgba.swap(next);
    w = dw;
    h = dh;
    std::vector<u8> blocks;
    u32 row_bytes = 0;
    EncodeLevel(format, rgba, w, h, blocks, row_bytes);
    std::vector<u8> tight;
    if (!ImportNativeTextureImage(base.format, w, h, 1, blocks, row_bytes,
                                  uint64_t(row_bytes) * ((h + 3) / 4), tight))
      return false;
    result.images.push_back(std::move(tight));
    ++result.mip_levels;
  }
  if (!ValidateNativeTexture(result))
    return false;
  out = std::move(result);
  return true;
}

} // namespace bd::gpu::scene
