#!/usr/bin/env python3
"""Generate one Lain program whose arithmetic the engine must prove cannot overflow.

Integer overflow is prove-or-reject in Lain, and the E086 family stops 47 corpus programs in
the OLD front end before the sovereign pass runs — the largest blocked bucket, and the shadow
B1's false proof of 2026-09-09 lived in. With `g_suppress_overflow` open, the new engine's
verdict is visible on exactly those programs, and UBSan is an EXACT oracle for the signed case:
a signed overflow at runtime in a program the engine proved check-free is unsound, full stop.

Every generated function is called with the EXTREMES of its declared parameter ranges, so a
proof that is wrong at the boundary is exercised rather than merely possible.

ORACLES, and exactly what each one can see (corrected 2026-10-01 — the previous claim that
"UBSan is an EXACT oracle for the signed case" was false for every type this generator used):

  UBSan   sees UNDEFINED BEHAVIOUR IN THE EMITTED C, and nothing else. The backend deliberately
          widens a narrow arithmetic op by one step — i8 computes in int16_t, i16 in int32_t,
          i32 in int64_t — so for those three widths the C addition CANNOT overflow, and the only
          narrowing is the implicit conversion on return, which is a conversion and not an
          overflowing operation. UBSan therefore cannot fire for i8/i16/i32 at all. i64 is
          emitted at its own width (`v = (v0 + v1)`), so it is the only width where a false proof
          of an overflow obligation becomes C-level UB that UBSan reports.
  the interpreter (`lain --interpret`) sees EVERY OBLIGATION THE ANALYSIS DISCHARGED, at the line
          that owes it, because it carries abstract values and checks each proof as it is used.
          This is the oracle that covers the narrow widths.
  the output differential (C vs interpreter) sees a wrong VALUE even where neither of the above
          fires: the emitted C wraps deterministically rather than committing UB, while the
          interpreter carried the out-of-range value on, so the two legs printed different
          numbers. Since 892f794 the interpreter also checks a narrowing where the value LANDS
          (ii_land), so most such cases now stop at their line; the differential stays as defence
          in depth.

The lesson behind the correction: a harness must say which oracle covers which case, or a zero
means only that nothing was watching.
"""
import random, sys

TYPES = {"i8": (-128, 127), "i16": (-32768, 32767), "i32": (-2147483648, 2147483647),
         "i64": (-9223372036854775808, 9223372036854775807)}
# i64 is here for the ORACLE, not for variety: see the oracle note above. The backend widens
# an i8/i16/i32 arithmetic op one step (i8 -> int16_t, i16 -> int32_t, i32 -> int64_t), so the
# C addition cannot overflow and UBSan can never fire. i64 is the one width emitted at its own
# width (`v = (v0 + v1)`), so it is the only one where a false proof becomes C-level UB.

def gen(rng):
    ty = rng.choice(list(TYPES))
    lo_t, hi_t = TYPES[ty]
    shape = rng.choice(["binop", "binop", "chain", "accum", "narrow"])
    op = rng.choice(["+", "-", "*"])

    # Parameter refinements: sometimes safe, sometimes right at the edge where the product or
    # sum leaves the type. The engine has to tell them apart; we only check that whatever it
    # PROVES actually holds.
    def rng_bounds():
        span = rng.choice([1, 3, 15, 127, 1000, hi_t // 2 if hi_t > 4 else 1])
        lo = rng.choice([0, -span, 1])
        return lo, max(lo, span)

    a_lo, a_hi = rng_bounds()
    b_lo, b_hi = rng_bounds()
    a_lo, a_hi = max(a_lo, lo_t), min(a_hi, hi_t)
    b_lo, b_hi = max(b_lo, lo_t), min(b_hi, hi_t)

    L = ["extern func libc_printf(fmt *u8, ...) i32 effects io"]

    if shape in ("binop", "chain"):
        expr = f"a {op} b" if shape == "binop" else f"(a {op} b) {op} a"
        L.append(f"func f(a {ty} >= {a_lo} and <= {a_hi}, b {ty} >= {b_lo} and <= {b_hi}) {ty} {{")
        L.append(f"    return {expr}")
        L.append("}")
        calls = [(a_hi, b_hi), (a_lo, b_lo), (a_hi, b_lo), (a_lo, b_hi)]
    elif shape == "narrow":
        # A proven `as` narrowing: the engine must show the value fits the smaller type.
        nty = rng.choice([t for t in TYPES if TYPES[t][1] < hi_t] or ["i8"])
        L.append(f"func f(a {ty} >= {a_lo} and <= {a_hi}, b {ty} >= {b_lo} and <= {b_hi}) {nty} {{")
        L.append(f"    return (a {op} b) as {nty}")
        L.append("}")
        calls = [(a_hi, b_hi), (a_lo, b_lo)]
    else:  # accum — B1's shape: a running total bounded by trip count x step
        trips = rng.choice([2, 4, 8, 16, 64])
        L.append(f"func f(a {ty} >= {a_lo} and <= {a_hi}, b {ty} >= {b_lo} and <= {b_hi}) {ty} {{")
        L.append(f"    var s {ty} = b")
        L.append("    var i i32 = 0")
        L.append(f"    while i < {trips} {{")
        L.append("        s = s + a")
        L.append("        i = i + 1")
        L.append("    }")
        L.append("    return s")
        L.append("}")
        calls = [(a_hi, b_hi), (a_lo, b_lo)]

    L.append("func main() i32 effects io, raises, alloc {")
    for (x, y) in calls:
        L.append(f"    libc_printf(\"%d\\n\", f({x}, {y}) as i32)")
    L.append("    return 0")
    L.append("}")
    return "\n".join(L) + "\n"

if __name__ == "__main__":
    rng = random.Random(int(sys.argv[1]) if len(sys.argv) > 1 else None)
    sys.stdout.write(gen(rng))
