#!/usr/bin/env bash
# fuzz_termination.sh — teeth for the ONE guarantee that had no fuzzer.
#
# "Non-termination: Rejected in `func`" is a headline row in the README's table, and until now
# nothing generated programs to test it. Three FALSE PROOFS were found in it by hand on
# 2026-09-09 — a rising counter read as falling because the guard's predicate was not reversed
# when the counter sat on the right, a counter a callee resets, and B1's trip count built on the
# same recovery. None was reachable through the corpus, because the old front end refuses those
# shapes with [E082]/[E011] before the new pass ever runs.
#
# ORACLE, and it is exact: a loop the engine PROVES terminating must terminate. Run it under a
# timeout; a hang on a proven-terminating program is an UNSOUND result, not a slow test.
#
# The seam is what makes this possible at all: `--suppress` now also opens the legacy
# TERMINATION diagnostics, so the new engine's verdict is observable on exactly the programs
# that used to be invisible. Emission goes through the NEW backend for the same reason — the old
# one refuses to emit for a program it rejected.
#
# TEETH, verified before this was trusted: making `vra_progress_on_every_path` return true
# unconditionally (the state before the multi-path fix) takes not-proven to 0 and produces
# **8 UNSOUND in 40 programs** — the `one_arm` shape, `while i <= 1 { if flag > 0 { i = i+2 } }`,
# which is proven terminating and hangs on `run(0, 0)`. Restore that line to re-verify.
#
# That check also caught a bug in THIS script: `if ! timeout ...; then rc=$?` reads the status
# of the NEGATION, always 0, so the 124 the whole harness exists to see was invisible and it
# reported "all clean" over a program that provably hangs. Take the status directly.
#
# PE.12's GATE, a second engine on every E011 note. A note names an input on which the program never
# ends, and the interpreter ran it into a repeated state before it was printed. Here the NATIVE
# program is built with `main` making that call (scripts/fuzz/note_harness.py) and run: a loop's
# must time out, a recursion's must time out or exhaust the stack (SIGSEGV). Ending any other way,
# a trap included, is a FALSE NOTE. A note the harness cannot rebuild (a value it assumes of an
# extern, a function with no parameters, which the note names by no call) is counted, not checked.
#
#   bash fuzz_termination.sh [N]
set -u
cd "$(dirname "$0")/../.."
SC="${TMPDIR:-/tmp}/fuzz_term.$$"; mkdir -p "$SC"; trap 'rm -rf "$SC"' EXIT
N="${1:-200}"
SEED=${RANDOM_SEED:-$$}
DEFS="-Dlibc_printf=printf -Dlibc_puts=puts"

gcc -std=c99 -O2 -o "$SC/vradrv"  src/tools/vra_driver.c -I src 2>/dev/null || { echo "vradrv build failed"; exit 2; }
gcc -std=c99 -O2 -o "$SC/lowerdrv" src/tools/lower_driver.c    -I src 2>/dev/null || { echo "lowerdrv build failed"; exit 2; }

