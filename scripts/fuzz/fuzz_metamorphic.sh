#!/usr/bin/env bash
# fuzz_metamorphic.sh — M14. Two meaning-preserving spellings of one program must get the SAME
# verdict, and if both compile, the SAME output.
#
# The oracle needs no semantics and no expected output, so this can use programs whose answer nobody
# has computed. It reaches a class M13 cannot: each spelling alone is a legal program with a coded
# verdict, and only the PAIR is a finding.
#
# WHAT IT REPORTS
#   DIVERGE-VERDICT-UNSAFE  the two spellings disagree AND the PROVING one misbehaves under a
#                           sanitizer or the interpreter. A soundness hole. Top priority.
#   DIVERGE-VERDICT         they disagree and the proving one runs clean. A precision asymmetry.
#   DIVERGE-OUTPUT          both compile, different stdout (or compiled vs interpreted). A miscompile.
#   DIVERGE-CODE            both refuse with different codes. A diagnostic asymmetry. Lowest.
#
# WHAT IS NOT A FINDING, and is counted separately so it can never be read as one
#   SKIP            the transformation did not apply (transformer exit 3)
#   XFORM-CRASH     the transformation raised (exit 4). A BUG IN THE TRANSFORMER, never a compiler
#                   finding. Reported beside the bug counts because a transformer that threw on
#                   every program would otherwise read as "applied 0", which looks inapplicable.
#   EXCLUDED-BASE   the base program does not compile, or its compiled and interpreted outputs
#                   already disagree. A pre-existing defect, so no PAIR from it can be attributed.
#   UNJUDGED        the compiler could not LOAD a file. A harness bug, never a verdict.
#
# A metamorphic fuzzer's findings are only as good as its transformations' correctness. Calibration
# (every transformation over the corpus, where verdicts must AGREE) found five bugs in the
# transformer before any of them reached a verdict. Re-run it when a transformation changes.
set -u

HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$(dirname "$0")/../.."                 # the TREE ROOT: see CELLDIR/UNJUDGED below

LAIN="${LAIN:-./lain}"
MM="$HERE/fuzz_metamorphic.py"
N="${1:-60}"                               # base programs to use
BASE_GLOB="${M14_BASES:-tests/**/*_pass.ln}"
# The default list is DERIVED from the transformer, never hardcoded here. It was hardcoded once, and
# a newly added transformation (`commute`) silently never ran: the table looked complete, every column
# read 0, and nothing said one transformation was missing. A list that must be edited in two places
# will be edited in one.
XFORMS="${M14_XFORMS:-$(python3 -c "import sys; sys.path.insert(0, '$HERE'); import fuzz_metamorphic as m; print(' '.join(sorted(m.TRANSFORMS)))")}"
[ -n "$XFORMS" ] || { echo "could not read the transformation list from $MM"; exit 2; }
for _x in $XFORMS; do
    python3 -c "import sys; sys.path.insert(0, '$HERE'); import fuzz_metamorphic as m; sys.exit(0 if '$_x' in m.TRANSFORMS else 1)" \
        || { echo "unknown transformation '$_x' (not in fuzz_metamorphic.py)"; exit 2; }
done
SEED_BASE="${RANDOM_SEED:-9100}"
# SEEDS per (program, transformation). One is not enough: a transformation picks ONE candidate site
# per seed, so with a single seed the driver missed a finding its own calibration found at 3 seeds.
# A harness weaker than the scratch script used to validate it is the wrong way round.
SEEDS="${M14_SEEDS:-3}"
BUDGET="${M14_BUDGET:-20}"
RUN_T="${M14_RUN_T:-10}"
STEPS="${LAIN_INTERP_STEPS:-20000000}"

# Programs are written HERE, under the tree root and reached by a RELATIVE path. An absolute path
# makes lain chdir away and `std/...` stops resolving; a DOTTED component made it read a different
# file entirely before Z (58e5cf0). Neither may creep back, so assert both.
TMP="local/m14run_$$"        # UNIQUE per run: a fixed path lets one run's exit trap delete
                             # another's. UNDERSCORE, not a dot: `m14run.$$` has a dotted
                             # component and this script's own guard rejected it, correctly.
