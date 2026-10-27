#!/usr/bin/env python3
"""solver_harness.py — the untrusted solver (src/solver/lia.h) against an INDEPENDENT kernel and
brute force. run_ir_tests.sh runs the gate form.

  solver_harness.py SOLVE_VC                       the gate: the worked examples, the sliced retry's
                                                   cases, then fixed seeds
  solver_harness.py SOLVE_VC N SEED [bounded|free] one random run
  solver_harness.py --check VCS ANSWERS [MODELS]   check solve_vc's answers to a file of VCs, with
                                                   counts per obligation kind; MODELS lists the
                                                   not-implied ones with their models

Generates N random VCs (seeded), runs SOLVE_VC on them, and checks every answer:
  - a certificate with this file's own kernel (judge_interfaces.md §4: lin signs, rnd, split,
    case, close, scoping, unique numbers), written from the doc, sharing nothing with the C;
  - a model by evaluating every hypothesis and g at it;
  - in `bounded` mode every variable has a box [-B, B] as hypotheses, and brute force over the box
    decides `hyps and g`: a certificate must meet UNSAT, a model SAT, and a refusal is WRONG.
    Branch and bound over a box always ends, so a solver that refuses here has lost strength (a
    mutant that splits the same point forever did exactly that, and only a count saw it). The
    budget is a few times the most a correct solver used on these seeds, so such a mutant stops.
The WIDE search (lia.h's last retry, s3, PE.13) is checked three ways: its LSW arithmetic against
Python's integers (`solve_vc --lsw`, with the kernel's own bound and multiply rule), its answers
byte for byte the 128-bit search's wherever that one finishes (`solve_vc --wide`), and on VCs with
coefficients near 2^60, where the retry answers, by this kernel and brute force.
lia_cone (lia.h, s2: a system closed under scaling, by one rational search) is checked with
`solve_vc --cone`, on worked shapes and seeded random systems, every answer by this kernel.
The DIOPHANTINE STEP (lia_core.h's ls_dioph: equalities with no integer solution, refuted before a
split) is checked on worked shapes and on two seeded sets of parity-shaped VCs (coefficients of 2
and 3, residues of small domains), one boxed (brute force) and one not, and its answers at both
widths alike.
The SLICED RETRY (lia.h, PE.13) is checked on solver_capacity/: VCs whose whole search stops on
CAPACITY. A certificate from a slice must pass this kernel against the WHOLE VC, and where no slice
holds the proof the answer must stay CAPACITY, never a model of a slice.
Prints the counts; exit 1 on any wrong answer. The kernel here is written from the format's
document, in another language, and shares nothing with the C: a solver bug that the C kernel
would also miss still has to get past this one.
"""
import random, subprocess, sys, itertools, math, tempfile, os, re

def fmt_sum(terms, names):
    ts = [(v, c) for v, c in sorted(terms.items()) if c]
    if not ts: return "0"
    return " + ".join(f"{c}*{names[v]}" for v, c in ts)

def gen_cons(rng, nv, ops):
    k = rng.randint(1, nv)
    vs = sorted(rng.sample(range(nv), k))
    terms = {v: rng.choice([c for c in range(-4, 5) if c]) for v in vs}
    return (terms, rng.choice(ops), rng.randint(-8, 8))

def gen_parity_vc(rng, idx, bounded, B):
    # equalities whose integer solutions a coefficient of 2 or 3 thins out (x = 2q + r, a residue of
    # a small domain), beside unit inequalities: the shapes lia_core.h's Diophantine step decides,
    # which the uniform sets draw rarely
    nv = rng.randint(4, 5)
    names = [f"x{i}" for i in range(nv)]
    hyps = []
    for v in range(nv):
        if bounded: hyps += [("type", ({v: -1}, "<=", B)), ("type", ({v: 1}, "<=", B))]
    if not bounded:
        for v in rng.sample(range(nv), rng.randint(0, 2)): hyps += [("def", ({v: 1}, "<=", 1)), ("def", ({v: -1}, "<=", 0))]
    pick = lambda k: {v: rng.choice([1, -1, 2, -2, 3, -3, 1, -1]) for v in rng.sample(range(nv), k)}
    for _ in range(rng.randint(1, 3)): hyps.append(("def", (pick(rng.randint(2, 3)), "=", rng.randint(-3, 3))))
    for _ in range(rng.randint(0, 3)):
        hyps.append(("guard", ({v: rng.choice([1, -1]) for v in rng.sample(range(nv), 2)}, "<=", rng.randint(-2, 2))))
    if rng.random() < 0.3: hyps.append(("path", [[(pick(1), "<=", rng.randint(-1, 1))], [(pick(2), "=", rng.randint(-2, 2))]]))
    goal = ({}, "<=", -1) if rng.random() < 0.5 else (pick(2), "<=", rng.randint(-2, 2))
    return {"id": f"p{idx} 0 bb0.0 parity 0", "names": names, "hyps": hyps, "goal": goal}

