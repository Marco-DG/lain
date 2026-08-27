#!/usr/bin/env bash
# fuzz_offset.sh — teeth for the symbolic-offset guard capture (`i < n ± k` folded
# into a difference constraint, so sliding-window / two-pointer indices prove). It
# generates `while i < n - K { s += a[i + M] }` over an array sized BY n, called with
# n = the real length. Safe iff M <= K (then i+M <= n-1); if the new constraint
# OVER-proves, an accepted M > K program reads out of bounds under ASan.
set -u
cd "$(dirname "$0")"
LAIN=./lain
SC="${TMPDIR:-/tmp}/fuzz_offset.$$"; mkdir -p "$SC"
N="${1:-250}"
RANDOM=${RANDOM_SEED:-$$}
accepted=0; rejected=0; unsound=0; brokenc=0
sizes=(6 8 12 16)

for ((i=0; i<N; i++)); do
    sz=${sizes[$((RANDOM % ${#sizes[@]}))]}
    src="$SC/t_$i.ln"
    if [ $((RANDOM % 2)) -eq 0 ]; then
        # PLUS: `while i < n - K { a[i + M] }` — safe iff M <= K (i+M <= n-1).
        K=$((RANDOM % 4)); M=$((RANDOM % 4))
        body="    var i usize = 0
    while i < n - $K decreasing n - $K - i {
        s = s +% a[i + $M]
        i = i + 1
    }"
    else
        # MINUS: `var i = S; while i < n { a[i - M] }` — safe iff S >= M (i-M >= 0).
        M=$((RANDOM % 4)); S=$((RANDOM % 4))
        body="    var i usize = $S
    while i < n decreasing n - i {
        s = s +% a[i - $M]
        i = i + 1
    }"
    fi
    cat > "$src" <<EOF
extern proc libc_printf(fmt *u8, ...) i32
proc scan(a i32[n], n usize) i32 {
    var s i32 = 0
$body
    return s
}
proc main() i32 {
    var arr i32[$sz] = [$(seq -s ', ' 1 $sz)]
    libc_printf("%d\n", scan(arr, $sz))
    return 0
}
EOF
    cfile="$SC/t_$i.c"
    if ! $LAIN "$src" -o "$cfile" >/dev/null 2>&1; then rejected=$((rejected+1)); continue; fi
    accepted=$((accepted+1))
    bin="$SC/t_$i"
    if ! gcc -fsanitize=address,undefined -o "$bin" "$cfile" -Dlibc_printf=printf -w >/dev/null 2>&1; then
        echo "BROKEN-C: $src"; brokenc=$((brokenc+1)); continue
    fi
    timeout 5 "$bin" >/dev/null 2>"$SC/e_$i"; rc=$?
    if [ $rc -eq 124 ]; then echo "HANG: $src"; unsound=$((unsound+1)); continue; fi
    if grep -qiE "AddressSanitizer|out of bounds|runtime error" "$SC/e_$i"; then
        echo "UNSOUND (offset guard over-proved): $src K=$K M=$M sz=$sz"; sed -n '1,3p' "$SC/e_$i"
        unsound=$((unsound+1))
    fi
done
echo "fuzz_offset: gens=$N accepted=$accepted rejected=$rejected"
echo "  bugs:  broken-C=$brokenc  UNSOUND=$unsound"
rm -rf "$SC"
[ $((brokenc + unsound)) -eq 0 ]
