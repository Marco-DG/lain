#!/usr/bin/env python3
"""Random NONDETERMINISTIC loops over two i8 variables whose paths test a sign case, judged against
GROUND TRUTH for every input and every choice.

fuzz_loops' loops are deterministic, and fuzz_cycles' are recursions: neither makes the loops a
piecewise or region ranking is for (UrbanMine-ESOP2014-Fig3: in x > 0 the pair <x, y> falls, in
x < 0 the pair <y, -x>, and one path may jump from x < 0 to x > 0, never back). Here a loop is

    while G { if C { A or A' } else { B or B' } }

or, a quarter of the time, with the else branch split by a second case `if C2 { B or B' } else { D }`

where `or` is a choice through `extern nondet()`, each alternative is one or two updates
(checked +? and -? by 1 or 2, a copy, a negation), and an alternative may end by setting one
variable anew (`x = nondet8()`). A third of the programs are Fig3 with one element changed, so the
one-way region moves and their near misses (a move back, a guard that no longer covers) are made
often, and with a GAP: the x > 0 side starting at 2 and x == 1 taking a third branch, so regions
x >= 2 and x <= -1 leave x = 1 uncovered (rank.cover; without the third branch the loop guard always
closed the gap, and a mutant dropping both cover checks survived). Every input and every choice is a run: the program can run for ever iff the graph of the
states (x, y) where G holds, one edge per alternative, has a cycle. A trapped update ends the run.
A "set anew" goes through an auxiliary node joined to every value of that variable, so the search
stays linear in the edges. That is decided here exactly. The compiler accepting such a loop
(exit 0: its termination obligation discharged) is UNSOUND.

LAIN_FLAGS adds compiler flags (`--judge=authority` runs the judge's verdict, not the range
analysis's). Not a program generator for fuzz_interp: its programs call externs, so they do not
link; fuzz_interp skips this file by name.

    fuzz_regions.py LAIN N SEED [KEEPDIR]
"""
import random, subprocess, sys, os, tempfile

GUARDS = ["x != 0", "y > 0", "x > 0", "x < 0", "y != 0", "x > y", "y < 100", "x > -100", "x < 100"]
CASES = ["x > 0", "x < 0", "x >= 0", "y > 0", "x > y", "x < y"]
STEPS = ["x = x -? 1", "x = x +? 1", "y = y -? 1", "y = y +? 1", "x = x -? 2", "y = y +? 2",
         "x = y", "y = x", "x = 0 -? x"]
ANEW = ["x = nondet8()", "y = nondet8()"]

FIG3 = (["x != 0", "y > 0"], "x > 0",
        [["x = x -? 1", "y = nondet8()"], ["y = y -? 1"]],
        [["x = x +? 1"], ["y = y -? 1", "x = nondet8()"]])
def as_prog(f): return (f[0], [f[1]], [f[2], f[3]])

def alt(rng):
    a = [rng.choice(STEPS) for _ in range(rng.choice([1, 1, 2]))]
    if rng.random() < 0.35: a.append(rng.choice(ANEW))
    return a

def gen(rng):
    if rng.random() < 1 / 3:
        gs, c, th, el = FIG3
        gs, th, el = list(gs), [list(a) for a in th], [list(a) for a in el]
        what = rng.choice(["guard", "case", "step", "step", "anew", "drop"])
        if what == "guard": gs[rng.randrange(len(gs))] = rng.choice(GUARDS)
        elif what == "case": c = rng.choice(CASES)
        elif what in ("step", "anew"):
            a = rng.choice(th + el)
            if what == "anew": a.append(rng.choice(ANEW)) if a[-1] not in ANEW else a.pop()
            else:
                i = rng.randrange(len(a))
                if a[i] not in ANEW: a[i] = rng.choice(STEPS)
        else:
            b = rng.choice([th, el])
            if len(b) == 2: b.pop(rng.randrange(2))
        if rng.random() < 0.3:
            # a GAP between the regions: the x > 0 side starts at 2, and x == 1 takes a third branch
            # that stays put or does something else; regions x >= 2 and x <= -1 then leave x = 1
            # uncovered (rank.cover), which no two-branch program here can make
            third = [["y = y"]] if rng.random() < 0.5 else [alt(rng)]
            return gs, ["x > 1" if c == "x > 0" else c, "x < 0"], [th, el, third]
        return gs, [c], [th, el]
    gs = rng.sample(GUARDS, rng.choice([1, 2, 2, 3]))
    th = [alt(rng) for _ in range(rng.choice([1, 2, 2]))]
    el = [alt(rng) for _ in range(rng.choice([1, 2, 2]))]
    c = rng.choice(CASES)
    if rng.random() < 0.25:
        return gs, [c, rng.choice(CASES)], [th, el, [alt(rng) for _ in range(rng.choice([1, 2]))]]
    return gs, [c], [th, el]

