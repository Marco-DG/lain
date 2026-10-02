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
PLAN = {}    # (axis, cell) -> the plan row that explains a KNOWN hole

def add(axis, cell, prog, expected, plan=None):
    """`plan` names the open item for a cell that is expected to be wrong TODAY.

    The census gate refuses a baseline hole with no plan row. That citation used to be hand-written
    into census_baseline.txt, where the next `--bless` destroyed it: a documented hole silently became
    an undocumented one and the gate failed with no indication that a comment had been dropped. Keeping
    it here means blessing EMITS it, so it survives every re-bless by construction.
    """
    CELLS.append((axis, cell, PRE + prog, expected))
    if plan:
        PLAN[(axis, cell)] = plan

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
        "7\n",
        plan=("I.86 — which scrutinee types `case` admits. It takes integers, u8, enums, sums and strings; it refuses f32, f64, a struct and a fixed array with E012, including when the only arm is `else:`. These four cells ask that one question, and refusing `case x { else: }` on any type is the least defensible of them" if name in ("f32", "f64") else None))

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
# CORRECTED 2026-10-02: this expected 6, the value the program would print IF `use` existed. `use` is
# the one reserved word that is refused and implements nothing, so a program containing it must be
# refused. If `use` is ever implemented the cell flips and the gate asks for a blessing, which is what
# is wanted from a cell about an absent construct.
add("misc", "use p.x as px in a function",
    'type P { x i32, y i32 }\nA i32[2] = [5, 6]\n'
    'func go(p P) i32 {\n    use p.x as px\n    if px < 2 { return A[px as usize] }\n    return 0\n}\n'
    'func main() i32 effects io {\n    libc_printf("%d\\n", go(P(1, 2)))\n    return 0\n}\n',
    "__ILLFORMED__")
add("misc", "case on a nonexistent variant",
    'type Color { Red, Green, Blue }\n'
    'func go(c Color) i32 {\n    case c {\n        Purple: return 1\n        else: return 20\n    }\n}\n'
    'func main() i32 effects io {\n    libc_printf("%d\\n", go(Color.Red))\n    return 0\n}\n',
    "__ILLFORMED__")
add("misc", "case on a struct scrutinee",
    'type P { x i32, y i32 }\n'
    'func go(p P) i32 {\n    case p {\n        else: return 20\n    }\n}\n'
    'func main() i32 effects io {\n    libc_printf("%d\\n", go(P(1, 2)))\n    return 0\n}\n',
    "20\n",
    plan="I.86 — which scrutinee types `case` admits. It takes integers, u8, enums, sums and strings; it refuses f32, f64, a struct and a fixed array with E012, including when the only arm is `else:`. These four cells ask that one question, and refusing `case x { else: }` on any type is the least defensible of them")
add("misc", "case on a fixed-array scrutinee",
    'func go(a i32[2]) i32 {\n    case a {\n        else: return 20\n    }\n}\n'
    'func main() i32 effects io {\n    var v i32[2] = [1, 2]\n    libc_printf("%d\\n", go(v))\n    return 0\n}\n',
    "20\n",
    plan="I.86 — which scrutinee types `case` admits. It takes integers, u8, enums, sums and strings; it refuses f32, f64, a struct and a fixed array with E012, including when the only arm is `else:`. These four cells ask that one question, and refusing `case x { else: }` on any type is the least defensible of them")


# ── axis 8: a PATTERN whose type does not match the scrutinee ─────────────────────────────────
# CORRECTED 2026-10-02: both cells expected the mistyped arm to be SKIPPED and `else` to run (20).
# That was wrong. This axis is about a pattern that cannot match its scrutinee, and refusing it IS the
# right answer, so the expectation is __ILLFORMED__. The old labels made the baseline record two
# correct refusals as over-rejections, which is what the citation rule now catches.
add("pattern-type", "string pattern on an i32 scrutinee",
    'func pick(x i32) i32 {\n    case x {\n        "ab": return 7\n        else: return 20\n    }\n}\n'
    'func main() i32 effects io {\n    libc_printf("%d\\n", pick(1))\n    return 0\n}\n',
    "__ILLFORMED__")
