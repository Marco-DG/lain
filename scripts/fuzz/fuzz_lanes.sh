#!/usr/bin/env bash
# fuzz_lanes.sh — vector lanes, the one kind of value no other fuzzer generated (I.202 and I.203
# were found by the judge's coverage census instead). Spec 07: integer lanes WRAP, so every lane
# operation is defined but two, a divisor that is not provably non-zero and a shift amount that is
# not provably in [0, w-1]. Each program applies one operator to a vector of random lane type and
# width, holding edge values (TYPE_MIN, TYPE_MAX, -1, 0, 1, ...), and to either a scalar (carrying
# the refinement that makes it legal: `!= 0` for a divisor, `<= w-1` for an amount) or a vector per
# lane under `unsafe` (whose lanes are chosen legal, since nothing can prove them so).
#
# The oracle is the VALUE: the program prints every lane, natively at -O0 and at -O2, both under
# UBSan with no recovery, and the three must equal `lain --interpret`'s. A difference is a
# MISCOMPILE (I.203 was one: TYPE_MIN / -1 in a lane, SIGFPE at -O0 and fine at -O2); a sanitizer
# report, a crash or --interpret's PROOF FAILED on an accepted program is UNSOUND.
set -u
cd "$(dirname "$0")/../.."
LAIN=./lain
SC="${TMPDIR:-/tmp}/fuzz_lanes.$$"; mkdir -p "$SC"; trap 'rm -rf "${SC:?}"' EXIT
N="${1:-120}"
RANDOM=${RANDOM_SEED:-$$}
accepted=0; rejected=0; unsound=0; miscompile=0; brokenc=0
TYPES=(i8 i16 i32 i64 u8 u16 u32 u64)
OPS=("+" "-" "*" "/" "%" "&" "|" "^" "<<" ">>" "<" ">=" "==" "neg")   # comparisons give a mask; neg is unary -
bits() { echo "${1:1}"; }
tmin() { case $1 in i8) echo -128;; i16) echo -32768;; i32) echo -2147483648;; i64) echo -9223372036854775808;; *) echo 0;; esac; }
tmax() { case $1 in i8) echo 127;; i16) echo 32767;; i32) echo 2147483647;; i64) echo 9223372036854775807;;
                    u8) echo 255;; u16) echo 65535;; u32) echo 4294967295;; u64) echo 18446744073709551615;; esac; }
tlit() { case $1 in -9223372036854775808) echo "(0 - 9223372036854775807 - 1)";; -*) echo "(0 - ${1#-})";; *) echo "$1";; esac; }
# a lane value: an edge of the type or a small one. A negative is computed, not spelled: `-$((x))` gave
# the string "-0" when x was 0, which lane_rhs's `= 0` test let through as a per-lane DIVISOR, so the
# program divided by zero inside its own `unsafe` and --interpret rightly said so (rc 96), reported
# here as UNSOUND though the generator, not the compiler, had broken the rule (2026-10-13)
lane_val() { local T=$1 r=$((RANDOM % 8)) s=${1:0:1}
    case $r in
        0) tmin $T;; 1) tmax $T;; 2) [ $s = i ] && echo -1 || echo 1;; 3) echo 0;; 4) echo 1;;
        5) [ $s = i ] && echo $(( -(RANDOM % 100) )) || echo $((RANDOM % 100));; *) echo $((RANDOM % 100));;
    esac; }
# a legal per-lane right operand: non-zero for / and %, in [0, w-1] for a shift
lane_rhs() { local T=$1 op=$2 w; w=$(bits $T)
    case $op in
        "/"|"%") local v; v=$(lane_val $T); [ "$v" = 0 ] && v=$([ ${T:0:1} = i ] && echo -1 || echo 3); echo "$v";;
        "<<"|">>") echo $((RANDOM % w));;
        *) lane_val $T;;
    esac; }

