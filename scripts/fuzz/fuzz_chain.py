#!/usr/bin/env python3
"""Generate one Lain program that CHAINS an operation's result into a later obligation.

Why this generator exists (M7, 2026-10-01): the per-operation soundness harness found four
false proofs that all 24 whole-program fuzzers had passed. Three of the four sat in cells the
generators DID produce (a UDIV with a non-constant divisor whose range contains 1, a SREM with
a possibly-negative divisor, a same-width u32 -> i32 cast). They survived because every
generator computes an operation and checks for UB AT THAT OPERATION, so the oracle cannot see
a wrong FACT about a result that nothing consumes. fuzz_div, for instance, emits
`return x % d` and only accumulates and prints it.

So every program here does three things the others do not:
  1. the result of the operation FEEDS AN OBLIGATION that a wrong fact would break — an array
     index, a divisor, a narrowing cast, a loop bound, or a slice end;
  2. that obligation comes AFTER the operation IN THE SAME FUNCTION, so a state the analysis
     wrongly emptied is observable rather than vacuous;
  3. the program is called with a WITNESS input that reaches the dangerous value, so a wrong
     proof crashes rather than merely being possible.

Two-sided, and the first line of each program says which side it is on:
    // EXPECT: prove    — safe by construction. A rejection is a WRONG-REJECT (a cost, not a
                          bug): the analysis lost a fact it needed.
    // EXPECT: reject    — genuinely unsafe at the witness input. ACCEPTANCE IS A FALSE PROOF,
                          confirmed by the crash under ASan/UBSan or by `--interpret` saying
                          PROOF FAILED.
A one-sided version of this generator would be useless: a permissive bug hides on the `prove`
side and a conservative one on the `reject` side.
"""
import random, sys

PRELUDE = 'extern func libc_printf(fmt *u8, ...) i32 effects io\n'
T4  = 'T4 u8[4] = [0, 1, 2, 3]\n'
T8  = 'T8 u8[8] = [0, 1, 2, 3, 4, 5, 6, 7]\n'
T33 = 'T33 u8[33] = [' + ', '.join(str(i % 7) for i in range(33)) + ']\n'


def udiv_divisor(rng, bad):
    """The quotient becomes a DIVISOR: a wrong LOWER bound on it divides by zero.
    This is the shape of the UDIV bug (lower bound computed as if the divisor were 1)."""
    if bad:
        # x in [1,4], d in [1,100] -> the quotient CAN be 0 (x=1, d=100), so `100 / q` is unsafe.
        body = ('    if x < 1 { return 0 }\n'
                '    if x > 4 { return 0 }\n'
                '    if d < 1 { return 0 }\n'
                '    if d > 100 { return 0 }\n'
                '    q = x / d\n'
                '    return 100 / q\n')
        witness = '1, 100'
    else:
        # x in [100,200], d in [1,4] -> the quotient is at least 25, so `100 / q` is safe.
        body = ('    if x < 100 { return 0 }\n'
                '    if x > 200 { return 0 }\n'
                '    if d < 1 { return 0 }\n'
                '    if d > 4 { return 0 }\n'
                '    q = x / d\n'
                '    return 100 / q\n')
        witness = '100, 4'
    return 'func chain(x u32, d u32) u32 {\n' + body + '}\n', 'u32', witness, ''


def srem_then_index(rng, bad):
    """A remainder by a possibly-negative divisor, FOLLOWED BY an index obligation.
    If the remainder empties the abstract state, everything after it is vacuously proven."""
    pre = ('    if d == 0 { return 0 }\n'
           '    if d < 0 - 8 { return 0 }\n'
           '    if d > 8 { return 0 }\n'
           '    r = x % d\n')
    if bad:
        # After the remainder, index with a value nothing bounds: unprovable, so it must be
        # refused. Accepting it means the state went empty at the remainder.
        body = pre + ('    if x < 0 { return 0 }\n'
                      '    return T4[x as usize]\n')
        witness = '9999, 0 - 3'
    else:
        body = pre + ('    m = r & 3\n'
                      '    if m < 0 { return 0 }\n'
                      '    return T4[m as usize]\n')
        witness = '9999, 0 - 3'
    return 'func chain(x i32, d i32) u8 {\n' + body + '}\n', 'u8', witness, T4


def cast_sign_index(rng, bad):
    """A SAME-WIDTH signedness cast feeding an OVERFLOW obligation. If u32 -> i32 is treated as
    value-preserving, then after `if s > 3` the analysis believes s is in [0, 3] and proves the
    subtraction in range, while 0x80000000 really casts to -2147483648 and it underflows.
    The obligation is an overflow rather than an index on purpose: an index would need a second
    cast to usize, and THAT cast's own obligation catches the program first, hiding the bug."""
    if bad:
        body = ('    s = x as i32\n'
                '    if s > 3 { return 0 }\n'
                '    return s - 2147483647\n')
        witness = '2147483648'
    else:
        body = ('    m = x & 3\n'
                '    s = m as i32\n'
                '    if s > 3 { return 0 }\n'
                '    return s - 2147483647\n')
        witness = '2147483648'
    return 'func chain(x u32) i32 {\n' + body + '}\n', 'i32', witness, ''


