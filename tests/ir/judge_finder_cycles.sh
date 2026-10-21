#!/usr/bin/env bash
# judge_finder_cycles.sh — the finder's recursion CYCLES, ranked jointly (fd_rank_cycle), and loops
# that cannot run (the constant measure), as --judge=authority decides them:
#   1. ping/pong (n - 1 both ways): one measure per function, every call strict, accepted;
#   2. spec 12's a -> b(n - 1), b -> c(n), c -> a(n - 1): accepted (a measure per member, the
#      indicator's constant telling them apart, so even the call that keeps n falls);
#   3. a cycle that does not end (b passes n back unchanged to a, which passes it on): refused, E011,
#      and one whose only unlowered call is a self-call: E011, never a list the judge refuses;
#   4. `while false {}` and `while 0 != 0 { x = x + 1 }`: no back edge can run, the constant measure
#      falls on each, accepted (SV-COMP's WhileFalse); the default mode refuses both, E011.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
T=$(mktemp -d); trap 'rm -rf "${T:?}"' EXIT
cd "$T" || exit 1
A="--judge=authority"
ok() { out=$("$ROOT/lain" "$1" $A -o /dev/null 2>&1) || { echo "$2: refused under authority: $out"; exit 1; }; }
cat > pp.ln <<'LN'
func ping(n u32) u32 {
    if n == 0 { return 0 }
    return pong(n - 1)
}
func pong(n u32) u32 {
    if n == 0 { return 1 }
    return ping(n - 1)
}
func main() i32 {
    a = ping(7)
    return 0
}
LN
ok pp.ln 1
cat > cut.ln <<'LN'
func a(n u32) u32 {
    if n == 0 { return 0 }
    return b(n - 1)
}
func b(n u32) u32 {
    return c(n)
}
func c(n u32) u32 {
    if n == 0 { return 0 }
    return a(n - 1)
}
func main() i32 {
    x = a(5)
    return 0
}
LN
ok cut.ln 2
cat > hang.ln <<'LN'
func a(n u32) u32 {
    if n == 0 { return 0 }
    return b(n)
}
func b(n u32) u32 {
    return a(n)
}
func main() i32 {
    x = a(5)
    return 0
}
LN
out=$("$ROOT/lain" hang.ln $A -o /dev/null 2>&1); rc=$?
[ $rc -eq 1 ] && echo "$out" | grep -q '^\[E011\]' || { echo "3: a cycle that never ends not refused with E011 ($rc): $out"; exit 1; }
# 3b. a cycle whose only unlowered call is a SELF-call (`a(n)`): the finder must not list it (a list
# that closes a cycle is refused by the judge, an internal error under authority), so E011, exit 1
cat > selfhang.ln <<'LN'
func a(n u32) u32 {
    if n == 0 { return 0 }
    y = a(n)
    return b(n - 1)
}
func b(n u32) u32 {
    return a(n)
}
func main() i32 {
    x = a(5)
    return 0
}
LN
out=$("$ROOT/lain" selfhang.ln $A -o /dev/null 2>&1); rc=$?
[ $rc -eq 1 ] && echo "$out" | grep -q '^\[E011\]' || { echo "3b: a self-call left unlowered not refused with E011 ($rc): $out"; exit 1; }
printf 'func a(n i32) i32 {\n    var x = n\n    while false {\n    }\n    while 0 != 0 {\n        x = x + 1\n    }\n    return x\n}\nfunc main() i32 { return a(3) }\n' > dead.ln
ok dead.ln 4
"$ROOT/lain" dead.ln --judge=vra -o /dev/null > /dev/null 2>&1 && { echo "4: the range analysis now accepts dead.ln: the probe has lost its point"; exit 1; }
exit 0
