#ifndef LAIN_SOLVER_LIA_H
#define LAIN_SOLVER_LIA_H
// lia.h — THE UNTRUSTED SOLVER: linear integer arithmetic over one VC (judge_interfaces.md).
//
// It reads a JVc and answers with a certificate, which the trusted kernel checks, a model, which
// the judge evaluates before showing it, or a refusal that names what ran out. Nothing it says is
// believed: a bug here can cost a proof, never admit one. So it is built to be right OFTEN and to
// be DETERMINISTIC, and the kernel is what is built to be right always.
//
//   - Over the rationals, a general simplex (Dutertre and de Moura, CAV 2006) with Bland's rule:
//     the smallest-index variable outside its bounds, repaired by the smallest-index variable that
//     can move. When none can, the violated row IS the refutation: its coefficients over the
//     slacks at their bounds are the Farkas multipliers, and the certificate is `lin` then `close`.
//   - Over the integers, the gcd test (`lin` then `rnd`) on each constraint as it enters, and
//     branch and bound (`split`) on the first variable whose value is not an integer.
//   - A disjunctive hypothesis is split (`case`) only when the integer model in hand violates it.
//
// Every variable of the VC is unbounded unless a hypothesis bounds it (A3), so the simplex keeps
// bounds on slacks only: one slack per constraint, `row <= b`, and `row >= b` too for an equality.
// An original variable is never at a bound, so a conflict row mentions slacks only, and each
// slack is one fact.
//
// NUMBERS are 128-bit and checked at every operation. A number that leaves them, or a region too
// small for the search, is the answer J_ANS_CAPACITY, never a wrap; the kernel's 512 bits hold
// every number this solver can state. A wider type is a change to the LSi functions only.
//
// DETERMINISM: variables in VC order, constraints in hypothesis order then g then each branch's
// own, Bland's rule throughout; no hash, no pointer order, no time. The same VC and the same
// limits give the same answer, byte for byte.
//
// A VC that stops on CAPACITY is asked again on fewer of its hypotheses (the sliced retry, at the
// end of this file), and only a certificate from that is answered.
//
// Only judge/format.h is included from the judge (scripts/gates/judge_includes.py checks it).
// Memory is the caller's region: the search's scratch grows up from its start and is released
// node by node, and the answer grows down from its end, so it outlives the search that built it.
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include "judge/format.h"

__extension__ typedef __int128 LSi;
__extension__ typedef unsigned __int128 LSu;
typedef struct { LSi n, d; } LQ;            // n / d in lowest terms, d >= 1

typedef struct {
    char *lo, *hi;              // scratch below lo is in use; the answer lives from hi up
    bool cap;                   // a number left 128 bits, or the region is full
    bool out;                   // a budget counter reached its limit
    JWork used, limit;
    int32_t nf, ns;             // the f and s numbers given so far
    int32_t nv;                 // the VC's variables
    const uint8_t *on;          // the hypotheses in play, by index (NULL: all): the sliced retry's
    bool cone;                  // lia_cone's search: a rational model is scaled, never split
} LSol;

// ── memory ──────────────────────────────────────────────────────────────────────────────────────
static void *ls_scratch(LSol *S, size_t size, int64_t n) {
    uintptr_t at = ((uintptr_t)S->lo + 15u) & ~(uintptr_t)15u;
    if (n < 0 || at > (uintptr_t)S->hi || (size && (uint64_t)n > ((uintptr_t)S->hi - at) / size)) {
        S->cap = true; return NULL;
    }
    S->lo = (char *)at + size * (size_t)n;
    memset((void *)at, 0, size * (size_t)n);
    return (void *)at;
}
static void *ls_keep(LSol *S, size_t size, int64_t n) {
    if (n < 0 || (size && (uint64_t)n > ((uintptr_t)S->hi - (uintptr_t)S->lo) / size)) {
        S->cap = true; return NULL;
    }
    uintptr_t at = ((uintptr_t)S->hi - size * (size_t)n) & ~(uintptr_t)15u;
    if (at < (uintptr_t)S->lo) { S->cap = true; return NULL; }
    S->hi = (char *)at;
    memset((void *)at, 0, size * (size_t)n);
    return (void *)at;
}

// ── numbers: checked 128-bit integers and rationals over them ─────────────────────────────────
static LSi ls_add(LSol *S, LSi a, LSi b) { LSi r; if (__builtin_add_overflow(a, b, &r)) { S->cap = true; return 0; } return r; }
static LSi ls_sub(LSol *S, LSi a, LSi b) { LSi r; if (__builtin_sub_overflow(a, b, &r)) { S->cap = true; return 0; } return r; }
static LSi ls_mul(LSol *S, LSi a, LSi b) { LSi r; if (__builtin_mul_overflow(a, b, &r)) { S->cap = true; return 0; } return r; }
static LSu ls_abs(LSi a) { return a < 0 ? (LSu)0 - (LSu)a : (LSu)a; }
static LSu ls_gcd(LSu a, LSu b) { while (b) { LSu t = a % b; a = b; b = t; } return a; }
// floor(a / b), b > 0
static LSi ls_floordiv(LSi a, LSi b) { LSi q = a / b; if (a % b != 0 && a < 0) q -= 1; return q; }

