#!/usr/bin/env python3
"""fuzz_metamorphic.py — M14's transformations: two spellings of ONE program.

THE ORACLE NEEDS NO SEMANTICS. Two spellings of the same program must get the same verdict, and if
both compile, the same output. Nothing here knows what a program is supposed to do, so this fuzzer
can use programs whose answer nobody has computed — which is why it reaches a class M13 cannot:
each spelling on its own is a legal program with a coded verdict, and only the PAIR is a finding.

The class is real and every instance so far was found by hand: a `case` qualified vs bare (X), an
instance qualifier in a first arm vs a later one (W), a guard on `r.i.s` vs binding it first (I.38),
`if` vs `while` and `+%` vs `+` precision gaps, and a cast positioned only when parenthesised.

A TRANSFORMATION MUST PRESERVE THE REST OF THE LINE. Both pattern transformations matched an arm up
to its colon and rewrote the whole line, so `Red: n = 1` became `Color.Red:` and the body was lost.
The mutilated program's verdict then differed from the base's and was reported as a finding — it
reached a peer's commit message as "X confirmed" before the census-cell corpus exposed it. A
verdict-only calibration cannot see it, because an ill-formed variant that still compiles counts as
agreement. Match what you rewrite, and keep what you did not match.

VERDICT AGREEMENT DOES NOT PROVE TWO PROGRAMS MEAN THE SAME THING. A scratch calibration script that
compared accept/refuse only counted a mutilated variant as AGREEING with its base: both compiled, so
both were "accept", though the variant had silently lost a `case` arm's body. The false claim that
came out of it reached a peer's commit message. So THE CALIBRATION IS THIS DRIVER, run over a corpus
where every pair must agree — it already compares verdict AND output, and a separate script with its
own notion of agreement is a second, weaker gatekeeper. Do not write one.

THIS INSTRUMENT'S CHARACTERISTIC FAILURE MODE IS FALSE POSITIVES FROM ITS OWN TRANSFORMATIONS: eleven
transformation bugs were found in calibration before any of them reached a verdict, and one reached a
commit message. Weigh its first number on any new corpus or transformation accordingly, and triage
every divergence before reporting it.

A NEW BASE CORPUS IS A NEW CALIBRATION. Calibration is a property of the pair (transformation,
corpus), not of the transformation: `rename-local` was clean over 276 applications on tests/ and then
produced 13 false divergences on fuzz_chain's proved programs, because their ranges end in a local
that gets renamed and tests/ happens not to. Re-run the calibration whenever the base set changes,
not only when a transformation changes.

EVERY TRANSFORMATION HERE MUST BE MEANING-PRESERVING, or the fuzzer reports its own bugs. Two that
look safe are NOT, and are deliberately absent from v1:
  - `for i in 0..n` <-> the equivalent `while`: equivalent only with no `continue` and an unchanged
    `n`. Three census cells of mine were wrong exactly there (the increment after a `continue`), and
    in a fuzzer that asymmetry fires on every program with a `continue`.
  - reordering module-scope declarations: makes INDEPENDENCE the fuzzer's problem, and
    order-dependence was a real bug (fixed in R), so the transformation is sound only against a tree
    where that fix exists.
A transformation returns None to SKIP when it cannot confidently match. Skip counts are reported:
a transformation that never applies is not a clean result, it is an unplugged one.
"""
import re
import sys

# ── helpers ───────────────────────────────────────────────────────────────────────────────────

KEYWORDS = {
    "func", "var", "return", "if", "else", "while", "for", "in", "case", "type", "extern",
    "import", "assert", "assume", "defer", "break", "continue", "effects", "decreasing",
    "as", "and", "or", "not", "true", "false", "use", "unsafe", "comptime",
    "mov", "ref", "copy", "borrow",            # modes: calibration found `rfree(mov x)`
}
# a type name, not a value: renaming one would be a different program
TYPES = {"i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "usize", "f32", "f64", "bool",
         "void", "type"}


