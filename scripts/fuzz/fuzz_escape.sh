#!/usr/bin/env bash
# fuzz_escape.sh — teeth for the ESCAPE check: a reference to a local that outlives its frame.
#
# WHY (M9, 2026-10-01): borrow.h's bor_roots_local follows ONE provenance chain and answers
# "not local" for whatever it cannot follow. So it holds for a direct return and a plain binding,
# and loses the reference as soon as it passes through a CALL or a struct FIELD. Measured at
# 3b58598: nine launderings compile and dangle, six are correctly refused.
#
# Each reject program returns a reference into a local, then main calls clobber() to reuse the
# dead frame before reading through it. The clobber is load-bearing: without it a surviving
# reference reads stale-but-intact data and all three oracles stay quiet.
#
# ORACLES, and a false acceptance counts only when one of them CONFIRMS it:
#   ASan                 stack-use-after-return on the read
#   lain --interpret     "use of storage that is no longer live"
#   the differential     gcc -O2 output vs interpreter output
# Verified on r_identity at 3b58598: gcc -O2 prints 1579282520 for an expected 11, ASan says
# stack-use-after-return, the interpreter says the storage is no longer live.
#
# TWO-SIDED, and both sides are load-bearing. A fail-closed escape check can buy its soundness
# with over-refusal, and only the prove side would notice:
#   EXPECT: reject   acceptance is a DANGLING RETURN (confirmed, as above)
#   EXPECT: prove    rooted in a parameter, a string literal or a module constant, or a COPY.
#                    A refusal is a WRONG-REJECT, printed beside the bugs.
# MEASURED HISTORY, seeds 909 / 31337 / 4242, 150 programs each. Both numbers matter: a harness
# whose expected cost is written down is harder to misread later than one that happens to be clean.
#   0ea11ef  DANGLING 44 / 55 / 45    WRONG-REJECT 25 / 19 / 22
#   c8032d1  DANGLING  0 /  0 /  0    WRONG-REJECT 13 /  9 /  9   (all p_module_table)
#   0367b65  DANGLING  0 /  0 /  0    WRONG-REJECT  0 /  0 /  0
# The c8032d1 column reconciles exactly: +proved equalled the p_copy_array wrong-rejects fixed,
# +refused equalled the dangling closed, 150 = 150 on each side. So a non-zero WRONG-REJECT here
# is a real over-refusal, not noise, and this harness is expected to read 0/0 from 0367b65 on.
#
#   bash fuzz_escape.sh [N]                  # default 200
#   RANDOM_SEED=123 bash fuzz_escape.sh 60   # reproducible
#   LAIN=/path/to/lain bash fuzz_escape.sh   # another build
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$(dirname "$0")/../.."
# The guards below resolve a path from a diagnostic against the TREE ROOT, not against
# whatever cwd happens to be. Captured here so the guard does not depend on a cd dozens of
# lines above it, and asserted so a future edit cannot quietly move it.
ROOT="$PWD"
[ -d "$ROOT/tests" ] && [ -d "$ROOT/src" ] || { echo "not at the tree root: $ROOT"; exit 2; }
LAIN="${LAIN:-$(pwd)/lain}"; [ -x "$LAIN" ] || { echo "build first: make"; exit 2; }
GEN="$HERE/fuzz_escape.py"; [ -f "$GEN" ] || { echo "missing $GEN"; exit 2; }
# THE SEED IS FIXED BY DEFAULT. It used to default to $$, the PID, so two runs generated different
# programs and their numbers were not comparable. That cost a retracted measurement: a `proved` count
# read 92, 95 and 100 across three commits and was reported as precision rising, when at a fixed seed
# all four binaries are identical and unseeded runs of ONE binary span 89 to 102. Randomising is now
# an explicit opt-in (RANDOM_SEED=random), and every run prints the seed it used.
N="${1:-200}"; BASE=${RANDOM_SEED:-9702}
case "$BASE" in random|RANDOM) BASE=$$ ;; esac
SC="$(mktemp -d)"; trap 'rm -rf "$SC"' EXIT
DEFS="-Dlibc_printf=printf -Dlibc_puts=puts"
# ── is this output a HARNESS failure rather than a verdict? ───────────────────────────────────
# Three spellings, and for the module one an EXISTENCE test, because after P a legitimately
# missing module and a harness running from the wrong directory print the SAME message:
#   "there is no module 'std.math' (no file 'std/math.ln')"
# If the named file EXISTS under the tree root, the compiler could not see a file that is there,
# which is the harness's fault. If it does not exist, the program genuinely asked for nothing.
m13_unjudged() {
    case "$1" in
        *"Cannot open module file"*|*"lain: cannot open"*) return 0 ;;
    esac
    _f=$(printf '%s' "$1" | sed -n "s/.*(no file '\([^']*\)').*/\1/p" | head -1)
    [ -n "$_f" ] && [ -f "$ROOT/$_f" ] && return 0
    return 1
}


