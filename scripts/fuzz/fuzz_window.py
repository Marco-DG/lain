#!/usr/bin/env python3
"""Windows and searches whose index is a SUM bounded through a DIFFERENCE, judged for every input.

I.144 bounds `x + y` through an exact subtraction d = B - a' that dominates it:
x + y = B + (x - a') + (y - d). This generates the two shapes that rule exists for, each with its
guards, loop bounds and index perturbed by small constants, so that most variants are off by one
somewhere:

  WINDOW   `func f(h u8[], pos usize, n usize) u32`: `if pos > h.len + K1`, `if n > h.len - pos + K2`,
           then `while i < n + K3` (or `<=`) reading `h[pos + i + K4]` (or `h[i + pos + K4]`).
  SEARCH   `func find(.., hay u8[n], .., needle u8[m]) usize` (lengths bounded, I.147):
           `if needle.len > hay.len + K0`, `while i <= hay.len - needle.len + K1` (or `<`), and
           `while j < needle.len + K2 and hay[i + j + K3] == needle[j]`.

A Python model runs the program for every input in a small domain (every length and offset up to
6, every byte string over {0, 1} for the search) and records the first input that indexes out of
bounds or underflows a subtraction. The compiler accepting a program that does either from some
input is UNSOUND. Refusing one that does neither on the domain is counted as the cost; the domain
is small, so a refusal there may still be right (an overflow needs huge values).

    fuzz_window.py LAIN N SEED
    fuzz_window.py SEED         (one program, whose main calls it across the domain: fuzz_interp)
"""
import itertools, os, random, subprocess, sys, tempfile

DOM = 6                                   # lengths, offsets and counts range over 0..DOM

def gen(rng):
    if rng.random() < 0.5:
        return ("window", dict(
            g1=rng.choice([None, 0, 0, 0, 1]),            # `if pos > h.len + K1 { return 0 }`
            g2=rng.choice([None, 0, 0, 0, 0, 1, 2]),      # `if n > h.len - pos + K2 { return 0 }`
            le=rng.random() < 0.2,                        # `while i <= n + K3`
            k3=rng.choice([0, 0, 0, 0, 1]),
            k4=rng.choice([0, 0, 0, 0, 1]),
            swap=rng.random() < 0.5))                     # `h[i + pos]`
    return ("search", dict(
        g0=rng.choice([None, 0, 0, 0, 1]),                # `if needle.len > hay.len + K0 { return }`
        k1=rng.choice([0, 0, 0, 0, 1]),                   # `while i <= hay.len - needle.len + K1`
        lt=rng.random() < 0.2,                            # ... `<` instead of `<=`
        k2=rng.choice([0, 0, 0, 0, 1]),                   # `while j < needle.len + K2 and ...`
        le2=rng.random() < 0.15,                          # ... `<=`
        k3=rng.choice([0, 0, 0, 0, 1]),                   # `hay[i + j + K3]`
        swap=rng.random() < 0.5))                         # `hay[j + i + K3]`

def plus(k): return " + %d" % k if k else ""

def src(kind, p, with_main=True):
    L = []
    if kind == "window":
        L.append("func f(h u8[], pos usize, n usize) u32 {")
        if p["g1"] is not None: L.append("    if pos > h.len%s { return 0 }" % plus(p["g1"]))
        if p["g2"] is not None: L.append("    if n > h.len - pos%s { return 0 }" % plus(p["g2"]))
        L += ["    var s u32 = 0", "    var i usize = 0",
              "    while i %s n%s {" % ("<=" if p["le"] else "<", plus(p["k3"])),
              "        s = s +%% (h[%s%s] as u32)" % ("i + pos" if p["swap"] else "pos + i", plus(p["k4"])),
              "        i = i + 1", "    }", "    return s", "}"]
        if with_main:
            L += ["", "func main() i32 {", "    var buf u8[%d] = [1 for k in 0..%d]" % (DOM, DOM),
                  "    var t u32 = 0", "    var len usize = 0",
                  "    while len <= %d {" % DOM, "        var pos usize = 0",
                  "        while pos <= %d {" % DOM, "            var n usize = 0",
                  "            while n <= %d {" % DOM,
                  "                t = t +% f(buf[0..len], pos, n)", "                n = n + 1",
                  "            }", "            pos = pos + 1", "        }", "        len = len + 1", "    }",
                  "    return (t % 7) as i32", "}"]
    else:
        B = "usize < 1048576"
        L.append("func find(n %s, hay u8[n], m %s, needle u8[m]) usize {" % (B, B))
        if p["g0"] is not None: L.append("    if needle.len > hay.len%s { return hay.len }" % plus(p["g0"]))
        idx = ("j + i" if p["swap"] else "i + j") + plus(p["k3"])
        L += ["    var i usize = 0",
              "    while i %s hay.len - needle.len%s {" % ("<" if p["lt"] else "<=", plus(p["k1"])),
              "        var j usize = 0",
              "        while j %s needle.len%s and hay[%s] == needle[j] {" % ("<=" if p["le2"] else "<", plus(p["k2"]), idx),
              "            j = j + 1", "        }",
              "        if j == needle.len { return i }", "        i = i + 1", "    }",
              "    return hay.len", "}"]
        if with_main:
            # every (hay, needle) over {0,1} with lengths <= 4 and 3: hay walks 0..2^H by bits
            L += ["", "func main() i32 {", "    var t usize = 0", "    var hl usize = 0",
                  "    while hl <= 4 {", "        var ml usize = 0", "        while ml <= 3 {",
                  "            var hb usize = 0", "            while hb < 16 {",
                  "                var mb usize = 0", "                while mb < 8 {",
                  "                    var hay u8[4] = [(hb & 1) as u8, ((hb >> 1) & 1) as u8, ((hb >> 2) & 1) as u8, ((hb >> 3) & 1) as u8]",
                  "                    var nd u8[3] = [(mb & 1) as u8, ((mb >> 1) & 1) as u8, ((mb >> 2) & 1) as u8]",
                  "                    t = t +% find(hl, hay[0..hl], ml, nd[0..ml])",
                  "                    mb = mb + 1", "                }", "                hb = hb + 1",
                  "            }", "            ml = ml + 1", "        }", "        hl = hl + 1", "    }",
                  "    return (t % 7) as i32", "}"]
    return "\n".join(L) + "\n"

