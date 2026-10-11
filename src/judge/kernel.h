#ifndef LAIN_JUDGE_KERNEL_H
#define LAIN_JUDGE_KERNEL_H
// kernel.h — the judge's KERNEL (local/internal/design/judge_interfaces.md §4). TRUSTED: it decides
// whether a certificate refutes a VC's hypotheses and the negation of its goal over the integers,
// and nothing else in the proof engine decides anything. The solver that writes certificates is
// untrusted; so is everything that proposes claims. A certificate the kernel accepts is a proof
// that the obligation holds wherever its hypotheses do (the generator's facts, checked at run time
// by judge_gate).
//
// It checks five steps and two rules, and searches nothing:
//   - `f<k> = lin m1*r1 + ...`: an exact non-negative combination of facts in scope (an equality
//     takes any sign), giving `sum a_i x_i <= b`;
//   - `f<k> = rnd f<j>`: a Chvátal-Gomory cut of a DERIVED fact: d = gcd of its coefficients,
//     computed here (a certificate cannot name it), each coefficient divided by d, and b replaced
//     with floor(b / d), FLOORED toward minus infinity (C's `/` truncates toward 0: `2x <= -1` is
//     `x <= -1`, not `x <= 0`). A fact with no variable has no gcd and is refused (A5); so is d = 1,
//     a cut that cuts nothing, which no certificate needs (the solver never writes one);
//   - `split s<k> : sum <= c`: in branch `le` s<k> is `sum <= c`, in `gt` `-sum <= -c - 1`; both close;
//   - `case s<k> : h<j>`: one branch per disjunct of h<j>, in branch n s<k>.<m> its m-th constraint;
//     every branch closes;
//   - `close f<k>`: f<k> is `0 <= b` with b < 0.
// SCOPING (A1): a fact made before a split or case is visible in all its branches; one made inside
// a branch only there and below. NUMBERING: an f or s number names one fact in the whole
// certificate, so a reference out of scope is refused by name. A sum it reads is CANONICAL
// (variables strictly ascending, each in the VC, coefficients nonzero): it never sorts, merges or
// normalises.
//
// Its integers are its own: JInt's representation (format.h: 512 bits in 32-bit limbs, sign and
// magnitude), with arithmetic written here, no compiler builtins. Anything that does not fit
// REFUSES the certificate; nothing wraps. Its memory is the caller's region (mem.h): running out
// refuses too. It refuses with a reason; it never aborts and keeps no state between calls. Its
// input may come in memory from the untrusted solver, so it assumes no field's representation: a
// number's sign byte, a step's kind, a pointer behind a count are checked, never trusted.
#include <string.h>
#include <stdio.h>
#include "judge/format.h"
#include "judge/mem.h"

