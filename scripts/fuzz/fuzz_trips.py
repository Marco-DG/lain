#!/usr/bin/env python3
"""Random loops over a FALLING measure with a running total beside it, judged for every input.

Each program is `func f(n u16 <= N) T` with `var d u16 = n`, `var c T = C0` (T is u8 or i8, C0 often
near an end of T) and one loop `while d > B` or `while d >= B` whose body is 1 to 4 statements:
decrements of d (plain, on both sides of an `if`, or with `else { break }`), updates of c (plain or
under an `if`, sometimes two in one trip), and an occasional `if ... { continue }`. A third of the
programs are TIGHT: one decrement by k and one update by a, C0 = max(T) - T*a or one more, with T
the exact trip count from n = N, so an off-by-one in the bound is accepted overflowing; some of
those update c twice a trip, or `continue` past the decrement, with C0 fit for the bound a rule
that missed it would compute. A fifth
declare `effects diverge`: a loop may then run for ever by design, the termination proof no longer
stands in front of the trip count, and only an update that leaves T is a bug.
The program is deterministic in n, so a Python model runs it for every n in 0..N and records
whether any input underflows d, takes c out of T, or (without `diverge`) loops for ever. The
compiler accepting a program that does any of these from some input is UNSOUND.

The trip count of a falling measure (I.158) and the running-total rule's one-update-per-trip,
nesting and entry conditions (I.162) are what this exercises; fuzz_loops decides termination over
two variables, and this decides the BOUND.

    fuzz_trips.py LAIN N SEED
    fuzz_trips.py SEED          (one program, so fuzz_interp runs this generator under its oracle)
"""
import random, subprocess, sys, os, tempfile

TYPES = {"u8": (0, 255), "i8": (-128, 127)}

