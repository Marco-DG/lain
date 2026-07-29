#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
# Phase-0 boundary defense  (Grail Plan T0.4 / T0.5 / T0.6)
#
# Defends the emitted-C boundary that `run_tests.sh` cannot: its gate only
# *compiles* emitted C (`gcc -c -w`), so it can neither run it, exercise it
# under optimization, nor surface warnings. This harness does all three.
#
# For every runnable non-_fail test it:
#   1. emits C  (./lain file.ln -o out.c)
#   2. compiles at -O0 AND -O2, each under UBSan+ASan + the real hardening flags
#      (-fwrapv -fno-strict-aliasing), with warnings VISIBLE (no -w)
#   3. runs both binaries (bounded by a timeout, stdin = /dev/null)
#   4. reports four classes of finding:
#        COMPILE  — emitted C failed to compile at some -O level
#        SANITIZE — UBSan/ASan fired at runtime  (the UB the type system missed)
#        DIVERGE  — -O0 and -O2 disagree on output/exit  (optimizer-visible UB)
#        WARN     — emitted-C warning count (codegen-quality signal)
#
# This is a DIAGNOSTIC seed, not a pass/fail gate — it prints findings to triage.
# Exit code = number of COMPILE+SANITIZE+DIVERGE findings (0 = clean), so CI can
# gate on it later once the emitter is known-clean.
#
# Env: CC (default gcc), LAIN_RUN_TIMEOUT (default 5s), LAIN_DIFF_QUIET=1.
# ─────────────────────────────────────────────────────────────────────────────
set -u

ROOT="$(cd "$(dirname "$0")" && pwd)"
LAIN="$ROOT/lain"
CC="${CC:-gcc}"
TIMEOUT="${LAIN_RUN_TIMEOUT:-5}"
QUIET="${LAIN_DIFF_QUIET:-0}"

# Map Lain's libc_* shims onto the real libc (same convention as run_tests.sh),
# plus a few extras so more programs link. An unmapped libc_* → link error →
# reported as a COMPILE finding (which is itself useful signal).
DEFS=(
  -Dlibc_printf=printf -Dlibc_puts=puts -Dlibc_putchar=putchar
  -Dlibc_malloc=malloc -Dlibc_free=free -Dlibc_realloc=realloc
  -Dlibc_calloc=calloc -Dlibc_memcpy=memcpy -Dlibc_memset=memset
  -Dlibc_memmove=memmove -Dlibc_strlen=strlen
)
HARDEN=(-std=c99 -fwrapv -fno-strict-aliasing -fsanitize=undefined,address -g)
WARNFLAGS=(-Wall -Wextra -Wno-unused-function -Wno-unused-variable -Wno-unused-parameter)

# Sanitizers: print but don't abort-hard; leaks OFF (arena/no-free is by design,
# not a bug we are hunting here — we want UB and memory-corruption, not leaks).
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0:exitcode=99"
export UBSAN_OPTIONS="print_stacktrace=0:halt_on_error=0"

if [[ ! -x "$LAIN" ]]; then
  echo "Building compiler..."
  gcc -std=c99 -Wall -Wextra -o "$LAIN" "$ROOT/src/main.c" -I "$ROOT/src" 2>/dev/null
  [[ -x "$LAIN" ]] || { echo "Build failed."; exit 255; }
fi

tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT

declare -a CFAIL SANI DIVER WARNS
n_ok=0 n_skip=0

mapfile -t files < <(find "$ROOT/tests" -name '*.ln' ! -name '*_fail.ln' | sort)
echo "Scanning ${#files[@]} runnable tests (compile+run @ -O0 and -O2, UBSan+ASan)..."

for f in "${files[@]}"; do
  base="$(basename "$f" .ln)"
  c="$tmp/$base.c"

  # Lain-level emit; skip cleanly if the compiler rejects it (shouldn't for pass).
  if ! "$LAIN" "$f" -o "$c" >/dev/null 2>&1; then n_skip=$((n_skip+1)); continue; fi
  # Only run things with an entry point; header-only libs still get compiled below.
  grep -qE '\bmain\s*\(' "$c" || { n_skip=$((n_skip+1)); continue; }

  warnings=0 ok=1
  declare -A rout rexit
  for O in O0 O2; do
    bin="$tmp/$base.$O"
    cerr="$("$CC" "-$O" "${HARDEN[@]}" "${WARNFLAGS[@]}" "${DEFS[@]}" -o "$bin" "$c" 2>&1)"
    if [[ $? -ne 0 ]]; then
      CFAIL+=("$base [-$O]: $(echo "$cerr" | grep -m1 'error:' | sed 's/^[^:]*://')")
      ok=0; break
    fi
    warnings=$((warnings + $(echo "$cerr" | grep -c 'warning:')))
    out="$(timeout "$TIMEOUT" "$bin" </dev/null 2>"$tmp/err")"; rc=$?
    if grep -qE 'runtime error:|Sanitizer|ERROR: Address' "$tmp/err"; then
      SANI+=("$base [-$O]: $(grep -m1 -E 'runtime error:|Sanitizer|ERROR: Address' "$tmp/err" | sed 's/^[^:]*: //')")
      ok=0
    fi
    rout[$O]="$out"; rexit[$O]=$rc
  done

  [[ $warnings -gt 0 ]] && WARNS+=("$base: $warnings warning(s)")
  if [[ $ok -eq 1 ]]; then
    if [[ "${rout[O0]:-}" != "${rout[O2]:-}" || "${rexit[O0]:-}" != "${rexit[O2]:-}" ]]; then
      DIVER+=("$base: -O0(exit ${rexit[O0]:-?}) vs -O2(exit ${rexit[O2]:-?}) output/exit differ")
    else
      n_ok=$((n_ok+1))
    fi
  fi
  unset rout rexit
done

section() { # name array...
  local title="$1"; shift
  local -a arr=("$@")
  echo ""
  echo "── $title (${#arr[@]}) ──────────────────────────────"
  if [[ ${#arr[@]} -eq 0 ]]; then echo "  (none)"; else
    printf '  • %s\n' "${arr[@]}"
  fi
}

set +u   # the summary expands possibly-empty arrays; nounset is unhelpful here
echo ""
echo "=============================================================="
echo " Boundary harness: $n_ok clean · $n_skip skipped (no emit/main)"
echo "=============================================================="
section "COMPILE failures"      "${CFAIL[@]}"
section "SANITIZER errors"      "${SANI[@]}"
section "O0/O2 DIVERGENCES"     "${DIVER[@]}"
[[ "$QUIET" == "1" ]] || section "WARNINGS (codegen quality)" "${WARNS[@]}"

findings=$(( ${#CFAIL[@]} + ${#SANI[@]} + ${#DIVER[@]} ))
echo ""
echo "Total hard findings (compile+sanitize+diverge): $findings"
exit $findings
