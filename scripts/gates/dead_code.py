#!/usr/bin/env python3
# dead_code.py ROOT: the static functions under ROOT/src that no entry point reaches, one per line as
# `file:line: name`, and the counts on stderr. Used by dead_code_gate.sh.
#
# gcc's -Wunused-function sees a static function nothing names. It cannot see a CLUSTER whose
# functions only name each other: W130's visitor (three functions, ~130 lines) survived the
# removal of the warning it served that way, and so did the front end's mutual-recursion walker
# (six functions) once the IR engine took the obligation over.
#
# The call graph is built from NAMES: a function body naming another function is an edge; a name
# at file scope (an initialiser, a function-pointer table, a macro body) is a root; every
# non-static function and every `main` is a root. A static function the roots do not reach is
# reported. Two functions with one name are one node, so a collision can hide a dead function but
# never report a live one.
#
# The text is stripped of comments and literals in ONE left-to-right pass, as a C lexer reads it.
# The first version removed block comments before line comments, so a `/*` inside a `//` comment
# swallowed real code up to the next `*/`, and the functions it called were reported dead.
import os, re, sys
root = sys.argv[1]
files = []
for d, _, fs in os.walk(os.path.join(root, 'src')):
    for f in fs:
        if f.endswith(('.h', '.c')): files.append(os.path.join(d, f))
ident = re.compile(r'[A-Za-z_][A-Za-z0-9_]*')
defn = re.compile(r'^(static\s+)?(?:inline\s+)?(?:const\s+)?(?:unsigned\s+|signed\s+|struct\s+|enum\s+)?[A-Za-z_][A-Za-z0-9_]*[\s\*]+([A-Za-z_][A-Za-z0-9_]*)\s*\(')
funcs = {}          # name -> (file, line, static)
bodies = {}         # name -> set of identifiers in its body
filescope = set()   # identifiers named outside any function body
def strip(text):
    # one left-to-right pass, as a C lexer reads: a `/*` inside a `//` comment or a string is not a
    # comment, and removing block comments first let one swallow real code up to the next `*/`
    out = []; i = 0; n = len(text)
    while i < n:
        c = text[i]
        if c == '/' and i + 1 < n and text[i+1] == '/':
            j = text.find('\n', i); i = n if j < 0 else j; continue
        if c == '/' and i + 1 < n and text[i+1] == '*':
            j = text.find('*/', i + 2); j = n if j < 0 else j + 2
            out.append('\n' * text.count('\n', i, j)); i = j; continue
        if c == '"' or c == "'":
            j = i + 1
            while j < n and text[j] != c and text[j] != '\n':
                j += 2 if text[j] == '\\' else 1
            out.append(c + c); i = j + 1; continue
        out.append(c); i += 1
    return ''.join(out)
for path in files:
    text = strip(open(path, errors='replace').read())
    lines = text.split('\n')
    depth = 0; cur = None; pending = None
    for no, line in enumerate(lines, 1):
        if depth == 0:
            m = defn.match(line)
            if m and not line.rstrip().endswith(';'):
                pending = (m.group(2), bool(m.group(1)), no)
        ids = ident.findall(line)
        opens = line.count('{'); closes = line.count('}')
        if depth == 0 and pending and opens:
            name, st, ln = pending
            cur = name; funcs.setdefault(name, (path, ln, st)); bodies.setdefault(name, set())
            pending = None
        if cur: bodies[cur].update(ids)
        elif depth == 0 and not (pending and not opens) and not (defn.match(line) and line.rstrip().endswith(';')):
            filescope.update(ids)
        depth += opens - closes
        if depth <= 0:
            depth = 0
            if cur and closes: cur = None
            if pending and line.rstrip().endswith(';'): pending = None
roots = {n for n, (p, l, st) in funcs.items() if not st or n == 'main'} | (filescope & set(funcs))
seen = set(); stack = list(roots)
while stack:
    n = stack.pop()
    if n in seen: continue
    seen.add(n)
    for m in bodies.get(n, ()):
        if m in funcs and m not in seen: stack.append(m)
dead = sorted((funcs[n][0], funcs[n][1], n) for n in funcs if n not in seen and funcs[n][2])
for p, l, n in dead: print(f"{os.path.relpath(p, root)}:{l}: {n}")
print(f"static functions: {sum(1 for f in funcs.values() if f[2])}, unreached: {len(dead)}", file=sys.stderr)
