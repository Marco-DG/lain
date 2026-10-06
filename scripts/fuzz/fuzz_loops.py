#!/usr/bin/env python3
"""Random `while` loops over two u8 variables, judged against GROUND TRUTH for every input.

Each program is one loop `while G { B }` in a `func` over `x` and `y` (both u8, from the parameters):
G is a conjunction of 2 to 6 comparisons drawn from a fixed list, always bounding both variables
below 200 so that `+ 1` and `+ 2` cannot overflow; B is 1 to 3 statements, each an assignment or an
`if C { A } else { A' }` over the same variables. The loop is deterministic over the 65536 states
(x, y), so it can run for ever from SOME input iff the successor graph restricted to the states
where G holds has a cycle. That is decided here exactly, by search, for every input at once. The
compiler accepting a loop (exit 0: its termination obligation discharged) that can run for ever is
UNSOUND.

fuzz_termination executes each proven loop on two inputs; this decides every input. It is the
loop counterpart of fuzz_cycles (I.130), and was written after that oracle found three holes in
the recursion rule which no execution-based fuzzer had reached.

    fuzz_loops.py LAIN N SEED
    fuzz_loops.py SEED          (one program, so fuzz_interp runs this generator under its oracle)
"""
import random, subprocess, sys, os, tempfile

GUARDS = ["x != 0", "y != 0", "x > 0", "y > 0", "x < 100", "y < 100", "x < y", "y < x", "x != y", "x > 5", "y > 5"]
CONDS = ["x < y", "x > y", "x == y", "x < 50", "y > 50", "x % 2 == 0", "y % 3 == 0", "true"]
UPD = ["x = x & (x - 1)", "y = y & (y - 1)", "x = x - 1", "x = x + 1", "y = y - 1", "y = y + 1", "x = y", "y = x", "x = x - 2", "y = y + 2", "x = y - 1"]

def gen(rng):
    gs = rng.sample(GUARDS, rng.choice([2, 3, 4]))
    # A lower guard on each variable, often enough for most loops to have a chance of a measure:
    # `> 0` or, read the same way on an unsigned value since H4, `!= 0`.
    if "x != 0" not in gs and "x > 0" not in gs and rng.random() < 0.7: gs.append(rng.choice(["x > 0", "x != 0"]))
    if "y != 0" not in gs and "y > 0" not in gs and rng.random() < 0.7: gs.append(rng.choice(["y > 0", "y != 0"]))
    gs += ["x < 200", "y < 200"]
    body = [(rng.choice(CONDS), rng.choice(UPD), rng.choice(UPD + [None])) for _ in range(rng.choice([1, 2, 2, 3]))]
    return gs, body

def src(gs, body):
    L = ["func run(x0 u8, y0 u8) u8 {", "    var x = x0", "    var y = y0", "    while " + " and ".join(gs) + " {"]
    for (c, a, b) in body:
        if c == "true": L.append("        " + a); continue
        L += ["        if " + c + " {", "            " + a, "        }"]
        if b: L[-1] = "        } else {"; L += ["            " + b, "        }"]
    L += ["    }", "    return x", "}", "", "func main() i32 {", "    return run(5, 7) as i32", "}"]
    return "\n".join(L) + "\n"

def py(e): return e.replace("true", "True")

def can_run_forever(gs, body):
    guard = eval("lambda x, y: " + " and ".join("(%s)" % py(g) for g in gs))
    steps = []
    for (c, a, b) in body:
        cf = eval("lambda x, y: " + py(c))
        def assign(s):
            if s is None: return None
            var, rhs = s.split(" = ")
            f = eval("lambda x, y: " + rhs)
            return (var == "x", f)
        steps.append((cf, assign(a), assign(b)))
    def succ(x, y):
        for (cf, a, b) in steps:
            s = a if cf(x, y) else b
            if s is None: continue
            v = s[1](x, y)
            if s[0]: x = v
            else: y = v
        return x, y
    color = bytearray(65536)                    # 0 unseen, 1 on the current path, 2 done
    for s0 in range(65536):
        if color[s0]: continue
        path = []; x, y = s0 >> 8, s0 & 255
        while True:
            if not guard(x, y): break
            k = (x << 8) | y
            if color[k] == 2: break
            if color[k] == 1: return True
            color[k] = 1; path.append(k)
            x, y = succ(x, y)
            if not (0 <= x <= 255 and 0 <= y <= 255): break   # an overflow is refused (E086) anyway
        for k in path: color[k] = 2
    return False

def main():
    # One argument, a seed: print one program. fuzz_interp takes every fuzz_*.py for a program
    # generator and runs what it prints two ways, the C against the interpreter; without this
    # mode each call failed and counted as GENERATOR-FAIL (30 per generator, 2026-10-05).
    if len(sys.argv) == 2:
        gs, body = gen(random.Random(int(sys.argv[1])))
        sys.stdout.write(src(gs, body)); return
    lain, N, seed = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    tmp = tempfile.mkdtemp(prefix="fuzz_loops.")
    acc = ref = other = unsound = term = lost = 0
    for k in range(N):
        gs, body = gen(random.Random(seed * 1000003 + k))
        p = os.path.join(tmp, "t.ln"); text = src(gs, body); open(p, "w").write(text)
        r = subprocess.run([lain, p, "-o", os.path.join(tmp, "t.c")], capture_output=True, text=True)
        forever = can_run_forever(gs, body)
        if not forever: term += 1
        if r.returncode == 0:
            acc += 1
            if forever:
                unsound += 1
                print("  ★ UNSOUND: accepted, and runs for ever from some input (seed %d, k %d)" % (seed, k))
                print("".join("      " + l + "\n" for l in text.splitlines()), end="")
        elif "[E011]" in r.stderr or "[E082]" in r.stderr:
            ref += 1
            if not forever: lost += 1
        else:
            other += 1
    for f in os.listdir(tmp): os.remove(os.path.join(tmp, f))
    os.rmdir(tmp)
    print("fuzz_loops: seed=%d gens=%d accepted=%d refused-termination=%d refused-other=%d" % (seed, N, acc, ref, other))
    print("  ground truth: %d end from every input; %d of those refused (the cost)" % (term, lost))
    print("  bugs:  UNSOUND(accepted, runs for ever)=%d" % unsound)
    if other > N // 2:
        print("  ★ FUZZER DID NOT RUN: %d/%d refused for another reason" % (other, N)); sys.exit(1)
    sys.exit(1 if unsound else 0)

if __name__ == "__main__":
    main()