static LQ lq_int(LSi v) { LQ q; q.n = v; q.d = 1; return q; }
static LQ lq_make(LSol *S, LSi n, LSi d) {                   // d != 0
    if (d < 0) { n = ls_sub(S, 0, n); d = ls_sub(S, 0, d); }
    LSu g = ls_gcd(ls_abs(n), (LSu)d);
    if (g > 1) { n /= (LSi)g; d /= (LSi)g; }
    LQ q; q.n = n; q.d = d;
    if (S->cap || d <= 0) { S->cap = true; q.n = 0; q.d = 1; }
    return q;
}
static LQ lq_add(LSol *S, LQ a, LQ b) {
    if (a.n == 0) return b;
    if (b.n == 0) return a;
    LSi g = (LSi)ls_gcd((LSu)a.d, (LSu)b.d);
    LSi n = ls_add(S, ls_mul(S, a.n, b.d / g), ls_mul(S, b.n, a.d / g));
    return lq_make(S, n, ls_mul(S, a.d / g, b.d));
}
static LQ lq_neg(LSol *S, LQ a) { a.n = ls_sub(S, 0, a.n); return a; }
static LQ lq_sub(LSol *S, LQ a, LQ b) { return lq_add(S, a, lq_neg(S, b)); }
static LQ lq_mul(LSol *S, LQ a, LQ b) {
    if (a.n == 0 || b.n == 0) return lq_int(0);
    LSi g1 = (LSi)ls_gcd(ls_abs(a.n), (LSu)b.d), g2 = (LSi)ls_gcd(ls_abs(b.n), (LSu)a.d);
    return lq_make(S, ls_mul(S, a.n / g1, b.n / g2), ls_mul(S, a.d / g2, b.d / g1));
}
static LQ lq_div(LSol *S, LQ a, LQ b) {                     // b != 0
    return lq_mul(S, a, lq_make(S, b.d, b.n));
}
static int lq_cmp(LSol *S, LQ a, LQ b) {
    LQ d = lq_sub(S, a, b);
    return (d.n > 0) - (d.n < 0);
}

static LSi ls_from_j(LSol *S, const JInt *x) {
    if (x->n > 4) { S->cap = true; return 0; }
    LSu m = 0;
    for (int i = x->n - 1; i >= 0; i--) m = (m << 32) | x->limb[i];
    if (m >> 127) { S->cap = true; return 0; }
    return x->neg ? -(LSi)m : (LSi)m;
}
static void ls_to_j(LSi v, JInt *x) {
    memset(x, 0, sizeof *x);
    LSu m = ls_abs(v);
    x->neg = v < 0;
    while (m) { x->limb[x->n++] = (uint32_t)m; m >>= 32; }
}

// ── constraints: Σ a[j]·x_j (<= | =) b, stated by the fact `ref` ──────────────────────────────
typedef struct { LSi *a; LSi b; bool eq; JRef ref; } LCon;

static JRef ls_ref(JRefKind k, int32_t a, int32_t b) { JRef r; r.k = k; r.a = a; r.b = b; return r; }

static bool ls_con(LSol *S, const JCons *c, JRef ref, LCon *out) {
    if (!(out->a = (LSi *)ls_scratch(S, sizeof(LSi), S->nv))) return false;
    for (int32_t t = 0; t < c->nt; t++) {
        int32_t v = c->t[t].var;
        if (v < 0 || v >= S->nv) { S->cap = true; return false; }   // not a VC the parser accepts
        out->a[v] = ls_add(S, out->a[v], ls_from_j(S, &c->t[t].coef));
    }
    out->b = ls_from_j(S, &c->rhs);
    out->eq = c->op == J_EQ;
    out->ref = ref;
    return !S->cap;
}

// ── the certificate under construction: a branch's steps, in the answer's memory ──────────────
typedef struct { JStep *s; int32_t n, cap; } LSteps;

static int32_t ls_step(LSol *S, LSteps *L) {                // the new step's index, or -1
    if (L->n == L->cap) {
        int32_t nc = L->cap ? L->cap * 2 : 4;
        JStep *ns = (JStep *)ls_keep(S, sizeof(JStep), nc);
        if (!ns) return -1;
        if (L->n) memcpy(ns, L->s, sizeof(JStep) * (size_t)L->n);
        L->s = ns; L->cap = nc;
    }
    memset(&L->s[L->n], 0, sizeof(JStep));
    return L->n++;
}
// f<new> = lin Σ m[k]·ref[k] over the nonzero m, in constraint order. Returns the new f, or 0.
static int32_t ls_emit_lin(LSol *S, LSteps *L, const JRef *ref, const LSi *m, int32_t n) {
    int32_t nz = 0;
    for (int32_t k = 0; k < n; k++) if (m[k]) nz++;
    int32_t i = ls_step(S, L);
    if (i < 0) return 0;
    JMul *mul = (JMul *)ls_keep(S, sizeof(JMul), nz);
    if (!mul) return 0;
    JStep *st = &L->s[i];
    st->k = J_LIN; st->f = ++S->nf; st->mul = mul; st->nmul = nz;
    for (int32_t k = 0, q = 0; k < n; k++)
        if (m[k]) { ls_to_j(m[k], &mul[q].m); mul[q].r = ref[k]; q++; }
    return st->f;
}
static int32_t ls_emit_lin1(LSol *S, LSteps *L, JRef r, LSi m) { return ls_emit_lin(S, L, &r, &m, 1); }
static int32_t ls_emit_rnd(LSol *S, LSteps *L, int32_t of) {
    int32_t i = ls_step(S, L);
    if (i < 0) return 0;
    L->s[i].k = J_RND; L->s[i].f = ++S->nf; L->s[i].of = of;
    return L->s[i].f;
}
static bool ls_emit_close(LSol *S, LSteps *L, int32_t f) {
    int32_t i = ls_step(S, L);
    if (i < 0 || !f) return false;
    L->s[i].k = J_CLOSE; L->s[i].f = f;
    return true;
}

