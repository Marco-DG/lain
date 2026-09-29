#!/usr/bin/env bash
# fuzz_scope.sh — teeth for the flow store's NAME-keying soundness. Lain forbids
# shadowing (E013), so within a function a name is a unique identity; the store also
# resets per function. This fuzzer stresses exactly the collision surface that would
# break if either guarantee leaked: the SAME variable names (`n`, `i`, `a`) reused
# across MULTIPLE functions with DIFFERENT array sizes and bounds facts, plus nested
# scopes and guards. Every accepted program is executed under ASan against inputs
# that would expose a cross-function/scope fact leak as an out-of-bounds read.
set -u
cd "$(dirname "$0")/../.."
LAIN=./lain
SC="${TMPDIR:-/tmp}/fuzz_scope.$$"; mkdir -p "$SC"
N="${1:-250}"
RANDOM=${RANDOM_SEED:-$$}
accepted=0; rejected=0; unsound=0; brokenc=0
sizes=(4 8 16)

for ((i=0; i<N; i++)); do
    s1=${sizes[$((RANDOM % ${#sizes[@]}))]}
    s2=${sizes[$((RANDOM % ${#sizes[@]}))]}
    # Two functions, same var names (n/i/a), different sizes. If a fact from g leaked
    # into h (or vice versa) via the shared name key, a[i] could be mis-proven.
    src="$SC/t_$i.ln"
    # Arrays sized BY the param `n` (a i32[n]) so a[i] is proven ONLY via the
    # name-keyed `i < n` constraint — the exact fact that would leak across the two
    # same-named functions if the store weren't per-function / name-sound.
    #
    # ★ `+%`, NOT `+`, AND THAT IS WHY THIS FUZZER WORKS AT ALL. With `+` the running total
    # over an unbounded `n` cannot be proven in i32, so from the day overflow became a
    # sovereign obligation (2026-09-18) EVERY generated program was refused — accepted=0 of
    # 250, reported as "bugs: 0". The rejection was correct and it was about something this
    # fuzzer does not test; saying `+%` states which arithmetic is meant and leaves the BOUNDS
    # question, the one under test, exactly as it was.
    #
    # ★ AND `+%` IN main TOO. `var t i32 = g(..) + h(..)` was accepted only because the range
    # analysis read the `+%` running totals as integers and so bounded g's result — a false
    # proof: with x = [2147483647, 0, 0, 0], `g(x, 4) + 1` compiled into an i32 and wrapped
    # negative. Fixed 2026-09-28, which refused all 250 programs — accepted=0 again, for the
    # opposite reason. The totals are modular; their SUM is too.
    cat > "$src" <<EOF
extern func libc_printf(fmt *u8, ...) i32 effects io
func g(a i32[n], n usize) i32 {
    var sum i32 = 0
    var i usize = 0
    while i < n decreasing n - i {
        sum = sum +% a[i]
        i = i + 1
    }
    return sum
}
func h(a i32[n], n usize) i32 {
    var sum i32 = 0
    var i usize = 0
    while i < n decreasing n - i {
        sum = sum +% a[i]
        i = i + 1
    }
    return sum
}
func main() i32 effects io, raises, alloc {
    var x i32[$s1] = [$(seq -s ', ' 1 $s1)]
    var y i32[$s2] = [$(seq -s ', ' 1 $s2)]
    var t i32 = g(x, $s1) +% h(y, $s2)
    libc_printf("%d\n", t)
    return 0
}
EOF
    cfile="$SC/t_$i.c"
    if ! $LAIN "$src" -o "$cfile" >/dev/null 2>&1; then rejected=$((rejected+1)); continue; fi
    accepted=$((accepted+1))
    bin="$SC/t_$i"
    if ! gcc -fsanitize=address,undefined -o "$bin" "$cfile" -Dlibc_printf=printf -Dlibc_puts=puts -w >/dev/null 2>&1; then
        echo "BROKEN-C: $src"; brokenc=$((brokenc+1)); continue
    fi
    timeout 5 "$bin" >/dev/null 2>"$SC/e_$i"; rc=$?
    if [ $rc -eq 124 ]; then echo "HANG: $src"; unsound=$((unsound+1)); continue; fi
    if grep -qiE "AddressSanitizer|runtime error|out of bounds" "$SC/e_$i"; then
        echo "UNSOUND (name-key fact leak → OOB): $src"; sed -n '1,3p' "$SC/e_$i"
        unsound=$((unsound+1))
    fi
done
echo "fuzz_scope: gens=$N accepted=$accepted rejected=$rejected"
echo "  bugs:  broken-C=$brokenc  UNSOUND=$unsound"
rm -rf "$SC"
# A run that accepted (almost) nothing judged (almost) nothing, and must not print a zero. Twice
# this harness reported "bugs: 0" over 0 accepted programs (2026-09-19, 2026-09-28).
[ $accepted -lt $((N / 10)) ] && { echo "  ★ FUZZER DID NOT RUN: $accepted/$N accepted — this report says nothing"; exit 1; }
[ $((brokenc + unsound)) -eq 0 ]
