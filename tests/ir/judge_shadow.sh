#!/usr/bin/env bash
# PE.6: `--judge=shadow` runs the proof engine's judge beside VRA and reports where they disagree,
# and decides nothing: the exit status and the emitted C are what they are without it.
#   1. a program VRA refuses and the judge proves (`i + j < n` guarding a[i + j], the shape the
#      octagon cannot hold, I.193): the same E085 and exit status, and a `judge-only` line;
#   2. an accepted program: the same C byte for byte, exit 0, and a summary line;
#   3. a judge mode this compiler does not have is refused, never read as shadow.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
D="$(mktemp -d)"; trap 'rm -rf "${D:?}"' EXIT
cat > "$D/sum3.ln" <<'LN'
func get(a u8[64], i u32 <= 64, j u32 <= 64, n u32 <= 64) u8 {
    if i + j < n {
        return a[i + j]
    }
    return 0
}
func main() i32 { return 0 }
LN
cat > "$D/ok.ln" <<'LN'
func pick(t u8[4], i u32) u8 {
    if i < 4 { return t[i] }
    return 0
}
func main() i32 {
    var t u8[4] = [1, 2, 3, 4]
    return pick(t, 2) as i32
}
LN
cd "$D" || exit 1
plain=$("$ROOT/lain" sum3.ln -o a.c 2>&1); rp=$?
shadow=$("$ROOT/lain" sum3.ln --judge=shadow -o b.c 2>&1); rs=$?
[ $rp -eq $rs ] || { echo "1: the exit status changed under shadow ($rp, $rs)"; exit 1; }
echo "$shadow" | grep -q '\[E085\]' || { echo "1: E085 lost under shadow: $shadow"; exit 1; }
echo "$shadow" | grep -q '^judge-shadow: judge-only .* Ln 3, Col 16: the judge proves this bounds check, VRA refuses it' \
    || { echo "1: no judge-only line: $shadow"; exit 1; }
"$ROOT/lain" ok.ln -o a.c > /dev/null 2>&1; rp=$?
shadow=$("$ROOT/lain" ok.ln --judge=shadow -o b.c 2>&1); rs=$?
[ $rp -eq 0 ] && [ $rs -eq 0 ] || { echo "2: exit $rp / $rs"; exit 1; }
cmp -s a.c b.c || { echo "2: the emitted C differs under shadow"; exit 1; }
echo "$shadow" | grep -qE '^judge-shadow: [0-9]+ VRA checks: ' || { echo "2: no summary: $shadow"; exit 1; }
# 2b. a counted loop's termination is proved by both: the finder's ranking function (PE.5), owed
# by the judge on the back edge, beside VRA's measure
cat > loop.ln <<'LN'
func f(n i32 >= 0 and <= 100) i32 {
    var i i32 = 0
    var s i32 = 0
    while i < n {
        s = s + 1
        i = i + 1
    }
    return s
}
func main() i32 { return f(5) }
LN
shadow=$("$ROOT/lain" loop.ln --judge=shadow -o d.c 2>&1)
echo "$shadow" | grep -qE "1 loops' termination among them \(1 proved by both, 0 by the judge only\)" \
    || { echo "2b: the loop's termination is not proved by both: $shadow"; exit 1; }
out=$("$ROOT/lain" ok.ln --judge=authority -o c.c 2>&1); rc=$?
[ $rc -ne 0 ] && echo "$out" | grep -q "unknown --judge mode 'authority'" || { echo "3: --judge=authority not refused ($rc): $out"; exit 1; }
[ ! -e c.c ] || { echo "3: C written under an unknown mode"; exit 1; }
exit 0
