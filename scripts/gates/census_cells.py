#!/usr/bin/env python3
"""census_cells.py — the construct census's CELLS and the stdout each one must produce.

THE ORACLE IS THIS FILE. Every expected string below is computed here, in Python, and never
derived from the compiler. That is the whole point: lowering can DROP a construct, mark the
function `incomplete`, print a note, skip its checks and emit it anyway, so the emitted C and the
IR interpreter run the same mutilated IR, agree with each other, and disagree only with the truth.
A sanitizer sees nothing either, because the code is absent rather than undefined. Measured at
fb04c22: 17 cells were wrong and every IR-based oracle called them clean.

So: never replace an expected value with what the compiler currently prints. That turns the gate
into a baseline of the compiler instead of a statement about the language.
"""
import sys

PRE = 'extern func libc_printf(fmt *u8, ...) i32 effects io\n'
CELLS = []   # (axis, cell, program, expected_stdout)

def add(axis, cell, prog, expected):
    CELLS.append((axis, cell, PRE + prog, expected))

# ── axis 1: `case` scrutinee type ─────────────────────────────────────────────────────────────
SCRUT = [
    ("bool",   "flag bool",  "true",      '        true: return 7\n        else: return 9\n'),
    ("i8",     "x i8",       "3",         '        3: return 7\n        else: return 9\n'),
    ("i32",    "x i32",      "3",         '        3: return 7\n        else: return 9\n'),
    ("i64",    "x i64",      "3",         '        3: return 7\n        else: return 9\n'),
    ("u8",     "x u8",       "3",         '        3: return 7\n        else: return 9\n'),
    ("u32",    "x u32",      "3",         '        3: return 7\n        else: return 9\n'),
    ("u64",    "x u64",      "3",         '        3: return 7\n        else: return 9\n'),
    ("f32",    "x f32",      "1.5",       '        1.5: return 7\n        else: return 9\n'),
    ("f64",    "x f64",      "1.5",       '        1.5: return 7\n        else: return 9\n'),
    ("char",   "x u8",       "65",        "        'A': return 7\n        else: return 9\n"),
]
for name, param, arg, arms in SCRUT:
    add("case-scrutinee", name,
        'func pick(%s) u8 {\n    case %s {\n%s    }\n}\n'
        'func main() i32 effects io {\n    libc_printf("%%d\\n", pick(%s) as i32)\n    return 0\n}\n'
        % (param, param.split()[0], arms, arg),
        "7\n")

add("case-scrutinee", "u8[] string",
    'func pick(s u8[]) u8 {\n    case s {\n        "ab": return 5\n        else: return 9\n    }\n}\n'
    'func main() i32 effects io {\n    libc_printf("%d\\n", pick("ab") as i32)\n    return 0\n}\n',
    "5\n")
add("case-scrutinee", "enum",
    'type K { A, B, C }\n'
    'func pick(k K) u8 {\n    case k {\n        K.B: return 7\n        else: return 9\n    }\n}\n'
    'func main() i32 effects io {\n    libc_printf("%d\\n", pick(K.B) as i32)\n    return 0\n}\n',
    "7\n")
add("case-scrutinee", "sum with payload",
    'type M { None, Some { v u8 } }\n'
    'func pick(m M) u8 {\n    case m {\n        Some(v): return v\n        else: return 9\n    }\n}\n'
    'func main() i32 effects io {\n    libc_printf("%d\\n", pick(M.Some(7)) as i32)\n    return 0\n}\n',
    "7\n")

# ── axis 2: defer count ───────────────────────────────────────────────────────────────────────
for n in (1, 2, 16, 63, 64, 65, 70):
    body = "".join('    defer libc_printf("d\\n", %d)\n' % i for i in range(1, n + 1))
    add("defer-count", "%d defers" % n,
        'func many() i32 effects io {\n%s    return 0\n}\n'
        'func main() i32 effects io { return many() }\n' % body,
        "d\n" * n)