case "$TMP" in
    /*) echo "TMP must be tree-relative, not absolute"; exit 2 ;;
    *.*|.*|*/.*) echo "TMP must have no dotted component"; exit 2 ;;
esac
mkdir -p "$TMP"; trap 'rm -rf "$TMP"' EXIT
SC="$(mktemp -d)"; trap 'rm -rf "$TMP" "$SC"' EXIT

[ -x "$LAIN" ] || { echo "no compiler at $LAIN (set LAIN=)"; exit 2; }
[ -f "$MM" ]   || { echo "no transformer at $MM"; exit 2; }

# A file the compiler could not LOAD is not a verdict. After P the message is the same whether the
# module is genuinely absent or the harness ran from the wrong place, so test whether the named file
# actually EXISTS. Resolved against the tree root, which the cd above guarantees is cwd.
unjudged() {
    case "$1" in
        *"Cannot open module file"*|*"lain: cannot open"*) return 0 ;;
    esac
    _f=$(printf '%s' "$1" | sed -n "s/.*(no file '\([^']*\)').*/\1/p" | head -1)
    [ -n "$_f" ] && [ -f "$_f" ] && return 0
    return 1
}

# verdict <file> -> sets V (accept|refuse|unjudged) and CODE
verdict() {
    _out=$(timeout "$BUDGET" "$LAIN" "$1" -o "$TMP/o.c" 2>&1); _rc=$?
    if unjudged "$_out"; then V=unjudged; CODE=""; return; fi
    if [ $_rc -eq 0 ] && [ -f "$TMP/o.c" ]; then V=accept; CODE=""; return; fi
    V=refuse
    CODE=$(printf '%s' "$_out" | grep -oE '\[E[0-9]+\]' | head -1)
    [ -n "$CODE" ] && : || CODE="uncoded"
}

# outputs <file> -> sets OUT and IOUT to a RUN SIGNATURE "<stdout>|rc=<n>".
# The exit code is part of the observable behaviour, not a failure indicator: a `_pass` program's
# main legitimately returns a computed value (simd_vec returns 11), and the first version read that
# as a failed run, turned it into "<RC11>" against the interpreter's "<HUNG>", and EXCLUDED the one
# base program with a known finding in it. Comparing the code as well is also strictly stronger: a
# transformation that changes the returned value is now a DIVERGE-OUTPUT instead of invisible.
# A timeout is 124 from `timeout` and is kept distinct from any value a program can return.
outputs() {
    rm -f "$TMP/o.c" "$TMP/b"
    timeout "$BUDGET" "$LAIN" "$1" -o "$TMP/o.c" >/dev/null 2>&1
    if ! gcc -O0 -o "$TMP/b" "$TMP/o.c" -Dlibc_printf=printf -Dlibc_puts=puts -w 2>/dev/null; then
        OUT="<C FAILED>"
    else
        _o=$(timeout "$RUN_T" "$TMP/b" 2>/dev/null); _rc=$?
        if [ "$_rc" -eq 124 ]; then OUT="<HUNG>"; else OUT="$_o|rc=$_rc"; fi
    fi
    _i=$(LAIN_INTERP_STEPS="$STEPS" timeout "$BUDGET" "$LAIN" "$1" --interpret 2>/dev/null); _rc=$?
    if [ "$_rc" -eq 124 ]; then IOUT="<HUNG>"; else IOUT="$_i|rc=$_rc"; fi
}

