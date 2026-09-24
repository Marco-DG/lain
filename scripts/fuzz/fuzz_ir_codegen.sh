#!/usr/bin/env bash
# fuzz_ir_codegen.sh — EXECUTION-DIFFERENTIAL fuzzer for the NEW IR pipeline (corrective C2).
#
# Every other fuzzer in this repo exercises the OLD pipeline. The new IR pipeline's codegen
# was guarded by only 15 self-checking programs, and that blind spot hid a REAL MISCOMPILE:
# a scalar `var x` parameter travelled BY VALUE, so callee writes were lost (41 instead of
# 42). This closes it.
#
# For each generated program, compile through BOTH pipelines and diff observable behaviour:
#     old:  ./lain  file.ln -o old.c   → cc → run → stdout+exit
#     new:  lowerdrv file.ln --emit-c  → cc → run → stdout+exit
# A difference is a MISCOMPILE in the new pipeline (the old engine is the oracle: it is what
# the corpus validates). Also flags broken-C from the new emitter.
#
# Generators deliberately target where the new lowering can diverge: mutable-borrow params
# (scalar AND struct — the found bug class), short-circuit and/or guards (whose lowering we
# changed to nested branches), loops+indexing, nested calls, and wrapping arithmetic.
#
#   bash fuzz_ir_codegen.sh [N]        # default 200 programs
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
LAIN="$ROOT/lain"; LOWERDRV="${LOWERDRV:-/tmp/lowerdrv}"
CC="${CC:-gcc}"; N="${1:-200}"
DEFS="-Dlibc_printf=printf -Dlibc_malloc=malloc -Dlibc_free=free"
TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
[ -x "$LAIN" ] || { echo "build first: gcc -std=c99 -o lain src/frontends/lain/main.c -I src"; exit 2; }
gcc -std=c99 -o "$LOWERDRV" src/tools/lower_driver.c -I src 2>/dev/null || { echo "build lowerdrv failed"; exit 2; }

r() { echo $(( RANDOM % $1 + ${2:-0} )); }

gen_mutparam_scalar() {   # scalar `var` param mutation — the class that hid the miscompile
    local a=$(r 50 1) b=$(r 9 1)
    cat <<EOF
proc bump(var x i32, k i32) {
    x = x +% k
}
proc main() i32 {
    var v i32 = $a
    bump(var v, $b)
    bump(var v, $b)
    return v
}
EOF
}
gen_mutparam_struct() {   # struct `var` param mutation through a field
    local a=$(r 40 1) b=$(r 7 1)
    cat <<EOF
type Box { val i32 }
proc addto(var b Box, k i32) {
    b.val = b.val +% k
}
proc main() i32 {
    var bx Box
    bx.val = $a
    addto(var bx, $b)
    return bx.val
}
EOF
}
gen_shortcircuit() {      # and/or guards — lowering changed to nested branches
    local n=$(r 20 4) t=$(r 10 1)
    cat <<EOF
proc main() i32 {
    var a i32[$n] = [0 for z in 0..$n]
    var i = 0
    var hits = 0
    while i < $n decreasing $n - i {
        a[i] = i
        if i > 1 and i < $t {
            hits = hits +% 1
        }
        if i == 0 or i == $((n-1)) {
            hits = hits +% 2
        }
        i += 1
    }
    return hits
}
EOF
}
gen_loop_index() {        # loop + array indexing + accumulate
    local n=$(r 16 3) k=$(r 5 1)
    cat <<EOF
proc main() i32 {
    var a i32[$n] = [0 for z in 0..$n]
    var i = 0
    while i < $n decreasing $n - i {
        a[i] = (i *% $k) % 97
        i += 1
    }
    var acc = 0
    var j = 0
    while j < $n decreasing $n - j {
        acc = acc +% a[j]
        j += 1
    }
    return acc % 251
}
EOF
}
gen_calls() {             # nested calls + returns
    local a=$(r 30 1) b=$(r 30 1)
    cat <<EOF
func addk(x i32, k i32) i32 {
    return x +% k
}
func twice(x i32) i32 {
    return addk(addk(x, x), 0)
}
proc main() i32 {
    return twice($a) % ($b + 7)
}
EOF
}
gen_arith() {             # wrapping / div / mod mix
    local a=$(r 200 3) b=$(r 12 2)
    cat <<EOF
proc main() i32 {
    var x i32 = $a
    var y i32 = $b
    var s = 0
    s = s +% (x / y)
    s = s +% (x % y)
    s = s *% 3
    return s % 211
}
EOF
}
gen_strlit() {            # string literals: STORAGE DURATION of the literal's bytes.
    # The class that hid a real segfault — the new emitter gave a MUTABLE binding a pointer
    # to a read-only C string literal, so the first write faulted. Mutation and escape pull
    # in opposite directions (writable wants a copy, escaping wants static), so generate
    # both: a mutated local, and a literal read through a call.
    local k=$(r 4) c=$(( $(r 60) + 65 ))
    case $k in
      0) cat <<EOF