# ── axis 3: each TYPE through a var, an assignment and a return ───────────────────────────────
TYPES = [
    ("bool",   "bool",   "true",  "false", '%d', 'r as i32', "1"),
    ("i8",     "i8",     "7",     "3",     '%d', 'r as i32', "7"),
    ("i16",    "i16",    "7",     "3",     '%d', 'r as i32', "7"),
    ("i32",    "i32",    "7",     "3",     '%d', 'r',        "7"),
    ("i64",    "i64",    "7",     "3",     '%ld', 'r',       "7"),
    ("u8",     "u8",     "7",     "3",     '%d', 'r as i32', "7"),
    ("u16",    "u16",    "7",     "3",     '%d', 'r as i32', "7"),
    ("u32",    "u32",    "7",     "3",     '%d', 'r as i32', "7"),
    ("u64",    "u64",    "7",     "3",     '%lu', 'r',       "7"),
]
for name, ty, v1, v2, fmt, pr, want in TYPES:
    add("type-var-return", name,
        'func go() %s {\n    var a %s = %s\n    a = %s\n    return a\n}\n'
        'func main() i32 effects io {\n    r = go()\n    libc_printf("%s\\n", %s)\n    return 0\n}\n'
        % (ty, ty, v2, v1, fmt, pr),
        want + "\n")

add("type-var-return", "f32",
    'func go() f32 {\n    var a f32 = 1.0\n    a = 2.5\n    return a\n}\n'
    'func main() i32 effects io {\n    r = go()\n    libc_printf("%d\\n", (r * 2.0) as i32)\n    return 0\n}\n',
    "5\n")
add("type-var-return", "f64",
    'func go() f64 {\n    var a f64 = 1.0\n    a = 2.5\n    return a\n}\n'
    'func main() i32 effects io {\n    r = go()\n    libc_printf("%d\\n", (r * 2.0) as i32)\n    return 0\n}\n',
    "5\n")
add("type-var-return", "fixed array",
    'func go() i32 {\n    var a i32[4] = [1, 2, 3, 4]\n    a[2] = 9\n    return a[2]\n}\n'
    'func main() i32 effects io {\n    libc_printf("%d\\n", go())\n    return 0\n}\n',
    "9\n")
add("type-var-return", "struct",
    'type S { a i32, b i32 }\n'
    'func go() i32 {\n    var s S = S(1, 2)\n    s.b = 9\n    return s.b\n}\n'
    'func main() i32 effects io {\n    libc_printf("%d\\n", go())\n    return 0\n}\n',
    "9\n")
add("type-var-return", "enum",
    'type K { A, B, C }\n'
    'func go() i32 {\n    var k K = K.A\n    k = K.C\n    case k {\n        K.C: return 9\n        else: return 1\n    }\n}\n'
    'func main() i32 effects io {\n    libc_printf("%d\\n", go())\n    return 0\n}\n',
    "9\n")
add("type-var-return", "sum payload",
    'type M { None, Some { v i32 } }\n'
    'func go() i32 {\n    var m M = M.None\n    m = M.Some(9)\n    case m {\n        Some(v): return v\n        else: return 1\n    }\n}\n'
    'func main() i32 effects io {\n    libc_printf("%d\\n", go())\n    return 0\n}\n',
    "9\n")
add("type-var-return", "u8[] string",
    'func go() u8[] { return "xyz" }\n'
    'func main() i32 effects io {\n    s = go()\n    libc_printf("%d\\n", s.len as i32)\n    return 0\n}\n',
    "3\n")