def body_lines(src):
    """Indices of lines inside a function body (indented, not a declaration)."""
    out = []
    for i, l in enumerate(src.split("\n")):
        s = l.strip()
        if not s or s.startswith("//"):
            continue
        if l[:1] not in (" ", "\t"):
            continue                       # module scope
        out.append(i)
    return out


def ident_free(src, name):
    return re.search(r'\b%s\b' % re.escape(name), src) is None


def fresh(src, stem="zz_m"):
    for k in range(100):
        n = "%s%d" % (stem, k)
        if ident_free(src, n):
            return n
    return None



def mask_text(src):
    """Replace every comment and string literal with same-length filler.

    A rename that reaches a STRING changes the program's OUTPUT, and one that reaches a comment
    makes a diff unreadable. Calibration caught both: `// must emit m->data` became `zz_m0->data`,
    and the same substitution would have rewritten a printf format. Transformations match against
    the masked text and splice into the real one, so offsets stay valid.
    """
    out = list(src)
    i, n = 0, len(src)
    while i < n:
        if src.startswith("//", i):
            j = src.find("\n", i)
            j = n if j < 0 else j
            for k in range(i, j):
                out[k] = " "
            i = j
        elif src[i] == '"':
            j = i + 1
            while j < n and src[j] != '"':
                j += 2 if src[j] == "\\" else 1
            for k in range(i, min(j + 1, n)):
                out[k] = " "
            i = j + 1
        else:
            i += 1
    return "".join(out)


def declared_variants(src):
    """{variant: type} for each `type T { A, B }` whose body is a bare variant list."""
    out = {}
    for m in re.finditer(r'^type\s+([A-Za-z_]\w*)\s*(?:[iu]\d+|usize)?\s*\{([^}]*)\}',
                         src, re.M):
        tname, body = m.group(1), m.group(2)
        parts = [p.strip() for p in body.split(",") if p.strip()]
        # a bare variant list has no field types: every part is a single identifier
        if parts and all(re.fullmatch(r'[A-Za-z_]\w*', p) for p in parts):
            for p in parts:
                out[p] = tname
    return out


# ── the transformations ───────────────────────────────────────────────────────────────────────
# Each takes (src, rng) and returns the transformed source, or None to skip.

def t_paren_identifier(src, rng):
    """Wrap a single identifier or integer literal in parentheses, in an EXPRESSION position only.

    Parenthesising an atom cannot regroup anything, so the pair is meaning-preserving wherever an
    expression is legal. Deciding WHERE an expression is legal is the whole difficulty: calibration
    caught this transform wrapping a declared name (`b u8 = 10` -> `(b) u8 = 10`), a mode keyword
    (`rfree(mov x)` -> `rfree((mov) x)`), a marker in a union type (`bool | (Nope)`) and, worst, the
    `n` of an escape inside a string literal. Chasing each declaration form needs a parser, so
    instead this only considers two positions where an expression is certain:

      - after `return `
      - inside the argument parentheses of a call, excluding the callee

    Everything else skips. A narrow transform that never lies beats a broad one that needs triage.
    """
    masked = mask_text(src)
    ls_m = masked.split("\n")
    cands = []
    for i in body_lines(src):
        line = ls_m[i]
        if "|" in line or re.search(r'\b(case|type|func|import|extern|decreasing|effects)\b', line):
            continue
        spans = []
        m = re.match(r'^\s*return\s+', line)
        if m:
            spans.append((m.end(), len(line)))
        for c in re.finditer(r'\b[A-Za-z_]\w*\(([^()]*)\)', line):
            lo, hi = c.start(1), c.end(1)
            # A BUILTIN takes a TYPE in its first argument: `@load(u8x16, buf, i)`. Parenthesising
            # it gives "Expected type name", and a vector alias like `u8x16` is not in TYPES, so the
            # name filter cannot catch it. Skip past the first comma for any `@`-prefixed callee.
            # A CAPITALISED callee is a type application or a constructor in Lain's convention:
            # `Vec(4, i32)` -> `Vec((4), i32)` gives "expected a lane count N", and the same holds
            # for `Option(T)` and `Buf(8)`. Skipping a constructor's value arguments too costs
            # coverage, not correctness, and no rule short of a parser separates them.
            if c.group(0)[:1].isupper():
                continue
            if c.start() > 0 and line[c.start() - 1] == "@":
                comma = line.find(",", lo, hi)
                if comma < 0:
                    continue
                lo = comma + 1
            spans.append((lo, hi))
        for lo, hi in spans:
            for t in re.finditer(r'\b([A-Za-z_]\w*|\d+)\b', line[lo:hi]):
                a, b = lo + t.start(), lo + t.end()
                tok = t.group(1)
                if tok in KEYWORDS or tok in TYPES:
                    continue
                if line[:a].rstrip().endswith("."):          # a member name
                    continue
                if line[b:].lstrip().startswith((".", "[", "(", ":")):
                    continue
                cands.append((i, a, b))
    if not cands:
        return None
    i, a, b = rng.choice(cands)
    ls = src.split("\n")            # splice into the REAL text; offsets match the mask
    ls[i] = ls[i][:a] + "(" + ls[i][a:b] + ")" + ls[i][b:]
    return "\n".join(ls)


