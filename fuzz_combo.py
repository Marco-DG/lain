#!/usr/bin/env python3
# fuzz_combo.py — generator for the COMBINATION fuzzer (fuzz_combo.sh).
#
# Every other fuzzer in this repo is VERTICAL: it drills one feature (bounds, niche,
# offset, linearity, strings, division, shifts) as deeply as it can. None of them CROSS
# two features — and every bug found by hand-probing the compiler in session 59 lived at
# an intersection: `case` on a by-reference parameter, whole-value assignment through a
# `var` aggregate, a fixed array OF A USER TYPE passed by reference, a field read off a
# CALL RESULT. Each axis alone was covered by the corpus; no test crossed them.
#
# So this generator composes three axes instead of deepening one:
#
#   SHAPE    what the aggregate is        struct / ADT / nested struct / array-of-struct
#   CARRIER  how the value gets there     local / shared param / var param / mov param /
#                                         call result (an rvalue) / array element
#   OP       what is done with it         read a field / write a field / whole-assign /
#                                         match / pass it on
#
# ORACLE. Every program is VALID BY CONSTRUCTION and SELF-CHECKING: the generator computes
# the answer in Python and the program returns 0 iff it agrees. That gives three distinct
# verdicts without needing a second pipeline —
#   rejected      → a wrongly-rejected valid program (a precision/soundness-of-rejection bug)
#   broken C      → the emitted C does not compile (the class the corpus exists to catch)
#   exit != 0     → a MISCOMPILE: it compiled and computed the wrong answer
#
# Arithmetic is deliberately small and uses the wrapping operators (`+%`, `*%`), so no
# program is rejected for an overflow the fuzzer did not mean to test.
import random, sys

seed = int(sys.argv[1]) if len(sys.argv) > 1 else 0
rng = random.Random(seed)
def r(lo, hi): return rng.randint(lo, hi)

# ── the three axes ───────────────────────────────────────────────────────────
SHAPES   = ["struct", "adt", "adt_struct", "nested", "arrayof", "sliceof", "adt_nested"]
CARRIERS = ["local", "shared_param", "var_param", "mov_param", "call_result", "array_elem"]
OPS      = ["read", "write", "whole_assign", "match", "pass_on", "defer"]

shape   = rng.choice(SHAPES)
carrier = rng.choice(CARRIERS)
op      = rng.choice(OPS)

# prune combinations that are not expressible rather than emitting invalid Lain
ADTS = ("adt", "adt_struct", "adt_nested")
if op == "match" and shape not in ADTS: op = "read"
if shape in ADTS and op in ("write", "whole_assign"): op = "match"
if carrier == "call_result" and op in ("write", "whole_assign"): op = "read"
if carrier == "array_elem" and op == "whole_assign":   op = "write"
if shape == "arrayof" and carrier in ("call_result",): carrier = "array_elem"
if shape == "sliceof" and carrier not in ("shared_param", "var_param"): carrier = "shared_param"
if shape == "sliceof": op = "read"
# `mov` transfers ownership, so it composes with a read/pass, not with an in-place write
if carrier == "mov_param" and op in ("write", "whole_assign"): op = "read"
if shape == "arrayof" and op in ("match", "whole_assign", "defer", "pass_on"): op = "write"

a, b, c = r(1, 9), r(1, 9), r(1, 9)
L = []                       # program lines
exp = None                   # expected value, computed here

def emit(*ls): L.extend(ls)

# ── type declarations ────────────────────────────────────────────────────────
if shape in ("struct", "nested", "arrayof", "adt_struct", "sliceof"):
    emit("type P {", "    x i32", "    y i32", "}")
if shape == "nested":
    emit("type N {", "    inner P", "    n i32", "}")
if shape == "adt":
    emit("type A {", "    V1 { a i32 }", "    V2 { a i32, b i32 }", "    V3", "}")
if shape == "adt_nested":
    # An ADT whose variant payload is ANOTHER ADT — two levels of tag.
    emit("type Inner {", "    I1 { v i32 }", "    I2", "}")
    emit("type A {", "    Wrap { w Inner }", "    Pair { a i32, b i32 }", "    V3", "}")