def line(u):
    if "?" in u: return u + ' else panic("overflow")'
    return u

def src(gs, cs, bs):
    L = ["extern func nondet() i32 effects io", "extern func nondet8() i8 effects io", "",
         "func task(x0 i8, y0 i8) i8 effects io, raises {", "    var x = x0", "    var y = y0",
         "    while " + " and ".join("(%s)" % g for g in gs) + " {"]
    def branch(alts, ind):
        if len(alts) == 1:
            return [ind + line(u) for u in alts[0]]
        out = [ind + "if nondet() != 0 {"] + [ind + "    " + line(u) for u in alts[0]]
        out += [ind + "} else {"] + [ind + "    " + line(u) for u in alts[1]] + [ind + "}"]
        return out
    # if c0 { b0 } else { if c1 { b1 } else { b2 } }
    ind = "        "
    for i, c in enumerate(cs):
        L += [ind + "if " + c + " {"] + branch(bs[i], ind + "    ") + [ind + "} else {"]
        ind += "    "
    L += branch(bs[-1], ind)
    for i in range(len(cs)):
        ind = ind[:-4]; L.append(ind + "}")
    L += ["    }", "    return x", "}"]
    return "\n".join(L) + "\n"

def can_run_forever(gs, cs, bs):
    guard = eval("lambda x, y: " + " and ".join("(%s)" % g for g in gs))
    cases = [eval("lambda x, y: " + c) for c in cs]
    def run(alts):
        fs = []
        for a in alts:
            ops = []
            for u in a:
                var, rhs = u.split(" = ")
                if rhs == "nondet8()": ops.append((var, None)); continue
                ops.append((var, eval("lambda x, y: " + rhs.replace("?", ""))))
            fs.append(ops)
        return fs
    BR = [run(b) for b in bs]
    def pick(x, y):
        for i, cf in enumerate(cases):
            if cf(x, y): return BR[i]
        return BR[-1]
    NS = 65536; AX = NS; AY = NS + 256          # AX + (y+128): x anew; AY + (x+128): y anew
    idx = lambda x, y: ((x + 128) << 8) | (y + 128)
    succ = [None] * (NS + 512)
    for x in range(-128, 128):
        for y in range(-128, 128):
            if not guard(x, y): continue
            out = []
            for ops in pick(x, y):
                a, b, dead = x, y, False
                for (var, f) in ops:
                    if f is None:
                        out.append(AX + (b + 128) if var == "x" else AY + (a + 128)); dead = True; break
                    v = f(a, b)
                    if not -128 <= v <= 127: dead = True; break        # the trap ends the run
                    if var == "x": a = v
                    else: b = v
                if not dead and guard(a, b): out.append(idx(a, b))
            succ[idx(x, y)] = out
    for k in range(256):
        succ[AX + k] = [idx(v, k - 128) for v in range(-128, 128) if guard(v, k - 128)]
        succ[AY + k] = [idx(k - 128, v) for v in range(-128, 128) if guard(k - 128, v)]
    color = bytearray(NS + 512)                 # 0 unseen, 1 on the DFS stack, 2 done
    for s0 in range(NS + 512):
        if color[s0] or not succ[s0]: continue
        stack = [(s0, 0)]; color[s0] = 1
        while stack:
            n, i = stack[-1]
            es = succ[n] or []
            if i < len(es):
                stack[-1] = (n, i + 1); m = es[i]
                if color[m] == 1: return True
                if color[m] == 0: color[m] = 1; stack.append((m, 0))
            else:
                color[n] = 2; stack.pop()
    return False

