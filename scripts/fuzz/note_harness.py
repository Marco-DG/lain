#!/usr/bin/env python3
"""note_harness.py PROG NOTE OUT — fuzz_termination's PE.12 gate: PROG rebuilt with `main` making the
call an E011 note names, written to OUT, so that the native program can be run on that input. Prints
what the native run must do: `loop` (not end) or `rec` (not return: time out or exhaust the stack).
Prints `skip` when the note names no call the harness can make:
- a value it assumes of an extern (` if nondet() returns 0 every time`): no native run can promise it;
- a function with no parameters, which the note names by no call;
- an argument said zeroed whose type is not a struct of integer fields.
An argument said zeroed is made as the interpreter made it: `var c = C(0, 0)`, passed as the
parameter is declared (`var c` for a `var` parameter)."""
import re, sys

prog, note, out = sys.argv[1:4]


def skip():
    print("skip")
    sys.exit(0)


m = re.search(r"this (loop never ends|recursion never returns) for ([A-Za-z_]\w*)\(([^()]*)\)(.*)$", note)
if not m:
    skip()
kind, fn, args, tail = m.groups()
zeroed = []
z = re.match(r", with (.+?) zeroed", tail)
if z:
    zeroed = re.split(r", | and ", z.group(1))
    tail = tail[z.end():]
if not tail.startswith(":"):
    skip()
src = open(prog).read()
sig = re.search(r"func %s\(([^)]*)\)" % re.escape(fn), src)
if not sig:
    skip()
params = [p.strip() for p in sig.group(1).split(",") if p.strip()]
vals = [a.strip() for a in args.split(",")]
if len(params) != len(vals):
    skip()
pre, call = [], []
for p, v in zip(params, vals):
    if re.fullmatch(r"-?\d+|true|false", v):
        call.append(v)
        continue
    pm = re.fullmatch(r"(\w+)\s+(var\s+)?(\w+)", p)
    if v not in zeroed or not pm or pm.group(1) != v:
        skip()
    ty = re.search(r"type %s \{([^}]*)\}" % re.escape(pm.group(3)), src)
    fields = [f.strip() for f in re.split(r"[\n,]", ty.group(1)) if f.strip()] if ty else []
    if not fields or not all(re.fullmatch(r"\w+\s+[iu]\d+", f) for f in fields):
        skip()
    pre.append("    var %s = %s(%s)\n" % (v, pm.group(3), ", ".join("0" for _ in fields)))
    call.append(("var " if pm.group(2) else "") + v)
i = src.find("func main(")
if i < 0:
    skip()
with open(out, "w") as f:
    f.write(src[:i] + "func main() i32 effects io, raises, alloc, diverge {\n%s    %s(%s)\n    return 0\n}\n"
            % ("".join(pre), fn, ", ".join(call)))
print("loop" if kind.startswith("loop") else "rec")
