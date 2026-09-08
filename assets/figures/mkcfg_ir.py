#!/usr/bin/env python3
"""
mkcfg_ir.py — draw the REAL Lain-IR control-flow graph of a function, with the
octagon facts the analyser proved at each block written onto it.

Both inputs are compiler output (`vradrv --dump` and `vradrv --dump-octagon`),
so the figure cannot drift from the engine it describes.

  usage: mkcfg_ir.py <ir-dump.txt> <octagon-dump.txt> <func> <out.dot> [--facts %7,%5]
"""
import re, sys, html

def parse_ir(path, func):
    blocks, order, cur, inside = {}, [], None, False
    for line in open(path):
        if re.match(r'^func\s+' + re.escape(func) + r'\b', line): inside = True; continue
        if inside and line.startswith('}'): break
        if not inside: continue
        m = re.match(r'^(bb\d+):(.*)$', line.strip())
        if m:
            cur = m.group(1); blocks[cur] = {"note": m.group(2).strip(" ;"), "insns": []}
            order.append(cur); continue
        if cur and line.strip():
            blocks[cur]["insns"].append(line.strip())
    return blocks, order

def edges_of(blocks):
    e = []
    for b, d in blocks.items():
        for ins in d["insns"]:
            m = re.match(r'^br_cond\s+\S+,\s*(bb\d+),\s*(bb\d+)', ins)
            if m: e += [(b, m.group(1), "true"), (b, m.group(2), "false")]; continue
            m = re.match(r'^br\s+(bb\d+)', ins)
            if m: e.append((b, m.group(1), ""))
    return e

def parse_octagon(path, func, keep):
    """keep: set of ssa names whose relations we render (else the figure is unreadable)."""
    facts, cur, inside = {}, None, False
    for line in open(path):
        if line.startswith('── octagon state:'):
            inside = func in line; continue
        if not inside: continue
        m = re.match(r'^\s{2}(bb\d+)', line)
        if m: cur = m.group(1); facts[cur] = []; continue
        if cur is None: continue
        for tok in re.findall(r'%\d+\s*−\s*%\d+\s*≤\s*-?\d+', line):
            a, b = re.findall(r'%\d+', tok)
            if keep and not (a in keep and b in keep): continue
            c = re.search(r'≤\s*(-?\d+)', tok).group(1)
            norm = f'{a} − {b} ≤ {c}'
            if norm not in facts[cur]: facts[cur].append(norm)
    return facts

MAX_FACTS = 3

OBLIG = [(re.compile(r'\belem_ptr\b'), "bounds obligation"),
         (re.compile(r'^%\d+ = add\b'), "overflow obligation")]

def rec(t):
    """escape for a graphviz record label"""
    for a, b in (('\\', '\\\\'), ('{', '\\{'), ('}', '\\}'), ('|', '\\|'),
                 ('<', '\\<'), ('>', '\\>'), ('"', '\\"'), (' ', ' ')):
        t = t.replace(a, b)
    return t

def emit(blocks, order, edges, facts, out, func, max_insn):
    """Drawn in LLVM's own `opt -dot-cfg` conventions: record nodes, Courier,
    T/F ports on a conditional branch. Annotations use the IR's comment syntax."""
    L = ['digraph "CFG for \'%s\' function" {' % func,
         '\tlabel="CFG for \'%s\' function";' % func,
         '\tnode [shape=record, fontname="Courier", fontsize=10, '
         'color="#000000", style=filled, fillcolor="#ffffff"];',
         '\tedge [fontname="Courier", fontsize=10, color="#000000"];']
    cond = {}
    for b in order:
        d = blocks[b]
        rows = []
        for ins in d["insns"][:max_insn]:
            note = next((t for rx, t in OBLIG if rx.search(ins)), None)
            rows.append(rec(f'  {ins}' + (f'    ; {note}' if note else '')) + '\\l')
        if len(d["insns"]) > max_insn:
            rows.append(rec('  ...') + '\\l')
        for f in sorted(facts.get(b, []), key=lambda f: (0 if '-1' in f else 1, f))[:MAX_FACTS]:
            rows.append(rec(f'  ; proved: {f}') + '\\l')
        hdr = rec(b + ':' + (('   ; ' + d["note"]) if d["note"] else '')) + '\\l'
        has_cond = any(i.startswith('br_cond') for i in d["insns"])
        cond[b] = has_cond
        ports = '|{<s0>T|<s1>F}' if has_cond else ''
        L.append(f'\t{b} [label="{{{hdr}|{"".join(rows)}{ports}}}"];')
    for a, b, lab in edges:
        port = '' if not cond.get(a) else (':s0' if lab == 'true' else ':s1')
        L.append(f'\t{a}{port} -> {b};')
    L.append('}')
    open(out, 'w').write("\n".join(L) + "\n")

if __name__ == "__main__":
    ir, oct_, func, out = sys.argv[1:5]
    keep = set()
    if "--facts" in sys.argv:
        keep = set(sys.argv[sys.argv.index("--facts") + 1].split(","))
    mi = 8
    if "--max-insn" in sys.argv: mi = int(sys.argv[sys.argv.index("--max-insn") + 1])
    blocks, order = parse_ir(ir, func)
    if not blocks: sys.exit(f"no function {func} in {ir}")
    emit(blocks, order, edges_of(blocks), parse_octagon(oct_, func, keep), out, func, mi)
    print(f"{out}: {len(blocks)} blocks")
