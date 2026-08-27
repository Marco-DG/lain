#!/usr/bin/env bash
# Thesis benchmark runner: compile-time-PROVEN safety (Lain) vs RUNTIME-CHECKED
# safety (the same kernel made safe by -fsanitize). The kernel (kernel.ln) compiles
# in Lain CHECK-FREE — its bounds (`idx & (N-1)`) and arithmetic are proven — so
# variant (A) is faithful to Lain's output.
set -u
cd "$(dirname "$0")"
CC="${CC:-gcc}"
FLAGS="-O3 -march=native"

echo "== Kernel proves in Lain (check-free)? =="
if ../../lain kernel.ln -o /dev/null >/dev/null 2>&1; then
    echo "  yes — a[idx[i] & (N-1)] and the accumulation are proven; no runtime checks emitted."
else
    echo "  NO — kernel.ln no longer compiles; benchmark not faithful. Fix before trusting numbers."
fi

echo
echo "== Compile-time-proven (Lain) vs runtime-checked (C) safety =="
$CC $FLAGS                                   -o bench_plain bench.c
$CC $FLAGS -DSANITIZED -fsanitize=undefined,bounds -o bench_san bench.c
oa=$(./bench_plain); ob=$(./bench_san)
echo "$oa"; echo "$ob"
A=$(echo "$oa" | grep -oE "[0-9]+\.[0-9]+"); B=$(echo "$ob" | grep -oE "[0-9]+\.[0-9]+")
awk -v a="$A" -v b="$B" 'BEGIN{printf "  --> runtime-checked safety costs %.2fx. Lain discharges those checks\n      at COMPILE TIME by proof: the same safety, at check-free speed.\n", b/a}'

rm -f bench_plain bench_san