add("pattern-type", "integer pattern on a u8[] scrutinee",
    'func pick(s u8[]) i32 {\n    case s {\n        7: return 7\n        else: return 20\n    }\n}\n'
    'func main() i32 effects io {\n    libc_printf("%d\\n", pick("ab"))\n    return 0\n}\n',
    "__ILLFORMED__")


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

# ── axis: a module-scope name is defined once (R, commit 2b1cd42) ──────────────────────────────
# spec 6. Each refusal is paired with the ALLOWED case the rule must not swallow, because the rule
# is a RESTRICTION: a gate that only records the refusals cannot tell "refuses exactly this" from
# "refuses this and more". Two `extern` declarations of one function are the allowed twin here.
add("module-scope-name", "a func defined twice",
    'func f() i32 {\n    return 1\n}\nfunc f() i32 {\n    return 2\n}\n'
    'func main() i32 {\n    return f() - 1\n}\n', "__ILLFORMED__")
add("module-scope-name", "a type defined twice",
    'type P { x i32 }\ntype P { y i32, z i32 }\nfunc main() i32 {\n    return 0\n}\n', "__ILLFORMED__")
add("module-scope-name", "a constant and a func of one name",
    'helper i32 = 9\nfunc helper() i32 {\n    return 9\n}\n'
    'func main() i32 {\n    return 0\n}\n', "__ILLFORMED__")
add("module-scope-name", "two externs, parameters differ",
    'extern func ext_thing(a i32) i32\nextern func ext_thing(a i64, b i64) i64\n'
    'func main() i32 {\n    return 0\n}\n', "__ILLFORMED__")
add("module-scope-name", "two externs, effect row differs",
    'extern func ext_go(a i32) i32\nextern func ext_go(a i32) i32 effects diverge\n'
    'func main() i32 {\n    return 0\n}\n', "__ILLFORMED__")
add("module-scope-name", "two externs that AGREE (allowed)",
    'extern func libc_puts(s *u8) i32 effects io\nextern func libc_puts(s *u8) i32 effects io\n'
    'func main() i32 effects io {\n    libc_puts("ok")\n    return 0\n}\n', "ok\n")

# ── axis: an opaque or void type is not a value (R, commit 2b1cd42) ────────────────────────────
# spec 17 for `extern type`, spec 7 for void. Five positions for the opaque type and three for
# void, each named separately: the check runs over declarations, so one position passing says
# nothing about another. The twins are the two uses that STAY legal, a pointer and a return type.
for pos, prog in [
    ("a variable",       'func f() i32 {\n    var fh FILE\n    return 0\n}\nfunc main() i32 {\n    return f()\n}\n'),
    ("a parameter",      'func f(fh FILE) i32 {\n    return 0\n}\nfunc main() i32 {\n    return 0\n}\n'),
    ("a field",          'type H { fh FILE }\nfunc main() i32 {\n    return 0\n}\n'),
    ("a module constant",'G FILE = 0\nfunc main() i32 {\n    return 0\n}\n'),
    ("a return value",   'func f() FILE {\n    return 0\n}\nfunc main() i32 {\n    return 0\n}\n'),
]:
    add("opaque-and-void", "opaque type as " + pos, 'extern type FILE\n' + prog, "__ILLFORMED__")
add("opaque-and-void", "opaque type through a POINTER (allowed)",
    'extern type FILE\nextern func libc_fopen(p *u8, m *u8) *FILE\n'
    'func f(fh *FILE) i32 {\n    return 0\n}\n'
    'func main() i32 effects io {\n    libc_printf("ok\\n")\n    return 0\n}\n', "ok\n")
