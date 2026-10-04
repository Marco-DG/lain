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
FILES = {}   # (axis, cell) -> {file name: text}, the OTHER modules of a multi-file cell

def add(axis, cell, prog, expected, plan=None, files=None):
    """`plan` names the open item for a cell that is expected to be wrong TODAY.

    The census gate refuses a baseline hole with no plan row. That citation used to be hand-written
    into census_baseline.txt, where the next `--bless` destroyed it: a documented hole silently became
    an undocumented one and the gate failed with no indication that a comment had been dropped. Keeping
    it here means blessing EMITS it, so it survives every re-bless by construction.
    """
    CELLS.append((axis, cell, PRE + prog, expected))
    if plan:
        PLAN[(axis, cell)] = plan
    if files:
        FILES[(axis, cell)] = files     # compiled from its own directory: see census_gate.sh

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
# DECIDED 2026-10-02 (I.86, reversible): a `case` scrutinee must be a type that has a pattern form,
# integers, `bool`, enums and sums, `u8[]` strings, and every refusal below states that rule in its own
# message. A float has no sound equality pattern, and a float RANGE pattern would first need NaN
# semantics designed. So f32 and f64 are refused by design and expect __ILLFORMED__. Until the decision
# they expected 7 and cited I.86 as an open question; the struct and fixed-array cells under `misc` were
# relabelled in the same commit for the same reason.
for name, param, arg, arms in SCRUT:
    add("case-scrutinee", name,
        'func pick(%s) u8 {\n    case %s {\n%s    }\n}\n'
        'func main() i32 effects io {\n    libc_printf("%%d\\n", pick(%s) as i32)\n    return 0\n}\n'
        % (param, param.split()[0], arms, arg),
        "__ILLFORMED__" if name in ("f32", "f64") else "7\n")

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
# DECIDED (I.86): a struct or a non-string array has no pattern form at all, so a `case` over one could
# only ever hold `else:`, which is its body. Refused for having no meaning beyond that, not by accident.
add("misc", "case on a struct scrutinee",
    'type P { x i32, y i32 }\n'
    'func go(p P) i32 {\n    case p {\n        else: return 20\n    }\n}\n'
    'func main() i32 effects io {\n    libc_printf("%d\\n", go(P(1, 2)))\n    return 0\n}\n',
    "__ILLFORMED__")
add("misc", "case on a fixed-array scrutinee",
    'func go(a i32[2]) i32 {\n    case a {\n        else: return 20\n    }\n}\n'
    'func main() i32 effects io {\n    var v i32[2] = [1, 2]\n    libc_printf("%d\\n", go(v))\n    return 0\n}\n',
    "__ILLFORMED__")


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
            "0\n" if want == "accept" else want)


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
        "10\n" if bname == "usize" else "__ILLFORMED__")
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
        '    return 0\n}\n' % lit, want)


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
            add("loop-guard", "%s, %s, %s" % (_gname, _cdir, _eff),
                'func f(ok bool, extra bool) i32%s {\n    var i i32 = %s\n    var s i32 = 0 - 1\n'
                '    while %s {\n        s = i\n        %s\n    }\n'
                '    libc_printf("%%d %%d\\n", i, s)\n    return 0\n}\n'
                'func main() i32%s {\n    return f(true, false)\n}\n' % (_row, _init, _g, _step, _row),
                _exp)



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
        "%d\n" % _math)


# ── axis: a `mov` leak OUTSIDE an aggregate, by the value's TYPE ─────────────────────────────────
# The I.82 defect one level out. The mov-field-leak row above is all declared fields inside a struct,
# and the fix for it is scoped to exactly that, so the same hole for a top-level `mov` binding and for
# a bare `mov` PARAMETER is invisible to every cell in this file. Two lines are enough to show it:
#
#   func sink(mov v *i32) { }     E003: a linear value is not consumed before it goes out of scope
#   func sink(mov v i32)  { }     accepted
#
# The two cells that make this an argument rather than a complaint are the last two. A top-level
# `mov x i32` IS enforced: consuming it twice is E002 and reading it after the consume is E001. So the
# machinery knows the binding is linear and declines to report only the LEAK, which is the identical
# signature the mov-field-leak row documents for fields. Not "linearity is missing here".
# The citation is I.92 and not I.82, on MCW's reading, which I accept: I.82 charges a declared `mov`
# FIELD and is a missing case of an existing rule, while this row asks whether `mov x i32` is a
# resource at all, which is a semantics choice. Their measurement for it: charging linear scalars at
# depth 0 changes no corpus file except their own I.82 trust test, which drops a `mov n i32` on purpose.
# Closed by I.92: a dropped `mov` scalar is E003 at any depth, so the two holes above now refuse.
add("mov-outside-aggregate", "integer parameter, dropped",
    'func sink(mov v i32) { }\nfunc main() i32 { return 0 }\n', "__ILLFORMED__")
add("mov-outside-aggregate", "pointer parameter, dropped",
    'func sink(mov v *i32) { }\nfunc main() i32 { return 0 }\n', "__ILLFORMED__")
add("mov-outside-aggregate", "integer binding, never consumed",
    'func go() i32 {\n    mov x i32 = 7\n    return 0\n}\n'
    'func main() i32 { return go() }\n', "__ILLFORMED__")
add("mov-outside-aggregate", "pointer binding, never consumed",
    'func go() i32 {\n    var n i32 = 0\n    mov p *i32 = &n\n    return 0\n}\n'
    'func main() i32 { return go() }\n', "__ILLFORMED__")
# The two enforcement cells consume through an EXTERN sink. A bodied `func sink(mov v i32) { }`
# drops its own parameter, so once I.92 charges a dropped scalar that sink's leak became the first error
# and these cells stopped pinning E002/E001 (MCW measured it on the I.92 stack). The obvious repair,
# `func sink(mov v i32) i32 { return v }`, works today but discharges by returning the value as a plain
# i32, which is the laundering route recorded in I.95: if returning a `mov` value as a non-`mov` type is
# ever refused, that sink breaks these cells a third time. An extern's signature is trusted under any
# such rule, so this discharge stays valid however I.95 is decided. The cells are refused, so the
# undefined extern never links.
add("mov-outside-aggregate", "integer binding, consumed twice",
    'extern func sink(v mov i32) effects io\n'
    'func go() i32 effects io {\n    mov x i32 = 7\n    sink(mov x)\n    sink(mov x)\n    return 0\n}\n'
    'func main() i32 effects io { return go() }\n', "__ILLFORMED__")
