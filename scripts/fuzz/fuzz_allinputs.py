#!/usr/bin/env python3
"""Index, narrowing and division obligations judged against EVERY input, plus a run of every input.

Each program is `func run(x u8, y u8) i32`: an index `i` (an i32 from `x as i32` and `y as i32`, or a
u8 from `x` and `y`, chosen per program) computed by an expression of depth up to 2 over + - * / %
(and & | ^ >> in u8 mode), optionally reassigned under a condition, then `a[i]` into a 10-element
array, optionally behind a guard. The model is Lain's arithmetic: unbounded intermediate values
(Path-F widens + - * to a type that holds them), a FIT check where a value is narrowed into `i`,
the index check, and division by zero. Over all 65536 inputs:
  - UNSOUND: the compiler accepts (exit 0) a program that faults on some input;
  - MISMATCH: an accepted program, compiled with gcc and RUN on all 65536 inputs, returns other
    than the model on some input (a miscompile, or this model is wrong; either is investigated).
fuzz_vra executes each program on a few inputs; this decides all of them, and runs all of them.

    fuzz_allinputs.py LAIN N SEED
    fuzz_allinputs.py SEED          (one program, so fuzz_interp runs this generator under its oracle)
"""
import random, subprocess, sys, os, tempfile

class Fault(Exception): pass
def tdiv(a, b):
    if b == 0: raise Fault("div0")
    q = abs(a) // abs(b); return q if (a >= 0) == (b >= 0) else -q
def tmod(a, b):
    if b == 0: raise Fault("div0")
    return a - tdiv(a, b) * b
def gen_expr(rng, d, u8):
    if d == 0 or rng.random() < 0.3:
        r = rng.random()
        if r < 0.4: return ("x",)
        if r < 0.8: return ("y",)
        return ("k", rng.randrange(0, 21))
    ops = ["+", "-", "*", "/", "%", "+", "-"] + (["&", "|", "^", ">>"] if u8 else [])
    op = rng.choice(ops)
    if op == ">>": return (op, gen_expr(rng, d - 1, u8), ("k", rng.randrange(0, 8)))
    return (op, gen_expr(rng, d - 1, u8), gen_expr(rng, d - 1, u8))
def show(e, u8):
    t = e[0]
    if t in ("x", "y"): return t if u8 else "(%s as i32)" % t
    if t == "i": return "i"
    if t == "k": return str(e[1])
    return "(%s %s %s)" % (show(e[1], u8), t, show(e[2], u8))
def ev(e, x, y, i):
    t = e[0]
    if t == "x": return x
    if t == "y": return y
    if t == "i": return i
    if t == "k": return e[1]
    a, b = ev(e[1], x, y, i), ev(e[2], x, y, i)
    if t == "+": return a + b
    if t == "-": return a - b
    if t == "*": return a * b
    if t == "/": return tdiv(a, b)
    if t == "%": return tmod(a, b)
    if t == "&": return a & b
    if t == "|": return a | b
    if t == "^": return a ^ b
    return a >> b
CONDS32 = [None, "i >= 0 and i < 10", "i < 10", "i >= 0", "i > -1 and i <= 9", "i >= 1 and i < 10", "x < 10",
           "i != 10 and i >= 0", "i * 2 < 20 and i >= 0"]
CONDS8 = [None, "i < 10", "i <= 9", "i >= 1 and i < 10", "x < 10", "x < 10 and i == x", "i != 10 and i < 11",
          "i * 2 < 20", "i < y and y < 10", "i % 10 == i"]
def gen(rng):
    u8 = rng.random() < 0.5
    e1 = gen_expr(rng, rng.choice([1, 2, 2]), u8)
    e2 = None
    if rng.random() < 0.5:
        e2 = gen_expr(rng, rng.choice([1, 2]), u8)
        if rng.random() < 0.5: e2 = (rng.choice(["+", "-", "%"]), ("i",), e2)
    c2 = rng.choice(["x > y", "i > 5", "y % 2 == 0", "i < 3"]) if e2 else None
    return u8, e1, e2, c2, rng.choice(CONDS8 if u8 else CONDS32)