for ((i = 0; i < N; i++)); do
    T=${TYPES[$((RANDOM % 8))]}; w=$(bits $T)
    ns=(2 4 8 16); L=${ns[$((RANDOM % 4))]}
    op=${OPS[$((RANDOM % 14))]}
    perlane=$((RANDOM % 3 == 0)); [ "$op" = neg ] && perlane=1
    V="${T}x${L}"
    src="$SC/v_$i.ln"
    {
        echo 'extern func libc_printf(fmt *u8, ...) i32 effects io'
        if [ "$op" = neg ]; then
            echo "func f(a $V, b $V) $V {"
            echo "    return -a"
        elif [ $perlane = 1 ]; then
            echo "func f(a $V, b $V) $V {"
            echo "    var r $V = a"
            echo "    unsafe { r = a $op b }"
            echo "    return r"
        else
            # one scalar in three has the OTHER signedness, holding a value both types hold: it
            # lands in the lane type (I.204), checked, and the program must be accepted
            BT=$T; [ $((RANDOM % 3)) = 0 ] && BT=$([ ${T:0:1} = i ] && echo u || echo i)${T:1}
            # ...and of those, one in two is UNCONSTRAINED and holds a value outside the lane type:
            # the program must be refused (E086 at the landing), and one that compiles is checked
            # by the oracle like any other, which is how the missing landing showed (I.204)
            wild=0; [ $BT != $T ] && [ "$op" != "<<" ] && [ "$op" != ">>" ] && [ $((RANDOM % 2)) = 0 ] && wild=1
            case $op in
                "/"|"%") if [ $wild = 1 ]; then echo "func f(a $V, b $BT != 0) $V {"; else echo "func f(a $V, b $BT != 0 and >= 1 and <= 100) $V {"; fi;;
                "<<"|">>") echo "func f(a $V, b u32 <= $((w - 1))) $V {";;
                *) if [ $BT = $T ] || [ $wild = 1 ]; then echo "func f(a $V, b $BT) $V {"; else echo "func f(a $V, b $BT >= 0 and <= 100) $V {"; fi;;
            esac
            echo "    return a $op b"
        fi
        echo '}'
        echo 'func main() i32 effects io {'
        for t in 0 1 2; do
            al=""; for ((k = 0; k < L; k++)); do al="$al${al:+, }$(tlit "$(lane_val $T)")"; done
            echo "    var a$t $V = [$al]"
            if [ $perlane = 1 ]; then
                bl=""; for ((k = 0; k < L; k++)); do bl="$bl${bl:+, }$(tlit "$(lane_rhs $T "$op")")"; done
                echo "    var b$t $V = [$bl]"
            else
                b=$(lane_rhs $T "$op")
                if [ "$op" = "<<" ] || [ "$op" = ">>" ]; then echo "    b$t u32 = $b"
                elif [ $wild = 1 ]; then v=$( [ ${BT:0:1} = u ] && tmax $BT || tmin $BT ); echo "    b$t $BT = $(tlit "$v")"
                elif [ $BT != $T ] || [ "$op" = "/" ] || [ "$op" = "%" ]; then echo "    b$t $BT = $((RANDOM % 100 + 1))"
                else echo "    b$t $T = $(tlit "$b")"; fi
            fi
            echo "    r$t = f(a$t, b$t)"
            for ((k = 0; k < L; k++)); do echo "    libc_printf(\"%lld\\n\", r$t[$k] as% i64)"; done
        done
        echo '    return 0'
        echo '}'
    } > "$src"
    if ! $LAIN "$src" -o "$SC/v_$i.c" > "$SC/v_$i.err" 2>&1; then rejected=$((rejected + 1)); continue; fi
    accepted=$((accepted + 1))
    itp=$(timeout 30 $LAIN "$src" --interpret 2> "$SC/vi_$i"); irc=$?
    if [ $irc -ne 0 ] || grep -q 'PROOF FAILED' "$SC/vi_$i"; then
        echo "UNSOUND (--interpret on an accepted program, rc=$irc): $src  $V $op $([ $perlane = 1 ] && echo per-lane || echo scalar)"
        sed -n 1p "$SC/vi_$i"; unsound=$((unsound + 1)); continue
    fi
    for O in -O0 -O2; do
        if ! gcc $O -fsanitize=undefined -fno-sanitize-recover=all -o "$SC/v_$i$O" "$SC/v_$i.c" -Dlibc_printf=printf -w > /dev/null 2>&1; then
            echo "BROKEN-C on accepted ($O): $src"; brokenc=$((brokenc + 1)); continue 2
        fi
        nat=$(timeout 10 "$SC/v_$i$O" 2> "$SC/ve_$i$O"); nrc=$?
        if [ $nrc -ne 0 ] || [ -s "$SC/ve_$i$O" ]; then
            echo "UNSOUND (native $O on an accepted program, rc=$nrc): $src  $V $op $([ $perlane = 1 ] && echo per-lane || echo scalar)"
            sed -n 1p "$SC/ve_$i$O"; unsound=$((unsound + 1)); continue 2
        fi
        if [ "$nat" != "$itp" ]; then
            echo "MISCOMPILE (native $O != --interpret): $src  $V $op $([ $perlane = 1 ] && echo per-lane || echo scalar)"
            diff <(echo "$nat") <(echo "$itp") | sed -n 2,3p; miscompile=$((miscompile + 1)); continue 2
        fi
    done
done
echo "fuzz_lanes: gens=$N accepted=$accepted rejected=$rejected"
echo "  bugs:  broken-C=$brokenc  UNSOUND=$unsound  MISCOMPILE=$miscompile"
# a run that accepted (almost) nothing judged (almost) nothing
[ $accepted -lt $((N / 4)) ] && { echo "  ★ FUZZER DID NOT RUN: $accepted/$N accepted — this report says nothing"; exit 1; }
[ $((brokenc + unsound + miscompile)) -eq 0 ]