add("mov-outside-aggregate", "integer binding, read after the consume",
    'extern func sink(v mov i32) effects io\n'
    'func go() i32 effects io {\n    mov x i32 = 7\n    sink(mov x)\n    return x\n}\n'
    'func main() i32 effects io { return go() }\n', "__ILLFORMED__")


# ── axis: a `mov` PARAMETER and a plain local of the same struct type (I.93) ─────────────────────
# Found by MCW while measuring the struct half of I.92, and confirmed here. Lowering caches one
# IrType per struct per function and the `mov` qualifier sets `linear` on that shared object, so a
# `mov` parameter makes every plain local of the same type linear too:
#
#   func f(mov k Counter) i32 { a = Counter(1)  b = a  d = a ... }   E001 at `d = a`
#   func f(k Counter)     i32 { ... the identical body ... }          runs, prints 7
#
# Over-rejection only, since the flag can only ADD linearity. The third cell is the one that locates
# the bug for whoever fixes it: a `mov` parameter of a DIFFERENT struct type does not contaminate, so
# the sharing is per TYPE and not per function. A fix keyed on the function would leave that cell
# alone and the first cell broken; a fix that cleared linearity too widely would stop charging the
# parameter itself, which the mov-field-leak row above would then catch.
_I93 = ("I.93 — lowering caches one IrType per struct per function and `mov` sets `linear` on that "
        "shared object, so a `mov` parameter makes plain locals of the same type linear: `d = a` is "
        "refused E001. Over-rejection only; it also blocks measuring the struct half of I.92")
_I93_BODY = ('type Counter { n i32 }\n%s'
             'func f(%s) i32 {\n    a = Counter(1)\n    b = a\n    d = a\n'
             '    if b.n == d.n {\n        return 7\n    }\n    return 9\n}\n'
             'func main() i32 effects io {\n    libc_printf("%%d\\n", f(%s))\n    return 0\n}\n')
# DERIVED: a and its two copies all hold n == 1, so the comparison holds and f returns 7. Counter owns
# nothing, so no copy of it can be a move in the first place.
add("mov-shared-irtype", "mov parameter of the local's own type",
    _I93_BODY % ("", "mov k Counter", "Counter(0)"), "7\n", plan=_I93)
add("mov-shared-irtype", "plain parameter of the local's own type",
    _I93_BODY % ("", "k Counter", "Counter(0)"), "7\n")
add("mov-shared-irtype", "mov parameter of a DIFFERENT type",
    _I93_BODY % ("type Other { m i32 }\n", "mov k Other", "Other(0)"), "7\n")


# ── axis: the ERROR UNION — `try`, `else return`, and which payloads can carry a marker ──────────
# Until this row the census had no cell using `try`, `raises` or an error union at all, while the corpus
# has 26 tests using `try` and 29 using `raises`. That mattered on 2026-10-02: MCW's I.93 fix moved
# five try/error-union corpus tests, and the census could not have seen any of it.
#
# A union `T | Marker` must be zero-cost: no hidden tag word, so the marker lives in a value T can
# never hold. E064 enforces that, and its message names three remedies:
#
#   "Give the value type spare values (a pointer, a bool, or a refinement like `u8 < 200`)"
#
# All three work. The refinement works through a refined ALIAS, and the cell for it also prints the
# union's size, because the promise is about REPRESENTATION: a niche-packed `Small | NotFound` is one
# byte, the size of a u8, and a union that grew a tag word would print 2 while every value stayed right.
#
#   type Small = u8 < 200     a refined alias            works: niche-packed, 1 byte
#   u8 < 200 | NotFound       inline in the type         E100, a parse error at the return type
#   type Small u8 < 200       no `=`                     E100: that is the enum-width form
#   u8 | NotFound < 200       a clause after the union   PARSED, then IGNORED; E064 for the bare u8
#
# CORRECTED 2026-10-02. This row first claimed the refinement remedy "works in no spelling", from the
# last three lines alone. MCW found the alias. What survives is two smaller things, which are I.94 now:
# (a) E064's example `u8 < 200` cannot be written where a union's payload goes, so the message should
# show `type Small = u8 < 200`; and (b) a refinement clause after a union return type parses and is
# silently ignored, which is "parsed is not enforced". The last cell pins (b) with a program the clause
# should refuse: a payload that can be 199 under a clause `< 100`. The same clause on a plain return is
# E086; on the union it is accepted. My first version of that cell used `u8 | NotFound < 200` instead,
# which is refused E064 for the bare u8's lack of spare values, so it never reached the clause at all.
#
# Closed by I.94: E064 now shows the refined alias, and the clause on a union return is refused
# where it is written (E012), so the cell below is a deliberate refusal.
_EU_INNER = ('func inner(fail bool, v %s) %s | NotFound {\n    if fail {\n        return NotFound\n'
             '    }\n    return v\n}\n')
_EU_MAIN = ('func main() i32 effects io {\n'
            '    var a %s | %s = %s(false, %s)\n    case a {\n        %s: libc_printf("%s")\n'
            '        else: libc_printf("%s", a)\n    }\n'
            '    var b %s | %s = %s(true, %s)\n    case b {\n        %s: libc_printf("%s")\n'
            '        else: libc_printf("%s", b)\n    }\n'
            '    libc_printf("\\n")\n    return 0\n}\n')
def _eu_main(ty, marker, fn, arg, ch, fmt):
    return _EU_MAIN % (ty, marker, fn, arg, marker, ch, fmt, ty, marker, fn, arg, marker, ch, fmt)
# DERIVED for the three running cells: the success path prints the payload, the failure path prints
# the marker's letter. "ok" then E; "ok" then D for the remapped marker; 1 (true) then E.
add("error-union", "try, pointer payload, both paths",
    _EU_INNER % ("*u8", "*u8")
    + 'func relay(fail bool, s *u8) *u8 | NotFound {\n    r = try inner(fail, s)\n    return r\n}\n'
    + _eu_main("*u8", "NotFound", "relay", '"ok"', "E", "%s"),
    "okE\n")
add("error-union", "else return remaps the marker",
    _EU_INNER % ("*u8", "*u8")
    + 'func outer(fail bool, s *u8) *u8 | Denied {\n    r = inner(fail, s) else return Denied\n'
      '    return r\n}\n'
    + _eu_main("*u8", "Denied", "outer", '"ok"', "D", "%s"),
    "okD\n")