# sanitized <file> -> SAN=clean|dirty. Used ONLY on the PROVING side of a verdict disagreement, to
# tell a soundness hole from a precision gap. Without the split the count needs hand triage.
#
# It keys on sanitizer OUTPUT and on signal-level exits, never on a plain nonzero status. The first
# version used `|| SAN=dirty`, so a `_pass` program whose main returns 11 was reported
# DIVERGE-VERDICT-UNSAFE — a shipping corpus program declared unsound by the harness. A false UNSAFE
# is the worst error this instrument can make, because UNSAFE is the one verdict that stops a release.
sanitized() {
    SAN=clean
    rm -f "$TMP/o.c" "$TMP/s"
    timeout "$BUDGET" "$LAIN" "$1" -o "$TMP/o.c" >/dev/null 2>&1 || return
    if gcc -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -o "$TMP/s" "$TMP/o.c" \
           -Dlibc_printf=printf -Dlibc_puts=puts -w 2>/dev/null; then
        _so=$(timeout "$RUN_T" "$TMP/s" 2>&1 >/dev/null); _rc=$?
        case "$_so" in
            *"runtime error:"*|*"AddressSanitizer"*|*"UndefinedBehaviorSanitizer"*|*"ERROR: "*)
                SAN=dirty ;;
        esac
        [ "$_rc" -ge 128 ] && SAN=dirty        # killed by a signal, not a value main returned
    fi
    _i=$(LAIN_INTERP_STEPS="$STEPS" timeout "$BUDGET" "$LAIN" "$1" --interpret 2>&1 >/dev/null)
    _irc=$?
    case "$_i" in *"CHECK FAILED"*|*"PROOF VIOLATED"*) SAN=dirty ;; esac
    [ "$_irc" -ge 128 ] && SAN=dirty
}

# ── base programs, and the unmutated baseline ─────────────────────────────────────────────────
shopt -s globstar nullglob
ALL=($BASE_GLOB)
shopt -u globstar
BASES=(); n_excl=0; n_unj=0; n_nointerp=0
for p in "${ALL[@]}"; do
    [ ${#BASES[@]} -ge "$N" ] && break
    verdict "$p"
    if [ "$V" = unjudged ]; then n_unj=$((n_unj+1)); continue; fi
    if [ "$V" != accept ]; then n_excl=$((n_excl+1)); continue; fi
    outputs "$p"
    if [ "$OUT" = "<C FAILED>" ] || [ "$OUT" = "<HUNG>" ]; then n_excl=$((n_excl+1)); continue; fi
    BASES+=("$p"); eval "B_OUT_${#BASES[@]}=\$OUT"
    if [ "$OUT" = "$IOUT" ]; then eval "B_IOK_${#BASES[@]}=1"
    else eval "B_IOK_${#BASES[@]}=0"; n_nointerp=$((n_nointerp+1)); fi
done
echo "baseline: ${#BASES[@]} usable base programs (compile, run, and agree with the interpreter)"
echo "  EXCLUDED-BASE=$n_excl  UNJUDGED=$n_unj  (a pre-existing defect yields no attributable pair)"
echo "  of the usable ones, $n_nointerp are compared on the COMPILED leg only (the interpreter"
echo "  disagrees on the base already, so its leg says nothing about the pair)"
if [ "${#BASES[@]}" -eq 0 ]; then
    echo "REFUSING to proceed: no usable base program. This is a harness or build problem."
    exit 2
fi

# ── the pairs ─────────────────────────────────────────────────────────────────────────────────
printf '\n%-20s %8s %6s %7s %8s %8s %8s %8s\n' \
    "transform" "applied" "skip" "XCRASH" "V-UNSAFE" "VERDICT" "OUTPUT" "CODE"
t_unsafe=0; t_verdict=0; t_output=0; t_code=0; t_xcrash=0; t_applied=0
for x in $XFORMS; do
    ap=0; sk=0; xc=0; du=0; dv=0; do_=0; dc=0; k=0
    for i in "${!BASES[@]}"; do
        p="${BASES[$i]}"; k=$((k+1))
      for sj in $(seq 0 $((SEEDS - 1))); do
        seed=$((SEED_BASE + k * 16 + sj))
        m="$TMP/m.ln"
        python3 "$MM" "$p" "$x" "$seed" > "$m" 2>"$SC/err"; rc=$?
        case $rc in
            0) : ;;
            3) sk=$((sk+1)); continue ;;
            4) xc=$((xc+1)); t_xcrash=$((t_xcrash+1))
               echo "  ** XFORM-CRASH (a bug in fuzz_metamorphic.py, NOT a finding) x=$x seed=$seed src=$p"
               sed -n '$p' "$SC/err" | sed 's/^/       /'
               continue ;;
            *) echo "  ** HARNESS FAILURE: the transformer exited $rc (not 0, 3 or 4) for"
               echo "     x=$x seed=$seed src=$p. This is not a SKIP and not a finding: the harness"
               echo "     could not produce or write the variant, so nothing was judged. Refusing to"
               echo "     continue, because every later pair in this run is equally unattributable."
               sed -n '$p' "$SC/err" 2>/dev/null | sed 's/^/       /'
               exit 2 ;;
        esac
        # rc=0 must also mean the variant actually landed on disk: a redirection into a directory
        # that has been removed under us exits 0 from the shell's point of view in some shells.
        if [ ! -s "$m" ]; then
            echo "  ** HARNESS FAILURE: the transformer reported success but $m is missing or empty"
            echo "     (x=$x seed=$seed src=$p). Refusing to continue."
            exit 2
        fi
        ap=$((ap+1)); t_applied=$((t_applied+1))
        eval "b_out=\$B_OUT_$((i+1))"
        verdict "$m"; v1="$V"; c1="$CODE"
        if [ "$v1" = unjudged ]; then
            n_unj=$((n_unj+1))
            echo "  ** UNJUDGED (harness) x=$x seed=$seed src=$p"
            continue
        fi
        if [ "$v1" = refuse ]; then
            # the base ACCEPTED (every base does), so the pair disagrees; the base is the prover
            sanitized "$p"
            if [ "$SAN" = dirty ]; then
                du=$((du+1)); t_unsafe=$((t_unsafe+1))
                echo "  ** DIVERGE-VERDICT-UNSAFE x=$x seed=$seed src=$p :: base proves but misbehaves; variant $c1"
            else
                dv=$((dv+1)); t_verdict=$((t_verdict+1))
                echo "  ** DIVERGE-VERDICT x=$x seed=$seed src=$p :: base accepted, variant $c1"
            fi
            continue
        fi
        outputs "$m"
        eval "b_iok=\$B_IOK_$((i+1))"
        if [ "$OUT" != "$b_out" ] || { [ "$b_iok" = 1 ] && [ "$IOUT" != "$b_out" ]; }; then
            do_=$((do_+1)); t_output=$((t_output+1))
            echo "  ** DIVERGE-OUTPUT x=$x seed=$seed src=$p :: base='$b_out' variant='$OUT' interp='$IOUT'"
        fi
      done
    done
    printf '%-20s %8d %6d %7d %8d %8d %8d %8d\n' "$x" "$ap" "$sk" "$xc" "$du" "$dv" "$do_" "$dc"
    t_code=$((t_code+dc))