for pos, prog in [
    ("a variable",  'func f() i32 {\n    var x void\n    return 0\n}\nfunc main() i32 {\n    return f()\n}\n'),
    ("a parameter", 'func f(x void) i32 {\n    return 0\n}\nfunc main() i32 {\n    return 0\n}\n'),
    ("a field",     'type H { x void }\nfunc main() i32 {\n    return 0\n}\n'),
]:
    add("opaque-and-void", "void as " + pos, prog, "__ILLFORMED__")
add("opaque-and-void", "void as a RETURN TYPE (allowed)",
    'func f() void {\n    return\n}\n'
    'func main() i32 effects io {\n    f()\n    libc_printf("ok\\n")\n    return 0\n}\n', "ok\n")

# ── axis: an import list names only what the module reaches (M, commit 1463bd5) ────────────────
# spec 16. These carry an `import`, so they are the first cells that cannot be compiled from an
# absolute path: see CELLDIR in census_gate.sh. Both ORDERS are cells, because the first version of
# the check read the imported module's declaration list and the order of two imports decided.
def add_raw(axis, cell, prog, expected):
    CELLS.append((axis, cell, prog, expected))        # no PRE: an import must come first

add_raw("import-list", "an unknown name in the list, used",
    'import std.math.{max, mxa}\n\nfunc main() i32 {\n    return max(1, 1) - 1\n}\n', "__ILLFORMED__")
add_raw("import-list", "an unknown name in the list, never used",
    'import std.math.{mxa}\n\nfunc main() i32 {\n    return 0\n}\n', "__ILLFORMED__")
add_raw("import-list", "a name through the module's own import, c then io",
    'import std.c.{fopen}\nimport std.io.{printf}\n\n'
    'func main() i32 effects io {\n    printf("ok\\n")\n    return 0\n}\n', "ok\n")
add_raw("import-list", "a name through the module's own import, io then c",
    'import std.io.{printf}\nimport std.c.{fopen}\n\n'
    'func main() i32 effects io {\n    printf("ok\\n")\n    return 0\n}\n', "ok\n")

# ── axis: a qualifier is the importing file's (O, commit 6a3f1b1) ──────────────────────────────
# spec 16. The third refusal is the one that returned a WRONG ANSWER rather than refusing: with
# `var helper = 9` in scope, `math.helper` read the local, so `math.helper - 7` returned 2.
add_raw("qualifier", "a qualifier the file does not import",
    'import std.io\n\nfunc main() i32 effects io {\n    c.printf("x\\n")\n    return 0\n}\n',
    "__ILLFORMED__")
add_raw("qualifier", "a qualifier of the wrong module",
    'import std.io\nimport std.math\n\n'
    'func main() i32 effects io {\n    math.printf("x\\n")\n    return 0\n}\n', "__ILLFORMED__")
add_raw("qualifier", "a qualifier that names a LOCAL",
    'import std.math\n\nfunc go() i32 {\n    var helper = 9\n    return math.helper - 7\n}\n'
    'func main() i32 {\n    return go()\n}\n', "__ILLFORMED__")
add_raw("qualifier", "a selective import's qualifier (allowed)",
    'import std.math.{max}\nextern func libc_printf(fmt *u8, ...) i32 effects io\n\n'
    'func main() i32 effects io {\n    if math.min(max(1, 2), 3) != 2 {\n        return 1\n    }\n'
    '    libc_printf("ok\\n")\n    return 0\n}\n', "ok\n")
add_raw("qualifier", "an alias (allowed)",
    'import std.io as o\n\nfunc main() i32 effects io {\n    o.printf("ok\\n")\n    return 0\n}\n',
    "ok\n")
add_raw("qualifier", "a name through the qualified module's import (allowed)",
    'import std.io\n\nfunc main() i32 effects io {\n    io.printf("ok\\n")\n    return 0\n}\n',
    "ok\n")

