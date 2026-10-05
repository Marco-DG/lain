#!/usr/bin/env python3
"""Random recursion cycles, judged against GROUND TRUTH for every input (I.130).

Each program has 2 to 4 functions `f(x, y)`, each making 1 to 3 calls to any of them, itself
included, with arguments a permutation of (x, y) each moved by -1, 0 or +1. Four guards return
before any call unless 1 <= x, y <= 100, so a run is a walk over the finite states
(function, x, y), one edge per call, and every call on the path executes: the program can run for
ever, from SOME input, iff that graph has a cycle. That is decided here exactly, by search, for
every input at once. The compiler accepting a program (exit 0: every obligation discharged) that
can run for ever is UNSOUND.

This is stronger than fuzz_termination's oracle, which executes one input: the 3-cycle hole that
made this file (18 of 5832 exhaustive programs accepted that never end) needed the right input to
show, and a second call or a self-call on a cycle (8 of 1000 two-function programs here) needed the
generator to make more than one call per function, which fuzz_termination's `mutual` shape never did.

    fuzz_cycles.py LAIN N SEED [KEEPDIR]
    fuzz_cycles.py SEED          (one program, so fuzz_interp runs this generator under its oracle)
"""
import random, subprocess, sys, os, tempfile

def arg(e, d): return e if d == 0 else ("%s - 1" % e if d < 0 else "%s + 1" % e)

def gen(rng):
    m = rng.choice([2, 3, 4])
    ty = rng.choice(["u8", "u8", "i32"])          # a signed measure is grounded only by its guard
    fs = []
    for _ in range(m):
        fs.append([(rng.randrange(m), rng.choice([("x", "y"), ("y", "x")]),
                    rng.choice([-1, 0, 0, 1]), rng.choice([-1, 0, 0, 1]))
                   for _ in range(rng.choice([1, 1, 2, 2, 3]))])
    return m, ty, fs

def src(m, ty, fs):
    out = []
    low = "== 0" if ty == "u8" else "<= 0"
    wrap = "+%"
    for i, calls in enumerate(fs):
        body = ["    if x %s { return 0 }" % low, "    if y %s { return 0 }" % low,
                "    if x > 100 { return 0 }", "    if y > 100 { return 0 }",
                "    var r %s = 0" % ty]
        for (j, perm, dx, dy) in calls:
            body.append("    r = r %s f%d(%s, %s)" % (wrap, j, arg(perm[0], dx), arg(perm[1], dy)))
        body.append("    return r")
        out.append("func f%d(x %s, y %s) %s {\n%s\n}\n" % (i, ty, ty, ty, "\n".join(body)))
    return "\n".join(out) + "\nfunc main() i32 {\n    return f0(5, 7) as i32\n}\n"

def can_run_forever(m, fs):
    stop = lambda x, y: x < 1 or y < 1 or x > 100 or y > 100
    def succ(i, x, y):
        v = {"x": x, "y": y}
        for (j, perm, dx, dy) in fs[i]:
            yield (j, v[perm[0]] + dx, v[perm[1]] + dy)
    color = {}
    for i in range(m):
        for x0 in range(1, 101):
            for y0 in range(1, 101):
                st = (i, x0, y0)
                if st in color: continue
                color[st] = 1; stack = [(st, succ(*st))]
                while stack:
                    cur, it = stack[-1]
                    n = next(it, None)
                    if n is None: color[cur] = 2; stack.pop(); continue
                    if stop(n[1], n[2]): continue
                    c = color.get(n)
                    if c == 1: return True
                    if c is None: color[n] = 1; stack.append((n, succ(*n)))
    return False

def main():
    # One argument, a seed: print one program. fuzz_interp takes every fuzz_*.py for a program
    # generator and runs what it prints two ways, the C against the interpreter; without this
    # mode each call failed and counted as GENERATOR-FAIL (30 per generator, 2026-10-05).
    if len(sys.argv) == 2:
        m, ty, fs = gen(random.Random(int(sys.argv[1])))
        sys.stdout.write(src(m, ty, fs)); return
    lain, N, seed = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    keep = sys.argv[4] if len(sys.argv) > 4 else None
    tmp = tempfile.mkdtemp(prefix="fuzz_cycles.")
    acc = ref = other = unsound = term = lost = 0
    for k in range(N):
        rng = random.Random(seed * 1000003 + k)
        m, ty, fs = gen(rng)
        p = os.path.join(tmp, "t.ln"); text = src(m, ty, fs); open(p, "w").write(text)
        r = subprocess.run([lain, p, "-o", os.path.join(tmp, "t.c")], capture_output=True, text=True)
        forever = can_run_forever(m, fs)
        if not forever: term += 1
        if r.returncode == 0:
            acc += 1
            if forever:
                unsound += 1
                print("  ★ UNSOUND: accepted, and runs for ever from some input (seed %d, k %d)" % (seed, k))
                print("".join("      " + l + "\n" for l in text.splitlines()), end="")
                if keep: open(os.path.join(keep, "unsound.%d.%d.ln" % (seed, k)), "w").write(text)
        elif "[E011]" in r.stderr or "[E082]" in r.stderr:
            ref += 1
            if not forever: lost += 1
        else:
            other += 1                              # refused for something else, or a crash
            if other <= 3: print("  refused for another reason (k %d): %s" % (k, (r.stderr.strip().splitlines() or ["rc=%d" % r.returncode])[0][:160]))
    for f in os.listdir(tmp): os.remove(os.path.join(tmp, f))
    os.rmdir(tmp)
    print("fuzz_cycles: seed=%d gens=%d accepted=%d refused-termination=%d refused-other=%d" % (seed, N, acc, ref, other))
    print("  ground truth: %d end from every input; %d of those refused (the cost: they end, by no measure the rule looks for)" % (term, lost))
    print("  bugs:  UNSOUND(accepted, runs for ever)=%d" % unsound)
    if other > N // 2:
        print("  ★ FUZZER DID NOT RUN: %d/%d refused for another reason" % (other, N)); sys.exit(1)
    sys.exit(1 if unsound else 0)

if __name__ == "__main__":
    main()
