#ifndef LAIN_SOLVER_FINDER_H
#define LAIN_SOLVER_FINDER_H
// finder.h — the CLAIM PROPOSER (untrusted; judge_interfaces §8). The judge states and owes every
// claim it is handed and checks every certificate with its kernel, so nothing here is believed: a
// bad claim costs a refusal, never a false proof. Moved from judge_vc_driver.c unchanged in what it
// proposes; its globals are a JFinder's fields, and the judge's generator reaches it as calls
// (format.h's JudgeGen), so of src/judge it includes format.h only (judge_includes.py).
//
//   JFinder F = { .gen = &gen, .mod = mod, .work = w, .worksz = n, .lim = lim, .use_vra = true,
//                 .hdr = hdr, .nhdr = nhdr };
//   JClaim *cl = finder_propose(&F, &ncl);      // malloc'd, NULL for none; F's counters count
//   if (F.oom) ...                              // NULL because memory ran out, not for want of claims
// It writes nothing anywhere (G5): the caller reports.
#include <stdlib.h>
#include <string.h>
#include <stdio.h>                       // snprintf into the caller's err only
#include "ir/ir.h"
#include "judge/format.h"
#include "solver/lia.h"
#include "solver/rank.h"                 // PE.5 ranking functions and PE.7 relations, over a loop's paths

// VRA's relation at a loop header (PE.4's sink: vra.h's VraRel, copied by whoever runs VRA):
// sa*x[a] + sb*x[b] <= c, b < 0 for one term; and a header's converged state.
typedef struct { int a, sa, b, sb; int64_t c; } FdRel;
typedef struct { IrFunc *f; int bid; const FdRel *r; int n; } FdHdr;
typedef struct {
    const JudgeGen *gen;                 // the judge's generator: claim VCs, slots, terms
    IrFunc *mod;                         // the module's functions
    char *work; size_t worksz; JWork lim;   // the solver's region and its budget per query
    bool use_vra;                        // VRA's header states and return intervals as candidates
    const FdHdr *hdr; int nhdr;
    long long proposed, kept, dropped;   // counted across calls
    bool oom;                            // the last call returned no claims because memory ran out
    JRank *rk; int32_t nrk;              // the last call's ranking claims (malloc'd; the caller frees)
} JFinder;

// ── its own small helpers (the generator's are the judge's; these are untrusted copies) ─────────
typedef struct { __int128 lo, hi; bool has_ne; int64_t ne; } FdRange;
static bool fd_range(const IrType *t, FdRange *r) {
    if (!t) return false;
    r->has_ne = false; r->ne = 0;
    if (t->kind == IRT_BOOL) { r->lo = 0; r->hi = 1; return true; }
    if (t->kind != IRT_INT || t->bits < 1 || t->bits > 64) return false;
    int b = t->bits;
    if (t->is_signed) { r->lo = -((__int128)1 << (b - 1)); r->hi = ((__int128)1 << (b - 1)) - 1; }
    else              { r->lo = 0; r->hi = ((__int128)1 << b) - 1; }
    if (t->has_refine) {
        if ((__int128)t->refine_lo > r->lo) r->lo = t->refine_lo;
        if ((__int128)t->refine_hi < r->hi) r->hi = t->refine_hi;
    }
    if (t->has_ne) { r->has_ne = true; r->ne = t->refine_ne; }
    return true;
}
static bool fd_is_int(const IrType *t) { return t && (t->kind == IRT_INT || t->kind == IRT_BOOL); }
static bool fd_lands_exact(const IrType *from, const IrType *to, bool waived) {
    if (!fd_is_int(from) || !fd_is_int(to)) return false;
    if (from->kind == to->kind && (from->kind == IRT_BOOL || (from->bits == to->bits && from->is_signed == to->is_signed))) return true;
    if (!waived || (to->kind == IRT_INT && (to->has_refine || to->has_ne))) return true;
    FdRange a, b;
    return fd_range(from, &a) && fd_range(to, &b) && b.lo <= a.lo && a.hi <= b.hi;
}
static __int128 fd_const(const IrInstr *ins) {
    const IrType *t = ins->result ? ins->result->type : NULL;
    if (t && t->kind == IRT_INT && !t->is_signed && t->bits == 64) return (__int128)(uint64_t)ins->aux.imm;
    return (__int128)ins->aux.imm;
}
static bool fd_same_module(const IrFunc *a, const IrFunc *b) {
    return (!a->src_file && !b->src_file) || (a->src_file && b->src_file && !strcmp(a->src_file, b->src_file));
}
static IrFunc *fd_callee(const JFinder *F, const IrInstr *i) {
    if (i->op != IR_CALL || !i->aux.callee) return NULL;
    for (IrFunc *c = F->mod; c; c = c->next)
        if (c->name && c->name->length == i->aux.callee->length &&
            !memcmp(c->name->name, i->aux.callee->name, (size_t)c->name->length)) return c;
    return NULL;
}
static void fd_jint(JInt *x, __int128 v) {
    memset(x, 0, sizeof *x);
    unsigned __int128 m = v < 0 ? (unsigned __int128)0 - (unsigned __int128)v : (unsigned __int128)v;
    x->neg = v < 0;
    while (m) { x->limb[x->n++] = (uint32_t)m; m >>= 32; }
}
static __int128 fd_int128(const JInt *x) {             // the generator's constants are below 2^66
    unsigned __int128 m = 0;
    for (int i = x->n - 1; i >= 0; i--) m = (m << 32) | x->limb[i];
    return x->neg ? -(__int128)m : (__int128)m;
}

