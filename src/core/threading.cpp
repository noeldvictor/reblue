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

#if defined(__ANDROID__)
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sched.h>
#endif

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

REXCVAR_DECLARE(bool, bd_thread_policy);

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

void ApplyThreadPolicy() {
#if defined(__ANDROID__)
  if (!REXCVAR_GET(bd_thread_policy))
    return;

  // Derive the clusters from the hardware rather than hardcoding a layout.
  // The Quest 2 is an XR2: four A55 at 1.80GHz, three A77 at 2.42GHz, one at
  // 2.84GHz. An AYN Thor is an 8 Gen 2 and splits 3+4+1 instead, so a fixed
  // mask that is right on one is wrong on the other.
  static u64 g_little = 0, g_fast = 0;
  static bool g_probed = false;
  if (!g_probed) {
    g_probed = true;
    u32 freq[16] = {0};
    u32 slowest = 0xFFFFFFFFu;
    for (int c = 0; c < 16; ++c) {
      char fp[96];
      std::snprintf(fp, sizeof(fp),
                    "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", c);
      if (FILE *f = std::fopen(fp, "r")) {
        if (std::fscanf(f, "%u", &freq[c]) == 1 && freq[c] > 0 &&
            freq[c] < slowest)
          slowest = freq[c];
        std::fclose(f);
      }
    }
    for (int c = 0; c < 16; ++c) {
      if (!freq[c])
        continue;
      if (freq[c] == slowest)
        g_little |= (u64(1) << c);
      else
        g_fast |= (u64(1) << c);
    }
    // A uniform machine has nothing to place, and a mask of zero would pin a
    // thread to no cores at all.
    if (!g_little || !g_fast) {
      g_little = g_fast = 0;
      BD_INFO("[cpu] uniform core layout, thread policy disabled");
    } else {
      BD_INFO("[cpu] core clusters: efficiency mask {:#x}, performance mask "
              "{:#x} (slowest {} kHz)",
              g_little, g_fast, slowest);
    }
  }
  if (!g_little || !g_fast)
    return;
  const u64 kLittle = g_little;
  const u64 kBigPlusPrime = g_fast;

  DIR *d = ::opendir("/proc/self/task");
  if (!d)
    return;
  u32 moved_workers = 0, moved_main = 0;
  while (dirent *e = ::readdir(d)) {
    if (e->d_name[0] == '.')
      continue;
    const int tid = std::atoi(e->d_name);
    if (tid <= 0)
      continue;

    char path[64];
    std::snprintf(path, sizeof(path), "/proc/self/task/%d/comm", tid);
    char name[64] = {0};
    if (FILE *f = std::fopen(path, "r")) {
      if (!std::fgets(name, sizeof(name), f))
        name[0] = 0;
      std::fclose(f);
    }
    if (!name[0])
      continue;

    // The guest's threads are the ones the SDK names "<name> (HHHHHHHH)", plus
    // SDL's, which is where the guest main loop runs.
    const bool is_guest_main = std::strncmp(name, "SDLThread", 9) == 0;
    const bool is_guest_worker = std::strncmp(name, "Main Thread", 11) == 0 ||
                                 std::strncmp(name, "XThread", 7) == 0;
    if (!is_guest_main && !is_guest_worker)
      continue;

    cpu_set_t want;
    CPU_ZERO(&want);
    // The guest main thread is the frame pacer - it is the thread that was
    // measured saturated - so it gets the prime core as well as the big
    // cluster. The workers go to the little cores: they are not on the
    // critical path, and leaving them free to roam is what starves the
    // renderer.
    const u64 mask = is_guest_main ? kBigPlusPrime : kLittle;
    for (int c = 0; c < 16; ++c) {
      if (mask & (u64(1) << c))
        CPU_SET(c, &want);
    }

    cpu_set_t have;
    CPU_ZERO(&have);
    if (::sched_getaffinity(tid, sizeof(have), &have) == 0 &&
        CPU_EQUAL(&have, &want))
      continue; // already right, no syscall
    if (::sched_setaffinity(tid, sizeof(want), &want) == 0) {
      if (is_guest_main)
        ++moved_main;
      else
        ++moved_workers;
    }
  }
  ::closedir(d);

  if (moved_main || moved_workers) {
    BD_INFO("[cpu] thread policy: {} guest main -> mask {:#x}, {} guest "
            "worker(s) -> mask {:#x}",
            moved_main, kBigPlusPrime, moved_workers, kLittle);
  }
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