# ── axis: a `case` that names every variant needs no `else:` (X, I.47) ──────────────────────────
# spec 15. Every other cell with a `case` has an `else:` arm, so exhaustiveness was never tested
# here. The qualified spelling (`Color.Red`) counted for nothing toward it, so a complete `case`
# was refused as non-exhaustive, and one qualified arm among bare ones was enough (Handwriting).
# The refusing twin keeps the rule from being confirmed by over-rejection.
_EXH = 'type Color { Red, Green }\n'
def _exh(arms):
    return (_EXH + 'func pick(c Color) i32 {\n    case c {\n' + arms + '    }\n}\n'
            'func main() i32 effects io {\n    libc_printf("%d\\n", pick(Color.Green))\n    return 0\n}\n')
add("case-exhaustive", "every variant, bare spelling",
    _exh('        Red: return 3\n        Green: return 7\n'), "7\n")
add("case-exhaustive", "every variant, qualified spelling",
    _exh('        Color.Red: return 3\n        Color.Green: return 7\n'), "7\n")
add("case-exhaustive", "one qualified arm among bare ones",
    _exh('        Color.Red: return 3\n        Green: return 7\n'), "7\n")
add("case-exhaustive", "a missing variant, no else",
    _exh('        Red: return 3\n'), "__ILLFORMED__")

# ── axis: an operator with an overflow policy, literal on the LEFT vs the RIGHT (I.56) ──────────
# The operation has ONE integer type, its operands'. A literal on the left made it an i32: `100 +% x`
# on an i64 printed 79, `100 +| x` on a u32 printed 2147483647, and the C and the interpreter agreed
# (the IR was typed wrong). Expected values are computed here, from the operator's definition.
def _pol_rng(t):
    b = int(t[1:])
    return (-(1 << (b - 1)), (1 << (b - 1)) - 1) if t[0] == 'i' else (0, (1 << b) - 1)
def _pol_val(op, a, b, t):
    lo, hi = _pol_rng(t)
    exact = {'+': a + b, '-': a - b, '*': a * b}[op[0]]
    if op[1] == '%':
        m = 1 << int(t[1:]); v = exact % m
        return v - m if t[0] == 'i' and v > hi else v
    if op[1] == '|': return max(lo, min(hi, exact))
    return exact if lo <= exact <= hi else 7          # `+? ... else 7`
POLICY_CELLS = [   # (op, type, literal, run-time operand)
    ('+%', 'i64', 100, 9223372036854775787), ('-%', 'i64', 100, -9223372036854775788),
    ('*%', 'i64', 3, 4611686018427387904),   ('+%', 'u8', 250, 10),
    ('+|', 'u32', 100, 4294967275),          ('*|', 'u32', 100, 4294967275),
    ('-|', 'u32', 100, 4294967275),          ('-|', 'i8', -100, 100),
    ('+?', 'u32', 100, 4294967275),          ('+?', 'i16', 100, 5),
]
for op, t, lit, arg in POLICY_CELLS:
    fmt, wide = ('%lld', 'i64') if t[0] == 'i' else ('%llu', 'u64')
    tail = ' else 7' if op.endswith('?') else ''
    for side, expr, a, b in (('left', '%d %s x%s' % (lit, op, tail), lit, arg),
                             ('right', 'x %s %d%s' % (op, lit, tail), arg, lit)):
        add("policy-op-literal", "%s %s literal-%s" % (op, t, side),
            'func f(x %s) %s {\n    return %s\n}\n'
            'func main() i32 effects io {\n    libc_printf("%s\\n", f(%d) as %s)\n    return 0\n}\n'
            % (t, t, expr, fmt, arg, wide),
            "%d\n" % _pol_val(op, a, b, t))

