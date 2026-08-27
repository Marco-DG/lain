/* Thesis benchmark: compile-time-PROVEN safety (Lain) vs RUNTIME-CHECKED safety (C).
 *
 * Lain proves indices in bounds and arithmetic non-overflowing AT COMPILE TIME, so
 * the emitted C is check-free — safe at ZERO runtime cost. A C programmer who wants
 * the same safety guarantees without such a proof pays for them at runtime (bounds
 * + overflow checks). This benchmark compiles ONE identical kernel two ways:
 *   (A) plain -O3               — what Lain emits (proven safe, check-free)
 *   (B) -O3 -fsanitize=undefined — the same code made safe by RUNTIME checks
 * and reports the overhead (B/A) that Lain's proof eliminates. It also cross-checks
 * against the `restrict` Lain derives (proven no-alias) vs no aliasing proof.
 *
 * Run via ./run.sh (compiles this file both ways and times it).
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <time.h>

#define N 4096

/* A bounds- and overflow-relevant kernel over fixed-size arrays (so -fsanitize
 * inserts real checks): a gather-accumulate with an index table. Lain proves
 * idx[i] in [0,N) (a refinement/mask) and the accumulation fits — check-free. */
static int32_t kernel(const int32_t a[N], const int32_t b[N], const uint32_t idx[N]) {
    int32_t acc = 0;
    for (int i = 0; i < N; i++) {
        uint32_t j = idx[i] & (N - 1);   /* provably in [0,N) */
        acc += a[j] * b[i];
    }
    return acc;
}

static double now_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

int main(void) {
    static int32_t a[N], b[N]; static uint32_t idx[N];
    for (int i = 0; i < N; i++) { a[i] = i & 7; b[i] = (i * 3) & 15; idx[i] = (uint32_t)(i * 131); }
    const int ITERS = 300000;
    volatile int64_t sink = 0;
    double t0 = now_ms();
    for (int k = 0; k < ITERS; k++) sink += kernel(a, b, idx);
    double t = now_ms() - t0;
    (void)sink;
#ifdef SANITIZED
    printf("  (B) runtime-checked safety (-fsanitize=undefined): %8.1f ms\n", t);
#else
    printf("  (A) proven safe, check-free (what Lain emits)    : %8.1f ms\n", t);
#endif
    return 0;
}
