#!/usr/bin/env bash
# fuzz_malformed.sh — no input may crash the compiler, and every refusal is coded and positioned.
#
# WHY (M13): `var a = [1, 2]` then `return a.zz` SEGFAULTED the compiler, as did every unknown
# member on an array or slice. All 26 fuzzers sat at 0 through it, because every generator emits
# programs meant to COMPILE or meant to fail ONE OBLIGATION — never ones with a name error. This
# one needs no semantics: whatever the input, the compiler must not crash, and a refusal must carry
# a code and a position.
#
# ONLY `_pass` PROGRAMS ARE MUTATED. A `_fail` program already refuses for its own reason, so a
# mutant's verdict there would be about that reason rather than the mutation, and WRONG-POSITION is
# only meaningful against a program that compiled.
#
# VERDICTS
#   CRASH          killed by a signal (rc >= 128) or an abort. Always a bug.
#   UNCODED        refused with no [E###] on stderr. Split into KNOWN (the five messages still
#                  uncoded at P-minus-1: destructured field absent, destructuring a non-struct,
#                  unknown @builtin, missing module, case arm with no body) and NEW. Only NEW is
#                  a finding; the split keeps this instrument from taking credit for commit P or
#                  being blamed for it.
#   NO-POSITION    refused, and the first error says "Ln 0".
#   WRONG-POSITION refused at a line that is not the mutated line. NOT automatically a bug — an
#                  error may legitimately surface at a use site — so it is a count to review.
#   HANG           exceeded the per-program budget.
#   ACCEPTED       the mutant still compiles. Not a bug in itself; counted, and run under
#                  --interpret so a mutant that compiles to something wrong is still seen.
#   UNJUDGED       the compiler could not LOAD the program. In the BASELINE run that is a harness
#                  bug. For a mutant it is a legitimate refusal (the mutation broke an import), so
#                  it counts as KNOWN-uncoded there, not as UNJUDGED.
#
# PATHS. Mutants are written under local/ (gitignored) and compiled by a RELATIVE path from the
# tree root, never absolute: lain chdirs to an absolute file's directory, and a program that
# imports a module then fails to resolve it. That cost another instrument 34 silently unloadable
# tests for days, in both legs of a differential, where "same" was the verdict nobody reads.
#
#   bash scripts/fuzz/fuzz_malformed.sh [N]        # N mutants per operator, default 40
#   RANDOM_SEED=123 bash scripts/fuzz/fuzz_malformed.sh 10
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$(dirname "$0")/../.."
# The guards below resolve a path from a diagnostic against the TREE ROOT, not against
# whatever cwd happens to be. Captured here so the guard does not depend on a cd dozens of
# lines above it, and asserted so a future edit cannot quietly move it.
ROOT="$PWD"
[ -d "$ROOT/tests" ] && [ -d "$ROOT/src" ] || { echo "not at the tree root: $ROOT"; exit 2; }
LAIN="${LAIN:-$(pwd)/lain}"; [ -x "$LAIN" ] || { echo "build first: make"; exit 2; }
MUT="$HERE/fuzz_malformed.py"; [ -f "$MUT" ] || { echo "missing $MUT"; exit 2; }
# THE SEED IS FIXED BY DEFAULT. It used to default to $$, the PID, so two runs generated different
# programs and their numbers were not comparable. That cost a retracted measurement: a `proved` count
# read 92, 95 and 100 across three commits and was reported as precision rising, when at a fixed seed
# all four binaries are identical and unseeded runs of ONE binary span 89 to 102. Randomising is now
# an explicit opt-in (RANDOM_SEED=random), and every run prints the seed it used.
N="${1:-40}"; BASE=${RANDOM_SEED:-5150}
case "$BASE" in random|RANDOM) BASE=$$ ;; esac
# Keep every component of this path free of a leading dot, and the path tree-relative.
# WHY, now history: before Z (58e5cf0) lain rebuilt the root file's path from the module name,
# turning '/' into '.' and every '.' back into '/', so `local/.m13/m.ln` was READ AS
# `local/m13/m.ln`. When that other file existed a DIFFERENT file was compiled, silently, exit 0.
# It cost this harness an entire invalid run: every mutant it wrote was ignored and one stale
# crashing file was recompiled 145 times, reported as 145 crashes across every operator.
# Z fixed the substitution (verified with a decoy sibling in place), but the LESSON is why the
# sanity line below still runs on every invocation: no guard could detect a silently substituted
# file, because the wrong file opened perfectly and nothing was absent to notice. A harness that
# cannot prove it read what it wrote cannot attribute anything it measures.
TMP="local/m13run"; mkdir -p "$TMP"; trap 'rm -rf "$TMP"' EXIT
BUDGET=20
OPS="${M13_OPS:-unknown-member unknown-member-seq undeclared-ident swap-type wrong-qualifier unknown-import drop-token dup-token call-to-member member-to-call}"
# The five messages still uncoded at P-minus-1. After P every refusal starts with [E###] or
# with "lain:" (driver errors), so this list goes empty and any UNCODED is new.
# After P, "there is no module 'x' (no file 'p')" is E106 and CODED, so it is a legitimate
# refusal — unless the named file EXISTS, which means the harness ran from the wrong directory.
m13_wrongcwd() {
    _f=$(printf '%s' "$1" | sed -n "s/.*(no file '\([^']*\)').*/\1/p" | head -1)
    [ -n "$_f" ] && [ -f "$ROOT/$_f" ] && return 0
    case "$1" in *"Cannot open module file"*|*"lain: cannot open"*) return 0 ;; esac
    return 1
}
KNOWN_UNCODED="not found in struct|Could not resolve struct type for destructuring|Unknown builtin|match patterns with no body|Cannot open module file"

