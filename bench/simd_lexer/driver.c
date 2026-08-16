/* Correctness + throughput for the Lain SIMD lexers (simdlex.ln) vs scalar
 * references. Kernel linked as a separate TU (robust to spaces in the path).
 *
 * The story, in three tokenizers:
 *   naive  = per-token SIMD whitespace skip           (count_tokens)
 *   smart  = scalar core + SIMD only on long runs      (count_tokens_smart)
 *   scalar = plain scalar reference                     (this file)
 * Measurement (below) shows `smart` matches scalar on dense code AND wins on
 * string-heavy code — the corrected architecture the naive lexer's numbers
 * pointed us to. */
#define _POSIX_C_SOURCE 199309L
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

typedef struct { uint8_t data[4096]; } Fixed_u8_4096;
extern uint32_t simdlex_count_tokens(const Fixed_u8_4096*, uint32_t);        /* naive */
extern uint32_t simdlex_count_tokens_smart(const Fixed_u8_4096*, uint32_t);  /* corrected */

/* string-aware scalar reference — same token semantics as count_tokens_smart */
static uint32_t ref_smart(const uint8_t *s, uint32_t n){
    uint32_t count=0,i=0;
    while(i<n){
        while(i<n&&(s[i]==' '||s[i]=='\t'||s[i]=='\n'||s[i]=='\r'))i++; if(i>=n)break;
        uint8_t c=s[i];
        if(c=='"'){ i++; while(i<n&&s[i]!='"')i++; if(i<n)i++; }
        else if((c>='A'&&c<='Z')||(c>='a'&&c<='z')||c=='_'){ while(i<n&&(((s[i]>='A'&&s[i]<='Z')||(s[i]>='a'&&s[i]<='z')||s[i]=='_')||(s[i]>='0'&&s[i]<='9')))i++; }
        else if(c>='0'&&c<='9'){ while(i<n&&(s[i]>='0'&&s[i]<='9'))i++; }
        else i++;
        count++;
    }
    return count;
}
static double now(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (double)t.tv_sec+(double)t.tv_nsec*1e-9;}

static int fails=0;
static uint32_t fill(Fixed_u8_4096*b,const char*u){uint32_t n=0,ul=(uint32_t)strlen(u);memset(b,0,sizeof*b);while(n+ul<4000){memcpy(b->data+n,u,ul);n+=ul;}return n;}
static void check(const char*label,const char*code){
    Fixed_u8_4096 b; memset(&b,0,sizeof b); uint32_t n=(uint32_t)strlen(code); memcpy(b.data,code,n);
    uint32_t s=simdlex_count_tokens_smart(&b,n), r=ref_smart(b.data,n);
    if(s!=r)fails++; printf("  %-22s smart=%2u ref=%2u %s\n",label,s,r,s==r?"OK":"MISMATCH");
}
static double mbps(uint32_t(*f)(const Fixed_u8_4096*,uint32_t),const Fixed_u8_4096*b,uint32_t n,int it){
    volatile uint32_t sink=0; double t0=now(); for(int k=0;k<it;k++)sink+=f(b,n); double t1=now(); (void)sink;
    return (double)n*it/1e9/(t1-t0);
}
static void bench(const char*label,const char*unit){
    Fixed_u8_4096 b; uint32_t n=fill(&b,unit); int it=300000;
    volatile uint32_t sink=0; double t0=now(); for(int k=0;k<it;k++)sink+=ref_smart(b.data,n); double t1=now(); (void)sink;
    double sc=(double)n*it/1e9/(t1-t0);
    double naive=mbps(simdlex_count_tokens,&b,n,it);
    double smart=mbps(simdlex_count_tokens_smart,&b,n,it);
    printf("  %-16s scalar %5.2f | naive %5.2f (%.2fx) | smart %5.2f (%.2fx)  GB/s\n",
           label, sc, naive, naive/sc, smart, smart/sc);
}
int main(void){
    printf("correctness (corrected lexer vs string-aware scalar):\n");
    check("empty","");                 check("expr","a + b * 42");
    check("string","x = \"hello world\"");
    check("string + code","if (s == \"quit\") break");
    check("function","func add(x i32, y i32) i32 { return x + y }");
    printf(fails?"  => %d MISMATCH\n":"  => ALL OK\n",fails);
    printf("\nthroughput vs scalar (smart should match on dense, WIN on strings):\n");
    bench("dense code","a+b*c-d/e=f(g,h,i)k;\n");
    bench("normal code","    var result = compute(alpha, beta) + gamma\n");
    bench("string-heavy","msg = \"the quick brown fox jumps over the lazy dog\"\n");
    return fails;
}
