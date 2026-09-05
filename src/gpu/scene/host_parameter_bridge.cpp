/**
 * @file    host_parameter_bridge.cpp
 * @brief   Host pass-matrix producers and temporary engine parameter
 * publication.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#include "gpu/scene/host_parameter_bridge.h"
#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/constant_buffers.h"
#include "gpu/device.h"
#include "gpu/frame_stats.h"
#include "gpu/scene/host_draw.h"
#include "gpu/scene/native_transform.h"
#include "gpu/scene/shader_parameter_import.h"
#include <bit>
#include <cstring>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/ppc/context.h>
#include <stdexcept>
#include <vector>

extern "C" void __imp__D3DDevice_SetVertexShaderConstantFN(PPCContext &,
                                                           uint8_t *);
extern "C" void __imp__D3DDevice_SetPixelShaderConstantFN(PPCContext &,
                                                          uint8_t *);
extern "C" void __imp__bdShaderConstantFlush(PPCContext &, uint8_t *);
extern "C" void __imp__sub_821764F8(PPCContext &, uint8_t *);
extern "C" void __imp__sub_82179868(PPCContext &, uint8_t *);
extern "C" void __imp__sub_8217A630(PPCContext &, uint8_t *);
REXCVAR_DECLARE(bool, bd_host_parameters);
REXCVAR_DECLARE(bool, bd_host_parameters_verify);

namespace bd::gpu::scene {
namespace {
constexpr uint32_t kDevice = (uint32_t(-32133) << 16) - 31532;
constexpr uint32_t kPrimary = (uint32_t(-32035) << 16) + 24832;
constexpr uint32_t kPrimaryParameters = (uint32_t(-32136) << 16) + 14936;
constexpr uint32_t kSecondary = (uint32_t(-32137) << 16) + 28452;
using Original = void (*)(PPCContext &, uint8_t *);
thread_local bool reference_execution = false;
struct ReferenceScope {
  ReferenceScope() { reference_execution = true; }
  ~ReferenceScope() { reference_execution = false; }
};
struct Stats {
  uint64_t primary = 0, secondary = 0, inactive = 0, matrices = 0;
  uint64_t flushes = 0, vertex = 0, pixel = 0, vectors = 0;
  uint64_t compatibility = 0, refused = 0, checked = 0, wrong = 0, calls = 0;
  uint32_t frame = 0;
};
thread_local Stats stats;
void Report() {
  if ((++stats.calls & 1023) != 0)
    return;
  const auto frame = FrameStatFrameCount();
  if (frame - stats.frame < 300)
    return;
  BD_INFO(
      "[host-parameters] projection primary {} secondary {} inactive {} "
      "matrix writes {}; flushes {} VS {} PS {} vectors {}; compatibility {} "
      "refused {}; checked {} wrong {}; engine inputs/parameter descriptors, "
      "inline writers and draw-time shader-register import remain",
      stats.primary, stats.secondary, stats.inactive, stats.matrices,
      stats.flushes, stats.vertex, stats.pixel, stats.vectors,
      stats.compatibility, stats.refused, stats.checked, stats.wrong);
  stats.frame = frame;
}
bool Range(uint64_t address, uint64_t bytes) {
  if (!address || !bytes || address > UINT32_MAX || bytes > UINT32_MAX ||
      address + bytes - 1 > UINT32_MAX ||
      !bd::mem::try_at<uint8_t>(uint32_t(address)))
    return false;
  for (uint64_t page = (address & ~uint64_t(4095)) + 4096;
       page < address + bytes; page += 4096)
    if (!bd::mem::try_at<uint8_t>(uint32_t(page)))
      return false;
  return true;
}
bool Overlap(uint64_t a, uint64_t a_bytes, uint64_t b, uint64_t b_bytes) {
  return a_bytes && b_bytes && a < b + b_bytes && b < a + a_bytes;
}
void Checked(bool same, const char *kind) {
  ++stats.checked;
  if (same)
    return;
  ++stats.wrong;
  BD_ERROR("[host-parameters] {} publication mismatch", kind);
  throw std::runtime_error("Host parameter publication differs from original");
}
void Fallback(PPCContext &ctx, uint8_t *base, Original original) {
  ++stats.compatibility;
  stats.refused += REXCVAR_GET(bd_host_parameters);
  original(ctx, base);
}
void Note(bool vertex, uint32_t first, uint32_t count, uint32_t source) {
  // These are still shader-register adapters, not native material ownership.
  NoteGuestConstantWrite();
  if (vertex)
    Video::MarkVSConstantsDirty();
  else
    Video::MarkPSConstantsDirty();
  NoteConstantsSet(vertex, first, count);
  NoteConstantsSource(vertex, first, count, source);
}
struct Transfer {
  uint32_t device, first, source, count;
  uint64_t dirty;
  bool vertex;
  uint32_t Destination() const {
    return device + (vertex ? 0x700 : 0x1700) + first * 16;
  }
  bool Supported() const {
    return ParameterRangeSupported(first, count) && !(device & 15) &&
           Range(device, kD3DDeviceAllocSize) &&
           (!count || Range(source, uint64_t(count) * 16));
  }
  bool StackIndependent(uint64_t scratch, uint64_t bytes) const {
    return !Overlap(source, uint64_t(count) * 16, scratch, bytes) &&
           !Overlap(device, kD3DDeviceAllocSize, scratch, bytes);
  }
  template <class Load, class Store> void Apply(Load load, Store store) const {
    CopyParameterRows(
        count, [&](uint32_t offset) { return load(source + offset); },
        [&](uint32_t offset, uint32_t word) {
          store(Destination() + offset, word);
        });
    const auto mask_address = device + (vertex ? 0 : 8);
    const uint64_t mask =
        (uint64_t(load(mask_address)) << 32) | load(mask_address + 4);
    store(mask_address, uint32_t((mask | dirty) >> 32));
    store(mask_address + 4, uint32_t(mask | dirty));
  }
  void Publish() const {
    Apply([](uint32_t address) { return bd::mem::load<uint32_t>(address); },
          [](uint32_t address, uint32_t word) {
            bd::mem::store<uint32_t>(address, word);
          });
    Note(vertex, first, count, source);
  }
  void Count() const {
    ++(vertex ? stats.vertex : stats.pixel);
    stats.vectors += count;
  }
};
// Only diagnostic execution allocates a snapshot. Reads consult it byte-wise,
// so unaligned sources crossing its edges and VS -> PS aliasing stay
// sequential.
struct DevicePrediction {
  uint32_t address;
  std::vector<uint8_t> bytes;
  explicit DevicePrediction(uint32_t device) : address(device) {
    const auto *data = bd::mem::at<const uint8_t>(device);
    bytes.assign(data, data + kD3DDeviceAllocSize);
  }
  void Apply(const Transfer &transfer) {
    transfer.Apply(
        [&](uint32_t at) {
          uint32_t word = 0;
          for (uint32_t i = 0; i < 4; ++i) {
            const auto offset = uint64_t(at) + i - address;
            word = (word << 8) |
                   (offset < bytes.size() ? bytes[size_t(offset)]
                                          : bd::mem::load<uint8_t>(at + i));
          }
          return word;
        },
        [&](uint32_t at, uint32_t word) {
          const auto be = std::byteswap(word);
          std::memcpy(bytes.data() + at - address, &be, 4);
        });
  }
  bool Matches() const {
    return !std::memcmp(bytes.data(), bd::mem::at<const uint8_t>(address),
                        bytes.size());
  }
};
bool Flush(PPCContext &ctx, uint8_t *base) {
  const auto descriptor = ctx.r3.u32;
  const auto scratch = uint64_t(ctx.r1.u32) - 160;
  if (!Range(descriptor, 16) || Overlap(descriptor, 16, scratch, 160))
    return false;
  const auto flags = bd::mem::load<uint32_t>(descriptor) & 3;
  if (!flags) {
    if (REXCVAR_GET(bd_host_parameters_verify)) {
      const auto result = ctx.r3.u64;
      ReferenceScope scope;
      __imp__bdShaderConstantFlush(ctx, base);
      Checked(ctx.r3.u64 == result, "empty flush");
    }
    ++stats.flushes;
    return true;
  }
  if (!Range(kDevice, 4))
    return false;
  const auto device = bd::mem::load<uint32_t>(kDevice);
  const auto first = bd::mem::load<uint32_t>(descriptor + 4);
  const auto count = bd::mem::load<uint32_t>(descriptor + 8) - first;
  const auto source = bd::mem::load<uint32_t>(descriptor + 12);
  Transfer transfer{
      device, first, source, count, ImportParameterDirtyMask(first, count),
      true};
  // Original code rereads control data after VS. Unsupported control aliases
  // fall back before any effect; source aliasing remains supported.
  if (!transfer.Supported() || !transfer.StackIndependent(scratch, 160) ||
      Overlap(descriptor, 16, device, kD3DDeviceAllocSize) ||
      Overlap(kDevice, 4, device, kD3DDeviceAllocSize))
    return false;
  const bool verify = REXCVAR_GET(bd_host_parameters_verify);
  if (verify) {
    DevicePrediction prediction(device);
    for (uint32_t stage = 0; stage < 2; ++stage)
      if (flags & (1u << stage)) {
        transfer.vertex = stage == 0;
        prediction.Apply(transfer);
        transfer.Count();
      }
    ReferenceScope scope;
    __imp__bdShaderConstantFlush(ctx, base);
    Checked(ctx.r3.u64 == device && prediction.Matches(), "flush");
  } else {
    for (uint32_t stage = 0; stage < 2; ++stage)
      if (flags & (1u << stage)) {
        transfer.vertex = stage == 0;
        transfer.Publish();
        transfer.Count();
      }
    ctx.r3.u64 = device;
  }
  ++stats.flushes;
  return true;
}
RenderMatrix ReadMatrix(uint32_t address) {
  RenderMatrix matrix;
  for (uint32_t i = 0; i < 16; ++i)
    matrix[i] = bd::mem::load<float>(address + i * 4);
  return matrix;
}
uint32_t MatrixDestination(uint64_t descriptor, uint64_t scratch,
                           uint64_t scratch_bytes) {
  if (!Range(descriptor, 16) || Overlap(descriptor, 16, scratch, scratch_bytes))
    return 0;
  const auto owner = bd::mem::load<uint32_t>(uint32_t(descriptor) + 4);
  if (!owner || !Range(uint64_t(owner) + 12, 4) ||
      Overlap(uint64_t(owner) + 12, 4, scratch, scratch_bytes))
    return 0;
  const auto buffer = bd::mem::load<uint32_t>(owner + 12);
  const auto index = bd::mem::load<uint32_t>(uint32_t(descriptor) + 12);
  const uint64_t destination = uint64_t(buffer) + uint64_t(index) * 16;
  return buffer && Range(destination, 64) ? uint32_t(destination) : 0;
}
struct MatrixPublication {
  uint32_t destination;
  RenderMatrix matrix;
  uint64_t result;
  bool primary_scalars = false;
  float enabled = 0, texel_size = 0;
  void Execute(PPCContext &ctx, uint8_t *base, Original original,
               bool arithmetic) const {
    if (REXCVAR_GET(bd_host_parameters_verify)) {
      ReferenceScope scope;
      original(ctx, base);
      bool same = ctx.r3.u64 == result;
      for (uint32_t i = 0; i < 16; ++i) {
        const auto actual = bd::mem::load<float>(destination + i * 4);
        const auto expected = matrix[i];
        same &=
            std::bit_cast<uint32_t>(actual) ==
                std::bit_cast<uint32_t>(expected) ||
            (arithmetic && ((std::isnan(actual) && std::isnan(expected)) ||
                            (std::isfinite(actual) && std::isfinite(expected) &&
                             std::abs(actual - expected) <=
                                 0.00001f * (1 + std::abs(expected)))));
      }
      if (primary_scalars)
        same &= bd::mem::load<uint32_t>(kPrimaryParameters + 60) ==
                    std::bit_cast<uint32_t>(enabled) &&
                bd::mem::load<uint32_t>(kPrimaryParameters + 64) ==
                    std::bit_cast<uint32_t>(texel_size);
      Checked(same, arithmetic ? "projection" : "matrix transpose");
      return;
    }
    for (uint32_t i = 0; i < 16; ++i)
      bd::mem::store<float>(destination + i * 4, matrix[i]);
    if (primary_scalars) {
      bd::mem::store<float>(kPrimaryParameters + 60, enabled);
      bd::mem::store<float>(kPrimaryParameters + 64, texel_size);
    }
    ctx.r3.u64 = result;
  }
};
bool MatrixWrite(PPCContext &ctx, uint8_t *base) {
  const auto source = ctx.r4.u32;
  // The original snapshots via stack before reading the descriptor. Refuse
  // unusual stack aliases that would modify either source or control data.
  const auto scratch = uint64_t(ctx.r1.u32) - 64;
  if (!Range(source, 64) ||
      (Overlap(source, 64, scratch, 64) && source != scratch) ||
      Overlap(ctx.r3.u32, 16, scratch, 64))
    return false;
  const auto destination = MatrixDestination(ctx.r3.u32, scratch, 64);
  if (!destination || (ctx.r1.u32 & 15))
    return false;
  MatrixPublication publication{
      destination, TransposeRenderMatrix(ReadMatrix(source)), ctx.r3.u64};
  publication.Execute(ctx, base, __imp__sub_8217A630, false);
  ++stats.matrices;
  return true;
}
bool Projection(PPCContext &ctx, uint8_t *base, bool secondary) {
  const auto source = secondary ? kSecondary : ctx.r3.u32;
  const auto original = secondary ? __imp__sub_82179868 : __imp__sub_821764F8;
  const auto scratch = uint64_t(ctx.r1.u32) - 304;
  if (!source || !Range(uint64_t(source) + 8, 1) || (ctx.r1.u32 & 15) ||
      Overlap(source, secondary ? 172 : 380, scratch, 328))
    return false;
  if (!bd::mem::load<uint8_t>(source + 8)) {
    if (REXCVAR_GET(bd_host_parameters_verify)) {
      const auto result = ctx.r3.u64;
      ReferenceScope scope;
      original(ctx, base);
      Checked(ctx.r3.u64 == result, "inactive projection");
    }
    ++stats.inactive;
    return true;
  }
  const auto descriptor_offset = secondary ? 156u : 364u;
  const auto matrix_offset = secondary ? 16u : 68u;
  const auto destination =
      MatrixDestination(uint64_t(source) + descriptor_offset, scratch, 328);
  if (!destination || !Range(uint64_t(source) + matrix_offset, 128))
    return false;
  MatrixPublication publication{
      destination,
      {},
      secondary ? uint64_t(int64_t(int32_t(kSecondary))) + descriptor_offset
                : ctx.r3.u64 + descriptor_offset};
  publication.primary_scalars = !secondary && source == kPrimary;
  constexpr uint32_t kEnabled = (uint32_t(-32247) << 16) - 4500;
  constexpr uint32_t kDisabled = (uint32_t(-32247) << 16) - 4612;
  constexpr uint32_t kNumerator = (uint32_t(-32250) << 16) + 8120;
  constexpr uint32_t kDimension = (uint32_t(-32137) << 16) + 28048 + 12;
  if (publication.primary_scalars) {
    for (const auto address :
         {kEnabled, kDisabled, kNumerator, kDimension, source + 296})
      if (!Range(address, 4) || Overlap(address, 4, destination, 64))
        return false;
    if (!Range(kPrimaryParameters + 60, 8) ||
        Overlap(kPrimaryParameters + 60, 8, destination, 64))
      return false;
  }
  ctx.fpscr.enableFlushMode();
  publication.matrix = TransposeRenderMatrix(
      MultiplyRenderMatrices(ReadMatrix(source + matrix_offset),
                             ReadMatrix(source + matrix_offset + 64)));
  if (publication.primary_scalars) {
    ctx.fpscr.disableFlushMode();
    publication.enabled = float(double(bd::mem::load<float>(
        bd::mem::load<uint8_t>(source + 296) ? kEnabled : kDisabled)));
    const auto dimension = float(bd::mem::load<int32_t>(kDimension));
    publication.texel_size =
        float(double(bd::mem::load<float>(kNumerator)) / double(dimension));
  }
  publication.Execute(ctx, base, original, true);
  ++(secondary ? stats.secondary : stats.primary);
  return true;
}
} // namespace

void SetHostFloatParameters(PPCContext &ctx, uint8_t *base, bool vertex) {
  const auto original = vertex ? __imp__D3DDevice_SetVertexShaderConstantFN
                               : __imp__D3DDevice_SetPixelShaderConstantFN;
  const Transfer transfer{ctx.r3.u32, ctx.r4.u32, ctx.r5.u32,
                          ctx.r6.u32, ctx.r7.u64, vertex};
  if (reference_execution) {
    original(ctx, base);
    Note(vertex, transfer.first, transfer.count, transfer.source);
    return;
  }
  // The old leaf spills device and dirty at SP-32 and SP+48. Do not convert
  // callers that alias those writes with parameter input or the device.
  if (!REXCVAR_GET(bd_host_parameters) || !transfer.Supported() ||
      !transfer.StackIndependent(uint64_t(ctx.r1.u32) - 32, 4) ||
      !transfer.StackIndependent(uint64_t(ctx.r1.u32) + 48, 8)) {
    Fallback(ctx, base, original);
    Note(vertex, transfer.first, transfer.count, transfer.source);
  } else {
    if (REXCVAR_GET(bd_host_parameters_verify)) {
      DevicePrediction prediction(transfer.device);
      prediction.Apply(transfer);
      const auto result = ctx.r3.u64;
      ReferenceScope scope;
      original(ctx, base);
      Checked(ctx.r3.u64 == result && prediction.Matches(), "float setter");
      Note(vertex, transfer.first, transfer.count, transfer.source);
    } else {
      transfer.Publish();
    }
    transfer.Count();
  }
  Report();
}
} // namespace bd::gpu::scene

#define HOST_PARAMETER_HOOK(Name, Action)                                      \
  REX_HOOK_RAW(Name) {                                                         \
    using namespace bd::gpu::scene;                                            \
    if (reference_execution) {                                                 \
      __imp__##Name(ctx, base);                                                \
      return;                                                                  \
    }                                                                          \
    if (!REXCVAR_GET(bd_host_parameters) || !(Action))                         \
      Fallback(ctx, base, __imp__##Name);                                      \
    Report();                                                                  \
  }
HOST_PARAMETER_HOOK(bdShaderConstantFlush, Flush(ctx, base))
HOST_PARAMETER_HOOK(sub_8217A630, MatrixWrite(ctx, base))
HOST_PARAMETER_HOOK(sub_821764F8, Projection(ctx, base, false))
HOST_PARAMETER_HOOK(sub_82179868, Projection(ctx, base, true))
#undef HOST_PARAMETER_HOOK
