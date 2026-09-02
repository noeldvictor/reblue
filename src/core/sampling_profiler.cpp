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
#include <chrono>
#include <filesystem>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(_WIN32)
#include "core/windows_lean.h"
#include <dbghelp.h>
#include <tlhelp32.h>
#endif

#if defined(__ANDROID__)
#include <csignal>
#include <dirent.h>
#include <dlfcn.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>
#endif

#if defined(__ANDROID__) || defined(_WIN32)
#define BD_HAVE_SAMPLER 1
#else
#define BD_HAVE_SAMPLER 0
#endif

REXCVAR_DECLARE(bool, bd_sample_profiler);
REXCVAR_DECLARE(i32, bd_sample_hz);
REXCVAR_DECLARE(double, bd_capture_after_s);

namespace bd {

#if BD_HAVE_SAMPLER
namespace {

// Power of two so the wrap is a mask. Samples are dropped rather than blocking:
// a profiler that perturbs the thing it measures is worse than a lossy one.
constexpr u32 kRingSize = 1u << 16;
constexpr u32 kRingMask = kRingSize - 1;

std::atomic<u64> g_ring[kRingSize];
// Alongside each PC: the link register and the thread. A flat PC histogram
// over seven threads cannot say which thread was *running* - a worker blocked
// in a futex and the main thread spinning at 100% both sample as libc
// `syscall` - and a leaf like `syscall` or `memcpy` is only interesting for
// who called it. LR names the caller of a leaf; the tid splits the threads.
std::atomic<u64> g_ring_lr[kRingSize];
std::atomic<u32> g_ring_tid[kRingSize];
std::atomic<u32> g_head{0};
std::atomic<u64> g_taken{0};
std::atomic<u64> g_dropped{0};

// Base address of our own shared object, so a sample is an offset that survives
// ASLR and can be looked up in the unstripped .so on the host.
uintptr_t g_module_base = 0;
std::atomic<bool> g_running{false};

#if defined(__ANDROID__)
void OnSigProf(int, siginfo_t *, void *ctx) {
  if (!ctx)
    return;
  const auto *uc = static_cast<const ucontext_t *>(ctx);
  const uintptr_t pc = static_cast<uintptr_t>(uc->uc_mcontext.pc);
  // The RAW pc, not an offset from libreblue.so.
  //
  // This used to discard anything below our own module base and anything more
  // than 128 MB above it, on the reasoning that a PC outside libreblue is "the
  // runtime, the driver or libc" and would dilute the histogram. On Android
  // that reasoning is wrong twice over: librexruntime.so is a *separate* 9.7 MB
  // object holding the SDK's kernel emulation, memory subsystem and threading -
  // work the guest is directly paying for - and a PC that survives the filter
  // but lands outside libreblue's 33.8 MB of text cannot be attributed to
  // anything at all. Measured on a Quest 2: 90.8% of samples resolved to no
  // symbol, and the single hottest address was 30% of the profile sitting
  // between the two libraries. On desktop every one of those is linked into one
  // image, so the identical filter keeps them - which is why this never showed
  // up there.
  //
  // The dump writes /proc/self/maps alongside the histogram so the host can
  // attribute each PC to a module. ASLR means an offset from one library is
  // meaningless for another, so the map has to travel with the samples.
  const u32 slot = g_head.fetch_add(1, std::memory_order_relaxed) & kRingMask;
  g_ring[slot].store(static_cast<u64>(pc), std::memory_order_relaxed);
  g_ring_lr[slot].store(static_cast<u64>(uc->uc_mcontext.regs[30]),
                        std::memory_order_relaxed);
  // A raw syscall is async-signal-safe; gettid() is not declared below API 30.
  g_ring_tid[slot].store(static_cast<u32>(::syscall(__NR_gettid)),
                         std::memory_order_relaxed);
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
#endif

#if defined(_WIN32)
// No thread names to filter on here, so every thread in the process is
// sampled. The histogram sorts that out: the guest dominates it because the
// guest is what runs.
void CollectTids(std::vector<int> &out) {
  out.clear();
  HANDLE snap = ::CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snap == INVALID_HANDLE_VALUE)
    return;
  THREADENTRY32 te{};
  te.dwSize = sizeof(te);
  const DWORD self = ::GetCurrentProcessId();
  if (::Thread32First(snap, &te)) {
    do {
      if (te.th32OwnerProcessID == self)
        out.push_back(static_cast<int>(te.th32ThreadID));
    } while (::Thread32Next(snap, &te));
  }
  ::CloseHandle(snap);
}
#else
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
#endif

void RecordPc(uintptr_t pc) {
  if (pc < g_module_base)
    return;
  const u64 offset = static_cast<u64>(pc - g_module_base);
  if (offset > 0x8000000ull)
    return;
  const u32 slot = g_head.fetch_add(1, std::memory_order_relaxed) & kRingMask;
  g_ring[slot].store(offset, std::memory_order_relaxed);
  g_ring_lr[slot].store(0, std::memory_order_relaxed);
  g_ring_tid[slot].store(0, std::memory_order_relaxed);
  g_taken.fetch_add(1, std::memory_order_relaxed);
}

void SamplerThread() {
  const i32 hz = REXCVAR_GET(bd_sample_hz) > 0 ? REXCVAR_GET(bd_sample_hz) : 1000;
  const long period_ns = 1000000000L / hz;
  std::vector<int> tids;
  u64 iterations = 0;
#if defined(__ANDROID__)
  const int pid = ::getpid();
#else
  const DWORD self_pid = ::GetCurrentProcessId();
  const DWORD self_tid = ::GetCurrentThreadId();
#endif

  while (g_running.load(std::memory_order_relaxed)) {
    // Guest threads come and go with the scene, so the list is refreshed rather
    // than captured once - a run that samples only the threads alive at startup
    // misses exactly the workers that appear in a field scene.
    if ((iterations++ % 512) == 0)
      CollectTids(tids);

#if defined(__ANDROID__)
    for (const int tid : tids) {
      // tgkill rather than pthread_kill: we have raw tids from /proc, and a
      // thread that has exited since the scan just returns ESRCH.
      if (::syscall(__NR_tgkill, pid, tid, SIGPROF) != 0)
        g_dropped.fetch_add(1, std::memory_order_relaxed);
    }
    timespec ts{0, period_ns};
    ::nanosleep(&ts, nullptr);
#else
    (void)self_pid;
    for (const int tid : tids) {
      if (static_cast<DWORD>(tid) == self_tid)
        continue;
      HANDLE h = ::OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE,
                              static_cast<DWORD>(tid));
      if (!h) {
        g_dropped.fetch_add(1, std::memory_order_relaxed);
        continue;
      }
      // Suspend/GetThreadContext/Resume is the Windows analogue of SIGPROF.
      // The window is a few microseconds and the target is never left
      // suspended on any path out of here.
      if (::SuspendThread(h) != DWORD(-1)) {
        CONTEXT ctx{};
        ctx.ContextFlags = CONTEXT_CONTROL;
        if (::GetThreadContext(h, &ctx))
          RecordPc(static_cast<uintptr_t>(ctx.Rip));
        ::ResumeThread(h);
      }
      ::CloseHandle(h);
    }
    ::Sleep(static_cast<DWORD>(period_ns / 1000000L > 0 ? period_ns / 1000000L : 1));
#endif
  }
}

} // namespace
#endif