proved=0 refused=0 wrongreject=0 dangled=0 unconfirmed=0 genfail=0 unjudged=0 cfail=0
declare -A d_shape wr_shape
for ((k=0; k<N; k++)); do
    seed=$((BASE * 1000 + k)); f="$SC/e_$k.ln"
    python3 "$GEN" "$seed" > "$f" 2>/dev/null || { genfail=$((genfail+1)); continue; }
    expect=$(head -1 "$f" | grep -oE 'prove|reject')
    shape=$(sed -n '2p' "$f" | sed 's|// shape: ||')
    [ -n "$expect" ] && [ -n "$shape" ] || { genfail=$((genfail+1)); continue; }

    # A program the compiler could not LOAD is not a verdict. $f is an absolute path, so lain
    # chdirs to its directory; if a program ever gains an `import`, the module would not resolve
    # and the failure would otherwise be miscounted as a refusal. Count it and move on.
    _out=$(timeout 60 "$LAIN" "$f" -o "$SC/x.c" 2>&1); _rc=$?
    # Both spellings: pre-P "Cannot open module file", post-P "lain: cannot open".
    # A guard keyed on one message stops firing when the message is reworded, and the
    # unloadable program then reads as an ordinary refusal — the same harness bug in a
    # verdict's clothes.
    if m13_unjudged "$_out"; then
        unjudged=$((unjudged+1)); continue
    fi
    if [ $_rc -eq 0 ]; then accepted=1; else accepted=0; fi

    if [ "$expect" = "prove" ]; then
        if [ $accepted -eq 1 ]; then proved=$((proved+1))
        else
            wrongreject=$((wrongreject+1)); wr_shape[$shape]=$(( ${wr_shape[$shape]:-0} + 1 ))
            echo "── WRONG-REJECT  $shape seed=$seed :: $("$LAIN" "$f" -o /dev/null 2>&1 | head -1 | cut -c1-80)"
        fi
        continue
    fi

    [ $accepted -eq 0 ] && { refused=$((refused+1)); continue; }

    why=""
    if gcc -O1 -fsanitize=address -o "$SC/xa" "$SC/x.c" $DEFS -w 2>/dev/null; then
        out=$(cd "$SC" && ASAN_OPTIONS=detect_leaks=0 timeout 20 ./xa 2>&1)
        why=$(echo "$out" | grep -oE 'AddressSanitizer: [a-z-]+' | head -1)
    else cfail=$((cfail+1)); fi
    if [ -z "$why" ]; then
        LAIN_INTERP_STEPS=50000000 timeout 60 "$LAIN" "$f" --interpret >"$SC/i.out" 2>"$SC/i.err"; ist=$?
        m=$(grep -m1 'lain --interpret' "$SC/i.err")
        if echo "$m" | grep -q 'no longer live\|PROOF FAILED\|UNDEFINED BEHAVIOUR'; then why="interpreter: $(echo "$m" | cut -c1-72)"
        elif [ -s "$SC/x.c" ] && gcc -O2 -o "$SC/xp" "$SC/x.c" $DEFS -w 2>/dev/null; then
            (cd "$SC" && timeout 20 ./xp > c.out 2>/dev/null; echo $? > c.st)
            cmp -s "$SC/c.out" "$SC/i.out" || why="DIFFER: C '$(head -c 32 "$SC/c.out" | tr '\n' ' ')' vs interpreter '$(head -c 32 "$SC/i.out" | tr '\n' ' ')'"
        fi
    fi
    if [ -n "$why" ]; then
        dangled=$((dangled+1)); d_shape[$shape]=$(( ${d_shape[$shape]:-0} + 1 ))
        echo "── DANGLING  $shape seed=$seed :: $why"
    else
        unconfirmed=$((unconfirmed+1))
        echo "── unconfirmed  $shape seed=$seed :: accepted, nothing observed (suspect the GENERATOR)"
    fi
done

echo "=================================================================="
echo "fuzz_escape: $N programs   proved=$proved  correctly refused=$refused   seed=$BASE"
echo "  BUGS:  DANGLING RETURN=$dangled"
for s in "${!d_shape[@]}"; do echo "           $s: ${d_shape[$s]}"; done
echo "  COST:  WRONG-REJECT=$wrongreject"
for s in "${!wr_shape[@]}"; do echo "           $s: ${wr_shape[$s]}"; done
[ $unconfirmed -gt 0 ] && echo "  unconfirmed=$unconfirmed  (accepted an EXPECT:reject program, nothing observed)"
[ $cfail -gt 0 ]       && echo "  C link failed=$cfail"
[ $unjudged -gt 0 ]     && echo "  UNJUDGED=$unjudged   (a program the compiler could not LOAD — harness bug, these tested NOTHING)"
[ $genfail -gt 0 ]     && echo "  GENERATOR-FAIL=$genfail   (harness bug — these tested NOTHING)"
echo "=================================================================="
[ $dangled -eq 0 ] && [ $genfail -eq 0 ] && exit 0 || exit 1
