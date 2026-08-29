/*
 * Equivalence check for the NEON constant byte-swap in
 * src/gpu/constant_buffers.cpp.
 *
 * That function runs twice per draw over 4 KiB each, ~2957 draws a frame, so it
 * is worth vectorising - but it also feeds every shader constant in the game,
 * so a wrong lane would corrupt rendering in a way that is very hard to
 * attribute. This runs both implementations over the same bytes and compares.
 *
 * The two kernels below are copied verbatim from constant_buffers.cpp rather
 * than included, because that file pulls in plume and the whole GPU layer.
 * If the original changes, change these too - that is the cost of the copy and
 * it is cheaper than the alternative.
 *
 * Build and run on the device:
 *   clang --target=aarch64-linux-android29 -O2 tools/neon_swap_test/neon_swap_test.c -o out/neon_swap_test
 *   adb push out/neon_swap_test /data/local/tmp/ && adb shell /data/local/tmp/neon_swap_test
 */
#include <arm_neon.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void scalar_swap(uint32_t *out, const uint32_t *src, uint32_t count,
                        int flush_nan) {
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t v = __builtin_bswap32(src[i]);
        out[i] = flush_nan && (v & 0x7FFFFFFFu) > 0x7F800000u ? 0u : v;
    }
}

static void neon_swap(uint32_t *out, const uint32_t *src, uint32_t count,
                      int flush_nan) {
    uint32_t i = 0;
    for (; i + 16 <= count; i += 16) {
        const uint8_t *s8 = (const uint8_t *)(src + i);
        uint8_t *d8 = (uint8_t *)(out + i);
        uint32x4_t v0 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(s8 + 0)));
        uint32x4_t v1 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(s8 + 16)));
        uint32x4_t v2 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(s8 + 32)));
        uint32x4_t v3 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(s8 + 48)));
        if (flush_nan) {
            const uint32x4_t abs_mask = vdupq_n_u32(0x7FFFFFFFu);
            const uint32x4_t inf_bits = vdupq_n_u32(0x7F800000u);
            v0 = vbicq_u32(v0, vcgtq_u32(vandq_u32(v0, abs_mask), inf_bits));
            v1 = vbicq_u32(v1, vcgtq_u32(vandq_u32(v1, abs_mask), inf_bits));
            v2 = vbicq_u32(v2, vcgtq_u32(vandq_u32(v2, abs_mask), inf_bits));
            v3 = vbicq_u32(v3, vcgtq_u32(vandq_u32(v3, abs_mask), inf_bits));
        }
        vst1q_u8(d8 + 0, vreinterpretq_u8_u32(v0));
        vst1q_u8(d8 + 16, vreinterpretq_u8_u32(v1));
        vst1q_u8(d8 + 32, vreinterpretq_u8_u32(v2));
        vst1q_u8(d8 + 48, vreinterpretq_u8_u32(v3));
    }
    for (; i < count; ++i) {
        const uint32_t v = __builtin_bswap32(src[i]);
        out[i] = flush_nan && (v & 0x7FFFFFFFu) > 0x7F800000u ? 0u : v;
    }
}

#define N 1031 /* prime, so the tail path is always exercised */

static uint32_t src[N], a[N], b[N];
static int failures;

static void run(const char *what, int flush_nan) {
    scalar_swap(a, src, N, flush_nan);
    neon_swap(b, src, N, flush_nan);
    for (uint32_t i = 0; i < N; ++i) {
        if (a[i] != b[i]) {
            printf("  FAIL  %-28s lane %u: scalar %08x neon %08x (src %08x)\n",
                   what, i, a[i], b[i], src[i]);
            ++failures;
            return;
        }
    }
    printf("  ok    %-28s %u dwords\n", what, N);
}

int main(void) {
    printf("NEON constant byte-swap equivalence\n\n");

    /* A cheap deterministic PRNG - the point is coverage, not randomness. */
    uint32_t x = 0x12345678u;
    for (uint32_t i = 0; i < N; ++i) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        src[i] = x;
    }
    run("pseudorandom bits", 0);
    run("pseudorandom bits, flush NaN", 1);

    /* The values that actually decide the NaN branch, in both byte orders.
       Stored big-endian because the kernel byte-swaps on the way in. */
    static const uint32_t edge[] = {
        0x00000000u, 0x80000000u,             /* +0, -0                     */
        0x7F800000u, 0xFF800000u,             /* +Inf, -Inf - must survive  */
        0x7F800001u, 0xFF800001u,             /* smallest NaN - must flush  */
        0x7FC00000u, 0xFFC00000u,             /* quiet NaN - must flush     */
        0x7F7FFFFFu, 0xFF7FFFFFu,             /* largest finite - survives  */
        0x00800000u, 0x007FFFFFu,             /* normal/denormal boundary   */
        0x3F800000u, 0xBF800000u,             /* +1, -1                     */
    };
    for (uint32_t i = 0; i < N; ++i)
        src[i] = __builtin_bswap32(edge[i % (sizeof(edge) / sizeof(edge[0]))]);
    run("float edge cases", 0);
    run("float edge cases, flush NaN", 1);

    /* Inf must NOT be flushed and NaN must be - assert the intent directly,
       not just that the two implementations agree with each other. */
    src[0] = __builtin_bswap32(0x7F800000u); /* +Inf */
    src[1] = __builtin_bswap32(0x7FC00000u); /* NaN  */
    neon_swap(b, src, N, 1);
    if (b[0] != 0x7F800000u) {
        printf("  FAIL  +Inf was flushed (%08x)\n", b[0]);
        ++failures;
    } else {
        printf("  ok    +Inf survives the NaN flush\n");
    }
    if (b[1] != 0u) {
        printf("  FAIL  NaN was not flushed (%08x)\n", b[1]);
        ++failures;
    } else {
        printf("  ok    NaN is flushed to +0\n");
    }

    printf("\n%s\n", failures == 0 ? "all checks passed" : "FAILURES PRESENT");
    return failures == 0 ? 0 : 1;
}