if shape == "adt_struct":
    # A variant whose payload is a STRUCT. The bound name needs its field's TYPE to be
    # usable at all — `Pt(p): p.x` failed with E102 while `V1(x): x + 1` worked, because a
    # scalar binding needs no type and a struct one does.
    emit("type A {", "    Pt { p P }", "    Pair { a i32, b i32 }", "    V3", "}")

# ── the value under test, and its expected contribution ──────────────────────
def make_val():
    """Lain expression constructing the aggregate, plus its Python 'value'."""
    if shape == "adt":
        k = rng.choice([1, 2, 3])
        if k == 1: return f"A.V1({a})",        ("V1", a, 0)
        if k == 2: return f"A.V2({a}, {b})",   ("V2", a, b)
        return "A.V3",                          ("V3", 0, 0)
    if shape == "adt_nested":
        k = rng.choice([1, 2, 3])
        if k == 1: return f"A.Wrap(Inner.I1({a}))", ("W1", a, 0)
        if k == 2: return f"A.Pair({a}, {b})",      ("Pair", a, b)
        return "A.V3",                              ("V3", 0, 0)
    if shape == "adt_struct":
        k = rng.choice([1, 2, 3])
        if k == 1: return f"A.Pt(P({a}, {b}))", ("Pt", a, b)
        if k == 2: return f"A.Pair({a}, {b})",  ("Pair", a, b)
        return "A.V3",                           ("V3", 0, 0)
    if shape == "nested":
        return f"N(P({a}, {b}), {c})", (a, b, c)
    return f"P({a}, {b})", (a, b)

val_expr, val = make_val()

def read_value(v):
    """What `read` computes from the aggregate."""
    if shape == "adt":
        tag, x, y = v
        return x if tag == "V1" else (x * y if tag == "V2" else 5)
    if shape == "adt_struct":
        tag, x, y = v
        return (x + y) if tag == "Pt" else (x * y if tag == "Pair" else 5)
    if shape == "adt_nested":
        tag, x, y = v
        return (x * 2) if tag == "W1" else (x * y if tag == "Pair" else 5)
    if shape == "sliceof":
        return v[0] + v[1]
    if shape == "nested":
        return v[0] + v[1] + v[2]
    return v[0] + v[1]

# reader function body, shared by several carriers
def reader_fn(name, param):
    if shape == "adt_nested":
        return [f"func {name}({param} A) i32 {{",
                "    case s {",
                "        Wrap(w): return inner_of(w) *% 2",
                "        Pair(x, y): return x *% y",
                "        V3: return 5",
                "    }",
                "    return -1", "}"]
    if shape == "adt_struct":
        return [f"func {name}({param} A) i32 {{",
                "    case s {",
                "        Pt(p): return p.x +% p.y",     # the struct-payload binding
                "        Pair(x, y): return x *% y",
                "        V3: return 5",
                "    }",
                "    return -1", "}"]
    if shape == "adt":
        return [f"func {name}({param} A) i32 {{",
                "    case s {",
                "        V1(x): return x",
                "        V2(x, y): return x *% y",
                "        V3: return 5",
                "    }",
                "    return -1", "}"]
    if shape == "nested":
        return [f"func {name}({param} N) i32 {{",
                "    return s.inner.x +% s.inner.y +% s.n", "}"]
    return [f"func {name}({param} P) i32 {{", "    return s.x +% s.y", "}"]

TY = {"struct": "P", "adt": "A", "adt_struct": "A", "adt_nested": "A",
      "nested": "N", "arrayof": "P", "sliceof": "P"}[shape]
if shape == "adt_nested":     # the inner reader the outer arm calls
    emit("func inner_of(w Inner) i32 {", "    case w {",
         "        I1(v): return v", "        I2: return 0", "    }", "    return -1", "}")

