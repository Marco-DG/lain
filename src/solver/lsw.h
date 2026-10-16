#ifndef LAIN_SOLVER_LSW_H
#define LAIN_SOLVER_LSW_H
// lsw.h — UNTRUSTED: the wide integers of lia.h's wide retry (s3, PE.13).
//
// LSW is JInt's own form (judge/format.h): sign and magnitude, J_LIMBS limbs of 32 bits, the top
// limb nonzero, zero unsigned. So every number it holds is one a certificate or a model can state,
// and its bound IS the kernel's: J_LIMBS, one definition. Its add, sub and mul refuse exactly where
// the kernel's kint_add and kint_mul do. A sum fails past J_LIMBS limbs. A product fails when its
// operands' limb counts exceed J_LIMBS + 1, or its result does. lia.h's certificate and model
// evaluators rely on that to say, before the judge does, whether the judge can follow an answer.
// This arithmetic is the solver's own, written apart from the kernel's (judge_includes.py keeps
// src/solver/ out of src/judge/'s functions); solver_harness.py checks it against Python's integers.
//
// Every operation scales with its operands' own limbs, not with J_LIMBS: the wide retry's numbers
// are mostly a few limbs. The gcd is binary (shifts and subtractions); the division is long
// division one bit at a time over the dividend's bits. Both only run on the wide path, which runs
// only after a 128-bit search overflowed.
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "judge/format.h"

typedef JInt LSW;

