#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "simdlex_kernel.c"
static uint32_t ref_count(const uint8_t *s, uint32_t n){
    uint32_t count=0,i=0;
    while(i<n){
        while(i<n&&(s[i]==' '||s[i]=='\t'||s[i]=='\n'||s[i]=='\r')) i++;
        if(i>=n) break;
        uint8_t c=s[i];
        int alpha=(c>='A'&&c<='Z')||(c>='a'&&c<='z')||c=='_';
        int digit=(c>='0'&&c<='9');
        if(alpha){ while(i<n&&(((s[i]>='A'&&s[i]<='Z')||(s[i]>='a'&&s[i]<='z')||s[i]=='_')||(s[i]>='0'&&s[i]<='9'))) i++; }
        else if(digit){ while(i<n&&(s[i]>='0'&&s[i]<='9')) i++; }
        else i++;
        count++;
    }
    return count;
}
static int fails=0;
static void test(const char*label,const char*code){
    Fixed_u8_4096 buf; memset(&buf,0,sizeof buf);
    uint32_t n=(uint32_t)strlen(code); memcpy(buf.data,code,n);
    uint32_t simd=bench_simd_lexer_simdlex_count_tokens(&buf,n);
    uint32_t ref=ref_count((const uint8_t*)code,n);
    int ok=simd==ref; if(!ok) fails++;
    printf("%-22s simd=%2u ref=%2u %s\n",label,simd,ref,ok?"OK":"MISMATCH");
}
int main(void){
    test("empty","");
    test("spaces only","        ");
    test("single ident","hello");
    test("expr","a + b * 42");
    test("function","func add(x i32, y i32) i32 { return x + y }");
    test("heavy whitespace","   foo         bar      123    ");
    test("newlines/tabs","a\n\tb\r\n  c");
    test("32+ ws then token","                                 x");
    test("long ident + more","abcdefghijklmnopqrstuvwxyz_0123456789 next 99");
    printf(fails?"\n%d MISMATCH\n":"\nALL OK\n",fails);
    return fails;
}