mapfile -t PASSES < <(find tests -name '*_pass.ln' | sort)
[ ${#PASSES[@]} -gt 0 ] || { echo "no _pass programs found"; exit 2; }

# ── the BASELINE run: the unmutated programs must already be clean ───────────────────────────
echo "baseline: ${#PASSES[@]} unmutated _pass programs"
b_crash=0; b_uncoded=0; b_nopos=0; b_unjudged=0; b_ok=0; b_ref=0
for p in "${PASSES[@]}"; do
    out=$(timeout $BUDGET "$LAIN" "$p" -o "$TMP/b.c" 2>&1); rc=$?
    if [ $rc -ge 128 ]; then b_crash=$((b_crash+1)); echo "  ** BASELINE CRASH rc=$rc $p"; continue; fi
    if echo "$out" | grep -q 'Cannot open module'; then b_unjudged=$((b_unjudged+1)); continue; fi
    if [ $rc -eq 0 ]; then b_ok=$((b_ok+1)); continue; fi
    b_ref=$((b_ref+1))
    echo "$out" | grep -qE '\[E[0-9]+\]' || { b_uncoded=$((b_uncoded+1)); echo "  ** BASELINE UNCODED $p :: $(echo "$out" | head -1 | cut -c1-70)"; }
    echo "$out" | grep -qE 'Ln 0,' && { b_nopos=$((b_nopos+1)); echo "  ** BASELINE Ln 0 $p"; }
done
echo "  compiled=$b_ok refused=$b_ref  CRASH=$b_crash UNCODED=$b_uncoded NO-POSITION=$b_nopos UNJUDGED=$b_unjudged"
if [ $b_crash -gt 0 ] || [ $b_unjudged -gt 0 ]; then
    echo "REFUSING to judge mutants: the baseline is not clean, so a finding could be pre-existing."
    exit 2
fi
echo

# ── the mutants ──────────────────────────────────────────────────────────────────────────────
tot_crash=0; tot_new=0; tot_nopos=0; tot_wrongpos=0; unjudged=0
printf "%-18s %7s %7s %7s %7s %7s %7s %7s %7s\n" operator gen skip judged CRASH UNCODED-new NO-POS WRONG-POS accepted
for op in $OPS; do
    gen=0; skip=0; judged=0; crash=0; uncoded_new=0; uncoded_known=0; nopos=0; wrongpos=0; acc=0; hang=0
    # Walk the program list until N mutants EXIST, rather than spending the budget on programs
    # that have no site for this operator. A skip is still counted, so an operator that can
    # mutate almost nothing stays visible instead of silently testing less than the others.
    k=0; tries=0; maxtries=$(( N * 12 ))
    while [ $gen -lt $N ] && [ $tries -lt $maxtries ]; do
        src="${PASSES[$(( (BASE + tries * 31 + ${#op} * 7) % ${#PASSES[@]} ))]}"
        tries=$((tries+1))
        m="$TMP/m.ln"
        mseed=$((BASE + k))
        if ! python3 "$MUT" "$src" "$op" "$mseed" > "$m" 2>/dev/null; then skip=$((skip+1)); continue; fi
        gen=$((gen+1)); k=$((k+1))
        line=$(head -1 "$m" | grep -oE 'line=[0-9]+' | cut -d= -f2)
        out=$(timeout $BUDGET "$LAIN" "$m" -o "$TMP/m.c" 2>&1); rc=$?
        judged=$((judged+1))
        if [ $rc -ge 128 ] && [ $rc -ne 124 ]; then
            crash=$((crash+1)); tot_crash=$((tot_crash+1))
            echo "  ** CRASH rc=$rc  op=$op seed=$mseed src=$src"
            sed -n '2,$p' "$m" | head -14 | sed 's/^/        /'
            continue
        fi
        [ $rc -eq 124 ] && { hang=$((hang+1)); echo "  ** HANG op=$op seed=$mseed src=$src"; continue; }
        if m13_wrongcwd "$out"; then
            unjudged=$((unjudged+1))
            echo "  ** UNJUDGED op=$op seed=$mseed :: the compiler could not see a file that EXISTS — harness bug"
            continue
        fi
        if [ $rc -eq 0 ]; then acc=$((acc+1)); continue; fi
        if ! echo "$out" | grep -qE '\[E[0-9]+\]|^lain:'; then
            if echo "$out" | grep -qE "$KNOWN_UNCODED"; then uncoded_known=$((uncoded_known+1))
            else
                uncoded_new=$((uncoded_new+1)); tot_new=$((tot_new+1))
                echo "  ** UNCODED(new) op=$op seed=$mseed src=$src :: $(echo "$out" | head -1 | cut -c1-72)"
            fi
            continue
        fi
        if echo "$out" | grep -qE 'Ln 0,'; then
            nopos=$((nopos+1)); tot_nopos=$((tot_nopos+1))
            echo "  ** NO-POSITION op=$op seed=$mseed src=$src :: $(echo "$out" | head -1 | cut -c1-72)"
            continue
        fi
        rl=$(echo "$out" | grep -oE 'Ln [0-9]+' | head -1 | grep -oE '[0-9]+' | head -1)
        # The mutant carries a 1-line header, so a correct report is the mutated line + 1.
        # Both values are checked to be PLAIN INTEGERS first: anything else means the header or
        # the diagnostic was not in the shape assumed, and a malformed value in $(( )) is a
        # syntax error that kills the run mid-operator rather than skipping one mutant.
        case "$rl$line" in
            ''|*[!0-9]*) : ;;                       # not both plain integers: cannot judge position
            *) if [ "$rl" -ne $((line + 1)) ]; then
                   wrongpos=$((wrongpos+1)); tot_wrongpos=$((tot_wrongpos+1))
                   echo "  -- WRONG-POSITION op=$op seed=$mseed src=$src :: refused at Ln $rl, mutated line $((line + 1))"
               fi ;;
        esac
    done
    printf "%-18s %7d %7d %7d %7d %7d %7d %7d %7d\n" "$op" "$gen" "$skip" "$judged" "$crash" "$uncoded_new" "$nopos" "$wrongpos" "$acc"
done
echo "=================================================================="
echo "fuzz_malformed: $N mutants per operator over ${#PASSES[@]} _pass programs, seed base $BASE"
echo "  BUGS:  CRASH=$tot_crash  UNCODED(new)=$tot_new  NO-POSITION=$tot_nopos"
if [ $unjudged -gt 0 ]; then echo "  UNJUDGED=$unjudged   (harness bug — these tested NOTHING)"; fi
echo "  REVIEW: WRONG-POSITION=$tot_wrongpos  (a refusal away from the mutated line; not always wrong)"
echo "=================================================================="
[ $tot_crash -eq 0 ] && [ $tot_new -eq 0 ] && [ $tot_nopos -eq 0 ] && exit 0 || exit 1
