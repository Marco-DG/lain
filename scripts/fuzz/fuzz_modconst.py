#!/usr/bin/env python3
# fuzz_modconst.py — generator for the MODULE-CONSTANT fuzzer (fuzz_modconst.sh).
#
# Module scope was the blind spot of every generator here: they all write their programs inside
# functions. So nothing noticed, until 2026-10-01, that a module constant was never checked
# against its declared type (`X u8 = 300` was 44; `T u8[256] = [5, 6, 7]` left 253 elements
# uninitialised), that its fold into a use ignored the declared type (`X u8 = 1000 as| u16` read
# as 232), or that a comprehension there was not modelled at all. This generator writes nothing
# BUT module constants, and a `main` that reads every one of them.
#
# FORMS, each with one meaning, computed exactly here:
#   literal         `C2 u16 = 4000`
#   reference       `C3 u8 = C1`            a constant of another type, by name
#   shift           `C4 u32 = 1 << 17`      (1 is an i32, so s <= 30)
#   explicit table  `T0 u8[5] = [1, 2, 3, 4, 5]`
#   comprehension   `T1 u16[9] = [(i * 7 + 3) % 50 for i in 0..9]`
#
# ORACLE. A program is either VALID — every value fits its type, every table has its length — or
# carries exactly ONE invalid constant. The first line says which:
#   // EXPECT accept   it must compile, and main returns 0 iff every constant reads back as
#                      computed here (a wrong value is a MISCOMPILE)
#   // EXPECT reject   it must be refused (accepting it is a MISSED-REJECTION: a constant whose
#                      value its type cannot hold, or a table with uninitialised elements)
import random, sys

seed = int(sys.argv[1]) if len(sys.argv) > 1 else 0
rng = random.Random(seed)
def r(lo, hi): return rng.randint(lo, hi)

TYPES = {"u8": (0, 255), "u16": (0, 65535), "u32": (0, 4294967295),
         "i8": (-128, 127), "i16": (-32768, 32767), "i32": (-2147483648, 2147483647)}
def fits(t, v): lo, hi = TYPES[t]; return lo <= v <= hi

def value_near(t, bad):
    lo, hi = TYPES[t]
    if bad:  # just outside, or well outside
        return rng.choice([hi + 1, hi + r(2, 1000), lo - 1, lo - r(2, 1000)])
    return rng.choice([lo, hi, r(lo, hi), r(max(lo, -50), min(hi, 50))])

def lit(v): return str(v) if v >= 0 else "0 - %d" % (-v)   # a negative constant, as Lain spells it

valid = rng.random() < 0.6
bad_at = -1 if valid else None
consts, lines = [], []           # consts: (name, type, value, kind)
n = r(3, 6)
if not valid: bad_at = r(0, n - 1)

for k in range(n):
    bad = (k == bad_at)
    t = rng.choice(list(TYPES))
    form = rng.choice(["literal", "reference", "shift", "table", "comprehension"])
    scalars = [c for c in consts if c[3] == "scalar"]
    if form == "reference" and not scalars: form = "literal"
    name = "C%d" % k
    if form == "literal":
        v = value_near(t, bad)
        lines.append("%s %s = %s" % (name, t, lit(v))); consts.append((name, t, v, "scalar"))
    elif form == "reference":
        # a reference to an earlier constant: valid iff its value fits THIS type
        cands = [c for c in scalars if fits(t, c[2]) != bad]
        if not cands:
            v = value_near(t, bad); lines.append("%s %s = %s" % (name, t, lit(v)))
            consts.append((name, t, v, "scalar")); continue
        src = rng.choice(cands)
        lines.append("%s %s = %s" % (name, t, src[0])); consts.append((name, t, src[2], "scalar"))
    elif form == "shift":
        lo, hi = TYPES[t]
        ok_s = [s for s in range(0, 31) if (1 << s) <= hi]
        bad_s = [s for s in range(0, 31) if (1 << s) > hi]
        if bad and not bad_s:
            v = value_near(t, True); lines.append("%s %s = %s" % (name, t, lit(v)))
            consts.append((name, t, v, "scalar")); continue
        s = rng.choice(bad_s if bad else ok_s)
        lines.append("%s %s = 1 << %d" % (name, t, s)); consts.append((name, t, 1 << s, "scalar"))
    elif form == "table":
        L = r(1, 9)
        how = rng.choice(["length", "value"]) if bad else None
        vals = [value_near(t, False) for _ in range(L)]
        decl_len = L
        if how == "length": decl_len = L + rng.choice([1, 2, 5])
        if how == "value":  vals[r(0, L - 1)] = value_near(t, True)
        lines.append("%s %s[%d] = [%s]" % (name, t, decl_len, ", ".join(lit(v) for v in vals)))
        consts.append((name, t, vals, "table"))
    else:  # comprehension: (i * a + b) % m, every value in [0, m - 1]
        lo, hi = TYPES[t]
        a, b = r(0, 9), r(0, 20)
        m = r(2, min(hi, 300)) if hi >= 2 else 2
        L = r(1, 12)
        how = rng.choice(["length", "value"]) if bad else None
        decl_len = L
        if how == "length": decl_len = L + rng.choice([1, 3])
        if how == "value":
            if hi >= 300:  # cannot exceed this type with a small modulus: make it a length error
                decl_len = L + 1
            else:
                m = hi + r(2, 50)
                if max((i * a + b) % m for i in range(L)) <= hi:
                    decl_len = L + 1          # no element happened to exceed: still invalid
        vals = [(i * a + b) % m for i in range(L)]
        lines.append("%s %s[%d] = [(i * %d + %d) %% %d for i in 0..%d]" % (name, t, decl_len, a, b, m, L))
        consts.append((name, t, vals, "table"))

print("// EXPECT %s  seed=%d" % ("accept" if valid else "reject", seed))
for l in lines: print(l)
print("func main() i32 {")
check = 0
for (name, t, v, kind) in consts:
    if kind == "scalar":
        check += 1
        print("    if %s as i64 != %s { return %d }" % (name, lit(v) if v >= 0 else "(%s) as i64" % lit(v), check))
    else:
        for i in sorted(set([0, len(v) - 1, r(0, len(v) - 1)])):
            check += 1
            print("    if %s[%d] as i64 != %s { return %d }" % (name, i, lit(v[i]) if v[i] >= 0 else "(%s) as i64" % lit(v[i]), check))
print("    return 0")
print("}")
