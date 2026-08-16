#!/usr/bin/env bash
# Build + run the SIMD whitespace-count benchmark: the Lain SIMD kernel
# (wsbench.ln) vs a scalar C loop, under gcc and clang at -O2 -march=native.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
LAIN="$ROOT/lain"
OUT="${TMPDIR:-/tmp}/lain_wsbench.$$"
mkdir -p "$OUT"

# Build the compiler if needed, then lower the kernel to C.
[ -x "$LAIN" ] || gcc -std=c99 -O2 -o "$LAIN" "$ROOT/src/main.c" -I "$ROOT/src"
"$LAIN" "$HERE/wsbench.ln" -o "$OUT/wsbench_kernel.c"

for CC in gcc clang; do
    command -v "$CC" >/dev/null 2>&1 || { echo "skip $CC (not found)"; continue; }
    echo "===== $CC -O2 -march=native ====="
    "$CC" -O2 -march=native -std=c11 -o "$OUT/wsbench" \
        "$OUT/wsbench_kernel.c" "$HERE/driver.c"
    "$OUT/wsbench"
done
rm -rf "$OUT"