// ── the gcd test, as a constraint enters ──────────────────────────────────────────────────────
// Over the integers Σ a·x <= b, with g the gcd of the a, is Σ (a/g)·x <= floor(b/g): the `rnd`
// cut. A constraint with no variables is decided on its constant, and an equality whose gcd does
// not divide its constant is refuted by two cuts that meet at 0 <= -1. 0: kept (maybe tightened,
// its ref then the cut); 1: the branch is closed; -1: capacity.
static int ls_tighten(LSol *S, LSteps *L, LCon *c) {
    LSu g = 0;
    for (int32_t j = 0; j < S->nv; j++) g = ls_gcd(g, ls_abs(c->a[j]));
    if (g == 0) {
        if (c->eq ? c->b == 0 : c->b >= 0) return 0;
        int32_t f = ls_emit_lin1(S, L, c->ref, c->eq && c->b > 0 ? -1 : 1);
        return ls_emit_close(S, L, f) ? 1 : -1;
    }
    LSi gi = (LSi)g;
    if (g == 1 || c->b % gi == 0) return 0;
    if (!c->eq) {
        int32_t f = ls_emit_lin1(S, L, c->ref, 1), r = f ? ls_emit_rnd(S, L, f) : 0;
        if (!r) return -1;
        for (int32_t j = 0; j < S->nv; j++) c->a[j] /= gi;
        c->b = ls_floordiv(c->b, gi);
        c->ref = ls_ref(J_REF_F, r, 0);
        return 0;
    }
    int32_t f1 = ls_emit_lin1(S, L, c->ref, 1), f2 = f1 ? ls_emit_rnd(S, L, f1) : 0;
    int32_t f3 = f2 ? ls_emit_lin1(S, L, c->ref, -1) : 0, f4 = f3 ? ls_emit_rnd(S, L, f3) : 0;
    if (!f4) return -1;
    JRef two[2] = { ls_ref(J_REF_F, f2, 0), ls_ref(J_REF_F, f4, 0) };
    LSi one[2] = { 1, 1 };
    return ls_emit_close(S, L, ls_emit_lin(S, L, two, one, 2)) ? 1 : -1;
}

// ── the general simplex ───────────────────────────────────────────────────────────────────────
// Variables 0..n-1 are the VC's, n+k is constraint k's slack. Row r says its basic variable is
// Σ_c T[r][c]·var_c over the nonbasic ones (a basic variable's own column is zero).
typedef struct {
    int32_t n, m, w;
    LQ *T, *val, *lo, *hi;
    bool *hlo, *hhi;
    int32_t *bas, *row;
} LTab;

static bool lt_build(LSol *S, LTab *t, const LCon *cons, int32_t m) {
    t->n = S->nv; t->m = m; t->w = t->n + m;
    int64_t w = t->w;
    t->T = (LQ *)ls_scratch(S, sizeof(LQ), (int64_t)m * w);
    t->val = (LQ *)ls_scratch(S, sizeof(LQ), w);
    t->lo = (LQ *)ls_scratch(S, sizeof(LQ), w);
    t->hi = (LQ *)ls_scratch(S, sizeof(LQ), w);
    t->hlo = (bool *)ls_scratch(S, sizeof(bool), w);
    t->hhi = (bool *)ls_scratch(S, sizeof(bool), w);
    t->bas = (int32_t *)ls_scratch(S, sizeof(int32_t), m);
    t->row = (int32_t *)ls_scratch(S, sizeof(int32_t), w);
    if (S->cap) return false;
    for (int64_t i = 0; i < (int64_t)m * w; i++) t->T[i] = lq_int(0);
    for (int32_t v = 0; v < t->w; v++) { t->val[v] = t->lo[v] = t->hi[v] = lq_int(0); t->row[v] = -1; }
    for (int32_t r = 0; r < m; r++) {
        int32_t s = t->n + r;
        t->bas[r] = s; t->row[s] = r;
        for (int32_t j = 0; j < t->n; j++) t->T[r * w + j] = lq_int(cons[r].a[j]);
        t->hhi[s] = true; t->hi[s] = lq_int(cons[r].b);
        if (cons[r].eq) { t->hlo[s] = true; t->lo[s] = lq_int(cons[r].b); }
    }
    return true;
}

// Move nonbasic j so that row r's basic variable reaches `target`, then exchange the two.
static void lt_pivot(LSol *S, LTab *t, int32_t r, int32_t j, LQ target) {
    int32_t w = t->w, b = t->bas[r];
    LQ *R = &t->T[(int64_t)r * w];
    LQ a = R[j];
    LQ theta = lq_div(S, lq_sub(S, target, t->val[b]), a);
    t->val[b] = target;
    t->val[j] = lq_add(S, t->val[j], theta);
    for (int32_t r2 = 0; r2 < t->m; r2++) {
        LQ e = t->T[(int64_t)r2 * w + j];
        if (r2 == r || e.n == 0) continue;
        t->val[t->bas[r2]] = lq_add(S, t->val[t->bas[r2]], lq_mul(S, e, theta));
    }
    // row r now defines j:  j = (1/a)·b − Σ_{c≠j} (R[c]/a)·c
    LQ inv = lq_div(S, lq_int(1), a);
    for (int32_t c = 0; c < w; c++) R[c] = c == j ? lq_int(0) : lq_neg(S, lq_mul(S, R[c], inv));
    R[b] = inv;
    for (int32_t r2 = 0; r2 < t->m; r2++) {
        LQ *R2 = &t->T[(int64_t)r2 * w];
        LQ e = R2[j];
        if (r2 == r || e.n == 0) continue;
        for (int32_t c = 0; c < w; c++)
            if (c != j && R[c].n) R2[c] = lq_add(S, R2[c], lq_mul(S, e, R[c]));
        R2[j] = lq_int(0);
    }
    t->bas[r] = j; t->row[j] = r; t->row[b] = -1;
}

