/**
 * @file    platform/crash_handler.cpp
 * @copyright Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *            All rights reserved.
 * @license   BSD 3-Clause License
 *            See LICENSE file in the project root for full license text.
 */
#include "platform/crash_handler.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <rex/types.h>
#include <string>
#include <typeinfo>

#include <rex/exception_handler.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/version.h>

#include "core/logging.h"
#include "platform/fatal_dialog.h"

#if defined(_WIN32)
// Linker-provided base of THIS module (the host exe). Its ADDRESS is the
// post-ASLR load base. Using it avoids including Windows.h (project rule) while
// still reducing a faulting host PC to an RVA that the .map/.pdb resolves
// offline.
extern "C" const unsigned char __ImageBase;

// Exported by kernel32/ntdll. Declared locally to avoid pulling in Windows.h.
// x64 has a single calling convention so __stdcall is a no-op there.
extern "C"
    __declspec(dllimport) unsigned short __stdcall RtlCaptureStackBackTrace(
        unsigned long frames_to_skip, unsigned long frames_to_capture,
        void **back_trace, unsigned long *back_trace_hash);
#else
#include <cxxabi.h>
#include <dlfcn.h>
#if defined(__ANDROID__)
// bionic ships no <execinfo.h>: backtrace() and backtrace_symbols() are glibc
// extensions Android never adopted. The unwinder is there regardless.
#include <unwind.h>
#else
#include <execinfo.h>
#endif
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#endif

