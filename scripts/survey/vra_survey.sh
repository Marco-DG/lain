#!/usr/bin/env bash
# 2.6 differential (⊇ accepts direction): over every corpus program the OLD engine
# accepts that contains array/slice indexing, run the NEW octagon VRA and count how
# many BOUNDS obligations it discharges. The old engine already proved these safe,
# so this measures how close the new engine's precision is on real indexing patterns.
set -u
cd "$(cd "$(dirname "$0")/../.." && pwd)"
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
# ★ Built from THIS tree unless VRADRV names a driver you built. This survey used whatever sat at
# /tmp/vradrv until 2026-09-30: a binary from another checkout, or from before the last change,
# measured in silence.
DRV="${VRADRV:-$TMP/vradrv}"
[ -n "${VRADRV:-}" ] || gcc -std=c99 -w -o "$DRV" src/tools/vra_driver.c -I src || { echo "build vradrv failed"; exit 2; }
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