// 1: every bound holds; 0: row *cr cannot be repaired, its basic variable below its lower bound
// (*below) or above its upper; -1: a budget or capacity stop.
static int lt_check(LSol *S, LTab *t, int32_t *cr, bool *below) {
    for (;;) {
        if (S->cap) return -1;
        int32_t bv = -1; bool lowv = false;
        for (int32_t v = 0; v < t->w && bv < 0; v++) {
            if (t->row[v] < 0) continue;
            if (t->hlo[v] && lq_cmp(S, t->val[v], t->lo[v]) < 0) { bv = v; lowv = true; }
            else if (t->hhi[v] && lq_cmp(S, t->val[v], t->hi[v]) > 0) { bv = v; lowv = false; }
        }
        if (bv < 0) return S->cap ? -1 : 1;
        int32_t r = t->row[bv], j = -1;
        LQ *R = &t->T[(int64_t)r * t->w];
        for (int32_t c = 0; c < t->w && j < 0; c++) {
            if (t->row[c] >= 0 || R[c].n == 0) continue;
            bool raise = lowv == (R[c].n > 0);       // the way c must move to move bv the right way
            if (raise ? (!t->hhi[c] || lq_cmp(S, t->val[c], t->hi[c]) < 0)
                      : (!t->hlo[c] || lq_cmp(S, t->val[c], t->lo[c]) > 0)) j = c;
        }
        if (j < 0) { *cr = r; *below = lowv; return S->cap ? -1 : 0; }
        if (S->used.pivot >= S->limit.pivot) { S->out = true; return -1; }
        S->used.pivot++;
        lt_pivot(S, t, r, j, lowv ? t->lo[bv] : t->hi[bv]);
    }
}

// The row that cannot be repaired, as integer Farkas multipliers m[k] over the constraints. Its
// basic variable is the slack of constraint i, and every nonbasic variable it mentions is a
// slack at the bound that blocks it (an original variable has no bound, so it could have moved):
//   below (only an equality has a lower bound):  −1·con[i] + Σ_c T[r][c]·con[c−n]
//   above:                                       +1·con[i] − Σ_c T[r][c]·con[c−n]
// The variables cancel, since row i IS Σ_c T[r][c]·row[c−n], and the constant is the amount by
// which the bound is missed, negative. A multiplier on an inequality is never negative: a slack
// below an upper bound with the wrong sign is one that could have moved.
static bool lt_explain(LSol *S, const LTab *t, int32_t r, bool below, LSi *m) {
    int32_t b = t->bas[r], n = t->n;
    const LQ *R = &t->T[(int64_t)r * t->w];
    if (b < n) { S->cap = true; return false; }
    LQ *q = (LQ *)ls_scratch(S, sizeof(LQ), t->m);
    if (!q) return false;
    for (int32_t k = 0; k < t->m; k++) q[k] = lq_int(0);
    q[b - n] = lq_int(below ? -1 : 1);
    for (int32_t c = 0; c < t->w; c++) {
        if (!R[c].n) continue;
        if (c < n) { S->cap = true; return false; }   // an unbounded variable: not a conflict
        q[c - n] = below ? R[c] : lq_neg(S, R[c]);
    }
    LSi l = 1;
    for (int32_t k = 0; k < t->m; k++)
        if (q[k].n) l = ls_mul(S, l / (LSi)ls_gcd((LSu)l, (LSu)q[k].d), q[k].d);
    for (int32_t k = 0; k < t->m; k++) m[k] = q[k].n ? ls_mul(S, q[k].n, l / q[k].d) : 0;
    return !S->cap;
}

// ── the search ────────────────────────────────────────────────────────────────────────────────
typedef enum { LS_UNSAT, LS_SAT, LS_STOP } LRes;

static bool ls_holds(LSol *S, const JCons *c, const LSi *x) {
    LSi s = 0;
    for (int32_t t = 0; t < c->nt; t++) s = ls_add(S, s, ls_mul(S, ls_from_j(S, &c->t[t].coef), x[c->t[t].var]));
    LSi b = ls_from_j(S, &c->rhs);
    return c->op == J_EQ ? s == b : c->op == J_LE ? s <= b : s != b;
}

