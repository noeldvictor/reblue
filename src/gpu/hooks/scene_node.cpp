/**
 * @file    gpu/hooks/scene_node.cpp
 * @brief   The seam for replacing the guest's per-node draw submission with
 *          host code.
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 * @license     BSD 3-Clause - see LICENSE
 */

// bdSceneNodeDrawSingle (0x8227FEE8, 0x1E3C bytes) is the per-node draw
// submission: ~2084 calls a frame on device, more than any other named guest
// function, and about 370 guest memory operations each - 285 stw, 245 lwz,
// 100 stfs, 65 lfs - marshalling a transform and a material into big-endian
// guest memory so that a Xenos command processor could read it back. Our hooks
// then read it straight back out. That round trip is the X360 ABI this port is
// supposed to remove.
//
// Replacing it wholesale means reproducing 1,935 guest instructions and 73
// calls to 38 distinct functions, including five D3DDevice_SetTexture (already
// host), eleven bdSetSamplerState (which the guest itself early-outs on an
// unchanged value) and two bdSetRenderState. That is not a change to make in
// one step and hope.
//
// So this takes the seam first and does nothing with it. REX_HOOK_RAW defines
// a strong `bdSceneNodeDrawSingle`, which overrides the weak alias the
// recompiler emits, and the body tail-calls __imp__bdSceneNodeDrawSingle - the
// always-original entry point. Behaviour is unchanged by construction, and
// what it proves is that the override links and runs for THIS symbol, which
// was an open question: the same mechanism fails with `duplicate symbol` on
// Visual__DrawVerticesUP for reasons still unexplained.
//
// With the seam held, work moves host-side one piece at a time, each verified
// against a capture, instead of as one 1,935-instruction leap.

#include <atomic>

#include <rex/hook.h>
#include <rex/ppc/context.h>
#include <rex/types.h>

#include "core/logging.h"

extern "C" void __imp__bdSceneNodeDrawSingle(PPCContext &__restrict ctx,
                                             uint8_t *base);

namespace {
std::atomic<u64> g_node_calls{0};
} // namespace

REX_HOOK_RAW(bdSceneNodeDrawSingle) {
  // Proof the override is live, once. A hook on a function nobody has watched
  // fire is a guess - this file exists to remove that doubt before anything is
  // built on it.
  const u64 n = g_node_calls.fetch_add(1, std::memory_order_relaxed);
  if (n == 0)
    BD_INFO("[node] host bdSceneNodeDrawSingle is live - the override links");
  if (n == 200000)
    BD_INFO("[node] host bdSceneNodeDrawSingle has run {} times", n);

  __imp__bdSceneNodeDrawSingle(ctx, base);
}
