#!/usr/bin/env bash
# fuzz_waiver.sh — M15. A WAIVED operation inside `unsafe`, then safe code whose proof needs its value.
#
# `unsafe` waives narrowing and overflow; the C wraps; VRA may keep the UNWRAPPED value. A bound proved
# from that value is a false proof. No other generator puts a waived operation in front of safe code
# depending on it, which is why 29 fuzzers read 0 over the class.
#
# THREE ORACLES, because each alone has a blind spot:
#   1. `--interpret` PROOF FAILED  — primary: it names the falsely discharged proof.
#   2. ASan/UBSan                  — confirms the consequence is real. Validated by a control: a
#                                    waiver with NO dependent proof is sanitizer-CLEAN, so the
#                                    sanitizers do not flag `unsafe` as such.
#   3. -O0 versus -O2              — the invariant category reaches gcc as __builtin_unreachable; the
#                                    program then runs correctly at -O0 and SIGSEGVs at -O2. A driver
#                                    compiling only at -O0 reads 0 for the most severe category.
#
# THE LINE RULE. On the pre-fix compiler the interpreter MISLABELS a waived operation: a return inside
# the block is reported as a discharged proof, and a waived division by zero as PROOF FAILED, at a line
# INSIDE the block. So only a PROOF FAILED OUTSIDE the block is a finding. Inside-the-block ones are
# counted in their own column: a filtered case that is counted is data, a filtered case that is dropped
# is a lie.
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$(dirname "$0")/../.."
ROOT="$PWD"
[ -d "$ROOT/tests" ] && [ -d "$ROOT/src" ] || { echo "not at the tree root: $ROOT"; exit 2; }

LAIN="${LAIN:-./lain}"
GEN="$HERE/fuzz_waiver.py"
N="${1:-60}"
# Fixed by default, so two runs are comparable; `RANDOM_SEED=random` opts in and the seed is printed.
BASE=${RANDOM_SEED:-9400}
case "$BASE" in random|RANDOM) BASE=$$ ;; esac
BUDGET="${M15_BUDGET:-20}"
RUN_T="${M15_RUN_T:-10}"
STEPS="${LAIN_INTERP_STEPS:-20000000}"

TMP="local/m15run_$$"                 # unique per run, and no dotted component
case "$TMP" in /*) echo "TMP must be tree-relative"; exit 2 ;; *.*|.*|*/.*) echo "TMP must have no dot"; exit 2 ;; esac
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT
[ -x "$LAIN" ] || { echo "no compiler at $LAIN (set LAIN=)"; exit 2; }
[ -f "$GEN" ]  || { echo "no generator at $GEN"; exit 2; }

unjudged() {
    case "$1" in *"Cannot open module file"*|*"lain: cannot open"*) return 0 ;; esac
    _f=$(printf '%s' "$1" | sed -n "s/.*(no file '\([^']*\)').*/\1/p" | head -1)
    [ -n "$_f" ] && [ -f "$ROOT/$_f" ] && return 0
    return 1
}

# A harmless program must compile, so the run proves it reads the file it wrote.
printf 'func main() i32 {\n    return 0\n}\n' > "$TMP/sane.ln"
if ! timeout "$BUDGET" "$LAIN" "$TMP/sane.ln" -o "$TMP/sane.c" >/dev/null 2>&1; then
    echo "REFUSING to proceed: a trivial program does not compile. Harness or build problem."
    exit 2
fi
echo "fuzz_waiver: $N programs, seed base $BASE, compiler $LAIN"

