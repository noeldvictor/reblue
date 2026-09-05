/**
 * @file    native_sampler_bridge.cpp
 * @brief   Host scene sampler defaults and supported sampler setter execution.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#include "gpu/scene/sampler_import.h"
#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/device.h"
#include "gpu/frame_stats.h"
#include "gpu/scene/host_draw.h"
#include <cstring>
#include <stdexcept>
#include <vector>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/ppc/context.h>

extern "C" void __imp__bdSetSamplerState(PPCContext &, uint8_t *);
extern "C" void __imp__sub_82184A88(PPCContext &, uint8_t *);
REXCVAR_DECLARE(bool, bd_host_sampler_state);
REXCVAR_DECLARE(bool, bd_host_sampler_verify);

namespace bd::gpu::scene {
namespace {
constexpr uint32_t kCache = 0x82DBE330;
constexpr uint32_t kDevice = (uint32_t(-32133) << 16) - 31532;
constexpr uint32_t kSettings = (uint32_t(-32035) << 16) - 26552;
constexpr uint32_t kDirty = (uint32_t(-32034) << 16) - 19936 + 12;
constexpr uint32_t kAnisotropyTable = 0x82001630;
using Original = void (*)(PPCContext &, uint8_t *);
// All engine sampler production is render-thread serialized. No video/registry
// lock or per-call atomic counters are introduced on this hot boundary.
thread_local bool reference_execution = false;
struct Stats {
  uint64_t defaults = 0, default_changes = 0, changes = 0, unchanged = 0;
  uint64_t compatibility = 0, refused = 0, checked = 0, wrong = 0, calls = 0;
  std::array<uint64_t, 7> setters{};
  uint32_t frame = 0;
};
thread_local Stats stats;
void Report() {
  if ((++stats.calls & 1023) != 0)
    return;
  const auto frame = FrameStatFrameCount();
  if (frame - stats.frame < 300)
    return;
  BD_INFO("[host-sampler] defaults {} default changes {} engine changes {} "
          "unchanged {}; setters U {} V {} W {} border {} mag {} min {} mip {}; "
          "compatibility {} refused {}; publications checked {} wrong {}; "
          "inline material writers and draw-time fetch import remain",
          stats.defaults, stats.default_changes, stats.changes, stats.unchanged,
          stats.setters[0], stats.setters[1], stats.setters[2], stats.setters[3],
          stats.setters[4], stats.setters[5], stats.setters[6], stats.compatibility,
          stats.refused, stats.checked, stats.wrong);
  stats.frame = frame;
}
bool Range(uint64_t address, uint64_t bytes) {
  if (!address || !bytes || address + bytes - 1 > UINT32_MAX ||
      !bd::mem::try_at<uint8_t>(uint32_t(address)))
    return false;
  for (uint64_t page = (address & ~uint64_t(4095)) + 4096;
       page < address + bytes; page += 4096)
    if (!bd::mem::try_at<uint8_t>(uint32_t(page)))
      return false;
  return true;
}
uint32_t Device() {
  if (!Range(kDevice, 4))
    return 0;
  const auto device = bd::mem::load<uint32_t>(kDevice);
  return Range(device, kD3DDeviceAllocSize) ? device : 0;
}
bool ReadShadow(uint32_t device, uint32_t slot, bool filters, SamplerShadow &s) {
  if (slot >= 32 || !Range(device, kD3DDeviceAllocSize))
    return false;
  for (uint32_t i = 0; i < 6; ++i)
    s.fetch[i] = bd::mem::load<uint32_t>(device + 1024 + slot * 24 + i * 4);
  s.dirty = bd::mem::load<uint64_t>(device + 16);
  if (filters) {
    const auto selector = bd::mem::load<uint8_t>(device + 11652 + slot);
    const auto address = kAnisotropyTable + selector * 4;
    if (!Range(address, 4))
      return false;
    s.anisotropy_lookup = bd::mem::load<uint32_t>(address);
    s.z_filter = bd::mem::load<uint8_t>(device + 11730 + slot);
  }
  return true;
}
struct Write {
  uint32_t address, bytes;
  uint64_t value;
};
struct Region {
  uint32_t address;
  std::vector<uint8_t> expected;
};
struct Publication {
  std::vector<Write> writes;
  std::vector<Region> regions;
  uint64_t result;
  void Watch(uint32_t address, uint32_t bytes) {
    if (!REXCVAR_GET(bd_host_sampler_verify))
      return;
    const auto *data = bd::mem::at<const uint8_t>(address);
    regions.push_back({address, std::vector<uint8_t>(data, data + bytes)});
  }
  template <class T> void Add(uint32_t address, T value) {
    writes.push_back({address, sizeof(T), uint64_t(value)});
    const auto be = std::byteswap(value);
    for (auto &region : regions)
      if (address >= region.address &&
          uint64_t(address) + sizeof(T) <= uint64_t(region.address) + region.expected.size())
        std::memcpy(region.expected.data() + address - region.address, &be, sizeof(T));
  }
  void AddShadow(uint32_t device, uint32_t slot, SamplerField field,
                 const SamplerShadow &shadow) {
    WriteSamplerShadow(shadow, slot, field, [&](uint32_t at, auto value) {
      Add(device + at, value);
    });
  }
  void Execute(PPCContext &ctx, uint8_t *base, Original original) const {
    if (REXCVAR_GET(bd_host_sampler_verify)) {
      // Execute the original exactly once, with nested setter replacements
      // bypassed. Compare the whole device plus cache/dirty publication, not
      // just sampler bits. No native writes hide an original mismatch.
      struct ReferenceScope {
        ReferenceScope() { reference_execution = true; }
        ~ReferenceScope() { reference_execution = false; }
      } scope;
      original(ctx, base);
      bool same = ctx.r3.u64 == result;
      for (const auto &region : regions)
        same &= !std::memcmp(region.expected.data(),
                             bd::mem::at<const uint8_t>(region.address),
                             region.expected.size());
      ++stats.checked;
      if (!same) {
        ++stats.wrong;
        BD_ERROR("[host-sampler] original publication mismatch, expected r3 {:x}, got {:x}",
                 result, ctx.r3.u64);
        throw std::runtime_error("Host sampler publication differs from original");
      }
      return;
    }
    for (const auto &write : writes) {
      if (write.bytes == 8)
        bd::mem::store<uint64_t>(write.address, write.value);
      else
        bd::mem::store<uint32_t>(write.address, uint32_t(write.value));
    }
    ctx.r3.u64 = result;
  }
};
bool Set(PPCContext &ctx, uint8_t *base, Original original,
         uint32_t device, uint32_t slot, SamplerField field, bool engine) {
  SamplerShadow shadow;
  const auto offset = SamplerOffset(field);
  if (!ReadShadow(device, slot, field == SamplerField::MinFilter ||
                               field == SamplerField::MagFilter, shadow))
    return false;
  if (engine && (!Range(kDirty, 4) ||
      bd::mem::load<uint32_t>(device + 444 + offset) != kSamplerSetters[uint32_t(field)]))
    return false;
  Publication publication{{}, {}, engine ? uint64_t(device) : ctx.r3.u64};
  publication.Watch(device, kD3DDeviceAllocSize);
  const auto cache = kCache + slot * 80 + offset;
  if (engine) {
    publication.Watch(cache, 4);
    publication.Watch(kDirty, 4);
  }
  PublishSamplerShadow(shadow, slot, field, ctx.r5.u32);
  publication.AddShadow(device, slot, field, shadow);
  if (engine) {
    publication.Add(cache, ctx.r5.u32);
    publication.Add(kDirty, 1u);
  }
  publication.Execute(ctx, base, original);
  ++stats.setters[uint32_t(field)];
  stats.changes += engine;
  return true;
}
bool Defaults(PPCContext &ctx, uint8_t *base) {
  const auto device = Device();
  if (!device || !Range(kSettings, 4) || !Range(kCache, 400) || !Range(kDirty, 4))
    return false;
  const auto settings = bd::mem::load<uint32_t>(kSettings);
  if (!settings || !Range(uint64_t(settings) + 7048, 12))
    return false;
  const auto plan = SceneSamplerDefaults(bd::mem::load<uint32_t>(settings + 7052),
      bd::mem::load<uint32_t>(settings + 7056), bd::mem::load<uint32_t>(settings + 7048));
  std::array<SamplerShadow, 5> shadows;
  for (uint32_t slot = 0; slot < 5; ++slot)
    if (!ReadShadow(device, slot, true, shadows[slot]))
      return false;
  Publication publication{{}, {}, ctx.r3.u64};
  publication.Watch(device, kD3DDeviceAllocSize);
  publication.Watch(kCache, 400);
  publication.Watch(kDirty, 4);
  uint64_t dirty = shadows[0].dirty;
  uint32_t changes = 0;
  for (const auto &command : plan) {
    const auto cache = kCache + command.slot * 80 + SamplerOffset(command.field);
    if (bd::mem::load<uint32_t>(cache) == command.value)
      continue;
    auto &shadow = shadows[command.slot];
    shadow.dirty = dirty;
    PublishSamplerShadow(shadow, command.slot, command.field, command.value);
    dirty = shadow.dirty;
    publication.AddShadow(device, command.slot, command.field, shadow);
    publication.Add(cache, command.value);
    publication.Add(kDirty, 1u);
    if (command.field == SamplerField::MinFilter || command.field == SamplerField::MagFilter)
      publication.result = device;
    ++changes;
  }
  publication.Execute(ctx, base, __imp__sub_82184A88);
  ++stats.defaults;
  stats.default_changes += changes;
  return true;
}
void Fallback(PPCContext &ctx, uint8_t *base, Original original) {
  ++stats.compatibility;
  stats.refused += REXCVAR_GET(bd_host_sampler_state);
  original(ctx, base);
}
void Direct(PPCContext &ctx, uint8_t *base, Original original, SamplerField field) {
  if (reference_execution) {
    original(ctx, base);
    return;
  }
  if (!REXCVAR_GET(bd_host_sampler_state) ||
      !Set(ctx, base, original, ctx.r3.u32, ctx.r4.u32, field, false))
    Fallback(ctx, base, original);
  Report();
}
} // namespace
} // namespace bd::gpu::scene

REX_HOOK_RAW(bdSetSamplerState) {
  using namespace bd::gpu::scene;
  NoteSamplerSet(ctx.r3.u32);
  const auto slot = ctx.r3.u32, offset = ctx.r4.u32;
  // Keep the existing cheap early-out, including the 13 not-yet-converted
  // fields. Unknown/unaligned offsets retain exact original dispatch semantics.
  if (REXCVAR_GET(bd_host_sampler_state) && slot < 32 && offset < 80 && !(offset & 3)) {
    const auto *cached = bd::mem::at<const be_u32>(kCache + slot * 80 + offset);
    if (cached && uint32_t(*cached) == ctx.r5.u32) {
      ++stats.unchanged;
      Report();
      return;
    }
    if (const auto field = SamplerImportField(offset); cached && field) {
      const auto device = Device();
      if (device && Set(ctx, base, __imp__bdSetSamplerState, device, slot, *field, true)) {
        Report();
        return;
      }
    }
  }
  Fallback(ctx, base, __imp__bdSetSamplerState);
  Report();
}
REX_HOOK_RAW(sub_82184A88) {
  using namespace bd::gpu::scene;
  if (!REXCVAR_GET(bd_host_sampler_state) || !Defaults(ctx, base))
    Fallback(ctx, base, __imp__sub_82184A88);
  Report();
}
#define HOST_SAMPLER_SETTER(Name, Field) \
  extern "C" void __imp__D3DDevice_SetSamplerState_##Name(PPCContext &, uint8_t *); \
  REX_HOOK_RAW(D3DDevice_SetSamplerState_##Name) { \
    bd::gpu::scene::Direct(ctx, base, __imp__D3DDevice_SetSamplerState_##Name, \
                          bd::gpu::scene::SamplerField::Field); \
  }
HOST_SAMPLER_SETTER(AddressU, AddressU)
HOST_SAMPLER_SETTER(AddressV, AddressV)
HOST_SAMPLER_SETTER(AddressW, AddressW)
HOST_SAMPLER_SETTER(BorderColor, BorderColor)
HOST_SAMPLER_SETTER(MagFilter, MagFilter)
HOST_SAMPLER_SETTER(MinFilter, MinFilter)
HOST_SAMPLER_SETTER(MipFilter, MipFilter)
#undef HOST_SAMPLER_SETTER
