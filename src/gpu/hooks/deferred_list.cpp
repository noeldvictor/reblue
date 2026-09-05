/**
 * @file    deferred_list.cpp
 * @brief   Replace guest deferred allocation and depth sorting with host work.
 * @copyright Copyright (c) 2026 reblue contributors
 * @license BSD 3-Clause, see LICENSE
 */
#include "gpu/scene/deferred_list.h"
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/ppc/context.h>

REXCVAR_DECLARE(bool, bd_native_deferred_order);
extern "C" void __imp__sub_8227F290(PPCContext &__restrict ctx, uint8_t *base);

REX_HOOK_RAW(sub_8227DB50) {
  ctx.r3.u64 = bd::gpu::scene::AllocateDeferredEntry(ctx.r3.u32);
}

REX_HOOK_RAW(sub_8227F290) {
  if (!REXCVAR_GET(bd_native_deferred_order)) {
    __imp__sub_8227F290(ctx, base); // correctness-only order comparison
    return;
  }
  // Invalid keys retain submission order and emit a refusal; do not execute
  // the old recursive sorter on malformed data after a failed host import.
  bd::gpu::scene::OrderDeferredEntries(ctx.r3.u32, ctx.r4.s32, ctx.r5.s32);
}
