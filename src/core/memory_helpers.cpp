/**
 * @file    core/memory_helpers.cpp
 * @brief   The validating half of the Xbox 360 address space accessor.
 * @license BSD 3-Clause, see LICENSE
 */
#include "core/memory_helpers.h"

#include <rex/runtime.h>
#include <rex/system/xmemory.h>

#include "gpu/frame_stats.h"

namespace bd::mem {

namespace {
// BaseHeap::QueryProtect takes the heap's recursive mutex on every call, and
// the host scene code asks thousands of times a frame: on the Quest 2 the
// query and its lock were a quarter of the Draw Thread's samples
// (2026-09-02). A page whose protection was readable this frame is taken as
// readable for the rest of it, per thread; the recompiled code itself reads
// guest memory with no check at all, so this is no looser than the guest.
constexpr u32 kPageShift = 12; // the smallest guest page
constexpr u32 kCacheEntries = 256;
struct PageCacheEntry {
  u32 page_plus_one = 0;
  u32 frame = 0;
};
thread_local PageCacheEntry t_pages[kCacheEntries];
} // namespace

bool ready() {
  auto *rt = rex::Runtime::instance();
  return rt && rt->memory();
}

void *try_translate(u32 va, u32 align) {
  // The guarded Runtime map, not REX_KERNEL_MEMORY: the checked accessors run
  // off the guest thread, where the kernel state pointer that macro chases is
  // not guaranteed to be there.
  auto *rt = rex::Runtime::instance();
  auto *memory = rt ? rt->memory() : nullptr;
  if (!memory || !va || (va & (align - 1)))
    return nullptr;

  const u32 page = va >> kPageShift;
  const u32 frame = bd::gpu::FrameStatFrameCount();
  PageCacheEntry &e = t_pages[page & (kCacheEntries - 1)];
  if (e.page_plus_one == page + 1 && e.frame == frame)
    return memory->TranslateVirtual<void *>(va);

  auto *heap = memory->LookupHeap(va);
  u32 protect = 0;
  if (!heap || !heap->QueryProtect(va, &protect) ||
      !(protect & rex::memory::kMemoryProtectRead)) {
    return nullptr;
  }
  e.page_plus_one = page + 1;
  e.frame = frame;
  return memory->TranslateVirtual<void *>(va);
}

} // namespace bd::mem
