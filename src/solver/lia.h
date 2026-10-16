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
typedef struct {
    char *lo, *hi;              // scratch below lo is in use; the answer lives from hi up
    bool cap;                   // a number left the type, or the region is full
    bool ovf;                   // ...and it was a number (the wide retry's cue)
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

// ── numbers: checked 128-bit integers ───────────────────────────────────────────────────────────
static void ls_ovf(LSol *S) { S->cap = true; S->ovf = true; }
static LSi ls_add(LSol *S, LSi a, LSi b) { LSi r; if (__builtin_add_overflow(a, b, &r)) { ls_ovf(S); return 0; } return r; }
static LSi ls_sub(LSol *S, LSi a, LSi b) { LSi r; if (__builtin_sub_overflow(a, b, &r)) { ls_ovf(S); return 0; } return r; }
static LSi ls_mul(LSol *S, LSi a, LSi b) { LSi r; if (__builtin_mul_overflow(a, b, &r)) { ls_ovf(S); return 0; } return r; }
static LSu ls_abs(LSi a) { return a < 0 ? (LSu)0 - (LSu)a : (LSu)a; }
static LSu ls_gcd(LSu a, LSu b) { while (b) { LSu t = a % b; a = b; b = t; } return a; }
// the gcd of |a| and |b| as an LSi: 2^127 (both multiples of it) does not fit
static LSi ls_gcdn(LSol *S, LSi a, LSi b) { LSu g = ls_gcd(ls_abs(a), ls_abs(b)); if (g >> 127) { ls_ovf(S); return 1; } return (LSi)g; }
// floor(a / b), b > 0
static LSi ls_floordiv(LSi a, LSi b) { LSi q = a / b; if (a % b != 0 && a < 0) q -= 1; return q; }

static LSi ls_from_j(LSol *S, const JInt *x) {
    if (x->n > 4) { ls_ovf(S); return 0; }
    LSu m = 0;
    for (int i = x->n - 1; i >= 0; i--) m = (m << 32) | x->limb[i];
    if (m >> 127) { ls_ovf(S); return 0; }
    return x->neg ? -(LSi)m : (LSi)m;
}
static void ls_to_j(LSi v, JInt *x) {
    memset(x, 0, sizeof *x);
    LSu m = ls_abs(v);
    x->neg = v < 0;
    while (m) { x->limb[x->n++] = (uint32_t)m; m >>= 32; }
}

static JRef ls_ref(JRefKind k, int32_t a, int32_t b) { JRef r; r.k = k; r.a = a; r.b = b; return r; }

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
typedef enum { LS_UNSAT, LS_SAT, LS_STOP } LRes;

// ── the search at 128 bits (lia_core.h): the solver ─────────────────────────────────────────────
#define N_T LSi
#define CN(x) x
#define N_I(v) ((LSi)(v))
#define N_SGN(a) (((a) > 0) - ((a) < 0))
#define N_CMP(a, b) (((a) > (b)) - ((a) < (b)))
#define N_ADD(S, a, b) ls_add((S), (a), (b))
#define N_SUB(S, a, b) ls_sub((S), (a), (b))
#define N_MUL(S, a, b) ls_mul((S), (a), (b))
#define N_NEG(S, a) ls_sub((S), 0, (a))
#define N_GCD(S, a, b) ls_gcdn((S), (a), (b))
#define N_DIV(a, b) ((a) / (b))
#define N_DIVIDES(a, b) ((a) % (b) == 0)
#define N_FLOORDIV(S, a, b) ls_floordiv((a), (b))
#define N_FROMJ(S, x) ls_from_j((S), (x))
#define N_TOJ(v, x) ls_to_j((v), (x))
#include "solver/lia_core.h"
#undef N_T
#undef CN
#undef N_I
#undef N_SGN
#undef N_CMP
#undef N_ADD
#undef N_SUB
#undef N_MUL
#undef N_NEG
#undef N_GCD
#undef N_DIV
#undef N_DIVIDES
#undef N_FLOORDIV
#undef N_FROMJ
#undef N_TOJ

// ── the search at 512 bits (lia_core.h again): the wide retry (s3, PE.13) ──────────────────────
// LSW (lsw.h) is JInt's own form, so its bound is the kernel's, J_LIMBS: the checks below and the
// kernel's refuse at the same place. These wrappers turn its refusals into the search's CAPACITY.
#include "solver/lsw.h"
static LSW lw_add(LSol *S, LSW a, LSW b) { LSW r; if (!lw_add_to(&r, a, b)) { ls_ovf(S); return lw_i(0); } return r; }
static LSW lw_sub(LSol *S, LSW a, LSW b) { LSW r; if (!lw_add_to(&r, a, lw_neg(b))) { ls_ovf(S); return lw_i(0); } return r; }
static LSW lw_mul(LSol *S, LSW a, LSW b) { LSW r; if (!lw_mul_to(&r, a, b)) { ls_ovf(S); return lw_i(0); } return r; }
static LSW lw_floordiv(LSol *S, LSW a, LSW b) { LSW r; if (!lw_floordiv_to(&r, a, b)) { ls_ovf(S); return lw_i(0); } return r; }
static LSW lw_from_j(LSol *S, const JInt *x) {
    if (x->n > J_LIMBS || x->neg > 1) { ls_ovf(S); return lw_i(0); }
    LSW r = *x; lw_norm(&r); return r;
}
static void lw_to_j(LSW v, JInt *x) { *x = v; }
#define N_T LSW
#define CN(x) x##_w
#define N_I(v) lw_i(v)
#define N_SGN(a) lw_sgn(a)
#define N_CMP(a, b) lw_cmp((a), (b))
#define N_ADD(S, a, b) lw_add((S), (a), (b))
#define N_SUB(S, a, b) lw_sub((S), (a), (b))
#define N_MUL(S, a, b) lw_mul((S), (a), (b))
#define N_NEG(S, a) ((void)(S), lw_neg(a))
#define N_GCD(S, a, b) lw_gcd((a), (b))
#define N_DIV(a, b) lw_div((a), (b))
#define N_DIVIDES(a, b) lw_divides((a), (b))
#define N_FLOORDIV(S, a, b) lw_floordiv((S), (a), (b))
#define N_FROMJ(S, x) lw_from_j((S), (x))
#define N_TOJ(v, x) lw_to_j((v), (x))
#include "solver/lia_core.h"
#undef N_T
#undef CN
#undef N_I
#undef N_SGN
#undef N_CMP
#undef N_ADD
#undef N_SUB
#undef N_MUL
#undef N_NEG
#undef N_GCD
#undef N_DIV
#undef N_DIVIDES
#undef N_FLOORDIV
#undef N_FROMJ
#undef N_TOJ

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

// The slices S_1, S_2, ... of a VC whose full search stopped on CAPACITY: a certificate, or
// CAPACITY. `used` carries the work spent so far and gains the slices'.
static JAnswer ls_sliced(const JVc *vc, JWork limit, void *mem, size_t size, JWork *used) {
    JAnswer A; memset(&A, 0, sizeof A); A.k = J_ANS_CAPACITY;
    if (vc->goal.nt == 0 || vc->nhyps == 0 || vc->nvars <= 0) return A;
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
        JWork left = { ls_left(limit.pivot, used->pivot), ls_left(limit.node, used->node), ls_left(limit.leaf, used->leaf) };
        if (!left.pivot || !left.node || !left.leaf) break;
        JAnswer B = ls_solve(vc, on, false, left, (char *)mem + mk, size - mk, NULL);
        used->pivot += B.used.pivot; used->node += B.used.node; used->leaf += B.used.leaf;
        if (B.k == J_ANS_CERT) return B;
    }
    return A;
}

