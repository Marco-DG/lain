#!/usr/bin/env bash
# Tier-1 auto-vec evidence. -O3 (or -fvect-cost-model=cheap) so gcc's cost model
# lets the runtime-trip loops vectorize; clang -O2 vectorizes without it.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"; ROOT="$(cd "$HERE/../.." && pwd)"; LAIN="$ROOT/lain"
OUT="${TMPDIR:-/tmp}/lain_autovec.$$"; mkdir -p "$OUT"
[ -x "$LAIN" ] || gcc -std=c99 -O2 -o "$LAIN" "$ROOT/src/main.c" -I "$ROOT/src"
"$LAIN" "$HERE/vecadd.ln" -o "$OUT/vecadd.c"
echo "== does the Lain slice loop vectorize? =="
gcc -O3 -march=native -c "$OUT/vecadd.c" -o /dev/null -fopt-info-vec 2>&1 | grep -i vectorized | head
echo "== throughput =="
gcc -O3 -march=native -std=gnu11 -o "$OUT/av" "$OUT/vecadd.c" "$HERE/driver.c"
"$OUT/av"
rm -rf "$OUT"
