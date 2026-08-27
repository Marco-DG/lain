#!/usr/bin/env bash
# fuzz_mask.sh — teeth for the L3 mask-invariant (`h = X & C` keeps h in [0,C], so a
# masked ring index `table[h]` proves). Generates `var h = h0 & M0; while k<N {
# table[h]=1; h=(h+1) & M1; k++ }` over table[SZ] with randomized masks and size,
# called with an ADVERSARIAL h0 (huge). Safe iff BOTH masks keep h < SZ (entry gate
# M0 and loop mask M1 must each be <= SZ-1). If the invariant over-proves, an accepted
# program writes out of bounds under ASan.
set -u
cd "$(dirname "$0")"
LAIN=./lain
SC="${TMPDIR:-/tmp}/fuzz_mask.$$"; mkdir -p "$SC"
N="${1:-250}"
RANDOM=${RANDOM_SEED:-$$}
accepted=0; rejected=0; unsound=0; brokenc=0
# masks are 2^k-1; sizes are powers of two so a matching mask is exactly SZ-1
masks=(15 31 63 127 255)
sizes=(16 32 64 128)

for ((i=0; i<N; i++)); do
    m0=${masks[$((RANDOM % ${#masks[@]}))]}
    m1=${masks[$((RANDOM % ${#masks[@]}))]}
    sz=${sizes[$((RANDOM % ${#sizes[@]}))]}
    src="$SC/t_$i.ln"
    {
      echo 'extern proc libc_printf(fmt *u8, ...) i32'
      echo "proc fill(var table i32[$sz], h0 usize) {"
      echo "    var h usize = h0 & $m0"
      echo "    var k usize = 0"
      echo "    while k < $sz decreasing $sz - k {"
      echo "        table[h] = 1"
      echo "        h = (h + 1) & $m1"
      echo "        k = k + 1"
      echo "    }"
      echo "}"
      echo 'proc main() i32 {'
      printf "    var t i32[$sz] = ["
      for ((e=0;e<sz;e++)); do printf "%s0" "$([ $e -gt 0 ] && echo ', ')"; done
      echo "]"
      echo "    fill(var t, 4294967295)"
      echo '    libc_printf("%d\n", t[0])'
      echo '    return 0'
      echo '}'
    } > "$src"
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
        echo "UNSOUND (mask invariant over-proved): $src m0=$m0 m1=$m1 sz=$sz"; sed -n '1,3p' "$SC/e_$i"
        unsound=$((unsound+1))
    fi
done
echo "fuzz_mask: gens=$N accepted=$accepted rejected=$rejected"
echo "  bugs:  broken-C=$brokenc  UNSOUND=$unsound"
rm -rf "$SC"
[ $((brokenc + unsound)) -eq 0 ]