def bitcount_index(rng, bad):
    """A bit intrinsic's result indexes a table sized width+1. Half the programs guard the
    operand with `!= 0`, so the zero-operand obligation is exercised BOTH ways."""
    # @popcount(0) is 0 and perfectly defined, so it has NO zero obligation and can never be
    # the unsafe side. Only @ctz and @clz are undefined at zero.
    op = rng.choice(['@ctz', '@clz']) if bad else rng.choice(['@ctz', '@clz', '@popcount'])
    if bad:
        body = ('    n = %s(x)\n'
                '    return T33[n as usize]\n') % op          # x may be 0
        witness = '0'
    else:
        body = ('    if x == 0 { return 0 }\n'
                '    n = %s(x)\n'
                '    return T33[n as usize]\n') % op
        witness = '0'
    return 'func chain(x u32) u8 {\n' + body + '}\n', 'u8', witness, T33


def or_xor_index(rng, bad):
    """A bitwise result indexes a table. `x & 7` is bounded by 7; `x | 7` is not bounded above."""
    if bad:
        opline = '    y = x | 7\n' if rng.random() < 0.5 else '    y = x ^ 7\n'
        witness = '4294967295'
    else:
        opline = '    y = x & 7\n'
        witness = '4294967295'
    body = opline + '    return T8[y as usize]\n'
    return 'func chain(x u32) u8 {\n' + body + '}\n', 'u8', witness, T8


def narrow_cast(rng, bad):
    """A masked result is narrowed. The mask decides whether the narrowing is provable."""
    mask = '511' if bad else '255'
    body = ('    y = x & %s\n'
            '    z = y as u8\n'
            '    return z\n') % mask
    return 'func chain(x u32) u8 {\n' + body + '}\n', 'u8', '4294967295', ''


def loop_bound(rng, bad):
    """The result becomes a LOOP BOUND, with an index inside the loop."""
    lim = '9' if bad else '4'
    body = ('    k = x & %s\n'
            '    var acc u8 = 0\n'
            '    var i usize = 0\n'
            '    while i < k as usize {\n'
            '        acc = acc +%% T4[i]\n'
            '        i = i + 1\n'
            '    }\n'
            '    return acc\n') % lim
    return 'func chain(x u32) u8 {\n' + body + '}\n', 'u8', '4294967295', T4


def slice_end(rng, bad):
    """The result becomes a slice END. `a[0..k]` needs k <= a.len."""
    if bad:
        body = ('    k = x & 15\n'
                '    b = a[0 .. k as usize]\n'
                '    if b.len == 0 { return 0 }\n'
                '    return b[0]\n')
    else:
        body = ('    k = x & 15\n'
                '    if k as usize > a.len { return 0 }\n'
                '    b = a[0 .. k as usize]\n'
                '    if b.len == 0 { return 0 }\n'
                '    return b[0]\n')
    return ('func chain(a u8[], x u32) u8 {\n' + body + '}\n', 'u8',
            'SRC, 4294967295', 'SRC u8[4] = [9, 8, 7, 6]\n')



def srem_refined_dividend(rng, bad):
    """A remainder whose DIVIDEND is refined >= 0 and whose divisor may be NEGATIVE.
    vra.h's SREM takes a special branch only when the dividend's lower bound is known >= 0; with
    a possibly-negative dividend it falls to the general branch, which is correct. So the refined
    dividend is what reaches the buggy branch, and the state after the remainder goes EMPTY,
    proving whatever follows. The obligation after it is an overflow, so the witness is visible
    as an interpreter/C DIFFERENCE (the emitted C wraps and has no UB for UBSan to see)."""
    if bad:
        fn = ('func chain(x i16 >= 0 and <= 39, y i16 >= 0 - 15436 and <= 0 - 15436) i16 {\n'
              '    return (x % y) + 32767\n}\n')
        witness = '7, 0 - 15436'
    else:
        # The same shape with a dividend that is NOT refined >= 0: the general branch applies.
        fn = ('func chain(x i16, y i16 >= 0 - 15436 and <= 0 - 15436) i16 {\n'
              '    r = x % y\n'
              '    return r & 31\n}\n')
        witness = '7, 0 - 15436'
    return fn, 'i16', witness, ''