# ── axis: the BOUNDARY of I.56's rule, both orders (Handwriting) ───────────────────────────────
# The rule: the operation's TYPE comes from the other operand; the literal keeps its EXACT VALUE and
# the policy applies to the exact result. These ten pin the cases the rule has to decide and that the
# twenty above do not reach. Expected values are DERIVED here from that statement, not copied from a
# measurement — a measurement agreeing with the derivation is a check, not the oracle.
#
#   `x +% 300` on a u8 is NOT "300 truncated to 44, then added": it is 10 + 300 = 310 wrapped in u8.
#   The two happen to coincide for `+%`; they do not for `+|`, which clamps the EXACT sum to 255.
def _b_wrap(v, bits, signed):
    m = 1 << bits
    v %= m
    return v - m if signed and v >= (1 << (bits - 1)) else v

for lit, arg, want in [(300, 10, _b_wrap(10 + 300, 8, False)),      # a literal OUT OF RANGE for u8
                       (-100, 10, _b_wrap(10 - 100, 8, False))]:    # a NEGATIVE literal, unsigned operand
    for side, expr in (("literal-left", "%d +%% x" % lit), ("literal-right", "x +%% %d" % lit)):
        add("policy-op-boundary", "u8 %d %s" % (lit, side),
            'func f(x u8) u8 {\n    return %s\n}\n'
            'func main() i32 effects io {\n    libc_printf("%%d\\n", f(%d) as i32)\n    return 0\n}\n'
            % (expr, arg), "%d\n" % want)

# Two STATED types where neither holds every value of the other: refused in BOTH orders, since the
# rule cannot choose and choosing by position is what I.56 removed.
for ta, tb, va, vb in [("i32", "u32", 1, 1), ("i8", "u64", 1, 1)]:
    for side, expr in (("a-first", "a +% b"), ("b-first", "b +% a")):
        add("policy-op-boundary", "%s/%s %s" % (ta, tb, side),
            'func f(a %s, b %s) %s {\n    return %s\n}\n'
            'func main() i32 { return 0 }\n' % (ta, tb, ta, expr), "__ILLFORMED__")

# A WIDENING pair: i32 holds every u8, so the operation is i32 in both orders and the answer does not
# depend on which side the u8 is written. 200 + (-300) = -100 exactly, and -100 fits i32.
for side, expr in (("a-first", "a +% b"), ("b-first", "b +% a")):
    add("policy-op-boundary", "u8/i32 widening %s" % side,
        'func f(a u8, b i32) i32 {\n    return %s\n}\n'
        'func main() i32 effects io {\n    libc_printf("%%d\\n", f(200, 0 - 300))\n    return 0\n}\n'
        % expr, "%d\n" % (200 + (-300)))


# ── axis: a `mov` field's leak, by the field's TYPE (I.82) ─────────────────────────────────────
# Documentation found that a `mov` field of non-pointer type is consumed-exactly-once on some paths
# and not others: E016 fires, E003 does not. Verified as a 2x3 matrix, and the shape is why this is a
# census row rather than one test: FIVE of the six cells are correct today, and the fix must flip
# exactly one and leave the others alone.
#
#   field type   full consume   partial (some paths)   never consumed
#   *i32         accepted       E016                   E003
#   i32          accepted       E016                   ACCEPTED   <- the bug (I.82)
#
# The partial case firing E016 for BOTH types is the interesting part: the linearity machinery knows
# the field is linear, it just does not report the LEAK for a non-pointer. So this is a missing report
# on one path, not a missing notion of linearity — which is what the five correct cells pin.
_MOV_SHAPES = [
    ("full consume",  '    sink(mov r)\n',                                  "accept"),
    ("partial",       '    if c {\n        sink(mov r)\n    }\n',           "__ILLFORMED__"),
    ("never consumed",'    n = n + 0\n',                                    "__ILLFORMED__"),
]
for fty, tyname, init in [("*i32", "pointer", "    var r = R(&n)\n"),
                          ("i32",  "integer", "    var r = R(7)\n")]:
    for shape, body, want in _MOV_SHAPES:
        add("mov-field-leak", "%s field, %s" % (tyname, shape),
            'type R { mov h %s }\n'
            'func sink(mov {h} R) { }\n'
            'func go(c bool) i32 {\n'
            '    var n i32 = 0\n'
            '%s%s'
            '    return 0\n}\n'
            'func main() i32 effects io {\n    libc_printf("%%d\\n", go(true))\n    return 0\n}\n'
            % (fty, init, body),
            # DERIVED, not measured: `go` consumes (or leaks) the resource and returns 0 on every
            # path it can reach, so an accepted cell prints exactly "0".
            "0\n" if want == "accept" else want,
            plan=("I.82 — a `mov` field of non-pointer type leaks silently: E016 fires on the partial "
                  "path, E003 does not fire on the leak"
                  if (tyname, shape) == ("integer", "never consumed") else None))