// ── the proposer ─────────────────────────────────────────────────────────────────────────────
// Candidates: at a forward join, each side of each fresh version; at a loop header, each side of
// each header version and of its difference with every other term the header may name
// (jgen_terms). A candidate's bound is the largest value its objective takes over the paths INTO
// the block (lia_maximize, PE.2): on a back edge that is one trip on from the candidates at their
// placeholder 0. There the header's comparisons, give or take one, contradict each other, so a
// back edge bounds nothing in that pass and a second pass bounds each version alone under the
// verified claims only (0057539). One query asks first whether the types' extreme is reached, and
// then nothing is claimed. A header's own comparisons
// add candidates with a fixed bound: `i < 2` proposes `i <= 1`, `i <= 2` and `i <= 3`, each side,
// since the bound an induction keeps is the guard's give or take a step. VRA's converged state at
// the header (PE.4) adds its relations as fixed candidates too, those whose terms a claim there may
// name, one of them a version the header makes. At a function's RETURNS (ret_candidates), for a
// function some function of its module calls: each side of the value returned and of its
// difference and sum with each integer parameter, bounded over the returns' paths, and VRA's
// return interval as a fixed candidate.
// Nothing here is believed: the generator owes every claim on every edge into its block, back
// edges included, and a return claim at every return, and HOUDINI drops a claim one of those VCs
// is not implied by, and solves the rest again, until none is: what survives is inductive
// together. Claims are proposed for the whole MODULE at once and Houdini runs over all of them,
// since a return claim may rest on its function's loop invariants, a loop on a callee's return
// claim, and a recursive function's return claim on itself at its own calls (induction over the
// calls). Bounds are computed in ROUNDS, each
// under the claims the previous round verified only (a candidate not yet verified may contradict
// another, and a bound computed under it would be vacuous): round 1 under none, then until a
// round verifies nothing new, three at most. A round's verified set contains the last one's.
typedef struct { JClaim c; __int128 tb; bool fixed; } JCand;
static bool cand_push(JCand **cd, int32_t *n, int32_t *cap, int32_t blk, int32_t k0, int c0, int32_t k1, int c1,
                      const IrType *t0, const IrType *t1) {
    FdRange r0, r1;
    if (!fd_range(t0, &r0) || (k1 >= 0 && !fd_range(t1, &r1))) return true;   // not an integer: no candidate
    if (*n == *cap) {
        int32_t nc = *cap ? *cap * 2 : 32;
        JCand *x = realloc(*cd, sizeof *x * (size_t)nc);
        if (!x) return false;
        *cd = x; *cap = nc;
    }
    JCand *c = &(*cd)[(*n)++]; memset(c, 0, sizeof *c);
    c->c.blk = blk; c->c.nt = k1 >= 0 ? 2 : 1;
    c->c.key[0] = k0; c->c.coef[0] = c0; c->tb = c0 > 0 ? r0.hi : -r0.lo;
    if (k1 >= 0) { c->c.key[1] = k1; c->c.coef[1] = c1; c->tb += c1 > 0 ? r1.hi : -r1.lo; }
    return true;
}
static bool same_form(const JClaim *a, const JClaim *b) {
    if (a->blk != b->blk || a->nt != b->nt) return false;
    for (int k = 0; k < a->nt; k++) if (a->key[k] != b->key[k] || a->coef[k] != b->coef[k]) return false;
    return true;
}
// Houdini over the module's claims cl[0..nc): each function's claim VCs (its cut points' and its
// returns'), solved; a claim one of them is not implied by is dropped, and the rest solved again,
// until none is. The claims left, in order. A function the generator refuses has none of its own,
// and its final generation reports the refusal.
static int32_t houdini_module(JFinder *F, JClaim *cl, int32_t nc) {
    char err[200];
    for (bool again = nc > 0; again; ) {
        again = false;
        uint8_t *bad = calloc((size_t)nc, 1);
        if (!bad) return -1;
        for (IrFunc *f = F->mod; f; f = f->next) {
            if (f->is_extern || !f->entry) continue;
            JClaimVc *o; int32_t n;
            if (!F->gen->claim_vcs(F->gen->ctx, f, cl, nc, JGEN_CLAIMS, &o, &n, err, sizeof err)) {
                for (int32_t ci = 0; ci < nc; ci++) if (cl[ci].fn == f) bad[ci] = 1;
                continue;
            }
            for (int32_t q = 0; q < n; q++)
                if (!bad[o[q].claim] && lia_solve(&o[q].vc, F->lim, F->work, F->worksz).k != J_ANS_CERT) bad[o[q].claim] = 1;
        }
        int32_t keep = 0;
        for (int32_t ci = 0; ci < nc; ci++) if (!bad[ci]) cl[keep++] = cl[ci]; else { F->dropped++; again = true; }
        nc = keep;
        free(bad);
    }
    return nc;
}
// ── PE.5, PE.7: a loop's paths, merged into one variable list for rank.h ─────────────────────
// gen->paths gives each edge into a header in its OWN variables; rank.h wants the back edges as
// conjunctive paths over ONE list, with a (pre, post) pair per quantity a function may read. Here
// each header version (a pair's key, a cell) is one pre and one post variable on every path; a
// value fixed at the header (a `%v<id>` term_kinds calls nameable, no cell) is one variable, its
// own pair (pre == post); every other variable stays its path's own. A disjunctive hypothesis is
// flattened, one path per choice of its disjuncts, FD_MAXFLAT at most per loop: beyond that, no
// candidate, which costs precision only. Nothing here is believed: a ranking claim is owed on every
// back edge, and a relation is a candidate Houdini keeps only if it is inductive.
#define FD_MAXFLAT 32
typedef struct { JCons c; int32_t *v; } FdCons;              // a constraint over a raw path's variables
typedef struct { bool disj; FdCons c; FdCons *dc; int32_t *dn; int32_t nd; } FdHyp;   // dn[k]: disjunct k's size
typedef struct { int32_t nv, *val, nh; FdHyp *h; int32_t from, *pre, *post; } FdRaw;
typedef struct {
    char *mem, *cur, *end;
    int32_t npairs, *pkey, *pre, *post;     // in the merged list; pkey: the cell or the value id
    uint8_t *ptk;                           // each pair's term kind (JTERM_LEN: a length)
    int32_t nv; const char **names;
    const JVc **paths; int32_t npaths, *edge;
    JVc **full;                             // each path before slicing, for a retry on fewer pairs
    int32_t nedges, edges[JRANK_MAXE];
    int32_t npk;                            // the first npk pairs are the header's versions (cells)
} FdLoop;
static void *fd_bump(FdLoop *L, size_t n) {
    uintptr_t at = ((uintptr_t)L->cur + 15u) & ~(uintptr_t)15u;
    if (at > (uintptr_t)L->end || n > (size_t)((uintptr_t)L->end - at)) return NULL;
    L->cur = (char *)at + n; memset((void *)at, 0, n); return (void *)at;
}
// A path variable that names a quantity fixed at the header: `%v<id>` a value (2*id) or
// `len(%v<id>)` that slice value's length (2*id + 1, JTERM_LEN), else -1.
static int32_t fd_value_id(const char *nm) {
    if (!nm) return -1;
    int tk = JTERM_VALUE;
    if (!strncmp(nm, "len(", 4)) { tk = JTERM_LEN; nm += 4; }
    if (nm[0] != '%' || nm[1] != 'v' || !nm[2]) return -1;
    int64_t x = 0; const char *q = nm + 2;
    for (; *q && *q != ')'; q++) { if (*q < '0' || *q > '9' || x > 100000000) return -1; x = x * 10 + (*q - '0'); }
    if (tk == JTERM_LEN ? (q[0] != ')' || q[1] != 0) : *q != 0) return -1;
    return (int32_t)(2 * x + tk);
}
static bool fd_copy_cons(FdLoop *L, const JCons *src, FdCons *dst) {
    dst->c = *src; dst->c.t = fd_bump(L, sizeof(JTerm) * (size_t)(src->nt ? src->nt : 1));
    if (!dst->c.t) return false;
    if (src->nt) memcpy(dst->c.t, src->t, sizeof(JTerm) * (size_t)src->nt);   // an empty sum has no terms to copy
    return true;
}
// A merged constraint: each term's variable through `map`, then ascending with duplicates summed
// (a value two variables share can meet itself), zero coefficients dropped.
static bool fd_map_cons(FdLoop *L, const JCons *c, const int32_t *map, JCons *out) {
    *out = *c; out->nt = 0; out->t = fd_bump(L, sizeof(JTerm) * (size_t)(c->nt ? c->nt : 1));
    if (!out->t) return false;
    for (int32_t i = 0; i < c->nt; i++) { out->t[out->nt] = c->t[i]; out->t[out->nt].var = map[c->t[i].var]; out->nt++; }
    for (int32_t i = 1; i < out->nt; i++)
        for (int32_t j = i; j > 0 && out->t[j - 1].var > out->t[j].var; j--) { JTerm x = out->t[j]; out->t[j] = out->t[j - 1]; out->t[j - 1] = x; }
    int32_t m = 0;
    for (int32_t i = 0; i < out->nt; i++) {
        if (m && out->t[m - 1].var == out->t[i].var) {
            LSol N; memset(&N, 0, sizeof N);
            LSi a = ls_from_j(&N, &out->t[m - 1].coef), b = ls_from_j(&N, &out->t[i].coef);
            if (N.cap) return false;
            ls_to_j(a + b, &out->t[m - 1].coef);
        } else out->t[m++] = out->t[i];
        if (m && out->t[m - 1].coef.n == 0) m--;
    }
    out->nt = m;
    return true;
}
// Which hypotheses a ranking function or a relation can use: those connected, through shared
// variables, to a quantity a pair names, and with no constant past 2^31. A type's bound such as
// 2^64 - 1 or an object size's 2^61 is never what a ranking proof rests on, and the Farkas system
// multiplies it with every other constant until 128-bit arithmetic overflows (CAPACITY after twenty
// pivots, on two thirds of the corpus's loops). Dropping a hypothesis only weakens the path, so
// what is found without it holds with it: candidates are lost at worst, never made wrong.
#define FD_BIGC ((LSi)1 << 31)
static bool fd_small(const JCons *c) {
    LSol N; memset(&N, 0, sizeof N);
    LSi r = ls_from_j(&N, &c->rhs);
    if (N.cap || r > FD_BIGC || r < -FD_BIGC) return false;
    for (int32_t i = 0; i < c->nt; i++) {
        LSi a = ls_from_j(&N, &c->t[i].coef);
        if (N.cap || a > FD_BIGC || a < -FD_BIGC) return false;
    }
    return true;
}
static int32_t fd_slice_on(FdLoop *L, JHyp *hy, int32_t m, int32_t nv, const uint8_t *on, int depth) {
    uint8_t *rel = fd_bump(L, (size_t)(nv ? nv : 1));
    if (!rel) return 0;
    int32_t k = 0;
    for (int32_t i = 0; i < m; i++) if (fd_small(&hy[i].cons)) hy[k++] = hy[i];
    m = k;
    for (int32_t q = 0; q < L->npairs; q++) if (!on || on[q]) rel[L->pre[q]] = rel[L->post[q]] = 1;
    // a round reaches the hypotheses touching what the LAST round reached (2: newly reached, 1: before)
    for (bool grew = true; grew && depth != 0; depth--) {     // depth < 0: to the whole component
        grew = false;
        for (int32_t i = 0; i < m; i++) {
            bool touch = false;
            for (int32_t t = 0; t < hy[i].cons.nt && !touch; t++) touch = rel[hy[i].cons.t[t].var] == 1;
            if (!touch) continue;
            for (int32_t t = 0; t < hy[i].cons.nt; t++) if (!rel[hy[i].cons.t[t].var]) { rel[hy[i].cons.t[t].var] = 2; grew = true; }
        }
        for (int32_t v = 0; v < nv; v++) if (rel[v] == 2) rel[v] = 1;
    }
    k = 0;
    for (int32_t i = 0; i < m; i++) {
        bool touch = false;
        for (int32_t t = 0; t < hy[i].cons.nt && !touch; t++) touch = rel[hy[i].cons.t[t].var];
        if (touch) hy[k++] = hy[i];
    }
    return k;
}
static int32_t fd_slice(FdLoop *L, JHyp *hy, int32_t m, int32_t nv) { return fd_slice_on(L, hy, m, nv, NULL, -1); }
static bool fd_int_eq(const JInt *a, const JInt *b) {
    if (a->neg != b->neg || a->n != b->n) return false;
    for (int i = 0; i < a->n; i++) if (a->limb[i] != b->limb[i]) return false;
    return true;
}
static bool fd_cons_eq(const JCons *a, const JCons *b) {
    if (a->op != b->op || a->nt != b->nt || !fd_int_eq(&a->rhs, &b->rhs)) return false;
    for (int32_t i = 0; i < a->nt; i++) if (a->t[i].var != b->t[i].var || !fd_int_eq(&a->t[i].coef, &b->t[i].coef)) return false;
    return true;
}
// Two paths that state the same rows (sliced paths of one edge that differed only in a case of a
// quantity the slice dropped, a running total's wraps): one is enough, and twelve such paths made
// the Farkas system a thousand variables wide, past lia_solve's room.
static bool fd_same_path(const JVc *a, const JVc *b) {
    if (a->nhyps != b->nhyps) return false;
    for (int32_t i = 0; i < a->nhyps; i++) if (!fd_cons_eq(&a->hyps[i].cons, &b->hyps[i].cons)) return false;
    return true;
}
// Build L for header H of f under the claims cl: false when there is nothing to propose (no back
// edge, a refused header, too many paths, out of memory); never an error of the judge's.
// With H = JRANK_ENTRY, for f's RECURSION: the paths are f's calls that may close a cycle through
// it (JudgeGen.call_paths), each a transition from f's parameters at entry to the call's arguments,
// as a back edge is from the header's versions to the edge's. Only SELF-recursion has a pre-state
// for every pair (a call to another function of the cycle has none), so a cycle of several
// functions builds nothing here: its measures must be found jointly (a later link).
static bool fd_loop_build(JFinder *F, IrFunc *f, int32_t H, const JClaim *cl, int32_t ncl, FdLoop *L) {
    char err[200];
    memset(L, 0, sizeof *L);
    size_t cap = (size_t)1 << 22;
    if (!(L->mem = malloc(cap))) { F->oom = true; return false; }
    L->cur = L->mem; L->end = L->mem + cap;
    JPaths P;
    bool entry = H == JRANK_ENTRY;
    if (entry ? !F->gen->call_paths || !F->gen->call_paths(F->gen->ctx, f, cl, ncl, &P, err, sizeof err)
              : !F->gen->paths || !F->gen->paths(F->gen->ctx, f, cl, ncl, H, &P, err, sizeof err)) return false;
    if (P.refused) return false;
    // 1. the back edges, copied out of the generator's region (the next call reuses it)
    int32_t nraw = 0;
    for (int32_t k = 0; k < P.n; k++) if (P.p[k].back) nraw++;
    if (nraw == 0 || nraw > JRANK_MAXE) return false;
    FdRaw *raw = fd_bump(L, sizeof *raw * (size_t)nraw);
    int32_t npk = -1, *pk = NULL; uint8_t *pktk = NULL;
    if (!raw) return false;
    int32_t r = 0;
    for (int32_t k = 0; k < P.n; k++) {
        const JPath *pp = &P.p[k];
        if (!pp->back) continue;
        if (npk < 0) {                                       // the header's versions, by key
            npk = pp->npairs; pk = fd_bump(L, sizeof *pk * (size_t)(npk ? npk : 1));
            pktk = fd_bump(L, (size_t)(npk ? npk : 1));
            if (!pk || !pktk) return false;
            for (int32_t q = 0; q < npk; q++) { pk[q] = pp->pairs[q].key; pktk[q] = pp->pairs[q].tk; }
        } else if (pp->npairs != npk) return false;
        FdRaw *w = &raw[r++];
        const JVc *v = pp->vc;
        w->from = pp->from; w->nv = v->nvars; w->nh = v->nhyps;
        w->val = fd_bump(L, sizeof(int32_t) * (size_t)(v->nvars ? v->nvars : 1));
        w->pre = fd_bump(L, sizeof(int32_t) * (size_t)(npk ? npk : 1));
        w->post = fd_bump(L, sizeof(int32_t) * (size_t)(npk ? npk : 1));
        w->h = fd_bump(L, sizeof(FdHyp) * (size_t)(v->nhyps ? v->nhyps : 1));
        if (!w->val || !w->pre || !w->post || !w->h) return false;
        for (int32_t q = 0; q < npk; q++) {
            int32_t at = -1;
            for (int32_t z = 0; z < pp->npairs; z++) if (pp->pairs[z].key == pk[q] && pp->pairs[z].tk == pktk[q]) at = z;
            if (at < 0 || pp->pairs[at].pre < 0 || pp->pairs[at].post < 0) return false;
            w->pre[q] = pp->pairs[at].pre; w->post[q] = pp->pairs[at].post;
        }
        for (int32_t x = 0; x < v->nvars; x++) w->val[x] = fd_value_id(v->vars[x]);
        for (int32_t q = 0; q < npk; q++) w->val[w->pre[q]] = w->val[w->post[q]] = -1;
        for (int32_t h = 0; h < v->nhyps; h++) {
            const JHyp *hy = &v->hyps[h];
            FdHyp *o = &w->h[h];
            o->disj = hy->disj;
            if (!hy->disj) { if (!fd_copy_cons(L, &hy->cons, &o->c)) return false; continue; }
            int32_t tot = 0;
            for (int32_t d = 0; d < hy->ndis; d++) tot += hy->dis[d].nc;
            o->nd = hy->ndis;
            o->dn = fd_bump(L, sizeof(int32_t) * (size_t)(hy->ndis ? hy->ndis : 1));
            o->dc = fd_bump(L, sizeof(FdCons) * (size_t)(tot ? tot : 1));
            if (!o->dn || !o->dc) return false;
            int32_t z = 0;
            for (int32_t d = 0; d < hy->ndis; d++) {
                o->dn[d] = hy->dis[d].nc;
                for (int32_t c = 0; c < hy->dis[d].nc; c++) if (!fd_copy_cons(L, &hy->dis[d].c[c], &o->dc[z++])) return false;
            }
        }
    }
    // 2. which `%v` values are fixed at the header (a claim may name them): one variable each
    int32_t nval = 0, *vals = fd_bump(L, sizeof(int32_t) * 256);
    if (!vals) return false;
    for (int32_t k = 0; k < nraw; k++)
        for (int32_t x = 0; x < raw[k].nv; x++) {
            int32_t id = raw[k].val[x]; bool seen = false;
            if (id < 0) continue;
            for (int32_t q = 0; q < nval && !seen; q++) seen = vals[q] == id;
            if (!seen && nval < 256) vals[nval++] = id;
        }
    uint8_t *kind = fd_bump(L, (size_t)(nval ? nval : 1)), *vtk = fd_bump(L, (size_t)(nval ? nval : 1));
    int32_t *vid = fd_bump(L, sizeof(int32_t) * (size_t)(nval ? nval : 1));
    if (!kind || !vtk || !vid) return false;
    for (int32_t q = 0; q < nval; q++) { vid[q] = vals[q] / 2; vtk[q] = (uint8_t)(vals[q] % 2); }
    // at a function's entry nothing but its parameters may be a measure's term, and they are pairs
    if (entry) nval = 0;
    if (nval && !F->gen->term_kinds(F->gen->ctx, f, H, vid, vtk, nval, kind, err, sizeof err)) return false;
    int32_t nshared = 0;
    for (int32_t q = 0; q < nval; q++) if (kind[q] == 1) vals[nshared] = vals[q], kind[nshared++] = 1;
    // 3. the merged list: the header's versions (pre, post), the shared values, then each path's own
    L->npk = npk;
    int32_t nv = 2 * npk + nshared, base_local = nv;
    for (int32_t k = 0; k < nraw; k++) nv += raw[k].nv;
    L->nv = nv;
    L->names = fd_bump(L, sizeof(char *) * (size_t)(nv ? nv : 1));
    L->npairs = npk + nshared;
    L->ptk = fd_bump(L, (size_t)(L->npairs ? L->npairs : 1));
    L->pkey = fd_bump(L, sizeof(int32_t) * (size_t)(L->npairs ? L->npairs : 1));
    L->pre = fd_bump(L, sizeof(int32_t) * (size_t)(L->npairs ? L->npairs : 1));
    L->post = fd_bump(L, sizeof(int32_t) * (size_t)(L->npairs ? L->npairs : 1));
    char *nb = fd_bump(L, (size_t)nv * 16);
    if (!L->names || !L->pkey || !L->ptk || !L->pre || !L->post || !nb) return false;
    for (int32_t v = 0; v < nv; v++) { snprintf(nb + (size_t)v * 16, 16, "m%d", v); L->names[v] = nb + (size_t)v * 16; }
    for (int32_t q = 0; q < npk; q++) { L->pkey[q] = pk[q]; L->ptk[q] = pktk[q]; L->pre[q] = 2 * q; L->post[q] = 2 * q + 1; }
    for (int32_t q = 0; q < nshared; q++) {
        L->pkey[npk + q] = vals[q] / 2; L->ptk[npk + q] = (uint8_t)(vals[q] % 2);
        L->pre[npk + q] = L->post[npk + q] = 2 * npk + q;
    }
    // 4. each path flattened, in the merged variables
    L->paths = fd_bump(L, sizeof(JVc *) * FD_MAXFLAT);
    L->edge = fd_bump(L, sizeof(int32_t) * FD_MAXFLAT);
    L->full = fd_bump(L, sizeof(JVc *) * FD_MAXFLAT);
    if (!L->paths || !L->edge || !L->full) return false;
    int32_t local = base_local;
    for (int32_t k = 0; k < nraw; k++) {
        FdRaw *w = &raw[k];
        int32_t *map = fd_bump(L, sizeof(int32_t) * (size_t)(w->nv ? w->nv : 1));
        if (!map) return false;
        for (int32_t x = 0; x < w->nv; x++) {
            map[x] = -1;
            // a quantity the path leaves unchanged has ONE variable for both sides (a recursive call
            // passing a parameter through, `f(lo + 1, hi)`): it is the pre-state's, and the post's is
            // tied to it below, or `hi` would have no pre-state and no measure could read it
            for (int32_t q = 0; q < npk; q++) { if (w->pre[q] == x) map[x] = 2 * q; if (w->post[q] == x && w->pre[q] != x) map[x] = 2 * q + 1; }
            for (int32_t q = 0; q < nshared && map[x] < 0; q++) if (w->val[x] == vals[q]) map[x] = 2 * npk + q;
            if (map[x] < 0) map[x] = local++;
        }
        bool seen = false;
        for (int32_t e = 0; e < L->nedges; e++) seen |= L->edges[e] == w->from;
        if (!seen) L->edges[L->nedges++] = w->from;
        // Which disjunctions to flatten: those connected, through the path's variables, to a pair
        // or a shared value (the others say nothing a ranking function or relation reads); and if
        // their product still passes what is left of FD_MAXFLAT, the widest are dropped until it
        // does not. Dropping a hypothesis only weakens the path (a wrap's cases on a running total
        // the loop's measure never reads, four of them a trip in judge_callee_contracts' `main`).
        uint8_t *rel = fd_bump(L, (size_t)(w->nv ? w->nv : 1)), *skip = fd_bump(L, (size_t)(w->nh ? w->nh : 1));
        if (!rel || !skip) return false;
        for (int32_t q = 0; q < npk; q++) rel[w->pre[q]] = rel[w->post[q]] = 1;
        for (int32_t x = 0; x < w->nv; x++) if (map[x] >= 2 * npk && map[x] < base_local) rel[x] = 1;
        for (bool grew = true; grew; ) {
            grew = false;
            for (int32_t h = 0; h < w->nh; h++) {
                FdHyp *o = &w->h[h];
                int32_t ncs = o->disj ? 0 : 1;
                for (int32_t d = 0; o->disj && d < o->nd; d++) ncs += o->dn[d];
                bool touch = false;
                for (int32_t c = 0; c < ncs && !touch; c++) {
                    const JCons *cc = o->disj ? &o->dc[c].c : &o->c.c;
                    for (int32_t t = 0; t < cc->nt && !touch; t++) touch = rel[cc->t[t].var];
                }
                if (!touch) continue;
                for (int32_t c = 0; c < ncs; c++) {
                    const JCons *cc = o->disj ? &o->dc[c].c : &o->c.c;
                    for (int32_t t = 0; t < cc->nt; t++) if (!rel[cc->t[t].var]) { rel[cc->t[t].var] = 1; grew = true; }
                }
            }
        }
        for (int32_t h = 0; h < w->nh; h++) {
            FdHyp *o = &w->h[h];
            if (!o->disj) continue;
            bool touch = false;
            for (int32_t c = 0, z = 0; c < o->nd && !touch; c++)
                for (int32_t e = 0; e < o->dn[c] && !touch; e++, z++)
                    for (int32_t t = 0; t < o->dc[z].c.nt && !touch; t++) touch = rel[o->dc[z].c.t[t].var];
            skip[h] = !touch;
        }
        int32_t left = FD_MAXFLAT - L->npaths, nchoice;
        for (;;) {
            int64_t prod = 1; int32_t widest = -1;
            for (int32_t h = 0; h < w->nh; h++) {
                if (!w->h[h].disj || skip[h]) continue;
                prod *= w->h[h].nd; if (prod > (int64_t)1 << 30) prod = (int64_t)1 << 30;
                if (widest < 0 || w->h[h].nd > w->h[widest].nd) widest = h;
            }
            if (prod <= left) { nchoice = (int32_t)prod; break; }
            skip[widest] = 1;                                // widest is set: prod > left >= 0 needs one
        }
        if (nchoice < 1) return false;
        for (int32_t ch = 0; ch < nchoice; ch++) {
            if (L->npaths == FD_MAXFLAT) return false;
            int32_t nh = 0;
            for (int32_t h = 0; h < w->nh; h++) nh += 1;
            int32_t extra = 0, rest = ch;
            for (int32_t h = 0; h < w->nh; h++) if (w->h[h].disj && !skip[h]) { extra += w->h[h].dn[rest % w->h[h].nd]; rest /= w->h[h].nd; }
            JVc *vc = fd_bump(L, sizeof *vc);
            JHyp *hy = fd_bump(L, sizeof(JHyp) * (size_t)(nh + extra + 1 + 2 * npk));
            if (!vc || !hy) return false;
            int32_t m = 0; rest = ch;
            for (int32_t h = 0; h < w->nh; h++) {
                FdHyp *o = &w->h[h];
                if (!o->disj) { hy[m].origin = "path"; if (!fd_map_cons(L, &o->c.c, map, &hy[m].cons)) return false; m++; continue; }
                if (skip[h]) continue;                       // a dropped disjunction: the path, weakened
                int32_t pick = rest % o->nd; rest /= o->nd;
                int32_t z = 0;
                for (int32_t d = 0; d < pick; d++) z += o->dn[d];
                for (int32_t c = 0; c < o->dn[pick]; c++) { hy[m].origin = "path case"; if (!fd_map_cons(L, &o->dc[z + c].c, map, &hy[m].cons)) return false; m++; }
            }
            for (int32_t q = 0; q < npk; q++) {               // ...post - pre <= 0 and pre - post <= 0
                if (w->pre[q] != w->post[q]) continue;
                for (int sg = 0; sg < 2; sg++) {
                    JTerm *t = fd_bump(L, sizeof(JTerm) * 2);
                    if (!t) return false;
                    t[0].var = 2 * q;     fd_jint(&t[0].coef, sg ? 1 : -1);
                    t[1].var = 2 * q + 1; fd_jint(&t[1].coef, sg ? -1 : 1);
                    memset(&hy[m], 0, sizeof hy[m]);
                    hy[m].origin = "unchanged"; hy[m].cons.t = t; hy[m].cons.nt = 2; hy[m].cons.op = J_LE; fd_jint(&hy[m].cons.rhs, 0);
                    m++;
                }
            }
            vc->vars = L->names; vc->nvars = nv; vc->hyps = hy; vc->nhyps = m;
            vc->id.fn = "loop"; vc->id.irhash = "0"; vc->id.point = "path"; vc->id.kind = "path"; vc->id.ordinal = "0";
            // a path that cannot run (a wrapping step's wrap case under a guard that rules it out)
            // is dropped: rk_build's Farkas form asks every path for the decrease as a combination
            // of its rows, which an infeasible one need not give, and lia_rank then says NONE. It is
            // asked on the WHOLE path, before slicing: a wrap case's 2^w is a large constant, and
            // without it the case would look feasible and say nothing
            vc->goal.op = J_LE; vc->goal.nt = 0; fd_jint(&vc->goal.rhs, -1);
            if (lia_solve(vc, F->lim, F->work, F->worksz).k == J_ANS_CERT) continue;
            fd_jint(&vc->goal.rhs, 0);                       // 0 <= 0: nothing is asked of it
            JVc *fv = fd_bump(L, sizeof *fv); JHyp *fh = fd_bump(L, sizeof(JHyp) * (size_t)(m ? m : 1));
            if (!fv || !fh) return false;
            memcpy(fh, hy, sizeof(JHyp) * (size_t)m); *fv = *vc; fv->hyps = fh; fv->nhyps = m;
            L->full[L->npaths] = fv;
            vc->nhyps = fd_slice(L, hy, m, nv);
            bool dup = false;
            for (int32_t z = 0; z < L->npaths && !dup; z++) dup = L->edge[z] == w->from && fd_same_path(L->paths[z], vc);
            if (dup) continue;
            L->paths[L->npaths] = vc; L->edge[L->npaths] = w->from; L->npaths++;
        }
    }
    return L->npaths > 0;
}
static bool fd_i128(const JInt *x, __int128 *out) {
    LSol N; memset(&N, 0, sizeof N);
    LSi v = ls_from_j(&N, x);
    if (N.cap || v > ((__int128)1 << 66) || v < -((__int128)1 << 66)) return false;
    *out = v; return true;
}
// PE.5: a ranking claim for the loop at H, from L: a linear one if lia_rank finds it, else a
// lexicographic one whose falling component agrees on every flattened path of each back edge (the
// judge owes one fall per edge). False: none.
static bool fd_rank_on(JFinder *F, IrFunc *f, int32_t H, const FdLoop *L, JRank *out);
// ...and when the whole loop is too much for the Farkas system (CAPACITY: a dozen paths of a
// hundred hypotheses still overflow 128 bits), or has no ranking function over all of its versions
// at once, once per header version: that one quantity, the values fixed at the header, and the
// paths sliced to their connected part. A counter's `n - i` is found from i alone.
static bool fd_rank(JFinder *F, IrFunc *f, int32_t H, FdLoop *L, JRank *out) {
    if (fd_rank_on(F, f, H, L, out)) return true;
    int32_t np = L->npaths;
    const JVc **sl = fd_bump(L, sizeof(JVc *) * (size_t)(np ? np : 1));
    uint8_t *on = fd_bump(L, (size_t)(L->npairs ? L->npairs : 1));
    if (!sl || !on) return false;
    for (int32_t q = 0; q < L->npk; q++) {
        for (int32_t z = 0; z < L->npairs; z++) on[z] = z == q || z >= L->npk;
        bool ok = true; int32_t ns = 0;
        int32_t *sedge = fd_bump(L, sizeof(int32_t) * (size_t)(np ? np : 1));
        if (!sedge) break;
        for (int32_t k = 0; k < np && ok; k++) {
            JVc *v = fd_bump(L, sizeof *v); JHyp *h = fd_bump(L, sizeof(JHyp) * (size_t)(L->full[k]->nhyps ? L->full[k]->nhyps : 1));
            if (!v || !h) { ok = false; break; }
            memcpy(h, L->full[k]->hyps, sizeof(JHyp) * (size_t)L->full[k]->nhyps);
            *v = *L->full[k]; v->hyps = h; v->nhyps = fd_slice_on(L, h, L->full[k]->nhyps, L->nv, on, 2);
            bool dup = false;
            for (int32_t z = 0; z < ns && !dup; z++) dup = sedge[z] == L->edge[k] && fd_same_path(sl[z], v);
            if (dup) continue;
            sl[ns] = v; sedge[ns] = L->edge[k]; ns++;
        }
        if (!ok) break;
        FdLoop V = *L; int32_t nn = 0;                       // only the pairs `on` names
        V.paths = sl; V.npaths = ns; V.edge = sedge;
        V.pkey = fd_bump(L, sizeof(int32_t) * (size_t)L->npairs); V.ptk = fd_bump(L, (size_t)L->npairs);
        V.pre = fd_bump(L, sizeof(int32_t) * (size_t)L->npairs); V.post = fd_bump(L, sizeof(int32_t) * (size_t)L->npairs);
        if (!V.pkey || !V.ptk || !V.pre || !V.post) break;
        for (int32_t z = 0; z < L->npairs; z++) if (on[z]) { V.pkey[nn] = L->pkey[z]; V.ptk[nn] = L->ptk[z]; V.pre[nn] = L->pre[z]; V.post[nn] = L->post[z]; nn++; }
        V.npairs = nn; V.npk = 1;
        bool got = fd_rank_on(F, f, H, &V, out);
        if (got) return true;
    }
    return false;
}
static bool fd_rank_on(JFinder *F, IrFunc *f, int32_t H, const FdLoop *L, JRank *out) {
    memset(out, 0, sizeof *out);
    out->blk = H; out->fn = f; out->nedge = L->nedges;
    for (int32_t e = 0; e < L->nedges; e++) out->from[e] = L->edges[e];
    LRank R1 = lia_rank(L->paths, L->npaths, L->pre, L->post, L->npairs, F->lim, F->work, F->worksz);
    JInt *coef[JRANK_MAXC]; JInt c0[JRANK_MAXC]; int32_t ncomp = 0;
    int32_t fall_of_edge[JRANK_MAXE];
    if (R1.k == LIA_RANK_FOUND) {
        coef[0] = R1.coef; c0[0] = R1.c0; ncomp = 1;
        for (int32_t e = 0; e < L->nedges; e++) fall_of_edge[e] = 0;
    } else {
        LRankLex R2 = lia_rank_lex(L->paths, L->npaths, L->pre, L->post, L->npairs, F->lim, F->work, F->worksz);
        if (R2.k != LIA_RANK_FOUND || R2.ncomp > JRANK_MAXC) return false;
        ncomp = R2.ncomp;
        for (int32_t j = 0; j < ncomp; j++) { coef[j] = R2.coef[j]; c0[j] = R2.c0[j]; }
        for (int32_t e = 0; e < L->nedges; e++) {
            fall_of_edge[e] = -1;
            for (int32_t k = 0; k < L->npaths; k++) {
                if (L->edge[k] != L->edges[e]) continue;
                if (fall_of_edge[e] < 0) fall_of_edge[e] = R2.fall[k];
                else if (fall_of_edge[e] != R2.fall[k]) return false;   // one fall per back edge
            }
            if (fall_of_edge[e] < 0) return false;
        }
    }
    // the terms: every pair a component reads
    int32_t idx[JRANK_MAXT]; int32_t nt = 0;
    for (int32_t i = 0; i < L->npairs; i++) {
        bool used = false;
        for (int32_t j = 0; j < ncomp; j++) used |= coef[j][i].n != 0;
        if (!used) continue;
        if (nt == JRANK_MAXT) return false;
        idx[nt++] = i;
    }
    if (nt == 0) return false;
    out->ncomp = ncomp; out->nt = nt;
    for (int32_t t = 0; t < nt; t++) { out->key[t] = L->pkey[idx[t]]; out->tk[t] = L->ptk[idx[t]]; }
    for (int32_t j = 0; j < ncomp; j++) {
        if (!fd_i128(&c0[j], &out->c0[j])) return false;
        for (int32_t t = 0; t < nt; t++) if (!fd_i128(&coef[j][idx[t]], &out->coef[j][t])) return false;
    }
    for (int32_t e = 0; e < L->nedges; e++) out->fall[e] = (int8_t)fall_of_edge[e];
    return true;
}
// PE.7: the two-variable relations the loop keeps (lia_relations), as candidates for the header's
// claims, bounded like any other (bound_candidates) and kept by Houdini only if inductive. One of
// the two terms must be a version the header makes (a claim names one); a relation between two
// values fixed at the header says nothing a claim may state.
static bool fd_relation_candidates(JFinder *F, IrFunc *f, int32_t H, const FdLoop *L, int32_t npk,
                                   JCand **cd, int32_t *n, int32_t *cap) {
    LRel rel[32];
    int32_t nr = lia_relations(L->paths, L->npaths, L->pre, L->post, L->npairs, F->lim, F->work, F->worksz, rel, 32);
    for (int32_t q = 0; q < nr; q++) {
        if (rel[q].t >= npk && rel[q].j >= npk) continue;
        if (*n == *cap) {
            int32_t nc = *cap ? *cap * 2 : 32;
            JCand *x = realloc(*cd, sizeof *x * (size_t)nc);
            if (!x) return false;
            *cd = x; *cap = nc;
        }
        JCand *c = &(*cd)[(*n)++]; memset(c, 0, sizeof *c);
        c->c.blk = H; c->c.nt = 2; c->c.fn = f;
        c->c.key[0] = L->pkey[rel[q].t]; c->c.tk[0] = L->ptk[rel[q].t]; c->c.coef[0] = rel[q].ct;
        c->c.key[1] = L->pkey[rel[q].j]; c->c.tk[1] = L->ptk[rel[q].j]; c->c.coef[1] = rel[q].cj;
        c->tb = ((__int128)1 << 100);                        // the type bound's role: any finite bound is tighter
    }
    return true;
}

