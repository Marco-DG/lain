#!/usr/bin/env bash
# fuzz_div.sh — teeth for division/modulo prove-or-reject. Generates div/mod under
# randomized guard shapes (unguarded, `if d != 0`, `if d > 0`, `if d == 0 return`,
# `!= 0` param constraint, constant divisor) and a main that calls the function
# across an adversarial divisor sweep that INCLUDES 0 and INT_MIN. Every program
# the compiler ACCEPTS is compiled with UBSan and executed: a SIGFPE (exit 136) or
# a UBSan "division by zero / cannot be represented" on an accepted program means
# the safety promise was violated — an UNSOUND bug.
#
# One program in three divides across SIGNEDNESS (I.198): a dividend and a divisor of two random
# integer types, through `/ % /% /|`, over each type's edge values. Its oracle is the VALUE: the
# native output must equal `lain --interpret`'s, since C converts an int32_t beside a uint32_t to
# unsigned and a quotient can be wrong without trapping (MISCOMPILE), and --interpret must not stop
# on a proof (UNSOUND). `mixed=` counts the accepted ones, so a zero there says nothing.
set -u
cd "$(dirname "$0")/../.."
LAIN=./lain
SC="${TMPDIR:-/tmp}/fuzz_div.$$"; mkdir -p "$SC"
N="${1:-300}"
RANDOM=${RANDOM_SEED:-$$}
accepted=0; rejected=0; unsound=0; brokenc=0; miscompile=0; mixed=0
TYPES=(i8 i16 i32 i64 u8 u16 u32 u64)
tmin() { case $1 in i8) echo -128;; i16) echo -32768;; i32) echo -2147483648;; i64) echo -9223372036854775808;; *) echo 0;; esac; }
tmax() { case $1 in i8) echo 127;; i16) echo 32767;; i32) echo 2147483647;; i64) echo 9223372036854775807;;
                    u8) echo 255;; u16) echo 65535;; u32) echo 4294967295;; u64) echo 18446744073709551615;; esac; }
# a Lain spelling of v: a negative value as a subtraction, TYPE_MIN of i64 as one below -MAX
tlit() { case $1 in -9223372036854775808) echo "(0 - 9223372036854775807 - 1)";; -*) echo "(0 - ${1#-})";; *) echo "$1";; esac; }

for ((i=0; i<N; i++)); do
    if [ $((RANDOM % 3)) -eq 0 ]; then                                # across signedness (I.198)
        A=${TYPES[$((RANDOM % 8))]}; B=${TYPES[$((RANDOM % 8))]}
        [ "${A:0:1}" = "${B:0:1}" ] && B=$([ "${A:0:1}" = i ] && echo u || echo i)${B:1}
        mops=("/" "%" "/%" "/|"); mop=${mops[$((RANDOM % 4))]}
        src="$SC/m_$i.ln"
        { echo 'extern func libc_printf(fmt *u8, ...) i32 effects io'
          echo "func dv(x $A, d $B) i64 {"; echo '    if d != 0 {'; echo "        return (x $mop d) as% i64"
          echo '    }'; echo '    return 0'; echo '}'
          echo 'func main() i32 effects io {'
          for x in $(tmin $A) -7 -1 0 7 $(tmax $A); do
              case $A in u*) [ "${x:0:1}" = - ] && continue;; esac
              for d in 0 1 3 -1 -3 $(tmin $B) $(tmax $B); do
                  case $B in u*) [ "${d:0:1}" = - ] && continue;; esac
                  echo "    libc_printf(\"%lld\\n\", dv($(tlit $x), $(tlit $d)))"
              done
          done
          echo '    return 0'; echo '}'; } > "$src"
        if ! $LAIN "$src" -o "$SC/m_$i.c" >/dev/null 2>&1; then rejected=$((rejected+1)); continue; fi
        accepted=$((accepted+1)); mixed=$((mixed+1))
        if ! gcc -fsanitize=undefined -fno-sanitize-recover=all -o "$SC/m_$i" "$SC/m_$i.c" -Dlibc_printf=printf -w >/dev/null 2>&1; then
            echo "BROKEN-C on accepted: $src"; brokenc=$((brokenc+1)); continue
        fi
        nat=$(timeout 5 "$SC/m_$i" 2>"$SC/me_$i"); rc=$?
        itp=$(timeout 30 $LAIN "$src" --interpret 2>"$SC/mi_$i")
        if grep -q 'PROOF FAILED' "$SC/mi_$i" || [ $rc -ne 0 ]; then
            echo "UNSOUND (accepted mixed-sign division fails): $src  $A $mop $B rc=$rc"; sed -n 1p "$SC/mi_$i" "$SC/me_$i"
            unsound=$((unsound+1))
        elif [ "$nat" != "$itp" ]; then
            echo "MISCOMPILE (native != --interpret): $src  $A $mop $B"
            diff <(echo "$nat") <(echo "$itp") | sed -n 2,3p
            miscompile=$((miscompile+1))
        fi
        continue
    fi
    op=$([ $((RANDOM%2)) -eq 0 ] && echo "/" || echo "%")
    shape=$((RANDOM % 6))
    # function signature + body
    case $shape in
      0) sig="func dv(x i32, d i32) i32"; body="    return x $op d";;                       # unguarded → must reject
      1) sig="func dv(x i32, d i32) i32"; body="    if d != 0 {
        return x $op d
    }
    return 0";;
      2) sig="func dv(x i32, d i32) i32"; body="    if d > 0 {
        return x $op d
    }
    return 0";;
      3) sig="func dv(x i32, d i32) i32"; body="    if d == 0 {
        return 0
    }
    return x $op d";;
      4) sig="func dv(x i32, d i32 != 0) i32"; body="    return x $op d";;                   # param precondition
      5) sig="func dv(x i32, d i32) i32"; body="    var k i32 = 7
    return x $op k";;                                                                          # constant nonzero
    esac
    src="$SC/t_$i.ln"
    cat > "$src" <<EOF
extern func libc_printf(fmt *u8, ...) i32 effects io
$sig {
$body
}
func main() i32 effects io, raises, alloc {
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
echo "fuzz_div: gens=$N accepted=$accepted rejected=$rejected mixed=$mixed"
echo "  bugs:  broken-C=$brokenc  UNSOUND=$unsound  MISCOMPILE=$miscompile"
rm -rf "$SC"
# A run that accepted (almost) nothing judged (almost) nothing, and must not print a zero. Twice
# this harness reported "bugs: 0" over 0 accepted programs (2026-09-19, 2026-09-28).
[ $accepted -lt $((N / 10)) ] && { echo "  ★ FUZZER DID NOT RUN: $accepted/$N accepted — this report says nothing"; exit 1; }
[ $((brokenc + unsound + miscompile)) -eq 0 ]
