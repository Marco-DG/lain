#!/usr/bin/env bash
# judge_written_cycle.sh — a cycle of functions, each with its `decreasing` measure WRITTEN, is
# judged by spec 12's cycle rule: the measure rises at no call, and the calls where it falls cut
# every cycle. `a -> b(n) -> c(n) -> a(n - 1)`, each `decreasing n`, falls only at c -> a; the judge
# owed rec.fall at every call (no `le` list was proposed for a written measure) and refused it at
# a -> b, while the same program with no measure written was accepted (Documentation, 2026-10-20).
# The finder now checks each call against the written measures, one VC per call, so a cycle of 17
# costs no search; the judge still owes every call. Wrong measures stay refused: one that never
# falls, one that rises at a call, and a cycle that diverges through members with none written.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
T=$(mktemp -d); trap 'rm -rf "${T:?}"' EXIT
LAIN="$ROOT/lain"
# cyc NAME N ARG_LAST [DECL]: N functions f0 -> f1 -> ... -> f(N-1) -> f0, every call passing n but
# the last, which passes ARG_LAST; DECL the measure clause of each
cyc() {
    local name=$1 n=$2 last=$3 decl=${4-decreasing n} i
    : > "$T/$name.ln"
    for ((i = 0; i < n; i++)); do
        local to=$(( (i + 1) % n )) arg=n
        [ $i -eq $((n - 1)) ] && arg=$last
        printf 'func f%d(n u32) u32 %s {\n    if n == 0 { return 0 }\n    return f%d(%s)\n}\n' $i "$decl" $to "$arg" >> "$T/$name.ln"
    done
    printf 'func main() i32 { return 0 }\n' >> "$T/$name.ln"
}
judge() { (cd "$T" && "$LAIN" --judge=authority --dump-measures "$1.ln" -o "$1.c" 2>&1); }

cyc w3 3 'n - 1'
out=$(judge w3) || { echo "a written measure cut at one call is refused:"; echo "$out" | head -4; exit 1; }
echo "$out" | grep -qF ': recursion at line 3: `n` does not rise at any call that may close the cycle, and every cycle of calls lowers it' \
    || { echo "f0's measure, listed at its call, is not printed as such:"; echo "$out" | grep measure; exit 1; }
echo "$out" | grep -qF ': recursion at line 11: `n` decreases at every call that may close the cycle' \
    || { echo "f2's measure, falling at its call, is not printed as such:"; echo "$out" | grep measure; exit 1; }

# off the finder's meter: with its work cap spent at once (judge_vc_driver --finder-work=1), the
# cycle, which the finder reaches last, is still checked call by call and every rec.* is proved:
# rec.le at the two listed calls, rec.fall and rec.bound at the one where n falls (s24: a listed
# call owes no bound)
gcc -std=c99 -O2 -w -o "$T/jvc" "$ROOT/src/tools/judge_vc_driver.c" -I "$ROOT/src" || { echo "judge_vc_driver does not build"; exit 1; }
rec=$(cd "$T" && "$T/jvc" w3.ln --judge --finder-work=1 2>&1 | grep -E '^v1 [^ ]+ [^ ]+ [^ ]+ rec\.')
[ "$(echo "$rec" | grep -c ' proved ')" -eq 4 ] && [ "$(echo "$rec" | wc -l)" -eq 4 ] \
    || { echo "with the finder's meter spent, the written cycle is not proved:"; echo "$rec"; exit 1; }

cyc w17 17 'n - 1'
out=$(judge w17) || { echo "a written cycle of 17 functions is refused:"; echo "$out" | head -4; exit 1; }

cyc nofall 3 n
out=$(judge nofall) && { echo "a written measure that never falls is accepted"; exit 1; }
echo "$out" | grep -q '^\[E082\].*not provably smaller at this call' || { echo "nofall:"; echo "$out" | head -3; exit 1; }

# f0 -> f1(n + 1) -> f2(n + 1) -> f0(n - 1) ends, but the measure written rises at f0 -> f1
cat > "$T/rises.ln" <<'LN'
func f0(n u32) u32 decreasing n {
    if n == 0 or n > 1000 { return 0 }
    return f1(n + 1)
}
func f1(n u32) u32 decreasing n {
    if n == 0 { return 0 }
    return f2(n)
}
func f2(n u32) u32 decreasing n {
    if n < 2 { return 0 }
    return f0(n - 2)
}
func main() i32 { return 0 }
LN
out=$(judge rises) && { echo "a written measure that rises at a call is accepted"; exit 1; }
echo "$out" | grep -q '^\[E082\].*may be larger at this call' || { echo "rises:"; echo "$out" | head -3; exit 1; }

# f0 written, f1 and f2 not: the search fixes f0's row to `n`; a cycle that passes n back diverges
cyc mixed 3 'n - 1'
sed -i '/^func f[12]/s/ decreasing n//' "$T/mixed.ln"
out=$(judge mixed) || { echo "a cycle with one measure written is refused:"; echo "$out" | head -4; exit 1; }
cyc mixdiv 3 n
sed -i '/^func f[12]/s/ decreasing n//' "$T/mixdiv.ln"
out=$(judge mixdiv) && { echo "a diverging cycle with one measure written is accepted"; exit 1; }
echo "$out" | grep -q '^\[E082\]' || { echo "mixdiv:"; echo "$out" | head -3; exit 1; }
echo "judge_written_cycle: ok"
