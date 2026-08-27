#!/usr/bin/env bash
# fuzz_scope.sh — teeth for the flow store's NAME-keying soundness. Lain forbids
# shadowing (E013), so within a function a name is a unique identity; the store also
# resets per function. This fuzzer stresses exactly the collision surface that would
# break if either guarantee leaked: the SAME variable names (`n`, `i`, `a`) reused
# across MULTIPLE functions with DIFFERENT array sizes and bounds facts, plus nested
# scopes and guards. Every accepted program is executed under ASan against inputs
# that would expose a cross-function/scope fact leak as an out-of-bounds read.
set -u
cd "$(dirname "$0")"
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
    cat > "$src" <<EOF
extern proc libc_printf(fmt *u8, ...) i32
proc g(a i32[n], n usize) i32 {
    var sum i32 = 0
    var i usize = 0
    while i < n decreasing n - i {
        sum = sum + a[i]
        i = i + 1
    }
    return sum
}
proc h(a i32[n], n usize) i32 {
    var sum i32 = 0
    var i usize = 0
    while i < n decreasing n - i {
        sum = sum + a[i]
        i = i + 1
    }
    return sum
}
proc main() i32 {
    var x i32[$s1] = [$(seq -s ', ' 1 $s1)]
    var y i32[$s2] = [$(seq -s ', ' 1 $s2)]
    var t i32 = g(x, $s1) + h(y, $s2)
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
[ $((brokenc + unsound)) -eq 0 ]
