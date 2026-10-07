#!/usr/bin/env bash
# H7 (Documentation's substring study): a 32-lane @movemask on x86-64 WITHOUT AVX2 is two SSE2
# movemasks on the halves, not a 32-iteration byte loop. The three paths (AVX2, SSE2 halves, the
# portable loop) must agree bit for bit; the default x86-64 build (SSE2, no AVX2) must use pmovmskb.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
LAIN="$ROOT/lain"
W="$(mktemp -d)"; trap 'rm -rf "$W"' EXIT
case "$(uname -m)" in x86_64) ;; *) echo "movemask32 sse2: not x86-64, skipped"; exit 0 ;; esac
cat > "$W/mm.ln" <<'LN'
extern func libc_printf(fmt *u8, ...) i32 effects io

func mask_of(n usize < 1024, hay u8[n], c u8, at usize) u32 {
    if at > n { return 0 }
    if n - at < 32 { return 0 }
    return @movemask(@load(u8x32, hay, at) == c)
}

func main() i32 effects io {
    var buf u8[64]
    for k in 0..64 { buf[k] = (k % 7) as u8 }
    libc_printf("%u %u %u\n", mask_of(64, buf, 3, 0), mask_of(64, buf, 0, 5), mask_of(64, buf, 6, 32))
    return 0
}
LN
( cd "$W" && "$LAIN" mm.ln -o mm.c >"$W/err" 2>&1 ) || { echo "lain refused the program"; cat "$W/err"; exit 1; }
D="-Dlibc_printf=printf"
gcc -O2 -w $D -o "$W/sse2" "$W/mm.c"                         || { echo "the SSE2 build did not compile"; exit 1; }
gcc -O2 -w $D -DLAIN_PORTABLE_SIMD -o "$W/port" "$W/mm.c"      || { echo "the portable build did not compile"; exit 1; }
a=$("$W/sse2"); b=$("$W/port")
[ "$a" = "$b" ] || { echo "SSE2 halves ($a) and the portable loop ($b) disagree"; exit 1; }
if grep -q avx2 /proc/cpuinfo 2>/dev/null; then
  gcc -O2 -w $D -mavx2 -o "$W/avx2" "$W/mm.c" || { echo "the AVX2 build did not compile"; exit 1; }
  c=$("$W/avx2"); [ "$a" = "$c" ] || { echo "SSE2 halves ($a) and AVX2 ($c) disagree"; exit 1; }
fi
gcc -O2 -w $D -S -o "$W/sse2.s" "$W/mm.c"
grep -q pmovmskb "$W/sse2.s" || { echo "the default x86-64 build does not use pmovmskb"; exit 1; }
echo "movemask32 sse2: $a"
