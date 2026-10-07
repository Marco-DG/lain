#!/usr/bin/env bash
# I.133: floating point is never contracted (spec 12). With a = 1 + 2^-27, b = 1 - 2^-27, c = -1,
# a*b + c is 0 when the multiply rounds before the add and -2^-54 when they fuse. The operands come
# from atof, so the C compiler cannot fold them. GNU C's default is -ffp-contract=fast: before this,
# `gcc -O2 -mfma` and `-O3 -march=native` fused (exit 1) where `gcc -O2` did not. Each build must
# now give 0. Only a module that computes in floating point carries the pragmas.
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
LAIN="$ROOT/lain"
D="$(mktemp -d)"; trap 'rm -rf "$D"' EXIT
grep -qw fma /proc/cpuinfo 2>/dev/null || { echo "SKIP: this CPU has no FMA, so nothing could fuse"; exit 0; }
fail=0
cat > "$D/fma.ln" <<'LN'
extern func atof(s *u8) f64 effects io

func mul_add(a f64, b f64, c f64) f64 {
    return a * b + c
}

func main() i32 effects io {
    a = atof("1.000000007450580596923828125")
    b = atof("0.999999992549419403076171875")
    c = atof("-1.0")
    r = mul_add(a, b, c)
    if r == 0.0 {
        return 0
    }
    return 1
}
LN
"$LAIN" "$D/fma.ln" -o "$D/fma.c" > "$D/out" 2>&1 || { echo "lain refused it"; cat "$D/out"; exit 1; }
for flags in "-O2" "-O2 -mfma" "-O3 -march=native" "-O2 -mfma -std=gnu11"; do
    gcc $flags -w -o "$D/fma" "$D/fma.c" -lm || { echo "gcc $flags: the C does not compile"; fail=1; continue; }
    "$D/fma"; rc=$?
    [ $rc -eq 0 ] || { echo "gcc $flags: a*b + c was fused (exit $rc)"; fail=1; }
done
if command -v clang >/dev/null 2>&1; then           # clang's own default honours the pragma
    clang -O2 -mfma -w -o "$D/fmac" "$D/fma.c" -lm && { "$D/fmac" || { echo "clang -O2 -mfma: fused"; fail=1; }; }
fi
printf 'func main() i32 {\n    return 0\n}\n' > "$D/int.ln"
"$LAIN" "$D/int.ln" -o "$D/int.c" > /dev/null 2>&1
grep -q 'FP_CONTRACT\|fp-contract' "$D/int.c" && { echo "a module with no floating point carries the pragmas"; fail=1; }
grep -q 'fp-contract=off' "$D/fma.c" || { echo "the floating-point module has no pragma"; fail=1; }
exit $fail
