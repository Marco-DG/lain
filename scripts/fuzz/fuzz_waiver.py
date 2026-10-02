#!/usr/bin/env python3
"""fuzz_waiver.py — M15's generator: a WAIVED operation in front of safe code that needs its value.

`unsafe` waives narrowing and overflow; the C wraps the value; VRA may keep modelling the UNWRAPPED
value. Safe code after the block then reasons from a number the machine does not hold, and a bound
proved from that number is a false proof. No other generator puts a waived operation in front of safe
code depending on its value, which is why 29 fuzzers read 0 over this class.

Each program has three parts, and the generator prints where they are:

    the SOURCE       an out-of-range value: a constant, or a parameter pinned by a GUARD
    the WAIVED op    inside `unsafe { ... }`, one of the categories below
    the DEPENDENT    safe code AFTER the block whose proof needs that value: `a[x]` on i32[4]

The header names the unsafe block's line range, because the oracle rule depends on it: on the pre-fix
compiler a `PROOF FAILED` at a line INSIDE the block is the interpreter mislabelling a waived
operation, NOT a finding. Only a `PROOF FAILED` OUTSIDE the block is M15's signal. The driver must
therefore compare line numbers, and must count the inside-the-block ones in their own column rather
than filtering them silently — a filtered case that is counted is data, a filtered case that is
dropped is a lie.

Two SOURCE kinds, and the second is not optional. Exactly-known operands are sufficient but not
necessary: a guard on a parameter carries the fact through the octagon just as well (`if y >= 260 and
y <= 263` keeps x == y), and a generator that only ever emits constants would miss half the class.
"""
import random
import sys

A4 = '    a i32[4] = [1, 2, 3, 4]\n'          # the object every dependent proof is about


def _src(kind, ty, lo):
    """The out-of-range source value. Returns (decl_lines, expr, guard_open, guard_close, arg)."""
    if kind == "const":
        return ('    var y %s = %d\n' % (ty, lo), 'y', '', '', None)
    # a PARAMETER pinned by a guard: the octagon carries the relation, no constant needed
    return ('', 'y', '    if y >= %d and y <= %d {\n' % (lo, lo + 3), '    }\n', lo + 1)


# Each category returns the body of `go`, given the pieces. `U0`/`U1` mark the unsafe block.
def cat_cast(d, e, go, gc):
    return (d + go + '    var x u8 = 0\n    unsafe {\nU0        x = %s as u8\nU1    }\n'
            '    if x >= 200 {\n        return 0\n    }\n    return a[x]\n' % e + gc)


def cat_overflow(d, e, go, gc):
    return ('    var p u8 = 200\n    var q u8 = 60\n'
            '    var x u8 = 0\n    unsafe {\nU0        x = p + q\nU1    }\n'
            '    if x >= 200 {\n        return 0\n    }\n    return a[x]\n')


def cat_field(d, e, go, gc):
    return (d + go + '    var s = S(0)\n    unsafe {\nU0        s.v = %s as u8\nU1    }\n'
            '    if s.v >= 200 {\n        return 0\n    }\n    return a[s.v]\n' % e + gc)


def cat_ctor(d, e, go, gc):
    return (d + go + '    var s = S(0)\n    unsafe {\nU0        s = S(%s as u8)\nU1    }\n'
            '    if s.v >= 200 {\n        return 0\n    }\n    return a[s.v]\n' % e + gc)


def cat_store(d, e, go, gc):
    return (d + go + '    var t u8[2] = [0, 0]\n    unsafe {\nU0        t[0] = %s as u8\nU1    }\n'
            '    if t[0] >= 200 {\n        return 0\n    }\n    return a[t[0]]\n' % e + gc)


def cat_argument(d, e, go, gc):
    return (d + go + '    var x u8 = 0\n    unsafe {\nU0        x = take(%s as u8)\nU1    }\n'
            '    if x >= 200 {\n        return 0\n    }\n    return a[x]\n' % e + gc)


def cat_wide(d, e, go, gc):
    """A narrowing from a WIDE type, not wide arithmetic.

    Measured: `unsafe { z = y + 5 }` on a u64 at its maximum is E086 on BOTH compilers, so a 64-bit
    OVERFLOW is not in the waived set at all and that shape tests nothing. A u64 -> u8 NARROWING is
    waived, and is a false proof on the pre-fix compiler.
    """
    return ('    var y u64 = 260\n    var x u8 = 0\n    unsafe {\n'
            'U0        x = y as u8\nU1    }\n'
            '    if x >= 200 {\n        return 0\n    }\n    return a[x]\n')


# (body, extra declarations, the SOURCE kinds this category accepts). `overflow` and `wide` supply
# their own operands, so a guarded parameter is meaningless for them — and worse than meaningless: the
# guard declares `y` as a parameter while the body declares `var y`, which is E013 "defined twice" and
# would have been counted as "does not reproduce" rather than as the generator contradicting itself.

def cat_refined(d, e, go, gc):
    """A REFINED target: `v i32 >= 0 and <= 3`. A wrap cannot honour a predicate.

    Found by MCW after my first A/B. It is a false proof on the pre-fix compiler AND on A alone,
    because A uses the REPRESENTATION's range and a refinement is narrower than its representation;
    only B's refusal at the store closes it. So this category is the one that distinguishes the two
    halves of the fix, as the invariant category does.
    """
    return (d + go + '    var dd = D(0)\n    unsafe {\nU0        dd.v = %s\nU1    }\n'
            '    return t[dd.v]\n' % e + gc)


