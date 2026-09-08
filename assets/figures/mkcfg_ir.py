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

OBLIG = [(re.compile(r'\belem_ptr\b'), "must be in bounds"),
         (re.compile(r'^%\d+ = add\b'), "must not overflow")]

# GitHub light palette
FG      = "#1f2328"   # default text
MUTED   = "#59636e"   # comments, edges
BORDER  = "#8c959f"   # box borders
SUBTLE  = "#f6f8fa"   # header band
DANGER  = "#cf222e"   # obligations owed
ACCENT  = "#0969da"   # facts proved

def esc(t):
    return t.replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;').replace('"', '&quot;')

def emit(blocks, order, edges, facts, out, func, max_insn):
    L = ['digraph ir {',
         '  graph [bgcolor="transparent", fontname="Helvetica", nodesep=0.36, ranksep=0.42];',
         f'  node  [shape=plaintext, fontname="SFMono-Regular,Menlo,monospace", fontsize=11];',
         f'  edge  [fontname="Helvetica", fontsize=10, color="{MUTED}", '
         f'fontcolor="{MUTED}", arrowsize=0.7];']
    cond = {}
    for b in order:
        d = blocks[b]
        has_cond = any(i.startswith('br_cond') for i in d["insns"])
        cond[b] = has_cond

        note = f'  <font color="{MUTED}">; {esc(d["note"])}</font>' if d["note"] else ''
        rows = [f'<tr><td align="left" bgcolor="{SUBTLE}" colspan="2">'
                f'<font color="{FG}"><b>{b}</b></font>{note}</td></tr>']

        for ins in d["insns"][:max_insn]:
            why = next((t for rx, t in OBLIG if rx.search(ins)), None)
            if why:
                rows.append(f'<tr><td align="left" colspan="2">'
                            f'<font color="{DANGER}">{esc(ins)}</font>'
                            f'<font color="{DANGER}">   ; {why}</font></td></tr>')
            else:
                rows.append(f'<tr><td align="left" colspan="2">'
                            f'<font color="{FG}">{esc(ins)}</font></td></tr>')
        if len(d["insns"]) > max_insn:
            rows.append(f'<tr><td align="left" colspan="2"><font color="{MUTED}">...</font></td></tr>')

        shown = sorted(facts.get(b, []), key=lambda f: (0 if '-1' in f else 1, f))[:MAX_FACTS]
        for i, f in enumerate(shown):
            lead = '; proved  ' if i == 0 else '&#160;' * 10
            rows.append(f'<tr><td align="left" colspan="2">'
                        f'<font color="{MUTED}">{lead}</font>'
                        f'<font color="{ACCENT}">{esc(f)}</font></td></tr>')

        if has_cond:
            rows.append(f'<tr><td port="s0" align="center" bgcolor="{SUBTLE}">'
                        f'<font color="{MUTED}">true</font></td>'
                        f'<td port="s1" align="center" bgcolor="{SUBTLE}">'
                        f'<font color="{MUTED}">false</font></td></tr>')

        L.append(f'  {b} [label=<<table border="2" cellborder="0" cellspacing="0" '
                 f'cellpadding="5" color="{BORDER}" bgcolor="white">{"".join(rows)}</table>>];')

    for a, b, lab in edges:
        port = '' if not cond.get(a) else (':s0' if lab == 'true' else ':s1')
        L.append(f'  {a}{port} -> {b};')
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
