#!/usr/bin/env bash
# --check-certificate (C.3a, local/internal/design/certificates.md): the analysis CHECKS a certificate
# instead of searching. Header states come from the file and every other state is rebuilt from them in
# one pass, with entailment on every edge into a loop header; each side fact the file states is
# checked, not trusted. A certificate that does not check is an internal error (exit 70), because a
# certificate this compiler wrote that it cannot check means the search and the check disagree.
# Here: emit then check gives the same C; then ONE mutation per kind of fact, each refused with the
# fact named. An accumulator bound that is not exactly the states' is not refused itself but NOT USED
# (one accumulator legitimately has several facts, one per query point), so what it carried fails:
# here the return range the certificate states for `total`, refused in turn.
# scripts/gates/certificate_gate.sh runs emit-then-check over the whole corpus.
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
    return pong(n - 1, 1)
}
func pong(m u32, k u32) u32 {
    if m == 0 { return k }
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
"$LAIN" "$D/t.ln" -o "$D/a.c" --emit-certificate "$D/t.cert" > "$D/out" 2>&1 || { echo "lain refused the program"; cat "$D/out"; exit 1; }
"$LAIN" "$D/t.ln" -o "$D/b.c" --check-certificate "$D/t.cert" > "$D/out" 2>&1 || { echo "its own certificate does not check"; cat "$D/out"; exit 1; }
cmp -s "$D/a.c" "$D/b.c" || { echo "checking the certificate changed the C"; exit 1; }
fail=0
mut() {   # what, sed program, the message (or code) the checked compile must print, its status
    sed "$2" "$D/t.cert" > "$D/m.cert"
    cmp -s "$D/t.cert" "$D/m.cert" && { echo "the mutation did not apply: $1"; fail=1; return; }
    out=$("$LAIN" "$D/t.ln" -o "$D/m.c" --check-certificate "$D/m.cert" 2>&1); rc=$?
    [ $rc -eq $4 ] && echo "$out" | grep -qF "$3" || { echo "not refused as expected ($1): rc=$rc"; echo "$out" | head -3; fail=1; }
}
mut "a header state, tightened"   '/^certificate t_total$/,/^end$/ s/^\(    %[0-9]*:k in \)\[0, 1000\]/\1[0, 999]/' "does not entail the header" 70
mut "an element range, narrowed"  's/^\(  elem %[0-9]*:a in \)\[10, 40\]/\1[10, 30]/'                        "a store puts [10, 40]" 70
mut "a return range, narrowed"    '/^certificate t_nib$/,/^end$/ s/^  ret in \[0, 15\]$/  ret in [0, 14]/'        "a return is in [0, 15]" 70
mut "a call-site range"           's/\(callee t_count bind 0=\[5, 5\] ret in \)\[5, 5\]/\1[6, 6]/'                 "does not establish it" 70
mut "a call-site binding"         's/callee t_nib bind 0=\[200, 200\]/callee t_nib bind 0=[201, 201]/'             "which the call site does not state" 70
mut "a loop measure"              '/^certificate t_total$/,/^end$/ s/^\(  measure loop bb[0-9]* \)rises /\1falls /' "for the loop at" 70
mut "a recursion measure"         '/^certificate t_fact$/,/^end$/ s/^  measure recursion param %0:n$/  measure recursion param %1/' "for the recursion does not decrease" 70
mut "a mutual cycle's position"   '/^certificate t_ping$/,/^end$/ s/^  measure mutual t_ping 0 t_pong 0$/  measure mutual t_ping 0 t_pong 1/' "the mutual measure it states does not rank the cycle" 70
mut "a mutual cycle's functions"  '/^certificate t_ping$/,/^end$/ s/^  measure mutual t_ping 0 t_pong 0$/  measure mutual t_ping 0 t_spin 0/' "the mutual measure it states does not rank the cycle" 70
mut "a missing certificate"       '/^certificate t_nib$/,/^end$/d'                                                  "there is no certificate for it" 70
mut "a header at no loop"         '/^certificate t_nib$/a\  header bb0'                                              "which is not a loop header" 70
mut "an accumulator delta"        '/^certificate t_total$/,/^end$/ s/delta \[7, 7\]/delta [6, 7]/'                 "it states returns in [1000, 7000]" 70
# A WRITTEN measure is judged after the certificate's: the certificate states the measure the rule
# found (a fact about the loop), and the comparison with the `decreasing` the programmer wrote (I.74)
# runs on it under both modes. Stating the final verdict instead would turn E082 into E011.
cat > "$D/w.ln" <<'LN'
func f(n u32) u32 {
    var i u32 = 0
    while i < n decreasing i {
        i = i + 1
    }
    return i
}
func main() i32 {
    return f(3) as i32
}
LN
s1=$("$LAIN" "$D/w.ln" -o "$D/w1.c" --emit-certificate "$D/w.cert" 2>&1 | grep -m1 '^\[E')
s2=$("$LAIN" "$D/w.ln" -o "$D/w2.c" --check-certificate "$D/w.cert" 2>&1 | grep -m1 '^\[E')
case "$s1" in "[E082]"*"not by the \`decreasing\` measure written"*) ;; *) echo "searched: not E082 ($s1)"; fail=1;; esac
[ "$s1" = "$s2" ] || { echo "a mismatched decreasing: searched '$s1', checked '$s2'"; fail=1; }
# A loop whose test does not end it (I.85): under `or`, the header's else branch goes to the test of
# `b`, inside the loop, so `i < 10` bounds nothing and the loop never ends when b holds. The engine
# states no measure for it, and a certificate CLAIMING one must be refused, because the check runs the
# same rule and the rule finds no exit test there. The claim is tried for every candidate counter id
# against the bound 10, so it is refused whatever lowering numbers it; before I.85 the true pair checked.
cat > "$D/o.ln" <<'LN'
func spin(b bool) i32 {
    var i i32 = 0
    while i < 10 or b {
        i = i + 1
    }
    return i
}
func main() i32 {
    return spin(false)
}
LN
out=$("$LAIN" "$D/o.ln" -o "$D/o.c" --emit-certificate "$D/o.cert" 2>&1)
echo "$out" | grep -q '^\[E011\]' || { echo "the \`or\` loop is not refused (E011)"; fail=1; }
grep -A20 '^certificate o_spin$' "$D/o.cert" | grep -qE '^  measure loop bb[0-9]+ none$' || { echo "the \`or\` loop states a measure"; fail=1; }
bnd=$(sed -n '/^certificate o_spin$/,/^end$/p' "$D/o.cert" | grep -m1 -oE '^  const %[0-9]+ = 10$' | grep -oE '%[0-9]+')
[ -n "$bnd" ] || { echo "no constant 10 in spin's certificate"; fail=1; }
for a in $(seq 0 20); do
    sed "/^certificate o_spin$/,/^end$/ s/^  measure loop \(bb[0-9]*\) none$/  measure loop \1 rises %$a $bnd/" "$D/o.cert" > "$D/om.cert"
    cmp -s "$D/o.cert" "$D/om.cert" && { echo "the claim did not apply"; fail=1; break; }
    "$LAIN" "$D/o.ln" -o "$D/om.c" --check-certificate "$D/om.cert" > /dev/null 2>&1; rc=$?
    [ $rc -eq 70 ] || { echo "a \`rises %$a $bnd\` claim on the \`or\` header was not refused (rc=$rc)"; fail=1; }
done
exit $fail