namespace bd::platform {
namespace {

// Guest virtual address space is the low 4 GB of the host mapping.
constexpr u64 kGuestAddressSpaceSize = 0x100000000ull;

// Generous upper bound on the host exe image span (it embeds all recompiled
// guest code, so it is large). Used only to decide whether a host address
// reduces to an in-module RVA, and oversizing it never misclassifies an
// in-module address.
constexpr u64 kHostImageSpan = 0x20000000ull; // 512 MiB

#if defined(_WIN32)
u64 HostModuleBase() { return reinterpret_cast<u64>(&__ImageBase); }
#else
// No module base RVA scheme on Linux. backtrace_symbols prints symbol names
// directly, so the base is only used to suppress bogus RVA lines.
u64 HostModuleBase() { return 0; }
#endif

// Resolved at install, never on the crash path: the handler must not call into
// the filesystem while unwinding a fault.
std::string g_host_module_name;

const char *HostModuleName() {
  return g_host_module_name.empty() ? "host" : g_host_module_name.c_str();
}

// Set by whichever crash path reports first. A second entry, whether a fault
// inside a handler or the abort() that ends the terminate handler, skips
// reporting and goes straight to dying, so no path can loop.
std::atomic_flag s_reporting = ATOMIC_FLAG_INIT;

#if !defined(_WIN32)
// Restore the kernel's default action for 'sig' and take it here. Returning
// from a POSIX handler instead (what the SDK dispatcher does when no handler
// claims the fault) re-executes the faulting instruction forever: on Linux
// there is no equivalent of EXCEPTION_CONTINUE_SEARCH handing off to the OS.
// Dying through the default action is also what produces the core dump.
[[noreturn]] void DieWithDefaultDisposition(int sig) {
  struct sigaction dfl{};
  dfl.sa_handler = SIG_DFL;
  sigemptyset(&dfl.sa_mask);
  sigaction(sig, &dfl, nullptr);

  // A signal is blocked for the duration of its own handler, so unblock it and
  // the raise below comes back here rather than after an unreachable return.
  sigset_t unblock;
  sigemptyset(&unblock);
  sigaddset(&unblock, sig);
  pthread_sigmask(SIG_UNBLOCK, &unblock, nullptr);

  raise(sig);
  _exit(128 + sig); // only reachable if the default action did not fire
}
#endif

const char *ExceptionCodeName(rex::arch::Exception::Code code) {
  using Code = rex::arch::Exception::Code;
  switch (code) {
  case Code::kAccessViolation:
    return "ACCESS_VIOLATION";
  case Code::kIllegalInstruction:
    return "ILLEGAL_INSTRUCTION";
  default:
    return "UNKNOWN";
  }
}

const char *AvOperationName(rex::arch::Exception::AccessViolationOperation op) {
  using Op = rex::arch::Exception::AccessViolationOperation;
  switch (op) {
  case Op::kRead:
    return "read";
  case Op::kWrite:
    return "write";
  default:
    return "unknown";
  }
}

// Walk the faulting thread's stack (the VEH runs inline on it) and log each
// return address with its module-relative RVA, giving the call chain that
// led to the separately logged faulting PC. The top frames are the crash
// handler itself, so the faulting caller is the first module+RVA below the
// OS dispatch frames.
void LogBacktrace(u64 base) {
#if defined(_WIN32)
  void *frames[32] = {};
  const unsigned short n = RtlCaptureStackBackTrace(0, 32, frames, nullptr);
  if (!n)
    return;
  BD_CRITICAL("backtrace ({} frames, top frames are the crash handler):", n);
  for (unsigned short i = 0; i < n; ++i) {
    const u64 a = reinterpret_cast<u64>(frames[i]);
    if (a >= base && a - base < kHostImageSpan) {
      BD_CRITICAL("    [{:>2}] {:#018x}  ({}+{:#010x})", i, a, HostModuleName(),
                  a - base);
    } else {
      BD_CRITICAL("    [{:>2}] {:#018x}", i, a);
    }
  }
#elif defined(__ANDROID__)
  (void)base;
  // bionic has no backtrace(), and installing our own signal handler means
  // debuggerd never writes a tombstone either - so without this a crash on
  // Android reports an address and nothing else. libgcc's unwinder is always
  // present, and dladdr turns each frame into a symbol.
  struct Frames {
    void *pc[32];
    int count;
  } frames{};
  _Unwind_Backtrace(
      [](_Unwind_Context *ctx, void *arg) -> _Unwind_Reason_Code {
        auto *f = static_cast<Frames *>(arg);
        const uintptr_t ip = _Unwind_GetIP(ctx);
        if (!ip || f->count >= 32)
          return _URC_END_OF_STACK;
        f->pc[f->count++] = reinterpret_cast<void *>(ip);
        return _URC_NO_REASON;
      },
      &frames);

  BD_CRITICAL("backtrace ({} frames, top frames are the crash handler):",
              frames.count);
  for (int i = 0; i < frames.count; ++i) {
    const u64 a = reinterpret_cast<u64>(frames.pc[i]);
    Dl_info info{};
    if (dladdr(frames.pc[i], &info) && info.dli_fname) {
      // Offsets are relative to the shared object, which is what an addr2line
      // against the unstripped .so wants.
      const u64 rel = a - reinterpret_cast<u64>(info.dli_fbase);
      BD_CRITICAL("    [{:>2}] {:#018x}  {}+{:#x}{}{}", i, a, info.dli_fname,
                  rel, info.dli_sname ? "  " : "",
                  info.dli_sname ? info.dli_sname : "");
    } else {
      BD_CRITICAL("    [{:>2}] {:#018x}", i, a);
    }
  }
#else
  (void)base;
  void *frames[32] = {};
  const int n = backtrace(frames, 32);
  if (n <= 0)
    return;
  BD_CRITICAL("backtrace ({} frames, top frames are the crash handler):", n);
  char **syms = backtrace_symbols(frames, n);
  for (int i = 0; i < n; ++i) {
    const u64 a = reinterpret_cast<u64>(frames[i]);
    if (syms && syms[i]) {
      BD_CRITICAL("    [{:>2}] {:#018x}  {}", i, a, syms[i]);
    } else {
      BD_CRITICAL("    [{:>2}] {:#018x}", i, a);
    }
  }
  free(syms);
#endif
}

// An indirect call to a garbage target faults on instruction fetch AFTER
// pushing the return address, so the caller is still at [sp] when rip is
// unwalkable - which is also when backtrace() faults and takes the whole frame
// list with it. Emitted in backtrace_symbols' module(+0xoffset) form so
// dbg-reblue.sh symbolizes these unchanged.
void LogStackCodeAddresses(const rex::arch::HostThreadContext *ctx,
                           [[maybe_unused]] u64 base) {
  if (!ctx)
    return;
#if REX_ARCH_AMD64
  const u64 sp = ctx->int_registers[4];
#elif REX_ARCH_ARM64
  const u64 sp = ctx->sp;
#else
  return;
#endif
#if REX_ARCH_AMD64 || REX_ARCH_ARM64
  if (!sp || (sp % sizeof(u64)) != 0)
    return;
  // Never leave sp's own page: the next one can be the guard page, and a second
  // fault in here costs the whole report.
  constexpr u64 kPageSize = 4096;
  const u64 limit = std::min(sp + 64 * sizeof(u64), (sp | (kPageSize - 1)) + 1);
  BD_CRITICAL("stack code addresses (sp {:#018x}, first is the return address "
              "of the call that faulted):",
              sp);
  u32 found = 0;
  for (u64 p = sp; p + sizeof(u64) <= limit; p += sizeof(u64)) {
    u64 v = 0;
    std::memcpy(&v, reinterpret_cast<const void *>(p), sizeof(v));
    if (!v)
      continue;
#if defined(_WIN32)
    if (v < base || v - base >= kHostImageSpan)
      continue;
    BD_CRITICAL("    [sp+{:#05x}] {:#018x}  ({}+{:#010x})", p - sp, v,
                HostModuleName(), v - base);
#else
    Dl_info info{};
    // dladdr resolves mapped module addresses only, so it doubles as the
    // is-this-code test: stack data words fail to resolve.
    if (!dladdr(reinterpret_cast<void *>(v), &info) || !info.dli_fbase ||
        !info.dli_fname) {
      continue;
    }
    const char *slash = std::strrchr(info.dli_fname, '/');
    BD_CRITICAL("    [sp+{:#05x}] {:#018x}  {}(+{:#010x})", p - sp, v,
                slash ? slash + 1 : info.dli_fname,
                v - reinterpret_cast<u64>(info.dli_fbase));
#endif
    if (++found >= 24)
      break;
  }
  if (!found)
    BD_CRITICAL("    (no module addresses on the stack)");
#endif
}

// Read the register file straight off the public HostThreadContext members -
// the SDK's GetRegisterName/GetStringFromValue helpers are header-declared but
// not exported by the runtime lib, so format the values here instead.
void LogRegisters(const rex::arch::HostThreadContext *ctx) {
  if (!ctx)
    return;
#if REX_ARCH_AMD64
  static const char *const kNames[16] = {
      "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
      "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15"};
  BD_CRITICAL("    rip = {:#018x}   eflags = {:#010x}", ctx->rip, ctx->eflags);
  for (int i = 0; i < 16; ++i) {
    BD_CRITICAL("    {:>3} = {:#018x}", kNames[i], ctx->int_registers[i]);
  }
#elif REX_ARCH_ARM64
  BD_CRITICAL("    pc = {:#018x}   sp = {:#018x}   pstate = {:#010x}", ctx->pc,
              ctx->sp, ctx->pstate);
  for (int i = 0; i < 31; ++i) {
    BD_CRITICAL("    x{:<2} = {:#018x}", i, ctx->x[i]);
  }
#endif
}

// Chained after the SDK MMIO handler, so it only sees faults the runtime
// declined. Returns false: never recover, let the OS produce its default
// termination.
bool CrashHandler(rex::arch::Exception *ex, void * /*data*/) {
#if defined(_WIN32)
  if (s_reporting.test_and_set(std::memory_order_acq_rel))
    return false;
#else
  const int sig = ex->code() == rex::arch::Exception::Code::kIllegalInstruction
                      ? SIGILL
                      : SIGSEGV;
  if (s_reporting.test_and_set(std::memory_order_acq_rel))
    DieWithDefaultDisposition(sig);
#endif

  const u64 host_base = HostModuleBase();

  BD_CRITICAL("================ reblue host crash ================");
  BD_CRITICAL("build: {}", REXGLUE_BUILD_TITLE);
  BD_CRITICAL("module base: {} @ {:#018x}", HostModuleName(), host_base);
  BD_CRITICAL("exception: {} @ host pc {:#018x}", ExceptionCodeName(ex->code()),
              ex->pc());
  if (host_base && ex->pc() >= host_base &&
      ex->pc() - host_base < kHostImageSpan) {
    BD_CRITICAL("faulting RVA: {}+{:#010x}", HostModuleName(),
                ex->pc() - host_base);
  }

  if (ex->code() == rex::arch::Exception::Code::kAccessViolation) {
    const u64 fa = ex->fault_address();
    BD_CRITICAL("fault address: {:#018x} ({})", fa,
                AvOperationName(ex->access_violation_operation()));

    // Map a guest range fault back to its guest VA, the actionable triage line
    // for a fault inside recompiled code.
    auto *rt = rex::Runtime::instance();
    u8 *membase = rt ? rt->virtual_membase() : nullptr;
    const u64 base = reinterpret_cast<u64>(membase);
    if (membase && fa >= base && fa < base + kGuestAddressSpaceSize) {
      BD_CRITICAL("guest fault VA: {:#010x}", static_cast<u32>(fa - base));
    }
  }

  BD_CRITICAL("registers:");
  LogRegisters(ex->thread_context());
  LogStackCodeAddresses(ex->thread_context(), host_base);
  // Flush before the unwind, not just after it: backtrace() faults on a garbage
  // rip, and what is already logged outweighs the frames it might add.
  rex::FlushLogging();
  LogBacktrace(host_base);
  BD_CRITICAL("===================================================");

  // Drain sinks before the OS tears us down, since a crash bypasses OnShutdown.
  rex::FlushLogging();

  const bool in_module = host_base && ex->pc() >= host_base &&
                         ex->pc() - host_base < kHostImageSpan;
  const std::string where =
      in_module ? fmt::format("{}+{:#010x}", HostModuleName(),
                              ex->pc() - host_base)
                : fmt::format("{:#018x}", ex->pc());
  ShowFatalError("reblue Crashed",
                 fmt::format("reblue hit a fatal error and has to close.\n\n"
                             "{} at {}",
                             ExceptionCodeName(ex->code()), where));
#if !defined(_WIN32)
  DieWithDefaultDisposition(sig);
#endif
  return false;
}

std::string TypeName(const std::type_info &ti) {
#if defined(_WIN32)
  return ti.name();
#else
  int status = 0;
  char *demangled = abi::__cxa_demangle(ti.name(), nullptr, nullptr, &status);
  std::string out = (status == 0 && demangled) ? demangled : ti.name();
  std::free(demangled);
  return out;
#endif
}

// An uncaught C++ exception (bad_alloc, a filesystem or plume throw) ends the
// process through std::terminate, which no signal handler sees until abort()
// has already discarded the exception. Naming it here is usually the whole
// diagnosis.
[[noreturn]] void TerminateHandler() {
  if (s_reporting.test_and_set(std::memory_order_acq_rel))
    std::_Exit(3);

  std::string detail = "std::terminate with no active exception";
  if (std::exception_ptr active = std::current_exception()) {
    try {
      std::rethrow_exception(active);
    } catch (const std::exception &e) {
      detail = fmt::format("uncaught {}: {}", TypeName(typeid(e)), e.what());
    } catch (...) {
      detail = "uncaught exception of non-std type";
    }
  }

  BD_CRITICAL("================ reblue host crash ================");
  BD_CRITICAL("build: {}", REXGLUE_BUILD_TITLE);
  BD_CRITICAL("{}", detail);
  LogBacktrace(HostModuleBase());
  BD_CRITICAL("===================================================");
  rex::FlushLogging();

  ShowFatalError(
      "reblue Crashed",
      fmt::format("reblue hit a fatal error and has to close.\n\n{}", detail));
  std::abort();
}

#if !defined(_WIN32)
const char *SignalName(int sig) {
  switch (sig) {
  case SIGABRT:
    return "SIGABRT (abort, failed assert, or uncaught exception)";
  case SIGBUS:
    return "SIGBUS (misaligned or unmapped memory access)";
  case SIGFPE:
    return "SIGFPE (arithmetic fault)";
  default:
    return "fatal signal";
  }
}

// SIGSEGV/SIGILL stay with the SDK dispatcher, which needs first refusal on
// them to service guest MMIO. These three have no owner, so take them directly:
// without this the process vanishes with nothing in the log at all.
void FatalSignalHandler(int sig, siginfo_t *info, void *context) {
  if (s_reporting.test_and_set(std::memory_order_acq_rel))
    DieWithDefaultDisposition(sig);

  BD_CRITICAL("================ reblue host crash ================");
  BD_CRITICAL("build: {}", REXGLUE_BUILD_TITLE);
  BD_CRITICAL("signal: {} {}", sig, SignalName(sig));
  if (info && (sig == SIGBUS || sig == SIGFPE)) {
    BD_CRITICAL("fault address: {:#018x}",
                reinterpret_cast<u64>(info->si_addr));
#if defined(__ANDROID__) && defined(__aarch64__)
    // The unwinder cannot walk back across __kernel_rt_sigreturn, so the
    // backtrace below starts inside this handler and never reaches the code
    // that faulted. The context does carry it: report the faulting PC and its
    // offset within whichever shared object owns it, which is what an
    // addr2line against the unstripped .so needs.
    if (context) {
      const auto *uc = static_cast<const ucontext_t *>(context);
      const u64 pc = uc->uc_mcontext.pc;
      Dl_info info_pc{};
      if (dladdr(reinterpret_cast<void *>(pc), &info_pc) && info_pc.dli_fname) {
        BD_CRITICAL("faulting pc:   {:#018x}  {}+{:#x}", pc, info_pc.dli_fname,
                    pc - reinterpret_cast<u64>(info_pc.dli_fbase));
      } else {
        BD_CRITICAL("faulting pc:   {:#018x}", pc);
      }
    }
#endif
  }
  LogBacktrace(HostModuleBase());
  BD_CRITICAL("===================================================");
  rex::FlushLogging();

  ShowFatalError("reblue Crashed",
                 fmt::format("reblue hit a fatal error and has to close.\n\n{}",
                             SignalName(sig)));
  DieWithDefaultDisposition(sig);
}
#endif

} // namespace

void InstallCrashStackForThread() {
#if !defined(_WIN32)
  // A stack overflow SIGSEGV has, by definition, no room left to run a handler
  // on the faulting stack. Deliberately leaked: the alt stack must outlive
  // every destructor the thread could still run.
  constexpr size_t kAltStackSize = 128 * 1024; // fmt formatting needs the room
  static thread_local void *s_alt_stack = nullptr;
  if (s_alt_stack)
    return;
  s_alt_stack = std::malloc(kAltStackSize);
  if (!s_alt_stack)
    return;

  stack_t ss{};
  ss.ss_sp = s_alt_stack;
  ss.ss_size = kAltStackSize;
  ss.ss_flags = 0;
  sigaltstack(&ss, nullptr);
#endif
}

void InstallTerminateHandler() {
  if (g_host_module_name.empty())
    g_host_module_name =
        rex::filesystem::GetExecutablePath().filename().string();

  std::set_terminate(&TerminateHandler);

#if !defined(_WIN32)
  InstallCrashStackForThread();

  struct sigaction sa{};
  sa.sa_sigaction = &FatalSignalHandler;
  sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sigemptyset(&sa.sa_mask);
  for (const int sig : {SIGABRT, SIGBUS, SIGFPE})
    sigaction(sig, &sa, nullptr);
#endif
}

void InstallCrashHandler() {
  rex::arch::ExceptionHandler::Install(&CrashHandler, nullptr);
  InstallTerminateHandler();

#if !defined(_WIN32)
  // The SDK installs SIGSEGV/SIGILL without SA_ONSTACK, so a stack overflow
  // fault re-faults inside its own handler and dies silently. Keep its
  // dispatcher, add the flag.
  for (const int sig : {SIGSEGV, SIGILL}) {
    struct sigaction current{};
    if (sigaction(sig, nullptr, &current) == 0 &&
        !(current.sa_flags & SA_ONSTACK)) {
      current.sa_flags |= SA_ONSTACK;
      sigaction(sig, &current, nullptr);
    }
  }
#endif

  BD_DEBUG("[crash] last-chance handler installed");
}

} // namespace bd::platform
