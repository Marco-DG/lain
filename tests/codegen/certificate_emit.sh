#!/usr/bin/env bash
# --emit-certificate (C.2, local/internal/design/certificates.md): what the range analysis found,
# per function, for a checker that does not search. Each section is checked on a program that
# needs it, by pattern (value ids move with lowering; the facts must not):
#   the constant table; a loop header's closed state; the loop measure (counter rises toward n);
#   the self-recursion measure (a parameter); the mutual-recursion cycle's positions, from both
#   ends (the rule SEARCHES the positions, so they are a found fact); element ranges per array;
#   return ranges; an accumulator bound (1000 trips of +7); and a call-site return range with the
#   callee's certificate under the constant argument NESTED (count(5): n in [5, 5], ret [5, 5]).
# Then the round trip: parse and re-print must reproduce the file byte for byte, because the
# checker (C.3) reads what the compiler writes. And the parser refuses malformed text with a line
# number rather than guessing.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
LAIN="$ROOT/lain"
D="$(mktemp -d)"; trap 'rm -rf "$D"' EXIT
cat > "$D/t.ln" <<'LN'
func count(n i32) i32 {
    var i = 0
    while i < n {
        i = i + 1
    }
    return i
}
func nib(c u8) u8 {
    return c & 15
}
func fact(n u32) u32 {
    if n <= 1 { return 1 }
    if n > 10 { return 1 }
    return n *% fact(n - 1)
}
func ping(n u32) u32 {
    if n == 0 { return 0 }
    return pong(n - 1)
}
func pong(m u32) u32 {
    if m == 0 { return 1 }
    return ping(m)
}
func spin(n i32) i32 effects diverge {
    var j = 0
    while j < n {
        j = j + 1
    }
    return j
}
func total() i32 {
    var s i32 = 0
    var k = 0
    while k < 1000 {
        s = s + 7
        k = k + 1
    }
    return s
}
func main() i32 {
    var a = [10, 20, 30, 40]
    t = a[0] + a[3]
    lut = [1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16]
    v = lut[nib(200)]
    p = ping(3) % 2
    return count(5) + t + v + total() + p as i32 + (fact(4) % 2) as i32
}
LN
"$LAIN" "$D/t.ln" -o "$D/t.c" --emit-certificate "$D/t.cert" > "$D/out" 2>&1 || { echo "lain refused the program"; cat "$D/out"; exit 1; }
fail=0
has() { grep -qE "$1" "$D/t.cert" || { echo "missing: $2 ($1)"; fail=1; }; }
R='%[0-9]+(:[A-Za-z0-9_.]+)?'
has "^certificate t_count$"                                   "a certificate per function"
has "^  const $R = 1$"                                        "the constant table"
has "^  header bb[0-9]+$"                                     "a loop header's state"
has "^    $R - %[0-9]+:n <= "                                 "a relational entry of the header state"
has "^  measure loop bb[0-9]+ rises $R %[0-9]+:n$"            "the counter measure"
has "^  measure recursion param %[0-9]+:n$"                   "the self-recursion measure"
has "^  measure mutual t_ping 0 t_pong 0$"      "the mutual cycle, from ping"
[ "$(grep -c "^  measure mutual t_ping 0 t_pong 0$" "$D/t.cert")" -ge 2 ] || { echo "missing: the mutual cycle, from pong"; fail=1; }
has "^  elem %[0-9]+:a in \[10, 40\]$"                       "an element range"
has "^  ret in \[0, 15\]$"                                    "a return range"
has "^  accum $R header bb[0-9]+ trips 1000 delta \[7, 7\] init \[0, 0\]$" "an accumulator bound"
has "^  callsite $R callee t_count bind 0=\[5, 5\] ret in \[5, 5\]$"       "a call-site return range"
# `effects diverge` waives the loop's obligation, and the effect row still asks whether it ends
grep -A12 '^certificate t_spin$' "$D/t.cert" | grep -qE "^  measure loop bb[0-9]+ rises $R %[0-9]+:n$" \
    || { echo "missing: the measure of a loop whose obligation the row waived"; fail=1; }
has "^        %[0-9]+:n in \[5, 5\]$"                           "the callee's state under the argument, nested"
"$LAIN" --certificate-roundtrip "$D/t.cert" > "$D/t2.cert" || { echo "the certificate does not parse"; fail=1; }
cmp -s "$D/t.cert" "$D/t2.cert" || { echo "parse and re-print differ"; diff "$D/t.cert" "$D/t2.cert" | head; fail=1; }
bad() {   # a malformed certificate is refused, with its line
    printf "$2" > "$D/bad.cert"
    out=$("$LAIN" --certificate-roundtrip "$D/bad.cert" 2>&1); rc=$?
    [ $rc -ne 0 ] && echo "$out" | grep -q "line $3:" || { echo "accepted or unlocated ($1): rc=$rc $out"; fail=1; }
}
bad "a constraint outside a header" 'certificate f\n  %%1 in [0, 1]\nend\n' 2
bad "an unknown line"               'certificate f\n  frobnicate\nend\n' 2
bad "a bad block"                   'certificate f\n  header bbx\nend\n' 2
bad "a callsite without its callee" 'certificate f\n  callsite %%1 callee g bind 0=[1, 1] ret in [1, 1]\nend\n' 3
bad "an unclosed certificate"       'certificate f\n  ret in [0, 1]\n' 2
bad "a bad range"                   'certificate f\n  ret in [0 1]\nend\n' 2
bad "a mutual cycle of one function" 'certificate f\n  measure mutual f 0\nend\n' 2
bad "a mutual position not a number" 'certificate f\n  measure mutual f 0 g x\nend\n' 2
# Every certificate the trust corpus produces round-trips: the parser must keep up with whatever
# the emitter writes, not only with this file's shapes.
cd "$ROOT"
nrt=0
for f in tests/trust/*_pass.ln; do
    "$LAIN" "$f" -o "$D/x.c" --emit-certificate "$D/x.cert" > /dev/null 2>&1 || continue
    "$LAIN" --certificate-roundtrip "$D/x.cert" > "$D/x2.cert" 2>&1 && cmp -s "$D/x.cert" "$D/x2.cert" \
        || { echo "round trip differs: $f"; fail=1; }
    nrt=$((nrt+1))
done
[ $nrt -gt 100 ] || { echo "only $nrt trust programs produced a certificate"; fail=1; }
exit $fail
