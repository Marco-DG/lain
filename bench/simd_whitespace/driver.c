/* Benchmark driver for the Lain SIMD whitespace counter (wsbench.ln).
 *
 * Fills a 256 MB buffer (~14% spaces), verifies the Lain SIMD kernel against a
 * plain scalar C loop, then times both. The scalar loop is the honest "what the
 * C compiler gives you for free" baseline — at -O2 -march=native both gcc and
 * clang auto-vectorize it, so this is SIMD-vs-auto-vectorized-scalar, not
 * SIMD-vs-naive. See run.sh. */
#define _POSIX_C_SOURCE 199309L
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

extern uint64_t wsbench_count_ws(const uint8_t *data, uintptr_t n); /* Lain SIMD kernel */

static uint64_t count_ws_scalar(const uint8_t *data, size_t n) {
    uint64_t t = 0;
    for (size_t i = 0; i < n; i++) if (data[i] == ' ') t++;
    return t;
}
static double now(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

int main(void) {
    size_t n = (size_t)256 * 1024 * 1024;            /* multiple of 32 */
    uint8_t *buf = malloc(n);
    if (!buf) return 1;
    for (size_t i = 0; i < n; i++)                   /* ~14% spaces, deterministic */
        buf[i] = (i % 7 == 0) ? ' ' : (uint8_t)('a' + (i & 15));

    uint64_t a = wsbench_count_ws(buf, n), b = count_ws_scalar(buf, n);
    printf("verify: simd=%llu scalar=%llu %s\n",
           (unsigned long long)a, (unsigned long long)b, a == b ? "MATCH" : "MISMATCH!");

    int iters = 8; volatile uint64_t sink = 0;
    double t0 = now(); for (int k = 0; k < iters; k++) sink += wsbench_count_ws(buf, n); double t1 = now();
    double t2 = now(); for (int k = 0; k < iters; k++) sink += count_ws_scalar(buf, n); double t3 = now();
    double gb = (double)n * iters / 1e9;
    printf("Lain SIMD : %6.2f GB/s\n", gb / (t1 - t0));
    printf("C scalar  : %6.2f GB/s\n", gb / (t3 - t2));
    printf("speedup   : %6.2fx\n",     (t3 - t2) / (t1 - t0));
    (void)sink; free(buf);
    return 0;
}
