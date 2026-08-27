#!/usr/bin/env bash
# fuzz_borrow.sh — teeth for the borrow/region (dangling-reference) checker.
#
# A returned SLICE is the ASan-observable borrow surface: indexing a slice is SAFE
# syntax, so a dangling slice the checker wrongly accepted derefs freed stack memory
# at runtime — caught under ASan with detect_stack_use_after_return=1. The generator
# (fuzz_borrow.py) emits two streams:
#   accept — the returned/held slice borrows memory that OUTLIVES the scan (a param,
#            or the caller's own local): must compile AND scan clean under ASan.
#   reject — the returned slice/ref borrows a callee LOCAL (dangles once the frame
#            pops): must NOT compile (E010). If it compiles it is scanned anyway;
#            a stack-use-after-return trap => unsound acceptance.
set -u
cd "$(dirname "$0")"
LAIN=./lain
GEN=fuzz_borrow.py
SC="${TMPDIR:-/tmp}/fuzz_borrow.$$"; mkdir -p "$SC"
N="${1:-300}"
BASE=${RANDOM_SEED:-$$}
export ASAN_OPTIONS=detect_stack_use_after_return=1
acc_ok=0 acc_rej=0 rej_ok=0 rej_compiled=0 unsound=0 brokenc=0

for ((k=0; k<N; k++)); do
    seed=$((BASE * 100000 + k))
    python3 "$GEN" "$seed" > "$SC/t.ln"
    exp=$(head -1 "$SC/t.ln" | grep -oE 'accept|reject')
    if "$LAIN" "$SC/t.ln" -o "$SC/t.c" >/dev/null 2>&1; then compiled=1; else compiled=0; fi

    if [ "$exp" = accept ]; then
        if [ $compiled -eq 0 ]; then
            echo "OVER-REJECTION (valid borrow refused) seed=$seed:"; "$LAIN" "$SC/t.ln" -o /dev/null 2>&1 | grep -oE '\[E[0-9]+\].*' | head -1
            acc_rej=$((acc_rej+1)); continue
        fi
        acc_ok=$((acc_ok+1))
    else
        if [ $compiled -eq 0 ]; then rej_ok=$((rej_ok+1)); continue; fi
        echo "MISSED DANGLE (reject-stream program compiled) seed=$seed"; rej_compiled=$((rej_compiled+1))
    fi

    if ! gcc -fsanitize=address,undefined -o "$SC/t" "$SC/t.c" -w >/dev/null 2>&1; then
        echo "BROKEN-C seed=$seed"; brokenc=$((brokenc+1)); continue
    fi
    timeout 5 "$SC/t" >/dev/null 2>"$SC/e"
    if grep -qiE "AddressSanitizer|stack-use-after-return|runtime error|use-after" "$SC/e"; then
        echo "UNSOUND (accepted program dangles) seed=$seed exp=$exp:"; sed -n '1,3p' "$SC/e"
        unsound=$((unsound+1))
    fi
done

echo "fuzz_borrow: gens=$N  accept(clean=$acc_ok overRej=$acc_rej)  reject(rejected=$rej_ok compiled=$rej_compiled)"
echo "  bugs:  broken-C=$brokenc  UNSOUND=$unsound  missed-dangles=$rej_compiled"
rm -rf "$SC"
[ $((brokenc + unsound + rej_compiled + acc_rej)) -eq 0 ]