# ── axis: a `for` bound's TYPE, by whether the body completes an iteration (I.81) ──────────────
# Documentation found that a `for` bound may be an f64. One missing typing rule, TWO symptoms, and
# which one you see depends on the BODY:
#
#   bound   body returns immediately        body completes an iteration
#   f64     ACCEPTED (silently wrong)       refused E086, at the BODY's first line, about "arithmetic"
#   usize   accepted                        accepted
#
# A returning body never steps the counter, so VRA never compares it against the float and nothing
# complains; a completing body does, and the refusal lands in the wrong place for the wrong reason
# (an f64 where an integer was wanted is not an overflow). E012 at the BOUND fixes both, so when it
# lands the first cell flips to a refusal and the second's CODE changes — which is why both are here:
# one pins the acceptance, the other pins the diagnostic.
#
# The emitted C makes the defect plain: `v3 < v0` compares an integer counter with a double.
for bname, bound, arg0, arg1 in [("f64", "f64", "0.5", "0.0"), ("usize", "usize", "1", "0")]:
    add("for-bound-type", "%s bound, body returns" % bname,
        'func f(a %s) i32 {\n    for i in 0..a {\n        return 1\n    }\n    return 0\n}\n'
        'func main() i32 effects io {\n    libc_printf("%%d\\n", f(%s) * 10 + f(%s))\n    return 0\n}\n'
        % (bound, arg0, arg1),
        # DERIVED: a non-empty bound enters the body once and returns 1; an empty bound returns 0.
        "10\n" if bname == "usize" else "__ILLFORMED__",
        plan=(None if bname == "usize" else
              "I.81 — a `for` bound may be an f64; a returning body never steps the counter, so nothing "
              "complains. The completing-body cell pins the other symptom (E086 at the body, should be "
              "E012 at the bound)"))
    add("for-bound-type", "%s bound, body completes" % bname,
        'func f(a %s) i32 effects io {\n    for i in 0..a {\n        libc_printf("x")\n    }\n'
        '    return 0\n}\n'
        'func main() i32 effects io {\n    f(%s)\n    libc_printf("\\n")\n    return 0\n}\n'
        % (bound, "4.0" if bname == "f64" else "4"),
        # DERIVED: four iterations print four x's.
        "xxxx\n" if bname == "usize" else "__ILLFORMED__")


