#!/usr/bin/env python3
# judge_includes.py ROOT: the include rules between the proof engine's trusted and untrusted sides.
# Run by run_ir_tests.sh; exit 0 when every rule holds, 1 with one line per violation.
#
# The judge (src/judge/: generator, kernel, the text form) is the TRUSTED side; the solver
# (src/solver/) and the finder (src/analysis/) search and are not trusted. They meet at one file:
#   1. nothing src/judge/ includes, directly or through other headers, is under src/analysis/ or
#      src/solver/: a judge that shares code with what it checks is blind to that code's bugs;
#   2. of src/judge/, src/solver/ includes, directly or not, only judge/format.h;
#   3. judge/format.h is plain data: it includes only <stdint.h> and <stdbool.h>, and defines no
#      function, so the solver receives no arithmetic of the judge's;
#   4. nothing under src/judge/ or src/solver/ names the process's streams or ends the process
#      (judge_interfaces §8, G5): a library reports through its records, an `err` buffer, a flag,
#      or a FILE * its caller passes, so a compiler that links it decides what reaches the user.
# Includes are followed TRANSITIVELY, since a rule checked one level deep holds only until a header
# in the middle gains an include. A quoted include that resolves to no file is a violation: the
# check cannot vouch for what it cannot read.
import os, re, sys
root = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else '.')
src = os.path.join(root, 'src')
inc = re.compile(r'^[ \t]*#[ \t]*include[ \t]*(?:<([^>\n]+)>|"([^"\n]+)")', re.M)
bad = []

def strip_comments(text):
    # one left-to-right pass, as a C lexer reads (dead_code.py's lesson): `"../compiler//msvc.h"`
    # holds a `//` that is no comment, and a quote inside a comment opens no string
    out = []; i = 0; n = len(text)
    while i < n:
        c = text[i]
        if c == '/' and text.startswith('//', i):
            j = text.find('\n', i); i = n if j < 0 else j; continue
        if c == '/' and text.startswith('/*', i):
            j = text.find('*/', i + 2); j = n if j < 0 else j + 2
            out.append('\n' * text.count('\n', i, j)); i = j; continue
        if c in '"\'':
            j = i + 1
            while j < n and text[j] != c and text[j] != '\n': j += 2 if text[j] == '\\' else 1
            out.append(text[i:j + 1]); i = j + 1; continue
        out.append(c); i += 1
    return ''.join(out)

def includes(path):
    # a commented-out include is not an include
    text = strip_comments(open(path, encoding='utf-8', errors='replace').read())
    return [('<', a) if a else ('"', b) for a, b in inc.findall(text)]

def resolve(name, frm):
    for base in (src, os.path.dirname(frm)):
        p = os.path.normpath(os.path.join(base, name))
        if os.path.isfile(p): return p
    return None

def closure(start):
    """Every file under src/ that `start` includes, directly or not, with one includer each."""
    seen, todo = {}, [start]
    while todo:
        f = todo.pop()
        for kind, name in includes(f):
            if kind == '<': continue
            p = resolve(name, f)
            if p is None:
                bad.append(f'{os.path.relpath(f, root)}: #include "{name}" names no file')
                continue
            if p not in seen:
                seen[p] = f; todo.append(p)
    return seen

def under(p, d): return os.path.commonpath([p, os.path.join(src, d)]) == os.path.join(src, d)
def files(d):
    out = []
    for dp, _, fs in os.walk(os.path.join(src, d)):
        out += [os.path.join(dp, f) for f in sorted(fs) if f.endswith(('.h', '.c'))]
    return sorted(out)
rel = lambda p: os.path.relpath(p, root)

judge, solver = files('judge'), files('solver')
fmt = os.path.join(src, 'judge', 'format.h')
for f in judge:
    for p, by in closure(f).items():
        if under(p, 'analysis') or under(p, 'solver'):
            bad.append(f'{rel(f)} reaches {rel(p)} (included by {rel(by)}): the judge includes nothing from src/analysis/ or src/solver/')
for f in solver:
    for p, by in closure(f).items():
        if under(p, 'judge') and p != fmt:
            bad.append(f'{rel(f)} reaches {rel(p)} (included by {rel(by)}): the solver includes only judge/format.h of the judge')
if os.path.isfile(fmt):
    allowed = {('<', 'stdint.h'), ('<', 'stdbool.h')}
    for kind, name in includes(fmt):
        if (kind, name) not in allowed:
            bad.append(f'src/judge/format.h includes {kind}{name}: it includes only <stdint.h> and <stdbool.h>')
    text = strip_comments(open(fmt).read())
    # a function definition: a parameter list's `)` followed by a body's `{`, or a static/inline
    for m in re.finditer(r'\)\s*\{|\bstatic\b|\binline\b|#\s*define\s+\w+\(', text):
        line = text.count('\n', 0, m.start()) + 1
        bad.append(f'src/judge/format.h:{line}: `{m.group(0).strip()}`: format.h defines no function or macro function, only data')
elif judge:
    bad.append('src/judge/format.h is missing: it is the one file the solver may include')

# rule 4, on the code with comments and string literals blanked: `fprintf(o, ...)` to a caller's
# FILE * and `snprintf` into a caller's buffer stay allowed
proc = re.compile(r'\b(stderr|stdout|stdin)\b|\b(exit|_Exit|quick_exit|abort|printf|puts|putchar|perror|getchar)\s*\(')
for f in judge + solver:
    text = re.sub(r'"(?:\\.|[^"\\\n])*"', '""', strip_comments(open(f, encoding='utf-8', errors='replace').read()))
    for m in proc.finditer(text):
        line = text.count('\n', 0, m.start()) + 1
        bad.append(f'{rel(f)}:{line}: `{m.group(0).rstrip("(").strip()}`: the proof engine writes to no process stream and never ends the process')

for b in bad: print('  ' + b)
print(f'judge include rules: {len(judge)} judge files, {len(solver)} solver files, '
      + ('all rules hold' if not bad else f'{len(bad)} violation(s)'))
sys.exit(1 if bad else 0)