# ── compose the program ──────────────────────────────────────────────────────
if shape == "arrayof":
    # a fixed array OF A USER TYPE — the shape whose Fixed_<T>_N typedef and `->data[i]`
    # indexing were both broken through a by-reference parameter.
    n = 3
    if carrier == "var_param":
        emit("proc setall(var xs P[3]) {",
             "    var i usize = 0",
             "    while i < 3 decreasing 3 - i {",
             f"        xs[i].x = (i as i32) +% {a}",
             "        i += 1", "    }", "}")
        emit("proc main() i32 {",
             "    var arr P[3] = [P(0, 0), P(0, 0), P(0, 0)]",
             "    setall(var arr)",
             "    var acc = 0",
             "    var j usize = 0",
             "    while j < 3 decreasing 3 - j {",
             "        acc = acc +% arr[j].x",
             "        j += 1", "    }")
        exp = sum(i + a for i in range(n))
    elif carrier == "shared_param":
        emit("func total(xs P[3]) i32 {",
             "    var s = 0",
             "    var i usize = 0",
             "    while i < 3 decreasing 3 - i {",
             "        s = s +% xs[i].x +% xs[i].y",
             "        i += 1", "    }",
             "    return s", "}")
        emit("proc main() i32 {",
             f"    var arr P[3] = [P({a}, {b}), P({b}, {c}), P({c}, {a})]",
             "    var acc = total(arr)")
        exp = (a + b) + (b + c) + (c + a)
    else:   # local / array_elem
        emit("proc main() i32 {",
             f"    var arr P[3] = [P({a}, {b}), P({b}, {c}), P({c}, {a})]",
             f"    arr[1].x = {a}",
             "    var acc = arr[0].x +% arr[1].x +% arr[2].y")
        exp = a + a + a