// ── the wide retry (s3, PE.13): the same search at 512 bits ─────────────────────────────────
// A search can overflow 128 bits in the middle while its answer is small: rec1's post VC 0x7c
// stores nothing past 125 bits and needs its whole VC (no slice holds its proof); udiv(x u64)'s
// bound checks store 129 bits; Gothenburg-1's sample LP 182. So a search that stopped because a
// NUMBER left 128 bits (not the region, not the budget) is run again by lia_core.h's wide instance,
// with the budget left, at most LS_WIDE_WORK tableau entry updates (below). It takes the same steps, so it gives
// the answer an unbounded search gives.
//
// The kernel and the judge are 512 bits too, and that binds what the wide search may answer: the
// kernel sums each `lin` step multiplier times fact in KInt and refuses one that leaves J_LIMBS
// limbs, and the judge evaluates a model's sums the same way. Either refusal is a `solver-bug`
// record. At 128 bits neither can happen; at 512 they can (a multiplier near 2^255 times a split's
// constant near 2^255). So before a wide answer is given, the solver follows it with LSW, whose
// add and mul refuse exactly where the kernel's do, in the order the kernel and the judge go: a
// certificate step by step (lw_cert_fits), a model sum by sum (lw_model_fits). One that would not
// fit is CAPACITY. This is the solver's own arithmetic; the kernel still checks every answer.
typedef struct { LSW *a; LSW b; } LwFact;
typedef struct {
    const JVc *v; char *cur, *end;
    LwFact *f; int32_t nf;                    // derived facts by number
    LwFact *s; const JConj **sc; int32_t ns;  // split facts, or the case's chosen disjunct
    LwFact g, tmp;
} LwEval;
static void *lw_bump(LwEval *E, size_t n) {
    uintptr_t at = ((uintptr_t)E->cur + 15u) & ~(uintptr_t)15u;
    if (at > (uintptr_t)E->end || n > (size_t)((uintptr_t)E->end - at)) return NULL;
    E->cur = (char *)at + n; memset((void *)at, 0, n);
    return (void *)at;
}
static bool lw_fact_new(LwEval *E, LwFact *x) {
    x->a = (LSW *)lw_bump(E, sizeof(LSW) * (size_t)(E->v->nvars ? E->v->nvars : 1));
    x->b = lw_i(0);
    return x->a != NULL;
}
static bool lw_fact_of(LwEval *E, const JCons *c, LwFact *x) {   // a constraint as written, dense
    for (int32_t v = 0; v < E->v->nvars; v++) x->a[v] = lw_i(0);
    for (int32_t i = 0; i < c->nt; i++) {
        if (c->t[i].var < 0 || c->t[i].var >= E->v->nvars) return false;
        x->a[c->t[i].var] = c->t[i].coef;
    }
    x->b = c->rhs;
    return true;
}
static const LwFact *lw_ref(LwEval *E, const JRef *r) {
    switch (r->k) {
        case J_REF_G: return &E->g;
        case J_REF_H:
            if (r->a < 1 || r->a > E->v->nhyps || E->v->hyps[r->a - 1].disj) return NULL;
            return lw_fact_of(E, &E->v->hyps[r->a - 1].cons, &E->tmp) ? &E->tmp : NULL;
        case J_REF_F: return r->a >= 1 && r->a < E->nf && E->f[r->a].a ? &E->f[r->a] : NULL;
        case J_REF_S: return r->a >= 1 && r->a < E->ns && E->s[r->a].a && !E->sc[r->a] ? &E->s[r->a] : NULL;
        case J_REF_SC: {
            if (r->a < 1 || r->a >= E->ns || !E->sc[r->a]) return NULL;
            const JConj *d = E->sc[r->a];
            if (r->b < 1 || r->b > d->nc) return NULL;
            return lw_fact_of(E, &d->c[r->b - 1], &E->tmp) ? &E->tmp : NULL;
        }
    }
    return NULL;
}
// true: every sum of this branch fits, as the kernel will compute it
static bool lw_steps_fit(LwEval *E, const JSteps *br) {
    for (int32_t i = 0; br && i < br->n; i++) {
        const JStep *st = &br->s[i];
        if (st->k == J_LIN || st->k == J_RND) {
            if (st->f < 1 || st->f >= E->nf) return false;
            LwFact r;
            if (!lw_fact_new(E, &r)) return false;
            if (st->k == J_LIN) {
                for (int32_t j = 0; j < st->nmul; j++) {
                    const LwFact *x = lw_ref(E, &st->mul[j].r);
                    if (!x) return false;
                    const LSW mu = st->mul[j].m;
                    for (int32_t v = 0; v < E->v->nvars; v++) {
                        if (!x->a[v].n) continue;
                        LSW p; if (!lw_mul_to(&p, mu, x->a[v]) || !lw_add_to(&r.a[v], r.a[v], p)) return false;
                    }
                    LSW p; if (!lw_mul_to(&p, mu, x->b) || !lw_add_to(&r.b, r.b, p)) return false;
                }
            } else {
                if (st->of < 1 || st->of >= E->nf || !E->f[st->of].a) return false;
                const LwFact *x = &E->f[st->of];
                LSW d = lw_i(0);
                for (int32_t v = 0; v < E->v->nvars; v++) if (x->a[v].n) d = lw_gcd(d, x->a[v]);
                if (!d.n) return false;
                for (int32_t v = 0; v < E->v->nvars; v++) r.a[v] = x->a[v].n ? lw_div(x->a[v], d) : lw_i(0);
                if (!lw_floordiv_to(&r.b, x->b, d)) return false;
            }
            E->f[st->f] = r;
        } else if (st->k == J_CLOSE) return true;
        else if (st->k == J_SPLIT) {
            if (st->s < 1 || st->s >= E->ns || st->nbr != 2) return false;
            LwFact le, gt;
            if (!lw_fact_new(E, &le) || !lw_fact_new(E, &gt) || !lw_fact_of(E, &st->split, &le)) return false;
            for (int32_t v = 0; v < E->v->nvars; v++) gt.a[v] = lw_neg(le.a[v]);
            if (!lw_add_to(&gt.b, lw_neg(le.b), lw_i(-1))) return false;
            for (int b = 0; b < 2; b++) {
                E->s[st->s] = b == 0 ? le : gt; E->sc[st->s] = NULL;
                if (!lw_steps_fit(E, &st->br[b])) return false;
            }
            return true;
        } else if (st->k == J_CASE) {
            if (st->s < 1 || st->s >= E->ns || st->hyp < 1 || st->hyp > E->v->nhyps) return false;
            const JHyp *H = &E->v->hyps[st->hyp - 1];
            if (!H->disj || st->nbr != H->ndis) return false;
            for (int32_t b = 0; b < st->nbr; b++) {
                E->sc[st->s] = &H->dis[b];
                if (!lw_steps_fit(E, &st->br[b])) return false;
            }
            return true;
        } else return false;
    }
    return false;
}
static void lw_scan(const JSteps *br, int32_t *mf, int32_t *ms) {
    for (int32_t i = 0; br && i < br->n; i++) {
        const JStep *st = &br->s[i];
        if ((st->k == J_LIN || st->k == J_RND || st->k == J_CLOSE) && st->f > *mf) *mf = st->f;
        if ((st->k == J_SPLIT || st->k == J_CASE) && st->s > *ms) *ms = st->s;
        for (int32_t b = 0; b < st->nbr; b++) lw_scan(&st->br[b], mf, ms);
    }
}
static bool lw_cert_fits(const JVc *vc, const JCert *c, void *mem, size_t size) {
    LwEval E; memset(&E, 0, sizeof E);
    E.v = vc; E.cur = (char *)mem; E.end = (char *)mem + size;
    int32_t mf = 0, ms = 0;
    lw_scan(&c->body, &mf, &ms);
    E.nf = mf + 1; E.ns = ms + 1;
    E.f = (LwFact *)lw_bump(&E, sizeof(LwFact) * (size_t)E.nf);
    E.s = (LwFact *)lw_bump(&E, sizeof(LwFact) * (size_t)E.ns);
    E.sc = (const JConj **)lw_bump(&E, sizeof(JConj *) * (size_t)E.ns);
    if (!E.f || !E.s || !E.sc || !lw_fact_new(&E, &E.tmp) || !lw_fact_new(&E, &E.g)) return false;
    // g, the goal's negation, as the kernel makes it: `-sum <= -b - 1`, or `sum = b` for `!=`
    if (!lw_fact_of(&E, &vc->goal, &E.g)) return false;
    if (vc->goal.op == J_LE) {
        for (int32_t v = 0; v < vc->nvars; v++) E.g.a[v] = lw_neg(E.g.a[v]);
        if (!lw_add_to(&E.g.b, lw_neg(E.g.b), lw_i(-1))) return false;
    }
    return lw_steps_fit(&E, &c->body);
}
// as the judge evaluates a model (judge.h, jm_model_ok): 1 holds, 0 does not, -1 does not fit
static int lw_holds_at(const JCons *c, const JInt *x, int32_t nv) {
    LSW s = lw_i(0);
    for (int32_t i = 0; i < c->nt; i++) {
        if (c->t[i].var < 0 || c->t[i].var >= nv) return -1;
        LSW p; if (!lw_mul_to(&p, c->t[i].coef, x[c->t[i].var]) || !lw_add_to(&s, s, p)) return -1;
    }
    int k = lw_cmp(s, c->rhs);
    return c->op == J_LE ? k <= 0 : c->op == J_EQ ? k == 0 : k != 0;
}
static bool lw_model_fits(const JVc *vc, const JModel *m) {
    if (m->n != vc->nvars) return false;
    for (int32_t h = 0; h < vc->nhyps; h++) {
        const JHyp *y = &vc->hyps[h];
        int r;
        if (!y->disj) r = lw_holds_at(&y->cons, m->val, vc->nvars);
        else {
            r = 0;
            for (int32_t d = 0; d < y->ndis && r != 1; d++) {
                int all = 1;
                for (int32_t c = 0; c < y->dis[d].nc && all == 1; c++) all = lw_holds_at(&y->dis[d].c[c], m->val, vc->nvars);
                if (all < 0) { r = -1; break; }
                if (all == 1) r = 1;
            }
        }
        if (r < 0) return false;
    }
    return lw_holds_at(&vc->goal, m->val, vc->nvars) >= 0;
}
// The wide retry's work. A wide pivot updates the whole tableau, rows times columns, in multi-limb
// arithmetic: 4NestedWith3Variables' path checks in rank.h's fallback (300 rows, about 470 columns)
// ran 1958 and 1926 pivots at 512 bits, 3.6 s each, about 77 million entry updates a second, for
// models no record used. Every answer the wide retry gained over the corpus and the trap ports took
// at most 99 pivots, on tableaux of 1,600 to 16,000 entries, and the harness's VCs with coefficients
// near 2^60 (about 200 entries) need up to 1203. So lia_solve and lia_cone give the wide search at
// most LS_WIDE_WORK entry updates, as pivots for its tableau's size (at least LS_WIDE_MIN): about
// 0.2 s. One cut off answers the 128-bit search's CAPACITY.
#define LS_WIDE_WORK ((uint64_t)1 << 24)
#define LS_WIDE_MIN 32
static JWork ls_wide_limit(const JVc *vc, JWork left) {
    uint64_t m = (uint64_t)vc->nhyps + 1, w = (uint64_t)(vc->nvars > 0 ? vc->nvars : 0) + m;
    uint64_t cap = LS_WIDE_WORK / (m * w);
    if (cap < LS_WIDE_MIN) cap = LS_WIDE_MIN;
    if (left.pivot > cap) left.pivot = cap;
    return left;
}

