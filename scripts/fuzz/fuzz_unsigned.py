#!/usr/bin/env python3
"""Generate one Lain program doing UNSIGNED arithmetic, plus the exact answers it owes.

UBSan cannot help here: unsigned wraparound is DEFINED behaviour in C, so `fuzz_overflow.sh`
generates only signed types and says so. Lain rejects unsigned overflow just the same
(prove-or-reject applies to usize and friends), and the class matters — the memory-safety bug
of 2026-09-09 was an unsigned UNDERFLOW: `n - 1` at n == 0 becoming SIZE_MAX and being used as
a loop bound.

The oracle is exact integer arithmetic, computed here and emitted as `// EXPECT_OUT:`:
  · every exact result fits the type  -> the program is safe; if the engine proved it, running
                                         it must print exactly these numbers
  · some exact result does NOT fit    -> the program is unsafe; the engine PROVING it is
                                         unsound, and that verdict needs no execution at all
"""
import random, sys

TYPES = {"u8": 8, "u16": 16, "u32": 32}

def gen(rng):
    ty = rng.choice(list(TYPES)); bits = TYPES[ty]
    hi_t = (1 << bits) - 1
    op = rng.choice(["+", "-", "*"])

    def bounds():
        span = rng.choice([1, 3, 15, 255, hi_t // 2, hi_t])
        lo = rng.choice([0, 0, 1, min(span, hi_t)])
        return min(lo, hi_t), min(max(lo, span), hi_t)

    a_lo, a_hi = bounds()
    b_lo, b_hi = bounds()

    calls = [(a_hi, b_hi), (a_lo, b_lo), (a_hi, b_lo), (a_lo, b_hi)]
    exact = []
    for (x, y) in calls:
        exact.append(x + y if op == "+" else x - y if op == "-" else x * y)
    fits = all(0 <= v <= hi_t for v in exact)

    L = ["extern func libc_printf(fmt *u8, ...) i32 effects io",
         f"// EXPECT_OUT: {' '.join(str(v) for v in exact)}",
         f"// FITS: {'yes' if fits else 'no'}",
         f"func f(a {ty} >= {a_lo} and <= {a_hi}, b {ty} >= {b_lo} and <= {b_hi}) {ty} {{",
         f"    return a {op} b",
         "}",
         "func main() i32 effects io, raises, alloc {"]
    for (x, y) in calls:
        L.append(f"    libc_printf(\"%llu\\n\", f({x}, {y}) as u64)")
    L += ["    return 0", "}"]
    return "\n".join(L) + "\n"

if __name__ == "__main__":
    rng = random.Random(int(sys.argv[1]) if len(sys.argv) > 1 else None)
    sys.stdout.write(gen(rng))