add("error-union", "bool payload, a remedy E064 names",
    _EU_INNER % ("bool", "bool")
    + 'func relay(fail bool, s bool) bool | NotFound {\n    r = try inner(fail, s)\n    return r\n}\n'
    + _eu_main("bool", "NotFound", "relay", "true", "E", "%d"),
    "1E\n")
add("error-union", "i32 payload, no spare values",
    _EU_INNER % ("i32", "i32") + 'func main() i32 {\n    return 0\n}\n',
    "__ILLFORMED__")
add("error-union", "try whose marker the enclosing union omits",
    _EU_INNER % ("*u8", "*u8")
    + 'func relay(fail bool, s *u8) *u8 {\n    r = try inner(fail, s)\n    return r\n}\n'
      'func main() i32 {\n    return 0\n}\n',
    "__ILLFORMED__")
# DERIVED: success prints the payload 9, failure the marker's letter E, then the size of a
# niche-packed u8 union, which is one byte.
add("error-union", "refined alias, the third remedy E064 names",
    'type Small = u8 < 200\n'
    + _EU_INNER % ("Small", "Small")
    + 'func main() i32 effects io {\n'
      '    var a Small | NotFound = inner(false, 9)\n    case a {\n        NotFound: libc_printf("E")\n'
      '        else: libc_printf("%d", a)\n    }\n'
      '    var b Small | NotFound = inner(true, 9)\n    case b {\n        NotFound: libc_printf("E")\n'
      '        else: libc_printf("%d", b)\n    }\n'
      '    libc_printf(" %d\\n", @sizeof(Small | NotFound) as i32)\n    return 0\n}\n',
    "9E 1\n")
add("error-union", "a refinement clause on a union return is ignored",
    'type Small = u8 < 200\n'
    'func inner(fail bool, v Small) Small | NotFound < 100 {\n    if fail {\n        return NotFound\n'
    '    }\n    return v\n}\n'
    'func main() i32 {\n    return 0\n}\n',
    "__ILLFORMED__")


# ── axis: `in` as ELEMENT MEMBERSHIP over a list of constants (DECIDE-X: I.78 + I.79) ─────────────
# Marco's DECIDE-X: `in` means membership in a set, and the index test is spelled `i < a.len` or
# `i in 0..a.len`. The same program across three builds is the whole argument for the decision:
#
#   if x in [1, 3, 5]  with x = 1, 2, 5, 6
#   before 9468c20     1100     meant x < 3, the INDEX of a 3-element list
#   HEAD               E012     refused: the old meaning retired loudly
#   with I.79          1010     element membership
#
# No program can slide from 1100 to 1010 silently, because HEAD refuses it in between. That is the
# migration's one safety property, and the first five cells pin where it ends up.
#
# The refusals are DESIGN, each documented in its own message: a list element must be a constant
# ("for a value known only at run time, compare: x == a or x == b"); a comprehension is built at run
# time; a runtime container's elements are "a scan, which is a function you write". The empty list
# follows from the general rule that an empty array literal is refused, not from a membership decision.
#
# The runtime-container cell pins the retirement itself. Before I.78, a runtime slice was still read
# as the INDEX test: against [7, 8, 9], `m(1)` was true and `m(7)` false, printing 100 where membership
# prints 010. I.78 refuses it and points at `x in 0..a.len` or a scan function, which is what this cell
# now holds in place.
# Closed by I.79: the five value cells below print the membership values derived above, in C at
# -O0 and -O2 and in the interpreter.
_MEM = 'func m(%s) %s {\n%s}\n'
_IF1 = '    if %s {\n        return 1\n    }\n    return 0\n'
def _mem_main(fmt, args):
    return 'func main() i32 effects io {\n    libc_printf("%s\\n", %s)\n    return 0\n}\n' % (fmt, args)
# DERIVED: each digit is 1 when the argument is an element of the list, 0 otherwise.
add("membership", "i32 elements, branch position",
    _MEM % ("x i32", "i32", _IF1 % "x in [1, 3, 5]")
    + _mem_main("%d%d%d%d", "m(1), m(2), m(5), m(6)"), "1010\n")
add("membership", "i32 elements, value position",
    _MEM % ("x i32", "bool", "    return x in [1, 3, 5]\n")
    + _mem_main("%d%d%d%d", "m(1) as i32, m(2) as i32, m(5) as i32, m(6) as i32"), "1010\n")
add("membership", "negative literal elements",
    _MEM % ("x i32", "i32", _IF1 % "x in [-3, 0, 4]")
    + _mem_main("%d%d%d%d", "m(0 - 3), m(0 - 1), m(0), m(4)"), "1011\n")
add("membership", "character literal elements",
    _MEM % ("c u8", "i32", _IF1 % "c in ['a', 'e', 'i']")
    + _mem_main("%d%d%d", "m('a'), m('b'), m('i')"), "101\n")
add("membership", "u64 against small constants, including u64 max",
    _MEM % ("x u64", "i32", _IF1 % "x in [1, 2, 3]")
    + 'func main() i32 effects io {\n    var big u64 = 0\n    big = big -% 1\n'
      '    libc_printf("%d%d%d\\n", m(2), m(7), m(big))\n    return 0\n}\n', "100\n")
add("membership", "a runtime container",
    _MEM % ("x usize, a u8[]", "i32", _IF1 % "x in a") + 'func main() i32 {\n    return 0\n}\n',
    "__ILLFORMED__")
add("membership", "a non-constant element",
    _MEM % ("x i32, p i32, q i32", "i32", _IF1 % "x in [p, q]") + 'func main() i32 {\n    return 0\n}\n',
    "__ILLFORMED__")
add("membership", "a comprehension",
    _MEM % ("x i32", "i32", _IF1 % "x in [i for i in 0..4]") + 'func main() i32 {\n    return 0\n}\n',
    "__ILLFORMED__")
add("membership", "an empty list",
    _MEM % ("x i32", "i32", _IF1 % "x in []") + 'func main() i32 {\n    return 0\n}\n',
    "__ILLFORMED__")


