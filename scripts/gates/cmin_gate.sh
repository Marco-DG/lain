#!/usr/bin/env bash
# cmin_gate.sh — E0.6, the SOVEREIGNTY test, run rather than argued.
#
# `src/frontends/cmin` is a second, non-Lain front end: it parses a small C subset and builds
# Lain-IR directly, including only `ir/*` and `analysis/*`. If the IR encoded Lain-specific
# policy, that front end could not exist — so this gate is the empirical form of the
# semantic-independence litmus that has until now been re-run by hand.
#
# Two questions:
#   VERDICTS  — do the SAME analyses find the same things in C that they find in Lain?
#   BEHAVIOUR — does a C program compiled THROUGH Lain-IR do what gcc says it does?
#
#   bash cmin_gate.sh
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; cd "$ROOT"
CMIN="${CMIN:-/tmp/cmin}"; CC="${CC:-gcc}"
gcc -std=c99 -o "$CMIN" src/frontends/cmin/cmin.c -I src 2>/dev/null || { echo "build cmin failed"; exit 2; }
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
fail=0

# ── analyses: file  expected-code ("none" = must be clean) ────────────────────
check() {  # $1 file  $2 expected
  out=$("$CMIN" "tests/cmin/$1" 2>&1)
  if [ "$2" = none ]; then
    if echo "$out" | grep -q '^\[E'; then echo "  FAIL $1: expected clean, got:"; echo "$out" | sed 's/^/       /'; fail=$((fail+1));
    else echo "  ok   $1  (clean)"; fi
  else
    if echo "$out" | grep -q "^\[$2\]"; then echo "  ok   $1  ($2 caught)";
    else echo "  FAIL $1: expected $2, got:"; echo "$out" | sed 's/^/       /'; fail=$((fail+1)); fi
  fi
}
echo "verdicts — the same analyses, over C:"
check oob.c     E085     # unguarded index
check safe.c    none     # the same shape, guarded ⇒ proven check-free
check uninit.c  E005     # read of a local nothing wrote
check divzero.c E015     # divisor not provably non-zero
# ★ E015, not E087. The division diagnostic became E015 when the sovereign analyses were
# made authoritative (4d449b1); this expectation stayed at E087 — an array/slice code —
# and the gate has been RED ever since. Nobody saw it because `make gates` does not run
# this gate. A gate nobody runs is not a gate.

# ── behaviour: C → Lain-IR → C must match C → gcc ────────────────────────────
echo "behaviour — a C program compiled THROUGH Lain-IR:"
for f in tests/cmin/exec.c; do
  "$CMIN" "$f" --emit-c > "$TMP/a.c" 2>/dev/null || { echo "  FAIL $f: cmin could not emit"; fail=$((fail+1)); continue; }
  $CC -std=c99 -w -o "$TMP/a" "$TMP/a.c" 2>/dev/null || { echo "  FAIL $f: emitted C does not compile"; fail=$((fail+1)); continue; }
  $CC -std=c99 -w -o "$TMP/ref" "$f" 2>/dev/null
  "$TMP/a"   >/dev/null 2>&1; a=$?
  "$TMP/ref" >/dev/null 2>&1; r=$?
  if [ "$a" = "$r" ]; then echo "  ok   $f  (Lain-IR $a == gcc $r)";
  else echo "  FAIL $f: Lain-IR $a != gcc $r"; fail=$((fail+1)); fi
done


# ── THE LAYERING, CHECKED RATHER THAN CONVENTIONAL (2026-09-24) ──────────────────────────
# The tree was restructured so it STATES the architecture: `ir/` and `analysis/` are the core,
# `frontends/lain/` and `frontends/cmin/` are clients of it. That is only worth doing if
# something notices when it stops being true — a single `#include "frontends/lain/ast.h"` in
# an analysis would invert the dependency and nothing below would fail.
#
# cmin already proves the claim ONE way: it builds with no front-end header, so a core that
# needed Lain's AST would break it. This proves it the OTHER way, which cmin cannot: that the
# core does not reach INTO a front end.
#
# `src/frontends/lain/lower.h` is where that line was actually being crossed. It lives in
# `frontends/lain/` now, and used to be `src/ir/lower.h` — filed under the IR while
# referencing AST types, which said the IR knew about Lain. It does not; the Lain front end
# knows about the IR.
bad=0
for f in src/ir/*.h src/analysis/*.h; do
  if grep -qE '#include "(frontends/|\.\./frontends/)' "$f"; then
    echo "  ★ LAYERING VIOLATION: $f includes a FRONT END"; bad=$((bad+1))
  fi
  # the front-end headers by their bare names, in case a relative path sneaks one in
  if grep -qE '#include "(\.\./)*(ast|sema|parser|lexer|token|module|args)\.h"' "$f"; then
    echo "  ★ LAYERING VIOLATION: $f includes a front-end header"; bad=$((bad+1))
  fi
done
if [ "$bad" -ne 0 ]; then
  echo "=================================================================="
  echo "SOVEREIGNTY GATE FAILS — the core depends on a front end ($bad)"
  echo "=================================================================="
  exit 1
fi
echo "layering: ir/ and analysis/ include no front end ($(ls src/ir/*.h src/analysis/*.h | wc -l) files checked)"

echo "=================================================================="
[ $fail -eq 0 ] && echo "SOVEREIGNTY GATE HOLDS — a non-Lain front end gets the same guarantees" \
                || echo "SOVEREIGNTY GATE: $fail failure(s)"
echo "=================================================================="
[ $fail -eq 0 ]
