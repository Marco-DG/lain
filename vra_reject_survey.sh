#!/usr/bin/env bash
# Reject-side of the 2.6 differential: the corpus's E085 (bounds) *_fail.ln programs
# are genuinely out of bounds. With --suppress the new VRA analyzes them; it MUST
# refuse (report a NOT-proven bounds obligation). A program where it proves ALL
# bounds is a potential unsoundness to inspect.
set -u
cd "/home/marco/Scrivania/MEGA/Progetti/Correnti/Lain/Lain Compiler/lain"
DRV="${VRADRV:-/tmp/vradrv}"
[ -x "$DRV" ] || { echo "build first: gcc -std=c99 -o $DRV src/analysis/vra_driver.c -I src"; exit 2; }
refused=0; proveall=0; noobl=0
: > /tmp/proveall.list
for f in $(find tests -name "*_fail.ln" | sort); do
    if grep -qE "E085|out of bounds|bounds error" "$f" 2>/dev/null || grep -qE "E085" "${f%.ln}.grep" 2>/dev/null; then
        out=$(bash -c '"$1" "$2" --suppress 2>/dev/null' _ "$DRV" "$f")
        p=$(printf '%s\n' "$out" | grep -c "index bounds .* PROVEN")
        n=$(printf '%s\n' "$out" | grep -c "index bounds .* NOT proven")
        if   [ "$n" -ge 1 ]; then refused=$((refused+1))
        elif [ "$p" -ge 1 ]; then proveall=$((proveall+1)); printf '%s (%s proven, 0 refused)\n' "${f#tests/}" "$p" >> /tmp/proveall.list
        else noobl=$((noobl+1)); fi
    fi
done
echo "reject-side over E085 bounds-fail tests:"
echo "  REFUSED (new engine caught it):        $refused"
echo "  PROVED-ALL (inspect for unsoundness):  $proveall"
echo "  NO bounds obligation (lowering gap):   $noobl"
echo "--- PROVED-ALL cases ---"
cat /tmp/proveall.list 2>/dev/null