proc main() i32 {
    var s = "hello"
    s[0] = $c
    return s[0] as i32
}
EOF
      ;;
      1) cat <<EOF
proc main() i32 {
    var s = "abcdef"
    s[1] = $c
    s[2] = s[1]
    return (s[2] as i32) % 251
}
EOF
      ;;
      2) cat <<EOF
func first(s u8[:0]) i32 { if s.len > 0 { return s[0] as i32 } return 0 }
proc main() i32 {
    t = "world"
    return first(t) % 251
}
EOF
      ;;
      *) cat <<EOF
proc main() i32 {
    var s = "xyz"
    var n = s.len as i32
    s[0] = $c
    return (n *% 7 +% (s[0] as i32)) % 251
}
EOF
      ;;
    esac
}

gen_defer() {             # `defer` ORDER — reverse registration, and it runs BEFORE the
    # return expression is evaluated (so a defer can change what is returned; that is Lain's
    # rule, not Go's). Lowering defer at all is new, and both halves are easy to get wrong.
    local a=$(r 30 1) b=$(r 9 2) k=$(r 3)
    case $k in
      0) cat <<EOF
proc main() i32 {
    var acc = $a
    defer acc = acc +% $b
    defer acc = acc *% 3
    acc = acc +% 1
    return acc % 251
}
EOF
      ;;
      1) cat <<EOF
proc main() i32 {
    var acc = $a
    var i = 0
    defer acc = acc +% 7
    while i < $b decreasing $b - i {
        acc = acc +% i
        i += 1
    }
    return acc % 251
}
EOF
      ;;
      *) cat <<EOF
proc pick(n i32) i32 {
    var acc = n
    defer acc = acc *% 2
    if n > $b {
        return acc % 251
    }
    acc = acc +% $a
    return acc % 251
}
proc main() i32 { return pick($a) }
EOF
      ;;
    esac
}

gen_enum() {              # SUM TYPES: variant construction + `case` on the tag + payload
    # reads. Exercises all three payload shapes (multi-field, single, none), both the local
    # and the by-reference-parameter scrutinee (the latter was uncompilable C in the OLD
    # backend until this generator's test found it), and arm selection order.
    local a=$(r 40 1) b=$(r 30 1) k=$(r 3)
    case $k in
      0) cat <<EOF
type Sh { Circle { radius i32 }
 Rect { w i32, h i32 }
 Dot }
func val(s Sh) i32 {
    case s {
        Circle(r): return r *% 3
        Rect(w, h): return w *% h
        Dot: return 5
    }
    return -1
}
proc main() i32 {
    var c = Sh.Circle($a)
    var q = Sh.Rect($a, $b)
    var d = Sh.Dot
    return (val(c) +% val(q) +% val(d)) % 251
}
EOF
      ;;
      1) cat <<EOF
type St { On { level i32 }
 Off }
proc main() i32 {
    var acc = 0
    var i = 0
    while i < 6 decreasing 6 - i {
        var s = St.Off
        if i % 2 == 0 { s = St.On(i +% $a) }
        case s {
            On(l): acc = acc +% l
            Off: acc = acc +% 1
        }
        i += 1
    }
    return acc % 251
}
EOF
      ;;
      *) cat <<EOF
type P { A { x i32, y i32 }
 B { z i32 }
 C }
func pick(p P) i32 {
    case p {
        A(x, y): return x -% y
        B(z): return z
        C: return $b
    }
    return 0
}
proc main() i32 {
    var t = P.A($a, $b)
    var u = P.B($a)
    var v = P.C
    var s = pick(t) +% pick(u) +% pick(v)
    if s > 0 { return s % 251 }
    return (0 -% s) % 251
}
EOF
      ;;
    esac
}