def t_rename_local(src, rng):
    """Rename a local (or a parameter) to a provably fresh name.

    Occurrences preceded by a `.` are member names and are left alone, so a local that happens to
    share a field's spelling is still renamed correctly.
    """
    masked = mask_text(src)
    names = set()
    for m in re.finditer(r'^\s+var\s+([A-Za-z_]\w*)\b', masked, re.M):
        names.add(m.group(1))
    for m in re.finditer(r'^\s+([A-Za-z_]\w*)\s*=\s*', masked, re.M):
        names.add(m.group(1))
    names -= KEYWORDS | TYPES
    names = sorted(n for n in names if n not in declared_variants(src))
    if not names:
        return None
    old = rng.choice(names)
    new = fresh(src)
    if new is None:
        return None
    # every occurrence of `old` as a word, except one directly after a dot
    out, last = [], 0
    for m in re.finditer(r'\b%s\b' % re.escape(old), masked):
        # A occurrence preceded by a `.` is a MEMBER name and keeps its spelling. But `..` is the
        # RANGE operator, not a member access: in `a[0 .. k as usize]` the text before `k` ends with
        # "..", so the naive test left every identifier after a range unrenamed, renaming the
        # declaration and not that use. Found on a new base corpus (fuzz_chain's proved programs)
        # after this transformation had been clean over 276 applications on the test corpus.
        _pre = masked[:m.start()].rstrip()
        if _pre.endswith(".") and not _pre.endswith(".."):
            continue
        out.append(src[last:m.start()] + new)
        last = m.end()
    if not out:
        return None
    return "".join(out) + src[last:]


def t_bind_return(src, rng):
    """`return <expr>` -> `t = <expr>` then `return t`, at the same indentation.

    Evaluated once either way, so effects are preserved. Skips a bare `return` and a `return` of a
    single atom (nothing to bind, and the pair would be trivial).
    """
    cands = []
    for i in body_lines(src):
        line = src.split("\n")[i]
        m = re.match(r'^(\s+)return\s+(\S.*)$', line)
        if not m:
            continue
        expr = m.group(2).strip()
        if re.fullmatch(r'[A-Za-z_]\w*|\d+', expr):
            continue
        # Calibration: `return var a.x` bound to a local first returns a reference to the LOCAL.
        # That is a different program and E017 was right to refuse it.
        if re.search(r'\b(var|ref)\b', expr) or "&" in expr:
            continue
        # Calibration: `return case r {` spans lines; binding only the first line breaks the block.
        if expr.count("(") != expr.count(")") or expr.count("{") != expr.count("}"):
            continue
        if expr.endswith("{"):
            continue
        cands.append((i, m.group(1), expr))
    if not cands:
        return None
    i, ind, expr = rng.choice(cands)
    name = fresh(src)
    if name is None:
        return None
    ls = src.split("\n")
    ls[i] = "%s%s = %s\n%sreturn %s" % (ind, name, expr, ind, name)
    return "\n".join(ls)


