#!/usr/bin/env bash
# hint_gate.sh — every fact the compiler hands gcc must be true when the program runs.
#
# Where the analysis proved something, the emitted C tells gcc so: `if (!c) __builtin_unreachable()`
# for an established fact, signed arithmetic gcc may assume never overflows, `const` and `pure`
# on functions. A wrong proof is then not a missing check but undefined behaviour that the
# optimizer is entitled to propagate. The baseline gate runs every corpus program, but at gcc's
# default -O0, where none of these hints is used, and run_trust sanitizes only tests/trust.
#
# This runs every accepted corpus program (the baseline's set: tests/ and std/, no _fail) twice:
#   1. at -O2, where gcc uses the hints: stdout and exit code must equal the same program at -O0,
#      where they are not used (baseline_gate pins the -O0 run to tests/BASELINE.txt);
#   2. under -fsanitize=address,undefined (unreachable, signed overflow, shifts, bounds), with no
#      recovery: any sanitizer report fails the gate and names the program.
# A program that does not end in 10 s is reported apart (a proven-terminating one that hangs is a
# termination bug; one declared `effects diverge` may hang by design).
#
# TEETH, checked before it was trusted: built with 85eb5f1, the compiler before I.177, the
# skipped-trip program (tests/vra/accum_counter_skipped_trip_fail.ln run as a pass) fails BOTH
# checks: -O0 exits 62 and -O2 exits 0, and ASan reports the out-of-bounds load. First run on
# d27e0f9: 681 programs run, 0 failing, 17 library modules not run, 73 s at 4 jobs.
#
#   bash scripts/gates/hint_gate.sh        (LAIN_GATE_JOBS sets the width, default 4)
set -u
cd "$(cd "$(dirname "$0")/../.." && pwd)"
[ -x ./lain ] || { echo "build first"; exit 2; }
T=$(mktemp -d); trap 'rm -rf "${T:?}"' EXIT
export T
DEFS="-Dlibc_printf=printf -Dlibc_puts=puts -Dlibc_putchar=putchar -Dlibc_malloc=malloc -Dlibc_free=free -Dlibc_realloc=realloc -Dlibc_calloc=calloc"
export DEFS
one() {
  local i="$1" f="$2" d="$T/$1"; mkdir -p "$d"
  local flags; flags=$(grep -o 'LAINFLAGS:.*' "$f" | sed 's/LAINFLAGS: *//' | head -1)
  # A program the compiler refuses, or whose C does not link (a library module has no `main`), is
  # not run; it is COUNTED, so a gate that ran nothing cannot read as clean.
  if ! ./lain $flags "$f" -o "$d/o.c" >/dev/null 2>&1; then printf '%06d\t%s\tSKIP refused\n' "$i" "$f"; rm -rf "${d:?}"; return; fi
  if ! gcc -O0 -w -o "$d/b0" "$d/o.c" $DEFS 2>/dev/null; then printf '%06d\t%s\tSKIP no-link\n' "$i" "$f"; rm -rf "${d:?}"; return; fi
  local ob rb; ob=$(timeout 10 "$d/b0" 2>/dev/null </dev/null); rb=$?
  local res="ok"
  if [ $rb -eq 124 ]; then res="TIMEOUT"
  elif gcc -O2 -w -o "$d/b2" "$d/o.c" $DEFS 2>/dev/null; then
    local o2 r2; o2=$(timeout 10 "$d/b2" 2>/dev/null </dev/null); r2=$?
    [ "$o2" = "$ob" ] && [ $r2 -eq $rb ] || res="O2-DIFFERS (O0 rc=$rb, O2 rc=$r2)"
  fi
  # The sanitized run must report nothing AND end as the plain run did: a program that panics by
  # design aborts in both, while one that aborts only when instrumented has a reason to find.
  if [ "$res" != "TIMEOUT" ] && gcc -O1 -g -w -fsanitize=address,undefined -fno-sanitize-recover=all -o "$d/bs" "$d/o.c" $DEFS 2>/dev/null; then
    local rs; ASAN_OPTIONS=detect_leaks=0 timeout 20 "$d/bs" >/dev/null 2>"$d/san" </dev/null; rs=$?
    local why=""
    if grep -qE "runtime error|AddressSanitizer" "$d/san"; then
      why="SANITIZER: $(grep -m1 -oE '(runtime error|AddressSanitizer)[^/]*' "$d/san" | head -c 160)"
    elif [ $rs -ne $rb ]; then why="SANITIZED-RUN-DIFFERS (plain rc=$rb, sanitized rc=$rs)"; fi
    if [ -n "$why" ]; then if [ "$res" = "ok" ]; then res="$why"; else res="$res; $why"; fi; fi
  fi
  printf '%06d\t%s\t%s\n' "$i" "$f" "$res"
  rm -rf "${d:?}"
}
export -f one
find tests std -name '*.ln' -type f | sort | grep -vE '_fail\.ln$|/_tmp/' | awk '{printf "%d\t%s\n", NR, $0}' \
  | xargs -P "${LAIN_GATE_JOBS:-4}" -d '\n' -n 1 bash -c 'IFS=$(printf "\t") read -r i f <<< "$1"; one "$i" "$f"' _ \
  | sort -n > "$T/res.txt"
n=$(grep -cvP '\tSKIP ' "$T/res.txt"); bad=$(grep -cvP '\tok$|\tTIMEOUT$|\tSKIP ' "$T/res.txt"); to=$(grep -cP '\tTIMEOUT$' "$T/res.txt")
sr=$(grep -cP '\tSKIP refused$' "$T/res.txt"); sl=$(grep -cP '\tSKIP no-link$' "$T/res.txt")
grep -vP '\tok$|\tSKIP ' "$T/res.txt" | cut -f2- | sed 's/^/  /' | head -40
echo "=================================================================="
echo "hint gate: $n programs run at -O2 and under ASan+UBSan; $bad failing; $to did not end in 10 s"
echo "           not run: $sr refused by the compiler, $sl without a main (libraries)"
echo "=================================================================="
[ "$bad" -eq 0 ] && [ "$n" -gt 0 ]