gen_rvalue_field() {      # a field/element read off a value with NO ADDRESS — `mk(i).x`.
    # An rvalue must be materialised into a temporary before its field can be addressed;
    # falling back to a placeholder slot read the field out of UNINITIALISED memory, so the
    # program silently computed nonsense instead of failing.
    local a=$(r 20 1) k=$(r 2)
    case $k in
      0) cat <<EOF
type P {
    x i32
    y i32
}
func mk(n i32) P { return P(n, n +% $a) }
proc main() i32 {
    var acc = 0
    var i = 0
    while i < 4 decreasing 4 - i {
        acc = acc +% mk(i).x +% mk(i).y
        i += 1
    }
    return acc % 251
}
EOF
      ;;
      *) cat <<EOF
type P {
    x i32
}
type Q {
    inner P
    n i32
}
func mq(n i32) Q { return Q(P(n *% 2), n +% $a) }
proc main() i32 {
    var s = mq($a).inner.x +% mq($a).n
    return s % 251
}
EOF
      ;;
    esac
}

gen_array_param() {       # a fixed ARRAY passed to a procedure — the decayed-parameter class.
    # Every array in the generators above is a LOCAL, so nothing exercised what a fixed array
    # becomes when it crosses a call boundary. The new emitter typed it `void*`, and GCC's
    # byte-arithmetic extension made `a[i]` advance by BYTES rather than elements: every read
    # past the first landed misaligned inside the array. Invisible to a differential that
    # never passes one.
    local n=$(r 12 4) k=$(r 7 1)
    cat <<EOF
func sum_at(a i32[$n], i usize) i32 {
    if i < $n {
        return a[i]
    }
    return 0
}
proc fill(var a i32[$n], k i32) {
    var i = 0
    while i < $n decreasing $n - i {
        a[i] = (i as i32 *% k) % 97
        i += 1
    }
}
proc main() i32 {
    var a i32[$n] = [0 for z in 0..$n]
    fill(var a, $k)
    var acc = 0
    var j = 0
    while j < $n decreasing $n - j {
        acc = acc +% sum_at(a, j)
        j += 1
    }
    return acc % 251
}
EOF
}

gen_effectful_result() {  # ★ the ANNOTATION class: a value-returning function with an
    # OBSERVABLE effect whose result is then DISCARDED. `pure`/`const` license gcc to delete
    # such a call outright, so if the effect row is wrong the print (or the abort) vanishes at
    # -O3 and survives at -O0. There is no other shape that can falsify those two attributes,
    # and until this existed the differential compiled at -O0 only — it could not have seen it.
    local k=$(r 3) n=$(( $(r 3) + 2 ))
    case $k in
      0) cat <<EOF
extern proc libc_printf(fmt *u8, ...) i32
proc noisy(x i32) i32 {
    libc_printf("t%d\n", x)
    return x +% 1
}
proc main() i32 {
    var i i32 = 0
    while i < $n {
        var t i32 = noisy(i)
        i = i + 1
    }
    return 0
}
EOF
      ;;
      1) cat <<EOF
extern proc libc_printf(fmt *u8, ...) i32
func quiet(x i32) i32 { return x *% 3 }
proc noisy(x i32) i32 {
    libc_printf("u%d\n", quiet(x))
    return x
}
proc main() i32 {
    var a i32 = noisy($n)
    var b i32 = quiet($n)
    var c i32 = noisy(b)
    return 0
}
EOF
      ;;
      *) cat <<EOF
extern proc libc_printf(fmt *u8, ...) i32
proc tally(var acc i32, x i32) i32 {
    acc = acc +% x
    libc_printf("v%d\n", acc)
    return acc
}
proc main() i32 {
    var s i32 = 0
    var i i32 = 0
    while i < $n {
        var t i32 = tally(var s, i)
        i = i + 1
    }
    libc_printf("s%d\n", s)
    return 0
}
EOF
      ;;
    esac
}

