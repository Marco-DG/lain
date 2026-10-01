#!/usr/bin/env python3
"""Generate one Lain program that LAUNDERS a reference to a local through some construct.

Why (M9, 2026-10-01): the escape check (borrow.h, bor_roots_local) follows ONE provenance chain
and answers "not local" for anything it cannot follow. It therefore holds for a direct return and
for a plain binding, and loses the reference the moment it passes through a CALL or a struct
FIELD. Measured at 3b58598, nine launderings compile and dangle while six are correctly refused.

Each REJECT program has the same skeleton, so only the laundering differs:

    func leak() i32[] { var xs i32[4] = [11,22,33,44]   <launder and return a reference into xs> }
    func clobber(n i32) i32 { ... 16-element local array, summed ... }
    func main() { s = leak()   c = clobber(7)   read s[0] }

The clobber call is load-bearing: it reuses the dead frame, so a surviving reference reads
someone else's data rather than the stale-but-intact value, which is what makes the three oracles
fire. Verified on the identity-call shape at 3b58598: gcc -O2 prints 1579282520 for an expected
11, ASan says stack-use-after-return, and --interpret says "use of storage that is no longer
live".

TWO-SIDED, and the first line declares the side:
    // EXPECT: reject   a reference into a LOCAL. Acceptance is a dangling return, counted only
                        when CONFIRMED by ASan, the interpreter, or a C/interpreter difference.
    // EXPECT: prove    the same shapes rooted in something that OUTLIVES the call (a parameter,
                        a string literal, a module constant) or a COPY, which is not a borrow at
                        all. A refusal is a WRONG-REJECT: a fail-closed escape check must not buy
                        its soundness by refusing these.
Note for the prove side: TWO of its shapes are already refused at 3b58598 (slicing a module
constant table, and copying a fixed array into a fixed-array field). They are pre-existing
over-refusals, not regressions, so the wrong-reject count does not start at zero.
"""
import random, sys

CLOB = ('func clobber(n i32) i32 {\n'
        '    var junk i32[16] = [9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9]\n'
        '    var acc i32 = 0\n'
        '    var i usize = 0\n'
        '    while i < 16 {\n'
        '        acc = acc +% junk[i] +% n\n'
        '        i = i + 1\n'
        '    }\n'
        '    return acc\n'
        '}\n')
LOCAL = '    var xs i32[4] = [11, 22, 33, 44]\n'

# ── REJECT shapes: a reference into a LOCAL, laundered. (hole) = accepted at 3b58598. ─────────
def r_direct(_):        return '', 'i32[]', LOCAL + '    return xs[0..2]\n', ''
def r_immutable(_):     return '', 'i32[]', LOCAL + '    s = xs[0..2]\n    return s\n', ''
def r_var(_):           return '', 'i32[]', LOCAL + '    var s i32[] = xs[0..2]\n    return s\n', ''
def r_pointer(_):       return '', '*var i32', LOCAL + '    return &xs[1]\n', ''
def r_subslice(_):      return '', 'i32[]', LOCAL + '    a = xs[0..3]\n    return a[0..2]\n', ''
def r_struct_ctor(_):   return 'type Box { s i32[] }\n', 'Box', LOCAL + '    return Box(xs[0..2])\n', ''
def r_var_branch(_):    # hole
    return '', 'i32[]', LOCAL + ('    var s i32[] = xs[0..1]\n'
                                 '    if xs[0] == 11 { s = xs[0..2] }\n    return s\n'), ''
def r_field_store(_):   # hole
    return 'type Box { s i32[] }\n', 'Box', LOCAL + ('    var b Box = Box(xs[0..1])\n'
                                                     '    b.s = xs[0..2]\n    return b\n'), ''
def r_nested(_):        # hole
    return 'type Inner { s i32[] }\ntype Outer { i Inner }\n', 'Outer', \
           LOCAL + '    return Outer(Inner(xs[0..2]))\n', ''
def r_sum_payload(_):   # hole
    return 'type Maybe { None, Some { s i32[] } }\n', 'Maybe', \
           LOCAL + '    return Maybe.Some(xs[0..2])\n', ''
def r_identity(_):      # hole
    return 'func id(s i32[]) i32[] { return s }\n', 'i32[]', LOCAL + '    return id(xs[0..2])\n', ''
def r_two_step(_):      # hole
    return ('func id(s i32[]) i32[] { return s }\n'
            'func id2(s i32[]) i32[] { return id(s) }\n'), 'i32[]', \
           LOCAL + '    return id2(xs[0..2])\n', ''
def r_second_arg(_):    # hole
    return 'func pick(s i32[], t i32[]) i32[] { return t }\n', 'i32[]', \
           LOCAL + '    return pick(xs[2..3], xs[0..2])\n', ''