NEG = {"==": "!=", "!=": "==", "<": ">=", ">=": "<", ">": "<=", "<=": ">"}


def t_negate_if(src, rng):
    """`if C { A } else { B }` -> `if !C { B } else { A }`, negating a single comparison.

    Only a one-comparison condition, and only when both arms are single-line blocks, so the brace
    structure is unambiguous without parsing. Anything else skips.
    """
    ls = src.split("\n")
    cands = []
    for i, l in enumerate(ls):
        m = re.match(r'^(\s+)if\s+(.+?)\s*\{\s*$', l)
        if not m:
            continue
        cond = m.group(2)
        ops = [o for o in NEG if o in cond]
        if len(ops) != 1 or " and " in cond or " or " in cond:
            continue
        op = max(ops, key=len)
        if cond.count(op) != 1:
            continue
        # shape:  if C {   /   A   /   } else {   /   B   /   }
        if i + 4 >= len(ls):
            continue
        if not re.match(r'^\s*\}\s*else\s*\{\s*$', ls[i + 2]):
            continue
        if not re.match(r'^\s*\}\s*$', ls[i + 4]):
            continue
        cands.append((i, m.group(1), cond, op))
    if not cands:
        return None
    i, ind, cond, op = rng.choice(cands)
    new_cond = cond.replace(op, NEG[op], 1)
    out = ls[:]
    out[i] = "%sif %s {" % (ind, new_cond)
    out[i + 1], out[i + 3] = ls[i + 3], ls[i + 1]
    return "\n".join(out)


def t_qualify_pattern(src, rng):
    """A bare variant pattern in a `case` arm -> the qualified spelling (`Red:` -> `Color.Red:`).

    Spec 15 allows both. This is the pair that found X, where naming every variant with the
    qualified spelling was refused as non-exhaustive while the bare spelling compiled.
    """
    vs = declared_variants(src)
    if not vs:
        return None
    ls = src.split("\n")
    cands = []
    for i, l in enumerate(ls):
        m = re.match(r'^(\s+)([A-Za-z_]\w*)(\s*:)(.*)$', l)
        if m and m.group(2) in vs:
            cands.append((i, m.group(1), m.group(2), m.group(3), m.group(4)))
    if not cands:
        return None
    i, ind, var, tail, rest = rng.choice(cands)
    ls[i] = "%s%s.%s%s%s" % (ind, vs[var], var, tail, rest)
    return "\n".join(ls)


def t_unqualify_pattern(src, rng):
    """The inverse: `Color.Red:` -> `Red:`. Both directions, because only one of them was broken."""
    vs = declared_variants(src)
    ls = src.split("\n")
    cands = []
    for i, l in enumerate(ls):
        m = re.match(r'^(\s+)([A-Za-z_]\w*)\.([A-Za-z_]\w*)(\s*:)(.*)$', l)
        if m and m.group(3) in vs and vs[m.group(3)] == m.group(2):
            cands.append((i, m.group(1), m.group(3), m.group(4), m.group(5)))
    if not cands:
        return None
    i, ind, var, tail, rest = rng.choice(cands)
    ls[i] = "%s%s%s%s" % (ind, var, tail, rest)
    return "\n".join(ls)


COMMUTATIVE = ("+%", "+|", "+?", "*%", "*|", "*?", "+", "*")
# An ATOM: a name, an optionally negative integer, or an indexed read. No calls: swapping operands
# changes EVALUATION ORDER, so an operand with a side effect is not a meaning-preserving swap.
_ATOM = r'-?\d+|[A-Za-z_]\w*(?:\[[^\]\[]*\])?'


