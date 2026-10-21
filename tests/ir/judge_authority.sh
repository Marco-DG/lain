#!/usr/bin/env bash
# judge_authority.sh — `--judge=authority` (PE.10): the judge's records decide, VRA's checks are read
# for no verdict. Checked here:
#   1. a program both engines prove: accepted, and its C equals the default mode's byte for byte;
#   2. each kind of obligation the judge refuses reaches its code: an index (E085), an overflow
#      (E086), a divisor (E015), @ctz of 0 (E015, the generator's `nonzero`: skipped by the first
#      draft of the adapter, and `@ctz(0)` compiled), a user assert (E012), a loop that does not end
#      (E011), a written loop measure that rises (E082), a recursion that does not end (E011);
#   3. one diagnostic per place: an index unproved at both ends is one E085;
#   4. a recursion the default mode refuses and the judge proves (`fact` by the finder's measure)
#      is accepted, and a loop VRA refuses that the judge proves (a lexicographic one) too;
#   5. an unknown --judge mode is refused, and so is nothing else.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
T=$(mktemp -d); trap 'rm -rf "${T:?}"' EXIT
cd "$T" || exit 1
A="--judge=authority"
cat > ok.ln <<'LN'
func f(n i32 >= 0 and <= 100) i32 {
    var i i32 = 0
    var s i32 = 0
    while i < n {
        s = s + 1
        i = i + 1
    }
    return s
}
func fact(n i32 >= 0 and <= 10) i32 {
    if n <= 1 { return 1 }
    return fact(n - 1)
}
func main() i32 {
    a = f(5)
    b = fact(3)
    return 0
}
LN
"$ROOT/lain" ok.ln --judge=vra -o a.c > a.err 2>&1 || { echo "1: refused by the range analysis: $(cat a.err)"; exit 1; }
"$ROOT/lain" ok.ln $A -o b.c > b.err 2>&1 || { echo "1: refused under authority: $(cat b.err)"; exit 1; }
cmp -s a.c b.c || { echo "1: the C differs under authority"; diff a.c b.c | head; exit 1; }
want() {   # want FILE CODE [TEXT]: refused under authority with CODE (and TEXT), exit 1
    out=$("$ROOT/lain" "$1" $A -o /dev/null 2>&1); rc=$?
    [ $rc -eq 1 ] || { echo "2: $1 exit $rc, not 1: $out"; exit 1; }
    echo "$out" | grep -q "^\[$2\]" || { echo "2: $1 has no $2: $out"; exit 1; }
    [ -z "${3:-}" ] || echo "$out" | grep -qF "$3" || { echo "2: $1 lacks '$3': $out"; exit 1; }
}
printf 'func g(a u8[4], i usize) u8 { return a[i] }\nfunc main() i32 { var a u8[4] = [1, 2, 3, 4]\n    return g(a, 2) as i32 }\n' > idx.ln
want idx.ln E085
n=$("$ROOT/lain" idx.ln $A -o /dev/null 2>&1 | grep -c '^\[E085\]'); [ "$n" = 1 ] || { echo "3: $n E085, not 1"; exit 1; }
printf 'func g(a i32, b i32) i32 { return a + b }\nfunc main() i32 { return g(1, 2) }\n' > ovf.ln;  want ovf.ln E086
printf 'func g(a i32, b i32) i32 { return a / b }\nfunc main() i32 { return g(4, 2) }\n' > div.ln;  want div.ln E015
printf 'func tz(x u32) u32 { return @ctz(x) }\nfunc main() i32 { return tz(8) as i32 }\n' > ctz.ln; want ctz.ln E015 "@ctz/@clz"
printf 'func g(a i32) i32 {\n    assert a > 0\n    return a\n}\nfunc main() i32 { return g(1) }\n' > asr.ln; want asr.ln E012
printf 'func g(n i32) i32 {\n    var i = 0\n    while i < n {\n        i = i + 0\n    }\n    return i\n}\nfunc main() i32 { return g(4) }\n' > lp.ln; want lp.ln E011
printf 'func g(n u32) u32 {\n    var i u32 = 0\n    while i < n decreasing i {\n        i = i + 1\n    }\n    return i\n}\nfunc main() i32 { return g(4) as i32 }\n' > ms.ln; want ms.ln E082
printf 'func s(n i32) i32 {\n    if n <= 0 { return 0 }\n    a = s(n - 1)\n    return s(n)\n}\nfunc main() i32 { return s(3) }\n' > rc.ln; want rc.ln E011
# a recursion through a pointer with two targets and no measure: one `outside` per call (two of one
# ID, one per target, were an internal error, exit 70, once IDs were checked unique)
cat > pr.ln <<'LN'
func a(x i32) i32 {
    if x <= 0 { return 0 }
    var p *func(i32) i32 = b
    return p(x)
}
func b(x i32) i32 {
    if x <= 0 { return 0 }
    var p *func(i32) i32 = a
    return p(x)
}
func main() i32 { return a(3) }
LN
want pr.ln E011
cat > lex.ln <<'LN'
func f(n i32 >= 0 and <= 100) i32 {
    var i = 0
    var x = 0
    while i < n {
        if x > 5 {
            i = i + 1
        } else {
            x = x + 1
        }
    }
    return i
}
func main() i32 { return f(10) }
LN
"$ROOT/lain" lex.ln --judge=vra -o /dev/null > /dev/null 2>&1 && { echo "4: the range analysis now proves lex.ln: the probe has lost its point"; exit 1; }
"$ROOT/lain" lex.ln $A -o c.c > c.err 2>&1 || { echo "4: a lexicographic loop refused under authority: $(cat c.err)"; exit 1; }
out=$("$ROOT/lain" ok.ln --judge=bogus -o d.c 2>&1); rc=$?
[ $rc -ne 0 ] && echo "$out" | grep -q "unknown --judge mode 'bogus'" || { echo "5: --judge=bogus not refused ($rc): $out"; exit 1; }
[ ! -e d.c ] || { echo "5: C written under an unknown mode"; exit 1; }
exit 0
