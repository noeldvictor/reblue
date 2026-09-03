/**
 * @file    gpu/constant_buffers.cpp
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "gpu/constant_buffers.h"

#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#endif

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

#include <xxhash.h>

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
#include "gpu/vertex_pull.h"
#include "gpu/frame_stats.h"
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

// Per frame slot. A draw costs at most 4096 (vertex) + 3584 (pixel) + 512
// (shared, padded) bytes, so 32 MiB carries about 4000 draws - well past the
// ~2100 a desktop field frame records.
constexpr u32 kSlotSpan = 32 * 1024 * 1024;
constexpr u32 kUploadChunkSize = kSlotSpan * kNumFrames;

// 256 vector4f registers = 4 KiB. Shaders reference the full window, so the
// upload spans the whole range every flush.
constexpr u32 kConstantRegisterCount = 256;
constexpr u32 kConstantBlockBytes = kConstantRegisterCount * 16;
// The pixel stage's register file is smaller, and a dynamic uniform buffer is
// validated on offset + range, so the descriptor must not claim more.
constexpr u32 kPixelConstantRegisterCount = 224;
constexpr u32 kPixelConstantBlockBytes = kPixelConstantRegisterCount * 16;

struct UploadChunk {
  std::unique_ptr<plume::RenderBuffer> buffer;
  u8 *mapped = nullptr;
  u64 gpuBase = 0;
};

// One upload chunk list per in-flight frame slot, rewound only after that
// slot's GPU fence is awaited, so the GPU never reads bytes the CPU has
// overwritten.
struct FrameUpload {
  // Offset within this slot's span of the shared buffer.
  u32 chunkOffset = 0;
  u32 peakOffset = 0;
  bool overflowed = false;
  // Instance records committed into this slot's region so far.
  u32 recordsCommitted = 0;
  bool recordsOverflowed = false;
};

// The GPU region per frame slot, in records.
constexpr u32 kInstanceRecordsPerSlot = 2 * kInstanceRecordsPerFrame;

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
  // Backs every frame slot. Created once, bound once.
  UploadChunk buffer;
  SharedConstants shared{};
  SharedConstants lastUploaded{};
  // The vertex and pixel blocks as last uploaded on this command list, and
  // whether that upload is still bound. The guest dirties these blocks on
  // every Set*ShaderConstant call, not on a change of value, and a scene walk
  // re-sets a material's registers for every node that wears it - so the
  // dirty flag alone re-uploads and re-binds ~535 4 KB blocks a frame whose
  // bytes did not move. A content gate, like the shared block's, keeps the
  // dynamic offset still and the driver's per-draw constant reload with it
  // (2026-09-02). Swapped into scratch first, so a hit costs no ring space.
  alignas(16) u8 lastVS[kConstantBlockBytes]{};
  alignas(16) u8 lastPS[kConstantBlockBytes]{};
  // scratchVS also holds this draw's vertex block for StageInstanceRecord,
  // uploaded or not, so the two stages keep separate scratch.
  alignas(16) u8 scratchVS[kConstantBlockBytes]{};
  alignas(16) u8 scratchPS[kConstantBlockBytes]{};
  bool vsBound = false;
  bool psBound = false;
  // The dynamic offset each bound block sits at (valid with the flag), and
  // whether lastVS holds the bound allocation's exact bytes or only its rest
  // (after a rest-keyed hit the node ranges in the allocation are another
  // node's, so the exact fast path must not trust lastVS).
  u32 boundVS = ~0u;
  u32 boundPS = ~0u;
  u32 boundShared = ~0u;
  bool lastVSExact = true;
  // Content -> dynamic offset for every block uploaded in the current frame
  // slot. The last-block compare above catches a run of one material; these
  // catch A-B-A-B, which the guest's scene walk produces constantly, and they
  // are what makes "equal offsets" mean "equal content" for the draw queue's
  // instancing key. Cleared with the slot. The vertex block has two: keyed by
  // the whole block for plain draws, and by everything outside the node
  // ranges for draws whose node constants travel in an instance record.
  // The vertex map names an allocation and its CPU shadow (the bytes the
  // ring holds there), so a masked compare can look at the allocation.
  struct VSAllocation {
    u32 offset;
    u32 shadow;
  };
  std::unordered_map<u64, VSAllocation> vsOffsets;
  std::vector<std::array<u8, kConstantBlockBytes>> vsShadow;
  u32 boundVSShadow = 0;
  std::unordered_map<u64, u32> psOffsets;
  std::unordered_map<u64, u32> sharedOffsets;
  // Instance records: staged on the CPU per draw, committed to the GPU by
  // the draw queue in emit order. See constant_buffers.h.
  UploadChunk instances;
  bool instancesTried = false;
  std::vector<InstanceRecord> staged;
  // Which texture slots were non-default last call. The slot loop used to
  // rewrite all 64 index entries every draw - 16 slots x 4 arrays, ~2000 draws
  // a frame - when Blue Dragon binds only a handful. Clearing just what was
  // actually set leaves the rest already correct from last time.
  // Starts all-dirty so the first call writes a real default into every slot -
  // SharedConstants is value-initialised to zero, and zero is a VALID
  // descriptor index, not the null one.
  u32 populatedSlots = 0xFFFFu;
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

  // Bind it into the three dynamic uniform descriptors the shaders read, once
  // for the life of the device: every draw re-bases it with a dynamic offset
  // instead, so no descriptor is rewritten while a submitted frame might still
  // be reading it.
  //
  // Through the backend-only hook, NOT a "#if defined(REBLUE_D3D12)" here: this
  // file is compiled once into reblue_common and linked into both Windows
  // executables, so the guard resolved for one backend and silently disabled
  // the binding in the other.
  if (!bd::gpu::Video::BindGuestConstantBuffer(chunk.buffer.get(),
                                               kConstantBlockBytes,
                                               kPixelConstantBlockBytes,
                                               sizeof(SharedConstants))) {
    BD_ERROR("constant_buffers: could not bind the guest constant buffer; "
             "shaders would read nothing");
    chunk.buffer.reset();
    return false;
  }
  return true;
}

// The instance record buffer: one storage buffer, kNumFrames regions of
// kInstanceRecordsPerSlot records, bound once at binding 3 of the constant
// set. Host-visible like the constant chunk and for the same reason; the
// vertex stage reads a 3.2 KB record per draw, which stays in cache.
bool CreateInstanceChunk(UploadState &s) {
  if (s.instancesTried)
    return s.instances.buffer != nullptr;
  s.instancesTried = true;
  auto *device = bd::gpu::Video::HostDevice();
  if (!device)
    return false;
  const u64 bytes =
      u64(kNumFrames) * kInstanceRecordsPerSlot * sizeof(InstanceRecord);
  auto desc = plume::RenderBufferDesc::UploadBuffer(
      bytes, plume::RenderBufferFlag::STORAGE);
  const bool want_gpu_heap = REXCVAR_GET(bd_constants_gpu_upload) &&
                             device->getCapabilities().gpuUploadHeap;
  if (want_gpu_heap)
    desc.heapType = plume::RenderHeapType::GPU_UPLOAD;
  s.instances.buffer =
      bd::gpu::CreateHostBuffer(device, desc, "instance-records");
  if (!s.instances.buffer && want_gpu_heap) {
    desc.heapType = plume::RenderHeapType::UPLOAD;
    s.instances.buffer =
        bd::gpu::CreateHostBuffer(device, desc, "instance-records");
  }
  if (!s.instances.buffer) {
    BD_ERROR("constant_buffers: createBuffer({} MiB instance records) failed; "
             "nothing will be instanced",
             bytes / (1024 * 1024));
    return false;
  }
  s.instances.mapped = reinterpret_cast<u8 *>(s.instances.buffer->map());
  if (!s.instances.mapped) {
    BD_ERROR("constant_buffers: instance record map() returned null");
    s.instances.buffer.reset();
    return false;
  }
  if (!bd::gpu::Video::BindInstanceRecordBuffer(s.instances.buffer.get(),
                                                sizeof(InstanceRecord), bytes)) {
    BD_ERROR("constant_buffers: could not bind the instance record buffer; "
             "nothing will be instanced");
    s.instances.buffer.reset();
    s.instances.mapped = nullptr;
    return false;
  }
  s.staged.reserve(kInstanceRecordsPerFrame);
  bd::gpu::VertexPullInit(device);
  BD_INFO("constant_buffers: instance records {} x {} per slot, {} MiB",
          kNumFrames, kInstanceRecordsPerSlot, bytes / (1024 * 1024));
  return true;
}

// The registers a vertex block compare covers: the shader's declared set.
struct RegisterMask {
  u32 bits[8];
};

RegisterMask VertexMask(const u32 *register_mask) {
  RegisterMask m;
  for (u32 i = 0; i < 8; ++i)
    m.bits[i] = register_mask ? register_mask[i] : 0xFFFFFFFFu;
  return m;
}

bool MaskedEqual(const u8 *a, const u8 *b, const RegisterMask &m) {
  for (u32 w = 0; w < 8; ++w) {
    u32 bits = m.bits[w];
    while (bits) {
      const u32 bit = __builtin_ctz(bits);
      bits &= bits - 1;
      const u32 off = (w * 32 + bit) * 16;
      if (std::memcmp(a + off, b + off, 16) != 0)
        return false;
    }
  }
  return true;
}

// The mask itself is part of the key, so blocks hashed under different
// shaders never collide into one allocation.
u64 MaskedHash(const u8 *a, const RegisterMask &m) {
  alignas(16) u8 tmp[sizeof(m) + kConstantBlockBytes];
  std::memcpy(tmp, &m, sizeof(m));
  u32 n = sizeof(m);
  for (u32 w = 0; w < 8; ++w) {
    u32 bits = m.bits[w];
    while (bits) {
      const u32 bit = __builtin_ctz(bits);
      bits &= bits - 1;
      std::memcpy(tmp + n, a + (w * 32 + bit) * 16, 16);
      n += 16;
    }
  }
  return XXH3_64bits(tmp, n);
}

// An allocation already in the ring, for a content hit: the caller binds it.
ConstantAllocation AllocationAt(UploadState &s, u32 offset, u32 size) {
  ConstantAllocation a;
  a.memory = s.buffer.mapped + offset;
  a.ref = plume::RenderBufferReference(s.buffer.buffer.get(), offset);
  a.gpuAddress = s.buffer.gpuBase + offset;
  a.dynamicOffset = offset;
  a.size = size;
  return a;
}


ConstantAllocation Allocate(UploadState &s, u32 size, u32 alignment) {
  if (!s.ready)
    return {};
  if (size > kUploadChunkSize) {
    BD_ERROR("constant_buffers: single allocation {} exceeds chunk size {}",
             size, kUploadChunkSize);
    return {};
  }
  if (!s.buffer.buffer && !CreateChunk(s.buffer))
    return {};

  FrameUpload &up = s.frames[s.cursor];
  u32 off = (up.chunkOffset + alignment - 1) & ~(alignment - 1);
  if (off + size > kSlotSpan) {
    // Wrapping corrupts constants the GPU may still be reading, but the
    // alternative is dropping the draw entirely. Say so once - a silent wrap
    // reads as a rendering bug with no cause.
    if (!up.overflowed) {
      up.overflowed = true;
      BD_ERROR("constant_buffers: slot {} exhausted its {} MiB span at {} bytes "
               "- constants will wrap and some draws will read the wrong ones. "
               "Raise kSlotSpan.",
               s.cursor, kSlotSpan / (1024 * 1024), off + size);
    }
    off = 0;
  }
  const u32 base = s.cursor * kSlotSpan + off;
  up.chunkOffset = off + size;
  if (up.chunkOffset > up.peakOffset)
    up.peakOffset = up.chunkOffset;

  ConstantAllocation a;
  a.memory = s.buffer.mapped + base;
  a.ref = plume::RenderBufferReference(s.buffer.buffer.get(), base);
  a.gpuAddress = s.buffer.gpuBase + base;
  a.dynamicOffset = base;
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
#elif defined(__SSSE3__) || defined(_M_X64) || defined(__x86_64__)
  // The same job on x86-64, which had none and ran the scalar loop for all
  // 2048 dwords of every block - twice per draw, ~2000 draws a frame. That is
  // the desktop's share of the per-draw cost, and the desktop is the dev loop.
  //
  // pshufb reverses four dwords at once against a constant mask. SSSE3 is the
  // floor here: x86-64 targets that predate it are a decade older than the
  // Vulkan 1.2 this build already requires.
  {
    const __m128i swap =
        _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
    const __m128i abs_mask = _mm_set1_epi32(0x7FFFFFFF);
    // Unsigned "greater than" has no direct SSE2 form, so bias both sides into
    // signed range and use the signed compare. +Inf biased is the constant.
    const __m128i bias = _mm_set1_epi32(int(0x80000000u));
    const __m128i inf_biased = _mm_set1_epi32(int(0x7F800000u ^ 0x80000000u));
    for (; i + 16 <= count; i += 16) {
      const auto *s8 = reinterpret_cast<const u8 *>(src + i);
      auto *d8 = reinterpret_cast<u8 *>(out + i);
      for (int blk = 0; blk < 4; ++blk) {
        __m128i v = _mm_shuffle_epi8(
            _mm_loadu_si128(reinterpret_cast<const __m128i *>(s8 + blk * 16)),
            swap);
        if constexpr (kFlushNaN) {
          const __m128i mag = _mm_and_si128(v, abs_mask);
          const __m128i is_nan =
              _mm_cmpgt_epi32(_mm_xor_si128(mag, bias), inf_biased);
          v = _mm_andnot_si128(is_nan, v);
        }
        _mm_storeu_si128(reinterpret_cast<__m128i *>(d8 + blk * 16), v);
      }
    }
  }
#endif

  // Tail, and the whole job where neither vector path applies. The constant
  // blocks are 4 KiB and
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

} // namespace

void ByteSwap32ToHost(u8 *dst, const u8 *src, u32 size) {
  const u32 count = size / 4;
  const u32 *in = reinterpret_cast<const u32 *>(src);
  u32 *out = reinterpret_cast<u32 *>(dst);
  for (u32 i = 0; i < count; ++i) {
#if defined(_MSC_VER)
    out[i] = _byteswap_ulong(in[i]);
#else
    out[i] = __builtin_bswap32(in[i]);
#endif
  }
  for (u32 i = count * 4; i < size; ++i)
    dst[i] = src[i];
}

namespace {

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
    up.chunkOffset = 0;
    up.peakOffset = 0;
    up.overflowed = false;
  }
  for (auto &slot : s.samplerSlots)
    slot.valid = false;
  // Same reason as the initialiser: after a reset every slot must be rewritten.
  s.populatedSlots = 0xFFFFu;
  s.sharedBound = false;
  s.vsBound = false;
  s.psBound = false;
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
  up.chunkOffset = 0;
  up.recordsCommitted = 0;
  s.staged.clear();
  bd::gpu::VertexPullFrameReset(slot);
  // The offset caches name allocations in the slot being rewound.
  s.vsOffsets.clear();
  s.vsShadow.clear();
  s.psOffsets.clear();
  s.sharedOffsets.clear();
  s.vsBound = false;
  s.psBound = false;
  s.sharedBound = false;
  s.lastVSExact = true;
  RecomputeShadowPcfScale(s);
}

bool InstanceRecordsReady() {
  auto &s = upload_state();
  if (!s.ready)
    return false;
  return CreateInstanceChunk(s);
}

bool InstanceRecordsRoom() {
  auto &s = upload_state();
  return s.instances.buffer && s.staged.size() < kInstanceRecordsPerFrame;
}

u32 StageInstanceRecord() {
  auto &s = upload_state();
  if (!s.instances.buffer || s.staged.size() >= kInstanceRecordsPerFrame)
    return ~0u;
  s.staged.emplace_back();
  std::memcpy(s.staged.back().regs, s.scratchVS, sizeof(InstanceRecord));
  return static_cast<u32>(s.staged.size() - 1);
}

u64 HashConstantBlock(u32 dynamic_offset, const u32 *register_mask) {
  auto &s = upload_state();
  if (!s.buffer.mapped)
    return 0;
  return MaskedHash(s.buffer.mapped + dynamic_offset, VertexMask(register_mask));
}

const u8 *ConstantBlockBytes(u32 dynamic_offset) {
  auto &s = upload_state();
  if (!s.buffer.mapped)
    return nullptr;
  return s.buffer.mapped + dynamic_offset;
}

u32 UploadVertexBlockFromStaged(u32 index) {
  auto &s = upload_state();
  if (index >= s.staged.size() || !s.ready)
    return ~0u;
  const u8 *block = reinterpret_cast<const u8 *>(s.staged[index].regs);
  const RegisterMask mask = VertexMask(nullptr);
  const u64 h = MaskedHash(block, mask);
  if (auto it = s.vsOffsets.find(h); it != s.vsOffsets.end())
    return it->second.offset;
  auto alloc = Allocate(s, kConstantBlockBytes, kCBVAlignment);
  if (!alloc.memory)
    return ~0u;
  std::memcpy(alloc.memory, block, kConstantBlockBytes);
  s.vsShadow.emplace_back();
  std::memcpy(s.vsShadow.back().data(), block, kConstantBlockBytes);
  s.vsOffsets.emplace(h, UploadState::VSAllocation{
                             alloc.dynamicOffset,
                             static_cast<u32>(s.vsShadow.size() - 1)});
  NoteConstantUpload(true, true);
  return alloc.dynamicOffset;
}

const InstanceRecord *StagedInstanceRecord(u32 index) {
  auto &s = upload_state();
  return index < s.staged.size() ? &s.staged[index] : nullptr;
}

u32 CommitInstanceRecords(const u32 *staged, u32 n) {
  auto &s = upload_state();
  if (!s.instances.mapped || n == 0)
    return ~0u;
  FrameUpload &up = s.frames[s.cursor];
  if (up.recordsCommitted + n > kInstanceRecordsPerSlot) {
    if (!up.recordsOverflowed) {
      up.recordsOverflowed = true;
      BD_ERROR("constant_buffers: slot {} out of instance records at {} + {} "
               "of {}; those draws render with stale transforms. Raise "
               "kInstanceRecordsPerFrame.",
               s.cursor, up.recordsCommitted, n, kInstanceRecordsPerSlot);
    }
    return ~0u;
  }
  const u32 first = s.cursor * kInstanceRecordsPerSlot + up.recordsCommitted;
  auto *dst = reinterpret_cast<InstanceRecord *>(s.instances.mapped) + first;
  for (u32 i = 0; i < n; ++i) {
    const u32 idx = staged[i];
    if (idx < s.staged.size())
      dst[i] = s.staged[idx];
    else
      std::memset(&dst[i], 0, sizeof(InstanceRecord));
  }
  up.recordsCommitted += n;
  bd::gpu::VertexPullCommit(staged, n, first);
  return first;
}

void InvalidateSharedBinding() {
  auto &s = upload_state();
  s.sharedBound = false;
  s.vsBound = false;
  s.psBound = false;
}

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

// The vertex or pixel block into scratch: from the host-issued draw's
// template when one is set (already host order), else swapped from the guest.
void FetchVertexBlock(UploadState &s, u32 device_guest) {
  const auto *ov = bd::gpu::state().material_override;
  if (ov && ov->vs) {
    std::memcpy(s.scratchVS, ov->vs, kConstantBlockBytes);
    return;
  }
  CopyByteSwap32FlushNaN(s.scratchVS,
                         device_guest + offsetof(D3DDevice, vsFloatConstants),
                         kConstantBlockBytes);
}

void FetchPixelBlock(UploadState &s, u32 device_guest) {
  const auto *ov = bd::gpu::state().material_override;
  if (ov && ov->ps) {
    std::memcpy(s.scratchPS, ov->ps, kConstantBlockBytes);
    return;
  }
  CopyByteSwap32FlushNaN(s.scratchPS,
                         device_guest + offsetof(D3DDevice, psFloatConstants),
                         kConstantBlockBytes);
}

void CopyGuestVertexBlock(u32 device_guest, u8 *out) {
  if (!device_guest || !out)
    return;
  CopyByteSwap32FlushNaN(out,
                         device_guest + offsetof(D3DDevice, vsFloatConstants),
                         kConstantBlockBytes);
}

void CopyGuestPixelBlock(u32 device_guest, u8 *out) {
  if (!device_guest || !out)
    return;
  CopyByteSwap32FlushNaN(out,
                         device_guest + offsetof(D3DDevice, psFloatConstants),
                         kConstantBlockBytes);
  PinScreenUVScaleReg(out);
}

void SnapshotVertexShaderConstants(u32 device_guest) {
  auto &s = upload_state();
  if (!device_guest)
    return;
  FetchVertexBlock(s, device_guest);
}

ConstantAllocation UploadVertexShaderConstants(u32 device_guest,
                                               float eye_skew,
                                               float eye_shift,
                                               const u32 *register_mask) {
  BD_CPU_ZONE("UploadVSConstants");
  auto &s = upload_state();
  if (!device_guest)
    return {};
  u8 *block = s.scratchVS;
  FetchVertexBlock(s, device_guest);
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
    auto *m = reinterpret_cast<float *>(block) + kViewProjRegister * 4;
    m[0] += eye_skew;
    for (int i = 0; i < 4; ++i)
      m[i] += eye_shift * m[12 + i];
  }
  // Equal, in every register this shader reads, to the allocation still
  // bound on this command list: keep it. The compare is against the
  // ALLOCATION's bytes (a CPU shadow of every block uploaded this frame),
  // never against the previous draw's block: a hit binds an allocation that
  // may differ from that draw's block outside its mask, and the next shader
  // may read exactly there.
  const RegisterMask mask = VertexMask(register_mask);
  if (s.vsBound && MaskedEqual(block, s.vsShadow[s.boundVSShadow].data(), mask)) {
    NoteConstantUpload(true, false);
    return {};
  }
  // Uploaded earlier this frame, equal in the masked registers: bind that
  // allocation again.
  const u64 h = MaskedHash(block, mask);
  if (auto it = s.vsOffsets.find(h); it != s.vsOffsets.end()) {
    const u32 off = it->second.offset;
    NoteConstantUpload(true, false);
    if (s.vsBound && s.boundVS == off)
      return {};
    s.vsBound = true;
    s.boundVS = off;
    s.boundVSShadow = it->second.shadow;
    return AllocationAt(s, off, kConstantBlockBytes);
  }
  auto alloc = Allocate(s, kConstantBlockBytes, kCBVAlignment);
  if (!alloc.memory)
    return {};
  std::memcpy(alloc.memory, block, kConstantBlockBytes);
  s.vsShadow.emplace_back();
  std::memcpy(s.vsShadow.back().data(), block, kConstantBlockBytes);
  s.vsBound = true;
  s.boundVS = alloc.dynamicOffset;
  s.boundVSShadow = static_cast<u32>(s.vsShadow.size() - 1);
  s.vsOffsets.emplace(h, UploadState::VSAllocation{alloc.dynamicOffset, s.boundVSShadow});
  NoteConstantUpload(true, true);
  return alloc;
}

ConstantAllocation UploadPixelShaderConstants(u32 device_guest) {
  BD_CPU_ZONE("UploadPSConstants");
  auto &s = upload_state();
  if (!device_guest)
    return {};
  u8 *block = s.scratchPS;
  FetchPixelBlock(s, device_guest);
  PinScreenUVScaleReg(block);
  if (s.psBound && std::memcmp(block, s.lastPS, kConstantBlockBytes) == 0) {
    NoteConstantUpload(false, false);
    return {};
  }
  const u64 h = XXH3_64bits(block, kConstantBlockBytes);
  if (auto it = s.psOffsets.find(h); it != s.psOffsets.end()) {
    const u32 off = it->second;
    std::memcpy(s.lastPS, block, kConstantBlockBytes);
    NoteConstantUpload(false, false);
    if (s.psBound && s.boundPS == off)
      return {};
    s.psBound = true;
    s.boundPS = off;
    return AllocationAt(s, off, kConstantBlockBytes);
  }
  auto alloc = Allocate(s, kConstantBlockBytes, kCBVAlignment);
  if (!alloc.memory)
    return {};
  std::memcpy(alloc.memory, block, kConstantBlockBytes);
  std::memcpy(s.lastPS, block, kConstantBlockBytes);
  s.psBound = true;
  s.boundPS = alloc.dynamicOffset;
  s.psOffsets.emplace(h, alloc.dynamicOffset);
  NoteConstantUpload(false, true);
  return alloc;
}

ConstantAllocation UploadHostConstants(const void *data, u32 size) {
  auto &s = upload_state();
  if (!data || !size)
    return {};
  auto alloc = Allocate(s, size, kCBVAlignment);
  if (!alloc.memory)
    return {};
  std::memcpy(alloc.memory, data, size);
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
  // Only the slots that carried something last time need resetting; every
  // other entry already holds the default from whenever it was last cleared.
  u32 prev_populated = s.populatedSlots;
  while (prev_populated) {
    const u32 i = u32(__builtin_ctz(prev_populated));
    prev_populated &= prev_populated - 1u;
    s.shared.samplerIndices[i] = 0;
    s.shared.texture2DIndices[i] = bd::gpu::kNullTexture2DDescriptorIndex;
    s.shared.texture3DIndices[i] = bd::gpu::kNullTexture3DDescriptorIndex;
    s.shared.textureCubeIndices[i] = bd::gpu::kNullTextureCubeDescriptorIndex;
  }
  u32 populated_now = 0;

  for (u32 i = 0; i < 16; ++i) {

    bd::gpu::GuestTexture *tex = vs.textures[i];
    if (tex && tex->sourceSurface && tex->sourceSurface->texture &&
        tex->resolveScale == 1.0f && // a scaled alias holds the unscaled image
        tex->sourceSurface->sampleCount == plume::RenderSampleCount::COUNT_1 &&
        tex->sourceSurface != vs.render_target &&
        tex->sourceSurface != vs.depth_stencil &&
        tex->sourceSurface->descriptorIndex !=
            bd::gpu::kInvalidDescriptorIndex) {
      tex = tex->sourceSurface;
    }
    if (tex && tex->descriptorIndex != bd::gpu::kInvalidDescriptorIndex) {
      populated_now |= 1u << i;
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
        u32 fc[6] = {
            u32(fc_be.dword[0]), u32(fc_be.dword[1]), u32(fc_be.dword[2]),
            u32(fc_be.dword[3]), u32(fc_be.dword[4]), u32(fc_be.dword[5]),
        };
        if (const auto *ov = vs.material_override; ov && ov->fetch)
          std::memcpy(fc, ov->fetch[i], sizeof(fc));
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
  // Whatever is published now is what the next call has to clear. Getting this
  // wrong leaves a stale descriptor index in a slot the guest has unbound,
  // which is why it is stored from the same variable the loop set.
  s.populatedSlots = populated_now;

  // Shader bool constants: VS at device+0x2700, PS at device+0x2710, 4 BE
  // dwords each. Shaders branch on BOOL_BIT(n) of a 256-bit register file
  // (VS 0..127, PS 128..255).
  if (const auto *ov = vs.material_override; ov && ov->bools) {
    for (u32 i = 0; i < 8; ++i)
      s.shared.booleansArr[i] = ov->bools[i];
  } else if (device_guest) {
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
  // One cvar, one meaning. bd_stereo_separation is calibrated on the
  // side-by-side path, and the multiview shader needs ~23x the same number to
  // produce the same parallax - so without this conversion the knob means two
  // different things depending on a second cvar, which is exactly the trap
  // bd_stereo/bd_stereo_multiview already sprang.
  //
  // Measured in one field scene, both paths, same build (the only comparison
  // that means anything here - see the +/-30% cross-run note):
  //
  //   side-by-side, sep 0.03  ->  far +4  near  -7   spread 11px of a 960 eye
  //   multiview,    sep 0.2   ->  far -2  near  -8   spread  6px of a 1920 layer
  //   multiview,    sep 0.7   ->  far -4  near -26   spread 22px of a 1920 layer
  //
  // Linear in between (3.5x the input, 3.67x the output), and 22px over 1920
  // is the same angle as 11px over 960 - so multiview 0.7 matches
  // side-by-side 0.03 and the ratio is 23.3.
  //
  // WHY it is 23 and not 1 is NOT understood. Both paths add a constant to
  // clip.x - the host at `m[0] += eye_skew`, the shader at
  // `oPos.x += eyeSign * g_StereoSeparation` - and a multiview layer is twice
  // the width of a side-by-side eye, so multiview should need *half*, not
  // twenty-three times. The likeliest suspect is the host's constant landing
  // on a coefficient of a position component that is not w, which would scale
  // it by a typical guest coordinate; Blue Dragon units are centimetres, so
  // that is the right order of magnitude. Not chased, because it would change
  // the path that already works.
  constexpr float kMultiviewSeparationScale = 23.3f;
  s.shared.stereoSeparation =
      stereo_on ? static_cast<float>(REXCVAR_GET(bd_stereo_separation)) *
                      kMultiviewSeparationScale
                : 0.0f;
  s.shared.stereoConvergence =
      stereo_on ? static_cast<float>(REXCVAR_GET(bd_stereo_convergence)) : 0.0f;
  {
    // Counts, not a capped log: the shader applies the skew wherever these are
    // non-zero, so "how many draws got a non-zero separation" is the whole
    // question when both eye layers come out identical.
    static std::atomic<u64> on{0}, off{0};
    (stereo_on ? on : off).fetch_add(1, std::memory_order_relaxed);
    const u64 total = on.load(std::memory_order_relaxed) +
                      off.load(std::memory_order_relaxed);
    if ((total % 20000u) == 0u)
      BD_INFO("[stereo] separation applied to {} draws, zero for {} (sep={})",
              on.load(std::memory_order_relaxed),
              off.load(std::memory_order_relaxed), s.shared.stereoSeparation);
  }

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
  const u64 h = XXH3_64bits(&s.shared, sizeof(SharedConstants));
  if (auto it = s.sharedOffsets.find(h); it != s.sharedOffsets.end()) {
    const u32 off = it->second;
    s.lastUploaded = s.shared;
    if (s.sharedBound && s.boundShared == off)
      return {};
    s.sharedBound = true;
    s.boundShared = off;
    return AllocationAt(s, off, sizeof(SharedConstants));
  }

  auto alloc = Allocate(s, sizeof(SharedConstants), kCBVAlignment);
  if (!alloc.memory)
    return {};
  std::memcpy(alloc.memory, &s.shared, sizeof(SharedConstants));
  s.lastUploaded = s.shared;
  s.sharedBound = true;
  s.boundShared = alloc.dynamicOffset;
  s.sharedOffsets.emplace(h, alloc.dynamicOffset);
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
