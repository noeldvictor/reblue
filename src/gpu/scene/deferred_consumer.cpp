/**
 * @file    deferred_consumer.cpp
 * @brief   Host scheduling, surface expansion and cleanup of deferred work.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#include "gpu/scene/deferred_consumer.h"
#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/constant_buffers.h"
#include "gpu/device.h"
#include "gpu/frame_stats.h"
#include "gpu/hooks/draw_dispatch.h"
#include "gpu/scene/deferred_depth_import.h"
#include "gpu/scene/deferred_entry_bridge.h"
#include "gpu/scene/deferred_shader_bridge.h"
#include "gpu/scene/deferred_surface.h"
#include "gpu/scene/deferred_work.h"
#include "gpu/scene/host_draw.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <initializer_list>
#include <rex/ppc/context.h>
#include <rex/system/function_dispatcher.h>
#include <stdexcept>
#include <vector>

extern "C" {
void bdSetRenderState(PPCContext &, uint8_t *);
void bdSetSamplerState(PPCContext &, uint8_t *);
void bdBuildViewMatrix(PPCContext &, uint8_t *);
void sub_8221DBE0(PPCContext &, uint8_t *);
void sub_8221DCA0(PPCContext &, uint8_t *);
void sub_82425C28(PPCContext &, uint8_t *);
void sub_82286228(PPCContext &, uint8_t *);
void D3DDevice_SetTexture(PPCContext &, uint8_t *);
void D3DDevice_SetIndices(PPCContext &, uint8_t *);
void D3DDevice_SetStreamSource(PPCContext &, uint8_t *);
void D3DDevice_SetVertexDeclaration(PPCContext &, uint8_t *);
}
bool bdRenderListEntryHook(PPCRegister &, PPCRegister &);

namespace bd::gpu::scene {
namespace {
constexpr uint32_t High(int32_t value) { return uint32_t(value) << 16; }
constexpr uint32_t kList = 0x82DBA8F8;
constexpr uint32_t kEngineState = High(-32034) - 19936;
constexpr uint32_t kPassMode = High(-32035) - 26711;
constexpr uint32_t kSortDisabled = High(-32035) - 26168;
constexpr uint32_t kArenaStats = High(-32036) - 5548;
constexpr uint32_t kObjectMode = High(-32036) - 5536;
constexpr uint32_t kVisualContext = High(-32030) - 31132;
constexpr uint32_t kMaterialStamp = High(-32133) - 31628;
constexpr uint32_t kDevice = High(-32133) - 31532;
constexpr uint32_t kDeclarationCache = High(-32036) - 6096 + 512;
constexpr uint32_t kRenderStateCache = 0x82DBE1A8;
constexpr uint32_t kFoliage = High(-32034) - 22100;
constexpr uint32_t kDrawStats = High(-32036) - 5564 + 8;

enum class BridgeKind {
  Visual,
  Material,
  State,
  World,
  Resource,
  Shader,
  Count
};
struct Stats {
  uint64_t lists = 0, entries = 0, replayed = 0, draws = 0, shells = 0,
           stencil = 0;
  uint64_t fallback = 0, refused = 0;
  std::array<uint64_t, size_t(BridgeKind::Count)> bridges{};
  uint32_t frame = 0;
};
thread_local Stats stats;
void Report() {
  const auto frame = FrameStatFrameCount();
  if (frame - stats.frame < 300)
    return;
  BD_INFO(
      "[host-consumer] lists {} entries {} replayed {}; direct draws {} shells "
      "{} stencil {}; "
      "bridges visual {} material {} state {} world {} resource {} shader {}; "
      "fallback {} refused {} (cumulative, engine adapters remain)",
      stats.lists, stats.entries, stats.replayed, stats.draws, stats.shells,
      stats.stencil, stats.bridges[0], stats.bridges[1], stats.bridges[2],
      stats.bridges[3], stats.bridges[4], stats.bridges[5], stats.fallback,
      stats.refused);
  stats.frame = frame;
}
uint8_t *Range(uint64_t address, uint64_t bytes) {
  if (!address || !bytes || address > UINT32_MAX || bytes > UINT32_MAX ||
      address + bytes - 1 > UINT32_MAX)
    return nullptr;
  auto *ptr = bd::mem::try_at<uint8_t>(uint32_t(address));
  if (!ptr)
    return nullptr;
  for (uint64_t page = (address & ~uint64_t(4095)) + 4096;
       page < address + bytes; page += 4096)
    if (!bd::mem::try_at<uint8_t>(uint32_t(page)))
      return nullptr;
  return ptr;
}
template <class T> T Read(uint32_t address) {
  return bd::mem::load<T>(address);
}
template <class T> void Write(uint32_t address, T value) {
  bd::mem::store<T>(address, value);
}

// Compatibility addresses live only in this import record/adapter. The host
// owns iteration and submission decisions, not a translated PPC register loop.
struct ImportedEntry {
  uint32_t address = 0;
  float depth = 0;
};
bool ImportList(std::vector<ImportedEntry> &entries) {
  if (!Range(kList, 44))
    return false;
  const auto count = Read<uint32_t>(kList + 20);
  const auto array = Read<uint32_t>(kList + 12);
  const auto pool = Read<uint32_t>(kList + 8);
  const auto cursor = Read<uint32_t>(kList + 4);
  if (count > 5140 || !pool || ((pool | cursor | array) & 3) || cursor < pool ||
      uint64_t(pool) + 4194304 > UINT32_MAX ||
      uint64_t(array) + uint64_t(count) * 4 != Read<uint32_t>(kList + 16) ||
      uint64_t(cursor) - pool > 4194304 || !Range(pool, kDeferredEntryBytes) ||
      (count && !Range(array, uint64_t(count) * 4)))
    return false;
  entries.clear();
  entries.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    const auto entry = Read<uint32_t>(array + i * 4);
    const auto *data = Range(entry, kDeferredEntryBytes);
    if (!data || (entry & 3) || entry < pool ||
        uint64_t(entry) + kDeferredEntryBytes > cursor)
      return false;
    const int bones = int(int8_t(data[289]));
    const uint32_t bytes =
        kDeferredEntryBytes + uint32_t(std::max(bones, 0)) * 4;
    if (bones > 49 || !Range(entry, bytes) || uint64_t(entry) + bytes > cursor)
      return false;
    const auto visual = Read<uint32_t>(entry + 272);
    if (visual && !Range(visual, 3672))
      return false;
    const auto declaration = Read<uint32_t>(entry + 376);
    if (!Range(declaration, 16))
      return false;
    if (bones > 0) {
      const auto palette = Read<uint32_t>(entry + 268);
      for (int bone = 0; bone < bones; ++bone)
        if (!Range(uint64_t(palette) +
                       uint64_t(Read<uint32_t>(entry + 800 + bone * 4)) * 64,
                   64))
          return false;
    }
    entries.push_back({entry, Read<float>(entry + 276)});
  }
  if (!Range(Read<uint32_t>(kDevice), 0x2720))
    return false;
  auto *dispatcher = REX_KERNEL_STATE()->function_dispatcher();
  return dispatcher->GetFunction(Read<uint32_t>(kList + 36)) &&
         dispatcher->GetFunction(Read<uint32_t>(kList + 40));
}

struct EngineBridge {
  PPCContext &ctx;
  uint8_t *base;
  uint32_t device;
  uint64_t saved_stack;
  EngineBridge(PPCContext &context, uint8_t *memory)
      : ctx(context), base(memory), device(Read<uint32_t>(kDevice)),
        saved_stack(ctx.r1.u64) {
    // Only engine callbacks need a guest ABI frame; all consumer locals,
    // matrix gathering and loop state are native. Never overwrite the caller.
    ctx.r1.u32 -= 128;
    Write<uint32_t>(ctx.r1.u32, uint32_t(saved_stack));
  }
  ~EngineBridge() { ctx.r1.u64 = saved_stack; }
  uint32_t Call(PPCFunc *fn, BridgeKind kind,
                std::initializer_list<uint64_t> args = {}) {
    std::array<PPCRegister *, 8> registers{&ctx.r3, &ctx.r4, &ctx.r5, &ctx.r6,
                                           &ctx.r7, &ctx.r8, &ctx.r9, &ctx.r10};
    size_t index = 0;
    for (auto value : args)
      registers[index++]->u64 = value;
    ++stats.bridges[size_t(kind)];
    fn(ctx, base);
    return ctx.r3.u32;
  }
  uint32_t Material(uint32_t slot, std::initializer_list<uint64_t> args = {}) {
    auto *fn = REX_KERNEL_STATE()->function_dispatcher()->GetFunction(
        Read<uint32_t>(kList + slot));
    if (!fn)
      throw std::runtime_error(
          "Deferred material callback disappeared during consumption");
    return Call(fn, BridgeKind::Material, args);
  }
  void State(uint32_t state, uint32_t value) {
    Call(bdSetRenderState, BridgeKind::State, {state, value});
  }
  void MarkFloats(bool vertex, uint32_t first, uint32_t count,
                  uint32_t source = 0) {
    const auto mask = DeferredConstantMask(first, count);
    if (!mask)
      throw std::runtime_error("Invalid deferred constant range");
    const auto mask_address = device + (vertex ? 0 : 8);
    Write<uint64_t>(mask_address, Read<uint64_t>(mask_address) | *mask);
    NoteGuestConstantWrite();
    if (vertex)
      Video::MarkVSConstantsDirty();
    else
      Video::MarkPSConstantsDirty();
    NoteConstantsSet(vertex, first, count);
    if (source)
      NoteConstantsSource(vertex, first, count, source);
  }
  void CopyFloats(bool vertex, uint32_t first, const uint8_t *bytes,
                  uint32_t count, uint32_t source = 0) {
    if (!count)
      return;
    if (!DeferredConstantMask(first, count))
      throw std::runtime_error("Invalid deferred constant copy range");
    std::memmove(
        bd::mem::at<uint8_t>(device + (vertex ? 0x700 : 0x1700) + first * 16),
        bytes, count * 16);
    MarkFloats(vertex, first, count, source);
  }
  void Floats(bool vertex, uint32_t first, const std::array<float, 4> &values) {
    if (!DeferredConstantMask(first, 1))
      throw std::runtime_error("Invalid deferred constant vector");
    for (uint32_t i = 0; i < 4; ++i)
      Write<float>(device + (vertex ? 0x700 : 0x1700) + first * 16 + i * 4,
                   values[i]);
    MarkFloats(vertex, first, 1);
  }
  void Bool(bool vertex, uint32_t index, uint32_t value) {
    if (index >= 128)
      throw std::runtime_error("Invalid deferred boolean index");
    const auto address = device + (vertex ? 0x2700 : 0x2710) + (index / 32) * 4;
    Write<uint32_t>(address, *DeferredBooleanWord(Read<uint32_t>(address),
                                                  index % 32, value));
    Write<uint64_t>(device + 24, Read<uint64_t>(device + 24) | 2);
    if (vertex)
      Video::MarkVSConstantsDirty();
    else
      Video::MarkPSConstantsDirty();
    NoteBoolsSet(vertex, index, 1);
  }
  void Draw(uint32_t entry) {
    const auto triangles = Read<uint16_t>(entry + 280);
    const auto first = Read<uint16_t>(entry + 284);
    Write<uint32_t>(kDrawStats, Read<uint32_t>(kDrawStats) + 1);
    Write<uint32_t>(kDrawStats + 4, Read<uint32_t>(kDrawStats + 4) + triangles);
    const bool wireframe = Read<uint8_t>(kObjectMode + 2) != 0;
    const auto fill = Read<uint32_t>(kRenderStateCache + 52);
    if (wireframe)
      State(52, 37);
    hooks::DispatchHostNodeDraw(device, 6, true, uint32_t(triangles) + 2, first,
                                0, 0);
    if (wireframe)
      State(52, fill);
    ++stats.draws;
  }
};

DeferredFoliageInputs ImportFoliage(uint32_t visual, uint32_t node) {
  DeferredFoliageInputs result;
  const auto table = Read<uint32_t>(visual + 3540);
  const auto object = Read<uint32_t>(visual + 3532);
  const auto entry = table + node * 20;
  if (table && Read<uint32_t>(entry)) {
    const float scale = Read<float>(object + 72);
    const float weight = Read<float>(entry + 12);
    result.displacement[0] = (Read<float>(entry + 4) * scale) * weight;
    result.displacement[2] = (Read<float>(entry + 8) * scale) * weight;
    result.collision = true;
  } else {
    result.displacement[0] = Read<float>(kFoliage);
    result.displacement[2] = Read<float>(kFoliage + 8);
  }
  result.displacement[1] =
      object ? Read<float>(visual + 3536) * Read<float>(object + 36)
             : Read<float>(kFoliage + 4);
  result.displacement[3] = Read<float>(kList + 80 + node * 4);
  result.stencil = object && Read<uint32_t>(object) == 2;
  return result;
}

void BindEntry(EngineBridge &bridge, uint32_t entry, uint32_t &indices,
               uint32_t &stream) {
  const auto next_indices = Read<uint32_t>(entry + 384);
  if (indices != next_indices) {
    bridge.Call(D3DDevice_SetIndices, BridgeKind::Resource,
                {bridge.device, next_indices});
    indices = next_indices;
  }
  const auto declaration = Read<uint32_t>(entry + 376);
  const auto next_stream = Read<uint32_t>(entry + 380);
  if (stream != next_stream) {
    bridge.Call(
        D3DDevice_SetStreamSource, BridgeKind::Resource,
        {bridge.device, 0, next_stream, 0, Read<uint8_t>(declaration), 4096});
    stream = next_stream;
  }
  const auto native_declaration = Read<uint32_t>(declaration + 12);
  if (native_declaration &&
      native_declaration != Read<uint32_t>(kDeclarationCache)) {
    bridge.Call(D3DDevice_SetVertexDeclaration, BridgeKind::Resource,
                {bridge.device, native_declaration});
    Write<uint32_t>(kDeclarationCache, native_declaration);
  }
  const auto material = entry + 388;
  const auto stamp = Read<uint32_t>(material + 408);
  if (stamp != Read<uint32_t>(kMaterialStamp)) {
    bridge.CopyFloats(true, 0, bd::mem::at<const uint8_t>(material), 5,
                      material);
    bridge.CopyFloats(false, 0, bd::mem::at<const uint8_t>(material + 80), 14,
                      material + 80);
    for (uint32_t i = 0; i < 6; ++i)
      bridge.Bool(true, i, Read<uint32_t>(material + 304 + i * 4));
    for (uint32_t i = 0; i < 11; ++i)
      bridge.Bool(false, i, Read<uint32_t>(material + 328 + i * 4));
    Write<uint32_t>(kMaterialStamp, stamp);
  }
}

void SubmitSurface(EngineBridge &bridge, uint32_t entry, uint32_t visual,
                   bool &stencil_pending, int32_t &depth_write) {
  const auto plan =
      PlanDeferredSurface(int8_t(Read<uint8_t>(entry + 292)), stencil_pending);
  if (plan.kind == DeferredSurfaceKind::FurShells) {
    Write<uint32_t>(kEngineState + 16, Read<uint8_t>(entry + 290) & 1);
    const float scale = (float(int8_t(Read<uint8_t>(entry + 293))) *
                         Read<float>(High(-32247) - 4392)) *
                        Read<float>(High(-32247) - 4388);
    const auto shader = visual && Read<uint32_t>(visual + 3668)
                            ? (Read<uint32_t>(visual + 1864) ? 16u : 17u)
                            : 15u;
    bridge.Call(sub_82286228, BridgeKind::Shader, {shader});
    bridge.State(48, 0);
    const float one = Read<float>(High(-32250) - 7108);
    for (uint32_t shell = 1; shell <= plan.draws; ++shell) {
      const auto slice = ComposeDeferredFurSlice(shell, plan.draws, scale);
      if (!slice)
        throw std::runtime_error("Invalid deferred fur extrusion");
      std::array<float, 4> colour;
      for (uint32_t i = 0; i < 4; ++i)
        colour[i] = Read<float>(visual + 3612 + i * 4);
      bridge.Floats(true, 50, colour);
      bridge.Floats(true, 51,
                    {one, slice->extrusion, slice->fraction, colour[3]});
      bridge.Draw(entry);
      ++stats.shells;
    }
    bridge.State(48, uint32_t(depth_write));
    return;
  }
  const int32_t next_depth = int8_t(Read<uint8_t>(entry + 295));
  if (next_depth != depth_write) {
    depth_write = next_depth;
    bridge.State(48, uint32_t(depth_write));
  }
  if (plan.kind == DeferredSurfaceKind::StencilPair) {
    bridge.State(108, 1);
    bridge.State(124, 6);
    bridge.State(116, 0);
    bridge.State(140, 255);
    bridge.State(128, 7);
    bridge.Bool(false, 28, 0);
    bridge.Draw(entry);
    bridge.State(132, 0);
    bridge.State(128, 5);
    bridge.Bool(false, 28, 1);
    bridge.Draw(entry);
    bridge.State(108, 0);
    stencil_pending = false;
    ++stats.stencil;
  } else {
    bridge.Draw(entry);
  }
}
} // namespace

void RecordDeferredConsumerFallback() {
  ++stats.fallback;
  Report();
}

bool ConsumeDeferredList(PPCContext &ctx, uint8_t *base) {
  std::vector<ImportedEntry> entries;
  if (ctx.r1.u32 < 128 || !Range(uint64_t(ctx.r1.u32) - 128, 128) ||
      !ImportList(entries)) {
    if (++stats.refused <= 8)
      BD_WARN("[host-consumer] invalid initial list import; no native side "
              "effects");
    return false;
  }
  // Sort native records once; no recursive guest calls or republished pointer
  // array is needed because the host owns the consuming loop and list drain.
  if (!Read<uint8_t>(kPassMode) && !Read<uint32_t>(kSortDisabled)) {
    std::vector<DeferredSortItem> order;
    order.reserve(entries.size());
    for (uint32_t i = 0; i < entries.size(); ++i)
      order.push_back({entries[i].depth, i});
    if (OrderDeferredWork(order)) {
      auto sorted = entries;
      for (size_t i = 0; i < order.size(); ++i)
        sorted[i] = entries[order[i].payload];
      entries = std::move(sorted);
    } else {
      ++stats.refused;
      BD_WARN("[host-consumer] nonfinite depth; retaining submission order");
    }
  }
  EngineBridge bridge(ctx, base);
  Write<uint32_t>(kEngineState + 24, UINT32_MAX);
  std::memset(bd::mem::at<uint8_t>(kEngineState + 28), 0, 12);
  const auto used = Read<uint32_t>(kList + 4) - Read<uint32_t>(kList + 8);
  Write<uint32_t>(kArenaStats, std::max(Read<uint32_t>(kArenaStats), used));
  Write<uint32_t>(kArenaStats + 4, 4194304);
  bridge.State(60, 1);
  if (!Read<uint8_t>(kPassMode)) {
    bridge.State(104, 6);
    bridge.State(96, 1);
  }
  const auto saved_mode = Read<uint8_t>(kObjectMode);
  uint32_t visual = 0, technique = 3, indices = 0, stream = 0;
  uint32_t alpha = UINT32_MAX, winding = UINT32_MAX, sidedness = 127;
  int32_t depth_write = 1;
  bool stencil_pending = false;
  for (const auto &item : entries) {
    const auto entry = item.address;
    ++stats.entries;
    PPCRegister entry_reg{}, visual_reg{};
    entry_reg.u32 = entry;
    visual_reg.u32 = visual;
    if (bdRenderListEntryHook(entry_reg, visual_reg)) {
      ++stats.replayed;
      continue;
    }
    const auto next_visual = Read<uint32_t>(entry + 272);
    if (next_visual != visual) {
      if (visual)
        bridge.Call(sub_8221DCA0, BridgeKind::Visual, {kVisualContext});
      visual = next_visual;
      technique = bridge.Call(sub_8221DBE0, BridgeKind::Visual,
                              {kVisualContext, visual, 1});
      if (visual)
        bridge.Call(sub_82425C28, BridgeKind::Visual,
                    {Read<uint32_t>(visual + 1864)});
    }
    if (technique == 3)
      continue;
    for (uint32_t slot = 0; slot < 6; ++slot) {
      const auto texture = Read<uint32_t>(entry + 352 + slot * 4);
      if (!texture)
        continue;
      bridge.Call(bdSetSamplerState, BridgeKind::State,
                  {slot, 0, Read<uint32_t>(entry + 304 + slot * 4)});
      bridge.Call(bdSetSamplerState, BridgeKind::State,
                  {slot, 4, Read<uint32_t>(entry + 328 + slot * 4)});
      bridge.Call(D3DDevice_SetTexture, BridgeKind::Resource,
                  {bridge.device, slot, texture, uint64_t(1) << (43 - slot)});
    }
    const auto next_alpha = Read<uint32_t>(entry + 260);
    if (next_alpha != alpha) {
      if (!Read<uint8_t>(kPassMode))
        bridge.State(100, next_alpha);
      alpha = next_alpha;
    }
    bridge.Call(bdBuildViewMatrix, BridgeKind::World, {entry + 16, 0, 0});
    const int bones = int8_t(Read<uint8_t>(entry + 289));
    if (bones > 0 && Read<uint8_t>(kEngineState + 54852) != 1) {
      std::vector<uint8_t> matrices(size_t(bones) * 64);
      const auto palette = Read<uint32_t>(entry + 268);
      for (int bone = 0; bone < bones; ++bone) {
        const auto source =
            palette + Read<uint32_t>(entry + 800 + bone * 4) * 64;
        std::memcpy(matrices.data() + bone * 64,
                    bd::mem::at<const uint8_t>(source), 64);
      }
      bridge.CopyFloats(true, 60, matrices.data(), uint32_t(bones) * 4);
    }
    const auto next_winding = Read<uint16_t>(entry + 286);
    const auto next_side = Read<uint8_t>(entry + 288);
    if (next_winding != winding || next_side != sidedness) {
      if (next_winding == 0x1000 || next_winding == 0x2000 ||
          next_winding == 0x3000) {
        const auto side = next_winding == 0x3000 ? 2u : next_side;
        Write<uint8_t>(kObjectMode, uint8_t(side));
        const auto face = DeferredFaces(next_winding == 0x2000, side);
        bridge.State(56, face == DeferredCullFace::Back    ? 6
                         : face == DeferredCullFace::Front ? 2
                                                           : 0);
      }
      winding = next_winding;
      sidedness = next_side;
    }
    if (bridge.Material(36, {entry + 240, 1}) != 3) {
      if (visual && Read<uint32_t>(visual + 3000) == 3) {
        const auto foliage = ImportFoliage(visual, Read<uint32_t>(entry + 252));
        bridge.Floats(true, 57, foliage.displacement);
        bridge.Bool(true, 31, foliage.collision);
        stencil_pending |= foliage.stencil;
      }
      if (visual && Read<uint32_t>(visual + 3000) == 8)
        Write<uint8_t>(entry + 295, 0);
      BindEntry(bridge, entry, indices, stream);
      SubmitSurface(bridge, entry, visual, stencil_pending, depth_write);
    }
    bridge.Material(40);
  }
  Write<uint8_t>(kObjectMode, saved_mode);
  if (visual)
    bridge.Call(sub_8221DCA0, BridgeKind::Visual, {kVisualContext});
  const auto pool = Read<uint32_t>(kList + 8);
  Write<uint32_t>(kList + 4, pool);
  Write<uint32_t>(kList, 0);
  Write<uint32_t>(kList + 16, Read<uint32_t>(kList + 12));
  Write<uint32_t>(kList + 20, 0);
  std::memset(bd::mem::at<uint8_t>(pool), 0, kDeferredEntryBytes);
  ResetDeferredDepthImports();
  if (!depth_write)
    bridge.State(48, 1);
  bridge.State(60, 0);
  bridge.State(96, 0);
  bridge.State(100, 0);
  ++stats.lists;
  Report();
  return true;
}
} // namespace bd::gpu::scene