proven=0; notproven=0; unsound=0; skipped=0; brokenc=0; ran=0
. scripts/fuzz/judge_leg.sh
junsound=0
# The judge leg's oracle is the VRA leg's: a program lain accepted ends, under the same 5 s.
judge_leg() {   # PROG VRA ID
  jl_compile "$1" "$2" "$3" "$SC/j.c" || { note_leg "$1" "$3"; return 0; }
  gcc -std=c99 -w -o "$SC/j" "$SC/j.c" $DEFS 2>/dev/null || { jl_brokenc=$((jl_brokenc+1)); cp "$1" "$SC/jbrokenc.$3.ln"; return 0; }
  jl_ran=$((jl_ran+1))
  timeout 5 "$SC/j" >/dev/null 2>&1; local rc=$?
  if [ $rc -eq 124 ]; then
    junsound=$((junsound+1))
    echo "  ★ JUDGE UNSOUND: $3 — accepted by lain, HANGS"
    cp "$1" "$SC/junsound.$3.ln"; sed 's/^/      /' "$1"
  fi
  return 0
}
notes=0; nconfirmed=0; nunchecked=0; nfalse=0
# note_leg PROG ID: each note lain printed for PROG (in $SC/j.c.err), run natively
note_leg() {
  local line kind rc
  while IFS= read -r line; do
    notes=$((notes+1))
    kind=$(python3 scripts/fuzz/note_harness.py "$1" "$line" "$SC/h.ln")
    if [ "$kind" != loop ] && [ "$kind" != rec ]; then nunchecked=$((nunchecked+1)); continue; fi
    "$SC/lowerdrv" "$SC/h.ln" --emit-c --suppress-term > "$SC/h.c" 2>/dev/null \
      && gcc -std=c99 -w -o "$SC/h" "$SC/h.c" $DEFS 2>/dev/null || { nunchecked=$((nunchecked+1)); continue; }
    ( timeout 2 prlimit --core=0 -- "$SC/h" >/dev/null 2>&1 ) 2>/dev/null; rc=$?   # quiet: a recursion's SIGSEGV
    if [ $rc -eq 124 ] || { [ "$kind" = rec ] && [ $rc -eq 139 ]; }; then nconfirmed=$((nconfirmed+1)); continue; fi
    nfalse=$((nfalse+1))
    echo "  ★ FALSE NOTE: $2 — the native program ended (exit $rc) on the input the note names:"
    echo "      $line"; cp "$1" "$SC/falsenote.$2.ln"; sed 's/^/      /' "$1"
  done < <(grep -E 'this (loop never ends|recursion never returns)' "$SC/j.c.err" 2>/dev/null | sed 's/^ *//')
}
for ((k=0; k<N; k++)); do
    prog="$SC/t.ln"
    python3 scripts/fuzz/fuzz_termination.py $((SEED + k)) > "$prog" || { skipped=$((skipped+1)); continue; }

    # The new engine's verdict for `run`, with the legacy termination diagnostics stood down.
    # Suffix-matched: D-54 made function identity module-qualified, so the driver prints
    # `t_run`, and `^\s+run\s+` matched nothing from that day on — 200/200 skipped, reported
    # as "bugs: UNSOUND=0". See the same fix in fuzz_vra.sh.
    # ...and `spin`: a counter in a `var` struct PARAMETER lives in a callee (`run` builds the
    # struct and calls `spin(var c)`), so `run` has no loop of its own. Reading `run` alone
    # skipped every such program — 43 of 200 the day the family was added, reported beside
    # "UNSOUND=0". Every termination verdict of the two must be PROVEN for the program to be.
    v=$("$SC/vradrv" "$prog" --suppress 2>/dev/null | awk '$1 ~ /(^|_)(run|spin)$/ && $2=="termination"')
    vp=-; [ -n "$v" ] && { echo "$v" | grep -qv "PROVEN check-free" && vp=0 || vp=1; }
    judge_leg "$prog" "$vp" "k$k"                # every program, whatever the VRA leg concludes
    [ -z "$v" ] && { skipped=$((skipped+1)); continue; }
    if echo "$v" | grep -qv "PROVEN check-free"; then notproven=$((notproven+1)); continue; fi
    proven=$((proven+1))

    "$SC/lowerdrv" "$prog" --emit-c --suppress-term > "$SC/t.c" 2>/dev/null || { skipped=$((skipped+1)); continue; }
    gcc -std=c99 -w -o "$SC/t" "$SC/t.c" $DEFS 2>/dev/null || { brokenc=$((brokenc+1)); cp "$prog" "$SC/brokenc.$k.ln"; continue; }

    # A loop proven to terminate must terminate. 5s is many orders of magnitude over any
    # bound these programs can legitimately need (the largest generated limit is 255).
    ran=$((ran+1))
    # Take the status DIRECTLY. `if ! cmd; then rc=$?` reads the status of the NEGATION, which
    # is always 0, so the 124 this whole harness exists to see was invisible — it reported
    # "all clean" over a program that provably hangs. Verified by opening a hole in the
    # analysis on purpose and watching the count stay at zero.
    timeout 5 "$SC/t" >/dev/null 2>&1; rc=$?
    if [ $rc -eq 124 ]; then
        unsound=$((unsound+1))
        echo "  ★ UNSOUND: proven terminating, HANGS"
        cp "$prog" "$SC/unsound.$k.ln"; sed 's/^/      /' "$prog"
    fi
done

echo "=============================================================="
echo "fuzz_termination: gens=$N  proven=$proven  not-proven=$notproven  skipped=$skipped"
if [ "$skipped" -gt $(( N / 2 )) ]; then
  echo "  ★ FUZZER DID NOT RUN: $skipped/$N skipped — this report says nothing about the engine"
  exit 1
fi
# EXECUTED is the number the teeth depend on: a verdict nothing ran is a verdict
# nothing checked. A run where proven is high and ran is low has no oracle at all.
echo "  proven AND EXECUTED : $ran   <- the ones actually held to account"
echo "  bugs:  UNSOUND(hang on a proven loop)=$unsound  broken-C=$brokenc"
jl_report "UNSOUND(hang)=$junsound"
echo "  E011 notes (PE.12): $notes  confirmed natively=$nconfirmed  not rebuildable=$nunchecked"
echo "    bugs:  FALSE-NOTE(the native program ends on the noted input)=$nfalse"
echo "=============================================================="
[ $unsound -eq 0 ] && [ $brokenc -eq 0 ] && [ $junsound -eq 0 ] && [ $nfalse -eq 0 ] && jl_bugs && exit 0 || exit 1
