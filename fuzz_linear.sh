#!/usr/bin/env bash
# fuzz_linear.sh — teeth for the ownership/linearity checker (the memory-safety core).
#
# Owned heap resources are modelled by a REAL malloc'd pointer: `release` frees it,
# `touch` derefs it. If the linear checker is SOUND, every program it ACCEPTS consumes
# each resource exactly once and never touches it afterwards — hence must be clean under
# ASan + LeakSanitizer. The generator (fuzz_linear.py) emits two streams:
#   accept — valid by construction: must compile AND run clean (dirty => UNSOUND accept;
#            rejected => over-rejection, a precision regression).
#   reject — one injected violation (leak / double-free / use-after-free / move-in-loop):
#            must NOT compile. If it compiles it is executed anyway; dirty => UNSOUND,
#            clean-but-accepted => a linearity violation the checker missed (SUSPECT).
set -u
cd "$(dirname "$0")"
LAIN=./lain
GEN=fuzz_linear.py
SC="${TMPDIR:-/tmp}/fuzz_linear.$$"; mkdir -p "$SC"
N="${1:-300}"
BASE=${RANDOM_SEED:-$$}
export ASAN_OPTIONS=detect_leaks=1
acc_ok=0 acc_rej=0 rej_ok=0 rej_compiled=0 unsound=0 brokenc=0

for ((k=0; k<N; k++)); do
    seed=$((BASE * 100000 + k))
    python3 "$GEN" "$seed" > "$SC/t.ln"
    exp=$(head -1 "$SC/t.ln" | grep -oE 'accept|reject')
    if "$LAIN" "$SC/t.ln" -o "$SC/t.c" >/dev/null 2>&1; then compiled=1; else compiled=0; fi

    if [ "$exp" = accept ]; then
        if [ $compiled -eq 0 ]; then
            echo "OVER-REJECTION (valid program refused) seed=$seed:"; "$LAIN" "$SC/t.ln" -o /dev/null 2>&1 | grep -oE '\[E[0-9]+\].*' | head -1
            acc_rej=$((acc_rej+1)); continue
        fi
        acc_ok=$((acc_ok+1))
    else
        if [ $compiled -eq 0 ]; then rej_ok=$((rej_ok+1)); continue; fi
        echo "MISSED VIOLATION (reject-stream program compiled) seed=$seed"; rej_compiled=$((rej_compiled+1))
    fi

    # run every compiled program under ASan+LSan — for accept-stream programs, run
    # BOTH branch directions (flip `flag`) so conditional consume paths are covered.
    if ! gcc -fsanitize=address,undefined -o "$SC/t" "$SC/t.c" -Dlibc_malloc=malloc -Dlibc_free=free -w >/dev/null 2>&1; then
        echo "BROKEN-C seed=$seed"; brokenc=$((brokenc+1)); continue
    fi
    timeout 5 "$SC/t" >/dev/null 2>"$SC/e"
    if grep -qiE "AddressSanitizer|LeakSanitizer|runtime error|double-free|use-after" "$SC/e"; then
        echo "UNSOUND (accepted program is dirty) seed=$seed exp=$exp flag=1:"; sed -n '1,3p' "$SC/e"
        unsound=$((unsound+1)); continue
    fi
    if [ "$exp" = accept ] && grep -q 'var flag i32 = 1' "$SC/t.ln"; then
        sed 's/var flag i32 = 1/var flag i32 = 0/' "$SC/t.ln" > "$SC/t0.ln"
        if "$LAIN" "$SC/t0.ln" -o "$SC/t0.c" >/dev/null 2>&1 \
           && gcc -fsanitize=address,undefined -o "$SC/t0" "$SC/t0.c" -Dlibc_malloc=malloc -Dlibc_free=free -w >/dev/null 2>&1; then
            timeout 5 "$SC/t0" >/dev/null 2>"$SC/e0"
            if grep -qiE "AddressSanitizer|LeakSanitizer|runtime error|double-free|use-after" "$SC/e0"; then
                echo "UNSOUND (accepted program is dirty) seed=$seed flag=0:"; sed -n '1,3p' "$SC/e0"
                unsound=$((unsound+1))
            fi
        fi
    fi
done

echo "fuzz_linear: gens=$N  accept(clean=$acc_ok overRej=$acc_rej)  reject(rejected=$rej_ok compiled=$rej_compiled)"
echo "  bugs:  broken-C=$brokenc  UNSOUND=$unsound  missed-violations=$rej_compiled"
rm -rf "$SC"
[ $((brokenc + unsound + rej_compiled + acc_rej)) -eq 0 ]