# ── axis: a call returning an error union, BOUND versus passed STRAIGHT to a union parameter (I.96) ─
# Found by MCW while writing I.94's trust test, confirmed here. The same value, the same parameter type,
# and the only difference is whether the call is bound to a name first:
#
#   var r *u8 | NotFound = find(false, "ok")
#   show(r)                                          runs, prints ok
#   show(find(false, "ok"))                          E012: cannot implicitly convert '?' to the union
#
# Measured with three payload types (a pointer, a bool, a refined alias) on HEAD and at the top of
# chain9: identical in all six, so the cells use the pointer alone.
#
# What the '?' hid. On HEAD the message reads "cannot implicitly convert '?' to '__U_ptr_u8_NotFound'".
# With I.94's type printer, which learned to name unions, the same refusal reads:
#
#   cannot implicitly convert '*u8 | NotFound' to '__U_ptr_u8_NotFound'
#
# So the call DOES have a type: the union as written. The parameter holds the compiler's synthesized
# form of the same union, and the two are not recognised as one type. The '?' was only the old printer
# failing to name the written form. An annotated binding works because `var r *u8 | NotFound` gives `r`
# the synthesized form, which then matches the parameter; an argument has no such step.
#
# A related refusal, with no cell here: binding the call WITHOUT an annotation, `r = find(false, "ok")`,
# is E012 "whose type cannot be inferred. Annotate it explicitly". That one has its own message and may
# be deliberate, so the row holds only the two forms whose meaning is not in question. Separately, the
# right-hand type in the message is an internal mangled name; after a fix both sides would print
# `*u8 | NotFound`, which is itself the clearest statement of the bug.
# Closed by I.96: the call's union is lowered as the parameter's was, so the direct form runs, and a
# mismatch prints both unions as written.
_U96 = ('func find(fail bool, v *u8) *u8 | NotFound {\n    if fail {\n        return NotFound\n    }\n'
        '    return v\n}\n'
        'func show(r *u8 | NotFound) effects io {\n    case r {\n        NotFound: libc_printf("E")\n'
        '        else: libc_printf("%s", r)\n    }\n}\n')
# DERIVED: find succeeds and returns "ok", show prints the payload.
add("union-call-arg", "bound to an annotated name, then passed",
    _U96 + 'func main() i32 effects io {\n    var r *u8 | NotFound = find(false, "ok")\n    show(r)\n'
           '    libc_printf("\\n")\n    return 0\n}\n', "ok\n")
add("union-call-arg", "passed straight to the union parameter",
    _U96 + 'func main() i32 effects io {\n    show(find(false, "ok"))\n    libc_printf("\\n")\n'
           '    return 0\n}\n', "ok\n")


# ── axis: two DIFFERENT error unions sharing a marker name in one module (I.97) ─────────────────
# `NotFound` is the obvious marker for every lookup API, so the second function in a file to use it
# is ordinary code. On the base this commit lands on it is refused: in `b`, `return NotFound` is E012
# "cannot implicitly convert 'i32'", as though the marker belonged to `a`'s union. Measured with three
# payload pairings for the second union (a refined alias, a bool, `*u16`): refused in all three, so it
# is the shared NAME, not the payload. The two controls say which part matters: the SAME union in two
# functions is fine, and two different unions with different marker names are fine.
# Closed by I.97: a marker means the variant of the union it flows into, so two unions that share
# a marker name both compile.
_U97 = ('type Small = u8 < 200\n'
        'func a(f bool, s *u8) *u8 | NotFound {\n    if f {\n        return NotFound\n    }\n    return s\n}\n')
def _u97_b(ty, marker):
    return ('func b(f bool, v %s) %s | %s {\n    if f {\n        return %s\n    }\n    return v\n}\n'
            % (ty, ty, marker, marker))
def _u97_main(ty, marker, arg, fmt):
    return ('func main() i32 effects io {\n'
            '    var x *u8 | NotFound = a(false, "ok")\n    case x {\n        NotFound: libc_printf("E")\n'
            '        else: libc_printf("%%s", x)\n    }\n'
            '    var y %s | %s = b(true, %s)\n    case y {\n        %s: libc_printf("E")\n'
            '        else: libc_printf("%%%s", y)\n    }\n'
            '    libc_printf("\\n")\n    return 0\n}\n' % (ty, marker, arg, marker, fmt))
# DERIVED: `a` succeeds and prints its payload "ok"; `b` fails and prints the marker's letter "E".
add("shared-marker", "two different unions, one marker name",
    _U97 + _u97_b("Small", "NotFound") + _u97_main("Small", "NotFound", "9", "d"), "okE\n")
add("shared-marker", "the same union in two functions",
    _U97 + _u97_b("*u8", "NotFound") + _u97_main("*u8", "NotFound", '"ok"', "s"), "okE\n")
add("shared-marker", "two different unions, two marker names",
    _U97 + _u97_b("Small", "Missing") + _u97_main("Small", "Missing", "9", "d"), "okE\n")


# ── axis: a function of an IMPORTED module reading that module's top-level constant (I.98) ──────
# The first multi-file cells: each is a small project compiled from its own directory (see
# census_gate.sh), so `import lib` resolves the sibling `lib.ln` the way `import ctype` resolves
# Marco's handwritten/src/ctype.ln. That import is what found this: ctype's four classifiers read its
# CTYPE table, and on the base this lands on any function of an imported module that reads its
# module's own constant is E100 "not supported by the code generator yet (unresolved-global)".
# Nothing in std/ has a top-level constant, so the corpus had never imported one.
#
# The two controls separate the halves: the same constant read in the MAIN file is fine, and an
# imported function that reads no constant is fine. So it is the pair, not either part.
#
# I.98's other half has no cell, because the census records a refusal's code and not its location:
# a diagnostic inside an imported function named the MAIN file (`use.ln:2:12` for a line of lib.ln).
# That belongs to a corpus test with EXPECT-TEXT naming the imported file.
# Closed by I.98: lowering reads the declaration the resolver bound, so an imported function reads its
# own module's constants.
_GET7 = 'func main() i32 effects io {\n    libc_printf("%d\\n", get() as i32)\n    return 0\n}\n'
# DERIVED: the constant is 7, so `get()` prints 7; the table's element 1 is 6.
add("imported-constant", "a scalar constant, read through an imported function",
    'import lib.{get}\n' + _GET7, "7\n", files={"lib.ln": 'K u8 = 7\nfunc get() u8 {\n    return K\n}\n'})
add("imported-constant", "an array constant, read through an imported function",
    'import lib.{at}\n'
    'func main() i32 effects io {\n    libc_printf("%d\\n", at(1) as i32)\n    return 0\n}\n', "6\n",
    files={"lib.ln": 'T u8[4] = [5, 6, 7, 8]\nfunc at(i u8) u8 {\n    return T[i & 3]\n}\n'})
add("imported-constant", "an imported function that reads no constant",
    'import lib.{get}\n' + _GET7, "7\n",
    files={"lib.ln": 'func get() u8 {\n    return 7\n}\n'})
