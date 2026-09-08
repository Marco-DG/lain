#!/usr/bin/env bash
# make_figures.sh — regenerate every figure in the README from source.
#
# Nothing here is drawn by hand: the blocks, the obligations and the octagon facts
# all come from the compiler's own dumps, rendered in LLVM's `opt -dot-cfg`
# conventions. A figure that stops being true stops being reproducible, so run
# this after any change that could move it.
#
#   bash assets/figures/make_figures.sh
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
cd "$ROOT"

[ -x ./lain ] || gcc -std=c99 -o lain src/main.c -I src
gcc -std=c99 -o "$WORK/vradrv" src/analysis/vra_driver.c -I src 2>/dev/null

# ── 1. Lain-IR CFG of `find`, carrying the octagon facts the analyser proved ──
cat > "$WORK/bytes.ln" <<'LN'
func find(haystack u8[], target u8) usize {
    var i usize = 0
    while i < haystack.len {
        if haystack[i] == target { return i }
        i = i + 1
    }
    return haystack.len
}
LN
"$WORK/vradrv" "$WORK/bytes.ln" --dump          > "$WORK/ir.txt"  2>/dev/null
"$WORK/vradrv" "$WORK/bytes.ln" --dump-octagon 2> "$WORK/oct.txt" >/dev/null
python3 "$HERE/mkcfg_ir.py" "$WORK/ir.txt" "$WORK/oct.txt" find "$WORK/ir.dot" --facts '%4,%5,%7'
dot -Tpng -Gdpi=110 "$WORK/ir.dot" -o "$WORK/ir.png"
convert "$WORK/ir.png" -bordercolor white -border 22 "$HERE/cfg_find_ir.png"

echo
echo "figure written to assets/figures/:"
printf "  %-22s %s\n" "cfg_find_ir.png" "$(identify -format '%wx%h' "$HERE/cfg_find_ir.png")"
