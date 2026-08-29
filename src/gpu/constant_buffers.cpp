/**
 * @file    gpu/constant_buffers.cpp
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/constant_buffers.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <memory>
#include <vector>

#include <plume_render_interface_builders.h>
#include <rex/cvar.h>
#include <rex/runtime.h>
#include <rex/types.h>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "core/profiling.h"
#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/hooks/tweaks.h"
#include "gpu/sampler_cache.h"
#include "gpu/settings.h"
#include "gpu/shaders/shader_cache.h"

REXCVAR_DECLARE(bool, bd_constants_gpu_upload);

REXCVAR_DECLARE(bool, bd_stereo);
REXCVAR_DECLARE(bool, bd_stereo_multiview);
REXCVAR_DECLARE(f64, bd_stereo_separation);
REXCVAR_DECLARE(f64, bd_stereo_convergence);

namespace bd::gpu {

namespace {

constexpr u32 kCBVAlignment = 256;

constexpr u32 kUploadChunkSize = 16 * 1024 * 1024;

// 256 vector4f registers = 4 KiB. Shaders reference the full window, so the
// upload spans the whole range every flush.
constexpr u32 kConstantRegisterCount = 256;
constexpr u32 kConstantBlockBytes = kConstantRegisterCount * 16;

struct UploadChunk {
  std::unique_ptr<plume::RenderBuffer> buffer;
  u8 *mapped = nullptr;
  u64 gpuBase = 0;
};

// One upload chunk list per in-flight frame slot, rewound only after that
// slot's GPU fence is awaited, so the GPU never reads bytes the CPU has
// overwritten.
struct FrameUpload {
  std::vector<UploadChunk> chunks;
  u32 chunkIndex = 0;
  u32 chunkOffset = 0;
  u32 peakChunkCount = 0;
};

// DecodeFromFetch + ResolveSlotLocked (mutex + hash lookup) run per bound slot
// on EVERY draw, and the fetch constants almost never change between draws, so
// a 24-byte compare replaces them on the hot path. Sampler heap slots are never
// reclaimed, so a cached index stays valid until device teardown.
struct SamplerSlotCache {
  u32 fc[6]{};
  u32 sampler = 0;
  i32 aniso = -1;
  bool clamp3d = false;
  bool valid = false;
};

struct UploadState {
  FrameUpload frames[kNumFrames];
  u32 cursor = 0;
  SharedConstants shared{};
  SharedConstants lastUploaded{};
  SamplerSlotCache samplerSlots[16];
  float shadowPcfScale = 1.0f;
  bool sharedBound = false;
  bool ready = false;
};

UploadState &upload_state() {
  static UploadState s;
  return s;
}

// Shrink the sun shadow PCF kernel inversely to the coverage box so its
// world-space penumbra stays constant as ShadowCoverageScale widens the light
// frustum, floored at one texel of the actual shadow map. Once per frame, not
// per draw, so a distance change applies without a restart (the dimension term
// lags a pending restart-gated change until the map is recreated).
void RecomputeShadowPcfScale(UploadState &s) {
  const f64 dist = std::clamp(ShadowCoverageScale(), 1.0, 4.0);
  const f64 dim = std::max(512, Settings::Get().ShadowDimension());
  s.shadowPcfScale = static_cast<float>(std::max(1.0 / dist, 1024.0 / dim));
}

bool CreateChunk(UploadChunk &chunk) {
  auto *device = bd::gpu::Video::HostDevice();
  if (!device)
    return false;
  // Where the shader constants physically live, which turns out to matter more
  // than anything else in this file.
  //
  // Translated shaders read every guest constant register with
  // vk::RawBufferLoad from a device address - see
  // research/20260829_0030_shader-constants-are-global-loads.md - so a skinned
  // vertex shader does 20-40 loads out of this buffer per vertex, and a field
  // scene runs ~400,000 vertices. An UPLOAD heap is host-visible write-combine,
  // which the GPU reads uncached; GPU_UPLOAD is DEVICE_LOCAL | HOST_VISIBLE, so
  // it stays mappable but the GPU's caches work on it. On a UMA part like the
  // Quest 2 that is the same physical memory with different caching, and costs
  // nothing to ask for.
  auto desc = plume::RenderBufferDesc::UploadBuffer(
      kUploadChunkSize, plume::RenderBufferFlag::CONSTANT |
                            plume::RenderBufferFlag::VERTEX |
                            plume::RenderBufferFlag::INDEX |
                            plume::RenderBufferFlag::DEVICE_ADDRESSABLE);
  const bool want_gpu_heap = REXCVAR_GET(bd_constants_gpu_upload) &&
                             device->getCapabilities().gpuUploadHeap;
  if (want_gpu_heap)
    desc.heapType = plume::RenderHeapType::GPU_UPLOAD;
  chunk.buffer = bd::gpu::CreateHostBuffer(device, desc, "cb-upload-chunk");
  if (!chunk.buffer && want_gpu_heap) {
    // The heap can exist and still fail to allocate 16 MiB of it. Falling back
    // is better than losing the renderer.
    BD_WARN("constant_buffers: GPU_UPLOAD chunk failed, falling back to UPLOAD");
    desc.heapType = plume::RenderHeapType::UPLOAD;
    chunk.buffer = bd::gpu::CreateHostBuffer(device, desc, "cb-upload-chunk");
  } else if (want_gpu_heap) {
    static bool told = false;
    if (!told) {
      told = true;
      BD_INFO("constant_buffers: shader constants in GPU_UPLOAD "
              "(device-local, host-visible)");
    }
  }
  if (!chunk.buffer) {
    BD_ERROR("constant_buffers: createBuffer({} MiB chunk) failed",
             kUploadChunkSize / (1024 * 1024));
    return false;
  }
  chunk.mapped = reinterpret_cast<u8 *>(chunk.buffer->map());
  if (!chunk.mapped) {
    BD_ERROR("constant_buffers: RenderBuffer::map() returned null");
    chunk.buffer.reset();
    return false;
  }
  chunk.gpuBase = chunk.buffer->getDeviceAddress();
  return true;
}

ConstantAllocation Allocate(UploadState &s, u32 size, u32 alignment) {
  if (!s.ready)
    return {};
  if (size > kUploadChunkSize) {
    BD_ERROR("constant_buffers: single allocation {} exceeds chunk size {}",
             size, kUploadChunkSize);
    return {};
  }
  FrameUpload &up = s.frames[s.cursor];
  u32 off = (up.chunkOffset + alignment - 1) & ~(alignment - 1);
  if (off + size > kUploadChunkSize) {
    ++up.chunkIndex;
    off = 0;
  }
  if (up.chunks.size() <= up.chunkIndex) {
    up.chunks.resize(up.chunkIndex + 1);
  }
  auto &chunk = up.chunks[up.chunkIndex];
  if (!chunk.buffer) {
    if (!CreateChunk(chunk))
      return {};
    const u32 total = up.chunkIndex + 1;
    if (total > up.peakChunkCount) {
      up.peakChunkCount = total;
      BD_DEBUG("constant_buffers: slot {} grew to {} chunk(s) ({} MiB total)",
               s.cursor, total, total * (kUploadChunkSize / (1024 * 1024)));
    }
  }
  up.chunkOffset = off + size;
  ConstantAllocation a;
  a.memory = chunk.mapped + off;
  a.ref = plume::RenderBufferReference(chunk.buffer.get(), off);
  a.gpuAddress = chunk.gpuBase + off;
  a.size = size;
  return a;
}

// kFlushNaN=true also flushes NaN -> +0 in the same pass. Xenos float ALU obeys
// the X360/D3D9 "multiply by zero yields zero" rule (0*NaN=0), so BD's
// degenerate constants (e.g. the bloom/glare 0/0 weight normalization when
// intensity is zero) are harmless on hardware. Our recompiled D3D12 shaders use
// strict IEEE (NaN*0=NaN), so a NaN constant propagates and blackens the
// post-fx composite. No BD shader reinterprets a float constant register as
// int, so the flush cannot corrupt int-encoded data. Branchless (cmov) and
// fused into the byte swap so it adds no extra memory pass.
template <bool kFlushNaN>
void CopyByteSwap32Impl(u8 *dst, u32 guest_va, u32 size) {
  const auto *src = bd::mem::at<const u32>(guest_va);
  if (!src) {
    std::memset(dst, 0, size);
    return;
  }
  const u32 count = size / sizeof(u32);
  auto *out = reinterpret_cast<u32 *>(dst);
  u32 i = 0;

#if defined(__aarch64__)
  // Sixteen dwords per iteration on ARM64, which is the hot path: this runs
  // twice per draw over 4 KiB each, and a field scene submits ~2957 draws, so
  // it is roughly 24 MB of stores a frame.
  //
  // Two separate reasons this is worth vectorising, and the second matters
  // more than the first:
  //
  //  - vrev32q_u8 byte-reverses four dwords in one instruction.
  //  - `dst` points into the persistently mapped UPLOAD heap, which is
  //    write-combine. Scalar 4-byte stores into WC memory dribble into the
  //    combine buffers and force partial flushes; 64 bytes at a time fills a
  //    whole line per iteration. On Adreno that is the larger effect.
  for (; i + 16 <= count; i += 16) {
    const auto *s8 = reinterpret_cast<const u8 *>(src + i);
    auto *d8 = reinterpret_cast<u8 *>(out + i);
    uint32x4_t v0 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(s8 + 0)));
    uint32x4_t v1 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(s8 + 16)));
    uint32x4_t v2 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(s8 + 32)));
    uint32x4_t v3 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(s8 + 48)));
    if constexpr (kFlushNaN) {
      // Same test as the scalar path: NaN iff |bits| > +Inf bits. vbicq is
      // "and not", so a lane that compares true is cleared to +0.
      const uint32x4_t abs_mask = vdupq_n_u32(0x7FFFFFFFu);
      const uint32x4_t inf_bits = vdupq_n_u32(0x7F800000u);
      v0 = vbicq_u32(v0, vcgtq_u32(vandq_u32(v0, abs_mask), inf_bits));
      v1 = vbicq_u32(v1, vcgtq_u32(vandq_u32(v1, abs_mask), inf_bits));
      v2 = vbicq_u32(v2, vcgtq_u32(vandq_u32(v2, abs_mask), inf_bits));
      v3 = vbicq_u32(v3, vcgtq_u32(vandq_u32(v3, abs_mask), inf_bits));
    }
    vst1q_u8(d8 + 0, vreinterpretq_u8_u32(v0));
    vst1q_u8(d8 + 16, vreinterpretq_u8_u32(v1));
    vst1q_u8(d8 + 32, vreinterpretq_u8_u32(v2));
    vst1q_u8(d8 + 48, vreinterpretq_u8_u32(v3));
  }
#endif

  // Tail, and the whole job on non-ARM64. The constant blocks are 4 KiB and
  // 256-byte aligned so the vector loop normally consumes all of it, but the
  // shared block is not a multiple of 64 bytes.
  for (; i < count; ++i) {
#if defined(_MSC_VER)
    const u32 v = _byteswap_ulong(src[i]);
#else
    const u32 v = __builtin_bswap32(src[i]);
#endif
    if constexpr (kFlushNaN) {
      // NaN iff exponent all-1 and mantissa != 0, i.e. |bits| > +Inf bits.
      out[i] = (v & 0x7FFFFFFFu) > 0x7F800000u ? 0u : v;
    } else {
      out[i] = v;
    }
  }
}

void CopyByteSwap32(u8 *dst, u32 guest_va, u32 size) {
  CopyByteSwap32Impl<false>(dst, guest_va, size);
}

void CopyByteSwap32FlushNaN(u8 *dst, u32 guest_va, u32 size) {
  CopyByteSwap32Impl<true>(dst, guest_va, size);
}

} // namespace

bool TryInit() {
  auto &s = upload_state();
  if (s.ready)
    return true;
  s.cursor = 0;
  for (auto &up : s.frames) {
    up.chunkIndex = 0;
    up.chunkOffset = 0;
    up.peakChunkCount = 0;
  }
  for (auto &slot : s.samplerSlots)
    slot.valid = false;
  s.sharedBound = false;
  RecomputeShadowPcfScale(s);
  s.ready = true;
  return true;
}

void ResetFrame(u32 slot) {
  auto &s = upload_state();
  if (!s.ready)
    return;
  s.cursor = slot;
  FrameUpload &up = s.frames[slot];
  up.chunkIndex = 0;
  up.chunkOffset = 0;
  RecomputeShadowPcfScale(s);
}

void InvalidateSharedBinding() { upload_state().sharedBound = false; }

// c50.xy is BD's NDC->UV half-scale (0.5 on hw). bd_blur_ps reconstructs its
// sample UV as uv = c50.xy*(ndc+1), so it MUST be 0.5. The guest derives it
// from sceneDim/1280x720*0.5, letting output res and supersampling leak in, and
// the pass oversamples until the source collapses into the top-left. bd_blur_ps
// is the ONLY pixel shader reading c50 as this screen->UV scale (verified
// across all 17 c50-using PS). The rest own c50 as material data, so a blanket
// pin would corrupt them (bd_lightshaft_ps's g_vLightShaftDiffuse -> gray
// god rays). bdCameraRefractionUvScaleHook pins device reg50 at the
// bdCameraRender writers, and this catches blur draws those writers miss.
constexpr u32 kScreenUVScaleRegByteOffset = 50 * 16;

// The view-projection matrix's first register. Taken from the emitted HLSL,
// where g_mViewProj(INDEX) reads VertexShaderConstants + (32 + INDEX) * 16.
constexpr u32 kViewProjRegister = 32;
constexpr u64 kBDBlurPSHash = 0xD94E164866C3B9BCull;
void PinScreenUVScaleReg(u8 *block) {
  auto *ps = bd::gpu::state().pipelineState.pixelShader;
  const u64 h = (ps && ps->shaderCacheEntry) ? ps->shaderCacheEntry->hash : 0;
  if (h == kBDBlurPSHash) {
    auto *reg = reinterpret_cast<float *>(block + kScreenUVScaleRegByteOffset);
    reg[0] = 0.5f;
    reg[1] = 0.5f;
  }
}

ConstantAllocation UploadVertexShaderConstants(u32 device_guest,
                                               float eye_skew,
                                               float eye_shift) {
  BD_CPU_ZONE("UploadVSConstants");
  auto &s = upload_state();
  if (!device_guest)
    return {};
  auto alloc = Allocate(s, kConstantBlockBytes, kCBVAlignment);
  if (!alloc.memory)
    return {};
  CopyByteSwap32FlushNaN(alloc.memory,
                         device_guest + offsetof(D3DDevice, vsFloatConstants),
                         kConstantBlockBytes);
  if (eye_skew != 0.0f || eye_shift != 0.0f) {
    // The four registers are the four COLUMNS of the view-projection, not its
    // rows. Read off a scene draw, register 35 is (0.063, -0.122, 0.991,
    // -6.267): its xyz has unit length, so it is the camera's forward axis plus
    // a distance, which is what clip.w must be. As rows the w coefficients
    // would include a -485, which no perspective matrix has.
    //
    // clip.x += skew * clip.z was the first attempt and it produces no depth
    // at all. For any normal projection clip.z and clip.w agree to within a
    // fraction of a percent beyond a few metres, so after the perspective
    // divide that term is a constant sideways shift of the whole image - which
    // is measurably what it did: +59px of disparity at the sky against +57px on
    // the near ground, across a scene hundreds of metres deep.
    //
    // A lateral eye translation is a *constant* added to clip.x. Dividing by w
    // then makes the screen-space shift inversely proportional to depth, which
    // is parallax: near geometry separates strongly, distant geometry barely
    // moves. Since clip.x = dot(position, register 32) and the position's w is
    // 1, the constant lives in whichever component of register 32 multiplies
    // that w - and from the shader's own swizzle,
    //   r3.x = dot(r5.xyzw, g_mViewProj(0).wzyx)
    // pairs r5.w with .x. So it is a single float.
    //
    // Convergence is unchanged and was always right: shift * clip.w moves the
    // projection centre, setting the distance at which parallax is zero.
    auto *m = reinterpret_cast<float *>(alloc.memory) + kViewProjRegister * 4;
    m[0] += eye_skew;
    for (int i = 0; i < 4; ++i)
      m[i] += eye_shift * m[12 + i];
  }
  return alloc;
}

ConstantAllocation UploadPixelShaderConstants(u32 device_guest) {
  BD_CPU_ZONE("UploadPSConstants");
  auto &s = upload_state();
  if (!device_guest)
    return {};
  auto alloc = Allocate(s, kConstantBlockBytes, kCBVAlignment);
  if (!alloc.memory)
    return {};
  CopyByteSwap32FlushNaN(alloc.memory,
                         device_guest + offsetof(D3DDevice, psFloatConstants),
                         kConstantBlockBytes);
  PinScreenUVScaleReg(alloc.memory);
  return alloc;
}

ConstantAllocation UploadSharedConstants(u32 device_guest) {
  BD_CPU_ZONE("UploadSharedConstants");
  auto &s = upload_state();
  if (!s.ready)
    return {};
  // bd_anisotropy participates in DecodeFromFetch's output, so a live change
  // must miss the per-slot cache so stale sampler indices are re-resolved.
  const i32 aniso_now = Settings::Get().Anisotropy();

  // vs.textures is authoritative: our SetTexture hook replaces BD's recompiled
  // body, so the engine's per-slot bound-texture shadow (device+0x2FF0+slot*4)
  // and GPU texture fetch constants (device+0x400+slot*0x18) are never written.
  // Video::SetTexture mirrors Xenos semantics (a null bind is ignored, since on
  // hardware it does not rebuild the fetch constant), so vs.textures holds the
  // last real texture per slot, exactly what the GPU would still be sampling.
  auto &vs = bd::gpu::state();
  const auto *device_p = bd::mem::at<const D3DDevice>(device_guest);
  for (u32 i = 0; i < 16; ++i) {
    s.shared.samplerIndices[i] = 0;
    s.shared.texture2DIndices[i] = bd::gpu::kNullTexture2DDescriptorIndex;
    s.shared.texture3DIndices[i] = bd::gpu::kNullTexture3DDescriptorIndex;
    s.shared.textureCubeIndices[i] = bd::gpu::kNullTextureCubeDescriptorIndex;

    bd::gpu::GuestTexture *tex = vs.textures[i];
    if (tex && tex->sourceSurface && tex->sourceSurface->texture &&
        tex->sourceSurface->sampleCount == plume::RenderSampleCount::COUNT_1 &&
        tex->sourceSurface != vs.render_target &&
        tex->sourceSurface != vs.depth_stencil &&
        tex->sourceSurface->descriptorIndex !=
            bd::gpu::kInvalidDescriptorIndex) {
      tex = tex->sourceSurface;
    }
    if (tex && tex->descriptorIndex != bd::gpu::kInvalidDescriptorIndex) {
      switch (tex->viewDimension) {
      case plume::RenderTextureViewDimension::TEXTURE_3D:
        s.shared.texture3DIndices[i] = tex->descriptorIndex;
        // X360: a 2D fetch on a 3D resource reads slice 0, so publish the
        // volume as its slice-0 2D view too so tfetch2D samples the base layer.
        if (tex->companion2D && tex->companion2D->descriptorIndex !=
                                    bd::gpu::kInvalidDescriptorIndex) {
          s.shared.texture2DIndices[i] = tex->companion2D->descriptorIndex;
        }
        break;
      case plume::RenderTextureViewDimension::TEXTURE_CUBE:
        s.shared.textureCubeIndices[i] = tex->descriptorIndex;
        break;
      case plume::RenderTextureViewDimension::TEXTURE_2D:
      case plume::RenderTextureViewDimension::UNKNOWN:
      default:
        s.shared.texture2DIndices[i] = tex->descriptorIndex;
        // BD static reflection cubes bind as a 2D atlas yet the water/glass
        // shader cube-fetches the slot, so publish the sliced TextureCube
        // companion so tfetchCube resolves a real cube, not the null cube.
        if (tex->companionCube && tex->companionCube->descriptorIndex !=
                                      bd::gpu::kInvalidDescriptorIndex) {
          s.shared.textureCubeIndices[i] = tex->companionCube->descriptorIndex;
        }
        break;
      }

      // X360 stores sampler state in fetchConstants[N].dword[*]. The
      // SetSamplerState_* setters are unhooked and run their recompiled bodies,
      // so the address mode bits there are valid.
      if (device_p) {
        const auto &fc_be = device_p->fetchConstants[i];
        const u32 fc[6] = {
            u32(fc_be.dword[0]), u32(fc_be.dword[1]), u32(fc_be.dword[2]),
            u32(fc_be.dword[3]), u32(fc_be.dword[4]), u32(fc_be.dword[5]),
        };
        const bool clamp3d =
            tex->viewDimension == plume::RenderTextureViewDimension::TEXTURE_3D;
        auto &sc = s.samplerSlots[i];
        if (sc.valid && sc.clamp3d == clamp3d && sc.aniso == aniso_now &&
            std::memcmp(sc.fc, fc, sizeof(fc)) == 0) {
          s.shared.samplerIndices[i] = sc.sampler;
        } else {
          auto desc = DecodeFromFetch(fc);

          // Shell fur volumes encode shell depth in W, and X360-default WRAP
          // wraps a z=0 fetch's second tap to the tip slice and halves density,
          // so force CLAMP to keep both taps on the dense base slice.
          if (clamp3d) {
            desc.addressW = plume::RenderTextureAddressMode::CLAMP;
          }
          const u32 resolved = ResolveSlotLocked(desc);
          std::memcpy(sc.fc, fc, sizeof(fc));
          sc.sampler = resolved;
          sc.aniso = aniso_now;
          sc.clamp3d = clamp3d;
          sc.valid = true;
          s.shared.samplerIndices[i] = resolved;
        }
      }
    }
  }

  // Shader bool constants: VS at device+0x2700, PS at device+0x2710, 4 BE
  // dwords each. Shaders branch on BOOL_BIT(n) of a 256-bit register file
  // (VS 0..127, PS 128..255).
  if (device_guest) {
    const auto *device = bd::mem::at<const D3DDevice>(device_guest);
    for (u32 i = 0; i < 4; ++i) {
      s.shared.booleansArr[i] = device ? u32(device->vsBoolConstants[i]) : 0u;
      s.shared.booleansArr[4 + i] =
          device ? u32(device->psBoolConstants[i]) : 0u;
    }
  }

  s.shared.alphaThreshold = bd::gpu::Video::AlphaThreshold();
  s.shared.halfPixelOffsetX = 0.0f;
  s.shared.halfPixelOffsetY = 0.0f;
  s.shared.swappedTexcoords =
      vs.vertex_declaration ? vs.vertex_declaration->swappedTexcoords : 0u;
  s.shared.swappedNormals =
      vs.vertex_declaration ? vs.vertex_declaration->swappedNormals : 0u;
  s.shared.swappedBinormals =
      vs.vertex_declaration ? vs.vertex_declaration->swappedBinormals : 0u;
  s.shared.swappedTangents =
      vs.vertex_declaration ? vs.vertex_declaration->swappedTangents : 0u;
  s.shared.swappedBlendWeights =
      vs.vertex_declaration ? vs.vertex_declaration->swappedBlendWeights : 0u;
  s.shared.swappedPositions =
      vs.vertex_declaration ? vs.vertex_declaration->swappedPositions : 0u;
  s.shared.sintTexcoords =
      vs.vertex_declaration ? vs.vertex_declaration->sintTexcoords : 0u;

  s.shared.shadowPcfScale = s.shadowPcfScale;
  // Multiview stereo, read by every recompiled vertex shader. Zero unless
  // bd_stereo is on, which makes the per-eye skew a no-op rather than something
  // the shader has to branch around.
  // Multiview ONLY. The shader's skew is keyed on SV_ViewID, which varies per
  // view exactly when a multiview pass is running and is 0 otherwise - so under
  // the side-by-side path it applied the same eyeSign to both eyes, adding
  // -sep to each on top of the host's per-eye matrix patch. That left eye 0 at
  // -2*sep and eye 1 at 0: still a stereo pair, but asymmetric about the mono
  // image and with the convergence term applied twice.
  //
  // bd_stereo does its per-eye work in UploadVertexShaderConstants instead, so
  // it must leave these at zero or the two mechanisms compound.
  // Multiview only, and only for scene geometry. The shader applies the skew
  // unconditionally wherever these are non-zero, so gating has to happen here -
  // which is the same gate the host's side-by-side patch already uses, and its
  // absence is why multiview slid the whole image instead of adding depth.
  const bool stereo_on = REXCVAR_GET(bd_stereo_multiview) && vs.stereoEligible;
  s.shared.stereoSeparation =
      stereo_on ? static_cast<float>(REXCVAR_GET(bd_stereo_separation)) : 0.0f;
  s.shared.stereoConvergence =
      stereo_on ? static_cast<float>(REXCVAR_GET(bd_stereo_convergence)) : 0.0f;

  // Viewport extent, not the render target's: the NDC->pixel mapping this
  // cancels is the viewport's. +x/-y = half a pixel right and down.
  s.shared.blitHalfPixelOffsetX =
      vs.viewport.width > 0.0f ? 1.0f / vs.viewport.width : 0.0f;
  s.shared.blitHalfPixelOffsetY =
      vs.viewport.height > 0.0f ? -1.0f / vs.viewport.height : 0.0f;

  // Byte-identical to the block already bound on this command list: the live
  // CBV is still correct, skip the upload and let the caller skip the rebind.
  // SharedConstants padding is zero-initialized and never written, so memcmp
  // is deterministic.
  if (s.sharedBound &&
      std::memcmp(&s.shared, &s.lastUploaded, sizeof(SharedConstants)) == 0) {
    return {};
  }

  auto alloc = Allocate(s, sizeof(SharedConstants), kCBVAlignment);
  if (!alloc.memory)
    return {};
  std::memcpy(alloc.memory, &s.shared, sizeof(SharedConstants));
  s.lastUploaded = s.shared;
  s.sharedBound = true;
  return alloc;
}

ConstantAllocation UploadGuestBytesByteSwap32(u32 guest_va, u32 size,
                                              u32 alignment) {
  BD_CPU_ZONE("UploadGuestBytesByteSwap32");
  auto &s = upload_state();
  if (!s.ready || !guest_va || !size)
    return {};
  auto alloc = Allocate(s, size, alignment);
  if (!alloc.memory)
    return {};
  CopyByteSwap32(alloc.memory, guest_va, size);
  return alloc;
}

ConstantAllocation UploadGuestBytes(u32 guest_va, u32 size, u32 alignment) {
  auto &s = upload_state();
  if (!s.ready || !guest_va || !size)
    return {};
  auto alloc = Allocate(s, size, alignment);
  if (!alloc.memory)
    return {};
  const auto *src = bd::mem::at<const u8>(guest_va);
  if (!src) {
    BD_WARN("constant_buffers: UploadGuestBytes translate failed for "
            "guest_va={:#x}",
            guest_va);
    return {};
  }
  std::memcpy(alloc.memory, src, size);
  return alloc;
}

ConstantAllocation UploadHostBytes(const void *host_data, u32 size,
                                   u32 alignment) {
  auto &s = upload_state();
  if (!s.ready || !host_data || !size)
    return {};
  auto alloc = Allocate(s, size, alignment);
  if (!alloc.memory)
    return {};
  std::memcpy(alloc.memory, host_data, size);
  return alloc;
}

} // namespace bd::gpu