add("imported-constant", "the same constant read in the main file",
    'K u8 = 7\nfunc get() u8 {\n    return K\n}\n' + _GET7, "7\n")


# ── axis: a variant whose name is another's with a prefix and `_` (I.105) ────────────────────────
# MCW's I.105, a MISCOMPILE on HEAD: variants were recovered from their mangled names by SUFFIX, first
# match wins, so with `Red` and `Dark_Red` in one type, `Dark_Red` can become `Red`. C and the
# interpreter agree on the wrong answer, so only a derived expectation sees it. Crossed as MCW laid it
# out: plain enum, union marker and payload variant; both declaration orders; qualified and bare
# patterns; construction and matching, and exhaustiveness.
#
# Construction and matching depend on ORDER: with the short name first, `Shade.Dark_Red` constructs Red
# and `return Not_Found` returns Found; with the long name first it works by luck, and those cells are
# the allowed side. Exhaustiveness does NOT depend on order: an arm for the LONGER name is credited as
# covering the shorter one too, so a `case` holding only `Dark_Red:` passes as complete in both orders,
# and a call with the uncovered variant falls off the end and returns 0 (C at -O0 and -O2 agree, UBSan
# is silent because C does not trap a missing return, the interpreter says NOT MODELLED). An arm for
# the SHORTER name is never over-credited; the `Shade.Red:` cell is that control.
# Closed by I.105: an identifier records the variant it names, and every reader uses it, so a name
# that ends in another variant's is itself, in construction, in matching and in exhaustiveness.
def _shade_match(order, q):
    return ('type Shade { %s }\nfunc name(s Shade) u8 {\n    case s {\n        %sRed: return 82\n'
            '        %sDark_Red: return 68\n    }\n}\n'
            'func main() i32 effects io {\n    libc_printf("%%c%%c\\n", name(Shade.Red), name(Shade.Dark_Red))\n'
            '    return 0\n}\n' % (order, q, q))
def _shade_one(order, arm):
    return ('type Shade { %s }\nfunc name(s Shade) u8 {\n    case s {\n        %s: return 1\n    }\n}\n'
            'func main() i32 {\n    return 0\n}\n' % (order, arm))
# DERIVED: Red names itself R and Dark_Red names itself D, so "RD" whatever the order or spelling.
add("variant-suffix", "enum, Red declared first, qualified patterns",
    _shade_match("Red, Dark_Red", "Shade."), "RD\n")
add("variant-suffix", "enum, Red declared first, bare patterns",
    _shade_match("Red, Dark_Red", ""), "RD\n")
add("variant-suffix", "enum, Dark_Red declared first, qualified patterns",
    _shade_match("Dark_Red, Red", "Shade."), "RD\n")
add("variant-suffix", "enum, Dark_Red declared first, bare patterns",
    _shade_match("Dark_Red, Red", ""), "RD\n")
# DERIVED: each case below names one variant of two and has no `else:`, so it must be refused (E014).
add("variant-suffix", "only a qualified Dark_Red arm, Red declared first",
    _shade_one("Red, Dark_Red", "Shade.Dark_Red"), "__ILLFORMED__")
add("variant-suffix", "only a bare Dark_Red arm, Red declared first",
    _shade_one("Red, Dark_Red", "Dark_Red"), "__ILLFORMED__")
add("variant-suffix", "only a qualified Dark_Red arm, Dark_Red declared first",
    _shade_one("Dark_Red, Red", "Shade.Dark_Red"), "__ILLFORMED__")
add("variant-suffix", "only a bare Dark_Red arm, Dark_Red declared first",
    _shade_one("Dark_Red, Red", "Dark_Red"), "__ILLFORMED__")
add("variant-suffix", "only a qualified Red arm: the shorter name is never over-credited",
    _shade_one("Red, Dark_Red", "Shade.Red"), "__ILLFORMED__")
def _markers(order):
    return ('func find(k u8, s *u8) *u8 | %s {\n    if k == 1 {\n        return Found\n    }\n'
            '    if k == 2 {\n        return Not_Found\n    }\n    return s\n}\n'
            'func show(k u8) effects io {\n    var r *u8 | %s = find(k, "v")\n    case r {\n'
            '        Found: libc_printf("F")\n        Not_Found: libc_printf("N")\n'
            '        else: libc_printf("%%s", r)\n    }\n}\n'
            'func main() i32 effects io {\n    show(1)\n    show(2)\n    show(0)\n    libc_printf("\\n")\n'
            '    return 0\n}\n' % (order, order))
# DERIVED: k=1 returns Found "F", k=2 returns Not_Found "N", anything else returns the payload "v".
add("variant-suffix", "union markers, Found declared first", _markers("Found | Not_Found"), "FNv\n")
add("variant-suffix", "union markers, Not_Found declared first", _markers("Not_Found | Found"), "FNv\n")
def _payload(order):
    return ('type Res { %s }\nfunc show(r Res) effects io {\n    case r {\n        Res.Found: libc_printf("F")\n'
            '        Res.Not_Found(c): libc_printf("%%d", c as i32)\n    }\n}\n'
            'func main() i32 effects io {\n    show(Res.Found)\n    show(Res.Not_Found(7))\n'
            '    libc_printf("\\n")\n    return 0\n}\n' % order)
# DERIVED: Found prints F, Not_Found(7) prints its payload 7. With the short name first the pattern
# `Res.Not_Found(c)` resolves to the payload-less Found, `c` is never bound, and codegen refuses it.
add("variant-suffix", "payload variant, Found declared first",
    _payload("Found, Not_Found { code u8 }"), "F7\n")
add("variant-suffix", "payload variant, Not_Found declared first",
    _payload("Not_Found { code u8 }, Found"), "F7\n")


# ── axis: what TYPE a bare variant has (I.103) ───────────────────────────────────────────────────
# MCW's I.103: a bare variant was typed i32. So `x i32 = Green` read the tag, `x i32 = NotFound` read
# the niche, `NotFound == 0` compiled, and the interpreter said NOT MODELLED for all of them. In the fix
# a variant is typed as its own enum or union: a plain enum's bare variant outside a pattern is E012
# naming `Color.Green`, and a union marker is a value of its union. Union markers stay bare by design.
#
# The last two cells are the allowed side. Binding a marker by inference and then using it as the union
# is REFUSED on the base this lands on, because the marker is an i32 there, and runs once it is typed as
# the union: that cell is an over-rejection I.103 closes. Arithmetic on a marker is refused both before
# and after; only its code moves, E086 (tag arithmetic) to E012, so it carries no citation.
# Closed by I.103: a bare variant is typed as its own enum or union, so it is never an integer, and a
# marker bound by inference is a value of its union.
_U103 = ('type Small = u8 < 200\ntype Color { Red, Green }\n'
         'func pick(f bool, v Small) Small | NotFound {\n    if f {\n        return NotFound\n    }\n'
         '    return v\n}\n')