else:
    if shape == "sliceof":
        # a fixed array DECAYING to a slice parameter — the slice/aggregate crossing
        emit("func total(xs P[]) i32 {",
             "    var s = 0",
             "    var i usize = 0",
             "    while i < xs.len decreasing xs.len - i {",
             "        s = s +% xs[i].x +% xs[i].y",
             "        i += 1", "    }",
             "    return s", "}")
        if carrier == "var_param":
            emit("proc scale(var xs P[3]) {",
                 "    var i usize = 0",
                 "    while i < 3 decreasing 3 - i {",
                 f"        xs[i].y = xs[i].y +% {c}",
                 "        i += 1", "    }", "}")
            emit("proc main() i32 {",
                 f"    var arr P[3] = [P({a}, {b}), P({b}, {c}), P({c}, {a})]",
                 "    scale(var arr)",
                 "    var acc = total(arr)")
            exp = (a+b) + (b+c) + (c+a) + 3*c
        else:
            emit("proc main() i32 {",
                 f"    var arr P[3] = [P({a}, {b}), P({b}, {c}), P({c}, {a})]",
                 "    var acc = total(arr)")
            exp = (a+b) + (b+c) + (c+a)
    elif op == "defer":
        # `defer` CROSSED with an aggregate. Lain runs deferred statements BEFORE the return
        # expression is evaluated, so a defer that touches the value changes what comes back.
        emit(*reader_fn("rd", "s"))
        if shape in ADTS:
            emit(f"proc build(var s {TY}) {{", f"    defer s = {val_expr}", "    s = " +
                 ("A.V3" if shape != "adt_nested" else "A.V3"), "}")
            emit("proc main() i32 {",
                 f"    var v = {'A.V3'}", "    build(var v)", "    var acc = rd(v)")
            exp = read_value(val)
        else:
            fld = "p.inner.x" if shape == "nested" else "p.x"
            emit(f"proc build(var p {TY}) {{", f"    defer {fld} = {fld} +% {c}",
                 f"    {fld} = {fld} +% 1", "}")
            emit("proc main() i32 {", f"    var v = {val_expr}", "    build(var v)",
                 "    var acc = " + ("v.inner.x +% v.inner.y +% v.n" if shape=="nested" else "v.x +% v.y"))
            exp = read_value(val) + 1 + c
    elif carrier == "mov_param":
        # an OWNED aggregate parameter: it must be consumed, so it is handed back out
        emit(*reader_fn("rd", "s"))
        emit(f"func take(mov s {TY}) {TY} {{", "    return mov s", "}")
        emit("proc main() i32 {", f"    var v = {val_expr}",
             "    var v2 = take(mov v)", "    var acc = rd(v2)")
        exp = read_value(val)
    elif carrier == "shared_param":
        emit(*reader_fn("rd", "s"))
        emit("proc main() i32 {", f"    var v = {val_expr}", "    var acc = rd(v)")
        exp = read_value(val)
    elif carrier == "var_param" and shape in ("adt_struct", "adt_nested"):
        emit(*reader_fn("rd", "s"))
        emit("proc main() i32 {", f"    var v = {val_expr}", "    var acc = rd(v)")
        exp = read_value(val)
    elif carrier == "var_param" and shape == "adt":
        # An ADT behind a MUTABLE borrow: the arm reads the payload out of a pointer, and
        # (for whole_assign) writes a whole new variant back through it.
        emit("proc bump(var s A) {",
             "    case s {",
             f"        V1(x): s = A.V1(x +% {c})",
             f"        V2(x, y): s = A.V2(x +% {c}, y)",
             "        V3: s = A.V3",
             "    }", "}")
        emit(*reader_fn("rd", "s"))
        emit("proc main() i32 {", f"    var v = {val_expr}", "    bump(var v)",
             "    var acc = rd(v)")
        tag, x, y = val
        exp = (x + c) if tag == "V1" else ((x + c) * y if tag == "V2" else 5)
    elif carrier == "var_param":
        if op == "whole_assign":
            emit(f"proc reset(var s {TY}) {{", f"    s = {val_expr}", "}")
            emit("proc main() i32 {",
                 f"    var v = {'N(P(0, 0), 0)' if shape=='nested' else 'P(0, 0)'}",
                 "    reset(var v)",
                 "    var acc = " + ("v.inner.x +% v.inner.y +% v.n" if shape=="nested" else "v.x +% v.y"))
            exp = read_value(val)
        else:                       # write a field through the reference
            fld = "s.inner.x" if shape == "nested" else "s.x"
            emit(f"proc bump(var s {TY}) {{", f"    {fld} = {fld} +% {c}", "}")
            emit("proc main() i32 {", f"    var v = {val_expr}", "    bump(var v)",
                 "    var acc = " + ("v.inner.x +% v.inner.y +% v.n" if shape=="nested" else "v.x +% v.y"))
            exp = read_value(val) + c
    elif carrier == "call_result":
        emit(f"func mk() {TY} {{", f"    return {val_expr}", "}")
        fld = "mk().inner.x +% mk().n" if shape == "nested" else \
              ("mk().x +% mk().y" if shape not in ADTS else None)
        if shape in ADTS:
            emit(*reader_fn("rd", "s"))
            emit("proc main() i32 {", "    var acc = rd(mk())")
            exp = read_value(val)
        else:
            emit("proc main() i32 {", f"    var acc = {fld}")
            exp = (val[0] + val[2]) if shape == "nested" else (val[0] + val[1])
    elif carrier == "array_elem":
        emit(*reader_fn("rd", "s"))
        emit("proc main() i32 {",
             f"    var arr {TY}[2] = [{val_expr}, {val_expr}]",
             "    var acc = rd(arr[0]) +% rd(arr[1])")
        exp = 2 * read_value(val)
    else:                            # local
        if op == "match" or shape in ADTS:
            emit(*reader_fn("rd", "s"))
            emit("proc main() i32 {", f"    var v = {val_expr}", "    var acc = rd(v)")
            exp = read_value(val)
        elif op == "write":
            fld = "v.inner.x" if shape == "nested" else "v.x"
            emit("proc main() i32 {", f"    var v = {val_expr}",
                 f"    {fld} = {fld} +% {c}",
                 "    var acc = " + ("v.inner.x +% v.inner.y +% v.n" if shape=="nested" else "v.x +% v.y"))
            exp = read_value(val) + c
        elif op == "pass_on":
            emit(*reader_fn("inner_rd", "s"))
            emit(f"func outer_rd(s {TY}) i32 {{", "    return inner_rd(s)", "}")
            emit("proc main() i32 {", f"    var v = {val_expr}", "    var acc = outer_rd(v)")
            exp = read_value(val)
        else:                        # read
            emit("proc main() i32 {", f"    var v = {val_expr}",
                 "    var acc = " + ("v.inner.x +% v.inner.y +% v.n" if shape=="nested" else "v.x +% v.y"))
            exp = read_value(val)

emit(f"    if acc == {exp} {{ return 0 }}", "    return 1", "}")

print(f"// COMBO shape={shape} carrier={carrier} op={op} expect={exp}")
print("\n".join(L))