# ── axis: the character literal (I.83) ────────────────────────────────────────────────────────
# `'ab'` is silently 'a'. Probing the whole family found 8 of 10 cases already correct, and the 8
# correct ones are the point of the row, because they are a TRAP for the obvious fix:
#
#   'a'      97   correct            'ab'    97   WRONG, should be refused (I.83)
#   '\n'     10   correct            'abc'   97   WRONG, same bug
#   '\t'      9   correct            ''           correctly refused (malformed)
#   '\\'     92   correct            '\q'         correctly refused (unknown escape)
#   '\''     39   correct
#   '\x41'   65   correct
#
# **`'\n'`, `'\t'`, `'\\'`, `'\''` and `'\x41'` are all MULTI-CHARACTER IN SOURCE and single-valued.**
# So a fix that refuses "more than one character between the quotes" breaks five working cases. The
# rule has to be "more than one character after escape processing". These five cells are what catches
# that, and they are the reason this is a row and not a test.
#
# Values are the ASCII code points, derived from the standard, not read back from the compiler.
for name, lit, want in [
        ("single",            r"'a'",    "97\n"),
        ("escape newline",    r"'\n'",   "10\n"),
        ("escape tab",        r"'\t'",   "9\n"),
        ("escape backslash",  r"'\\'",   "92\n"),
        ("escape quote",      r"'\''",   "39\n"),
        ("escape hex",        r"'\x41'", "65\n"),
        ("two characters",    r"'ab'",   "__ILLFORMED__"),
        ("three characters",  r"'abc'",  "__ILLFORMED__"),
        ("empty",             r"''",     "__ILLFORMED__"),
        ("unknown escape",    r"'\q'",   "__ILLFORMED__")]:
    add("char-literal", name,
        'func main() i32 effects io {\n    c u8 = %s\n    libc_printf("%%d\\n", c as i32)\n'
        '    return 0\n}\n' % lit, want,
        plan=("I.83 — extra characters are silently dropped; the fix must count characters AFTER escape "
              "processing, or the five escape cells break"
              if name in ("two characters", "three characters") else None))


# ── axis: a loop's GUARD crossed with the counter's DIRECTION and the function's effect row ─────
# Suggested by MCW after I.85: the corpus had no `or` loop guard with a counter at all, in any shape.
# Every rule that reads a loop test reads it as "the loop continues while this holds", which needs the
# test's ELSE to leave the loop. `while i < 4 or extra` breaks that: the header's else goes to the test
# of `extra`, INSIDE the loop. Three axes, because measuring one at a time finds none of this:
#
#   guard        total func                      effects diverge
#   single       runs                            runs
#   and          runs                            runs
#   and-swapped  REFUSED E011  <- over-reject    runs          <- the SAME program, so E011 is the rule
#   or           ACCEPTED, must be refused       ACCEPTED, must be refused
#   or + down    already E086                    already E086  <- the hole is DIRECTION-dependent
#   range-in     E100 until I.77                 E100 until I.77
#
# The and-swapped pair is the sharpest cell in the row: one program, two effect rows, and today only
# the TOTAL one is refused, which proves the refusal comes from the termination rule and not from the
# arithmetic. The `or` + down cells are the allowed side: they were already refused, for the counter's
# underflow rather than for termination, so a fix must not be credited for them.
#
# The body is `s = i`, an ASSIGNMENT. An earlier version used `s = s + i` and every cell in the row
# was refused for the running total instead, on every binary: the accumulator hid the whole axis.
_I85 = ("I.85 — a loop rule reads the header test as the loop's exit test; under `or` the header's "
        "else stays inside the loop. Fix: read the chain of EXIT tests, of which an `or` header has none")
_I77 = "I.77 — `x in lo..hi` is not yet accepted, so a range-in loop guard is E100"
for _cdir, _init, _step, _want in (("up", "0", "i = i + 1", "4 3\n"), ("down", "4", "i = i - 1", "0 1\n")):
    for _gname, _g in (("single",      "i < 4"            if _cdir == "up" else "i > 0"),
                       ("and",         "i < 4 and ok"     if _cdir == "up" else "i > 0 and ok"),
                       ("and-swapped", "ok and i < 4"     if _cdir == "up" else "ok and i > 0"),
                       ("or",          "i < 4 or extra"   if _cdir == "up" else "i > 0 or extra"),
                       ("range-in",    "i in 0..4"        if _cdir == "up" else "i in 1..=4")):
        for _eff in ("total", "diverge"):
            _row = " effects io" if _eff == "total" else " effects io, diverge"
            # DERIVED: four iterations either way. Counting up, i ends at 4 and s holds the last
            # value entered with, 3. Counting down, i ends at 0 and s holds 1. An `or` guard makes
            # the counter unbounded, so neither the loop's end nor the step's arithmetic is provable
            # and the program must be REFUSED whatever the effect row says.
            _exp = "__ILLFORMED__" if _gname == "or" else _want
            _plan = None
            if _gname == "or" and _cdir == "up":
                _plan = _I85                      # ACCEPTED today, and it must not be
            elif _gname == "and-swapped" and _eff == "total":
                _plan = _I85                      # REFUSED today, and it should run
            elif _gname == "range-in":
                _plan = _I77
            add("loop-guard", "%s, %s, %s" % (_gname, _cdir, _eff),
                'func f(ok bool, extra bool) i32%s {\n    var i i32 = %s\n    var s i32 = 0 - 1\n'
                '    while %s {\n        s = i\n        %s\n    }\n'
                '    libc_printf("%%d %%d\\n", i, s)\n    return 0\n}\n'
                'func main() i32%s {\n    return f(true, false)\n}\n' % (_row, _init, _g, _step, _row),
                _exp, plan=_plan)



