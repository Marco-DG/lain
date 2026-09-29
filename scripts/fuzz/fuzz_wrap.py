#!/usr/bin/env python3
"""Generator for fuzz_wrap.sh — the explicit-policy arithmetic operators, against a Python model.

`+% -% *% /%` (wrap), `+| -| *| /|` (saturate) promise a DEFINED result for every input. Until
2026-09-28 the wrapping ones compiled to C signed overflow (undefined; wrong at -O2), u16 `*%`
overflowed `int` by promotion, and an odd width (`u4 +% 1`) was not wrapped at all — and no
fuzzer ever executed an operator AT the overflow, because every generator kept its values
comfortably inside the type. This one aims at the boundaries on purpose, and the oracle is
exact: Python's integers, reduced the way the operator says.

The same boundary discipline covers every other operator with an explicit overflow POLICY:
checked `+? -? *?` and `as?` (the `else` value on overflow), and the casts `as%` (wrap) and `as|`
(clamp) between random source and target widths. Its first run found `as|` compiled as a plain
truncation (300 as| u8 = 44), and a wrapping result modelled over ℤ by the range analysis, which
read an in-range check on it as always failing and so proved the code after it dead.

Each program checks K results and returns the index of the first wrong one (0 = all right).
Every check is its own function: the range analysis is cubic in the live variables of ONE
function, and a dozen checks in `main` took ~10 s per program — a dozen small functions, well
under one.
"""
import random, sys

def rng_of(bits, signed):
    return (-(1 << (bits - 1)), (1 << (bits - 1)) - 1) if signed else (0, (1 << bits) - 1)

def wrap(v, bits, signed):
    v &= (1 << bits) - 1
    if signed and v >= (1 << (bits - 1)): v -= (1 << bits)
    return v

def sat(v, bits, signed):
    lo, hi = rng_of(bits, signed)
    return lo if v < lo else hi if v > hi else v

def cdiv(a, b):                       # C truncation toward zero
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b >= 0) else -q

def lit(v, bits, signed, tname):
    """A Lain expression of type `tname` with value v (literals above i64::MAX are not accepted)."""
    if v == -(1 << 63): return "(-9223372036854775807 - 1)"
    if v > (1 << 63) - 1:             # u64 above i64::MAX: count down from the all-ones value
        return f"(mx64 -% {((1 << 64) - 1) - v})"
    return str(v)

WIDTHS = [8, 16, 32, 64, 8, 16, 32, 64, 3, 5, 7, 12, 33, 63]

def pick_type(rng):
    bits = rng.choice(WIDTHS); signed = rng.random() < 0.6
    return bits, signed, f"{'i' if signed else 'u'}{bits}"

def picker(rng, bits, signed):
    lo, hi = rng_of(bits, signed)
    return lambda: rng.choice([lo, hi, lo + 1, hi - 1, 0, 1, rng.randint(lo, hi), rng.randint(lo, hi),
                               (-1 if signed else 2)])

def gen(rng):
    K = rng.randint(6, 12)
    out = []
    for k in range(1, K + 1):
        out += [f"func c{k}() i32 {{", "    var z64 u64 = 0", "    var mx64 = z64 -% 1"]
        bits, signed, t = pick_type(rng)
        lo, hi = rng_of(bits, signed)
        pick = picker(rng, bits, signed)
        kind = rng.choice(["arith", "arith", "checked", "cast"])
        if kind == "cast":                        # a source of one type, a target of another
            sb, ss, st = pick_type(rng)
            a = picker(rng, sb, ss)()
            tier = rng.choice(["as%", "as|", "as?"])
            if tier == "as%":   e, expr = wrap(a, bits, signed), f"a{k} as% {t}"
            elif tier == "as|": e, expr = sat(a, bits, signed),  f"a{k} as| {t}"
            else:
                fb = pick()
                e = a if lo <= a <= hi else fb
                expr = f"a{k} as? {t} else {lit(fb, bits, signed, t)}"
            out.append(f"    var a{k} {st} = {lit(a, sb, ss, st)}")
            out.append(f"    var r{k} = {expr}")
        else:
            if kind == "checked":
                op = rng.choice(["+?", "-?", "*?"])
            else:
                op = rng.choice(["+%", "-%", "*%", "+|", "-|", "*|", "/%", "/|"])
            a, b = pick(), pick()
            if op in ("/%", "/|"):
                if b == 0: b = -1 if signed else 1
                if signed and rng.random() < 0.3: a, b = lo, -1      # the case the operator exists for
                # A u64 above i64::MAX has no representation in the range analysis (it is i64-based),
                # so such a divisor cannot be shown non-zero: a known, QUEUED limitation (plan 7H.0),
                # not something this fuzzer should keep re-reporting. The dividend may still be one.
                if not signed and bits == 64 and b > (1 << 63) - 1: b = (1 << 63) - 1
            exact = {"+": a + b, "-": a - b, "*": a * b}.get(op[0])
            if op == "+%" or op == "-%" or op == "*%": e = wrap(exact, bits, signed)
            elif op in ("+|", "-|", "*|"):             e = sat(exact, bits, signed)
            elif op == "/%":                           e = wrap(cdiv(a, b), bits, signed)
            elif op == "/|":                           e = sat(cdiv(a, b), bits, signed)
            else:
                fb = pick()
                e = exact if lo <= exact <= hi else fb
            out.append(f"    var a{k} {t} = {lit(a, bits, signed, t)}")
            out.append(f"    var b{k} {t} = {lit(b, bits, signed, t)}")
            if kind == "checked":
                out.append(f"    var r{k} = a{k} {op} b{k} else {lit(fb, bits, signed, t)}")
            else:
                out.append(f"    var r{k} = a{k} {op} b{k}")
        out.append(f"    var e{k} {t} = {lit(e, bits, signed, t)}")
        out.append(f"    if r{k} != e{k} {{ return {k} }}")
        out += ["    return 0", "}"]
    out.append("func main() i32 {")
    for k in range(1, K + 1):
        out.append(f"    if c{k}() != 0 {{ return {k} }}")
    out += ["    return 0", "}"]
    return "\n".join(out) + "\n"

if __name__ == "__main__":
    rng = random.Random(int(sys.argv[1]) if len(sys.argv) > 1 else None)
    sys.stdout.write(gen(rng))