def _go103(body):
    return _U103 + 'func go() i32 {\n%s\n}\nfunc main() i32 {\n    return go()\n}\n' % body
add("bare-variant-type", "a plain enum's bare variant stored in an i32",
    _go103('    x i32 = Green\n    return 0'), "__ILLFORMED__")
add("bare-variant-type", "a union marker stored in an i32",
    _go103('    x i32 = NotFound\n    return 0'), "__ILLFORMED__")
add("bare-variant-type", "a union marker compared with ==",
    _go103('    if NotFound == 0 {\n        return 1\n    }\n    return 0'), "__ILLFORMED__")
add("bare-variant-type", "a union marker stored in a u8 inside unsafe",
    _go103('    unsafe {\n        z u8 = NotFound\n    }\n    return 0'), "__ILLFORMED__")
add("bare-variant-type", "arithmetic on a union marker",
    _go103('    y = NotFound + 1\n    return 0'), "__ILLFORMED__")
# DERIVED: `w` holds the marker, so matching it as the union takes the NotFound arm: "N".
add("bare-variant-type", "a marker bound by inference, then used as its union",
    _U103 + 'func main() i32 effects io {\n    w = NotFound\n    var r Small | NotFound = w\n    case r {\n'
            '        NotFound: libc_printf("N")\n        else: libc_printf("%d", r)\n    }\n'
            '    libc_printf("\\n")\n    return 0\n}\n', "N\n")
# DERIVED: Color.Green matches its own arm: "G".
add("bare-variant-type", "a plain enum's qualified variant",
    _U103 + 'func main() i32 effects io {\n    c Color = Color.Green\n    case c {\n'
            '        Color.Red: libc_printf("R")\n        Color.Green: libc_printf("G")\n    }\n'
            '    libc_printf("\\n")\n    return 0\n}\n', "G\n")


# ── axis: two ENUM names where one is a suffix of the other (I.105, its third matcher) ───────────
# Found by MCW while closing the qualified-arm hole above: exhaustiveness also looked up the
# SCRUTINEE's enum by suffix. With `type Shade { A, B }` declared before `type Dark_Shade { A, B, C }`,
# a `case` on a Dark_Shade with bare arms A and B was checked against Shade, which those two arms do
# cover, and was accepted; a call with Dark_Shade.C fell off the end and returned 0. With qualified
# arms it is refused, but for a false reason: E106 "`Dark_Shade.A` is not a variant of 'Shade'". With
# Dark_Shade declared first it works by luck. The last cell is the allowed side: the same two arms ARE
# complete for Shade.
def _two_enums(first, second, scrut, arms):
    return ('type %s\ntype %s\nfunc f(s %s) u8 {\n    case s {\n%s    }\n}\nfunc main() i32 {\n    return 0\n}\n'
            % (first, second, scrut, arms))
_SH, _DSH = "Shade { A, B }", "Dark_Shade { A, B, C }"
# DERIVED: Dark_Shade has three variants and these arms name two, with no `else:`: refused, E014.
add("enum-name-suffix", "bare arms on Dark_Shade, Shade declared first",
    _two_enums(_SH, _DSH, "Dark_Shade", "        A: return 1\n        B: return 2\n"), "__ILLFORMED__")
add("enum-name-suffix", "bare arms on Dark_Shade, Dark_Shade declared first",
    _two_enums(_DSH, _SH, "Dark_Shade", "        A: return 1\n        B: return 2\n"), "__ILLFORMED__")
add("enum-name-suffix", "qualified arms on Dark_Shade, Shade declared first",
    _two_enums(_SH, _DSH, "Dark_Shade", "        Dark_Shade.A: return 1\n        Dark_Shade.B: return 2\n"),
    "__ILLFORMED__")
# DERIVED: Shade has exactly A and B, so the case is complete: A prints '1', B prints '2'.
add("enum-name-suffix", "the same two arms on Shade, which they do complete",
    'type %s\ntype %s\nfunc f(s Shade) u8 {\n    case s {\n        Shade.A: return 49\n        Shade.B: return 50\n    }\n}\n'
    'func main() i32 effects io {\n    libc_printf("%%c%%c\\n", f(Shade.A), f(Shade.B))\n    return 0\n}\n' % (_SH, _DSH),
    "12\n")


# ── axis: a `case` payload pattern walked as a CALL (I.111) ───────────────────────────────────────
# The call-site spelling rules (E007 `mov`, E017 `var`) visited a bare payload pattern `Full(v):` as a
# call to the mangled constructor `<module>_Box_Full`. That name resolves to no function, so a fallback
# looked it up by SUFFIX, first match wins, and an UNRELATED function that happens to be named `Full`
# matched: its `mov` or `var` parameter was then demanded of the pattern binding `v`. So renaming an
# unrelated function changes whether a correct program compiles. Found by instrumenting the fallback and
# running the whole corpus through it: it ran 665 times and the suffix pass never once matched, because
# no test names a function after a variant. The qualified pattern and the no-clash program are the
# allowed side.
# Closed by I.111: a `case` pattern is not a call site, and the lookup's suffix pass takes only a unique
# match.
def _box(fn, pat):
    return ('type Box { Empty, Full { v i32 } }\n%s'
            'func main() i32 effects io {\n    var n i32 = 5\n    b = Box.Full(n)\n    case b {\n'
            '        %s: libc_printf("%%d\\n", v)\n        else: libc_printf("E\\n")\n    }\n    return 0\n}\n'
            % (fn, pat))
_FULL_MOV = 'func Full(mov x i32) i32 {\n    return x\n}\n'
_FULL_VAR = 'func Full(var x i32) {\n    x = 1\n}\n'
# DERIVED: the box holds 5, so the Full arm binds v = 5 and prints it.
add("pattern-as-call", "a bare payload pattern beside an unrelated func Full(mov ...)",
    _box(_FULL_MOV, "Full(v)"), "5\n")
add("pattern-as-call", "a bare payload pattern beside an unrelated func Full(var ...)",
    _box(_FULL_VAR, "Full(v)"), "5\n")
add("pattern-as-call", "a qualified payload pattern beside the same function",
    _box(_FULL_MOV, "Box.Full(v)"), "5\n")
