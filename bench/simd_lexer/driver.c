/* Correctness + throughput for the Lain SIMD lexer (simdlex.ln) vs a scalar
 * reference tokenizer. See run.sh. The throughput result is the important one:
 * it shows naive per-token SIMD whitespace-skip is SLOWER than scalar on real
 * code (short whitespace runs) and only wins on long runs. Measure, don't assume. */
#define _POSIX_C_SOURCE 199309L
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* The kernel is linked as a separate TU (robust to spaces in the repo path).
 * Fixed_u8_4096 is Lain's fixed-array wrapper — layout-identical here. */
typedef struct { uint8_t data[4096]; } Fixed_u8_4096;
extern uint32_t simdlex_count_tokens(const Fixed_u8_4096*, uint32_t);

static uint32_t ref_count(const uint8_t *s, uint32_t n){
    uint32_t count=0,i=0;
    while(i<n){ while(i<n&&(s[i]==' '||s[i]=='\t'||s[i]=='\n'||s[i]=='\r'))i++; if(i>=n)break;
        uint8_t c=s[i]; int a=(c>='A'&&c<='Z')||(c>='a'&&c<='z')||c=='_'; int d=(c>='0'&&c<='9');
        if(a){while(i<n&&(((s[i]>='A'&&s[i]<='Z')||(s[i]>='a'&&s[i]<='z')||s[i]=='_')||(s[i]>='0'&&s[i]<='9')))i++;}
        else if(d){while(i<n&&(s[i]>='0'&&s[i]<='9'))i++;} else i++; count++; }
    return count;
}
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (double)t.tv_sec+(double)t.tv_nsec*1e-9;}

static int fails=0;
static uint32_t fill(Fixed_u8_4096*buf,const char*unit){
    uint32_t n=0,ul=(uint32_t)strlen(unit); memset(buf,0,sizeof*buf);
    while(n+ul<4000){memcpy(buf->data+n,unit,ul);n+=ul;} return n;
}
static void check(const char*label,const char*code){
    Fixed_u8_4096 b; memset(&b,0,sizeof b); uint32_t n=(uint32_t)strlen(code); memcpy(b.data,code,n);
    uint32_t s=simdlex_count_tokens(&b,n), r=ref_count(b.data,n);
    if(s!=r)fails++; printf("  %-20s simd=%2u ref=%2u %s\n",label,s,r,s==r?"OK":"MISMATCH");
}
static void bench(const char*label,const char*unit){
    Fixed_u8_4096 b; uint32_t n=fill(&b,unit);
    uint32_t w=0; for(uint32_t j=0;j<n;j++) if(b.data[j]<=32) w++;
    int it=300000; volatile uint32_t sink=0;
    double t0=now();for(int k=0;k<it;k++)sink+=simdlex_count_tokens(&b,n);double t1=now();
    double t2=now();for(int k=0;k<it;k++)sink+=ref_count(b.data,n);double t3=now();
    double gb=(double)n*it/1e9;
    printf("  %-18s ws=%2.0f%%   SIMD %5.2f GB/s   scalar %5.2f GB/s   %4.2fx\n",
           label,100.0*w/n,gb/(t1-t0),gb/(t3-t2),(t3-t2)/(t1-t0)); (void)sink;
}
int main(void){
    printf("correctness (SIMD vs scalar reference):\n");
    check("empty","");                check("spaces only","        ");
    check("single ident","hello");    check("expr","a + b * 42");
    check("function","func add(x i32, y i32) i32 { return x + y }");
    check("heavy ws","   foo         bar      123    ");
    check("newlines/tabs","a\n\tb\r\n  c");
    check("32+ ws then token","                                 x");
    printf(fails?"  => %d MISMATCH\n":"  => ALL OK\n",fails);
    printf("\nthroughput (naive per-token SIMD skip is only worth it on LONG runs):\n");
    bench("indented code","        return foo(bar, baz) + qux * 2\n");
    bench("dense code","a+b*c-d/e=f(g,h,i,j)k;l+m*n\n");
    bench("heavy whitespace","word                                    \n");
    return fails;
}