def gen(rng):
    T = rng.choice(["u8", "i8"]); lo, hi = TYPES[T]
    N = rng.choice([40, 100, 200, 600, 1000])
    op = rng.choice([">", ">="])
    kmax = rng.choice([1, 2, 3, 5, 8])
    B = rng.randint(kmax - 1 if op == ">" else kmax, kmax + 6)   # d - k cannot underflow under the guard
    def k(): return rng.randint(1, kmax)
    def a(): return rng.choice([1, 1, 2, 3, -1, -2, 0])
    stmts = []
    for _ in range(rng.choice([1, 2, 2, 3, 4])):
        r = rng.random()
        if r < 0.25:   stmts.append(("dec", k()))
        elif r < 0.45: stmts.append(("ifdec", rng.randint(B, B + 20), k(), k()))
        elif r < 0.55: stmts.append(("ifbreak", rng.randint(B, B + 20), k()))
        elif r < 0.80: stmts.append(("inc", a()))
        elif r < 0.92: stmts.append(("ifinc", rng.randint(0, N), a()))
        else:          stmts.append(("cont", rng.randint(B, N)))
    if not any(s[0] in ("dec", "ifdec", "ifbreak") for s in stmts): stmts.insert(0, ("dec", k()))
    if not any(s[0] in ("inc", "ifinc") for s in stmts): stmts.append(("inc", a()))
    span = max(1, (N - B) // max(1, kmax) + 2)                  # a start the bound has to reach
    C0 = rng.choice([lo, hi, lo + rng.randint(0, span), hi - rng.randint(0, span), 0])
    C0 = max(lo, min(hi, C0))
    div = rng.random() < 0.2
    if rng.random() < 0.33:                                      # TIGHT: the bound decides
        kk, aa = rng.randint(1, kmax), rng.choice([1, 1, 2, 3])
        stmts = [("dec", kk), ("inc", aa)] if rng.random() < 0.5 else [("inc", aa), ("dec", kk)]
        trips = (0 if N <= B else -(-(N - B) // kk)) if op == ">" else (0 if N < B else (N - B) // kk + 1)
        C0 = hi - trips * aa + rng.choice([0, 1])
        r2 = rng.random()
        if r2 < 0.3:                                             # TWO updates a trip, C0 fit for one
            # in ONE block, or the first under an `if d > 0` that every trip passes (d >= 1 there)
            if rng.random() < 0.5: stmts.insert(rng.randint(0, 2), ("inc", aa))
            else: stmts = [("ifinc", 0, aa), ("dec", kk), ("inc", aa)]
            C0 = hi - trips * aa
        elif r2 < 0.55:                                          # a `continue` that skips the decrement
            div = True; X = rng.randint(B, N)
            stmts = [("inc", aa), ("cont", X), ("dec", kk)]; C0 = hi - trips * aa
        if C0 < lo: C0 = lo
    return T, N, op, B, C0, stmts, div

def cupd(a): return "c = c + %d" % a if a >= 0 else "c = c - %d" % (-a)

def src(T, N, op, B, C0, stmts, div):
    eff = " effects diverge" if div else ""
    L = ["func f(n u16 <= %d) %s%s {" % (N, T, eff), "    var d u16 = n", "    var c %s = %d" % (T, C0),
         "    while d %s %d {" % (op, B)]
    for s in stmts:
        if s[0] == "dec":     L.append("        d = d - %d" % s[1])
        elif s[0] == "ifdec": L += ["        if d > %d {" % s[1], "            d = d - %d" % s[2], "        } else {",
                                    "            d = d - %d" % s[3], "        }"]
        elif s[0] == "ifbreak": L += ["        if d > %d {" % s[1], "            d = d - %d" % s[2], "        } else {",
                                      "            break", "        }"]
        elif s[0] == "inc":   L.append("        " + cupd(s[1]))
        elif s[0] == "ifinc": L += ["        if d > %d {" % s[1], "            " + cupd(s[2]), "        }"]
        elif s[0] == "cont":  L += ["        if d > %d {" % s[1], "            continue", "        }"]
    L += ["    }", "    return c", "}", "", "func main() i32%s {" % eff, "    return f(%d) as i32" % N, "}"]
    return "\n".join(L) + "\n"

def bad_input(T, N, op, B, C0, stmts, div):
    """The first n in 0..N from which the program underflows d, leaves T, or runs for ever; None."""
    lo, hi = TYPES[T]
    for n in range(N + 1):
        d, c, steps = n, C0, 0
        while (d > B) if op == ">" else (d >= B):
            steps += 1
            if steps > 100000:
                if div: break                   # allowed to: `diverge` says so
                return (n, "runs for ever")
            brk = False
            for s in stmts:
                if s[0] == "dec":   d -= s[1]
                elif s[0] == "ifdec": d -= s[2] if d > s[1] else s[3]
                elif s[0] == "ifbreak":
                    if d > s[1]: d -= s[2]
                    else: brk = True; break
                elif s[0] == "inc": c += s[1]
                elif s[0] == "ifinc":
                    if d > s[1]: c += s[2]
                elif s[0] == "cont":
                    if d > s[1]: break          # to the next trip
                if d < 0: return (n, "d underflows")
                if not (lo <= c <= hi): return (n, "c = %d leaves %s" % (c, T))
            if brk: break
    return None

def main():
    if len(sys.argv) == 2:
        # fuzz_interp runs what this prints and counts a run that does not end as a crash, so a
        # program for it never says `diverge`: one that may not end is then refused (E011).
        g = list(gen(random.Random(int(sys.argv[1])))); g[-1] = False
        sys.stdout.write(src(*g)); return
    lain, N, seed = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    tmp = tempfile.mkdtemp(prefix="fuzz_trips.")
    acc = refused = other = unsound = good = lost = 0
    for k in range(N):
        g = gen(random.Random(seed * 1000003 + k))
        text = src(*g); p = os.path.join(tmp, "t.ln"); open(p, "w").write(text)
        r = subprocess.run([lain, p, "-o", os.path.join(tmp, "t.c")], capture_output=True, text=True)
        bad = bad_input(*g)
        if bad is None: good += 1
        if r.returncode == 0:
            acc += 1
            if bad is not None:
                unsound += 1
                print("  ★ UNSOUND: accepted, and from n = %d %s (seed %d, k %d)" % (bad[0], bad[1], seed, k))
                print("".join("      " + l + "\n" for l in text.splitlines()), end="")
        elif r.returncode == 1 and "[E" in r.stderr:
            refused += 1
            if bad is None: lost += 1
        else:
            other += 1
            print("  ★ HARNESS: exit %d without a coded refusal (seed %d, k %d): %s" % (r.returncode, seed, k, r.stderr.strip()[:200]))
    for f in os.listdir(tmp): os.remove(os.path.join(tmp, f))
    os.rmdir(tmp)
    print("fuzz_trips: seed=%d gens=%d accepted=%d refused=%d other=%d" % (seed, N, acc, refused, other))
    print("  ground truth: %d safe from every input; %d of those refused (the cost)" % (good, lost))
    print("  bugs:  UNSOUND(accepted, fails from some input)=%d" % unsound)
    sys.exit(1 if (unsound or other) else 0)

if __name__ == "__main__":
    main()