static void lw_norm(LSW *x) { while (x->n && x->limb[x->n - 1] == 0) x->n--; if (!x->n) x->neg = 0; }
static LSW lw_i(int64_t v) {
    LSW x; memset(&x, 0, sizeof x);
    uint64_t u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    x.limb[0] = (uint32_t)u; x.limb[1] = (uint32_t)(u >> 32); x.n = 2; x.neg = v < 0;
    lw_norm(&x);
    return x;
}
static int lw_sgn(LSW a) { return a.n == 0 ? 0 : a.neg ? -1 : 1; }
static int lw_mag_cmp(const LSW *a, const LSW *b) {
    if (a->n != b->n) return a->n < b->n ? -1 : 1;
    for (int i = a->n - 1; i >= 0; i--) if (a->limb[i] != b->limb[i]) return a->limb[i] < b->limb[i] ? -1 : 1;
    return 0;
}
static int lw_cmp(LSW a, LSW b) {
    if (a.neg != b.neg) return a.neg ? -1 : 1;
    int c = lw_mag_cmp(&a, &b);
    return a.neg ? -c : c;
}
// r = |a| + |b|; false when it needs more than J_LIMBS limbs (kint_add's rule)
static bool lw_mag_add(LSW *r, const LSW *a, const LSW *b) {
    LSW t; memset(&t, 0, sizeof t);
    int n = a->n > b->n ? a->n : b->n;
    uint64_t c = 0;
    for (int i = 0; i < n; i++) {
        uint64_t s = c + (i < a->n ? a->limb[i] : 0) + (i < b->n ? b->limb[i] : 0);
        t.limb[i] = (uint32_t)s; c = s >> 32;
    }
    if (c) { if (n == J_LIMBS) return false; t.limb[n++] = (uint32_t)c; }
    t.n = (uint8_t)n; lw_norm(&t); *r = t; return true;
}
// r = |a| - |b|, |a| >= |b|
static void lw_mag_sub(LSW *r, const LSW *a, const LSW *b) {
    LSW t; memset(&t, 0, sizeof t);
    int64_t br = 0;
    for (int i = 0; i < a->n; i++) {
        int64_t d = (int64_t)a->limb[i] - (i < b->n ? b->limb[i] : 0) - br;
        br = d < 0; if (d < 0) d += (int64_t)1 << 32;
        t.limb[i] = (uint32_t)d;
    }
    t.n = a->n; lw_norm(&t); *r = t;
}
static bool lw_add_to(LSW *r, LSW a, LSW b) {
    if (a.neg == b.neg) { uint8_t s = a.neg; if (!lw_mag_add(r, &a, &b)) return false; r->neg = r->n ? s : 0; return true; }
    int c = lw_mag_cmp(&a, &b);
    if (c == 0) { memset(r, 0, sizeof *r); return true; }
    if (c > 0) { uint8_t s = a.neg; lw_mag_sub(r, &a, &b); r->neg = r->n ? s : 0; }
    else       { uint8_t s = b.neg; lw_mag_sub(r, &b, &a); r->neg = r->n ? s : 0; }
    return true;
}
static LSW lw_neg(LSW x) { if (x.n) x.neg = !x.neg; return x; }
// r = a * b; false when it does not fit (kint_mul's rule, its pre-check included)
static bool lw_mul_to(LSW *r, LSW a, LSW b) {
    if (!a.n || !b.n) { memset(r, 0, sizeof *r); return true; }
    if (a.n + b.n - 1 > J_LIMBS) return false;
    uint32_t t[2 * J_LIMBS + 1]; memset(t, 0, sizeof t);
    for (int i = 0; i < a.n; i++) {
        uint64_t c = 0;
        for (int j = 0; j < b.n; j++) {
            uint64_t p = (uint64_t)a.limb[i] * b.limb[j] + t[i + j] + c;
            t[i + j] = (uint32_t)p; c = p >> 32;
        }
        int k = i + b.n;
        while (c) { uint64_t s = (uint64_t)t[k] + c; t[k] = (uint32_t)s; c = s >> 32; k++; }
    }
    int n = a.n + b.n;
    while (n && t[n - 1] == 0) n--;
    if (n > J_LIMBS) return false;
    LSW x; memset(&x, 0, sizeof x);
    for (int i = 0; i < n; i++) x.limb[i] = t[i];
    x.n = (uint8_t)n; x.neg = (uint8_t)(a.neg != b.neg); lw_norm(&x); *r = x; return true;
}
// q = |a| / |b|, m = |a| mod |b|, b != 0: long division, one bit of a at a time
static void lw_mag_divmod(LSW *q, LSW *m, const LSW *a, const LSW *b) {
    uint32_t Q[J_LIMBS], R[J_LIMBS + 1];
    memset(Q, 0, sizeof Q); memset(R, 0, sizeof R);
    int rn = 0;                                          // R's limbs in use; R < |b| between steps
    for (int i = a->n * 32 - 1; i >= 0; i--) {
        uint32_t c = (a->limb[i >> 5] >> (i & 31)) & 1u;
        for (int j = 0; j <= rn && j <= J_LIMBS; j++) { uint32_t nc = R[j] >> 31; R[j] = (R[j] << 1) | c; c = nc; }
        if (rn < J_LIMBS + 1 && R[rn]) rn++;
        // R >= |b|?
        int ge;
        if (rn != b->n) ge = rn > b->n;
        else { ge = 1; for (int j = rn - 1; j >= 0; j--) if (R[j] != b->limb[j]) { ge = R[j] > b->limb[j]; break; } }
        if (ge) {
            int64_t br = 0;
            for (int j = 0; j < rn; j++) {
                int64_t d = (int64_t)R[j] - (j < b->n ? b->limb[j] : 0) - br;
                br = d < 0; if (d < 0) d += (int64_t)1 << 32;
                R[j] = (uint32_t)d;
            }
            while (rn && R[rn - 1] == 0) rn--;
            Q[i >> 5] |= 1u << (i & 31);
        }
    }
    memset(q, 0, sizeof *q); memset(m, 0, sizeof *m);
    for (int j = 0; j < J_LIMBS; j++) q->limb[j] = Q[j];
    q->n = J_LIMBS; lw_norm(q);
    for (int j = 0; j < rn && j < J_LIMBS; j++) m->limb[j] = R[j];
    m->n = (uint8_t)(rn < J_LIMBS ? rn : J_LIMBS); lw_norm(m);
}
// a / b truncated toward zero, b != 0 (never wider than a)
static LSW lw_div(LSW a, LSW b) {
    LSW q, m; lw_mag_divmod(&q, &m, &a, &b);
    q.neg = q.n ? (uint8_t)(a.neg != b.neg) : 0;
    return q;
}
static bool lw_divides(LSW a, LSW b) {                    // b != 0: does b divide a?
    LSW q, m; lw_mag_divmod(&q, &m, &a, &b);
    return m.n == 0;
}
// floor(a / b), b > 0; false only when q - 1 does not fit
static bool lw_floordiv_to(LSW *r, LSW a, LSW b) {
    LSW q, m; lw_mag_divmod(&q, &m, &a, &b);
    q.neg = q.n ? (uint8_t)(a.neg != b.neg) : 0;
    if (m.n && a.neg != b.neg) { LSW one = lw_i(1); if (!lw_add_to(&q, q, lw_neg(one))) return false; }
    *r = q; return true;
}
static int lw_ctz(const LSW *x) {                        // x != 0: its trailing zero bits
    int i = 0; while (x->limb[i] == 0) i++;
    return i * 32 + __builtin_ctz(x->limb[i]);
}
static void lw_shr(LSW *x, int k) {                       // |x| >> k
    int w = k >> 5, s = k & 31;
    for (int i = 0; i < x->n; i++) {
        uint64_t lo = i + w < x->n ? x->limb[i + w] : 0, hi = i + w + 1 < x->n ? x->limb[i + w + 1] : 0;
        x->limb[i] = s ? (uint32_t)((lo >> s) | (hi << (32 - s))) : (uint32_t)lo;
    }
    lw_norm(x);
}
static bool lw_shl(LSW *x, int k) {                       // |x| << k; false past J_LIMBS limbs
    for (int i = 0; i < k; i++) { LSW t = *x; if (!lw_mag_add(x, &t, &t)) return false; }
    return true;
}
// the gcd of |a| and |b| (0 when both are 0): binary, Stein's
static LSW lw_gcd(LSW a, LSW b) {
    a.neg = b.neg = 0;
    if (!a.n) return b;
    if (!b.n) return a;
    int za = lw_ctz(&a), zb = lw_ctz(&b), k = za < zb ? za : zb;
    lw_shr(&a, za); lw_shr(&b, zb);
    for (;;) {                                           // both odd
        int c = lw_mag_cmp(&a, &b);
        if (c == 0) break;
        if (c < 0) { LSW t = a; a = b; b = t; }
        lw_mag_sub(&a, &a, &b);                          // even, nonzero
        lw_shr(&a, lw_ctz(&a));
    }
    lw_shl(&a, k);                                       // never past a's own width: the gcd divides both
    return a;
}

#endif // LAIN_SOLVER_LSW_H