// One node: cons[0..fresh) entered at an ancestor, cons[fresh..ncons) enter here. On LS_UNSAT the
// node's refutation is in L; on LS_SAT the integer model is in `model`.
static LRes ls_node(LSol *S, const JVc *vc, LCon *cons, int32_t ncons, int32_t fresh,
                    LSteps *L, LSi *model) {
    char *mark = S->lo;
    for (int32_t k = fresh; k < ncons; k++) {
        int r = ls_tighten(S, L, &cons[k]);
        if (r < 0 || S->cap) return LS_STOP;
        if (r == 1) { S->lo = mark; return LS_UNSAT; }
    }
    char *tabmark = S->lo;
    LTab t;
    if (!lt_build(S, &t, cons, ncons)) return LS_STOP;
    int32_t cr = 0; bool below = false;
    int st = lt_check(S, &t, &cr, &below);
    if (st < 0) return LS_STOP;
    if (st == 0) {
        LSi *m = (LSi *)ls_scratch(S, sizeof(LSi), ncons);
        JRef *ref = (JRef *)ls_scratch(S, sizeof(JRef), ncons);
        if (!m || !ref || !lt_explain(S, &t, cr, below, m)) return LS_STOP;
        for (int32_t k = 0; k < ncons; k++) ref[k] = cons[k].ref;
        if (!ls_emit_close(S, L, ls_emit_lin(S, L, ref, m, ncons))) return LS_STOP;
        S->lo = mark; return LS_UNSAT;
    }
    // The rationals agree. The first variable that is not an integer is split at its floor.
    int32_t j = -1; LSi fl = 0;
    for (int32_t v = 0; v < t.n && j < 0; v++)
        if (t.val[v].d != 1) { j = v; fl = ls_floordiv(t.val[v].n, t.val[v].d); }
    LSi *x = (LSi *)ls_scratch(S, sizeof(LSi), S->nv);
    if (!x) return LS_STOP;
    for (int32_t v = 0; v < t.n; v++) x[v] = t.val[v].n;
    if (j >= 0 && S->cone) {                         // a cone's: the rational model times its denominators' lcm
        LSi l = 1;
        for (int32_t v = 0; v < t.n; v++) l = ls_mul(S, l / (LSi)ls_gcd((LSu)l, (LSu)t.val[v].d), t.val[v].d);
        for (int32_t v = 0; v < t.n; v++) x[v] = ls_mul(S, t.val[v].n, l / t.val[v].d);
        if (S->cap) return LS_STOP;
        memcpy(model, x, sizeof(LSi) * (size_t)S->nv);
        return LS_SAT;
    }
    if (j >= 0) {
        S->lo = tabmark;
        if (S->used.node >= S->limit.node) { S->out = true; return LS_STOP; }
        S->used.node++;
        int32_t sk = ++S->ns, si = ls_step(S, L);
        JTerm *term = (JTerm *)ls_keep(S, sizeof(JTerm), 1);
        JSteps *br = (JSteps *)ls_keep(S, sizeof(JSteps), 2);
        LCon *c2 = (LCon *)ls_scratch(S, sizeof(LCon), (int64_t)ncons + 1);
        if (si < 0 || !term || !br || !c2) return LS_STOP;
        term->var = j; ls_to_j(1, &term->coef);
        JStep *sp = &L->s[si];
        sp->k = J_SPLIT; sp->s = sk; sp->br = br; sp->nbr = 2;
        sp->split.t = term; sp->split.nt = 1; sp->split.op = J_LE; ls_to_j(fl, &sp->split.rhs);
        memcpy(c2, cons, sizeof(LCon) * (size_t)ncons);
        if (!(c2[ncons].a = (LSi *)ls_scratch(S, sizeof(LSi), S->nv))) return LS_STOP;
        c2[ncons].ref = ls_ref(J_REF_S, sk, 0);
        for (int side = 0; side < 2; side++) {         // le: x_j <= fl;  gt: −x_j <= −fl − 1
            c2[ncons].a[j] = side ? -1 : 1;
            c2[ncons].b = side ? ls_sub(S, ls_sub(S, 0, fl), 1) : fl;
            c2[ncons].eq = false;
            LSteps sub; memset(&sub, 0, sizeof sub);
            LRes r = ls_node(S, vc, c2, ncons + 1, ncons, &sub, model);
            if (r != LS_UNSAT) return r;
            br[side].s = sub.s; br[side].n = sub.n;
        }
        S->lo = mark; return LS_UNSAT;
    }
    // An integer model of the conjunction. A disjunctive hypothesis it violates is split.
    int32_t hd = -1;
    for (int32_t h = 0; h < vc->nhyps && hd < 0; h++) {
        const JHyp *H = &vc->hyps[h];
        if (!H->disj || (S->on && !S->on[h])) continue;
        bool any = false;
        for (int32_t d = 0; d < H->ndis && !any; d++) {
            bool all = true;
            for (int32_t q = 0; q < H->dis[d].nc && all; q++) all = ls_holds(S, &H->dis[d].c[q], x);
            any = all;
        }
        if (S->cap) return LS_STOP;
        if (!any) hd = h;
    }
    if (hd < 0) { memcpy(model, x, sizeof(LSi) * (size_t)S->nv); return LS_SAT; }
    S->lo = tabmark;
    const JHyp *H = &vc->hyps[hd];
    int32_t sk = ++S->ns, ci = ls_step(S, L);
    JSteps *br = (JSteps *)ls_keep(S, sizeof(JSteps), H->ndis);
    if (ci < 0 || !br) return LS_STOP;
    L->s[ci].k = J_CASE; L->s[ci].s = sk; L->s[ci].hyp = hd + 1; L->s[ci].br = br; L->s[ci].nbr = H->ndis;
    for (int32_t d = 0; d < H->ndis; d++) {
        char *dmark = S->lo;
        if (S->used.leaf >= S->limit.leaf) { S->out = true; return LS_STOP; }
        S->used.leaf++;
        const JConj *D = &H->dis[d];
        LCon *c2 = (LCon *)ls_scratch(S, sizeof(LCon), (int64_t)ncons + D->nc);
        if (!c2) return LS_STOP;
        memcpy(c2, cons, sizeof(LCon) * (size_t)ncons);
        for (int32_t q = 0; q < D->nc; q++)
            if (!ls_con(S, &D->c[q], ls_ref(J_REF_SC, sk, q + 1), &c2[ncons + q])) return LS_STOP;
        LSteps sub; memset(&sub, 0, sizeof sub);
        LRes r = ls_node(S, vc, c2, ncons + D->nc, ncons, &sub, model);
        if (r != LS_UNSAT) return r;
        br[d].s = sub.s; br[d].n = sub.n;
        S->lo = dmark;
    }
    S->lo = mark; return LS_UNSAT;
}

