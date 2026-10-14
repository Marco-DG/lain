#!/usr/bin/env bash
# judge_finder_nested_accumulator.sh — a running total of NESTED loops, bounded by both counters.
# Inside, `s - 163*j` does not rise and j stays in [0, 20], so one run of the inner loop adds at
# most 3260; the outer counter i steps by 1 a trip, so `s - 3260*i <= 0` at the outer header and
# `s - 163*j - 3260*i <= 0` at the inner one. Neither holds alone (each is the other's back-edge
# hypothesis): the finder proposes both, fixed, and Houdini keeps them together. 20 * 20 * 163 =
# 65200 fits a u16, so --judge=authority accepts; with 164 (65600) it is refused with E086.
# `rows` resets its total each outer trip, so the same guess is wrong there, and one of its bounds
# (the outer header's value on entry, near 2^64 * 240) is past what the generator states: a claim
# it cannot state refuses EVERY claim of the function, so the finder must never propose it.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
T=$(mktemp -d); trap 'rm -rf "${T:?}"' EXIT
cat > "$T/nest.ln" <<'LN'
func carried() u16 {
    var s u16 = 0
    var i usize = 0
    while i < 20 {
        var j usize = 0
        while j < 20 {
            s = s + 163
            j = j + 1
        }
        i = i + 1
    }
    return s
}
func rows() u8 {
    var best u8 = 0
    var r usize = 0
    while r < 20 {
        var s u8 = 0
        var c usize = 0
        while c < 20 {
            s = s + 12
            c = c + 1
        }
        if s > best { best = s }
        r = r + 1
    }
    return best
}
func main() i32 {
    return carried() as i32 - 65200 + rows() as i32 - 240
}
LN
sed 's/s = s + 163/s = s + 164/; s/- 65200/- 65600/' "$T/nest.ln" > "$T/nest_fail.ln"
out=$("$ROOT/lain" --judge=authority "$T/nest.ln" -o "$T/nest.c" 2>&1) \
    || { echo "the nested total is refused under --judge=authority:"; echo "$out" | head -8; exit 1; }
out=$("$ROOT/lain" --judge=authority "$T/nest_fail.ln" -o "$T/nest_fail.c" 2>&1) \
    && { echo "a nested total that passes 65535 is ACCEPTED under --judge=authority"; exit 1; }
echo "$out" | grep -q 'E086' || { echo "the overflowing nested total is refused without E086:"; echo "$out" | head -8; exit 1; }
exit 0
