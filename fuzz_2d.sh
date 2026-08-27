#!/usr/bin/env bash
# fuzz_2d.sh — teeth for symbolic 2D indexing `a[i*w+j]` over `a[h*w]` (i<h, j<w).
# The bounds proof `i*w+j < h*w = len(a)` and the `i*w` overflow skip both rest on
# `len(a) == h*w` — a sized-slice precondition. When the precondition holds (matching
# dimensions), the flattened access must stay in bounds: this fuzzer sums an N-element
# matrix via `a[i*w+j]` for MANY (h,w) factorizations with h*w == N, executed under
# ASan — any OOB means the 2D bounds proof is wrong for a valid matrix. It also checks
# that a PROVABLE mismatch (h*w != N) is rejected at compile (E087), never run.
set -u
cd "$(dirname "$0")"
LAIN=./lain
SC="${TMPDIR:-/tmp}/fuzz_2d.$$"; mkdir -p "$SC"
N="${1:-200}"
RANDOM=${RANDOM_SEED:-$$}
accepted=0; rejected=0; unsound=0; brokenc=0; mism_ok=0; mism_bad=0

# (rows, cols) pairs whose product is the array size
declare -A DIMS=( [12]="1 2 3 4 6 12" [16]="1 2 4 8 16" [24]="1 2 3 4 6 8 12 24" [36]="1 2 3 4 6 9 12 18 36" )
sizes=(12 16 24 36)

kernel() {
cat <<EOF
extern proc libc_printf(fmt *u8, ...) i32
proc msum(a i32[h * w], h usize, w usize) i32 {
    var s i32 = 0
    var i usize = 0
    while i < h decreasing h - i {
        var j usize = 0
        while j < w decreasing w - j {
            s = s +% a[i * w + j]
            j = j + 1
        }
        i = i + 1
    }
    return s
}
proc main() i32 {
    var m i32[$1] = [$(seq -s ', ' 1 $1)]
    libc_printf("%d\n", msum(m, $2, $3))
    return 0
}
EOF
}

for ((k=0; k<N; k++)); do
    sz=${sizes[$((RANDOM % ${#sizes[@]}))]}
    rows=($(echo ${DIMS[$sz]})); h=${rows[$((RANDOM % ${#rows[@]}))]}
    if [ $((RANDOM % 4)) -eq 0 ]; then
        # MISMATCH: pick a w so that h*w != sz — must REJECT at compile (E087)
        w=$(( h + 1 + RANDOM % 3 ))
        (( h * w == sz )) && w=$((w+1))
        kernel "$sz" "$h" "$w" > "$SC/t.ln"
        if $LAIN "$SC/t.ln" -o /dev/null >/dev/null 2>&1; then
            echo "MISMATCH NOT REJECTED (h*w=$((h*w)) != sz=$sz): compiled — UNSOUND"; mism_bad=$((mism_bad+1))
        else mism_ok=$((mism_ok+1)); fi
        continue
    fi
    w=$(( sz / h ))
    (( h * w != sz )) && continue    # not an exact factor pair
    kernel "$sz" "$h" "$w" > "$SC/t.ln"
    if ! $LAIN "$SC/t.ln" -o "$SC/t.c" >/dev/null 2>&1; then rejected=$((rejected+1)); continue; fi
    accepted=$((accepted+1))
    if ! gcc -fsanitize=address,undefined -o "$SC/t" "$SC/t.c" -Dlibc_printf=printf -w >/dev/null 2>&1; then
        echo "BROKEN-C: h=$h w=$w sz=$sz"; brokenc=$((brokenc+1)); continue
    fi
    out=$(timeout 5 "$SC/t" 2>"$SC/e"); exp=$(( sz*(sz+1)/2 ))
    if grep -qiE "AddressSanitizer|out of bounds|runtime error" "$SC/e"; then
        echo "UNSOUND (2D OOB, matching dims h=$h w=$w sz=$sz):"; sed -n '1,2p' "$SC/e"; unsound=$((unsound+1))
    elif [ "$out" != "$exp" ]; then
        echo "WRONG RESULT h=$h w=$w sz=$sz: got $out want $exp"; unsound=$((unsound+1))
    fi
done
echo "fuzz_2d: gens=$N  matching(accepted=$accepted rejected=$rejected)  mismatch(rejected=$mism_ok NOTrejected=$mism_bad)"
echo "  bugs:  broken-C=$brokenc  UNSOUND=$((unsound + mism_bad))"
rm -rf "$SC"
[ $((brokenc + unsound + mism_bad)) -eq 0 ]