def t_commute(src, rng):
    """Swap the operands of a COMMUTATIVE operator: `100 +% x` <-> `x +% 100`.

    Added after the Secondary worker's I.56: an operator with an overflow policy computed in i32
    when a literal was on the LEFT, so `100 +% x` on an i64 printed 79 where `x +% 100` printed
    -9223372036854775729 — in one program, at -O0 and -O2, with the C and the interpreter agreeing.
    A commutation pair finds that class without anyone suspecting it.

    Only the WHOLE expression of a `return` or an assignment is considered, and only when it is
    exactly `atom op atom`. Swapping a sub-expression would regroup: in `a + b * c`, turning `a + b`
    into `b + a` yields `b + a * c`, a different program. Precedence is why this is narrow.
    """
    masked = mask_text(src)
    ls_m = masked.split("\n")
    cands = []
    for i in body_lines(src):
        line = ls_m[i]
        if "|" in line.replace("+|", "").replace("-|", "").replace("*|", ""):
            continue                                  # a union type, not an operator
        m = re.match(r'^(\s*)(return\s+|[A-Za-z_]\w*\s*=\s*)(.+?)\s*$', line)
        if not m:
            continue
        expr = m.group(3)
        for op in COMMUTATIVE:                        # longest first: "+%" before "+"
            mm = re.fullmatch(r'(%s)\s*%s\s*(%s)' % (_ATOM, re.escape(op), _ATOM), expr)
            if mm:
                cands.append((i, m.group(1) + m.group(2), mm.group(1), op, mm.group(2)))
                break
    if not cands:
        return None
    i, head, a, op, b = rng.choice(cands)
    ls = src.split("\n")
    ls[i] = "%s%s %s %s" % (head, b, op, a)
    return "\n".join(ls)


TRANSFORMS = {
    "paren-atom":       t_paren_identifier,
    "rename-local":     t_rename_local,
    "bind-return":      t_bind_return,
    "negate-if":        t_negate_if,
    "qualify-pattern":  t_qualify_pattern,
    "unqualify-pattern": t_unqualify_pattern,
    "commute":          t_commute,
}


# ── exit contract ─────────────────────────────────────────────────────────────────────────────
# The driver must be able to tell a DELIBERATE skip from a CRASH. Both were exit 1 at first, so a
# transformation that threw on every program would have read as "skipped 120, applied 0" — which
# looks like an inapplicable transformation rather than a broken one, and the driver would have
# reported 0 findings over 0 judgements. (The same shape as fuzz_interp.sh reading a mutator's
# usage error as GENERATOR-FAIL, which is how that line earned its keep.)
#
#   0  transformed, written to stdout
#   2  usage: wrong arguments, or an unknown transformation
#   3  SKIP: the transformation did not apply to this program (expected, and counted)
#   4  CRASH: the transformation raised. A BUG IN THIS FILE, never a finding about the compiler.
EXIT_OK, EXIT_USAGE, EXIT_SKIP, EXIT_CRASH = 0, 2, 3, 4

if __name__ == "__main__":
    if len(sys.argv) < 4:
        sys.stderr.write("usage: fuzz_metamorphic.py <file.ln> <transform> <seed>\n")
        sys.stderr.write("transforms: %s\n" % " ".join(sorted(TRANSFORMS)))
        sys.exit(EXIT_USAGE)
    import random
    import traceback
    path, which = sys.argv[1], sys.argv[2]
    try:
        seed = int(sys.argv[3])
    except ValueError:
        sys.stderr.write("seed must be an integer\n")
        sys.exit(EXIT_USAGE)
    if which not in TRANSFORMS:
        sys.stderr.write("unknown transform %r\n" % which)
        sys.exit(EXIT_USAGE)
    try:
        src = open(path).read()
    except OSError as e:
        sys.stderr.write("cannot read %s: %s\n" % (path, e))
        sys.exit(EXIT_USAGE)
    try:
        out = TRANSFORMS[which](src, random.Random(seed))
    except Exception:
        traceback.print_exc()
        sys.exit(EXIT_CRASH)
    if out is None or out == src:
        sys.exit(EXIT_SKIP)
    sys.stdout.write(out)
