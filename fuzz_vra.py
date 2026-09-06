#!/usr/bin/env python3
"""Generator for fuzz_vra.sh — programs whose bounds safety rests on the NEW VRA's rules.

Every other fuzzer here validates the OLD engine. The new VRA is what survives Stage 3.5,
and three of its proof rules (division relational facts, the midpoint difference
substitution, inferred call return ranges) were added without any fuzzer able to falsify
them: a FALSE PROOF is a removed bounds check, and only 33 corpus cases stood against it.

Each program computes an index by one of those idioms and reads an array at it, from
`main` with values chosen to sit ON the boundary. The harness asks the new engine whether
the access is check-free; if it says yes, the program is executed under ASan. Any
out-of-bounds read is a false proof.

The generator deliberately emits both SAFE and UNSAFE variants — a fuzzer that can only
produce safe programs proves nothing about the prover.
"""
import random, sys

def gen(rng):
    kind = rng.choice(["midpoint", "divsub", "retrange", "divconst", "midpoint_wide",
                       "loop_scan", "loop_scan", "loop_unbounded"])
    # Large arrays matter: the widening bug this fuzzer must catch is a SLOT COLLISION, and
    # which slot collides depends on how many values the function has. But an explicit literal
    # costs one trivially-proven obligation PER ELEMENT, so the initializer is a comprehension
    # — one obligation, any size.
    N    = rng.choice([16, 32, 64, 128, 256, 1024, 4096])
    lit  = f"0 for z in 0..{N}"

    if kind == "midpoint":
        # mid = lo + (hi-lo)/D, guarded lo < hi < BOUND. Safe iff BOUND <= N.
        D     = rng.choice([2, 2, 2, 3, 4])
        bound = rng.choice([N, N, N // 2 or 1, N + rng.randint(1, 40)])
        lo, hi = rng.randint(0, 3), rng.randint(1, bound + 20)
        body = f"""proc probe(lo usize, hi usize, a i32[{N}]) i32 {{
    if lo < hi and hi < {bound} {{
        var mid usize = lo + (hi - lo) / {D}
        return a[mid]
    }}
    return 0
}}"""
        call = f"probe({lo}, {hi}, arr)"

    elif kind == "midpoint_wide":
        # the same shape with NO upper guard on hi — must never be proven.
        lo, hi = rng.randint(0, 3), rng.randint(1, N + 50)
        body = f"""proc probe(lo usize, hi usize, a i32[{N}]) i32 {{
    if lo < hi {{
        var mid usize = lo + (hi - lo) / 2
        return a[mid]
    }}
    return 0
}}"""
        call = f"probe({lo}, {hi}, arr)"

    elif kind == "divsub":
        # m - m/D, guarded m < BOUND. Max value is BOUND-1 - (BOUND-1)/D.
        D     = rng.choice([2, 2, 3, 4])
        bound = rng.choice([N, N + rng.randint(1, 60), 2 * N, N // 2 or 1])
        m     = rng.randint(0, bound + 20)
        body = f"""proc probe(m usize, a i32[{N}]) i32 {{
    if m < {bound} {{
        return a[m - m / {D}]
    }}
    return 0
}}"""
        call = f"probe({m}, arr)"

    elif kind == "divconst":
        # plain m/D under a guard — exercises the new interval half of the div facts.
        D     = rng.choice([2, 3, 5, 8])
        bound = rng.choice([N, N * D, N * D + rng.randint(1, 30), N // 2 or 1])
        m     = rng.randint(0, bound + 20)
        body = f"""proc probe(m usize, a i32[{N}]) i32 {{
    if m < {bound} {{
        return a[m / {D}]
    }}
    return 0
}}"""
        call = f"probe({m}, arr)"

    elif kind == "loop_scan":
        # A counted scan. The WIDENING at the loop header is what keeps `i` sound here, and
        # nothing else in this generator exercises it — a bug that stops widening the counter
        # leaves a stale bound and turns an unbounded scan into a "proof". The loop bound is
        # drawn to sit above, at, and below the array length on purpose.
        bound = rng.choice([N, N, N // 2 or 1, N + rng.randint(1, 40)])
        ty, inc = rng.choice([("usize", "i + 1"), ("u32", "i +% 1"), ("u32", "i + 1")])
        body = f"""proc probe(m {ty}, a i32[{N}]) i32 {{
    var acc i32 = 0
    var i {ty} = 0
    while i < {bound} {{
        acc = acc +% a[i]
        i = {inc}
    }}
    return acc
}}"""
        call = f"probe({rng.randint(0, N)}, arr)"

    elif kind == "loop_unbounded":
        # The soundness lock's own shape: `while i < n` with n a PARAMETER carrying no upper
        # bound. It must never be proven, whatever the caller happens to pass.
        n = rng.choice([0, 1, N // 2 or 1, N, N + rng.randint(1, 200)])
        ty, inc = rng.choice([("usize", "i + 1"), ("u32", "i +% 1"), ("u32", "i + 1")])
        # ★ The layout matters. A wrong-keyed table (a value id used where a packed slot
        # belongs) is a COLLISION bug: whether it misfires depends on which id lands on which
        # slot, so a single fixed shape probes exactly one arrangement and can miss it
        # completely. Vary the parameter ORDER and pad with dead locals to shift every id.
        pad  = "\n".join(f"    var d{k} {ty} = {k}" for k in range(rng.randint(0, 4)))
        aacc = rng.choice(["acc = acc +% a[i]", "acc = a[i]"])
        if rng.random() < 0.5:
            sig, call = f"proc probe(n {ty}, a i32[{N}]) i32", f"probe({n}, arr)"
        else:
            sig, call = f"proc probe(a i32[{N}], n {ty}) i32", f"probe(arr, {n})"
        body = f"""{sig} {{
    var acc i32 = 0
{pad}
    var i {ty} = 0
    while i < n {{
        {aacc}
        i = {inc}
    }}
    return acc
}}"""

    else:  # retrange — the index comes from a helper's return value
        mask = rng.choice([7, 15, 31, 63, 127])
        op   = rng.choice(["mask", "mask", "div", "raw"])
        c    = rng.randint(0, 255)
        if op == "mask":   helper = f"func idx(c u8) u8 {{ return c & {mask} }}"
        elif op == "div":  helper = f"func idx(c u8) u8 {{ return c / {rng.choice([2,4,8])} }}"
        else:              helper = "func idx(c u8) u8 { return c }"
        body = f"""{helper}
proc probe(c u8, a i32[{N}]) i32 {{
    return a[idx(c) as usize]
}}"""
        call = f"probe({c}, arr)"

    return f"""{body}
proc main() i32 {{
    var arr i32[{N}] = [{lit}]
    var s i32 = 0
    s = s +% {call}
    if s == 999999 {{
        return 1
    }}
    return 0
}}
"""

if __name__ == "__main__":
    rng = random.Random(int(sys.argv[1]) if len(sys.argv) > 1 else None)
    sys.stdout.write(gen(rng))