add("pattern-as-call", "a bare payload pattern with no function of that name",
    _box("", "Full(v)"), "5\n")


# ── axis: the call-site spelling rules at an EXTERN call (I.112) ─────────────────────────────────
# A `mov` parameter must be handed its argument with `mov` written, and a `var` parameter with `var`,
# so that a transfer or a mutation is visible where it happens. A bodied function enforces both. An
# extern enforced neither: the check took the callee's declaration only when it was a bodied function,
# and an extern fell through to a lookup that found it and then returned without checking. Ownership is
# still tracked (after `sink(p)` a second `sink(p)` is E001), so for `mov` the gap is the spelling only.
# The allowed side runs through a real C function: `libc_puts` declared with a `mov` parameter, which
# the census maps to puts and the interpreter also runs.
#
# Not here: an extern `var` parameter is emitted BY VALUE in the C prototype while a `var` call passes
# the address, so `frexp(8.0, var e)` segfaults. The interpreter does not model frexp, so a census cell
# expecting "4" could never pass; it belongs to a corpus test that runs the compiled binary.
# Closed by I.112: an extern's `mov` and `var` parameters are checked at the call like a function's.
add("extern-call-spelling", "an extern's mov parameter given its argument without mov",
    'extern func sink(v mov *u8) effects io\nfunc main() i32 effects io {\n    mov s *u8 = "hi"\n'
    '    sink(s)\n    return 0\n}\n', "__ILLFORMED__")
# DERIVED: libc_puts prints its argument and a newline.
add("extern-call-spelling", "an extern's mov parameter given its argument with mov",
    'extern func libc_puts(s mov *u8) i32 effects io\nfunc main() i32 effects io {\n    mov s *u8 = "hi"\n'
    '    libc_puts(mov s)\n    return 0\n}\n', "hi\n")
add("extern-call-spelling", "an extern's var parameter given its argument without var",
    'extern func bump(x var i32) effects io\nfunc main() i32 effects io {\n    var n i32 = 1\n'
    '    bump(n)\n    return 0\n}\n', "__ILLFORMED__")


# ── axis: a RUNTIME array length (I.118) ──────────────────────────────────────────────────────────
# A local `var a u8[n]` with a runtime length allocates n elements in the frame and gives the array
# len n. Nothing required n >= 0. A signed n of -1 allocated (size_t)-1 bytes, the array's len became
# (size_t)-1, and the guarded `if 5 < a.len { a[5] = 7 }` below was proven in bounds and wrote past the
# frame (ASan: dynamic-stack-buffer-overflow WRITE; -O2 printed 7). Only the interpreter objected ("a
# slice of -1 elements"), so C against the interpreter is the instrument for this shape. An unsigned
# length needs no test. A signed one needs `n >= 0` proven first, and the test must be the right one:
# `n < -1` lets -1 through. Elements are u8 throughout, so the allocation's size is the length itself.
# Closed by I.118: a signed runtime length is an E085 obligation, `n >= 0`, at the declaration.
def _len_prog(param, body, args):
    return ('func f(n %s) i32 {\n%s}\n'
            'func main() i32 effects io {\n    libc_printf("%s\\n", %s)\n    return 0\n}\n'
            % (param, body, " ".join(["%d"] * len(args)), ", ".join("f(%d)" % a for a in args)))
def _arr(name, ind="    "):
    return (ind + 'var a u8[%s]\n' % name + ind + 'if 5 < a.len {\n' + ind + '    a[5] = 7\n'
            + ind + '    return a[5] as i32\n' + ind + '}\n' + ind + 'return 1\n')
def _f(n, least=None):
    # DERIVED: a length below `least` takes the early return (0); otherwise the array has n
    # elements, and index 5 exists exactly when 5 < n (7), else 1.
    if least is not None and n < least:
        return 0
    return 7 if 5 < n else 1
def _out(args, least=None):
    return " ".join(str(_f(a, least)) for a in args) + "\n"
_EARLY = '    if n < 0 {\n        return 0\n    }\n'
for t in ("i8", "i32", "i64"):
    add("array-runtime-length", "an %s length with no test" % t, _len_prog(t, _arr("n"), [8]),
        "__ILLFORMED__")
add("array-runtime-length", "an i32 length after `if n < -1 { return 0 }`: one short",
    _len_prog("i32", '    if n < -1 {\n        return 0\n    }\n' + _arr("n"), [8]),
    "__ILLFORMED__")
add("array-runtime-length", "an i32 length copied into an immutable local with no test",
    _len_prog("i32", '    m = n\n' + _arr("m"), [8]), "__ILLFORMED__")
add("array-runtime-length", "a constant -1 bound to an immutable local",
    'func main() i32 effects io {\n    m i32 = -1\n    var a u8[m]\n    libc_printf("done\\n")\n'
    '    return 0\n}\n', "__ILLFORMED__")
add("array-runtime-length", "an i32 length after `if n < 0 { return 0 }`",
    _len_prog("i32", _EARLY + _arr("n"), [-1, 3, 8]), _out([-1, 3, 8], least=0))
add("array-runtime-length", "an i32 length inside `if 0 <= n`",
    _len_prog("i32", '    if 0 <= n {\n' + _arr("n", "        ") + '    }\n    return 0\n', [-1, 3, 8]),
    _out([-1, 3, 8], least=0))
add("array-runtime-length", "an i32 parameter refined `>= 0`",
    _len_prog("i32 >= 0", _arr("n"), [3, 8]), _out([3, 8]))
for t in ("usize", "u8"):
    add("array-runtime-length", "a %s length with no test" % t, _len_prog(t, _arr("n"), [3, 8]),
        _out([3, 8]))


# ── axis: a CONSTANT array length, in every position a type is written (I.84) ─────────────────────
# Spec 07: "N shall be a compile-time constant integer greater than zero". `var a i32[0]` was
# accepted, and a constant of zero or less in any other spelling (`-1`, `K - 5` with K = 5, a module
# constant 0) fell through to the runtime-length path. So a -1 here is the I.118 hole above, refused
# on that path first (E085), and a 0 is I.84's.
# The position matters as much as the spelling: a struct field, a parameter, a type alias and an inner
# dimension each read the length on a different path. A parameter whose length is zero or less can
# never be called (every call is E087), so that hole is a declaration the spec forbids, not a run.
# Closed by I.84: a constant length of zero or less is E100 in every position, with the true reason
# (a struct field was E132 and a type alias E012, both refused for something else).
_CONSTS = 'K i32 = 5\nZ i32 = 0\n'
_DONE = 'func main() i32 effects io {\n    libc_printf("done\\n")\n    return 0\n}\n'
def _local(n):
    return (_CONSTS + 'func main() i32 effects io {\n    var a i32[%s]\n    libc_printf("done\\n")\n'
            '    return 0\n}\n' % n)
