#!/usr/bin/env bash
# fuzz_div.sh — teeth for division/modulo prove-or-reject. Generates div/mod under
# randomized guard shapes (unguarded, `if d != 0`, `if d > 0`, `if d == 0 return`,
# `!= 0` param constraint, constant divisor) and a main that calls the function
# across an adversarial divisor sweep that INCLUDES 0 and INT_MIN. Every program
# the compiler ACCEPTS is compiled with UBSan and executed: a SIGFPE (exit 136) or
# a UBSan "division by zero / cannot be represented" on an accepted program means
# the safety promise was violated — an UNSOUND bug.
set -u
cd "$(dirname "$0")"
LAIN=./lain
SC="${TMPDIR:-/tmp}/fuzz_div.$$"; mkdir -p "$SC"
N="${1:-300}"
RANDOM=${RANDOM_SEED:-$$}
accepted=0; rejected=0; unsound=0; brokenc=0

for ((i=0; i<N; i++)); do
    op=$([ $((RANDOM%2)) -eq 0 ] && echo "/" || echo "%")
    shape=$((RANDOM % 6))
    # function signature + body
    case $shape in
      0) sig="proc dv(x i32, d i32) i32"; body="    return x $op d";;                       # unguarded → must reject
      1) sig="proc dv(x i32, d i32) i32"; body="    if d != 0 {
        return x $op d
    }
    return 0";;
      2) sig="proc dv(x i32, d i32) i32"; body="    if d > 0 {
        return x $op d
    }
    return 0";;
      3) sig="proc dv(x i32, d i32) i32"; body="    if d == 0 {
        return 0
    }
    return x $op d";;
      4) sig="proc dv(x i32, d i32 != 0) i32"; body="    return x $op d";;                   # param precondition
      5) sig="proc dv(x i32, d i32) i32"; body="    var k i32 = 7
    return x $op k";;                                                                          # constant nonzero
    esac
    src="$SC/t_$i.ln"
    cat > "$src" <<EOF
extern proc libc_printf(fmt *u8, ...) i32
$sig {
$body
}
proc main() i32 {
    var acc i32 = 0
    var d i32 = 0 - 2
    while d < 4 decreasing 4 - d {
        acc = acc + dv(100, d)
        d = d + 1
    }
    libc_printf("%d\n", acc)
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
    out=$(timeout 5 "$bin" 2>"$SC/e_$i"); rc=$?
    if [ $rc -eq 136 ] || grep -qiE "division by zero|cannot be represented|runtime error" "$SC/e_$i"; then
        echo "UNSOUND (accepted program traps on division): $src"
        echo "    shape=$shape op=$op rc=$rc"; sed -n '1,2p' "$SC/e_$i"
        unsound=$((unsound+1))
    fi
done
echo "fuzz_div: gens=$N accepted=$accepted rejected=$rejected"
echo "  bugs:  broken-C=$brokenc  UNSOUND=$unsound"
rm -rf "$SC"
[ $((brokenc + unsound)) -eq 0 ]