void SamplingProfilerDump() {
#if BD_HAVE_SAMPLER
  const u64 taken = g_taken.load(std::memory_order_relaxed);
  if (!taken)
    return;

  // Only the most recent kRingSize samples survive, which is what we want: a
  // dump describes the recent past rather than averaging over the load screen.
  const u32 head = g_head.load(std::memory_order_relaxed);
  const u32 count = (taken < kRingSize) ? static_cast<u32>(taken) : kRingSize;

  std::unordered_map<u64, u32> hist;
  hist.reserve(count);
  // (tid, pc, lr) -> count, for the per-thread and caller views. Keyed on a
  // string because the tuple is three words and this runs once.
  std::unordered_map<std::string, u32> hist2;
  std::unordered_map<u32, u32> per_tid;
  for (u32 i = 0; i < count; ++i) {
    const u32 slot = (head - 1 - i) & kRingMask;
    const u64 off = g_ring[slot].load(std::memory_order_relaxed);
    if (off) {
      ++hist[off];
      const u64 lr = g_ring_lr[slot].load(std::memory_order_relaxed);
      const u32 tid = g_ring_tid[slot].load(std::memory_order_relaxed);
      char key[64];
      std::snprintf(key, sizeof(key), "%u %llx %llx", tid,
                    static_cast<unsigned long long>(off),
                    static_cast<unsigned long long>(lr));
      ++hist2[key];
      ++per_tid[tid];
    }
  }

  std::vector<std::pair<u64, u32>> sorted(hist.begin(), hist.end());
  std::sort(sorted.begin(), sorted.end(),
            [](const auto &a, const auto &b) { return a.second > b.second; });

  const auto dir = bd::AppRootFolder() / "logs";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  const std::string path = (dir / "guest_profile.txt").string();
  if (FILE *f = std::fopen(path.c_str(), "w")) {
    std::fprintf(f, "# raw PCs, most frequent first, with the module map that\n"
                    "# makes them attributable. resolve with:\n"
                    "#   python tools/symbolize_profile.py %s\n"
                    "# samples=%llu unique=%zu dropped=%llu\n",
                 path.c_str(), static_cast<unsigned long long>(taken),
                 sorted.size(),
                 static_cast<unsigned long long>(
                     g_dropped.load(std::memory_order_relaxed)));

    // The executable mappings, so a PC outside libreblue can still be named.
    // Without this, ASLR makes every sample that left our own module
    // unattributable - which was 90.8% of them.
    std::fprintf(f, "# MODULES\n");
#if defined(__ANDROID__)
    if (FILE *m = std::fopen("/proc/self/maps", "r")) {
      char line[512];
      while (std::fgets(line, sizeof(line), m)) {
        // Executable file-backed mappings only.
        if (std::strstr(line, "r-xp") && std::strchr(line, '/'))
          std::fprintf(f, "# MAP %s", line);
      }
      std::fclose(m);
    }
#endif
    // Thread names, so the per-thread section below reads as "SDLThread" and
    // not as a number that changes every launch.
    for (const auto &kv : per_tid) {
      char name[64] = {0};
#if defined(__ANDROID__)
      char p[64];
      std::snprintf(p, sizeof(p), "/proc/self/task/%u/comm", kv.first);
      if (FILE *c = std::fopen(p, "r")) {
        if (std::fgets(name, sizeof(name), c)) {
          if (char *nl = std::strchr(name, '\n'))
            *nl = 0;
        } else {
          name[0] = 0;
        }
        std::fclose(c);
      }
#endif
      std::fprintf(f, "# TID %u %u %s\n", kv.first, kv.second,
                   name[0] ? name : "?");
    }
    std::fprintf(f, "# SAMPLES\n");
    for (const auto &kv : sorted)
      std::fprintf(f, "%llx %u\n", static_cast<unsigned long long>(kv.first),
                   kv.second);
    // "tid pc lr count": the same samples with their thread and caller. Lines
    // have four fields so the plain reader above skips them.
    std::fprintf(f, "# SAMPLES2\n");
    for (const auto &kv : hist2)
      std::fprintf(f, "%s %u\n", kv.first.c_str(), kv.second);
    std::fclose(f);
  }

  BD_INFO("[profile] {} samples, {} unique addresses, {} dropped -> {}", taken,
          sorted.size(), g_dropped.load(std::memory_order_relaxed), path);
#endif
}