// ── magnitudes: limb[0..n) little-endian, limb[n-1] != 0, zero n == 0 ──────────────────────────
typedef JInt KInt;
static void kint_norm(KInt *x) { while (x->n && x->limb[x->n - 1] == 0) x->n--; if (!x->n) x->neg = 0; }
static void kint_set(KInt *x, int64_t v) {
    memset(x, 0, sizeof *x);
    uint64_t u = v < 0 ? (uint64_t)0 - (uint64_t)v : (uint64_t)v;
    x->limb[0] = (uint32_t)u; x->limb[1] = (uint32_t)(u >> 32); x->n = 2; x->neg = v < 0;
    kint_norm(x);
}
// Canonical: a sign of 0 or 1 (the arithmetic compares signs, so a 2 would read as a third sign:
// I.197, Main Compiler Work's review), at most J_LIMBS limbs, the top one nonzero, zero unsigned.
static bool kint_canon(const KInt *x) {
    if (x->neg > 1 || x->n > J_LIMBS) return false;
    if (x->n == 0) return x->neg == 0;
    return x->limb[x->n - 1] != 0;
}
static int kmag_cmp(const KInt *a, const KInt *b) {
    if (a->n != b->n) return a->n < b->n ? -1 : 1;
    for (int i = a->n - 1; i >= 0; i--) if (a->limb[i] != b->limb[i]) return a->limb[i] < b->limb[i] ? -1 : 1;
    return 0;
}
// r = |a| + |b|; false when it needs more than J_LIMBS limbs
static bool kmag_add(KInt *r, const KInt *a, const KInt *b) {
    KInt t; memset(&t, 0, sizeof t);
    int n = a->n > b->n ? a->n : b->n;
    uint64_t c = 0;
    for (int i = 0; i < n; i++) {
        uint64_t s = c + (i < a->n ? a->limb[i] : 0) + (i < b->n ? b->limb[i] : 0);
        t.limb[i] = (uint32_t)s; c = s >> 32;
    }
    if (c) { if (n == J_LIMBS) return false; t.limb[n++] = (uint32_t)c; }
    t.n = (uint8_t)n; kint_norm(&t); *r = t; return true;
}
// r = |a| - |b|, |a| >= |b|
static void kmag_sub(KInt *r, const KInt *a, const KInt *b) {
    KInt t; memset(&t, 0, sizeof t);
    int64_t br = 0;
    for (int i = 0; i < a->n; i++) {
        int64_t d = (int64_t)a->limb[i] - (i < b->n ? b->limb[i] : 0) - br;
        br = d < 0; if (d < 0) d += (int64_t)1 << 32;
        t.limb[i] = (uint32_t)d;
    }
    t.n = a->n; kint_norm(&t); *r = t;
}
// r = a + b (signed)
static bool kint_add(KInt *r, const KInt *a, const KInt *b) {
    if (a->neg == b->neg) { uint8_t s = a->neg; if (!kmag_add(r, a, b)) return false; r->neg = r->n ? s : 0; return true; }
    int c = kmag_cmp(a, b);
    if (c == 0) { memset(r, 0, sizeof *r); return true; }
    if (c > 0) { uint8_t s = a->neg; kmag_sub(r, a, b); r->neg = r->n ? s : 0; }
    else       { uint8_t s = b->neg; kmag_sub(r, b, a); r->neg = r->n ? s : 0; }
    return true;
}
static void kint_neg(KInt *x) { if (x->n) x->neg = !x->neg; }
// r = a * b (signed); false when it does not fit
static bool kint_mul(KInt *r, const KInt *a, const KInt *b) {
    if (!a->n || !b->n) { memset(r, 0, sizeof *r); return true; }
    if (a->n + b->n - 1 > J_LIMBS) return false;
    uint32_t t[2 * J_LIMBS + 1]; memset(t, 0, sizeof t);
    for (int i = 0; i < a->n; i++) {
        uint64_t c = 0;
        for (int j = 0; j < b->n; j++) {
            uint64_t p = (uint64_t)a->limb[i] * b->limb[j] + t[i + j] + c;
            t[i + j] = (uint32_t)p; c = p >> 32;
        }
        int k = i + b->n;
        while (c) { uint64_t s = (uint64_t)t[k] + c; t[k] = (uint32_t)s; c = s >> 32; k++; }
    }
    int n = a->n + b->n;
    while (n && t[n - 1] == 0) n--;
    if (n > J_LIMBS) return false;
    KInt x; memset(&x, 0, sizeof x);
    for (int i = 0; i < n; i++) x.limb[i] = t[i];
    x.n = (uint8_t)n; x.neg = (uint8_t)(a->neg != b->neg); kint_norm(&x); *r = x; return true;
}
static bool kmag_bit(const KInt *x, int i) { return (i >> 5) < x->n && ((x->limb[i >> 5] >> (i & 31)) & 1u); }
// q = |a| / |b|, m = |a| mod |b|, b != 0: long division, one bit at a time
static void kmag_divmod(KInt *q, KInt *m, const KInt *a, const KInt *b) {
    KInt Q, R; memset(&Q, 0, sizeof Q); memset(&R, 0, sizeof R);
    for (int i = a->n * 32 - 1; i >= 0; i--) {
        // R = 2R + bit: R < |b| <= 2^512 before, so 2R + 1 < 2^513 fits one spare limb at most;
        // |b| fits J_LIMBS limbs and R < |b| after the subtraction, so R never exceeds J_LIMBS
        uint32_t c = kmag_bit(a, i);
        for (int j = 0; j < J_LIMBS; j++) { uint32_t nc = R.limb[j] >> 31; R.limb[j] = (R.limb[j] << 1) | c; c = nc; }
        int over = (int)c;                                 // the bit shifted past limb J_LIMBS-1
        R.n = J_LIMBS; kint_norm(&R); R.neg = 0;
        if (over || kmag_cmp(&R, b) >= 0) {
            if (over) {                                    // R + 2^512 - |b|: |b| > R here
                KInt t; kmag_sub(&t, b, &R);               // t = |b| - R
                // (R + 2^512) - |b| = 2^512 - t: the two's complement of t in J_LIMBS limbs
                KInt u; memset(&u, 0, sizeof u);
                uint64_t cc = 1;
                for (int j = 0; j < J_LIMBS; j++) { uint64_t s = (uint64_t)(uint32_t)~(j < t.n ? t.limb[j] : 0u) + cc; u.limb[j] = (uint32_t)s; cc = s >> 32; }
                u.n = J_LIMBS; kint_norm(&u); R = u;
            } else kmag_sub(&R, &R, b);
            Q.limb[i >> 5] |= 1u << (i & 31);
        }
    }
    Q.n = J_LIMBS; kint_norm(&Q); Q.neg = 0;
    *q = Q; *m = R;
}
// floor(a / d) for d > 0, toward minus infinity
static void kint_floordiv(KInt *r, const KInt *a, const KInt *d) {
    KInt q, m; kmag_divmod(&q, &m, a, d);
    if (a->neg) {
        KInt one; kint_set(&one, 1);
        if (m.n) { KInt t; kmag_add(&t, &q, &one); q = t; }   // |q| + 1 <= |a| fits
        q.neg = q.n ? 1 : 0;
    }
    *r = q;
}
static void kint_gcd(KInt *r, const KInt *a, const KInt *b) {
    KInt x = *a, y = *b; x.neg = y.neg = 0;
    while (y.n) { KInt q, m; kmag_divmod(&q, &m, &x, &y); x = y; y = m; }
    *r = x;
}

