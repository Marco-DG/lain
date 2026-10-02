#!/usr/bin/env python3
"""Apply ONE malformation to a Lain program and print the result.

Why (M13): `var a = [1, 2]` then `return a.zz` SEGFAULTED the compiler, and so did every unknown
member on an array or slice, because E128's message printed the receiver's base name and an array
has none. All 26 fuzzers sat at 0 through it, because every generator emits programs meant to
compile or meant to fail one obligation — never ones with a NAME error. The class is "an error path
no generator walks", and it needs no semantics to test: whatever the input, the compiler must not
crash, and every refusal must carry a code and a position.

Usage:  fuzz_malformed.py <source.ln> <operator> <seed>
Prints the mutated program, and on the first line a comment recording the operator, the seed and
the 1-based LINE the mutation landed on, so the driver can judge WRONG-POSITION without guessing.
Exits 1 if the operator found nothing to mutate in this program (the driver counts that as a skip).
"""
import random, re, sys

def _lines(t): return t.split("\n")

def _pick(rng, idxs):
    return rng.choice(idxs) if idxs else None

# Each operator returns (new_text, line_number) or None if it found no site.
def op_unknown_member(t, rng):
    """Any receiver. Prefers nothing, so most hits are structs."""
    L = _lines(t)
    idxs = [i for i, l in enumerate(L) if re.search(r'\.\s*[a-z_]\w*', l) and not l.strip().startswith('//')]
    i = _pick(rng, idxs)
    if i is None: return None
    L[i] = re.sub(r'\.(\s*)([a-z_]\w*)', lambda m: '.%szz_no_such' % m.group(1), L[i], count=1)
    return "\n".join(L), i + 1

def op_unknown_member_on_seq(t, rng):
    """An unknown member on an ARRAY or SLICE receiver specifically, by rewriting `.len`.

    This operator exists because the generic one above does not reach the case that matters: it
    picks any `.member`, which in this corpus is almost always a struct field, and a struct
    receiver has a clean E128. An array or slice receiver SEGFAULTS the compiler (measured at
    f267f14: `var a = [1, 2]` then `return a.zz` is rc=139, and so is `s.zz` on a slice
    parameter), because the diagnostic printed the receiver's base type name and a sequence has
    none. `.len` is the one member every slice and array has, so rewriting it guarantees the
    receiver is a sequence rather than a struct."""
    L = _lines(t)
    idxs = [i for i, l in enumerate(L) if re.search(r'\.\s*len\b', l) and not l.strip().startswith('//')]
    i = _pick(rng, idxs)
    if i is None: return None
    L[i] = re.sub(r'\.(\s*)len\b', lambda m: '.%szz_no_such' % m.group(1), L[i], count=1)
    return "\n".join(L), i + 1

def op_undeclared_ident(t, rng):
    L = _lines(t)
    idxs = [i for i, l in enumerate(L) if re.search(r'\breturn\s+[a-z_]\w*', l)]
    i = _pick(rng, idxs)
    if i is None: return None
    L[i] = re.sub(r'\breturn\s+([a-z_]\w*)', 'return zz_undeclared', L[i], count=1)
    return "\n".join(L), i + 1

def op_swap_type(t, rng):
    SWAP = {'i32': 'u8[]', 'u8[]': 'i32', 'usize': 'bool', 'bool': 'usize',
            'i64': 'f64', 'f64': 'i64', 'u8': 'i32[4]'}
    L = _lines(t)
    cand = []
    for i, l in enumerate(L):
        if l.strip().startswith('//'): continue
        for k in SWAP:
            if re.search(r'(?<![\w\[])%s(?![\w\]])' % re.escape(k), l): cand.append((i, k))
    pick = _pick(rng, cand)
    if pick is None: return None
    i, k = pick
    L[i] = re.sub(r'(?<![\w\[])%s(?![\w\]])' % re.escape(k), SWAP[k], L[i], count=1)
    return "\n".join(L), i + 1

def op_wrong_qualifier(t, rng):
    L = _lines(t)
    idxs = [i for i, l in enumerate(L) if re.search(r'\b[a-z_]\w*\s*\(', l) and 'func ' not in l
            and not l.strip().startswith('//')]
    i = _pick(rng, idxs)
    if i is None: return None
    L[i] = re.sub(r'\b([a-z_]\w*)\s*\(', lambda m: 'zz_mod.%s(' % m.group(1), L[i], count=1)
    return "\n".join(L), i + 1

def op_unknown_import(t, rng):
    L = _lines(t)
    idxs = [i for i, l in enumerate(L) if l.startswith('import ') and '{' in l]
    i = _pick(rng, idxs)
    if i is None: return None
    L[i] = L[i].replace('{', '{zz_no_such_name, ', 1)
    return "\n".join(L), i + 1

def op_drop_token(t, rng):
    L = _lines(t)
    idxs = [i for i, l in enumerate(L) if len(l.split()) >= 3 and not l.strip().startswith('//')]
    i = _pick(rng, idxs)
    if i is None: return None
    w = L[i].split()
    del w[rng.randrange(len(w))]
    L[i] = " ".join(w)
    return "\n".join(L), i + 1

