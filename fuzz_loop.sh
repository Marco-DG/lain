#!/usr/bin/env bash
# fuzz_loop.sh — teeth for general loop bounds analysis (VRA widening / affine /
# inductive invariant), beyond the binary-search shape. Generates while-loops with
# randomized bound, step, and array-index expression (i, i±k, n-1-i, i*k, i%k, a
# second running var), then a main that runs the loop over a real array. Every
# program the compiler ACCEPTS promises every `a[idx]` is in bounds — so it is
# executed under ASan. Any heap/stack overflow (or a hang) on an accepted program
# is an UNSOUND bug.
set -u
cd "$(dirname "$0")"
LAIN=./lain
SC="${TMPDIR:-/tmp}/fuzz_loop.$$"; mkdir -p "$SC"
N="${1:-300}"
RANDOM=${RANDOM_SEED:-$$}
accepted=0; rejected=0; unsound=0; brokenc=0

sizes=(4 8 16 32)
for ((i=0; i<N; i++)); do
    sz=${sizes[$((RANDOM % ${#sizes[@]}))]}
    # loop upper bound: the array size, or off-by-some (too big → OOB if accepted)
    case $((RANDOM % 4)) in
      0) bound=$sz;;
      1) bound=$((sz - 1));;
      2) bound=$((sz + 1 + RANDOM % 8));;   # exceeds size → index must reject
      3) bound=$sz;;
    esac
    (( bound < 1 )) && bound=1
    step=$(( (RANDOM % 3) + 1 ))            # 1..3
    # index expression forms (k small)
    k=$(( RANDOM % 3 ))
    case $((RANDOM % 7)) in
      0) idx="i";;
      1) idx="i + $k";;
      2) idx="i - $k";;                     # can underflow usize when i<k
      3) idx="$((sz-1)) - i";;              # reverse
      4) idx="i * $(( (RANDOM%2)+1 ))";;
      5) idx="i % $sz";;                    # ring
      6) idx="j";;                          # second running var
    esac
    # optional guard around the write
    guard=$((RANDOM % 3))

    src="$SC/t_$i.ln"
    {
      echo "extern proc libc_printf(fmt *u8, ...) i32"
      echo "proc run(a i32[$sz]) i32 {"
      echo "    var i usize = 0"
      echo "    var j usize = 0"
      echo "    var s i32 = 0"
      echo "    while i < $bound decreasing $bound - i {"
      case $guard in
        0) echo "        s = s + a[$idx]";;
        1) echo "        if $idx < $sz {"; echo "            s = s + a[$idx]"; echo "        }";;
        2) echo "        if i < $sz {"; echo "            s = s + a[$idx]"; echo "        }";;
      esac
      echo "        j = j + 1"
      echo "        i = i + $step"
      echo "    }"
      echo "    return s"
      echo "}"
      echo "proc main() i32 {"
      printf "    var arr i32[$sz] = ["
      for ((e=0; e<sz; e++)); do printf "%s%d" "$([ $e -gt 0 ] && echo ', ')" "$e"; done
      echo "]"
      echo "    libc_printf(\"%d\\n\", run(arr))"
      echo "    return 0"
      echo "}"
    } > "$src"

    cfile="$SC/t_$i.c"
    if ! $LAIN "$src" -o "$cfile" >/dev/null 2>&1; then rejected=$((rejected+1)); continue; fi
    accepted=$((accepted+1))
    bin="$SC/t_$i"
    if ! gcc -fsanitize=address,undefined -o "$bin" "$cfile" -Dlibc_printf=printf -Dlibc_puts=puts -w >/dev/null 2>&1; then
        echo "BROKEN-C on accepted: $src"; brokenc=$((brokenc+1)); continue
    fi
    timeout 5 "$bin" >/dev/null 2>"$SC/e_$i"; rc=$?
    if [ $rc -eq 124 ]; then
        echo "HANG (accepted, non-terminating): $src"; unsound=$((unsound+1)); continue
    fi
    if grep -qiE "AddressSanitizer|runtime error|out of bounds|overflow" "$SC/e_$i"; then
        echo "UNSOUND (accepted program OOB/UB): $src"; sed -n '1,3p' "$SC/e_$i"
        unsound=$((unsound+1))
    fi
done
echo "fuzz_loop: gens=$N accepted=$accepted rejected=$rejected"
echo "  bugs:  broken-C=$brokenc  UNSOUND=$unsound"
rm -rf "$SC"
[ $((brokenc + unsound)) -eq 0 ]