gen_rawptr_alias() {      # ★ the RESTRICT class. Two RAW pointers to the SAME object, written
    # through one and read through the other. `restrict` says that cannot happen, and Lain
    # promises nothing of the kind for `*T` in `unsafe` — only for borrows, where the borrow
    # checker has refused the aliasing program. Emit restrict here and the same binary prints
    # 11 at -O0 and 1 at -O3, which is what makes this the shape that can falsify the rule.
    local k=$(r 2) inc=$(( $(r 9) + 1 ))
    if [ "$k" = "0" ]; then cat <<EOF
extern proc libc_printf(fmt *u8, ...) i32
proc bump2(p *var i32, q *var i32) i32 {
    unsafe {
        *p = *p + 1
        *q = *q + $inc
        return *p
    }
}
proc main() i32 {
    var x i32 = 0
    var r i32 = bump2(&x, &x)
    libc_printf("%d\n", r)
    return 0
}
EOF
    else cat <<EOF
extern proc libc_printf(fmt *u8, ...) i32
proc mix(p *var i32, q *var i32) i32 {
    unsafe {
        *p = $inc
        var a i32 = *q
        *p = *p +% a
        return *q
    }
}
proc main() i32 {
    var x i32 = 7
    var y i32 = 3
    var s i32 = mix(&x, &x) +% mix(&x, &y)
    libc_printf("%d\n", s)
    return 0
}
EOF
    fi
}

GENS=(gen_mutparam_scalar gen_mutparam_struct gen_shortcircuit gen_loop_index gen_calls gen_arith gen_strlit gen_defer gen_enum gen_rvalue_field gen_array_param gen_effectful_result gen_effectful_result gen_rawptr_alias gen_rawptr_alias)

run_pipeline() {  # $1=c-file $2=bin [$3=opt level] -> prints "<exit>|<stdout>"
    "$CC" -o "$2" "$1" $DEFS -w "${3:--O0}" 2>/dev/null || { echo "BUILDFAIL"; return; }
    local out; out=$("$2" 2>/dev/null); local rc=$?
    echo "$rc|$out"
}

mis=0 brokenc=0 oldfail=0 ok=0
for ((t=0;t<N;t++)); do
    g=${GENS[$((RANDOM % ${#GENS[@]}))]}
    src="$TMP/p.ln"; $g > "$src"
    # oracle: the OLD pipeline (what the corpus validates)
    "$LAIN" "$src" -o "$TMP/old.c" >/dev/null 2>&1 || { oldfail=$((oldfail+1)); continue; }
    old=$(run_pipeline "$TMP/old.c" "$TMP/old.bin")
    [ "$old" = "BUILDFAIL" ] && { oldfail=$((oldfail+1)); continue; }
    # subject: the NEW IR pipeline
    if ! "$LOWERDRV" "$src" --emit-c > "$TMP/new.c" 2>/dev/null; then
        brokenc=$((brokenc+1)); cp "$src" "$TMP/keep_lower_$t.ln"; continue
    fi
    new=$(run_pipeline "$TMP/new.c" "$TMP/new.bin")
    if [ "$new" = "BUILDFAIL" ]; then
        brokenc=$((brokenc+1)); echo "### BROKEN-C ($g)"; cat "$src"; continue
    fi
    if [ "$old" != "$new" ]; then
        mis=$((mis+1))
        echo "### MISCOMPILE ($g)  old=[$old]  new=[$new]"; cat "$src"
        continue
    fi
    # ★ THE ANNOTATION DIFFERENTIAL. The new backend now emits `restrict`, `pure` and `const`
    # from the analyses, and those are PROMISES the optimizer acts on: a wrong `const` lets gcc
    # delete a call, a wrong `restrict` lets it skip an overlap test. Neither shows at -O0 —
    # which is the only level this harness used to compile at, so it could not have caught the
    # very class the annotations introduce. Same program, -O0 vs -O3: any difference is the
    # optimizer acting on something we promised and should not have.
    newO3=$(run_pipeline "$TMP/new.c" "$TMP/new3.bin" -O3)
    if [ "$newO3" = "BUILDFAIL" ]; then
        brokenc=$((brokenc+1)); echo "### BROKEN-C at -O3 ($g)"; cat "$src"; continue
    fi
    if [ "$new" != "$newO3" ]; then
        mis=$((mis+1))
        echo "### ANNOTATION MISCOMPILE ($g)  -O0=[$new]  -O3=[$newO3]"; cat "$src"
        continue
    fi
    ok=$((ok+1))
done
echo "=================================================================="
echo "fuzz_ir_codegen: $N programs   agree=$ok  old-rejected=$oldfail"
echo "  bugs:  MISCOMPILE=$mis  broken-C(new)=$brokenc"
echo "=================================================================="
[ $mis -eq 0 ] && [ $brokenc -eq 0 ] && exit 0 || exit 1