# ── axis 4: each STATEMENT KIND in each CONTEXT ───────────────────────────────────────────────
# Every body must leave `acc` at 9, so the expected output is the same for all of them and a
# dropped statement shows as a different number rather than as a crash.
CTX = {
  "top level":   'func go() i32 {\n    var acc i32 = 0\n%s    return acc\n}\n',
  "loop body":   'func go() i32 {\n    var acc i32 = 0\n    var i usize = 0\n    while i < 1 {\n%s        i = i + 1\n    }\n    return acc\n}\n',
  "if branch":   'func go() i32 {\n    var acc i32 = 0\n    if 1 == 1 {\n%s    }\n    return acc\n}\n',
  "case arm":    'func go() i32 {\n    var acc i32 = 0\n    case 1 {\n        1:\n%s        else: acc = 0\n    }\n    return acc\n}\n',
  "unsafe block":'func go() i32 {\n    var acc i32 = 0\n    unsafe {\n%s    }\n    return acc\n}\n',
  "defer body":  'func go() i32 {\n    var acc i32 = 0\n%s    return acc\n}\n',
}
STMT = {
  "assign":       '        acc = 9\n',
  "var decl":     '        var t i32 = 9\n        acc = t\n',
  "if":           '        if 1 == 1 { acc = 9 }\n',
  "while":        '        var k usize = 0\n        while k < 1 {\n            acc = 9\n            k = k + 1\n        }\n',
  "case":         '        case 1 {\n            1: acc = 9\n            else: acc = 0\n        }\n',
  "assert":       '        assert(1 == 1)\n        acc = 9\n',
  "nested unsafe":'        unsafe { acc = 9 }\n',
  "comptime if":  '        comptime if 1 == 1 {\n            acc = 9\n        }\n',
}
for cname, ctpl in CTX.items():
    if cname == "defer body": continue
    for sname, sbody in STMT.items():
        body = sbody if cname != "top level" else sbody.replace("        ", "    ")
        add("stmt x context", "%s / %s" % (sname, cname),
            ctpl % body + 'func main() i32 effects io {\n    libc_printf("%d\\n", go())\n    return 0\n}\n',
            "9\n")


# ── axis 5: CONTROL FLOW. loop form x exit x placement, each run twice: once in a `diverge`
# function (so a HANG shows as MISMATCH) and once in a TOTAL one (so a false E011 shows as
# REFUSED). Every body is built so that acc lands on a value stated here.
LOOPS = {
    # name:        (header, trip values, what the body sees as the loop variable)
    "for-range":  ('    for i in 0..3 {\n', '    }\n', 'i'),
    "for-each":   ('    for i in TRIP {\n', '    }\n', 'i'),
    "while":      ('    var i i32 = 0\n    while i < 3 {\n        i = i + 1\n', '    }\n', 'i'),
}
# acc counts iterations that reach the end of the body.
#   break at i==1    -> acc = 1   (i=0 only)
#   continue at i==1 -> acc = 2   (i=0 and i=2)
#   return at i==1   -> returns 99 directly
EXITS = {
    "break":    ('        if %s == 2 { break }\n', "1"),
    "continue": ('        if %s == 2 { continue }\n', "2"),
    "return":   ('        if %s == 2 { return 99 }\n', "99"),
}
PLACE = {
    "body top level": lambda g: g,
    "inside an if":   lambda g: '        if 1 == 1 {\n    ' + g.replace('\n', '\n    ').rstrip() + '\n        }\n',
    "inside a case":  lambda g: '        case 1 {\n            1:\n    ' + g.replace('\n','\n    ').rstrip() + '\n            else: acc = acc\n        }\n',
}

# Expected acc per (loop form, exit), with the guard at the value 2. The `for` forms iterate
# 0,1,2; the `while` form increments FIRST so it sees 1,2,3. They therefore differ for `break`:
#   for   break at 2 -> 0 and 1 completed -> 2        while break at 2 -> only 1 completed -> 1
WANT = {
    ("for-range", "break"): "2", ("for-range", "continue"): "2", ("for-range", "return"): "99",
    ("for-each",  "break"): "2", ("for-each",  "continue"): "2", ("for-each",  "return"): "99",
    ("while",     "break"): "1", ("while",     "continue"): "2", ("while",     "return"): "99",
}
for lname, (lhead, ltail, lvar) in LOOPS.items():
    for ename, (etpl, ewant) in EXITS.items():
        for pname, wrap in PLACE.items():
            guard = wrap(etpl % lvar)
            body = lhead + guard + '        acc = acc + 1\n' + ltail
            pre = 'TRIP i32[3] = [0, 1, 2]\n' if lname == "for-each" else ''
            for eff, tag in (('effects io, diverge', 'diverge'), ('effects io', 'total')):
                want = WANT[(lname, ename)]
                add("control-flow/%s" % tag, "%s %s %s" % (lname, ename, pname),
                    pre + 'func go() i32 %s {\n    var acc i32 = 0\n%s    return acc\n}\n'
                    'func main() i32 %s {\n    libc_printf("%%d\\n", go())\n    return 0\n}\n'
                    % (eff, body, eff),
                    want + "\n")