# ── axis: a MIXED-SIGN comparison, where C's conversions are not mathematics (I.88) ──────────────
# MCW's I.88, a memory-safety hole on HEAD. `x < n` with x i32 and n usize: the IR, the octagon and
# the interpreter read it mathematically; C converts -1 to SIZE_MAX. The decided semantics is
# mathematics, so every expectation below is the MATHEMATICAL answer, written here from the operand
# values and never read from any oracle.
#
# Two cells carry the row on their own. `u64 > negative i32` is the dangerous one: on HEAD, C AND the
# interpreter agree on the wrong answer, because the lowering took signedness from the LEFT operand,
# so no differential between those two oracles can see it and only an external authority can.
# `u32 < negative i64` is its mirror: there C was RIGHT and the interpreter was wrong, which is the
# direction a C-vs-interpreter differential would have blamed on the backend.
# `negative i8 < u8` is the allowed side: C promotes both to int, so C is already exact there and a
# fix must leave it alone.
_I88 = ("I.88 — a mixed-sign comparison is mathematical in the IR and converted in C (-1 becomes "
        "SIZE_MAX), and the lowering took signedness from the LEFT operand, so the IR itself answered "
        "one question two ways")
for _cell, _pa, _pb, _expr, _aa, _ab, _math in [
    ("negative i32 < u64",            "i32", "u64",   "a < b",  "0 - 1", "4",          1),
    ("u64 > negative i32",            "u64", "i32",   "a > b",  "4",     "0 - 1",      1),
    ("negative i32 < u32",            "i32", "u32",   "a < b",  "0 - 1", "4",          1),
    ("negative i32 <= usize",         "i32", "usize", "a <= b", "0 - 1", "4",          1),
    ("u32 < negative i64",            "u32", "i64",   "a < b",  "4",     "0 - 1",      0),
    ("negative i32 == u32 max",       "i32", "u32",   "a == b", "0 - 1", "4294967295", 0),
    ("negative i8 < u8 (exact in C)", "i8",  "u8",    "a < b",  "0 - 1", "4",          1),
]:
    # DERIVED: -1 is less than every non-negative number, so the first four are 1; 4 is not less than
    # -1, so the fifth is 0; -1 does not equal 4294967295, so the sixth is 0. No compiler needed.
    add("mixed-sign-cmp", _cell,
        'func cmp(a %s, b %s) i32 {\n    if %s {\n        return 1\n    }\n    return 0\n}\n'
        'func main() i32 effects io {\n    libc_printf("%%d\\n", cmp(%s, %s))\n    return 0\n}\n'
        % (_pa, _pb, _expr, _aa, _ab),
        "%d\n" % _math,
        plan=(None if _cell.startswith("negative i8") else _I88))


if __name__ == "__main__":
    import json
    print(json.dumps([{"axis": a, "cell": c, "prog": p, "want": w,
                       "plan": PLAN.get((a, c))} for a, c, p, w in CELLS]))