// The wide search, in the first three quarters of the region (its answer at their top), its
// answer followed at the kernel's and the judge's width in the last quarter.
static JAnswer ls_wide(const JVc *vc, bool cone, JWork limit, void *mem, size_t size) {
    size_t ws = size / 4 * 3;
    JAnswer W = ls_solve_w(vc, NULL, cone, limit, mem, ws, NULL);
    bool fits = W.k == J_ANS_CERT ? lw_cert_fits(vc, &W.cert, (char *)mem + ws, size - ws)
              : W.k == J_ANS_MODEL ? lw_model_fits(vc, &W.model) : true;
    if (!fits) { JWork u = W.used; memset(&W, 0, sizeof W); W.k = J_ANS_CAPACITY; W.used = u; }
    return W;
}

// Solve `vc` within `limit`, in the caller's region of `size` bytes. Every pointer in the answer
// points into that region. The 128-bit search; on CAPACITY its slices; and when it stopped because
// a number left 128 bits, the wide search last.
static JAnswer lia_solve(const JVc *vc, JWork limit, void *mem, size_t size) {
    bool ovf = false;
    JAnswer A = ls_solve(vc, NULL, false, limit, mem, size, &ovf);
    if (A.k != J_ANS_CAPACITY) return A;
    JWork used = A.used;
    JAnswer B = ls_sliced(vc, limit, mem, size, &used);
    if (B.k == J_ANS_CERT) { B.used = used; return B; }
    if (ovf) {
        JWork left = { ls_left(limit.pivot, used.pivot), ls_left(limit.node, used.node), ls_left(limit.leaf, used.leaf) };
        if (left.pivot && left.node && left.leaf) {
            JAnswer W = ls_wide(vc, false, ls_wide_limit(vc, left), mem, size);
            used.pivot += W.used.pivot; used.node += W.used.node; used.leaf += W.used.leaf;
            if (W.k == J_ANS_CERT || W.k == J_ANS_MODEL) { W.used = used; return W; }
        }
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
    bool ovf = false;
    A = ls_solve(vc, NULL, true, limit, mem, size, &ovf);
    if (A.k != J_ANS_CAPACITY || !ovf) return A;
    JWork left = { ls_left(limit.pivot, A.used.pivot), ls_left(limit.node, A.used.node), ls_left(limit.leaf, A.used.leaf) };
    if (!left.pivot || !left.node || !left.leaf) return A;
    JAnswer W = ls_wide(vc, true, ls_wide_limit(vc, left), mem, size);   // the wide retry, as lia_solve's
    W.used.pivot += A.used.pivot; W.used.node += A.used.node; W.used.leaf += A.used.leaf;
    if (W.k == J_ANS_CERT || W.k == J_ANS_MODEL) return W;
    A.used = W.used;
    return A;
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