def src(u8, e1, e2, c2, g):
    cv = lambda c: c if u8 else c.replace("x < 10", "(x as i32) < 10").replace("x > y", "x > y")
    L = ["func run(x u8, y u8) i32 {", "    var a = [10, 11, 12, 13, 14, 15, 16, 17, 18, 19]",
         "    var i %s = %s" % ("u8" if u8 else "i32", show(e1, u8))]
    if e2: L += ["    if %s {" % cv(c2), "        i = %s" % show(e2, u8), "    }"]
    if g: L += ["    if %s {" % cv(g), "        return a[i]", "    }", "    return 0"]
    else: L += ["    return a[i]"]
    L += ["}", "", "func main() i32 {", "    return run(3, 4)", "}"]
    return "\n".join(L) + "\n"
def fit(v, u8):
    lo, hi = (0, 255) if u8 else (-2**31, 2**31 - 1)
    if not (lo <= v <= hi): raise Fault("narrow")
    return v
def model(u8, e1, e2, c2, g, x, y):
    i = fit(ev(e1, x, y, 0), u8)
    if e2 and eval(c2, {}, {"x": x, "y": y, "i": i}): i = fit(ev(e2, x, y, i), u8)
    if g is None or eval(g, {}, {"x": x, "y": y, "i": i}):
        if not (0 <= i < 10): raise Fault("index %d" % i)
        return 10 + i
    return 0
def main():
    # One argument, a seed: print one program. fuzz_interp takes every fuzz_*.py for a program
    # generator and runs what it prints two ways, the C against the interpreter; without this
    # mode each call failed and counted as GENERATOR-FAIL (30 per generator, 2026-10-05).
    if len(sys.argv) == 2:
        u8, e1, e2, c2, g = gen(random.Random(int(sys.argv[1])))
        sys.stdout.write(src(u8, e1, e2, c2, g)); return
    lain, N, seed = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    d = tempfile.mkdtemp(prefix="fuzz_allinputs.")
    acc = unsound = mism = safe = lost = 0
    for k in range(N):
        u8, e1, e2, c2, g = gen(random.Random(seed * 1000003 + k))
        text = src(u8, e1, e2, c2, g)
        p = os.path.join(d, "t.ln"); open(p, "w").write(text)
        r = subprocess.run([lain, p, "-o", os.path.join(d, "t.c")], capture_output=True, text=True)
        fault = None; expect = []
        for x in range(256):
            for y in range(256):
                try: expect.append(model(u8, e1, e2, c2, g, x, y))
                except Fault as f:
                    expect.append(None)
                    if fault is None: fault = "%s at x=%d y=%d" % (f, x, y)
        if not fault: safe += 1
        if r.returncode != 0:
            if not fault: lost += 1
            continue
        acc += 1
        if fault:
            unsound += 1
            print("  ★ UNSOUND: accepted, and faults (%s) (seed %d, k %d)" % (fault, seed, k))
            print("".join("      " + l + "\n" for l in text.splitlines()), end=""); continue
        c = open(os.path.join(d, "t.c")).read().replace("int main(void)", "int lain_main(void)")
        c += "\n#include <stdio.h>\nint main(void){for(int x=0;x<256;x++)for(int y=0;y<256;y++)printf(\"%d\\n\",(int)t_run((uint8_t)x,(uint8_t)y));return 0;}\n"
        open(os.path.join(d, "d.c"), "w").write(c)
        b = subprocess.run(["gcc", "-O0", "-w", "-o", os.path.join(d, "d"), os.path.join(d, "d.c")], capture_output=True, text=True)
        bad = None
        if b.returncode: bad = "the driver did not build: " + b.stderr.strip()[:160]
        else:
            out = subprocess.run([os.path.join(d, "d")], capture_output=True, text=True).stdout.split()
            if len(out) != 65536: bad = "the driver printed %d results" % len(out)
            for n, (o, ex) in enumerate(zip(out, expect)):
                if int(o) != ex: bad = "x=%d y=%d: C returned %s, the model %s" % (n // 256, n % 256, o, ex); break
        if bad:
            mism += 1
            print("  ★ MISMATCH (seed %d, k %d): %s" % (seed, k, bad))
            print("".join("      " + l + "\n" for l in text.splitlines()), end="")
    for f in os.listdir(d): os.remove(os.path.join(d, f))
    os.rmdir(d)
    print("fuzz_allinputs: seed=%d gens=%d accepted=%d safe-on-every-input=%d refused-though-safe=%d" % (seed, N, acc, safe, lost))
    print("  bugs:  UNSOUND(accepted, faults on some input)=%d  MISMATCH(C != model)=%d" % (unsound, mism))
    sys.exit(1 if unsound or mism else 0)

if __name__ == "__main__":
    main()