tot_false=0; tot_mis=0; tot_ub=0; tot_miscomp=0; tot_nondet=0; tot_ref=0; tot_acc_clean=0; tot_unj=0
declare -A C_GEN C_FALSE C_REF C_MIS C_UB C_MC C_ND
for k in $(seq 1 "$N"); do
    seed=$((BASE + k))
    p="$TMP/w.ln"
    if ! python3 "$GEN" "$seed" > "$p" 2>"$TMP/err"; then
        echo "  ** HARNESS FAILURE: the generator exited non-zero at seed $seed. Not a finding."
        sed -n '$p' "$TMP/err" | sed 's/^/       /'
        exit 2
    fi
    [ -s "$p" ] || { echo "  ** HARNESS FAILURE: the generator wrote nothing at seed $seed"; exit 2; }
    cat=$(sed -n 's|^// CATEGORY: ||p' "$p"); src=$(sed -n 's|^// SOURCE: ||p' "$p")
    rng=$(sed -n 's|^// UNSAFE-LINES: ||p' "$p"); u0=${rng%-*}; u1=${rng#*-}
    case "$cat$src$u0$u1" in ''|*[!A-Za-z0-9_-]*) : ;; esac
    [ -n "$cat" ] && [ -n "$u0" ] && [ -n "$u1" ] || { echo "  ** HARNESS FAILURE: no header at seed $seed"; exit 2; }
    key="$cat/$src"; C_GEN[$key]=$(( ${C_GEN[$key]:-0} + 1 ))

    out=$(timeout "$BUDGET" "$LAIN" "$p" -o "$TMP/w.c" 2>&1); rc=$?
    if unjudged "$out"; then
        tot_unj=$((tot_unj+1)); echo "  ** UNJUDGED (harness) seed=$seed cat=$key"; continue
    fi
    if [ $rc -ne 0 ] || [ ! -f "$TMP/w.c" ]; then
        tot_ref=$((tot_ref+1)); C_REF[$key]=$(( ${C_REF[$key]:-0} + 1 )); continue
    fi

    # ORACLE 1: the interpreter. PROOF FAILED outside the block is the finding.
    iout=$(LAIN_INTERP_STEPS="$STEPS" timeout "$BUDGET" "$LAIN" "$p" --interpret 2>&1); irc=$?
    pf_line=$(printf '%s' "$iout" | sed -n 's|.*PROOF FAILED at [^:]*:\([0-9]\+\):.*|\1|p' | head -1)
    is_ub=$(printf '%s' "$iout" | grep -c 'UNDEFINED BEHAVIOUR IN UNSAFE CODE')

    # ORACLE 3: -O0 against -O2. The invariant category differs only here.
    o0="<C FAILED>"; o2="<C FAILED>"
    if gcc -O0 -o "$TMP/b0" "$TMP/w.c" -w 2>/dev/null; then
        s0=$(timeout "$RUN_T" "$TMP/b0" 2>/dev/null); r0=$?; o0="$s0|rc=$r0"
    fi
    # -O2 is run TWICE, because a program that reads out of bounds returns different garbage each
    # time: measured on one `return` program, six runs of ONE -O2 binary gave rc 160, 80, 240, 224,
    # 112 and 16 while -O0 stayed at 0. So an `-O0 != -O2` test is FLAKY — the garbage occasionally
    # equals -O0's answer and the finding vanishes, which is exactly what happened on 2 of 6 identical
    # programs in the first run of this instrument.
    #
    # Two runs of the SAME binary disagreeing is also a STRONGER signal than two optimisation levels
    # disagreeing: a deterministic program cannot do it, so it is a definite indeterminate read and
    # needs no appeal to what the optimiser is allowed to assume.
    o2b="<C FAILED>"
    if gcc -O2 -o "$TMP/b2" "$TMP/w.c" -w 2>/dev/null; then
        s2=$(timeout "$RUN_T" "$TMP/b2" 2>/dev/null); r2=$?; o2="$s2|rc=$r2"
        s2b=$(timeout "$RUN_T" "$TMP/b2" 2>/dev/null); r2b=$?; o2b="$s2b|rc=$r2b"
    fi
    # ORACLE 2: sanitizers.
    san=clean
    if gcc -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -o "$TMP/bs" "$TMP/w.c" -w 2>/dev/null; then
        se=$(timeout "$RUN_T" "$TMP/bs" 2>&1 >/dev/null); sr=$?
        case "$se" in *"runtime error:"*|*"AddressSanitizer"*|*"ERROR: "*) san=dirty ;; esac
        [ "$sr" -ge 128 ] && san=dirty
    fi

    # THE THREE ORACLES ARE INDEPENDENT. They were an if/elif chain, and the chain hid evidence twice:
    # the `return` category's first interpreter failure is the MISLABEL inside the block, which stopped
    # the chain before the real finding outside it; and the `invariant` category's PROOF FAILED stopped
    # it before the -O0/-O2 comparison, so the SIGSEGV that is that category's whole severity was never
    # reported. A classification must never suppress another oracle — the same shape as a catch-all
    # absorbing a failure into a benign column.
    _hit=0
    if [ "$is_ub" -gt 0 ]; then
        tot_ub=$((tot_ub+1)); C_UB[$key]=$(( ${C_UB[$key]:-0} + 1 )); _hit=1
    fi
    if [ -n "$pf_line" ] && [ "$pf_line" -ge "$u0" ] && [ "$pf_line" -le "$u1" ]; then
        # INSIDE the block: the pre-fix interpreter mislabelling a waived operation, not a finding.
        tot_mis=$((tot_mis+1)); C_MIS[$key]=$(( ${C_MIS[$key]:-0} + 1 )); _hit=1
    elif [ -n "$pf_line" ]; then
        tot_false=$((tot_false+1)); C_FALSE[$key]=$(( ${C_FALSE[$key]:-0} + 1 )); _hit=1
        echo "  ** FALSE PROOF seed=$seed cat=$key :: a proof at line $pf_line (outside the waived block,"
        echo "     lines $u0-$u1) was discharged from a value the machine does not hold; sanitizer=$san"
        printf '%s\n' "$iout" | grep -m1 'PROOF FAILED' | sed 's/^/       /'
    fi
    if [ "$o2" != "$o2b" ]; then
        tot_nondet=$((tot_nondet+1)); C_ND[$key]=$(( ${C_ND[$key]:-0} + 1 )); _hit=1
        echo "  ** NONDETERMINISTIC seed=$seed cat=$key :: two runs of ONE -O2 binary gave '$o2' and"
        echo "     '$o2b'. A deterministic program cannot; the accepted program reads indeterminate memory."
    fi
    if [ "$o0" != "$o2" ]; then
        tot_miscomp=$((tot_miscomp+1)); C_MC[$key]=$(( ${C_MC[$key]:-0} + 1 )); _hit=1
        echo "  ** O0-NE-O2 seed=$seed cat=$key :: -O0 gives '$o0' and -O2 gives '$o2'. For a program"
        echo "     whose UB the compiler proved absent this is the CONSEQUENCE, not a separate bug; for"
        echo "     the invariant category it is a violated __builtin_unreachable, i.e. gcc was lied to."
    fi
    if [ "$san" = dirty ] && [ "$_hit" -eq 0 ]; then
        # the sanitizer fired and no other oracle did: a consequence with no proof named
        tot_false=$((tot_false+1)); C_FALSE[$key]=$(( ${C_FALSE[$key]:-0} + 1 )); _hit=1
        echo "  ** FALSE PROOF (sanitizer only) seed=$seed cat=$key :: no oracle named a proof, but the"
        echo "     compiled program misbehaves under ASan/UBSan"
    fi
    [ "$_hit" -eq 0 ] && tot_acc_clean=$((tot_acc_clean+1))
