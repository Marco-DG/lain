/* Branchless-padded vs guarded wide SIMD scan — measurement.
 *
 * Question (per the doctrine's measurement contract): the constraint-chaining
 * proof lets a wide @load run BRANCHLESS over a padded buffer — one compare per
 * 16-byte block, no per-block in-guard, no scalar tail. Is that actually faster
 * than the guarded P2b form (in-guard + scalar tail)? Measure, don't assume.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

typedef struct { uint8_t data[4096]; } Fixed_u8_4096;
typedef struct { uint8_t data[4112]; } Fixed_u8_4112;

extern uint32_t bench_simd_padded_padded_count_guarded(const Fixed_u8_4096*, uint32_t);
extern uint32_t bench_simd_padded_padded_count_branchless(const Fixed_u8_4112*, uint32_t);

static double now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return t.tv_sec+t.tv_nsec*1e-9; }

int main(void){
    /* Realistic-ish content: ~16% spaces sprinkled through printable bytes. */
    Fixed_u8_4112 pb; memset(&pb,0,sizeof pb);            /* padding zeroed */
    Fixed_u8_4096 gb; memset(&gb,0,sizeof gb);
    uint32_t n = 4090;   /* NOT divisible by 16 → guarded runs a scalar tail */
    uint32_t expect = 0;
    for(uint32_t i=0;i<n;i++){
        uint8_t c = (i%6==0)?32:(uint8_t)(65+(i%26));
        pb.data[i]=c; gb.data[i]=c;
        if(c==32) expect++;
    }

    uint32_t g = bench_simd_padded_padded_count_guarded(&gb,n);
    uint32_t b = bench_simd_padded_padded_count_branchless(&pb,n);
    printf("correctness: expect=%u guarded=%u branchless=%u  %s\n",
           expect,g,b,(g==expect&&b==expect)?"OK":"MISMATCH");
    if(g!=expect||b!=expect) return 1;

    int iters = 1000000;
    volatile uint32_t sink=0;
    double best_g=1e9, best_b=1e9;
    for(int trial=0; trial<5; trial++){
        double t0=now();
        for(int k=0;k<iters;k++) sink+=bench_simd_padded_padded_count_guarded(&gb,n);
        double tg=now()-t0; if(tg<best_g) best_g=tg;
        t0=now();
        for(int k=0;k<iters;k++) sink+=bench_simd_padded_padded_count_branchless(&pb,n);
        double tb=now()-t0; if(tb<best_b) best_b=tb;
    }
    (void)sink;
    double bytes = (double)n*iters;
    double gbps_g = bytes/best_g/1e9, gbps_b = bytes/best_b/1e9;
    printf("guarded    : %.2f GB/s  (best of 5)\n", gbps_g);
    printf("branchless : %.2f GB/s  (best of 5, %.2fx vs guarded)\n", gbps_b, gbps_b/gbps_g);
    return 0;
}