def op_dup_token(t, rng):
    L = _lines(t)
    idxs = [i for i, l in enumerate(L) if len(l.split()) >= 2 and not l.strip().startswith('//')]
    i = _pick(rng, idxs)
    if i is None: return None
    w = L[i].split(); j = rng.randrange(len(w))
    w.insert(j, w[j])
    L[i] = " ".join(w)
    return "\n".join(L), i + 1

def op_call_to_member(t, rng):
    L = _lines(t)
    idxs = [i for i, l in enumerate(L) if re.search(r'\b[a-z_]\w*\s*\([^)]*\)', l) and 'func ' not in l
            and not l.strip().startswith('//')]
    i = _pick(rng, idxs)
    if i is None: return None
    L[i] = re.sub(r'\b([a-z_]\w*)\s*\([^)]*\)', lambda m: '%s.zz_field' % m.group(1), L[i], count=1)
    return "\n".join(L), i + 1

def op_member_to_call(t, rng):
    L = _lines(t)
    idxs = [i for i, l in enumerate(L) if re.search(r'\.\s*[a-z_]\w*(?!\s*\()', l)
            and not l.strip().startswith('//')]
    i = _pick(rng, idxs)
    if i is None: return None
    L[i] = re.sub(r'\.(\s*)([a-z_]\w*)', lambda m: '.%s%s()' % (m.group(1), m.group(2)), L[i], count=1)
    return "\n".join(L), i + 1

def _delimited(t):
    """Every string literal, character literal and block comment in t, as (start, end, kind), with
    end one past the closing delimiter. A small scan with the lexer's own rules: a backslash skips
    the next byte inside a literal, a line comment runs to the end of its line, block comments
    nest. A token the text itself leaves open is not a site (the program would be refused already)."""
    out, i, n = [], 0, len(t)
    while i < n:
        c = t[i]
        if c == '/' and t[i+1:i+2] == '/':
            while i < n and t[i] not in '\r\n': i += 1
        elif c == '/' and t[i+1:i+2] == '*':
            j, depth = i + 2, 1
            while j < n and depth:
                if t[j:j+2] == '/*': depth += 1; j += 2
                elif t[j:j+2] == '*/': depth -= 1; j += 2
                else: j += 1
            if depth: return out
            out.append((i, j, 'comment')); i = j
        elif c in '"\'':
            j = i + 1
            while j < n and t[j] != c:
                j += 2 if t[j] == '\\' else 1
            if j >= n: return out
            out.append((i, j + 1, 'string' if c == '"' else 'char')); i = j + 1
        else:
            i += 1
    return out

def op_truncate(t, rng):
    """The text ENDS inside a string literal, a character literal or a block comment (I.101).

    The lexer had no case for the end of the text inside a literal, and read on past the source's
    NUL until some later byte was a quote. This fuzzer sat at 0 through it, because no operator
    ever ended a program early: every mutant was a whole file. And at -O2 the overread usually
    printed a CODED E100 ("unknown escape sequence") at the right line, which passes every check
    above, so the driver runs this operator against an ASan build of lain (the source has its own
    allocation, so a read past it is visible). The cut lands anywhere from just after the opening
    delimiter to just before the closing one, so right after a backslash too. The refusal belongs
    at the token's first line."""
    sites = _delimited(t)
    if not sites: return None
    start, end, _ = rng.choice(sites)
    cut = rng.randrange(start + 1, end)       # past the opener, before the closer's last byte
    return t[:cut], t.count("\n", 0, start) + 1

def op_truncate_any(t, rng):
    """The text ends at ANY byte, inside a token or between two. Where the refusal belongs depends
    on what was cut (an unclosed block, a missing operand), so the driver does not judge its
    position; it judges that nothing crashes, ASan sees nothing, and a refusal is coded."""
    if len(t) < 2: return None
    cut = rng.randrange(1, len(t))
    return t[:cut], t.count("\n", 0, cut) + 1

OPS = {
    "truncate":          op_truncate,
    "truncate-any":      op_truncate_any,
    "unknown-member":    op_unknown_member,
    "unknown-member-seq": op_unknown_member_on_seq,
    "undeclared-ident":  op_undeclared_ident,
    "swap-type":         op_swap_type,
    "wrong-qualifier":   op_wrong_qualifier,
    "unknown-import":    op_unknown_import,
    "drop-token":        op_drop_token,
    "dup-token":         op_dup_token,
    "call-to-member":    op_call_to_member,
    "member-to-call":    op_member_to_call,
}

if __name__ == "__main__":
    if len(sys.argv) < 4:
        sys.stderr.write("usage: fuzz_malformed.py <source.ln> <operator> <seed>\n")
        sys.stderr.write("operators: %s\n" % " ".join(sorted(OPS)))
        sys.exit(2)
    src, opname, seed = sys.argv[1], sys.argv[2], int(sys.argv[3])
    if opname not in OPS: sys.stderr.write("unknown operator %s\n" % opname); sys.exit(2)
    text = open(src, errors="replace").read()
    got = OPS[opname](text, random.Random(seed))
    if got is None: sys.exit(1)            # no site for this operator: the driver counts a skip
    out, line = got
    sys.stdout.write("// MUTANT op=%s seed=%d line=%d src=%s\n%s" % (opname, seed, line, src, out))