// One search over the hypotheses `on` names (NULL: every one). A reference keeps its hypothesis's
// index in the full list, so a certificate over a slice is the full VC's.
static JAnswer ls_solve(const JVc *vc, const uint8_t *on, bool cone, JWork limit, void *mem, size_t size) {
    JAnswer A; memset(&A, 0, sizeof A);
    LSol S; memset(&S, 0, sizeof S);
    S.lo = (char *)mem; S.hi = (char *)mem + size; S.limit = limit; S.nv = vc->nvars; S.on = on; S.cone = cone;
    int32_t nc = 1;
    for (int32_t h = 0; h < vc->nhyps; h++) if (!vc->hyps[h].disj && (!on || on[h])) nc++;
    LCon *cons = (LCon *)ls_scratch(&S, sizeof(LCon), nc);
    LSi *model = (LSi *)ls_scratch(&S, sizeof(LSi), S.nv);
    LRes r = LS_STOP;
    LSteps body; memset(&body, 0, sizeof body);
    if (cons && model) {
        int32_t k = 0;
        for (int32_t h = 0; h < vc->nhyps && !S.cap; h++)
            if (!vc->hyps[h].disj && (!on || on[h])) ls_con(&S, &vc->hyps[h].cons, ls_ref(J_REF_H, h + 1, 0), &cons[k++]);
        // g, the goal's negation: `-lin <= -c - 1` for `lin <= c`, `lin = c` for `lin != c`
        if (!S.cap && ls_con(&S, &vc->goal, ls_ref(J_REF_G, 0, 0), &cons[k])) {
            if (vc->goal.op == J_LE) {
                for (int32_t j = 0; j < S.nv; j++) cons[k].a[j] = ls_sub(&S, 0, cons[k].a[j]);
                cons[k].b = ls_sub(&S, ls_sub(&S, 0, cons[k].b), 1);
            } else cons[k].eq = true;
        }
        if (!S.cap) r = ls_node(&S, vc, cons, nc, 0, &body, model);
    }
    if (r == LS_SAT) {
        JInt *val = (JInt *)ls_keep(&S, sizeof(JInt), S.nv);
        if (val) for (int32_t j = 0; j < S.nv; j++) ls_to_j(model[j], &val[j]);
        if (val) { A.k = J_ANS_MODEL; A.model.id = vc->id; A.model.val = val; A.model.n = S.nv; }
        else r = LS_STOP;
    } else if (r == LS_UNSAT) {
        A.k = J_ANS_CERT; A.cert.id = vc->id; A.cert.body.s = body.s; A.cert.body.n = body.n;
    }
    if (r == LS_STOP) A.k = S.cap || !S.out ? J_ANS_CAPACITY : J_ANS_BUDGET;
    A.used = S.used;
    return A;
}

// ── the sliced retry (PE.13) ──────────────────────────────────────────────────────────────────
// A pivot through a fact the proof never needs can leave 128 bits where the proof's own numbers
// are small. NestedRecursion's rec1 is one: its post VC stops at pivot 13 on a product past 127
// bits (whose sum is back under 125), through the contract rows `2147483649*ret - i <= 2^31`,
// while five small facts refute it. So a VC that stops on CAPACITY is asked again on its slices:
// S_1, the hypotheses naming a variable of the goal; S_2, those naming a variable of S_1's; and so
// on, until a round adds none (the goal's connected part) or the slice is the whole VC, whose
// answer is known. rec1's S_2, 20 of its 34 facts, is refuted in 10 pivots. A disjunction names
// every variable of every disjunct. A goal with no variable has no slice.
//
// Only a CERTIFICATE from a slice is answered, and it is the full VC's as it stands: a slice is a
// mask over the full list, so every `h<k>` and every `case` names its hypothesis by its index in
// the FULL VC, and the kernel, which is given the full VC, checks it there (a refutation need not
// cite every fact). A slice's MODEL is no model of the full VC: a dropped fact may be false at it,
// or contradict the rest in a part the goal never reaches. A slice's BUDGET or CAPACITY says
// nothing new. Each goes on to the next slice; when none certifies, the answer is the full VC's
// own CAPACITY. The attempts share the call's one budget, `used` their sum, and the slices are a
// function of the VC alone, so the answer stays deterministic.
static bool ls_names(const JCons *c, const uint8_t *seen) {
    for (int32_t t = 0; t < c->nt; t++) if (seen[c->t[t].var]) return true;
    return false;
}
static void ls_see(const JCons *c, uint8_t *seen) {
    for (int32_t t = 0; t < c->nt; t++) seen[c->t[t].var] = 1;
}
static bool ls_in_range(const JCons *c, int32_t nv) {
    for (int32_t t = 0; t < c->nt; t++) if (c->t[t].var < 0 || c->t[t].var >= nv) return false;
    return true;
}
static uint64_t ls_left(uint64_t limit, uint64_t used) { return used < limit ? limit - used : 0; }

