/**
 * @file    native_shadow.cpp
 * @brief   Import live receiver visibility from the scene, not draw history.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#include "gpu/scene/native_shadow.h"
#include "core/memory_helpers.h"
#include "gpu/scene/node_tag.h"
#include <limits>
#include <rex/system/xthread.h>

namespace bd::gpu::scene {
namespace {
template <typename T> std::optional<T> Read(uint64_t address) {
  if (address > std::numeric_limits<uint32_t>::max())
    return {};
  const auto *value = bd::mem::try_at<const bd::be<T>>(uint32_t(address));
  return value ? std::optional<T>(T(*value)) : std::nullopt;
}
constexpr uint32_t Address(int high, int low) {
  return (uint32_t(high) << 16) + uint32_t(low);
}
} // namespace

std::optional<NativeShadowInputs> ImportNodeShadowInputs(const NodeTag &tag) {
  if (!tag.valid || tag.from_list || !tag.visual_va || !tag.ctx_va ||
      bd::mem::try_load<uint32_t>(tag.ctx_va + 16, ~0u) != 0)
    return {};
  // bdSceneNodeDrawSingle, 0x822802DC..0x822803D8: the incoming
  // pass state is saved, then filtered by this node's current visibility.
  const auto enabled = Read<uint32_t>(Address(-32034, -32552) + 348);
  const auto filter = Read<uint32_t>(Address(-32133, -31616));
  if (!enabled || !filter)
    return {};
  NativeShadowInputs result{*enabled != 0, *filter != 0, false};
  if (!result.pass_enabled || !result.receiver_filter_enabled)
    return result;
  const auto per_node = Read<uint32_t>(uint64_t(tag.visual_va) + 3380);
  if (!per_node)
    return {};
  uint64_t receiver = uint64_t(tag.visual_va) + 3132;
  if (*per_node) {
    const auto table = Read<uint32_t>(uint64_t(tag.visual_va) + 3376);
    if (!table || !*table)
      return {};
    const auto entry =
        Read<uint32_t>(uint64_t(*table) + uint64_t(tag.node_index) * 4);
    if (!entry)
      return {};
    if (!*entry)
      return result; // explicitly absent node receiver
    receiver = *entry;
  }
  // sub_82189E00 selects a 12-byte stamp slot. sub_82184A38 selects
  // the current thread's frame counter; do not substitute host-present count.
  const auto slot = Read<uint32_t>(Address(-32035, -26424));
  const auto frame_owner = Read<uint32_t>(Address(-32137, 30280));
  const auto primary_thread = Read<uint32_t>(Address(-32035, -26664));
  if (!slot || !frame_owner || !*frame_owner || !primary_thread ||
      !rex::system::XThread::GetCurrentThread())
    return {};
  const auto stamp = Read<uint16_t>(receiver + uint64_t(*slot) * 12 + 8);
  const bool primary =
      rex::system::XThread::GetCurrentThreadId() == *primary_thread;
  const auto frame =
      Read<uint32_t>(uint64_t(*frame_owner) + (primary ? 12007 : 12008) * 4);
  if (!stamp || !frame)
    return {};
  result.receiver_visible = ShadowStampMatches(*stamp, *frame);
  return result;
}
} // namespace bd::gpu::scene
