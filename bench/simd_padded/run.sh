#!/usr/bin/env bash
# Branchless-padded vs guarded wide SIMD scan — build + measure.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
LAIN="$ROOT/lain"

[[ -x "$LAIN" ]] || gcc -std=c99 -O2 -o "$LAIN" "$ROOT/src/main.c" -I "$ROOT/src"

# Lain → C (both scans proven check-free: 0 [E085] expected). Invoke with a
# RELATIVE path from the repo root: symbol mangling is path-derived, and the
# driver's extern names match the `bench/simd_padded/padded.ln` relative form.
cd "$ROOT"
"$LAIN" bench/simd_padded/padded.ln -o "$HERE/padded.c"

CC="${CC:-gcc}"
$CC -O3 -march=native -c "$HERE/padded.c" -o "$HERE/padded.o" \
    -Dlibc_printf=printf -Dlibc_puts=puts -w
$CC -O3 -march=native -c "$HERE/driver.c" -o "$HERE/driver.o"
$CC -O3 -march=native "$HERE/padded.o" "$HERE/driver.o" -o "$HERE/padtest"

"$HERE/padtest"
