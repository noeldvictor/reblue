/**
 * @file    core/sampling_profiler.cpp
 * @copyright Copyright (c) 2026 re:Blue contributors
 * @license   BSD 3-Clause - see LICENSE
 */
#include "core/sampling_profiler.h"

#include "core/app_root.h"
#include "core/logging.h"

#include <rex/types.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(__ANDROID__)
#include <csignal>
#include <dirent.h>
#include <dlfcn.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#endif

REXCVAR_DECLARE(bool, bd_sample_profiler);
REXCVAR_DECLARE(i32, bd_sample_hz);

namespace bd {

#if defined(__ANDROID__)
namespace {

// Power of two so the wrap is a mask. Samples are dropped rather than blocking:
// a profiler that perturbs the thing it measures is worse than a lossy one.
constexpr u32 kRingSize = 1u << 16;
constexpr u32 kRingMask = kRingSize - 1;

std::atomic<u64> g_ring[kRingSize];
std::atomic<u32> g_head{0};
std::atomic<u64> g_taken{0};
std::atomic<u64> g_dropped{0};

// Base address of our own shared object, so a sample is an offset that survives
// ASLR and can be looked up in the unstripped .so on the host.
uintptr_t g_module_base = 0;
std::atomic<bool> g_running{false};

void OnSigProf(int, siginfo_t *, void *ctx) {
  if (!ctx)
    return;
  const auto *uc = static_cast<const ucontext_t *>(ctx);
  const uintptr_t pc = static_cast<uintptr_t>(uc->uc_mcontext.pc);
  // A PC outside our module is the runtime, the driver or libc. Those are not
  // what this is for and keeping them would just dilute the histogram.
  if (pc < g_module_base)
    return;
  const u64 offset = static_cast<u64>(pc - g_module_base);
  if (offset > 0x8000000ull)
    return;

  const u32 slot = g_head.fetch_add(1, std::memory_order_relaxed) & kRingMask;
  g_ring[slot].store(offset, std::memory_order_relaxed);
  g_taken.fetch_add(1, std::memory_order_relaxed);
}

// The threads worth sampling: the guest's own. The renderer and the audio
// mixer are already known to be cheap, and sampling everything makes the
// histogram harder to read rather than more informative.
bool IsInteresting(const char *name) {
  return std::strncmp(name, "SDLThread", 9) == 0 ||
         std::strncmp(name, "Main Thread", 11) == 0 ||
         std::strncmp(name, "XThread", 7) == 0 ||
         std::strncmp(name, "Draw Thread", 11) == 0;
}

void CollectTids(std::vector<int> &out) {
  out.clear();
  DIR *d = ::opendir("/proc/self/task");
  if (!d)
    return;
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
    if (name[0] && IsInteresting(name))
      out.push_back(tid);
  }
  ::closedir(d);
}

void SamplerThread() {
  const i32 hz = REXCVAR_GET(bd_sample_hz) > 0 ? REXCVAR_GET(bd_sample_hz) : 1000;
  const long period_ns = 1000000000L / hz;
  const int pid = ::getpid();
  std::vector<int> tids;
  u64 iterations = 0;

  while (g_running.load(std::memory_order_relaxed)) {
    // Guest threads come and go with the scene, so the list is refreshed rather
    // than captured once - a run that samples only the threads alive at startup
    // misses exactly the workers that appear in a field scene.
    if ((iterations++ % 512) == 0)
      CollectTids(tids);

    for (const int tid : tids) {
      // tgkill rather than pthread_kill: we have raw tids from /proc, and a
      // thread that has exited since the scan just returns ESRCH.
      if (::syscall(__NR_tgkill, pid, tid, SIGPROF) != 0)
        g_dropped.fetch_add(1, std::memory_order_relaxed);
    }

    timespec ts{0, period_ns};
    ::nanosleep(&ts, nullptr);
  }
}

} // namespace
#endif

void SamplingProfilerDump() {
#if defined(__ANDROID__)
  const u64 taken = g_taken.load(std::memory_order_relaxed);
  if (!taken)
    return;

  // Only the most recent kRingSize samples survive, which is what we want: a
  // dump describes the recent past rather than averaging over the load screen.
  const u32 head = g_head.load(std::memory_order_relaxed);
  const u32 count = (taken < kRingSize) ? static_cast<u32>(taken) : kRingSize;

  std::unordered_map<u64, u32> hist;
  hist.reserve(count);
  for (u32 i = 0; i < count; ++i) {
    const u32 slot = (head - 1 - i) & kRingMask;
    const u64 off = g_ring[slot].load(std::memory_order_relaxed);
    if (off)
      ++hist[off];
  }

  std::vector<std::pair<u64, u32>> sorted(hist.begin(), hist.end());
  std::sort(sorted.begin(), sorted.end(),
            [](const auto &a, const auto &b) { return a.second > b.second; });

  const auto dir = bd::AppRootFolder() / "logs";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  const std::string path = (dir / "guest_profile.txt").string();
  if (FILE *f = std::fopen(path.c_str(), "w")) {
    std::fprintf(f, "# module-relative PCs from libreblue.so, most frequent "
                    "first.\n# resolve with: python tools/symbolize_profile.py "
                    "%s\n# samples=%llu unique=%zu\n",
                 path.c_str(), static_cast<unsigned long long>(taken),
                 sorted.size());
    for (const auto &kv : sorted)
      std::fprintf(f, "%llx %u\n", static_cast<unsigned long long>(kv.first),
                   kv.second);
    std::fclose(f);
  }

  BD_INFO("[profile] {} samples, {} unique addresses -> {}", taken,
          sorted.size(), path);
#endif
}

void SamplingProfilerTick() {
#if defined(__ANDROID__)
  if (!REXCVAR_GET(bd_sample_profiler))
    return;
  static bool started = false;
  if (!started) {
    started = true;

    Dl_info info{};
    if (!dladdr(reinterpret_cast<const void *>(&SamplingProfilerTick), &info) ||
        !info.dli_fbase) {
      BD_ERROR("[profile] cannot find our own module base; sampling disabled");
      return;
    }
    g_module_base = reinterpret_cast<uintptr_t>(info.dli_fbase);

    struct sigaction sa {};
    sa.sa_sigaction = OnSigProf;
    // SA_RESTART so an interrupted read in the guest resumes instead of
    // returning EINTR - the guest was written against an OS that never did
    // this to it and does not check for it.
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    if (::sigaction(SIGPROF, &sa, nullptr) != 0) {
      BD_ERROR("[profile] sigaction(SIGPROF) failed; sampling disabled");
      return;
    }

    g_running.store(true, std::memory_order_relaxed);
    std::thread(SamplerThread).detach();
    BD_INFO("[profile] sampling guest threads at {} Hz, base {:#x}",
            REXCVAR_GET(bd_sample_hz), g_module_base);
  }

  // Periodic dumps, so a run that is killed rather than exited still leaves a
  // profile behind.
  static u32 ticks = 0;
  if ((++ticks % 600u) == 0u)
    SamplingProfilerDump();
#endif
}

} // namespace bd