# DERIVED: each program only declares the array and prints "done"; K - 2 is 3, K - 5 is 0,
# K - 6 is -1 and Z is 0.
for label, n, exp, plan in (
        ("a local of length 3", "3", "done\n", None),
        ("a local of length 0", "0", "__ILLFORMED__", None),
        ("a local of length 0x0", "0x0", "__ILLFORMED__", None),
        ("a local of length -1", "-1", "__ILLFORMED__", None),
        ("a local of length K - 2, which is 3", "K - 2", "done\n", None),
        ("a local of length K - 5, which is 0", "K - 5", "__ILLFORMED__", None),
        ("a local of length K - 6, which is -1", "K - 6", "__ILLFORMED__", None),
        ("a local of length Z, a module constant 0", "Z", "__ILLFORMED__", None),
        ("a local whose inner length is 0", "2][0", "__ILLFORMED__", None)):
    add("array-constant-length", label, _local(n), exp, plan=plan)
for label, n, exp, plan in (
        ("a struct field of length 3", "3", "done\n", None),
        ("a struct field of length 0", "0", "__ILLFORMED__", None),
        ("a struct field of length -1", "-1", "__ILLFORMED__", None)):
    add("array-constant-length", label,
        _CONSTS + 'type S {\n    a i32[%s]\n    n i32\n}\n' % n + _DONE, exp, plan=plan)
for label, n, exp, plan in (
        ("a parameter of length 3", "3", "done\n", None),
        ("a parameter of length 0", "0", "__ILLFORMED__", None),
        ("a parameter of length -1", "-1", "__ILLFORMED__", None)):
    add("array-constant-length", label,
        _CONSTS + 'func g(a i32[%s]) i32 {\n    return 1\n}\n' % n + _DONE, exp, plan=plan)
add("array-constant-length", "a type alias of length 0",
    _CONSTS + 'type A = i32[0]\n' + _DONE, "__ILLFORMED__")


# ── axis: the SIZE of a runtime-length stack array (I.121) ─────────────────────────────────────────
# A runtime-length array is allocated as n * sizeof(T) bytes, and nothing checked that product. With a
# 64-bit length and an element wider than a byte it wraps: n = 2^62 + 1 i32 elements asked for 4 bytes,
# the array's len stayed 2^62 + 1, and the guarded write a[5] was proven in bounds and landed past the
# frame (ASan: dynamic-stack-buffer-overflow WRITE), with or without I.118's `n >= 0` test in front. The
# bound is n <= SIZE_MAX / sizeof(T). A test in front that stops one short of it must be refused: for a
# 32-byte struct, `n > 2^59` lets 2^59 through, whose size 2^64 wraps to 0. The bound's other side is not
# pinned, because the compiler bounds a struct's size from above (every field rounded up to 8 bytes,
# plus 8) and the octagon holds bounds only up to about 2^60, so a test AT the exact bound may be
# refused; nothing promises it. A u8 element or a 32-bit length cannot wrap. Separately, an
# allocation larger than the stack's guard gap jumped it and wrote into another mapping. The probing
# that stops it ends in a fault, which a cell expecting output cannot state, so only its allowed side
# is here: a 3 MiB array that must still run.
# Closed by I.121: n <= SIZE_MAX / sizeof(T) is an E085 obligation at the declaration, and the
# emitted allocation touches its pages from the top down, so an oversized one faults at the guard.
def _arr_t(elem, store, read):
    return ('    var a %s[n]\n    if 5 < a.len {\n        a[5] = %s\n        return %s\n    }\n    return 1\n'
            % (elem, store, read))
_A_I32 = _arr_t("i32", "7", "a[5]")
_POS = 'type Pos {\n    line i32\n    col i32\n}\n'
_BIG = 'type Big {\n' + ''.join('    %s i32\n' % f for f in "abcdefgh") + '}\n'
_A_BIG = _arr_t("Big", "Big(7, 0, 0, 0, 0, 0, 0, 0)", "a[5].a")
_BOUND32 = (2**64 - 1) // 32       # DERIVED: SIZE_MAX / sizeof(Big), eight i32 fields = 32 bytes
def _test(cond):
    return '    if %s {\n        return 0\n    }\n' % cond
# DERIVED: as in the runtime-length axis, f(n) is 7 when index 5 exists (5 < n) and 1 otherwise; no
# argument below reaches an early return.
for label, prog, exp, plan in (
        ("i32 elements, a usize length with no test",
         _len_prog("usize", _A_I32, [8]), "__ILLFORMED__", None),
        ("i32 elements, an i64 length after `if n < 0 { return 0 }`",
         _len_prog("i64", _test("n < 0") + _A_I32, [8]), "__ILLFORMED__", None),
        ("u16 elements, a usize length with no test",
         _len_prog("usize", _arr_t("u16", "7", "a[5] as i32"), [8]), "__ILLFORMED__", None),
        ("struct elements, a usize length with no test",
         _POS + _len_prog("usize", _arr_t("Pos", "Pos(7, 0)", "a[5].line"), [8]), "__ILLFORMED__", None),
        ("32-byte struct elements, after `if n > 2^59`: one past the bound",
         _BIG + _len_prog("usize", _test("n > %d" % (_BOUND32 + 1)) + _A_BIG, [8]), "__ILLFORMED__", None),
        ("i32 elements, a usize length after `if n > 1024`",
         _len_prog("usize", _test("n > 1024") + _A_I32, [3, 8]), _out([3, 8]), None),
        ("i32 elements, a u32 length with no test",
         _len_prog("u32", _A_I32, [3, 8]), _out([3, 8]), None),
        ("u8 elements, a u64 length with no test",
         _len_prog("u64", _arr("n"), [3, 8]), _out([3, 8]), None),
        ("u8 elements, 3 MiB: deeper than a page, within the stack",
         _len_prog("usize", _arr("n"), [3145728]), _out([3145728]), None)):
    add("array-runtime-size", label, prog, exp, plan=plan)


if __name__ == "__main__":
    import json
    print(json.dumps([{"axis": a, "cell": c, "prog": p, "want": w,
                       "plan": PLAN.get((a, c)), "files": FILES.get((a, c))} for a, c, p, w in CELLS]))