def gen_vc(rng, idx, bounded, B):
    nv = rng.randint(1, 4)
    names = [f"x{i}" for i in range(nv)]
    hyps = []
    for v in range(nv):
        if bounded or rng.random() < 0.3:
            hyps.append(("type", ({v: -1}, "<=", B)))
            hyps.append(("type", ({v: 1}, "<=", B)))
    for _ in range(rng.randint(0, 4)):
        hyps.append(("def", gen_cons(rng, nv, ["<=", "<=", "<=", "="])))
    if rng.random() < 0.35:
        dis = [[gen_cons(rng, nv, ["<=", "<=", "="]) for _ in range(rng.randint(1, 2))] for _ in range(rng.randint(2, 3))]
        hyps.append(("path", dis))
    r = rng.random()
    if r < 0.15: goal = ({}, "<=", -1)
    elif r < 0.35: goal = gen_cons(rng, nv, ["!="])
    else: goal = gen_cons(rng, nv, ["<="])
    return {"id": f"r{idx} 0 bb0.0 rand 0", "names": names, "hyps": hyps, "goal": goal}

def vc_text(vc):
    L = [f"vc {vc['id']}"] + [f"  var {n}" for n in vc["names"]]
    for i, (origin, h) in enumerate(vc["hyps"], 1):
        if isinstance(h, list):
            ds = " | ".join(" , ".join(f"{fmt_sum(t, vc['names'])} {o} {b}" for t, o, b in d) for d in h)
            L.append(f"  hyp h{i} path : or {{ {ds} }}")
        else:
            t, o, b = h
            L.append(f"  hyp h{i} {origin} : {fmt_sum(t, vc['names'])} {o} {b}")
    t, o, b = vc["goal"]
    L.append(f"  goal {fmt_sum(t, vc['names'])} {o} {b}")
    L.append("end")
    return "\n".join(L) + "\n"

# ── semantics ──────────────────────────────────────────────────────────────────────────────────
def holds(c, x):
    t, o, b = c
    s = sum(a * x[v] for v, a in t.items())
    return s <= b if o == "<=" else s == b if o == "=" else s != b

def g_of(vc):
    t, o, b = vc["goal"]
    if o == "<=": return ({v: -a for v, a in t.items()}, "<=", -b - 1)
    return (dict(t), "=", b)

def sat_at(vc, x):
    for origin, h in vc["hyps"]:
        if isinstance(h, list):
            if not any(all(holds(c, x) for c in d) for d in h): return False
        elif not holds(h, x): return False
    return holds(g_of(vc), x)

def brute(vc, B):
    for x in itertools.product(range(-B, B + 1), repeat=len(vc["names"])):
        if sat_at(vc, x): return x
    return None

# ── the independent kernel ─────────────────────────────────────────────────────────────────────
class Bad(Exception): pass

def parse_sum(tokens, names):
    if tokens == ["0"]: return {}
    t = {}
    for i, w in enumerate(tokens):
        if i % 2 == 1:
            if w != "+": raise Bad("sum joined by +")
            continue
        c, n = w.split("*", 1)
        v = names.index(n)
        if v in t: raise Bad("duplicate variable")
        t[v] = int(c)
    return t