def main():
    lain, N, seed = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    keep = sys.argv[4] if len(sys.argv) > 4 else None
    # The oracle's own teeth, every run: Fig3 ends from every input and choice; with (a) stepping
    # x by 2, x = 1 jumps to x = -1, whence (d) sets x anew to 1 and (a) sets y back: a cycle.
    # And with a third branch at x == 1 that stays put (from x = 2, (a) reaches it): a cycle too.
    back = (FIG3[0], FIG3[1], [["x = x -? 2", "y = nondet8()"], ["y = y -? 1"]], FIG3[3])
    gap = (FIG3[0], ["x > 1", "x < 0"], [FIG3[2], FIG3[3], [["y = y"]]])
    if can_run_forever(*as_prog(FIG3)) or not can_run_forever(*as_prog(back)) or not can_run_forever(*gap):
        print("  ★ ORACLE BROKEN: Fig3, its two-step variant or its gap variant decided wrongly"); sys.exit(1)
    tmp = tempfile.mkdtemp(prefix="fuzz_regions.")
    acc = ref = other = internal = unsound = term = lost = fig3 = 0
    for k in range(N):
        prog = gen(random.Random(seed * 1000003 + k))
        p = os.path.join(tmp, "t.ln"); text = src(*prog); open(p, "w").write(text)
        r = subprocess.run([lain, p, "-o", os.path.join(tmp, "t.c")] + os.environ.get("LAIN_FLAGS", "").split(),
                           capture_output=True, text=True)
        forever = can_run_forever(*prog)
        if not forever: term += 1
        if r.returncode == 0:
            acc += 1
            if forever:
                unsound += 1
                print("  ★ UNSOUND: accepted, and runs for ever from some input (seed %d, k %d)" % (seed, k))
                print("".join("      " + l + "\n" for l in text.splitlines()), end="")
                if keep: open(os.path.join(keep, "unsound_%d_%d.ln" % (seed, k)), "w").write(text)
        elif r.returncode == 70 or r.returncode < 0 or "internal error" in r.stderr:
            # the compiler failing (an internal error, a signal) is no verdict on the program: until
            # 2026-10-14 it counted as refused-other, and 31 duplicate-ID refusals hid there
            internal += 1
            print("  ★ INTERNAL: exit %d (seed %d, k %d): %s" % (r.returncode, seed, k, r.stderr.strip().splitlines()[0][:160] if r.stderr.strip() else ""))
        elif "[E011]" in r.stderr or "[E082]" in r.stderr:
            ref += 1
            if not forever: lost += 1
        else:
            other += 1
            if keep: open(os.path.join(keep, "other_%d_%d.ln" % (seed, k)), "w").write(text + "\n// " + r.stderr.replace("\n", "\n// "))
    for f in os.listdir(tmp): os.remove(os.path.join(tmp, f))
    os.rmdir(tmp)
    print("fuzz_regions: seed=%d gens=%d accepted=%d refused-termination=%d refused-other=%d" % (seed, N, acc, ref, other))
    print("  ground truth: %d end from every input and choice; %d of those refused (the cost)" % (term, lost))
    print("  bugs:  UNSOUND(accepted, runs for ever)=%d INTERNAL(exit 70 or a signal)=%d" % (unsound, internal))
    if other > N // 2:
        print("  ★ FUZZER DID NOT RUN: %d/%d refused for another reason" % (other, N)); sys.exit(1)
    sys.exit(1 if unsound or internal else 0)

if __name__ == "__main__":
    main()
