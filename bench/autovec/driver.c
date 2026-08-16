/* Tier-1 evidence: an ordinary Lain loop, proven alias-free, vs the C a
 * programmer actually writes (with and without the unsound hand-`restrict`).
 * Build with run.sh (-O3 -march=native, so gcc's cost model lets it vectorize). */
#define _POSIX_C_SOURCE 199309L
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#define N 16384
typedef struct { int32_t data[N]; } Fixed_i32_16384;
extern void vecadd_add_fixed(const Fixed_i32_16384*, const Fixed_i32_16384*, Fixed_i32_16384* restrict);
static void c_noalias (const int32_t*a,const int32_t*b,int32_t*out){for(int i=0;i<N;i++)out[i]=a[i]+b[i];}          /* honest C */
static void c_restrict(const int32_t*a,const int32_t*b,int32_t*restrict out){for(int i=0;i<N;i++)out[i]=a[i]+b[i];} /* UB if aliased */
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
int main(void){
    static Fixed_i32_16384 a,b,o; for(int i=0;i<N;i++){a.data[i]=i;b.data[i]=2*i;}
    int it=200000; double gb=(double)N*4*it/1e9; volatile int s=0;
    double t0=now();for(int k=0;k<it;k++){vecadd_add_fixed(&a,&b,&o);s+=o.data[k&(N-1)];}double t1=now();
    double t2=now();for(int k=0;k<it;k++){c_noalias(a.data,b.data,o.data);s+=o.data[k&(N-1)];}double t3=now();
    double t4=now();for(int k=0;k<it;k++){c_restrict(a.data,b.data,o.data);s+=o.data[k&(N-1)];}double t5=now();
    printf("Lain (PROVEN restrict)      %6.2f GB/s   [safe + clean codegen]\n", gb/(t1-t0));
    printf("C no-restrict (honest)      %6.2f GB/s   [gcc versions: alias check + fallback]\n", gb/(t3-t2));
    printf("C hand-restrict (UB-risk)   %6.2f GB/s   [fast, but UB if caller aliases]\n", gb/(t5-t4));
    (void)s; return 0;
}