// Solve `vc` within `limit`, in the caller's region of `size` bytes. Every pointer in the answer
// points into that region.
static JAnswer lia_solve(const JVc *vc, JWork limit, void *mem, size_t size) {
    JAnswer A = ls_solve(vc, NULL, false, limit, mem, size);
    if (A.k != J_ANS_CAPACITY || vc->goal.nt == 0 || vc->nhyps == 0 || vc->nvars <= 0) return A;
    // a VC the search refused as malformed (a variable out of range) is not sliced
    bool ok = ls_in_range(&vc->goal, vc->nvars);
    for (int32_t h = 0; h < vc->nhyps && ok; h++) {
        const JHyp *H = &vc->hyps[h];
        if (!H->disj) ok = ls_in_range(&H->cons, vc->nvars);
        else for (int32_t d = 0; d < H->ndis && ok; d++)
            for (int32_t q = 0; q < H->dis[d].nc && ok; q++) ok = ls_in_range(&H->dis[d].c[q], vc->nvars);
    }
    // the masks at the region's start (the failed search kept nothing), the searches after them
    size_t mk = ((size_t)vc->nhyps + (size_t)vc->nvars + 15u) & ~(size_t)15u;
    if (!ok || size < mk + 4096) return A;
    uint8_t *on = (uint8_t *)mem, *seen = on + vc->nhyps;
    memset(mem, 0, mk);
    ls_see(&vc->goal, seen);
    JWork used = A.used;
    for (int32_t in = 0;;) {
        int32_t added = 0;
        for (int32_t h = 0; h < vc->nhyps; h++) {      // this round: what names a variable seen before it
            const JHyp *H = &vc->hyps[h];
            bool names = false;
            if (on[h]) continue;
            if (!H->disj) names = ls_names(&H->cons, seen);
            else for (int32_t d = 0; d < H->ndis && !names; d++)
                for (int32_t q = 0; q < H->dis[d].nc && !names; q++) names = ls_names(&H->dis[d].c[q], seen);
            if (names) { on[h] = 2; added++; }
        }
        if (!added) break;
        for (int32_t h = 0; h < vc->nhyps; h++) {      // its variables are seen by the next round
            const JHyp *H = &vc->hyps[h];
            if (on[h] != 2) continue;
            on[h] = 1;
            if (!H->disj) ls_see(&H->cons, seen);
            else for (int32_t d = 0; d < H->ndis; d++)
                for (int32_t q = 0; q < H->dis[d].nc; q++) ls_see(&H->dis[d].c[q], seen);
        }
        in += added;
        if (in == vc->nhyps) break;
        JWork left = { ls_left(limit.pivot, used.pivot), ls_left(limit.node, used.node), ls_left(limit.leaf, used.leaf) };
        if (!left.pivot || !left.node || !left.leaf) break;
        JAnswer B = ls_solve(vc, on, false, left, (char *)mem + mk, size - mk);
        used.pivot += B.used.pivot; used.node += B.used.node; used.leaf += B.used.leaf;
        if (B.k == J_ANS_CERT) { B.used = used; return B; }
    }
    A.used = used;
    return A;
}

// ── a cone's model (s2, PE.13): the rational answer, scaled ────────────────────────────────────
// For a system CLOSED UNDER SCALING by any k >= 1: conjunctions only, every `<=` constant at most 0,
// every `=` constant 0, and the goal `0 <= -1` (are the hypotheses satisfiable?). A rational model
// times the lcm of its denominators is then an integer model, so one rational search answers, with
// no branch and bound. rank.h's sample LP (rk_cegis) is such a system, and on Gothenburg_v2-1's
// samples lia_solve's branch and bound overflowed within a few nodes where the rational search needs
// one pivot. A refutation is the ordinary certificate. The scaled model keeps every row: the gcd
// test only tightens a row (its rational model satisfies the original), and a row whose constant is
// at most 0 holds at k times a point where it holds. A scaling past 128 bits is CAPACITY. A system
// that is not closed under scaling is not asked: CAPACITY. (Its one caller checks each candidate
// on every path anyway, and solver_harness.py evaluates every model.)
static bool ls_nonpos(const JInt *x) { return x->n == 0 || x->neg; }
static JAnswer lia_cone(const JVc *vc, JWork limit, void *mem, size_t size) {
    JAnswer A; memset(&A, 0, sizeof A); A.k = J_ANS_CAPACITY;
    bool cone = vc->goal.nt == 0 && vc->goal.op == J_LE && vc->goal.rhs.neg && vc->goal.rhs.n == 1 && vc->goal.rhs.limb[0] == 1;
    for (int32_t h = 0; h < vc->nhyps && cone; h++) {
        const JHyp *H = &vc->hyps[h];
        if (H->disj) cone = false;
        else if (H->cons.op == J_LE) cone = ls_nonpos(&H->cons.rhs);
        else cone = H->cons.op == J_EQ && H->cons.rhs.n == 0;
        for (int32_t t = 0; cone && t < H->cons.nt; t++) cone = H->cons.t[t].var >= 0 && H->cons.t[t].var < vc->nvars;
    }
    if (!cone) return A;
    return ls_solve(vc, NULL, true, limit, mem, size);
}

// ── optimization (PE.2): the largest value of a linear objective over a VC's hypotheses ─────────
// The answer a hull claim at a join needs: a bound b, an ordinary certificate of the hypotheses
// with goal `obj <= b` (the kernel checks it like any other), and a model of the hypotheses with
// obj = b (the judge evaluates it like any other), which shows the bound is attained, so the hull
// is exact. A minimum is the maximum of −obj.
//
// It is found with the decision procedure alone, so nothing new is trusted and nothing new is
// searched: a model gives a lower bound L, a certificate of `obj <= U` an upper bound, the upper
// bound is first found by doubling steps above L, and the gap is closed by halving. Each query is
// one lia_solve; the work is summed over all of them against one limit.
typedef enum { LIA_MAX_BOUND, LIA_MAX_EMPTY, LIA_MAX_NONE, LIA_MAX_STOP } LMaxKind;
typedef struct {
    LMaxKind k;      // BOUND: b, vc, cert, model. EMPTY: the hypotheses alone are refuted (vc's
                     // goal is 0 <= -1, cert refutes it). NONE: obj passes every query up to 2^120.
                     // STOP: a budget or capacity ran out (stop says which).
    JInt b;
    JVc vc;          // what cert refutes: the hypotheses with goal `obj <= b` (the goal's terms are
                     // the caller's, which must outlive the answer)
    JCert cert;
    JModel model;
    JAnswerKind stop;
    JWork used;
} LMax;

