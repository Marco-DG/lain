#!/usr/bin/env bash
# fuzz_shift.sh — teeth for shift prove-or-reject (amount-in-range AND signed
# left-shift overflow). Generates `x << n` / `x >> n` with randomized operand
# signedness, amount forms (literal / variable / guarded), and a main that sweeps
# the amount across [0, width+8) and the value across small magnitudes. Every
# ACCEPTED program is executed under UBSan: any "shift exponent too large" or
# "cannot be represented" (signed-shift overflow) on an accepted program is an
# UNSOUND bug.
set -u
cd "$(dirname "$0")"
LAIN=./lain
SC="${TMPDIR:-/tmp}/fuzz_shift.$$"; mkdir -p "$SC"
N="${1:-300}"
RANDOM=${RANDOM_SEED:-$$}
accepted=0; rejected=0; unsound=0; brokenc=0

for ((i=0; i<N; i++)); do
    ty=$([ $((RANDOM%2)) -eq 0 ] && echo i32 || echo u32)
    fmt=$([ "$ty" = i32 ] && echo '%d' || echo '%u')
    dir=$([ $((RANDOM%2)) -eq 0 ] && echo '<<' || echo '>>')
    amt=$((RANDOM % 4))
    case $amt in
      0) sig="proc sh(x $ty, n $ty) $ty"; body="    return x $dir n";;                 # unguarded var
      1) sig="proc sh(x $ty, n $ty) $ty"; body="    if n < 32 {
        return x $dir n
    }
    return 0";;                                                                          # guarded < width
      2) lit=$((RANDOM % 40)); sig="proc sh(x $ty, n $ty) $ty"; body="    return x $dir $lit";;  # literal amount
      3) sig="proc sh(x $ty, n $ty) $ty"; body="    if n < 32 and x < 2 {
        return x $dir n
    }
    return 0";;                                                                          # guarded amount+value
    esac
    src="$SC/t_$i.ln"
    cat > "$src" <<EOF
extern proc libc_printf(fmt *u8, ...) i32
$sig {
$body
}
proc main() i32 {
    var acc $ty = 0
    var n $ty = 0
    while n < 40 decreasing 40 - n {
        acc = acc +% sh(1, n)
        n = n + 1
    }
    libc_printf("$fmt\n", acc)
    return 0
}
EOF
    cfile="$SC/t_$i.c"
    if ! $LAIN "$src" -o "$cfile" >/dev/null 2>&1; then rejected=$((rejected+1)); continue; fi
    accepted=$((accepted+1))
    bin="$SC/t_$i"
    if ! gcc -fsanitize=undefined -o "$bin" "$cfile" -Dlibc_printf=printf -Dlibc_puts=puts -w >/dev/null 2>&1; then
        echo "BROKEN-C on accepted: $src"; brokenc=$((brokenc+1)); continue
    fi
    timeout 5 "$bin" >/dev/null 2>"$SC/e_$i"; rc=$?
    if [ $rc -eq 124 ]; then continue; fi
    if grep -qiE "shift|cannot be represented|runtime error" "$SC/e_$i"; then
        echo "UNSOUND (accepted program shift-UB): $src"; sed -n '1,2p' "$SC/e_$i"
        unsound=$((unsound+1))
    fi
done
echo "fuzz_shift: gens=$N accepted=$accepted rejected=$rejected"
echo "  bugs:  broken-C=$brokenc  UNSOUND=$unsound"
rm -rf "$SC"
[ $((brokenc + unsound)) -eq 0 ]
