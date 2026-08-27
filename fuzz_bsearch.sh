#!/usr/bin/env bash
# fuzz_bsearch.sh — teeth for the inductive loop-invariant VRA (binary-search
# midpoint index proof). Generates binary-search-shaped loops with randomized
# array size, hi-start, and lo/hi update patterns. For every program the
# compiler ACCEPTS (no E085/E082), it PROMISED `a[mid]` is in bounds and the
# loop terminates — so we execute the emitted C under ASan against many
# adversarial target values and a bounded time budget. Any ASan overflow, or a
# hang, on an ACCEPTED program is an UNSOUND bug.
set -u
cd "$(dirname "$0")"
LAIN=./lain
SC="${TMPDIR:-/tmp}/fuzz_bsearch.$$"
mkdir -p "$SC"
N="${1:-300}"
seed="${RANDOM_SEED:-$$}"
RANDOM=$seed

crashes=0; unsound=0; accepted=0; rejected=0

sizes=(4 8 16 32 64)
for ((i=0; i<N; i++)); do
    sz=${sizes[$((RANDOM % ${#sizes[@]}))]}
    # hi-start: sometimes the true length (sound), sometimes off (must reject)
    case $((RANDOM % 4)) in
        0) histart=$sz;;
        1) histart=$((sz - 1));;
        2) histart=$((sz + RANDOM % 40));;   # too big → a[mid] OOB if accepted
        3) histart=$sz;;
    esac
    (( histart < 1 )) && histart=1
    # updates
    loU=("mid + 1" "mid")
    hiU=("mid" "mid + 1" "mid - 1")
    lo_upd=${loU[$((RANDOM % ${#loU[@]}))]}
    hi_upd=${hiU[$((RANDOM % ${#hiU[@]}))]}

    src="$SC/t_$i.ln"
    cat > "$src" <<EOF
proc bsearch(a i32[$sz], target i32) usize {
    var lo usize = 0
    var hi usize = $histart
    while lo < hi decreasing hi - lo {
        var mid usize = lo + (hi - lo) / 2
        if a[mid] < target { lo = $lo_upd } else { hi = $hi_upd }
    }
    return lo
}
proc main() i32 {
    var arr i32[$sz] = [$(seq -s ', ' 0 $((sz-1)))]
    var t i32 = 0
    var acc usize = 0
    while t < $((sz + 4)) decreasing $((sz + 4)) - t {
        acc = acc + bsearch(arr, t)
        t = t + 1
    }
    libc_printf("%zu\n", acc)
    return 0
}
EOF
    cfile="$SC/t_$i.c"
    if ! $LAIN "$src" -o "$cfile" >/dev/null 2>&1; then
        rejected=$((rejected+1)); continue
    fi
    accepted=$((accepted+1))
    bin="$SC/t_$i"
    if ! gcc -fsanitize=address,undefined -o "$bin" "$cfile" \
         -Dlibc_printf=printf -Dlibc_puts=puts -w >/dev/null 2>&1; then
        echo "BROKEN-C on accepted program: $src"
        crashes=$((crashes+1)); continue
    fi
    out=$(timeout 5 "$bin" 2>"$SC/err_$i")
    rc=$?
    if [ $rc -eq 124 ]; then
        echo "HANG (accepted but non-terminating): $src"
        unsound=$((unsound+1))
    elif grep -qiE "runtime error|AddressSanitizer|overflow|out of bounds" "$SC/err_$i"; then
        echo "UNSOUND (ASan/UBSan on accepted program): $src"
        sed -n '1,3p' "$SC/err_$i"
        unsound=$((unsound+1))
    fi
done

echo "fuzz_bsearch: gens=$N accepted=$accepted rejected=$rejected"
echo "  bugs:  broken-C=$crashes  UNSOUND=$unsound"
rm -rf "$SC"
[ $((crashes + unsound)) -eq 0 ]
