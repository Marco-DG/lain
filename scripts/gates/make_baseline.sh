#!/usr/bin/env bash
# make_baseline.sh — RECORD what the compiler produces, so a differential can outlive the
# implementation it was differencing against.
#
# ★ WHY. `backend_corpus` and `layout_gate` each compared the IR backend against the legacy
# one. Both found real defects that nothing else could (D-62: 29 sums silently lost their
# niche packing while ten gates stayed green; D-63: a slice niche emitting C that does not
# compile). Deleting src/emit/ would delete the leg they compare against, and with it the only
# instrument that has ever caught a REPRESENTATION change.
#
# So the reference is recorded instead of retired. This writes, per corpus program:
#
#   rc=    the exit code
#   out=   every line it prints
#   ty=    every type it declares, and whether that type is a STRUCT or a flat SCALAR
#
# The last line is the one that matters most and the one no other gate asks: a `T | markers`
# that stops being niche-packed still prints exactly the same thing.
#
# ★ AND IT IS A STRONGER INSTRUMENT THAN THE DIFFERENTIAL IT REPLACES, not a weaker one. A
# differential says "the two agree", which is silent when both are wrong. A baseline says
# "this is the answer", and it was generated from the backend that had been shipping for
# years. Regenerate deliberately (and read the diff) when an intended change moves it.
#
#   bash scripts/gates/make_baseline.sh [outfile] [--backend=...]
#
# ⚠ The OUTFILE is a parameter because `baseline_gate.sh` runs this same recorder into a temp
# file and diffs. A gate that regenerates its own reference in place would pass unconditionally
# — it would be comparing the compiler against itself, which is the defect D-60 already taught
# this project once (`backend_corpus` comparing the IR backend to itself and reporting
# "423 agree"). One recorder, two callers, and the gate never writes the committed copy.
set -u
cd "$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${1:-tests/BASELINE.txt}"
BE="${2:-}"
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
[ -x ./lain ] || { echo "build first"; exit 2; }

: > "$OUT"
n=0
for f in $(find tests std -name '*.ln' -type f | sort); do
  case "$f" in *_fail.ln|*/_tmp/*) continue;; esac
  flags=$(grep -o 'LAINFLAGS:.*' "$f" | sed 's/LAINFLAGS: *//' | head -1)
  ./lain $flags $BE "$f" -o "$TMP/o.c" >/dev/null 2>&1 || continue
  gcc -o "$TMP/o" "$TMP/o.c" -Dlibc_printf=printf -Dlibc_puts=puts -w 2>/dev/null || continue
  out=$("$TMP/o" 2>/dev/null); rc=$?
  {
    echo "### $f"
    echo "rc=$rc"
    printf '%s\n' "$out" | sed 's/^/out=/'
    # Every type the program declares, classified by how it is REPRESENTED.
    for nm in $(grep -ohE '^(typedef .*|\}) *[A-Za-z_][A-Za-z0-9_]*;' "$TMP/o.c" \
                | sed -E 's/.*[^A-Za-z0-9_]([A-Za-z_][A-Za-z0-9_]*);/\1/' | sort -u); do
      if grep -qE "struct $nm[[:space:]]*\{" "$TMP/o.c"; then echo "ty=$nm struct"
      else echo "ty=$nm scalar"; fi
    done
  } >> "$OUT"
  n=$((n+1))
done
echo "baseline: $n programs -> $OUT ($(wc -l < "$OUT") lines)${BE:+, recorded from $BE}"