static LSi lmax_eval(LSol *S, const JTerm *t, int32_t nt, const JModel *m) {
    LSi s = 0;
    for (int32_t i = 0; i < nt; i++)
        s = ls_add(S, s, ls_mul(S, ls_from_j(S, &t[i].coef), ls_from_j(S, &m->val[t[i].var])));
    return s;
}

// `hyps` supplies the variables and hypotheses (its goal is not read). `obj` is a canonical sum
// over its variables: ascending, each once, coefficients nonzero.
static LMax lia_maximize(const JVc *hyps, const JTerm *obj, int32_t nt, JWork limit, void *mem, size_t size) {
    LMax R; memset(&R, 0, sizeof R);
    LSol N; memset(&N, 0, sizeof N);                        // checked arithmetic only
    int32_t nv = hyps->nvars;
    // The region: the best model's values first, then the final query's region, then the scratch
    // every other query reuses.
    size_t keep = ((sizeof(JInt) * (size_t)(nv > 0 ? nv : 1)) + 15u) & ~(size_t)15u;
    if (size < keep + 2 * 4096) { R.k = LIA_MAX_STOP; R.stop = J_ANS_CAPACITY; return R; }
    JInt *best = (JInt *)mem;
    char *fin = (char *)mem + keep;
    size_t fsz = (size - keep) / 2;
    char *wrk = fin + fsz;
    size_t wsz = size - keep - fsz;
    JVc q = *hyps;
    memset(&q.goal, 0, sizeof q.goal);
    q.goal.op = J_LE;
    ls_to_j(-1, &q.goal.rhs);                                // first: are the hypotheses satisfiable?
    JWork left = limit;
    #define LMAX_ASK(region, rsz) do {                                                        \
        A = lia_solve(&q, left, (region), (rsz));                                               \
        R.used.pivot += A.used.pivot; R.used.node += A.used.node; R.used.leaf += A.used.leaf;   \
        left.pivot = limit.pivot - R.used.pivot; left.node = limit.node - R.used.node;          \
        left.leaf = limit.leaf - R.used.leaf;                                                   \
    } while (0)
    JAnswer A;
    LMAX_ASK(wrk, wsz);
    if (A.k == J_ANS_CERT) {
        LMAX_ASK(fin, fsz);                                  // the same query, kept
        if (A.k != J_ANS_CERT) { R.k = LIA_MAX_STOP; R.stop = J_ANS_CAPACITY; return R; }
        R.k = LIA_MAX_EMPTY; R.vc = q; R.cert = A.cert; return R;
    }
    if (A.k != J_ANS_MODEL) { R.k = LIA_MAX_STOP; R.stop = A.k; return R; }
    memcpy(best, A.model.val, sizeof(JInt) * (size_t)nv);
    JModel bm; bm.id = hyps->id; bm.val = best; bm.n = nv;
    LSi lo = lmax_eval(&N, obj, nt, &bm), hi = 0, step = 1;
    bool have_hi = false;
    const LSi lim = (LSi)1 << 120;
    q.goal.t = (JTerm *)obj; q.goal.nt = nt;
    while (!N.cap) {
        LSi ask;
        if (!have_hi) {                                      // doubling steps above the best model
            if (lo > lim - step) { R.k = LIA_MAX_NONE; R.model = bm; return R; }
            ask = lo + step - 1;                             // `obj <= lo + step - 1`
            step = step < lim ? step * 2 : step;
        } else {
            if (lo == hi) break;
            ask = lo + (hi - lo) / 2;                        // lo <= ask < hi
        }
        ls_to_j(ask, &q.goal.rhs);
        LMAX_ASK(wrk, wsz);
        if (A.k == J_ANS_CERT) { hi = ask; have_hi = true; if (hi < lo) { N.cap = true; break; } }
        else if (A.k == J_ANS_MODEL) {
            LSi v = lmax_eval(&N, obj, nt, &A.model);
            if (v <= ask) { N.cap = true; break; }           // a model must violate the goal
            memcpy(best, A.model.val, sizeof(JInt) * (size_t)nv);
            lo = v;
            if (have_hi && lo > hi) { N.cap = true; break; }
        } else { R.k = LIA_MAX_STOP; R.stop = A.k; return R; }
    }
    if (N.cap) { R.k = LIA_MAX_STOP; R.stop = J_ANS_CAPACITY; return R; }
    ls_to_j(hi, &q.goal.rhs);
    LMAX_ASK(fin, fsz);                                      // the certificate of `obj <= b`, kept
    #undef LMAX_ASK
    if (A.k != J_ANS_CERT) { R.k = LIA_MAX_STOP; R.stop = A.k == J_ANS_MODEL ? J_ANS_CAPACITY : A.k; return R; }
    R.k = LIA_MAX_BOUND; ls_to_j(hi, &R.b); R.vc = q; R.cert = A.cert; R.model = bm;
    return R;
}

#endif // LAIN_SOLVER_LIA_H