done

printf '\n%-20s %4s %5s %7s %7s %7s %6s %5s\n' category gen refused "FALSE" NONDET 'O0!=O2' mislbl UB
for key in $(printf '%s\n' "${!C_GEN[@]}" | sort); do
    printf '%-20s %4d %5d %7d %7d %7d %6d %5d\n' "$key" "${C_GEN[$key]}" "${C_REF[$key]:-0}" \
        "${C_FALSE[$key]:-0}" "${C_ND[$key]:-0}" "${C_MC[$key]:-0}" "${C_MIS[$key]:-0}" "${C_UB[$key]:-0}"
done
echo "=================================================================="
echo "fuzz_waiver: $N programs, seed base $BASE"
echo "  BUGS:    FALSE PROOF=$tot_false   (a proof discharged outside the waived block)"
echo "  BUGS:    NONDETERMINISTIC=$tot_nondet   (two runs of ONE -O2 binary disagree)"
echo "  SAME BUG, SECOND ORACLE: -O0 != -O2 on $tot_miscomp programs. Not $tot_miscomp separate"
echo "           miscompiles: an accepted program that executes the UB its proof denied behaves"
echo "           differently per optimisation level. It is the column that catches the categories"
echo "           the interpreter mislabels, which is why it is reported separately."
echo "  EXPECTED ON A FIXED COMPILER: refused=$tot_ref  UB-reported=$tot_ub"
echo "  ORACLE:  mislabel-inside-block=$tot_mis  (the pre-fix interpreter, NOT findings)"
echo "  HARNESS: UNJUDGED=$tot_unj   accepted-and-clean=$tot_acc_clean"
echo "=================================================================="
[ "$tot_unj" -gt 0 ] && exit 2
[ "$tot_false" -gt 0 ] && exit 1
[ "$tot_nondet" -gt 0 ] && exit 1
[ "$tot_miscomp" -gt 0 ] && exit 1
exit 0