def r_field_read(_):    # hole
    return 'type Box { s i32[] }\n', 'i32[]', \
           LOCAL + '    var b Box = Box(xs[0..2])\n    return b.s\n', ''
def r_field_read_call(_):  # hole
    return ('type Box { s i32[] }\n'
            'func unwrap(b Box) i32[] { return b.s }\n'), 'i32[]', \
           LOCAL + '    return unwrap(Box(xs[0..2]))\n', ''

REJECT = [r_direct, r_immutable, r_var, r_pointer, r_subslice, r_struct_ctor, r_var_branch,
          r_field_store, r_nested, r_sum_payload, r_identity, r_two_step, r_second_arg,
          r_field_read, r_field_read_call]

# ── PROVE shapes: rooted in something that outlives the call, or a COPY. ──────────────────────
def p_param_identity(_):
    return 'func id(s i32[]) i32[] { return s }\n', 'i32[]', \
           '    if p.len < 2 { return p }\n    return id(p[0..2])\n', 'p i32[]'
def p_param_field(_):
    return 'type Box { s i32[] }\n', 'Box', \
           ('    if p.len < 2 { return Box(p) }\n'
            '    var b Box = Box(p[0..1])\n    b.s = p[0..2]\n    return b\n'), 'p i32[]'
def p_param_nested(_):
    return 'type Inner { s i32[] }\ntype Outer { i Inner }\n', 'Outer', \
           '    if p.len < 2 { return Outer(Inner(p)) }\n    return Outer(Inner(p[0..2]))\n', 'p i32[]'
def p_string_literal(_):
    return '', 'u8[]', '    return "hello"\n', ''
def p_module_table(_):
    return 'TBL i32[4] = [1, 2, 3, 4]\n', 'i32[]', '    return TBL[0..2]\n', ''
def p_copy_array(_):
    return 'type Arr { a i32[4] }\n', 'Arr', LOCAL + '    return Arr(xs)\n', ''
def p_copy_element(_):
    return '', 'i32', LOCAL + '    return xs[1]\n', ''

PROVE = [p_param_identity, p_param_field, p_param_nested, p_string_literal, p_module_table,
         p_copy_array, p_copy_element]


def gen(rng):
    reject = rng.random() < 0.5
    shape = rng.choice(REJECT if reject else PROVE)
    decls, rty, body, params = shape(rng)
    name = shape.__name__
    L = ['// EXPECT: %s' % ('reject' if reject else 'prove'),
         '// shape: %s' % name,
         'extern func libc_printf(fmt *u8, ...) i32 effects io']
    if decls: L.append(decls.rstrip('\n'))
    L.append('func leak(%s) %s {' % (params, rty))
    L.append(body.rstrip('\n'))
    L.append('}')
    L.append(CLOB.rstrip('\n'))
    L.append('func main() i32 effects io {')
    if params:
        L.append('    var src i32[4] = [11, 22, 33, 44]')
        L.append('    r = leak(src)')
    else:
        L.append('    r = leak()')
    L.append('    c = clobber(7)')
    # Read THROUGH the returned thing, after the frame has been reused.
    if rty == 'i32[]':
        L.append('    if r.len > 0 {')
        L.append('        libc_printf("%d %d\\n", r[0], c)')
        L.append('    }')
    elif rty == 'u8[]':
        L.append('    if r.len > 0 {')
        L.append('        libc_printf("%d %d\\n", r[0] as i32, c)')
        L.append('    }')
    elif rty == '*var i32':
        L.append('    unsafe {')
        L.append('        libc_printf("%d %d\\n", *r, c)')
        L.append('    }')
    elif rty == 'i32':
        L.append('    libc_printf("%d %d\\n", r, c)')
    elif rty == 'Arr':
        L.append('    libc_printf("%d %d\\n", r.a[1], c)')
    elif rty == 'Maybe':
        L.append('    case r {')
        L.append('        Some(s): if s.len > 0 { libc_printf("%d %d\\n", s[0], c) }')
        L.append('        else: libc_printf("none %d\\n", c)')
        L.append('    }')
    elif rty == 'Outer':
        L.append('    inner = r.i.s')
        L.append('    if inner.len > 0 {')
        L.append('        libc_printf("%d %d\\n", inner[0], c)')
        L.append('    }')
    else:  # Box
        L.append('    if r.s.len > 0 {')
        L.append('        libc_printf("%d %d\\n", r.s[0], c)')
        L.append('    }')
    L.append('    return 0')
    L.append('}')
    return '\n'.join(L) + '\n'


if __name__ == '__main__':
    sys.stdout.write(gen(random.Random(int(sys.argv[1]) if len(sys.argv) > 1 else 0)))