# ── axis 6: DEFER ─────────────────────────────────────────────────────────────────────────────
add("defer", "defer in a loop body with break",
    'func go() i32 effects io {\n    var i i32 = 0\n    while i < 3 {\n        defer libc_printf("d\\n", 0)\n        if i == 1 { break }\n        i = i + 1\n    }\n    return 0\n}\n'
    'func main() i32 effects io { return go() }\n', "d\nd\n")
add("defer", "defer in an if branch",
    'func go() i32 effects io {\n    if 1 == 1 {\n        defer libc_printf("d\\n", 0)\n    }\n    libc_printf("after\\n", 0)\n    return 0\n}\n'
    'func main() i32 effects io { return go() }\n', "d\nafter\n")
add("defer", "defer in a case arm",
    'func go() i32 effects io {\n    case 1 {\n        1:\n            defer libc_printf("d\\n", 0)\n        else: libc_printf("no\\n", 0)\n    }\n    libc_printf("after\\n", 0)\n    return 0\n}\n'
    'func main() i32 effects io { return go() }\n', "d\nafter\n")
add("defer", "defer in an unsafe block",
    'func go() i32 effects io {\n    unsafe {\n        defer libc_printf("d\\n", 0)\n    }\n    libc_printf("after\\n", 0)\n    return 0\n}\n'
    'func main() i32 effects io { return go() }\n', "d\nafter\n")
add("defer", "defer defer (direct)",
    'func go() i32 effects io {\n    defer defer libc_printf("inner\\n", 0)\n    libc_printf("body\\n", 0)\n    return 0\n}\n'
    'func main() i32 effects io { return go() }\n', "body\ninner\n")
add("defer", "defer containing a defer block",
    'func go() i32 effects io {\n    defer if 1 == 1 {\n        defer libc_printf("inner\\n", 0)\n    }\n    libc_printf("body\\n", 0)\n    return 0\n}\n'
    'func main() i32 effects io { return go() }\n', "body\ninner\n")
add("defer", "order across nested scopes",
    'func go() i32 effects io {\n    defer libc_printf("1\\n", 0)\n    if 1 == 1 {\n        defer libc_printf("2\\n", 0)\n    }\n    defer libc_printf("3\\n", 0)\n    libc_printf("b\\n", 0)\n    return 0\n}\n'
    'func main() i32 effects io { return go() }\n', "2\nb\n3\n1\n")
add("defer", "return inside a defer body (spec 9 forbids)",
    'func go() i32 effects io {\n    defer libc_printf("outer\\n", 0)\n    defer return 7\n    return 1\n}\n'
    'func main() i32 effects io { libc_printf("%d\\n", go())  \n    return 0 }\n', "__ILLFORMED__")
add("defer", "break inside a defer body (spec 9 forbids)",
    'func go() i32 effects io {\n    var i i32 = 0\n    while i < 2 {\n        defer break\n        i = i + 1\n    }\n    return 0\n}\n'
    'func main() i32 effects io { return go() }\n', "__ILLFORMED__")

# ── axis 7: the three extra constructs ────────────────────────────────────────────────────────
add("misc", "use p.x as px in a function",
    'type P { x i32, y i32 }\nA i32[2] = [5, 6]\n'
    'func go(p P) i32 {\n    use p.x as px\n    if px < 2 { return A[px as usize] }\n    return 0\n}\n'
    'func main() i32 effects io {\n    libc_printf("%d\\n", go(P(1, 2)))\n    return 0\n}\n',
    "6\n")
