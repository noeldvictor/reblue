/**
 * @file    core/threading.cpp
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause - see LICENSE
 */
#include "core/threading.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <thread>

#include "core/logging.h"

#include <rex/ppc.h>
#include <rex/system/xthread.h>
#include <rex/types.h>

#if defined(_WIN32)
#include "core/windows_lean.h"
#include <timeapi.h>
#else
#include <sys/resource.h>
#endif

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

namespace {
std::once_flag g_timer_init;

#if defined(__x86_64__) || defined(_M_X64)
inline void CpuPause() { _mm_pause(); }
#elif defined(__aarch64__) || defined(__arm__)
// The analogue of PAUSE is the YIELD *instruction* - a hint to the core that
// costs a few cycles. std::this_thread::yield() is sched_yield(), a syscall,
// and spinning on it turns a wait into a scheduler storm that starves every
// other thread in the process.
inline void CpuPause() { __asm__ __volatile__("yield" ::: "memory"); }
#else
inline void CpuPause() { std::this_thread::yield(); }
#endif

// Guest Sleep traffic, so the cost is visible rather than inferred.
std::atomic<u64> g_sleep_calls{0};
std::atomic<u64> g_sleep_req_us{0};
} // namespace

namespace bd {

void EnableHighResTimer() {
  std::call_once(g_timer_init, [] {
#if defined(_WIN32)
    timeBeginPeriod(1);
#endif
  });
}

void DisableHighResTimer() {
#if defined(_WIN32)
  timeEndPeriod(1);
#endif
}

void DemoteThreadToBackground() {
#if defined(_WIN32)
  ::SetThreadPriority(::GetCurrentThread(), THREAD_PRIORITY_LOWEST);
#else
  // who=0 is the calling THREAD on Linux, not the process.
  ::setpriority(PRIO_PROCESS, 0, 10);
#endif
}

void TerminateProcessNow(int exit_code) {
#if defined(_WIN32)
  ::TerminateProcess(::GetCurrentProcess(),
                     static_cast<unsigned int>(exit_code));
  // TerminateProcess can return before the caller halts, so spin and nothing
  // runs past here.
  for (;;) {
    CpuPause();
  }
#else
  std::_Exit(exit_code);
#endif
}

} // namespace bd

// PPC kernel bypass for Sleep.
u32 Sleep_hook(u32 ms) {
  bd::EnableHighResTimer();

  const u64 n = g_sleep_calls.fetch_add(1, std::memory_order_relaxed) + 1;
  g_sleep_req_us.fetch_add(u64(ms) * 1000u, std::memory_order_relaxed);
  if ((n % 20000u) == 0u) {
    BD_INFO("[sleep] {} guest Sleep() calls, {} ms requested in total", n,
            g_sleep_req_us.load(std::memory_order_relaxed) / 1000u);
  }

  if (ms == 0) {
    std::this_thread::yield();
    return 0;
  }

  auto target =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(u32(ms));

#if defined(__ANDROID__)
  // No busy-wait tail here. Sleep is a floor, not a deadline: the guest asked
  // to be woken no earlier than ms, and Linux honours that to well under the
  // 1.5ms guard band this used to spin out. Spinning it instead cost a full
  // core per sleeping guest thread - with five of them the render thread could
  // not get scheduled, which showed up as a 77ms "GPU fence wait" on a frame
  // whose command buffer measured 2ms.
  std::this_thread::sleep_for(std::chrono::milliseconds(u32(ms)));
  (void)target;
#else
  if (ms >= 2) {
    std::this_thread::sleep_for(std::chrono::milliseconds(u32(ms)) -
                                std::chrono::microseconds(1500));
  } else {
    std::this_thread::yield();
  }

  while (std::chrono::steady_clock::now() < target)
    CpuPause();
#endif

  return 0;
}
REX_HOOK(rex_Sleep, Sleep_hook);

// PPC kernel bypass for NtSuspendThread.
u32 NtSuspendThread_hook(u32 handle) {
  auto thread =
      REX_KERNEL_OBJECTS()->LookupObject<rex::system::XThread>(handle);
  if (thread)
    thread->Suspend();
  return 0;
}
REX_HOOK(rex_NtSuspendThread, NtSuspendThread_hook);

// PPC kernel bypass for ResumeThread.
u32 ResumeThread_hook(u32 handle) {
  auto thread =
      REX_KERNEL_OBJECTS()->LookupObject<rex::system::XThread>(handle);
  if (thread)
    thread->Resume();
  return 0;
}
REX_HOOK(rex_ResumeThread, ResumeThread_hook);
