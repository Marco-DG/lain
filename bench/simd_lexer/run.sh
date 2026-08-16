#!/usr/bin/env bash
# Build + run the SIMD lexer correctness check: the Lain SIMD tokenizer
# (simdlex.ln) vs a scalar reference tokenizer, across varied inputs.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
LAIN="$ROOT/lain"
OUT="${TMPDIR:-/tmp}/lain_simdlex.$$"; mkdir -p "$OUT"
[ -x "$LAIN" ] || gcc -std=c99 -O2 -o "$LAIN" "$ROOT/src/main.c" -I "$ROOT/src"
"$LAIN" "$HERE/simdlex.ln" -o "$OUT/simdlex_kernel.c"
# gnu11: kernel uses GNU statement-exprs; kernel + driver as separate TUs, linked.
gcc -O2 -march=native -std=gnu11 -o "$OUT/simdlex" "$OUT/simdlex_kernel.c" "$HERE/driver.c"
"$OUT/simdlex"
rm -rf "$OUT"