done

echo "=================================================================="
echo "fuzz_metamorphic: ${#BASES[@]} base programs, $t_applied pairs judged, seed base $SEED_BASE"
echo "  BUGS:   DIVERGE-VERDICT-UNSAFE=$t_unsafe  DIVERGE-OUTPUT=$t_output"
echo "  COST:   DIVERGE-VERDICT=$t_verdict   (a precision asymmetry: one spelling proves, one does not)"
echo "  DIAG:   DIVERGE-CODE=$t_code"
echo "  HARNESS: XFORM-CRASH=$t_xcrash  UNJUDGED=$n_unj  EXCLUDED-BASE=$n_excl"
if [ "$t_applied" -eq 0 ]; then
    echo "  ** every transformation SKIPPED: this run judged NOTHING. Not a clean result."
    exit 2
fi
echo "=================================================================="
# EXIT NONZERO ON THE BUGS ROW, as every sibling fuzzer does: fuzz_chain exits 1 on FALSE PROOF and
# GENERATOR-FAIL, fuzz_malformed on CRASH, UNCODED and NO-POSITION. Without this the instrument
# printed a soundness hole and `make fuzz` still passed, which makes the gate decorative for exactly
# the two categories that matter. COST and DIAG do NOT fail: a precision asymmetry is a cost, like
# chain's WRONG-REJECT, and one is currently known and open (I.56). XFORM-CRASH fails too, for the
# same reason chain fails on GENERATOR-FAIL — those pairs tested nothing.
[ "$t_unsafe"  -gt 0 ] && exit 1
[ "$t_output"  -gt 0 ] && exit 1
[ "$t_xcrash"  -gt 0 ] && exit 1
exit 0