def cat_wide_computed(d, e, go, gc):
    """A COMPUTED u64 overflow, which IS waived.

    My first attempt wrote u64's maximum as a literal and got E086 "too large to fit" — the literal
    limit (DECIDE-T), not the waiver. I reported that as "64-bit overflow is not waived", which was
    wrong. Computed with `y = y -% 1` from 0 it compiles, and the overflow is waived.

    SAFE FOR A STATED REASON, and kept as a TRIPWIRE rather than dropped. The analysis reads a u64
    above INT64_MAX as unbounded (vra_cast_policy's `above`), so the false value is never precise
    enough to make a dependent proof false, and three dependent shapes (a direct bound, an index in a
    branch the analysis believes dead, a narrowing to u8 first) are all honestly refused. That is a
    consequence of the u64 domain question (DECIDE-T): if u64 ever gets a true top, this category
    becomes live, and M15 should already be testing it on the day that happens rather than being
    extended afterwards.
    """
    return ('    var y u64 = 0\n    y = y -% 1\n    var z u64 = 0\n    unsafe {\n'
            'U0        z = y + 5\nU1    }\n'
            '    if z >= 200 {\n        return 0\n    }\n    return a[z as usize]\n')


CATS = {
    "cast": (cat_cast, "", ("const", "guard")),
    "overflow": (cat_overflow, "", ("const",)),
    "field": (cat_field, "type S {\n    v u8\n}\n", ("const", "guard")),
    "ctor": (cat_ctor, "type S {\n    v u8\n}\n", ("const", "guard")),
    "store": (cat_store, "", ("const", "guard")),
    "argument": (cat_argument, "func take(k u8) u8 {\n    return k\n}\n", ("const", "guard")),
    "wide": (cat_wide, "", ("const",)),
    "wide-computed": (cat_wide_computed, "", ("const",)),
    "refined": (cat_refined, "type D {\n    v i32 >= 0 and <= 3\n}\n", ("const", "guard")),
}

# The RETURN category is a whole function, not a body: the waived value must be returned DIRECTLY from
# inside the block. A value loaded from a cell and then returned is safe by accident (vra_range
# intersects it with its own type, the intersection is empty, and the return range becomes unusable),
# so that shape tests nothing.
RETURN_PROG = '''func add(p u8, q u8) u8 {
    unsafe {
U0        return p + q
U1    }
}
func go() i32 {
''' + A4 + '''    v = add(200, 60)
    if v >= 200 {
        return 0
    }
    return a[v]
}
'''

# The INVARIANT category: the false fact travels through an ASSUME, not a range, and the assume
# reaches gcc as __builtin_unreachable. The consequence is not a bad index but unbounded
# miscompilation: measured at 0f13bee this runs at -O0/-O1 and SIGSEGVs at -O2/-O3.
INVARIANT_PROG = '''type Buf {
    cap usize
    len usize <= cap
}
func room(b Buf) usize {
    return b.cap - b.len
}
func go() i32 {
    var b = Buf(2, 1)
    var y u16 = 260
    unsafe {
U0        b.len = (y as u8) as usize
U1    }
    if room(b) == 0 {
        return 0
    }
    return 1
}
'''


def gen(rng):
    name = rng.choice(sorted(CATS) + ["return", "invariant"])
    _garg = None
    if name == "return":
        body, decls, src = RETURN_PROG, "", "const"
    elif name == "invariant":
        body, decls, src = INVARIANT_PROG, "", "const"
    else:
        fn, decls, srcs = CATS[name]
        src = rng.choice(list(srcs))
        ty, lo = ("i32", 100) if name == "refined" else ("u16", 260)
        d, e, go, gc, _garg = _src(src, ty, lo)
        inner = fn(d, e, go, gc)
        sig = ('func go(y %s) i32 {\n' % ty) if src == "guard" else 'func go() i32 {\n'
        arr = '    t i32[4] = [1, 2, 3, 4]\n' if name == 'refined' else A4
        body = sig + arr + inner + '    return 0\n}\n' if src == "guard" else \
            sig + arr + inner + '}\n'
    # the argument must satisfy the GUARD, which differs per category: `refined` guards 100..103
    # and the others 260..263. It was hardcoded 261, so every refined/guard program failed its own
    # guard and the body never ran — the category reported accepted-and-clean, which reads as a
    # pass and is really a generator that tested nothing.
    arg = str(_garg) if src == "guard" else ''
    out = ['// CATEGORY: %s' % name, '// SOURCE: %s' % src, decls.rstrip('\n') if decls else None,
           body.rstrip('\n'), 'func main() i32 {', '    return go(%s)' % arg, '}']
    text = '\n'.join(x for x in out if x) + '\n'
    # resolve the U0/U1 markers into the header's line range
    lines = text.split('\n')
    u0 = u1 = 0
    for i, l in enumerate(lines):
        if l.startswith('U0'):
            u0 = i + 1
        if l.startswith('U1'):
            u1 = i + 1
    lines = [l[2:] if l.startswith(('U0', 'U1')) else l for l in lines]
    lines.insert(2, '// UNSAFE-LINES: %d-%d' % (u0 + 1, u1 + 1))
    return '\n'.join(lines)


if __name__ == '__main__':
    seed = int(sys.argv[1]) if len(sys.argv) > 1 else 0
    sys.stdout.write(gen(random.Random(seed)))
