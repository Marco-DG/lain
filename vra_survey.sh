#!/usr/bin/env bash
# 2.6 differential (⊇ accepts direction): over every corpus program the OLD engine
# accepts that contains array/slice indexing, run the NEW octagon VRA and count how
# many BOUNDS obligations it discharges. The old engine already proved these safe,
# so this measures how close the new engine's precision is on real indexing patterns.
set -u
cd "/home/marco/Scrivania/MEGA/Progetti/Correnti/Lain/Lain Compiler/lain"
DRV="${VRADRV:-/tmp/vradrv}"
[ -x "$DRV" ] || { echo "build first: gcc -std=c99 -o $DRV src/analysis/vra_driver.c -I src"; exit 2; }
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
prog_ok=0 prog_partial=0 prog_skip=0
bounds_ok=0 bounds_tot=0
: > "$TMP/partial.list"
while IFS= read -r f; do
    case "$f" in *_fail.ln) continue;; esac
    grep -qE '\[[a-zA-Z_]|\.len|\.\.' "$f" || continue          # has indexing/ranges
    # run the new engine; the driver runs the old sema first and exits(1) if the old
    # engine rejects — those we skip (can't compare an accepted-program bound there)
    if ! bash -c '"$1" "$2" 2>/dev/null' _ "$DRV" "$f" > "$TMP/o" 2>/dev/null; then
        prog_skip=$((prog_skip+1)); continue
    fi
    p=$(grep -c "index bounds .* PROVEN" "$TMP/o")
    n=$(grep -c "index bounds .* NOT proven" "$TMP/o")
    tot=$((p+n))
    [ "$tot" -eq 0 ] && continue                                # no bounds site reached
    bounds_ok=$((bounds_ok+p)); bounds_tot=$((bounds_tot+tot))
    if [ "$n" -eq 0 ]; then prog_ok=$((prog_ok+1));
    else prog_partial=$((prog_partial+1)); echo "$(echo "$f"|sed 's#tests/##')  ($p/$tot)" >> "$TMP/partial.list"; fi
done < <(find tests -name '*.ln' -type f | sort)

echo "=================================================================="
echo "new octagon VRA over old-accepted indexing programs:"
echo "  BOUNDS obligations proven check-free: $bounds_ok / $bounds_tot"
echo "  programs fully proven: $prog_ok   partially: $prog_partial   (skipped/old-rejects: $prog_skip)"
echo "=================================================================="
echo "programs with an unproven bound (new-engine precision gaps to chase):"
sort "$TMP/partial.list" | head -30