def check_cert(vc, lines):
    names = vc["names"]
    base = {}
    for i, (origin, h) in enumerate(vc["hyps"], 1):
        if not isinstance(h, list): base[f"h{i}"] = h
    base["g"] = g_of(vc)
    used_f, used_s = set(), set()
    pos = [0]
    def depth(l): return (len(l) - len(l.lstrip(" "))) // 2
    def branch(d, facts):
        facts = dict(facts)
        while True:
            if pos[0] >= len(lines): raise Bad("branch does not end")
            l = lines[pos[0]]
            if depth(l) != d: raise Bad(f"level {depth(l)} where {d} expected: {l!r}")
            w = l.split(); pos[0] += 1
            if len(w) >= 3 and w[1] == "=" and w[0][0] == "f":
                name = w[0]
                if name in used_f: raise Bad(f"{name} defined twice")
                used_f.add(name)
                if w[2] == "rnd":
                    src = facts.get(w[3])
                    if src is None or not w[3].startswith("f"): raise Bad(f"rnd of {w[3]} out of scope")
                    t, o, b = src
                    g = 0
                    for a in t.values(): g = math.gcd(g, abs(a))
                    if g == 0: raise Bad("rnd of a fact with no variables")
                    facts[name] = ({v: a // g for v, a in t.items()}, "<=", b // g)
                    continue
                if w[2] != "lin": raise Bad("unknown derivation")
                acc, const = {}, 0
                for i, term in enumerate(w[3:]):
                    if i % 2 == 1:
                        if term != "+": raise Bad("lin joined by +")
                        continue
                    m, ref = term.split("*", 1); m = int(m)
                    if m == 0: raise Bad("zero multiplier")
                    if ref not in facts: raise Bad(f"{ref} out of scope")
                    t, o, b = facts[ref]
                    if o == "<=" and m < 0: raise Bad(f"negative multiplier on inequality {ref}")
                    for v, a in t.items(): acc[v] = acc.get(v, 0) + m * a
                    const += m * b
                facts[name] = ({v: a for v, a in acc.items() if a}, "<=", const)
                continue
            if w[0] == "close":
                t, o, b = facts.get(w[1], (None, None, None))
                if t is None or not w[1].startswith("f"): raise Bad(f"close of {w[1]}")
                if t or b >= 0: raise Bad(f"close of {w[1]}: not 0 <= negative ({t}, {b})")
                return
            if w[0] == "split":
                s = w[1]
                if s in used_s: raise Bad(f"{s} twice")
                used_s.add(s)
                i = w.index("<=")
                t = parse_sum(w[3:i], names); c = int(w[i + 1])
                for lab, fact in (("le:", (t, "<=", c)), ("gt:", ({v: -a for v, a in t.items()}, "<=", -c - 1))):
                    if pos[0] >= len(lines) or lines[pos[0]].strip() != lab or depth(lines[pos[0]]) != d: raise Bad(f"{lab} missing")
                    pos[0] += 1
                    f2 = dict(facts); f2[s] = fact
                    branch(d + 1, f2)
                return
            if w[0] == "case":
                s, h = w[1], w[3]
                if s in used_s: raise Bad(f"{s} twice")
                used_s.add(s)
                hi = int(h[1:])
                origin, hy = vc["hyps"][hi - 1]
                if not isinstance(hy, list): raise Bad("case on a non-disjunctive hypothesis")
                for n, dis in enumerate(hy, 1):
                    if pos[0] >= len(lines) or lines[pos[0]].strip() != f"{n}:" or depth(lines[pos[0]]) != d: raise Bad(f"branch {n}: missing")
                    pos[0] += 1
                    f2 = dict(facts)
                    for q, c in enumerate(dis, 1): f2[f"{s}.{q}"] = c
                    branch(d + 1, f2)
                if pos[0] < len(lines) and lines[pos[0]].strip() == f"{len(hy) + 1}:": raise Bad("extra case branch")
                return
            raise Bad(f"unknown step {l!r}")
    branch(1, base)
    if pos[0] != len(lines): raise Bad("trailing steps")

# The worked examples of judge_interfaces.md §4 (and three more shapes), with the answer kind each
# must get: a certificate where the goal is implied, a model where it is not.
WORKED = [
    ("guard", "cert", ["%v3", "%v8"], [("type", ({0: -1}, "<=", 0)), ("objsize", ({1: 1}, "<=", 9223372036854775807)),
                                       ("guard", ({0: 1, 1: -1}, "<=", -1))], ({0: 1, 1: -1}, "<=", -1)),
    ("parity", "cert", ["x", "y"], [("def", ({0: 2, 1: -2}, "=", 1))], ({}, "<=", -1)),
    ("bb", "cert", ["x"], [("def", ({0: 2}, "<=", 1)), ("def", ({0: -2}, "<=", -1))], ({}, "<=", -1)),
    ("notimplied", "model", ["%v3", "%v8"], [("type", ({0: -1}, "<=", 0)), ("objsize", ({1: 1}, "<=", 9223372036854775807)),
                                            ("guard", ({0: 1, 1: -1}, "<=", 0))], ({0: 1, 1: -1}, "<=", -1)),
    ("divisor", "cert", ["y"], [("path", [[({0: 1}, "<=", -1)], [({0: -1}, "<=", -1)]])], ({0: 1}, "!=", 0)),
    ("strip", "model", ["x", "y"], [("def", ({0: 3, 1: 3}, "<=", 4)), ("def", ({0: -3, 1: -3}, "<=", -2))], ({}, "<=", -1)),
    # the Diophantine step (lia_core.h): equalities with no integer solution together, which a split
    # on an unbounded quotient never refutes. x = 2q, y = 2p + 1, y = x + 2;
    ("parity_chain", "cert", ["x", "q", "y", "p"], [("def", ({0: 1, 1: -2}, "=", 0)), ("def", ({2: 1, 3: -2}, "=", 1)),
                                                    ("path", ({0: -1, 2: 1}, "=", 2))], ({}, "<=", -1)),
    # a remainder fixed at 1 only through a constant's equality (t >= c + 1, c = 0, t <= 1);
    ("parity_fixed", "cert", ["x", "q", "p", "t", "c"], [("def", ({0: 1, 1: -2}, "=", 0)), ("def", ({0: 1, 2: -2, 3: -1}, "=", 0)),
                                                         ("def", ({4: 1}, "=", 0)), ("guard", ({3: -1, 4: 1}, "<=", -1)),
                                                         ("rem", ({3: 1}, "<=", 1))], ({}, "<=", -1)),
    # Kroening's odd path: x odd makes its residue r 1 (a split on r, its domain [0, 1]), r <= u
    # makes u 1, and y = x - 1 is even;
    ("residue_odd", "cert", ["x", "q", "r", "p", "y", "s", "u"],
     [("def", ({0: 1, 1: -2, 2: -1}, "=", 0)), ("def", ({2: -1}, "<=", 0)), ("def", ({2: 1}, "<=", 1)),
      ("path", ({0: 1, 3: -2}, "=", 1)), ("path", ({0: -1, 4: 1}, "=", -1)),
      ("def", ({4: 1, 5: -2, 6: -1}, "=", 0)), ("def", ({6: -1}, "<=", 0)), ("def", ({6: 1}, "<=", 1)),
      ("premise", ({2: 1, 6: -1}, "<=", 0))], ({}, "<=", -1)),
    # and the same chain with y even: satisfiable, a model
    ("parity_sat", "model", ["x", "q", "y", "p"], [("def", ({0: 1, 1: -2}, "=", 0)), ("def", ({2: 1, 3: -2}, "=", 0)),
                                                   ("path", ({0: -1, 2: 1}, "=", 2))], ({}, "<=", -1)),
]

LIMITS = ["40000", "512", "512"]     # pivots, nodes, leaves; the seeds below need 6534, 155, 27
# A run that does not finish is a FAILURE, not a slow pass: a mutant that splits the same point
# forever spends its whole node budget on every VC, over ever-deeper tableaux, and only a clock
# sees that. A correct solver runs each set below in a few seconds.
TIMEOUT = 120

def run(exe, vcs, bounded, B, expect=None, limits=None, cone=False):
    with tempfile.NamedTemporaryFile("w", suffix=".vc", delete=False) as f:
        for vc in vcs: f.write(vc_text(vc))
        path = f.name
    try:
        out = subprocess.run([exe] + (["--cone"] if cone else []) + [path] + (limits or LIMITS), capture_output=True, text=True, timeout=TIMEOUT)
    except subprocess.TimeoutExpired:
        os.unlink(path); print(f"  WRONG: the solver did not finish {len(vcs)} VCs within {TIMEOUT} s"); return 1
    os.unlink(path)
    if out.returncode != 0: print("solver exit", out.returncode, out.stderr); return 1
    lines = out.stdout.split("\n")
    i = 0; counts = {}; bad = 0; work = {"pivot": 0, "node": 0, "leaf": 0}
    def count(k): counts[k] = counts.get(k, 0) + 1
    for vc in vcs:
        head = lines[i]; i += 1
        body = []
        if head.startswith("cert ") or head.startswith("model "):
            while lines[i] != "end": body.append(lines[i]); i += 1
            i += 1
        wl = lines[i]; i += 1
        for kv in wl.split()[1:]:
            k, v = kv.split(":"); work[k] = max(work[k], int(v))
        truth = brute(vc, B) if bounded else "unknown"
        try:
            if expect and not head.startswith(expect[vcs.index(vc)] + " "):
                raise Bad(f"expected a {expect[vcs.index(vc)]}, got {head!r}")
            if head.startswith("cert "):
                check_cert(vc, body)
                if bounded and truth is not None: raise Bad(f"certificate for a SAT VC (witness {truth})")
                count("cert")
            elif head.startswith("model "):
                x = [int(l.split(" = ")[1]) for l in body]
                if not sat_at(vc, x): raise Bad(f"model {x} does not satisfy")
                count("model")
            else:
                if bounded: raise Bad(f"refused ({head.split()[0]}) on a bounded VC, which is {'UNSAT' if truth is None else 'SAT'}")
                count(head.split()[0])
        except Bad as e:
            bad += 1
            if bad <= 5: print("WRONG:", e); print(vc_text(vc) + head + "\n" + "\n".join(body))
    print("  " + ", ".join(f"{k} {v}" for k, v in sorted(counts.items())) + f"; wrong {bad}; max work {work}")
    run.work = work
    return 1 if bad else 0

# ── the retries (lia.h, PE.13): the slices (s1) and the wide search (s3) ──────────────────────
# Each VC's 128-bit search stops on CAPACITY (a product past 128 bits), and the answer each must get.
# Every certificate is checked by this kernel against the WHOLE VC, every model at every row.
CAPACITY_CASES = [
    # NestedRecursion's rec1, the post VC of its return claims, two claim sets: S_2, 20 and 26 of
    # the facts, is refuted at 128 bits in 10 pivots (s1)
    ("rec1_0x17.vc", ["cert"]), ("rec1_0x7f.vc", ["cert"]),
    # the same with five claims: no slice below the whole VC holds the proof, and the wide search
    # refutes the whole VC (it stores nothing past 125 bits) (s3)
    ("rec1_0x7c.vc", ["cert"]),
    # valid only through a part the goal never reaches (rec1's refutation over other variables);
    # the wide search refutes that part
    ("twoparts.vc", ["cert"]),
    # the proof is a case split on h36, after 35 facts the slice leaves out: the `case` names the
    # disjunction by its index in the WHOLE VC, which this kernel reads
    ("caselast.vc", ["cert"]),
    # twoparts with its constants near 2^90: valid only through the far part, which needs 361-bit
    # numbers and overflows at 512 bits too, while the goal's own part is satisfiable: CAPACITY,
    # never the model y = 4 of a slice
    ("deep90.vc", ["capacity"]),
    # a valid VC whose refutation multiplies 2^250 + 1 by 2^300: the wide search finds it, but the
    # kernel's 512-bit sums cannot follow it (it would refuse it, a solver-bug record), so CAPACITY
    ("kernelwidth.vc", ["capacity"]),
    # x = y = z = 2^251 satisfies every row, and the wide search reaches it without passing 2^511
    # (Bland's rule raises x, then z, then y), but the judge sums h4 in variable order, x's term
    # and y's first: 2^512, past its 512 bits, a model it cannot evaluate (a solver-bug record), so
    # CAPACITY
    ("modelwidth.vc", ["capacity"]),
    # udiv(x u64)'s bound checks (tests/trust/policy_op_literal_left_pass.ln), which store 129 bits
    ("udiv_u64.vc", ["cert", "model", "cert", "cert", "cert", "model", "cert", "cert"]),
    # a path check of 4NestedWith3Variables' (s2's fallback), 299 rows and 168 variables: the wide
    # search finds its model only after 1958 pivots, past LS_WIDE_WORK (2^24 entry updates, 118
    # pivots on this tableau), so lia_solve answers CAPACITY, never the model (checked below:
    # --wide, uncapped, does find it)
    ("wide_long.vc", ["capacity"]),
]
# Gothenburg-1's sample LP (s2's fallback): 110 samples, 182-bit numbers, through lia_cone
CONE_CAPACITY_CASES = [("gothenburg1_samples.vc", ["model"])]

# ── lia_cone (lia.h, s2): a system closed under scaling, by one rational search, its model scaled ──
# Each answer is checked as any other (a certificate by this kernel, a model at every row); the
# worked ones pin the shapes: a model only scaling gives (x >= 1/2 and 3y = x: the numerators (1, 1)
# break 3y = x, and the answer must be (3, 1) or another integer point), a refutation, and systems
# lia_cone must not accept (a positive `<=` constant, an `=` with a nonzero one, a disjunction, a
# goal other than `0 <= -1`), each `capacity`.
CONE_WORKED = [
    ("scaled", "model", ["x", "y"], [("sample", ({0: -2}, "<=", -1)), ("sample", ({0: -1, 1: 3}, "=", 0))], ({}, "<=", -1)),
    ("refuted", "cert", ["c"], [("sample", ({0: 1}, "<=", -1)), ("sample", ({0: -1}, "<=", -1))], ({}, "<=", -1)),
    ("positive", "capacity", ["x"], [("sample", ({0: 1}, "<=", 1))], ({}, "<=", -1)),
    ("eqconst", "capacity", ["x"], [("sample", ({0: 1}, "=", -2))], ({}, "<=", -1)),
    ("disj", "capacity", ["x"], [("path", [[({0: 1}, "<=", -1)], [({0: -1}, "<=", -1)]])], ({}, "<=", -1)),
    ("goal", "capacity", ["x"], [("sample", ({0: 1}, "<=", -1))], ({0: 1}, "<=", 0)),
]
def gen_cone(rng, idx):
    nv = rng.randint(1, 4)
    hyps = []
    for _ in range(rng.randint(1, 6)):
        k = rng.randint(1, nv); vs = sorted(rng.sample(range(nv), k))
        terms = {v: rng.choice([c for c in range(-6, 7) if c]) for v in vs}
        if rng.random() < 0.2: hyps.append(("sample", (terms, "=", 0)))
        else: hyps.append(("sample", (terms, "<=", rng.randint(-5, 0))))
    return {"id": f"c{idx} 0 bb0.0 cone 0", "names": [f"x{i}" for i in range(nv)], "hyps": hyps, "goal": ({}, "<=", -1)}
def run_cone(exe):
    vcs = [{"id": f"{n} 0 bb0.0 cone 0", "names": names, "hyps": hyps, "goal": goal} for n, _, names, hyps, goal in CONE_WORKED]
    rc = run(exe, vcs, False, 4, [k for _, k, _, _, _ in CONE_WORKED], cone=True)
    rng = random.Random(5)
    print("  seed 5, 1000 systems closed under scaling:")
    rc |= run(exe, [gen_cone(rng, i) for i in range(1000)], False, 4, cone=True)
    return rc

# ── s3: the wide search against the 128-bit one, and LSW against Python's integers ─────────────
def answers(exe, vcs, flag=None, limits=None):
    """solve_vc's answer blocks, one per VC (the head line through the work line)"""
    with tempfile.NamedTemporaryFile("w", suffix=".vc", delete=False) as f:
        for vc in vcs: f.write(vc_text(vc))
        path = f.name
    out = subprocess.run([exe] + ([flag] if flag else []) + [path] + (limits or LIMITS), capture_output=True, text=True, timeout=TIMEOUT)
    os.unlink(path)
    blocks, cur = [], []
    for l in out.stdout.split("\n"):
        if not l: continue
        cur.append(l)
        if l.startswith("work "): blocks.append("\n".join(cur)); cur = []
    return out.returncode, blocks
def run_wide_property(exe):
    # The two searches take the same steps, so wherever the 128-bit one finishes (anything but
    # capacity) the wide one must print the same block, byte for byte, its work included. At 4000
    # pivots: the wide search is about ten times slower, and a budget stop must match too.
    rc = 0
    lim = ["4000", "512", "512"]
    for seed, mode in ((1, "bounded"), (2, "bounded"), (3, "free"), (5, "parity bounded"), (6, "parity free")):
        rng = random.Random(seed)
        par = mode.startswith("parity")
        vcs = [(gen_parity_vc(rng, i, mode.endswith("bounded"), 2) if par else gen_vc(rng, i, mode == "bounded", 4)) for i in range(400 if par else 1000)]
        r1, a = answers(exe, vcs, None, lim); r2, b = answers(exe, vcs, "--wide", lim)
        if r1 or r2 or len(a) != len(vcs) or len(b) != len(vcs):
            print(f"  WRONG: seed {seed}: exits {r1} {r2}, {len(a)} and {len(b)} answers of {len(vcs)}"); rc = 1; continue
        cmp = [i for i in range(len(vcs)) if not a[i].startswith("capacity ")]
        diff = [i for i in cmp if a[i] != b[i]]
        print(f"  seed {seed}, {mode}: {len(cmp)} finished at 128 bits, the wide search's the same on {len(cmp) - len(diff)}")
        for i in diff[:3]: print("  WRONG: differs:\n" + a[i] + "\n--- wide:\n" + b[i])
        if diff: rc = 1
    return rc
def gen_big(rng, idx, B, e):
    # each term's coefficient drawn near 2^e on its own (a common factor would let the gcd test
    # divide the row back to small numbers), each constant likewise; the box decides it
    vc = gen_vc(rng, idx, True, B)
    big = lambda: rng.choice([-1, 1]) * rng.randint(2 ** (e - 2), 2 ** (e + 2))
    hyps = []
    for origin, h in vc["hyps"]:
        if isinstance(h, list) or origin == "type": hyps.append((origin, h)); continue
        t, o, b = h
        hyps.append((origin, ({v: big() for v in t}, o, big() if o == "<=" else 0)))
    vc["hyps"] = hyps
    return vc
def run_wide_big(exe):
    # Coefficients near 2^60: the search's products pass 128 bits on 56 of these 300 (the solver
    # before s3 answered `capacity` there), so lia_solve's wide retry answers. Each answer is checked
    # by this kernel (a certificate), at every row (a model), and against brute force over the box
    # (a refusal on a boxed VC is WRONG, as in `bounded` mode).
    rng = random.Random(6)
    vcs = [gen_big(rng, i, 3, 60) for i in range(300)]
    return run(exe, vcs, True, 3)
def lsw_expected(op, a, b):
    LIM = 2 ** (32 * 16)                                   # J_LIMBS 32-bit limbs, the kernel's bound
    limbs = lambda x: (abs(x).bit_length() + 31) // 32
    if op == "add": r = a + b; return None if abs(r) >= LIM else r
    if op == "sub": r = a - b; return None if abs(r) >= LIM else r
    if op == "mul":
        if a == 0 or b == 0: return 0
        if limbs(a) + limbs(b) - 1 > 16: return None        # kint_mul's pre-check
        r = a * b; return None if abs(r) >= LIM else r
    if op == "div": q = abs(a) // abs(b); return q if (a < 0) == (b < 0) else -q
    if op == "floordiv": return a // b
    if op == "gcd": return math.gcd(a, b)
    if op == "cmp": return (a > b) - (a < b)
    if op == "divides": return int(a % b == 0)
def run_lsw(exe):
    rng = random.Random(7)
    widths = [1, 31, 32, 33, 63, 64, 65, 127, 128, 129, 255, 256, 257, 480, 511]
    num = lambda: rng.choice([-1, 1]) * (rng.getrandbits(rng.choice(widths)) | 1)
    cases = []
    for _ in range(4000):
        op = rng.choice(["add", "sub", "mul", "div", "floordiv", "gcd", "cmp", "divides"])
        a, b = num(), num()
        if op == "floordiv": b = abs(b)
        if rng.random() < 0.1: a = b * rng.choice([-3, -1, 1, 2, 7])   # exact quotients, equal values
        if abs(a) >= 2 ** 512: a = a % (2 ** 511)
        cases.append((op, a, b))
    cases += [("add", 2 ** 512 - 1, 1), ("mul", 2 ** 256, 2 ** 255), ("mul", 2 ** 255, 2 ** 256 - 1), ("sub", -(2 ** 512 - 1), 1),
              ("floordiv", -7, 2), ("div", -7, 2), ("gcd", 0, 0), ("gcd", 0, -12), ("cmp", -5, -5)]
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as f:
        for op, a, b in cases: f.write(f"{op} {a} {b}\n")
        path = f.name
    out = subprocess.run([exe, "--lsw", path], capture_output=True, text=True, timeout=TIMEOUT)
    os.unlink(path)
    got = out.stdout.split("\n")
    bad = 0
    for i, (op, a, b) in enumerate(cases):
        want = lsw_expected(op, a, b)
        line = got[i] if i < len(got) else "(none)"
        ok = line == "overflow" if want is None else line == f"= {want}"
        if not ok:
            bad += 1
            if bad <= 5: print(f"  WRONG: {op} {a} {b}: {line}, expected {'overflow' if want is None else want}")
    print(f"  {len(cases)} operations, {sum(lsw_expected(o, a, b) is None for o, a, b in cases)} past the bound; wrong {bad}")
    return 1 if bad or out.returncode else 0

def run_capacity(exe):
    d = os.path.join(os.path.dirname(os.path.abspath(__file__)), "solver_capacity")
    vcs, kinds = [], []
    for f, ks in CAPACITY_CASES:
        got = read_vcs(open(os.path.join(d, f)).read())
        if len(got) != len(ks): print(f"  WRONG: {f} holds {len(got)} VCs, {len(ks)} expected"); return 1
        vcs += got; kinds += ks
    rc = run(exe, vcs, False, 4, kinds)
    for f, ks in CONE_CAPACITY_CASES:
        print(f"  {f}, through lia_cone:")
        rc |= run(exe, read_vcs(open(os.path.join(d, f)).read()), False, 4, ks, cone=True)
    # the retries share the call's one budget: rec1 0x17 needs 13 pivots to fail whole, 5 for S_1
    # and 10 for S_2, so 20 leave S_2 too few (CAPACITY, at most 20 used), while a budget the
    # whole search exhausts is BUDGET, and is not retried
    for lim, kind in ((["20", "512", "512"], "capacity"), (["5", "512", "512"], "budget")):
        print(f"  rec1_0x17 within {lim[0]} pivots:")
        r = run(exe, vcs[:1], False, 4, [kind], lim)
        if r == 0 and run.work["pivot"] > int(lim[0]):
            print(f"  WRONG: {run.work['pivot']} pivots used of {lim[0]}"); r = 1
        rc |= r
    # the cap is what stops wide_long: the wide search alone, uncapped, finds a model past it
    wl = read_vcs(open(os.path.join(d, "wide_long.vc")).read())[0]
    cap = max(32, (1 << 24) // ((len(wl["hyps"]) + 1) * (len(wl["names"]) + len(wl["hyps"]) + 1)))
    r, blk = answers(exe, [wl], "--wide")
    m = re.search(r"work pivot:(\d+)", blk[0]) if len(blk) == 1 else None
    vals = [int(l.split(" = ")[1]) for l in blk[0].split("\n")[1:] if " = " in l] if m else []
    if r or not m or not blk[0].startswith("model ") or int(m.group(1)) <= cap or not sat_at(wl, vals):
        print(f"  WRONG: wide_long's uncapped wide search does not find a model past {cap} pivots:\n" + "\n".join(blk)[:400]); rc = 1
    # the ladder's order: rec1_0x17 costs 13 pivots whole, 5 for S_1 and 10 for S_2, so its answer
    # comes from the slices (28 pivots), before the wide search could run
    r, blk = answers(exe, vcs[:1])
    if r or len(blk) != 1 or not blk[0].startswith("cert ") or "work pivot:28 " not in blk[0]:
        print("  WRONG: rec1_0x17 is not certified by its slices in 28 pivots:\n" + "\n".join(blk)); rc = 1
    # deterministic: the same VCs twice, byte for byte (vcs[0] is rec1_0x17)
    with tempfile.NamedTemporaryFile("w", suffix=".vc", delete=False) as f:
        for vc in vcs: f.write(vc_text(vc))
        path = f.name
    outs = [subprocess.run([exe, path] + LIMITS, capture_output=True, text=True, timeout=TIMEOUT).stdout for _ in range(2)]
    os.unlink(path)
    if outs[0] != outs[1]: print("  WRONG: two runs answered differently"); rc = 1
    return rc

# ── reading the texts (for --check over a generator's VCs) ─────────────────────────────────────
def parse_cons(tokens, names):
    op_i = next(i for i, w in enumerate(tokens) if w in ("<=", "=", "!="))
    return (parse_sum(tokens[:op_i], names), tokens[op_i], int(tokens[op_i + 1]))

def read_vcs(text):
    vcs, cur = [], None
    for line in text.split("\n"):
        w = line.split()
        if not w: continue
        if w[0] == "vc": cur = {"id": " ".join(w[1:]), "names": [], "hyps": [], "goal": None}
        elif w[0] == "var": cur["names"].append(w[1])
        elif w[0] == "hyp":
            colon = w.index(":")
            origin, body = " ".join(w[2:colon]), w[colon + 1:]
            if body[0] == "or":
                inner = body[2:-1]                     # between `{` and `}`
                dis, conj, cons = [], [], []
                for t in inner + ["|"]:
                    if t in (",", "|"):
                        conj.append(parse_cons(cons, cur["names"])); cons = []
                        if t == "|": dis.append(conj); conj = []
                    else: cons.append(t)
                cur["hyps"].append((origin, dis))
            else: cur["hyps"].append((origin, parse_cons(body, cur["names"])))
        elif w[0] == "goal": cur["goal"] = parse_cons(w[1:], cur["names"])
        elif w[0] == "end": vcs.append(cur); cur = None
    return vcs

def check_files(vcfile, ansfile, models_out=None):
    vcs = read_vcs(open(vcfile).read())
    lines = open(ansfile).read().split("\n")
    i = 0; per = {}; bad = 0; notimp = []
    for vc in vcs:
        head = lines[i]; i += 1; body = []
        if head.startswith("cert ") or head.startswith("model "):
            while lines[i] != "end": body.append(lines[i]); i += 1
            i += 1
        i += 1                                           # the `work` line
        kind = vc["id"].split()[3]
        try:
            if head != f"{head.split()[0]} {vc['id']}" + (" v1" if head.split()[0] in ("cert", "model") else ""):
                raise Bad(f"answer {head!r} is not for {vc['id']!r}")
            if head.startswith("cert "): check_cert(vc, body); out = "proved"
            elif head.startswith("model "):
                x = [int(l.split(" = ")[1]) for l in body]
                if not sat_at(vc, x): raise Bad(f"model {x} does not satisfy")
                out = "not-implied"; notimp.append((vc["id"], dict(zip(vc["names"], x))))
            else: out = head.split()[0]
        except Bad as e:
            bad += 1; out = "WRONG"
            if bad <= 5: print("WRONG:", vc["id"], e)
        per.setdefault(kind, {}).setdefault(out, 0); per[kind][out] += 1
    tot = {}
    for kind in sorted(per):
        print(f"  {kind:14} " + ", ".join(f"{k} {v}" for k, v in sorted(per[kind].items())))
        for k, v in per[kind].items(): tot[k] = tot.get(k, 0) + v
    print(f"  {'ALL':14} " + ", ".join(f"{k} {v}" for k, v in sorted(tot.items())) + f"  ({len(vcs)} VCs)")
    if models_out:
        with open(models_out, "w") as f:
            for vid, m in notimp: f.write(vid + "\t" + " ; ".join(f"{k}={v}" for k, v in m.items()) + "\n")
    return 1 if bad else 0

def hyps_hold(vc, x):
    for origin, h in vc["hyps"]:
        if isinstance(h, list):
            if not any(all(holds(c, x) for c in d) for d in h): return False
        elif not holds(h, x): return False
    return True

# Maximization (PE.2): each VC's goal SUM is the objective. Brute force over the box gives the true
# maximum; the answer's bound must equal it, its certificate must refute `hyps and obj > b`, and
# its model must satisfy the hypotheses with obj = b. An empty box must be refuted; in a box a
# missing bound or a refusal is wrong.
def run_max(exe, vcs, B):
    with tempfile.NamedTemporaryFile("w", suffix=".vc", delete=False) as f:
        for vc in vcs: f.write(vc_text(vc))
        path = f.name
    try:
        out = subprocess.run([exe, "--max", path] + LIMITS, capture_output=True, text=True, timeout=TIMEOUT)
    except subprocess.TimeoutExpired:
        os.unlink(path); print(f"  WRONG: --max did not finish {len(vcs)} VCs within {TIMEOUT} s"); return 1
    os.unlink(path)
    if out.returncode != 0: print("solver exit", out.returncode, out.stderr); return 1
    lines = out.stdout.split("\n"); i = 0; counts = {}; bad = 0
    def block(i, tag):
        body = []
        if i < len(lines) and lines[i].startswith(tag + " "):
            i += 1
            while lines[i] != "end": body.append(lines[i]); i += 1
            i += 1
        return i, body
    for vc in vcs:
        head = lines[i].split(); i += 1
        i, cert = block(i, "cert"); i, model = block(i, "model"); i += 1   # and the work line
        obj = vc["goal"][0]
        pts = [x for x in itertools.product(range(-B, B + 1), repeat=len(vc["names"])) if hyps_hold(vc, x)]
        try:
            if head[0] == "max-empty":
                if pts: raise Bad(f"called empty, but {pts[0]} satisfies the hypotheses")
                check_cert(dict(vc, goal=({}, "<=", -1)), cert)
            elif head[0] == "max":
                b = int(head[-1])
                true = max(sum(a * x[v] for v, a in obj.items()) for x in pts) if pts else None
                if true != b: raise Bad(f"bound {b}, the maximum is {true}")
                check_cert(dict(vc, goal=(obj, "<=", b)), cert)
                x = [int(l.split(" = ")[1]) for l in model]
                if not hyps_hold(vc, x) or sum(a * x[v] for v, a in obj.items()) != b:
                    raise Bad(f"model {x} does not attain {b}")
            else:
                raise Bad(f"{head[0]} in a box")
            counts[head[0]] = counts.get(head[0], 0) + 1
        except Bad as e:
            bad += 1
            if bad <= 5: print("WRONG:", e); print(vc_text(vc) + " ".join(head) + "\n" + "\n".join(cert + model))
    print("  " + ", ".join(f"{k} {v}" for k, v in sorted(counts.items())) + f"; wrong {bad}")
    return 1 if bad else 0

def main():
    if sys.argv[1] == "--check":
        sys.exit(check_files(sys.argv[2], sys.argv[3], sys.argv[4] if len(sys.argv) > 4 else None))
    exe = sys.argv[1]
    if len(sys.argv) > 2:
        N, seed = int(sys.argv[2]), int(sys.argv[3])
        mode = sys.argv[4] if len(sys.argv) > 4 else "bounded"
        rng = random.Random(seed)
        print(f"seed {seed} N {N} mode {mode}:")
        sys.exit(run(exe, [gen_vc(rng, i, mode == "bounded", 4) for i in range(N)], mode == "bounded", 4))
    rc = 0
    vcs = [{"id": f"{n} 0 bb0.0 worked 0", "names": names, "hyps": hyps, "goal": goal} for n, _, names, hyps, goal in WORKED]
    print("worked examples:")
    rc |= run(exe, vcs, False, 4, [k for _, k, _, _, _ in WORKED])
    print("the sliced retry (solver_capacity/):")
    rc |= run_capacity(exe)
    print("lia_cone:")
    rc |= run_cone(exe)
    print("the wide search (s3): LSW's arithmetic against Python's integers:")
    rc |= run_lsw(exe)
    print("the wide search, byte for byte the 128-bit one's wherever that finishes:")
    rc |= run_wide_property(exe)
    print("coefficients near 2^60, the wide retry answering, 300 boxed VCs:")
    rc |= run_wide_big(exe)
    for seed, mode in ((1, "bounded"), (2, "bounded"), (3, "free")):
        rng = random.Random(seed)
        print(f"seed {seed}, 1000 VCs, {mode}:")
        rc |= run(exe, [gen_vc(rng, i, mode == "bounded", 4) for i in range(1000)], mode == "bounded", 4)
    for seed, mode in ((5, "bounded"), (6, "free")):
        rng = random.Random(seed)
        print(f"seed {seed}, 400 parity-shaped VCs (the Diophantine step), {mode}:")
        rc |= run(exe, [gen_parity_vc(rng, i, mode == "bounded", 2) for i in range(400)], mode == "bounded", 2)
    rng = random.Random(4)
    print("seed 4, 500 VCs, bounded, maximized:")
    rc |= run_max(exe, [gen_vc(rng, i, True, 4) for i in range(500)], 4)
    print("solver: " + ("ALL PASSED" if rc == 0 else "FAILURES"))
    sys.exit(rc)

main()
