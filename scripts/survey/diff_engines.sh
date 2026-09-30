#!/usr/bin/env bash
# diff_engines.sh — the clean-core rebuild's differential harness.
#
# Two guarantees, both against the corpus the old engine already vets:
#
#   1. BEHAVIOURAL GATE (hard).  Every self-checking program in tests/ir/*.ln is
#      written to `return 0` iff it computed the right answer. We run each through
#      the NEW pipeline end to end — frontend → lower to IR → emit C → gcc → run —
#      and require exit 0. The old engine already compiles these (run_tests.sh), so
#      exit 0 here means the new middle-end + backend agree with it behaviourally.
#
#   2. COVERAGE (reported).  Over every corpus program the old engine accepts
#      (i.e. not *_fail.ln), how many does the new pipeline lower to C that at
#      least COMPILES (to an object — link/extern resolution is out of scope). This
#      is the Phase-1 progress metric, not a pass/fail gate.
#
#   bash diff_engines.sh
set -u
cd "$(dirname "$0")/../.."
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
# ★ Built from THIS tree unless LOWERDRV names a driver you built. This survey used whatever sat at
# /tmp/lowerdrv until 2026-09-30: a binary from another checkout, or from before the last change,
# measured in silence.
DRV="${LOWERDRV:-$TMP/lowerdrv}"
[ -n "${LOWERDRV:-}" ] || gcc -std=c99 -w -o "$DRV" src/tools/lower_driver.c -I src || { echo "build lowerdrv failed"; exit 2; }

# ── 1. behavioural gate ──────────────────────────────────────────────────────
echo "── behavioural gate: tests/ir/*.ln through the NEW pipeline ──"
gate_fail=0 gate_n=0
for f in tests/ir/*.ln; do
    gate_n=$((gate_n+1)); name=$(basename "$f" .ln)
    if ! "$DRV" "$f" --emit-c > "$TMP/o.c" 2>"$TMP/e"; then echo "  FAIL $name (lower)"; gate_fail=$((gate_fail+1)); continue; fi
    if ! gcc -std=c99 -o "$TMP/o" "$TMP/o.c" -w 2>"$TMP/e"; then echo "  FAIL $name (gcc)"; gate_fail=$((gate_fail+1)); continue; fi
    "$TMP/o"; rc=$?
    if [ $rc -ne 0 ]; then echo "  FAIL $name (ran, exit $rc, expected 0)"; gate_fail=$((gate_fail+1)); fi
done
echo "  $((gate_n-gate_fail))/$gate_n self-checking programs run correctly"

# ── 2. coverage over the accepted corpus ─────────────────────────────────────
echo "── coverage: NEW pipeline over the accepted corpus (compile-to-object) ──"
ok=0 gccfail=0 crash=0 total=0
while IFS= read -r f; do
    case "$f" in *_fail.ln) continue;; esac
    total=$((total+1))
    # inner bash -c owns the wait, so its stderr swallows the shell's own
    # "segfault" report for the std-path crashes
    bash -c '"$1" "$2" --emit-c > "$3"' _ "$DRV" "$f" "$TMP/o.c" 2>/dev/null || { crash=$((crash+1)); continue; }
    gcc -std=c99 -c -o /dev/null "$TMP/o.c" -w 2>/dev/null || { gccfail=$((gccfail+1)); continue; }
    ok=$((ok+1))
done < <(find tests -name '*.ln' -type f | sort)
echo "  OK=$ok  GCC_FAIL=$gccfail  LOWER_CRASH=$crash  (of $total accepted; crashes incl. std-path resolution)"

[ $gate_fail -eq 0 ] || { echo "BEHAVIOURAL GATE FAILED"; exit 1; }
echo "behavioural gate OK"
