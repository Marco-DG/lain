#!/usr/bin/env bash
# fuzz_chain.sh — teeth for a wrong FACT about a value, not UB at the operation.
#
# WHY (M7, 2026-10-01): the per-operation soundness harness found four false proofs that all 24
# whole-program fuzzers had passed. Three of the four sat in cells the generators DID produce.
# They survived because every generator checks for UB AT the operation, so a wrong fact about a
# result that nothing consumes is invisible. fuzz_div emits `return x % d` and only prints it.
#
# Every program here chains the result into an obligation that a wrong fact breaks (an index, a
# divisor, a narrowing cast, a loop bound, a slice end), puts that obligation AFTER the operation
# in the same function so an emptied state is observable, and is called with a WITNESS input that
# reaches the dangerous value.
#
# TWO-SIDED. Each program's first line declares its side, and both sides are load-bearing: a
# permissive bug hides on the `prove` side and a conservative one on the `reject` side.
#
#   EXPECT: prove    safe by construction. A rejection is a WRONG-REJECT — reported beside the
#                    bug count, never hidden, because a harness must report its cost.
#   EXPECT: reject    genuinely unsafe at the witness. ACCEPTANCE IS A FALSE PROOF, and it is
#                    only counted as one when the sanitizers or the interpreter CONFIRM it.
#
# ORACLES, all four, in this order:
#   gcc -fsanitize=undefined,address   a runtime error on an accepted program
#   lain --interpret                   PROOF FAILED / UNDEFINED BEHAVIOUR
#   C output vs interpreter output     a DIFFERENCE. This one is not optional: for an OVERFLOW
#                                      obligation UBSan is silent, because the emitted C computes
#                                      in a wider or unsigned type and wraps deterministically
#                                      rather than committing UB. Measured: a false proof on
#                                      `(x % y) + 32767` at i16 prints 32774 from the interpreter
#                                      (outside i16) and -32762 from the C. Without this oracle
#                                      both the SREM and the wrapping-cast classes read as clean.
#   exit status                        a crash the sanitizers did not name
#
#   bash fuzz_chain.sh [N]                   # default 200 programs
#   RANDOM_SEED=123 bash fuzz_chain.sh 50    # reproducible
#   LAIN=/path/to/lain bash fuzz_chain.sh    # test another build
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$(dirname "$0")/../.."
LAIN="${LAIN:-$(pwd)/lain}"; [ -x "$LAIN" ] || { echo "build first: make"; exit 2; }
GEN="$HERE/fuzz_chain.py"; [ -f "$GEN" ] || { echo "missing $GEN"; exit 2; }
N="${1:-200}"; BASE=${RANDOM_SEED:-$$}
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
    [ -n "$_f" ] && [ -f "$_f" ] && return 0
    return 1
}


proved=0 refused=0 wrongreject=0 falseproof=0 unconfirmed=0 genfail=0 unjudged=0 cfail=0
declare -A fp_shape
for ((k=0; k<N; k++)); do
    seed=$((BASE * 1000 + k)); f="$SC/p_$k.ln"
    python3 "$GEN" "$seed" > "$f" 2>/dev/null || { genfail=$((genfail+1)); continue; }
    expect=$(head -1 "$f" | grep -oE 'prove|reject')
    shape=$(sed -n '2p' "$f" | sed 's|// shape: ||')
    [ -n "$expect" ] || { genfail=$((genfail+1)); continue; }

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
            wrongreject=$((wrongreject+1))
            echo "── WRONG-REJECT  $shape seed=$seed :: $("$LAIN" "$f" -o /dev/null 2>&1 | head -1)"
        fi
        continue
    fi

    # EXPECT: reject
    if [ $accepted -eq 0 ]; then refused=$((refused+1)); continue; fi

    # Accepted an unsafe program. CONFIRM it before calling it a bug.
    why=""
    if gcc -O1 -fsanitize=undefined,address -o "$SC/x" "$SC/x.c" $DEFS -w 2>/dev/null; then
        out=$(cd "$SC" && ASAN_OPTIONS=detect_leaks=0 timeout 20 ./x 2>&1); st=$?
        why=$(echo "$out" | grep -oE 'runtime error: [^\n]*|ERROR: AddressSanitizer: [^\n]*' | head -1)
        [ -z "$why" ] && [ $st -ge 128 ] && why="crashed with signal $((st-128))"
    else
        cfail=$((cfail+1))
    fi
    # oracle 2: the interpreter's own verdict on the proofs it was handed
    if [ -z "$why" ]; then
        LAIN_INTERP_STEPS=50000000 timeout 60 "$LAIN" "$f" --interpret > "$SC/i.out" 2>"$SC/i.err"; istat=$?
        m=$(grep -m1 'lain --interpret' "$SC/i.err")
        echo "$m" | grep -q "PROOF FAILED\|UNDEFINED BEHAVIOUR" && why="interpreter: $m"
        # oracle 3: the two legs must agree. An overflow false proof shows up ONLY here.
        if [ -z "$why" ] && [ -s "$SC/x.c" ]; then
            if gcc -O2 -o "$SC/xp" "$SC/x.c" $DEFS -w 2>/dev/null; then
                (cd "$SC" && timeout 20 ./xp > c2.out 2>/dev/null; echo $? > c2.st)
                if [ "$istat" = "$(cat "$SC/c2.st")" ] && cmp -s "$SC/c2.out" "$SC/i.out"; then :
                else why="DIFFER: C printed '$(head -c 40 "$SC/c2.out")' exit $(cat "$SC/c2.st"), interpreter printed '$(head -c 40 "$SC/i.out")' exit $istat"; fi
            fi
        fi
    fi
    if [ -n "$why" ]; then
        falseproof=$((falseproof+1)); fp_shape[$shape]=$(( ${fp_shape[$shape]:-0} + 1 ))
        echo "── FALSE PROOF  $shape seed=$seed :: $why"
    else
        unconfirmed=$((unconfirmed+1))
        echo "── unconfirmed  $shape seed=$seed :: accepted, but nothing observed (generator may be wrong)"
    fi
done

echo "=================================================================="
echo "fuzz_chain: $N programs   proved=$proved  correctly refused=$refused"
echo "  BUGS:  FALSE PROOF=$falseproof"
for s in "${!fp_shape[@]}"; do echo "           $s: ${fp_shape[$s]}"; done
echo "  COST:  WRONG-REJECT=$wrongreject   (a safe program the analysis could not prove)"
[ $unconfirmed -gt 0 ] && echo "  unconfirmed=$unconfirmed  (accepted an EXPECT:reject program, nothing observed — suspect the GENERATOR)"
[ $cfail -gt 0 ]       && echo "  C link failed=$cfail"
[ $unjudged -gt 0 ]     && echo "  UNJUDGED=$unjudged   (a program the compiler could not LOAD — harness bug, these tested NOTHING)"
[ $genfail -gt 0 ]     && echo "  GENERATOR-FAIL=$genfail   (harness bug — these tested NOTHING)"
echo "=================================================================="
[ $falseproof -gt 0 ] && exit 1
[ $genfail -gt 0 ] && exit 1      # a program that failed to generate tested nothing
exit 0