add("misc", "case on a nonexistent variant",
    'type Color { Red, Green, Blue }\n'
    'func go(c Color) i32 {\n    case c {\n        Purple: return 1\n        else: return 20\n    }\n}\n'
    'func main() i32 effects io {\n    libc_printf("%d\\n", go(Color.Red))\n    return 0\n}\n',
    "__ILLFORMED__")
add("misc", "case on a struct scrutinee",
    'type P { x i32, y i32 }\n'
    'func go(p P) i32 {\n    case p {\n        else: return 20\n    }\n}\n'
    'func main() i32 effects io {\n    libc_printf("%d\\n", go(P(1, 2)))\n    return 0\n}\n',
    "20\n")
add("misc", "case on a fixed-array scrutinee",
    'func go(a i32[2]) i32 {\n    case a {\n        else: return 20\n    }\n}\n'
    'func main() i32 effects io {\n    var v i32[2] = [1, 2]\n    libc_printf("%d\\n", go(v))\n    return 0\n}\n',
    "20\n")


# ── axis 8: a PATTERN whose type does not match the scrutinee ─────────────────────────────────
add("pattern-type", "string pattern on an i32 scrutinee",
    'func pick(x i32) i32 {\n    case x {\n        "ab": return 7\n        else: return 20\n    }\n}\n'
    'func main() i32 effects io {\n    libc_printf("%d\\n", pick(1))\n    return 0\n}\n',
    "20\n")
add("pattern-type", "integer pattern on a u8[] scrutinee",
    'func pick(s u8[]) i32 {\n    case s {\n        7: return 7\n        else: return 20\n    }\n}\n'
    'func main() i32 effects io {\n    libc_printf("%d\\n", pick("ab"))\n    return 0\n}\n',
    "20\n")


add("misc", "case on a wrong enum's variant (qualified)",
    'type Color { Red, Green, Blue }\ntype Light { Red, Green }\n'
    'func go(c Color) i32 {\n    case c {\n        Light.Green: return 1\n        else: return 20\n    }\n}\n'
    'func main() i32 effects io {\n    libc_printf("%d\\n", go(Color.Red))\n    return 0\n}\n',
    "__ILLFORMED__")


# ── axis 9: the `assert` SPELLING ASYMMETRY ───────────────────────────────────────────────────
# At module scope `assert <expr>` is bare; inside a function it needs parentheses, and the bare
# form is [E100] "Expected '(' after assert/assume". H accepts the bare form in a function, so
# these cells are what makes that change measurable: REFUSED [E100] before H, OK after.
# Without them the census is blind to H, since its only assert cell used the parenthesised form.
_ASSERT_CTX = {
  "at module scope (control)":
      'assert 1 == 1\nfunc go() i32 {\n    return 9\n}\n',
  "bare, function top level":
      'func go() i32 {\n    assert 1 == 1\n    return 9\n}\n',
  "bare, in a loop body":
      'func go() i32 {\n    var i usize = 0\n    while i < 1 {\n        assert 1 == 1\n        i = i + 1\n    }\n    return 9\n}\n',
  "bare, in an if branch":
      'func go() i32 {\n    if 1 == 1 {\n        assert 1 == 1\n    }\n    return 9\n}\n',
  "bare, in a case arm":
      'func go() i32 {\n    case 1 {\n        1:\n            assert 1 == 1\n        else: return 0\n    }\n    return 9\n}\n',
  "bare, in an unsafe block":
      'func go() i32 {\n    unsafe {\n        assert 1 == 1\n    }\n    return 9\n}\n',
  "parenthesised, function top level (control)":
      'func go() i32 {\n    assert(1 == 1)\n    return 9\n}\n',
}
for _name, _body in _ASSERT_CTX.items():
    add("assert-spelling", _name,
        _body + 'func main() i32 effects io {\n    libc_printf("%d\\n", go())\n    return 0\n}\n',
        "9\n")

if __name__ == "__main__":
    import json
    print(json.dumps([{"axis": a, "cell": c, "prog": p, "want": w} for a, c, p, w in CELLS]))
