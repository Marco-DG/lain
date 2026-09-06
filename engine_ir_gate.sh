#!/usr/bin/env bash
# engine_ir_gate.sh — Stage 3.5 READINESS. Runs the real compiler with the sovereign IR
# analyses AUTHORITATIVE (`--engine=ir`) over the corpus and asks the only two questions that
# matter for switching over:
#
#   FALSE POSITIVES  — a `_pass` program the new engine rejects. Each is a regression a user
#                      would hit, and the number must reach 0 before the split can be default.
#   MISSED           — a `_fail` program it accepts. Not automatically a bug: many fail-tests
#                      are parse/type errors, which are sema's business and not these passes'.
#
# Unlike phase3_differential.sh (which compares VERDICTS between engines) this exercises the
# actual user-facing path: real diagnostics, real exit codes, real messages.
#
#   bash engine_ir_gate.sh [N]
set -u
ROOT="$(cd "$(dirname "$0")" && pwd)"; cd "$ROOT"
[ -x ./lain ] || { echo "build first: gcc -std=c99 -o lain src/main.c -I src"; exit 2; }
N="${1:-100000}"
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
MODE="${MODE:---engine=ir}"
fp=0; ok=0; caught=0; missed=0; i=0
: > "$TMP/fp.list"; : > "$TMP/why"
for f in $(find tests -name '*.ln' -type f | sort | head -"$N"); do
  case "$f" in */_tmp/*) continue;; esac
  i=$((i+1))
  ./lain $MODE "$f" -o "$TMP/o.c" >"$TMP/out" 2>&1; rc=$?
  [ $rc -ne 0 ] && grep -oE '^\[E[0-9]+\]' "$TMP/out" >> "$TMP/why"
  case "$f" in
    *_fail.ln) if [ $rc -ne 0 ]; then caught=$((caught+1)); else missed=$((missed+1)); fi ;;
    *)         if [ $rc -ne 0 ]; then fp=$((fp+1)); { echo "  $f"; grep -m2 '^\[E' "$TMP/out" | sed 's/^/      /'; } >> "$TMP/fp.list"
               else ok=$((ok+1)); fi ;;
  esac
done
echo "=================================================================="
echo "Stage 3.5 readiness ($MODE) — IR analyses AUTHORITATIVE in the real compiler"
echo "  pass programs accepted   : $ok"
echo "  pass programs REJECTED   : $fp      ← false positives; must be 0"
echo "  fail programs caught     : $caught"
echo "  fail programs accepted   : $missed  (many are parse/type errors — sema's business)"
echo "=================================================================="
if [ "$fp" -gt 0 ]; then
  echo "  ── which diagnostic rejected them ──"
  sort "$TMP/why" | uniq -c | sort -rn | sed 's/^/    /'
  echo "false positives (first 20):"; head -60 "$TMP/fp.list"
fi
[ "$fp" -eq 0 ]
