#!/usr/bin/env bash
# judge_for_counter.sh — s22: spec 13 lists "the range of the variable of `for i in a..b`" among the
# facts every guaranteed obligation may use, and spec 12 says a `for` loop ends. The judge used to
# owe both to the finder's claims: with `--no-claims`, `a[i - 2]` in `for i in 2..a.len` was not
# implied (i = 0 at the header) and the loop's termination was `outside`. The generator now states a
# counter's floor at its loop's header (lo <= i, from the store on the loop's one entry edge, every
# write in the loop a checked step up) and, where no ranking is proposed, owes `hi - i` itself.
# Each program below is judged with no claims at all:
# - Documentation's repro (ch3 findings): every record proved;
# - nested, `for j in i..n { s = s + x[j - i] }`: the inner floor is i's value, `j - i` proved;
# - `for i in 1..=n { s = s + 100 / i }`, an inclusive range under n <= 1000 (at u32's top the
#   step past n would overflow, an E086 the program owns): the division and the loop proved;
# - a `while` whose body also doubles i: no floor (a write that is no step up), `i - 2` not implied,
#   and no ranking of its own (outside).
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
D="$(mktemp -d)"; trap 'rm -rf "${D:?}"' EXIT
gcc -std=c99 -O2 -w -o "$D/jvc" "$ROOT/src/tools/judge_vc_driver.c" -I "$ROOT/src" || { echo "the driver does not build"; exit 1; }
cat > "$D/repro.ln" <<'LN'
func f(a u8[]) u8 {
    var s u8 = 0
    for i in 2..a.len {
        s = a[i - 2]
    }
    return s
}
func main() i32 {
    var b u8[4] = [1, 2, 3, 4]
    if f(b) == 2 { return 0 }
    return 1
}
LN
cat > "$D/nested.ln" <<'LN'
func g(x u32[], n usize) u64 {
    var s u64 = 0
    if n > x.len { return 0 }
    for i in 0..n {
        for j in i..n {
            s = s +% x[j - i] as u64
        }
    }
    return s
}
func main() i32 {
    var b u32[3] = [1, 2, 3]
    if g(b, 3) != 10 { return 1 }
    return 0
}
LN
cat > "$D/incl.ln" <<'LN'
func h(n u32) u32 {
    var s u32 = 0
    if n > 1000 { return 0 }
    for i in 1..=n {
        s = s +% 100 / i
    }
    return s
}
func main() i32 {
    if h(4) != 208 { return 1 }
    return 0
}
LN
cat > "$D/doubled.ln" <<'LN'
func k(n u64, m u64) u64 {
    var i u64 = 2
    var s u64 = 0
    while i < n {
        s = i - 2
        if m > 5 { i = i * 2 }
        i = i + 1
    }
    return s
}
func main() i32 {
    if k(10, 1) != 7 { return 1 }
    return 0
}
LN
fail=0
run() { (cd "$D" && "$D/jvc" "$1" --judge --no-claims 2>&1); }
for p in repro nested incl; do
    out=$(run $p.ln)
    bad=$(echo "$out" | grep -E '^v1 ' | grep -vE ' proved ' )
    if [ -n "$bad" ] || ! echo "$out" | grep -q ' rank.fall 0 proved'; then
        echo "FAIL $p: every record proved with no claims, its loops' rank.fall among them"; echo "$out" | grep -E '^v1 |^# judge' | grep -v ' proved '; fail=1
    else echo "ok   $p: $(echo "$out" | grep -E '^# judge' | cut -d' ' -f3)"; fi
done
out=$(run doubled.ln)
if echo "$out" | grep -qE 'narrow.lo 0 not-implied' && echo "$out" | grep -qE 'rank.fall 0 outside'; then
    echo "ok   doubled: i - 2 not implied, the loop outside (i * 2 is no step up)"
else echo "FAIL doubled: i - 2 not implied and the loop outside"; echo "$out" | grep -E '^v1 '; fail=1; fi
[ $fail -eq 0 ] && echo "judge_for_counter: all passed"
exit $fail
