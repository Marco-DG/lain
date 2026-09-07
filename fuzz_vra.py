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
                       "loop_scan", "loop_scan", "loop_unbounded",
                       "alias", "alias", "alias",
                       "signedidx", "signedidx"])
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

    elif kind == "signedidx":
        # ★ SIGNED INDEX under an UNSIGNED guard. `(unsigned)i < n` is the C idiom for "valid
        # index" and what `i in a` lowers to; the domain now reads the non-negative half out of
        # it. That deduction is only sound if the LOWERING really emits an unsigned compare, so
        # the generator feeds negative values through all three spellings and executes whatever
        # is proven: a signed `p < N` guard must NOT be proven (p = -1 passes it and reads
        # a[-1]), while `p in a` must be.
        guard = rng.choice(["in", "in", "signed_lt", "both_sides", "unsigned_cast"])
        k     = rng.choice([-9, -1, 0, 1, N // 2, N - 1, N, N + 7])
        if guard == "in":            cond = f"p in a"
        elif guard == "signed_lt":   cond = f"p < {N}"
        elif guard == "both_sides":  cond = f"p >= 0 and p < {N}"
        else:                        cond = f"usize(p) < usize({N})"
        body = f"""proc probe(a i32[{N}], p i32) i32 {{
    if {cond} {{
        return a[p]
    }}
    return 0
}}"""
        call = f"probe(arr, {k})"

    elif kind == "alias":
        # ★ THE ALIAS ORACLE. A call between the guard and the access used to erase the guard:
        # every escaped cell was forgotten, whether or not the call could reach it. Now only
        # what the call can actually write is forgotten — so `bump(var j)` must leave `i < N`
        # standing, while `bump(var i)` must not. Both directions are generated: an oracle that
        # is too generous removes a bounds check on a program that walks off the array, and the
        # harness executes exactly those under ASan.
        seed  = rng.randint(0, max(0, N - 1))
        shape = rng.choice([
            "safe_other", "safe_other", "unsafe_self", "unsafe_self",
            "safe_second", "unsafe_second", "safe_wrap", "unsafe_wrap",
            "safe_pure", "stash_self", "stash_other",
            "stash_write_self", "stash_write_self", "stash_write_other",
        ])
        helpers = [f"proc bump(var x usize) {{ x = {N} }}"]
        pre, perturb = "", ""
        if shape == "safe_other":
            perturb = "bump(var j)"
        elif shape == "unsafe_self":
            perturb = "bump(var i)"
        elif shape in ("safe_second", "unsafe_second"):
            # writes the SECOND parameter only: passing a cell is not writing it
            helpers.append(f"proc bump2(var p usize, var q usize) {{ q = {N} }}")
            perturb = "bump2(var i, var j)" if shape == "safe_second" else "bump2(var j, var i)"
        elif shape in ("safe_wrap", "unsafe_wrap"):
            helpers.append("proc wrap(var y usize) { bump(var y) }")   # transitive footprint
            perturb = "wrap(var j)" if shape == "safe_wrap" else "wrap(var i)"
        elif shape == "safe_pure":
            helpers.append("func peek(x usize) usize { return x }")
            perturb = "var t usize = peek(j)"
        elif shape in ("stash_self", "stash_other"):
            # the address is STASHED in a struct, so a later call could write it through the
            # stash. Conservative by design: `stash_self` must never be proven.
            helpers.append("type Holder { p *var usize }")
            helpers.append("proc keep(var h Holder) { unsafe { var z usize = *h.p } }")
            tgt = "i" if shape == "stash_self" else "j"
            pre = f"    var h Holder = Holder(&{tgt})"
            perturb = "keep(var h)"
        else:
            # ...and the stash actually FIRED. `fire` is handed the Holder, never the cell, so
            # nothing in its argument list names `i` — the write arrives entirely through the
            # squirreled-away address. This is the shape the `persist` half of the oracle
            # exists for, and the only one that can falsify it: drop that half and this walks
            # off the array with the bounds check removed.
            helpers.append("type Holder { p *var usize }")
            helpers.append(f"proc fire(var h Holder) {{ unsafe {{ *h.p = {N} }} }}")
            tgt = "i" if shape == "stash_write_self" else "j"
            pre = f"    var h Holder = Holder(&{tgt})"
            perturb = "fire(var h)"
        body = "\n".join(helpers) + f"""
proc probe(a i32[{N}]) i32 {{
    var i usize = {seed}
    var j usize = 0
{pre}
    if i < {N} {{
        {perturb}
        return a[i]
    }}
    return 0
}}"""
        call = "probe(arr)"

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