// One block's candidates (the slots sl[i..j), all at one block), into `cd`.
static bool block_candidates(JFinder *F, IrFunc *f, const JSlot *sl, int32_t i, int32_t j,
                             JCand **cd, int32_t *ncd, int32_t *ccd, char *err, size_t errcap) {
    int32_t H = sl[i].blk, nt = 0;
    bool head = sl[i].head;
    JSlot *tm = NULL;
    *ncd = 0;
    if (head) {
        JSlot *t0;
        if (!F->gen->terms(F->gen->ctx, f, H, &t0, &nt, err, (uint32_t)errcap)) return false;
        if (!(tm = malloc(sizeof *tm * (size_t)(nt ? nt : 1)))) goto oom;
        memcpy(tm, t0, sizeof *tm * (size_t)nt);
    }
    for (int32_t k = i; k < j; k++) {
        if (!cand_push(cd, ncd, ccd, H, sl[k].key, 1, -1, 0, sl[k].type, NULL) ||
            !cand_push(cd, ncd, ccd, H, sl[k].key, -1, -1, 0, sl[k].type, NULL)) goto oom;
        for (int32_t t = 0; t < nt; t++) {
            if (tm[t].key == sl[k].key) continue;
            bool own = false;                                // two of the header's versions: one pair
            for (int32_t k2 = i; k2 < j; k2++) if (sl[k2].key == tm[t].key) own = true;
            if (own && tm[t].key < sl[k].key) continue;
            if (!cand_push(cd, ncd, ccd, H, sl[k].key, 1, tm[t].key, -1, sl[k].type, tm[t].type) ||
                !cand_push(cd, ncd, ccd, H, sl[k].key, -1, tm[t].key, 1, sl[k].type, tm[t].type)) goto oom;
        }
    }
    // a header's comparisons: a header version against a constant or another term, each side at
    // the compared value give or take one (fixed bounds, nothing to maximize)
    IrBlock *hb = NULL;
    for (IrBlock *b = f->blocks; head && b && !hb; b = b->next) if (b->id == H) hb = b;
    for (IrInstr *ic = hb ? hb->instrs : NULL; ic; ic = ic->next) {
        if (ic->op != IR_ICMP || ic->n_operands < 2) continue;
        int32_t key[2] = { -1, -1 }; bool isc[2] = { false, false }; __int128 cv[2] = { 0, 0 };
        const IrType *ty[2] = { NULL, NULL };
        for (int k = 0; k < 2; k++) {
            IrValue *o = ic->operands[k];
            IrInstr *d = NULL;
            for (IrInstr *x = hb->instrs; x && x != ic; x = x->next) if (x->result == o) d = x;
            if (d && d->op == IR_CONST) { isc[k] = true; cv[k] = fd_const(d); continue; }
            int32_t kk = d && d->op == IR_LOAD && d->n_operands >= 1 && d->operands[0] ? d->operands[0]->id : o ? o->id : -1;
            for (int32_t t = 0; t < nt; t++) if (tm[t].key == kk) { key[k] = kk; ty[k] = tm[t].type; }
        }
        for (int k = 0; k < 2; k++) {
            int32_t x = -1;                                  // which header version this side is
            for (int32_t s2 = i; s2 < j; s2++) if (sl[s2].key == key[k]) x = s2;
            if (x < 0 || (!isc[1 - k] && key[1 - k] < 0)) continue;
            for (int d = -1; d <= 1; d++) {
                int32_t before = *ncd;
                bool ok = isc[1 - k]
                    ? cand_push(cd, ncd, ccd, H, key[k], 1, -1, 0, sl[x].type, NULL) && cand_push(cd, ncd, ccd, H, key[k], -1, -1, 0, sl[x].type, NULL)
                    : cand_push(cd, ncd, ccd, H, key[k], 1, key[1 - k], -1, sl[x].type, ty[1 - k]) &&
                      cand_push(cd, ncd, ccd, H, key[k], -1, key[1 - k], 1, sl[x].type, ty[1 - k]);
                if (!ok) goto oom;
                if (*ncd != before + 2) continue;
                (*cd)[before].fixed = (*cd)[before + 1].fixed = true;
                (*cd)[before].c.rhs = isc[1 - k] ? cv[1 - k] + d : d;
                (*cd)[before + 1].c.rhs = isc[1 - k] ? -(cv[1 - k] + d) : d;
            }
        }
    }
    // VRA's relations at this header
    for (int h = 0; head && h < F->nhdr; h++) {
        if (F->hdr[h].f != f || F->hdr[h].bid != H || F->hdr[h].n == 0) continue;
        int32_t *keys = malloc(sizeof *keys * (size_t)F->hdr[h].n * 2);
        uint8_t *kind = malloc((size_t)F->hdr[h].n * 2);
        if (!keys || !kind) { free(keys); free(kind); goto oom; }
        for (int r = 0; r < F->hdr[h].n; r++) { keys[2 * r] = F->hdr[h].r[r].a; keys[2 * r + 1] = F->hdr[h].r[r].b >= 0 ? F->hdr[h].r[r].b : F->hdr[h].r[r].a; }
        if (!F->gen->term_kinds(F->gen->ctx, f, H, keys, NULL, F->hdr[h].n * 2, kind, err, (uint32_t)errcap)) { free(keys); free(kind); free(tm); return false; }
        for (int r = 0; r < F->hdr[h].n; r++) {
            const FdRel *v = &F->hdr[h].r[r];
            if (!kind[2 * r] || !kind[2 * r + 1] || (kind[2 * r] != 2 && kind[2 * r + 1] != 2)) continue;
            if ((v->sa != 1 && v->sa != -1) || (v->b >= 0 && v->sb != 1 && v->sb != -1)) continue;
            const IrType *ta = NULL, *tb = NULL;
            for (int32_t t = 0; t < nt; t++) { if (tm[t].key == v->a) ta = tm[t].type; if (tm[t].key == v->b) tb = tm[t].type; }
            for (int32_t k = i; k < j; k++) { if (sl[k].key == v->a) ta = sl[k].type; if (sl[k].key == v->b) tb = sl[k].type; }
            if (!ta || (v->b >= 0 && !tb)) continue;         // a term whose type the proposer cannot name
            int32_t before = *ncd;
            if (!cand_push(cd, ncd, ccd, H, v->a, v->sa, v->b, v->sb, ta, tb)) { free(keys); free(kind); goto oom; }
            if (*ncd == before + 1) { (*cd)[before].fixed = true; (*cd)[before].c.rhs = v->c; }
        }
        free(keys); free(kind);
    }
    // PE.7: the relations the loop keeps between its versions (and values fixed at it)
    if (head) {
        FdLoop L;
        bool built = fd_loop_build(F, f, H, NULL, 0, &L);
        bool ok = !built || fd_relation_candidates(F, f, H, &L, L.npk, cd, ncd, ccd);
        free(L.mem);
        if (!ok) goto oom;
    }
    free(tm);
    return true;
oom:
    free(tm); snprintf(err, errcap, "out of memory");
    return false;
}
// Each candidate's bound, from the VCs o[0..n) of a claim set whose candidates start at `base`:
// the largest value its objective takes over its VCs (a fixed one keeps its own). What survives,
// unless a verified claim is as tight or this very claim was proposed already, goes into cl.
static bool bound_candidates(JFinder *F, const JCand *cd, int32_t ncd, const JClaimVc *o, int32_t n, int32_t base,
                             const JClaim *as, int32_t nv, JClaim **cl, int32_t *nc, int32_t *ccl) {
    for (int32_t c = 0; c < ncd; c++) {
        JClaim got = cd[c].c;
        if (!cd[c].fixed) {
            __int128 best = 0;
            bool ok = true, any = false;
            for (int32_t q = 0; q < n && ok; q++) {
                if (o[q].claim != base + c) continue;
                JVc v = o[q].vc;
                fd_jint(&v.goal.rhs, cd[c].tb - 1);
                if (lia_solve(&v, F->lim, F->work, F->worksz).k != J_ANS_CERT) { ok = false; break; }
                LMax x = lia_maximize(&o[q].vc, o[q].vc.goal.t, o[q].vc.goal.nt, F->lim, F->work, F->worksz);
                if (x.k == LIA_MAX_EMPTY) continue;
                if (x.k != LIA_MAX_BOUND) { ok = false; break; }
                __int128 bb = fd_int128(&x.b);
                if (!any || bb > best) best = bb;
                any = true;
            }
            if (!ok || !any) continue;
            got.rhs = best;
        }
        // already verified as tight, or this very candidate already proposed (a tighter one not
        // yet verified does not make it redundant: it may fail)
        bool dup = false;
        for (int32_t v2 = 0; v2 < nv && !dup; v2++) dup = same_form(&as[v2], &got) && as[v2].fn == got.fn && as[v2].rhs <= got.rhs;
        for (int32_t v2 = nv; v2 < *nc && !dup; v2++) dup = same_form(&(*cl)[v2], &got) && (*cl)[v2].fn == got.fn && (*cl)[v2].rhs == got.rhs;
        if (dup) continue;
        if (*nc == *ccl) {
            int32_t nn = *ccl ? *ccl * 2 : 64;
            JClaim *x = realloc(*cl, sizeof *x * (size_t)nn);
            if (!x) return false;
            *cl = x; *ccl = nn;
        }
        (*cl)[(*nc)++] = got;
        F->proposed++;
    }
    return true;
}
// Whether a function of the module that `f`'s claims reach (its own, I.181) calls f.
static bool called_here(JFinder *F, IrFunc *f) {
    IrFunc *mod = F->mod;
    for (IrFunc *g = mod; g; g = g->next) {
        if (g->is_extern || !fd_same_module(g, f)) continue;
        for (IrBlock *b = g->blocks; b; b = b->next)
            for (IrInstr *i = b->instrs; i; i = i->next)
                if (i->op == IR_CALL && fd_callee(F, i) == f) return true;
    }
    return false;
}
// A function's return candidates: VRA's interval (its summary, after the analysis: ir.h's
// ret_range_state), each side of the returned value, and of its difference and sum with each
// integer parameter (`min(a, b) <= a`; `clamp(x, lo, hi) >= lo`). None for a function no caller
// here can use them in, or whose return may wrap, whose claims the generator would refuse.
static bool ret_candidates(JFinder *F, IrFunc *f, JCand **cd, int32_t *ncd, int32_t *ccd) {
    *ncd = 0;
    if (!fd_is_int(f->ret_type) || f->incomplete || !called_here(F, f)) return true;
    for (IrBlock *b = f->blocks; b; b = b->next)
        if (b->term.kind == IR_TERM_RET && b->term.cond && !fd_lands_exact(b->term.cond->type, f->ret_type, b->term.unchecked)) return true;
    FdRange tr;
    // a side tighter than the type's
    if (F->use_vra && f->ret_range_state == 2 && fd_range(f->ret_type, &tr))
        for (int sd = 1; sd >= -1; sd -= 2) {
            __int128 rhs = sd > 0 ? (__int128)f->ret_range_hi : -(__int128)f->ret_range_lo;
            if (rhs >= (sd > 0 ? tr.hi : -tr.lo)) continue;
            int32_t at = *ncd;
            if (!cand_push(cd, ncd, ccd, JCLAIM_RET, JKEY_RET, sd, -1, 0, f->ret_type, NULL)) return false;
            if (*ncd == at + 1) { (*cd)[at].fixed = true; (*cd)[at].c.rhs = rhs; }
        }
    if (!cand_push(cd, ncd, ccd, JCLAIM_RET, JKEY_RET, 1, -1, 0, f->ret_type, NULL) ||
        !cand_push(cd, ncd, ccd, JCLAIM_RET, JKEY_RET, -1, -1, 0, f->ret_type, NULL)) return false;
    for (IrParam *p = f->params; p; p = p->next) {
        if (!p->value || !fd_is_int(p->value->type)) continue;
        for (int sr = -1; sr <= 1; sr += 2)
            for (int sp = -1; sp <= 1; sp += 2)
                if (!cand_push(cd, ncd, ccd, JCLAIM_RET, JKEY_RET, sr, p->value->id, sp, f->ret_type, p->value->type)) return false;
    }
    for (int32_t c = 0; c < *ncd; c++) (*cd)[c].c.fn = f;
    return true;
}
// The module's claims, in ROUNDS (each under the claims the previous one verified only): every
// function's cut-point candidates and return candidates, bounded, then Houdini over them all, since
// a return claim may need its function's loop invariants and a loop a callee's return claim.
static JClaim *finder_propose(JFinder *F, int32_t *ncl) {
    IrFunc *mod = F->mod;
    char err[200];
    JClaim *cl = NULL, *as = NULL;
    JCand *cd = NULL;
    int32_t nc = 0, ccl = 0, nv = 0, ncd = 0, ccd = 0;
    *ncl = 0;
    F->oom = false;
    F->rk = NULL; F->nrk = 0;                                // the caller frees the last call's
    for (int round = 0; round < 3; round++) {
        if (nv > ccl) { JClaim *x = realloc(cl, sizeof *x * (size_t)nv); if (!x) goto oom; cl = x; ccl = nv; }
        if (nv) memcpy(cl, as, sizeof *cl * (size_t)nv);
        nc = nv;
        for (IrFunc *f = mod; f; f = f->next) {
            if (f->is_extern || !f->entry) continue;
            JSlot *s0; int32_t ns = 0;
            JSlot *sl = NULL;
            if (F->gen->slots(F->gen->ctx, f, &s0, &ns, err, sizeof err) && ns > 0) {
                if (!(sl = malloc(sizeof *sl * (size_t)ns))) goto oom;
                memcpy(sl, s0, sizeof *sl * (size_t)ns);
            }
            // the cut points, block by block, then the returns; each under the verified claims
            for (int32_t i = 0, j; i <= ns; i = j) {
                int32_t only;
                if (i < ns) {
                    for (j = i; j < ns && sl[j].blk == sl[i].blk; j++) ;
                    if (!block_candidates(F, f, sl, i, j, &cd, &ncd, &ccd, err, sizeof err)) continue;
                    for (int32_t c = 0; c < ncd; c++) cd[c].c.fn = f;
                    only = sl[i].blk;
                } else {
                    j = ns + 1;
                    if (!ret_candidates(F, f, &cd, &ncd, &ccd)) { free(sl); goto oom; }
                    only = JGEN_POST;
                }
                if (ncd == 0) continue;
                JClaim *q2 = malloc(sizeof *q2 * (size_t)(nv + ncd + 1));
                if (!q2) { free(sl); goto oom; }
                if (nv) memcpy(q2, as, sizeof *q2 * (size_t)nv);
                for (int32_t c = 0; c < ncd; c++) q2[nv + c] = cd[c].c;
                JClaimVc *o; int32_t n;
                bool gok = F->gen->claim_vcs(F->gen->ctx, f, q2, nv + ncd, only, &o, &n, err, sizeof err);
                free(q2);
                if (gok && !bound_candidates(F, cd, ncd, o, n, nv, as, nv, &cl, &nc, &ccl)) { free(sl); goto oom; }
                // a header's back edge from ANY previous trip: each side of each header version
                // alone, under the verified claims only (at their placeholders the header's
                // comparisons, give or take one, contradict each other, and the back edge bounds
                // nothing): `k = next(k)` with `next(k) <= 7` keeps `k <= 7` whatever k was
                if (gok && i < ns && sl[i].head) {
                    int32_t n1 = 0;
                    for (int32_t c = 0; c < ncd; c++) if (!cd[c].fixed && cd[c].c.nt == 1) cd[n1++] = cd[c];
                    if (n1 > 0) {
                        if (!(q2 = malloc(sizeof *q2 * (size_t)(nv + n1)))) { free(sl); goto oom; }
                        if (nv) memcpy(q2, as, sizeof *q2 * (size_t)nv);
                        for (int32_t c = 0; c < n1; c++) { q2[nv + c] = cd[c].c; q2[nv + c].rhs = cd[c].tb; }
                        gok = F->gen->claim_vcs(F->gen->ctx, f, q2, nv + n1, only, &o, &n, err, sizeof err);
                        free(q2);
                        if (gok && !bound_candidates(F, cd, n1, o, n, nv, as, nv, &cl, &nc, &ccl)) { free(sl); goto oom; }
                    }
                }
            }
            free(sl);
        }
        if (nc == nv) break;                                 // nothing new to verify
        int32_t kept = houdini_module(F, cl, nc);
        if (kept < 0) goto oom;
        bool grew = kept > nv;
        free(as); as = malloc(sizeof *as * (size_t)(kept ? kept : 1));
        if (!as) goto oom;
        memcpy(as, cl, sizeof *as * (size_t)kept);
        nv = kept;
        if (!grew) break;
    }
    free(cd); free(cl);
    // PE.5: a ranking claim per loop header, under the claims Houdini kept (owed, never believed)
    for (IrFunc *f = mod; f; f = f->next) {
        if (f->is_extern || !f->entry) continue;
        for (IrBlock *b = f->blocks; b; b = b->next) {
            if (!b->is_loop_header) continue;
            FdLoop L; JRank r;
            bool got = fd_loop_build(F, f, b->id, as, nv, &L) && fd_rank(F, f, b->id, &L, &r);
            free(L.mem);
            if (!got) continue;
            JRank *x = realloc(F->rk, sizeof *x * (size_t)(F->nrk + 1));
            if (!x) goto oom;
            F->rk = x; F->rk[F->nrk++] = r;
        }
        // ...and a measure for its recursion, if it has one to rank: one component over its
        // parameters at entry, falling at every self-call (the judge owes rec.fall and rec.bound)
        FdLoop L; JRank r;
        bool got = fd_loop_build(F, f, JRANK_ENTRY, as, nv, &L) && fd_rank(F, f, JRANK_ENTRY, &L, &r) && r.ncomp == 1;
        free(L.mem);
        if (!got) continue;
        JRank *x = realloc(F->rk, sizeof *x * (size_t)(F->nrk + 1));
        if (!x) goto oom;
        F->rk = x; F->rk[F->nrk++] = r;
    }
    F->kept += nv;
    *ncl = nv;
    if (nv == 0) { free(as); return NULL; }
    return as;
oom:
    free(cd); free(cl); free(as); free(F->rk); F->rk = NULL; F->nrk = 0;
    F->oom = true;
    return NULL;
}


#endif // LAIN_SOLVER_FINDER_H