// ── facts ─────────────────────────────────────────────────────────────────────────────────────
// `sum a_i x_i <= b`, or `= b` for an equality: a[0..nvars) dense, over the VC's variables.
typedef struct { bool eq; KInt *a; KInt b; } KFact;
typedef struct {
    JMem *m; const JVc *v;
    char *why; size_t whycap;
    KFact *hyp; bool *hypok;                 // h1..h<nhyps> at [k - 1] (hypok false: disjunctive)
    KFact g;
    int32_t nf, ns;                          // the numbers' bounds (max + 1)
    KFact *f; uint8_t *fstate;               // 0 never made, 1 visible, 2 made and out of scope
    KFact *s; uint8_t *sstate;               // a split's fact; a case's in sc
    const JConj **sc;                        // s<k> made by a case: the chosen disjunct
    int32_t *made; int32_t nmade, cmade;     // the derived facts made, to hide at their branch's end
} KK;
static bool kfail(KK *k, const char *why, int32_t a) {
    if (k->why && k->whycap) { if (a >= 0) snprintf(k->why, k->whycap, "%s (%d)", why, a); else snprintf(k->why, k->whycap, "%s", why); }
    return false;
}
static bool kfact_new(KK *k, KFact *x) {
    memset(x, 0, sizeof *x);
    x->a = JMEM_NEW(k->m, KInt, k->v->nvars ? k->v->nvars : 1);
    if (!x->a) return kfail(k, "out of memory (the caller's region is full)", -1);
    memset(x->a, 0, sizeof *x->a * (size_t)(k->v->nvars ? k->v->nvars : 1));
    return true;
}
// A constraint as written: canonical, or refused.
static bool kfact_of(KK *k, const JCons *c, KFact *x) {
    if (c->op == J_NE) return kfail(k, "a `!=` is a goal only, never a fact", -1);
    if (c->op != J_LE && c->op != J_EQ) return kfail(k, "a constraint of no known kind", -1);
    if (c->nt < 0 || (c->nt > 0 && !c->t)) return kfail(k, "a sum with no terms", -1);
    if (!kfact_new(k, x)) return false;
    x->eq = c->op == J_EQ;
    if (!kint_canon(&c->rhs)) return kfail(k, "a constant that is not canonical", -1);
    x->b = c->rhs;
    for (int32_t i = 0; i < c->nt; i++) {
        int32_t v = c->t[i].var;
        if (v < 0 || v >= k->v->nvars) return kfail(k, "a variable the VC does not declare", v);
        if (i && v <= c->t[i - 1].var) return kfail(k, "a sum that is not strictly ascending by variable", v);
        if (!kint_canon(&c->t[i].coef) || !c->t[i].coef.n) return kfail(k, "a coefficient that is zero or not canonical", v);
        x->a[v] = c->t[i].coef;
    }
    return true;
}
static bool kmade(KK *k, int32_t id) {
    if (k->nmade == k->cmade) {
        int32_t nc = k->cmade ? k->cmade * 2 : 32;
        int32_t *n = JMEM_NEW(k->m, int32_t, nc);
        if (!n) return kfail(k, "out of memory (the caller's region is full)", -1);
        if (k->nmade) memcpy(n, k->made, sizeof *n * (size_t)k->nmade);
        k->made = n; k->cmade = nc;
    }
    k->made[k->nmade++] = id;
    return true;
}
// The fact a reference names, in scope (a case fact made in `tmp`).
static const KFact *kref(KK *k, const JRef *r, KFact *tmp) {
    switch (r->k) {
        case J_REF_G: return &k->g;
        case J_REF_H:
            if (r->a < 1 || r->a > k->v->nhyps) { kfail(k, "a hypothesis the VC does not have", r->a); return NULL; }
            if (!k->hypok[r->a - 1]) { kfail(k, "a disjunctive hypothesis outside a `case`", r->a); return NULL; }
            return &k->hyp[r->a - 1];
        case J_REF_F:
            if (r->a < 1 || r->a >= k->nf || k->fstate[r->a] != 1) { kfail(k, "a derived fact not in scope", r->a); return NULL; }
            return &k->f[r->a];
        case J_REF_S:
            if (r->a < 1 || r->a >= k->ns || k->sstate[r->a] != 1 || k->sc[r->a]) { kfail(k, "a split fact not in scope", r->a); return NULL; }
            return &k->s[r->a];
        case J_REF_SC: {
            if (r->a < 1 || r->a >= k->ns || k->sstate[r->a] != 1 || !k->sc[r->a]) { kfail(k, "a case fact not in scope", r->a); return NULL; }
            const JConj *d = k->sc[r->a];
            if (r->b < 1 || r->b > d->nc || !d->c) { kfail(k, "a case fact its disjunct does not have", r->b); return NULL; }
            if (!kfact_of(k, &d->c[r->b - 1], tmp)) return NULL;
            return tmp;
        }
    }
    kfail(k, "a reference of no known kind", -1);
    return NULL;
}
static bool ksteps(KK *k, const JSteps *br);
// Check one branch, and hide the derived facts it made at its end (its split and case facts are
// hidden by the split or case that made them).
static bool kbranch(KK *k, const JSteps *br) {
    int32_t base = k->nmade;
    bool ok = ksteps(k, br);
    for (int32_t i = base; i < k->nmade; i++) k->fstate[k->made[i]] = 2;
    k->nmade = base;
    return ok;
}
static bool ksteps(KK *k, const JSteps *br) {
    if (!br || br->n < 1 || !br->s) return kfail(k, "a branch with no step", -1);
    for (int32_t i = 0; i < br->n; i++) {
        const JStep *s = &br->s[i];
        bool last = i == br->n - 1, term = s->k == J_CLOSE || s->k == J_SPLIT || s->k == J_CASE;
        if (last != term) return kfail(k, last ? "a branch that does not end in `close`, `split` or `case`" : "a step after a terminal step", i);
        switch (s->k) {
            case J_LIN: case J_RND: {
                if (s->f < 1 || s->f >= k->nf) return kfail(k, "a derived fact's number out of range", s->f);
                if (k->fstate[s->f]) return kfail(k, "a derived fact's number used twice", s->f);
                KFact r;
                if (!kfact_new(k, &r)) return false;
                if (s->k == J_LIN) {
                    if (s->nmul < 1 || !s->mul) return kfail(k, "an empty combination", s->f);
                    kint_set(&r.b, 0);
                    for (int32_t j = 0; j < s->nmul; j++) {
                        KFact tmp; const KFact *x = kref(k, &s->mul[j].r, &tmp);
                        if (!x) return false;
                        const KInt *mu = &s->mul[j].m;
                        if (!kint_canon(mu)) return kfail(k, "a multiplier that is not canonical", s->f);
                        if (mu->neg && !x->eq) return kfail(k, "a negative multiplier on an inequality", s->f);
                        for (int32_t v = 0; v < k->v->nvars; v++) {
                            if (!x->a[v].n) continue;
                            KInt p; if (!kint_mul(&p, mu, &x->a[v]) || !kint_add(&r.a[v], &r.a[v], &p)) return kfail(k, "a coefficient beyond 512 bits", s->f);
                        }
                        KInt p; if (!kint_mul(&p, mu, &x->b) || !kint_add(&r.b, &r.b, &p)) return kfail(k, "a constant beyond 512 bits", s->f);
                    }
                } else {
                    if (s->of < 1 || s->of >= k->nf || k->fstate[s->of] != 1) return kfail(k, "a cut of a derived fact not in scope", s->of);
                    const KFact *x = &k->f[s->of];
                    KInt d; memset(&d, 0, sizeof d);
                    for (int32_t v = 0; v < k->v->nvars; v++) if (x->a[v].n) kint_gcd(&d, &d, &x->a[v]);
                    if (!d.n) return kfail(k, "a cut of a fact with no variable (its gcd is undefined)", s->f);
                    KInt one; kint_set(&one, 1);
                    if (kmag_cmp(&d, &one) == 0) return kfail(k, "a cut by 1, which cuts nothing", s->f);
                    for (int32_t v = 0; v < k->v->nvars; v++) {
                        if (!x->a[v].n) continue;
                        KInt q, m; kmag_divmod(&q, &m, &x->a[v], &d);
                        q.neg = q.n ? x->a[v].neg : 0;
                        r.a[v] = q;
                    }
                    kint_floordiv(&r.b, &x->b, &d);
                }
                r.eq = false;
                k->f[s->f] = r; k->fstate[s->f] = 1;
                if (!kmade(k, s->f)) return false;
                break;
            }
            case J_CLOSE: {
                if (s->f < 1 || s->f >= k->nf || k->fstate[s->f] != 1) return kfail(k, "a close of a derived fact not in scope", s->f);
                const KFact *x = &k->f[s->f];
                for (int32_t v = 0; v < k->v->nvars; v++) if (x->a[v].n) return kfail(k, "a close of a fact that still has a variable", s->f);
                if (!x->b.neg) return kfail(k, "a close of `0 <= b` with b not negative", s->f);
                return true;
            }
            case J_SPLIT: {
                if (s->s < 1 || s->s >= k->ns) return kfail(k, "a split fact's number out of range", s->s);
                if (k->sstate[s->s]) return kfail(k, "a split fact's number used twice", s->s);
                if (s->split.op != J_LE) return kfail(k, "a split on other than `<=`", s->s);
                if (s->nbr != 2 || !s->br) return kfail(k, "a split without exactly `le` and `gt`", s->s);
                KFact le; if (!kfact_of(k, &s->split, &le)) return false;
                KFact gt; if (!kfact_new(k, &gt)) return false;
                for (int32_t v = 0; v < k->v->nvars; v++) { gt.a[v] = le.a[v]; kint_neg(&gt.a[v]); }
                KInt one; kint_set(&one, 1);
                gt.b = le.b; kint_neg(&gt.b);
                KInt mo = one; kint_neg(&mo);
                if (!kint_add(&gt.b, &gt.b, &mo)) return kfail(k, "a split constant beyond 512 bits", s->s);
                for (int b = 0; b < 2; b++) {
                    k->s[s->s] = b == 0 ? le : gt; k->sstate[s->s] = 1; k->sc[s->s] = NULL;
                    if (!kbranch(k, &s->br[b])) return false;
                }
                k->sstate[s->s] = 2;
                return true;
            }
            case J_CASE: {
                if (s->s < 1 || s->s >= k->ns) return kfail(k, "a case fact's number out of range", s->s);
                if (k->sstate[s->s]) return kfail(k, "a case fact's number used twice", s->s);
                if (s->hyp < 1 || s->hyp > k->v->nhyps) return kfail(k, "a case on a hypothesis the VC does not have", s->hyp);
                const JHyp *h = &k->v->hyps[s->hyp - 1];
                if (!h->disj) return kfail(k, "a case on a hypothesis that is not a disjunction", s->hyp);
                if (s->nbr != h->ndis || !s->br || !h->dis) return kfail(k, "a case whose branches are not one per disjunct", s->hyp);
                for (int32_t b = 0; b < s->nbr; b++) {
                    k->sstate[s->s] = 1; k->sc[s->s] = &h->dis[b];
                    if (!kbranch(k, &s->br[b])) return false;
                }
                k->sstate[s->s] = 2; k->sc[s->s] = NULL;
                return true;
            }
            default: return kfail(k, "a step of no known kind", i);
        }
    }
    return kfail(k, "a branch that does not end in a terminal step", -1);
}
// The numbers a certificate uses, to size the tables. It reads before anything is checked, so it
// follows no pointer it has not seen set (ksteps refuses what is missing).
static void kscan(const JSteps *br, int32_t *mf, int32_t *ms) {
    for (int32_t i = 0; br && br->s && i < br->n; i++) {
        const JStep *s = &br->s[i];
        if ((s->k == J_LIN || s->k == J_RND || s->k == J_CLOSE) && s->f > *mf) *mf = s->f;
        if (s->k == J_RND && s->of > *mf) *mf = s->of;
        if (s->k == J_LIN && s->mul) for (int32_t j = 0; j < s->nmul; j++) {
            const JRef *r = &s->mul[j].r;
            if (r->k == J_REF_F && r->a > *mf) *mf = r->a;
            if ((r->k == J_REF_S || r->k == J_REF_SC) && r->a > *ms) *ms = r->a;
        }
        if ((s->k == J_SPLIT || s->k == J_CASE) && s->s > *ms) *ms = s->s;
        for (int32_t b = 0; s->br && b < s->nbr; b++) kscan(&s->br[b], mf, ms);
    }
}
#define J_KERNEL_MAX_NUMBER (1 << 20)
// Does certificate `c` refute VC `v` (its hypotheses and its goal's negation) over the integers?
// True, or false with the reason in `why`. Its tables and facts are made in `m`.
static bool jkernel_check(JMem *m, const JVc *v, const JCert *c, char *why, size_t whycap) {
    KK k; memset(&k, 0, sizeof k);
    k.m = m; k.v = v; k.why = why; k.whycap = whycap;
    if (why && whycap) why[0] = 0;
    if (!v || !c || v->nvars < 0 || v->nhyps < 0 || (v->nhyps > 0 && !v->hyps)) return kfail(&k, "no VC or no certificate", -1);
    k.hyp = JMEM_NEW(m, KFact, v->nhyps ? v->nhyps : 1);
    k.hypok = JMEM_NEW(m, bool, v->nhyps ? v->nhyps : 1);
    if (!k.hyp || !k.hypok) return kfail(&k, "out of memory (the caller's region is full)", -1);
    for (int32_t i = 0; i < v->nhyps; i++) {
        const JHyp *h = &v->hyps[i];
        k.hypok[i] = !h->disj;
        if (h->disj) {                                    // checked as a case reads it: canonical
            if (h->ndis < 2) return kfail(&k, "a disjunction of fewer than two disjuncts", i + 1);
            continue;
        }
        if (!kfact_of(&k, &h->cons, &k.hyp[i])) return false;
    }
    // g, the goal's negation: `sum <= b` negated is `-sum <= -b - 1`; `sum != b` negated is `sum = b`
    KFact goal;
    if (v->goal.op == J_EQ) return kfail(&k, "a goal that is an equality (it is two VCs)", -1);
    {   JCons gc = v->goal; gc.op = v->goal.op == J_NE ? J_EQ : J_LE;
        if (!kfact_of(&k, &gc, &goal)) return false; }
    if (v->goal.op == J_NE) k.g = goal;
    else {
        if (!kfact_new(&k, &k.g)) return false;
        for (int32_t x = 0; x < v->nvars; x++) { k.g.a[x] = goal.a[x]; kint_neg(&k.g.a[x]); }
        KInt mo; kint_set(&mo, -1);
        k.g.b = goal.b; kint_neg(&k.g.b);
        if (!kint_add(&k.g.b, &k.g.b, &mo)) return kfail(&k, "a goal constant beyond 512 bits", -1);
    }
    int32_t mf = 0, ms = 0;
    kscan(&c->body, &mf, &ms);
    if (mf >= J_KERNEL_MAX_NUMBER || ms >= J_KERNEL_MAX_NUMBER) return kfail(&k, "a fact number beyond the kernel's tables", -1);
    k.nf = mf + 1; k.ns = ms + 1;
    k.f = JMEM_NEW(m, KFact, k.nf); k.fstate = JMEM_NEW(m, uint8_t, k.nf);
    k.s = JMEM_NEW(m, KFact, k.ns); k.sstate = JMEM_NEW(m, uint8_t, k.ns); k.sc = JMEM_NEW(m, const JConj *, k.ns);
    if (!k.f || !k.fstate || !k.s || !k.sstate || !k.sc) return kfail(&k, "out of memory (the caller's region is full)", -1);
    memset(k.fstate, 0, (size_t)k.nf); memset(k.sstate, 0, (size_t)k.ns); memset(k.sc, 0, sizeof *k.sc * (size_t)k.ns);
    return ksteps(&k, &c->body);
}

#endif // LAIN_JUDGE_KERNEL_H