def cast_wrapping(rng, bad):
    """The WRAPPING cast `as%` owes no obligation, which is the only tier where a wrong
    same-width cast fact survives: a plain `as` raises its own overflow obligation and the
    program is refused before the fact matters (measured at HEAD). vra_range follows `as%` back
    to the u32 and believes [0, 2^31), so a subtraction that really underflows is 'proven'."""
    if bad:
        fn = ('func chain(x u32 >= 4294967291) i32 {\n'
              '    return (x as% i32) - 2147483646\n}\n')
        witness = '4294967291'
    else:
        fn = ('func chain(x u32 <= 100) i32 {\n'
              '    return (x as% i32) - 2147483646\n}\n')
        witness = '100'
    return fn, 'i32', witness, ''


def precond_const(rng, bad):
    """A precondition that names a MODULE CONSTANT is assumed by the callee and was never
    asserted at the caller. Parameters, literals and `p.len` were enforced; constants were not."""
    arg = '50' if bad else '5'
    fn = ('func chain(n u32 < K10) u8 {\n'
          '    return T10[n as usize]\n}\n')
    return fn, 'u8', arg, 'K10 u32 = 10\n' + 'T10 u8[10] = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9]\n'


def slice_len_const(rng, bad):
    """A sized-slice parameter whose length names a MODULE CONSTANT: the caller's shorter array
    was accepted, and the callee reads past it (ASan)."""
    if bad:
        caller = ('    var small u8[2] = [1, 2]\n'
                  '    r = chain(small)\n')
    else:
        caller = ('    var full u8[4] = [1, 2, 3, 4]\n'
                  '    r = chain(full)\n')
    fn = ('func chain(a u8[K4]) u8 {\n'
          '    return a[3]\n}\n')
    return fn, 'u8', ('@CALLER@' + caller), 'K4 usize = 4\n'


def assert_bool(rng, bad):
    """`assert(b)` where b is not a comparison (a bool parameter) raised NO obligation at all.
    The `prove` side asserts a comparison, which is a real obligation and must still hold."""
    if bad:
        fn = ('func chain(flag bool) i32 {\n'
              '    assert(flag)\n'
              '    return 0\n}\n')
        witness = 'false'
    else:
        fn = ('func chain(n u32 <= 3) i32 {\n'
              '    assert(n <= 3)\n'
              '    return n as i32\n}\n')
        witness = '3'
    return fn, 'i32', witness, ''


def bool_from_negative(rng, bad):
    """`x as bool` was COPIED by the range analysis, so a negative x made the bool read as that
    negative number. A branch on `(b as i32) < 0` then looked DECIDED, the other arm read as dead,
    and an obligation on the dead arm was never checked. Truly `b as i32` is 0 or 1, so the
    comparison is always false and it is the 'dead' arm that runs."""
    if bad:
        fn = ('func chain(x i32 <= 0 - 1, z i32 >= 0 and <= 0) i32 {\n'
              '    b = x as bool\n'
              '    if (b as i32) < 0 {\n'
              '        return 1\n'
              '    }\n'
              '    return 100 / z\n}\n')
        witness = '0 - 5, 0'
    else:
        # A bool from a NON-negative value: the branch is honestly decidable and the divisor is
        # guarded, so the same shape must still be provable.
        fn = ('func chain(x i32 >= 0 and <= 1, z i32 >= 1 and <= 4) i32 {\n'
              '    b = x as bool\n'
              '    if (b as i32) < 0 {\n'
              '        return 1\n'
              '    }\n'
              '    return 100 / z\n}\n')
        witness = '1, 2'
    return fn, 'i32', witness, ''

SHAPES = [udiv_divisor, srem_then_index, cast_sign_index, bitcount_index,
          or_xor_index, narrow_cast, loop_bound, slice_end,
          srem_refined_dividend, cast_wrapping, precond_const, slice_len_const,
          assert_bool, bool_from_negative]


def gen(rng):
    shape = rng.choice(SHAPES)
    bad = rng.random() < 0.5
    fn, rty, witness, tables = shape(rng, bad)
    expect = 'reject' if bad else 'prove'
    fmt = '%d' if rty.startswith('i') or rty == 'u32' else '%d'
    out = ['// EXPECT: %s' % expect,
           '// shape: %s' % shape.__name__,
           PRELUDE.rstrip('\n')]
    if tables:
        out.append(tables.rstrip('\n'))
    out.append(fn.rstrip('\n'))
    out.append('func main() i32 effects io {')
    if witness.startswith('@CALLER@'):
        out.append(witness[len('@CALLER@'):].rstrip('\n'))
    else:
        out.append('    r = chain(%s)' % witness)
    out.append('    libc_printf("%s\\n", r as i32)' % fmt)
    out.append('    return 0')
    out.append('}')
    return '\n'.join(out) + '\n'


if __name__ == '__main__':
    seed = int(sys.argv[1]) if len(sys.argv) > 1 else 0
    sys.stdout.write(gen(random.Random(seed)))