def bad_window(p):
    for hl in range(DOM + 1):
        for pos in range(DOM + 1):
            for n in range(DOM + 1):
                if p["g1"] is not None and pos > hl + p["g1"]: continue
                if p["g2"] is not None:
                    if pos > hl: return (hl, pos, n, "h.len - pos underflows")
                    if n > hl - pos + p["g2"]: continue
                i, top = 0, n + p["k3"]
                while (i <= top) if p["le"] else (i < top):
                    k = pos + i + p["k4"]
                    if k >= hl: return (hl, pos, n, "h[%d] of %d" % (k, hl))
                    i += 1
    return None

def bad_search(p):
    for hl in range(5):
        for ml in range(4):
            for hay in itertools.product((0, 1), repeat=hl):
                for nd in itertools.product((0, 1), repeat=ml):
                    if p["g0"] is not None and ml > hl + p["g0"]: continue
                    if ml > hl: return (hay, nd, "hay.len - needle.len underflows")
                    i, found = 0, False
                    while (i < hl - ml + p["k1"]) if p["lt"] else (i <= hl - ml + p["k1"]):
                        j = 0
                        while (j <= ml + p["k2"]) if p["le2"] else (j < ml + p["k2"]):
                            k = i + j + p["k3"]
                            if k >= hl: return (hay, nd, "hay[%d] of %d" % (k, hl))
                            if j >= ml: return (hay, nd, "needle[%d] of %d" % (j, ml))
                            if hay[k] != nd[j]: break
                            j += 1
                        if j == ml: found = True; break
                        i += 1
    return None

def main():
    if len(sys.argv) == 2:
        kind, p = gen(random.Random(int(sys.argv[1])))
        sys.stdout.write(src(kind, p)); return
    lain, N, seed = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
    tmp = tempfile.mkdtemp(prefix="fuzz_window.")
    acc = refused = other = unsound = good = lost = 0
    kinds = {"window": 0, "search": 0}
    for k in range(N):
        kind, p = gen(random.Random(seed * 1000003 + k)); kinds[kind] += 1
        text = src(kind, p, with_main=False) + "\nfunc main() i32 { return 0 }\n"
        path = os.path.join(tmp, "t.ln"); open(path, "w").write(text)
        r = subprocess.run([lain, path, "-o", os.path.join(tmp, "t.c")], capture_output=True, text=True)
        bad = bad_window(p) if kind == "window" else bad_search(p)
        if bad is None: good += 1
        if r.returncode == 0:
            acc += 1
            if bad is not None:
                unsound += 1
                print("  ★ UNSOUND: accepted, and %s (seed %d, k %d)" % (bad, seed, k))
                print("".join("      " + l + "\n" for l in text.splitlines()), end="")
        elif r.returncode == 1 and "[E" in r.stderr:
            refused += 1
            if bad is None: lost += 1
        else:
            other += 1
            print("  ★ HARNESS: exit %d without a coded refusal (seed %d, k %d): %s" % (r.returncode, seed, k, r.stderr.strip()[:200]))
    for f in os.listdir(tmp): os.remove(os.path.join(tmp, f))
    os.rmdir(tmp)
    print("fuzz_window: seed=%d gens=%d (window %d, search %d) accepted=%d refused=%d other=%d"
          % (seed, N, kinds["window"], kinds["search"], acc, refused, other))
    print("  ground truth: %d safe on the domain; %d of those refused (the cost, or an overflow beyond it)" % (good, lost))
    print("  bugs:  UNSOUND(accepted, out of bounds or underflow on some input)=%d" % unsound)
    sys.exit(1 if (unsound or other) else 0)

if __name__ == "__main__":
    main()
