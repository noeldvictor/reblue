/**
 * @file    engine/guest_math.cpp
 * @brief   Host implementations of pure guest maths functions.
 *
 * The recompiled guest is ordinary C++, so a function with no state and a
 * known signature can be taken over wholesale by a host one - which is the
 * cheapest form of "stop running the PowerPC". What makes it safe is not
 * reading the disassembly carefully; it is checking the replacement against
 * the original while both exist. `DEFINE_REX_FUNC` emits the recompiled body
 * as `__imp__<name>` and the public symbol as a weak alias, so a host
 * `REX_HOOK_RAW` can call the original and compare.
 *
 * `bd_verify_guest_math` turns that check on. It costs a call to the original
 * plus a couple of libm calls per invocation, so it is a diagnostic rather
 * than something to ship enabled - but it is what turns "this looks like
 * sin/cos" into a fact, and a wrong guess here would rotate the entire game.
 *
 * @copyright Copyright (c) 2026 re:Blue contributors
 * @license   BSD 3-Clause - see LICENSE
 */
#include "core/hooks.h"
#include "core/logging.h"
#include "core/memory_helpers.h"

#include <rex/ppc.h>
#include <rex/types.h>

#include <atomic>
#include <cmath>
#include <cstring>

REXCVAR_DECLARE(bool, bd_verify_guest_math);
REXCVAR_DECLARE(bool, bd_host_sincos);
REXCVAR_DECLARE(bool, bd_host_matrix_copy);

namespace {

float LoadGuestFloat(u32 va) {
  const u32 bits = bd::mem::try_load<u32>(va);
  float f = 0.0f;
  std::memcpy(&f, &bits, sizeof(f));
  return f;
}

bool StoreGuestFloat(u32 va, float f) {
  u32 bits = 0;
  std::memcpy(&bits, &f, sizeof(bits));
  return bd::mem::try_store<u32>(va, bits);
}

} // namespace

// bdSinCos(angle in f1, out_a in r3, out_b in r4) - writes one float through
// each pointer. Which of the two is the sine is not obvious from the
// polynomial, and the two are indistinguishable in any test where the angle
// happens to be a multiple of pi/4, so it is established by measurement here
// rather than by reading.
REX_EXTERN(__imp__bdSinCos);
REX_HOOK_RAW(bdSinCos) {
  // Measured: *r3 is the sine and *r4 the cosine, to within 1e-4 across the
  // angles the game actually asks for. Established with the verification path
  // below rather than read off the polynomial - and worth noting that at an
  // angle of pi/4 both outputs are 0.707107 and every comparison passes, so it
  // is the off-axis samples that carry the answer.
  if (REXCVAR_GET(bd_host_sincos) && !REXCVAR_GET(bd_verify_guest_math)) {
    const float angle = static_cast<float>(ctx.f1.f64);
    // Both pointers are written by the original on every path, so a failed
    // store here has to fall back rather than leave one of them stale.
    if (StoreGuestFloat(ctx.r3.u32, std::sin(angle)) &&
        StoreGuestFloat(ctx.r4.u32, std::cos(angle)))
      return;
  }

  if (!REXCVAR_GET(bd_verify_guest_math)) {
    __imp__bdSinCos(ctx, base);
    return;
  }

  const float angle = static_cast<float>(ctx.f1.f64);
  const u32 out_a = ctx.r3.u32;
  const u32 out_b = ctx.r4.u32;
  __imp__bdSinCos(ctx, base);

  static std::atomic<int> shown{0};
  const int n = shown.fetch_add(1, std::memory_order_relaxed);
  if (n >= 8)
    return;

  const float a = LoadGuestFloat(out_a);
  const float b = LoadGuestFloat(out_b);
  const float s = std::sin(angle);
  const float c = std::cos(angle);
  BD_INFO("[sincos] angle={:+.5f}  *r3={:+.6f}  *r4={:+.6f}  |  sin={:+.6f} "
          "cos={:+.6f}  |  r3~sin={} r3~cos={} r4~sin={} r4~cos={}",
          angle, a, b, s, c, std::fabs(a - s) < 1e-4f, std::fabs(a - c) < 1e-4f,
          std::fabs(b - s) < 1e-4f, std::fabs(b - c) < 1e-4f);
}

// bdMatrixCopyAligned(dst in r3, src in r4) - 64 bytes, four vectors. The guest
// loads each with the lvlx/lvrx/vor unaligned idiom and stores it back through
// a full byte-reverse mask, so the reversal on the way in is undone on the way
// out and the net effect should be a plain byte copy. "Should be" is not good
// enough to act on, so bd_verify_guest_math runs the original and then checks
// the destination against the source byte for byte.
//
// A null source makes the guest substitute a default matrix from a global; that
// path is left to the original rather than reimplemented, because it is rare
// and getting the global's address wrong would be silent.
REX_EXTERN(__imp__bdMatrixCopyAligned);
REX_HOOK_RAW(bdMatrixCopyAligned) {
  constexpr u32 kMatrixBytes = 64;
  const u32 dst = ctx.r3.u32;
  const u32 src = ctx.r4.u32;

  if (REXCVAR_GET(bd_verify_guest_math)) {
    __imp__bdMatrixCopyAligned(ctx, base);
    static std::atomic<int> shown{0};
    if (src && shown.fetch_add(1, std::memory_order_relaxed) < 6) {
      const auto *d = bd::mem::try_at<const u8>(dst);
      const auto *sp = bd::mem::try_at<const u8>(src);
      const bool tail_ok = bd::mem::try_at<const u8>(dst + kMatrixBytes - 1) &&
                           bd::mem::try_at<const u8>(src + kMatrixBytes - 1);
      if (d && sp && tail_ok) {
        BD_INFO("[matcopy] dst={:08X} src={:08X} identical={}", dst, src,
                std::memcmp(d, sp, kMatrixBytes) == 0);
      }
    }
    return;
  }

  if (REXCVAR_GET(bd_host_matrix_copy) && src && dst) {
    auto *d = bd::mem::try_at<u8>(dst);
    const auto *sp = bd::mem::try_at<const u8>(src);
    // Both ends of both blocks, so a matrix straddling the end of a mapping
    // falls back rather than reading or writing past it.
    if (d && sp && bd::mem::try_at<const u8>(dst + kMatrixBytes - 1) &&
        bd::mem::try_at<const u8>(src + kMatrixBytes - 1)) {
      std::memcpy(d, sp, kMatrixBytes);
      return;
    }
  }

  __imp__bdMatrixCopyAligned(ctx, base);
}