void SamplingProfilerTick() {
#if BD_HAVE_SAMPLER
  if (!REXCVAR_GET(bd_sample_profiler))
    return;
  static bool started = false;
  if (!started) {
    started = true;

#if defined(_WIN32)
    // The whole executable, so an offset here matches what a disassembler or
    // llvm-nm reports for the same image.
    g_module_base = reinterpret_cast<uintptr_t>(::GetModuleHandleW(nullptr));
    if (!g_module_base) {
      BD_ERROR("[profile] cannot find our own module base; sampling disabled");
      return;
    }
#else
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
#endif

    g_running.store(true, std::memory_order_relaxed);
    std::thread(SamplerThread).detach();
    BD_INFO("[profile] sampling guest threads at {} Hz, base {:#x}",
            REXCVAR_GET(bd_sample_hz), g_module_base);
  }

  // Dump once at the capture moment and stop. The ring holds the most recent
  // 65536 samples - 16 seconds at 1000 Hz over four threads - so a periodic
  // dump describes whatever the run was doing when the last tick landed. On
  // 2026-09-01 that was a transition: 78% of samples in libc's syscall and
  // nanosleep, 18% in the Adreno shader compiler, and 0.4% in this module.
  // bd_capture_after_s already names the moment a run is known to be in a
  // field scene, so the profile is pinned to the same moment as the capture.
  static std::chrono::steady_clock::time_point t0 =
      std::chrono::steady_clock::now();
  static bool dumped_at_capture = false;
  const double after = REXCVAR_GET(bd_capture_after_s);
  if (after > 0.0 && !dumped_at_capture) {
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
            .count();
    if (elapsed >= after) {
      dumped_at_capture = true;
      SamplingProfilerDump();
      g_running.store(false, std::memory_order_relaxed);
      BD_INFO("[profile] dumped at {:.0f}s and stopped - this profile is the "
              "16s before the capture", elapsed);
    }
    return;
  }
  if (dumped_at_capture)
    return;

  // Periodic dumps, so a run that is killed rather than exited still leaves a
  // profile behind.
  static u32 ticks = 0;
  if ((++ticks % 600u) == 0u)
    SamplingProfilerDump();
#endif
}

} // namespace bd
