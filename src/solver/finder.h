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
#include "ir/callgraph.h"                 // a recursion cycle's members, ranked jointly (fd_rank_cycle)
#include "ir/lap_only.h"                  // PE.12: a witness's cycle leaves out what only the loop's counting reads

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
    uint64_t meter; bool meter_out;      // the last call's metered work (lia.h), and whether any account ran out
    uint64_t meter_cap;                  // the work allowed each account; 0: FD_WORK (a test sets it smaller)
    // the last call's accounts that ran out (finder_propose): the functions whose own search did, at
    // most 16 of them (out_more: others too), and the module's (Houdini, guarded claims, the cycles)
    IrFunc *out_fn[16]; int32_t nout_fn; bool out_more, out_module;
    JGuardClaim *gc; int32_t ngc;        // the last call's guarded return claims (malloc'd; the caller frees)
    bool oom;                            // the last call returned no claims because memory ran out
    JRank *rk; int32_t nrk;              // the last call's ranking claims (malloc'd; the caller frees)
    const char **rkstage;                // with fd_track_stages: the stage that ranked each (a label)
    struct FdMemo *memo;                 // during a call: the solver's answers by VC (fd_implied)
    bool keep_dead;                      // fd_loop_from keeps the paths the solver refutes (f64)
    // during B1's last round: the guarded return claims every claim VC states at its calls (gc)
    const JGuardClaim *hgc; int32_t nhgc;
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
// The slice whose length VRA's variable `vid` is (an IR_SLICE_LEN's result), its id; -1 otherwise.
static int32_t fd_len_of(IrFunc *f, int32_t vid, const IrType **lt) {
    for (IrBlock *b = f->blocks; b; b = b->next)
        for (IrInstr *i = b->instrs; i; i = i->next)
            if (i->result && i->result->id == vid) {
                if (i->op != IR_SLICE_LEN || i->n_operands < 1 || !i->operands[0]) return -1;
                *lt = i->result->type;
                return i->operands[0]->id;
            }
    return -1;
}
static bool same_form(const JClaim *a, const JClaim *b) {
    if (a->blk != b->blk || a->nt != b->nt) return false;
    for (int k = 0; k < a->nt; k++) if (a->key[k] != b->key[k] || a->tk[k] != b->tk[k] || a->coef[k] != b->coef[k]) return false;
    return true;
}
// ── Questions asked again ─────────────────────────────────────────────────────────────────────
// Houdini's iterations and the finder's rounds put the same VC to the solver again: a candidate one
// round dropped is proposed again by the next and dropped the same way (zune.ln: each of the three
// rounds' Houdini repeated the last one's five iterations), and a candidate's bound is maximized
// again in each round over VCs the new claims left as they were. An answer is a fact about the VC:
// implied (a certificate) or not (a model); the greatest value of its goal's sum, its hypotheses
// unsatisfiable, or no bound below lia_maximize's limit. So each is kept for the call, keyed by a
// 128-bit FINGERPRINT of the VC's numbers (its variable count, every hypothesis, the goal) and its
// function's name, and a VC met again is answered from it. A budget or capacity answer depends on
// what was left and is never kept; the name keeps one function's answers from charging another's
// account. Two VCs sharing a fingerprint would share an answer (for n VCs a chance about
// n^2 / 2^129): a non-inductive claim kept so is still owed by the judge, which refuses it loudly,
// and a true one dropped costs a proof, never soundness. Keeping the VCs themselves instead would
// take 125 MB on simdlex.ln.
typedef struct { uint64_t a, b; } FdPrint;
typedef struct { FdPrint p; uint8_t tag, k; JInt b; } FdMemoE;   // tag 1: lia_solve; 2: lia_maximize
struct FdMemo { FdMemoE *e; size_t cap, n; };
static uint64_t fd_mix(uint64_t x) {                     // splitmix64's finalizer
    x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ull; x ^= x >> 27; x *= 0x94d049bb133111ebull; return x ^ (x >> 31);
}
static void fd_fpw(FdPrint *p, uint64_t x) { p->a = fd_mix(p->a ^ x); p->b = fd_mix(p->b + (x ^ 0x9e3779b97f4a7c15ull)); }
static void fd_fpint(FdPrint *p, const JInt *v) {
    fd_fpw(p, (uint64_t)v->neg << 8 | v->n);
    for (int i = 0; i < v->n; i++) fd_fpw(p, v->limb[i]);
}
static void fd_fpcons(FdPrint *p, const JCons *c, bool sum_only) {
    fd_fpw(p, (uint64_t)(uint32_t)c->nt);
    if (!sum_only) { fd_fpw(p, (uint64_t)c->op); fd_fpint(p, &c->rhs); }
    for (int32_t i = 0; i < c->nt; i++) { fd_fpw(p, (uint64_t)(uint32_t)c->t[i].var); fd_fpint(p, &c->t[i].coef); }
}
// lia_maximize reads its goal's sum only: the operator and constant are left out of tag 2's
static FdPrint fd_print(const JVc *v, uint8_t tag) {
    FdPrint p = { 0x243f6a8885a308d3ull, 0x13198a2e03707344ull };
    fd_fpw(&p, tag);
    for (const char *s = v->id.fn ? v->id.fn : ""; *s; s++) fd_fpw(&p, (unsigned char)*s);
    fd_fpw(&p, 0x100);
    fd_fpw(&p, (uint64_t)(uint32_t)v->nvars); fd_fpw(&p, (uint64_t)(uint32_t)v->nhyps);
    for (int32_t i = 0; i < v->nhyps; i++) {
        const JHyp *y = &v->hyps[i];
        fd_fpw(&p, y->disj);
        if (!y->disj) { fd_fpcons(&p, &y->cons, false); continue; }
        fd_fpw(&p, (uint64_t)(uint32_t)y->ndis);
        for (int32_t d = 0; d < y->ndis; d++) {
            fd_fpw(&p, (uint64_t)(uint32_t)y->dis[d].nc);
            for (int32_t k = 0; k < y->dis[d].nc; k++) fd_fpcons(&p, &y->dis[d].c[k], false);
        }
    }
    fd_fpcons(&p, &v->goal, tag == 2);
    return p;
}
static const FdMemoE *fd_memo_get(const JFinder *F, FdPrint p, uint8_t tag) {
    const struct FdMemo *M = F->memo;
    if (!M || !M->cap) return NULL;
    for (size_t i = p.a & (M->cap - 1); M->e[i].tag; i = (i + 1) & (M->cap - 1))
        if (M->e[i].tag == tag && M->e[i].p.a == p.a && M->e[i].p.b == p.b) return &M->e[i];
    return NULL;
}
static void fd_memo_put(JFinder *F, FdPrint p, uint8_t tag, uint8_t k, const JInt *b) {   // no memory: not kept
    struct FdMemo *M = F->memo;
    if (!M) return;
    if (2 * (M->n + 1) > M->cap) {
        size_t nc = M->cap ? 2 * M->cap : 1024;
        FdMemoE *ne = calloc(nc, sizeof *ne);
        if (!ne) return;
        for (size_t i = 0; i < M->cap; i++) {
            if (!M->e[i].tag) continue;
            size_t j = M->e[i].p.a & (nc - 1);
            while (ne[j].tag) j = (j + 1) & (nc - 1);
            ne[j] = M->e[i];
        }
        free(M->e); M->e = ne; M->cap = nc;
    }
    size_t i = p.a & (M->cap - 1);
    while (M->e[i].tag) i = (i + 1) & (M->cap - 1);
    memset(&M->e[i], 0, sizeof M->e[i]);
    M->e[i].p = p; M->e[i].tag = tag; M->e[i].k = k;
    if (b) M->e[i].b = *b;
    M->n++;
}
// Whether v is implied (lia_solve answers a certificate) within `lim`, from the memo when v was
// asked before. A budget answer is not kept: a larger budget may still decide v.
static bool fd_implied_in(JFinder *F, const JVc *v, JWork lim) {
    FdPrint p = fd_print(v, 1);
    const FdMemoE *e = fd_memo_get(F, p, 1);
    if (e) return e->k == J_ANS_CERT;
    JAnswerKind k = lia_solve(v, lim, F->work, F->worksz).k;
    if (k == J_ANS_CERT || k == J_ANS_MODEL) fd_memo_put(F, p, 1, (uint8_t)k, NULL);
    return k == J_ANS_CERT;
}
static bool fd_implied(JFinder *F, const JVc *v) { return fd_implied_in(F, v, F->lim); }
// lia_maximize of v's goal sum over its hypotheses: the kind and the bound only (no certificate,
// no model: a caller reads x.k and x.b), from the memo when v was asked before.
static LMax fd_max_goal(JFinder *F, const JVc *v) {
    FdPrint p = fd_print(v, 2);
    const FdMemoE *e = fd_memo_get(F, p, 2);
    LMax x;
    if (e) { memset(&x, 0, sizeof x); x.k = (LMaxKind)e->k; x.b = e->b; return x; }
    x = lia_maximize(v, v->goal.t, v->goal.nt, F->lim, F->work, F->worksz);
    if (x.k != LIA_MAX_STOP) fd_memo_put(F, p, 2, (uint8_t)x.k, &x.b);
    return x;
}
// Houdini over the module's claims cl[0..nc): each function's claim VCs (its cut points' and its
// returns'), solved; a claim one of them is not implied by is dropped, and the rest solved again,
// until none is. The claims left, in order. A function the generator refuses has none of its own,
// and its final generation reports the refusal. The first `nold` claims were verified in an
// earlier round: when the generator refuses a function's set (a claim it cannot state, or more
// VCs than its region holds), only the function's NEW claims go first, so a round's candidates
// never cost what earlier rounds kept (2519 candidates once cost the SIMD lexer's `tokenize` its
// 142 claims: "out of memory building a VC").
// INCREMENTAL: an iteration re-solves only the functions whose VCs can have changed. A function's
// claim VCs read its own claims and, at its calls, the return claims of its callees; one whose own
// claims all held and none of whose callees lost a claim has the same VCs next time, all implied
// again. So after the first iteration, a function is solved again only if a claim of its own was
// dropped, it calls a function that lost one, or it calls through a pointer (any target may have).
// Exact in the claims kept. simdlex.ln: Houdini was 32% of the judge's run, every iteration
// solving every function's VCs again.
// `first`: the functions whose VCs can have changed (B1's last round: its candidates' functions),
// in the module's order, or NULL for every function.
static int32_t houdini_module(JFinder *F, JClaim *cl, int32_t nc, int32_t nold, const uint8_t *first) {
    char err[200];
    int32_t nfn = 0;
    for (IrFunc *f = F->mod; f; f = f->next) nfn++;
    uint8_t *dirty = malloc((size_t)(nfn ? nfn : 1)), *lost = calloc((size_t)(nfn ? nfn : 1), 1);
    if (!dirty || !lost) { free(dirty); free(lost); return -1; }
    if (first) memcpy(dirty, first, (size_t)nfn); else memset(dirty, 1, (size_t)(nfn ? nfn : 1));
    for (bool again = nc > 0; again; ) {
        again = false;
        uint8_t *bad = calloc((size_t)nc, 1);
        if (!bad) { free(dirty); free(lost); return -1; }
        int32_t fk = -1;
        for (IrFunc *f = F->mod; f; f = f->next) {
            fk++;
            if (f->is_extern || !f->entry || !dirty[fk]) continue;
            JClaimVc *o; int32_t n;
            if (!F->gen->claim_vcs(F->gen->ctx, f, cl, nc, F->hgc, F->nhgc, JGEN_CLAIMS, &o, &n, err, sizeof err)) {
                bool fresh = false;                          // this round's: drop those, then retry
                for (int32_t ci = nold; ci < nc; ci++) if (cl[ci].fn == f) { bad[ci] = 1; fresh = true; }
                if (!fresh) for (int32_t ci = 0; ci < nc; ci++) if (cl[ci].fn == f) bad[ci] = 1;
                continue;
            }
            for (int32_t q = 0; q < n; q++)                  // a guarded claim's own (hgc) is owed apart
                if (o[q].claim < nc && !bad[o[q].claim] && !fd_implied(F, &o[q].vc)) bad[o[q].claim] = 1;
        }
        // the functions that lost a claim, then next iteration's: those and their callers
        memset(lost, 0, (size_t)(nfn ? nfn : 1));
        bool any = false;
        for (int32_t ci = 0; ci < nc; ci++) {
            if (!bad[ci]) continue;
            int32_t k = 0;
            for (IrFunc *h = F->mod; h && h != cl[ci].fn; h = h->next) k++;
            if (k < nfn) lost[k] = 1;
            any = true;
        }
        fk = -1;
        for (IrFunc *f = F->mod; f; f = f->next) {
            fk++;
            dirty[fk] = lost[fk];
            if (!any || dirty[fk] || f->is_extern || !f->entry) continue;
            for (IrBlock *b = f->blocks; b && !dirty[fk]; b = b->next)
                for (IrInstr *i = b->instrs; i && !dirty[fk]; i = i->next) {
                    if (i->op != IR_CALL) continue;
                    const IrFunc *g = fd_callee(F, i);
                    if (!g) { dirty[fk] = 1; continue; }     // through a pointer (or unresolved): solve
                    int32_t kg = 0;
                    for (IrFunc *h = F->mod; h && h != g; h = h->next) kg++;
                    if (kg < nfn && lost[kg]) dirty[fk] = 1;
                }
        }
        int32_t keep = 0, kold = 0;
        for (int32_t ci = 0; ci < nc; ci++) {
            if (bad[ci]) { F->dropped++; again = true; continue; }
            if (ci < nold) kold++;
            cl[keep++] = cl[ci];
        }
        nc = keep; nold = kold;                              // the earlier rounds' still lead
        free(bad);
    }
    free(dirty); free(lost);
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
#define FD_MAXEDGES 64                      // a recursion cycle's calls, ranked jointly (fd_rank_cycle)
typedef struct { JCons c; int32_t *v; } FdCons;              // a constraint over a raw path's variables
typedef struct { bool disj, claim, upto, wrap; FdCons c; FdCons *dc; int32_t *dn; int32_t nd; } FdHyp;   // dn[k]: disjunct k's size; claim: a kept claim's; upto: a guarded EACH claim's cases; wrap: a wrapping operation's cases
typedef struct { int32_t nv, *val, nh; FdHyp *h; int32_t from, *pre, *post; } FdRaw;
typedef struct {
    char *mem, *cur, *end;
    int32_t npairs, *pkey, *pre, *post;     // in the merged list; pkey: the cell or the value id
    uint8_t *ptk;                           // each pair's term kind (JTERM_LEN: a length)
    int32_t *parg;                          // and its argument (JTERM_MOD: the modulus k), else 0;
                                            // NULL on fd_rank's subset loops
    int32_t nv; const char **names;
    const JVc **paths; int32_t npaths, *edge;
    JVc **full;                             // each path before slicing, for a retry on fewer pairs
    int32_t nedges, edges[FD_MAXEDGES];     // a loop's back edges (at most JRANK_MAXE), a cycle's calls
    int32_t npk;                            // the first npk pairs are the header's versions (cells)
    FdRaw *raw; int32_t nraw;               // the back edges as the generator gave them, disjunctions
                                            // whole (the flattening above drops some); NULL on subsets
    uint8_t *moves;                         // per version: some path gives it a new value (its pre and
                                            // post differ there); NULL on fd_rank's subset loops, whose
                                            // pairs are renumbered
    bool dead;                              // every flattened path the solver refuted: no trip can
                                            // run, and a constant measure falls on every one (c2)
    uint8_t *wrapped;                       // per path: some wrapping operation took a case past its
                                            // type (fd_karr_candidates); NULL on subset loops
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
// what is found without it holds with it: candidates are lost at worst, never made wrong. A
// COEFFICIENT may reach 2^32: an i32 threshold return claim, (2^31 + 1)*ret - i <= 2^31, is what
// NestedRecursion's measure rests on (the inner call's argument at most 0), while a u32 bound's
// constant 2^32 - 1 stays out.
// A constant the PROGRAM names is kept further, to 2^62: it reaches a path as its value's
// definition, `v = 2147483649`, one term of coefficient 1. Dropped with the bounds, `while i <
// 2147483649 { i = i + 1 }` over an i64 kept the test `i - v <= -1` and lost what v is, so no
// ranking existed, though 2147483648 - i is one (TCW, 2026-10-25: 2^31 accepted, 2^31 + 1 refused).
// One such constant beside small ones stays well inside 128 bits; a system that does not lands in
// CAPACITY, which lia_rank answers with its sample search.
#define FD_BIGC ((LSi)1 << 31)
#define FD_BIGK ((LSi)1 << 32)
#define FD_BIGD ((LSi)1 << 62)
static bool fd_small(const JCons *c) {
    LSol N; memset(&N, 0, sizeof N);
    LSi r = ls_from_j(&N, &c->rhs);
    bool def = c->op == J_EQ && c->nt == 1 && !N.cap;          // v = constant
    if (def) { LSi a = ls_from_j(&N, &c->t[0].coef); def = !N.cap && (a == 1 || a == -1); }
    if (N.cap || ((r > FD_BIGC || r < -FD_BIGC) && !(def && r <= FD_BIGD && r >= -FD_BIGD))) return false;
    for (int32_t i = 0; i < c->nt; i++) {
        LSi a = ls_from_j(&N, &c->t[i].coef);
        if (N.cap || a > FD_BIGK || a < -FD_BIGK) return false;
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
static bool fd_loop_from(JFinder *F, IrFunc *f, int32_t H, const JPaths *Pp, FdLoop *L, int32_t maxedges);
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
    return fd_loop_from(F, f, H, &P, L, JRANK_MAXE);
}
// ...from paths already in hand (the generator's, or a recursion cycle's built by fd_rank_cycle),
// in L's region, at most `maxedges` of them
static bool fd_loop_from(JFinder *F, IrFunc *f, int32_t H, const JPaths *Pp, FdLoop *L, int32_t maxedges) {
    char err[200];
    const JPaths P = *Pp;
    bool entry = H == JRANK_ENTRY;
    // 1. the back edges, copied out of the generator's region (the next call reuses it)
    int32_t nraw = 0;
    for (int32_t k = 0; k < P.n; k++) if (P.p[k].back) nraw++;
    if (nraw == 0 || nraw > maxedges || nraw > FD_MAXEDGES) return false;
    FdRaw *raw = fd_bump(L, sizeof *raw * (size_t)nraw);
    int32_t npk = -1, *pk = NULL, *pkarg = NULL; uint8_t *pktk = NULL;
    if (!raw) return false;
    int32_t r = 0;
    for (int32_t k = 0; k < P.n; k++) {
        const JPath *pp = &P.p[k];
        if (!pp->back) continue;
        if (npk < 0) {                                       // the header's versions, by key
            npk = pp->npairs; pk = fd_bump(L, sizeof *pk * (size_t)(npk ? npk : 1));
            pktk = fd_bump(L, (size_t)(npk ? npk : 1));
            pkarg = fd_bump(L, sizeof *pkarg * (size_t)(npk ? npk : 1));
            if (!pk || !pktk || !pkarg) return false;
            for (int32_t q = 0; q < npk; q++) {
                pk[q] = pp->pairs[q].key; pktk[q] = pp->pairs[q].tk;
                pkarg[q] = pp->pairs[q].tk == JTERM_MOD ? pp->pairs[q].arg : 0;
            }
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
            for (int32_t z = 0; z < pp->npairs; z++)
                if (pp->pairs[z].key == pk[q] && pp->pairs[z].tk == pktk[q] && (pktk[q] != JTERM_MOD || pp->pairs[z].arg == pkarg[q])) at = z;
            if (at < 0 || pp->pairs[at].pre < 0 || pp->pairs[at].post < 0) return false;
            w->pre[q] = pp->pairs[at].pre; w->post[q] = pp->pairs[at].post;
        }
        for (int32_t x = 0; x < v->nvars; x++) w->val[x] = fd_value_id(v->vars[x]);
        for (int32_t q = 0; q < npk; q++) w->val[w->pre[q]] = w->val[w->post[q]] = -1;
        for (int32_t h = 0; h < v->nhyps; h++) {
            const JHyp *hy = &v->hyps[h];
            FdHyp *o = &w->h[h];
            o->disj = hy->disj; o->claim = hy->origin && !strncmp(hy->origin, "claim", 5);
            // a guarded EACH claim's instance as gen.h states it, `claim bbH #ci upto`: an
            // interface with the Secondary Compiler Worker, the whole form matched
            size_t ol = hy->origin ? strlen(hy->origin) : 0;
            o->upto = hy->disj && ol >= 5 && !strncmp(hy->origin, "claim bb", 8) && !strcmp(hy->origin + ol - 5, " upto");
            o->wrap = hy->disj && ol >= 5 && !strcmp(hy->origin + ol - 5, ".wrap");   // gen.h's `def %vN add.wrap`, ...
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
            // a guarded instance the path DECIDES: where the path's own facts refute the guard
            // case (`i <= k` beside a stated `i >= 5`, c34), it is the bound alone, and the relation
            // search reads it (a sum over an array a finished loop filled); undecided, it stays
            // skipped. One satisfiability question per case, the path's conjunctive facts beside it.
            // ...and ANY disjunction the path decides, at a sixteenth of the budget: `x != 0` is
            // two cases, and beside a stated `x > 5` only one lives. Undecided, each doubled the
            // flattened paths, and two of them past FD_MAXFLAT (32) dropped the widest facts and
            // the loop's ranking with them (TCW's flip check, fuzz_loops seed 20261021 k59, k282:
            // terminating, ranked by VRA, E011 under the judge).
            {
                JWork dlim = F->lim;
                if (!o->upto) { dlim.pivot /= 16; dlim.node /= 16; dlim.leaf /= 16; }
                int32_t ncj = 0, mx = 0;
                for (int32_t h2 = 0; h2 < v->nhyps; h2++) if (!v->hyps[h2].disj) ncj++;
                for (int32_t d = 0; d < hy->ndis; d++) if (hy->dis[d].nc > mx) mx = hy->dis[d].nc;
                JHyp *ph = malloc(sizeof *ph * (size_t)(ncj + mx + 1));
                int32_t live = -1, nlive = 0;
                for (int32_t d = 0; ph && d < hy->ndis && nlive < 2; d++) {
                    int32_t m = 0;
                    for (int32_t h2 = 0; h2 < v->nhyps; h2++) if (!v->hyps[h2].disj) ph[m++] = v->hyps[h2];
                    for (int32_t c = 0; c < hy->dis[d].nc; c++) { memset(&ph[m], 0, sizeof ph[m]); ph[m].cons = hy->dis[d].c[c]; ph[m].origin = "case"; m++; }
                    JVc q = *v; q.hyps = ph; q.nhyps = m;
                    memset(&q.goal, 0, sizeof q.goal); q.goal.op = J_LE; fd_jint(&q.goal.rhs, -1);
                    if (lia_solve(&q, dlim, F->work, F->worksz).k != J_ANS_CERT) { live = d; nlive++; }
                }
                free(ph);
                if (nlive == 1) {                            // decided: that case alone, flattened as one
                    int32_t off = 0;
                    for (int32_t d = 0; d < live; d++) off += o->dn[d];
                    o->dn[0] = o->dn[live];
                    for (int32_t c = 0; c < o->dn[0]; c++) o->dc[c] = o->dc[off + c];
                    o->nd = 1; o->upto = false;
                }
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
    // which versions a trip may change: a path leaving one unchanged gives it ONE variable (pre ==
    // post); every version pair has its own two slots below, so the merged pre and post never say it
    L->moves = fd_bump(L, (size_t)(npk ? npk : 1));
    if (!L->moves) return false;
    for (int32_t k = 0; k < nraw; k++)
        for (int32_t q = 0; q < npk; q++) if (raw[k].pre[q] != raw[k].post[q]) L->moves[q] = 1;
    int32_t nv = 2 * npk + nshared, base_local = nv;
    for (int32_t k = 0; k < nraw; k++) nv += raw[k].nv;
    L->nv = nv;
    L->names = fd_bump(L, sizeof(char *) * (size_t)(nv ? nv : 1));
    L->npairs = npk + nshared;
    L->ptk = fd_bump(L, (size_t)(L->npairs ? L->npairs : 1));
    L->parg = fd_bump(L, sizeof(int32_t) * (size_t)(L->npairs ? L->npairs : 1));
    L->pkey = fd_bump(L, sizeof(int32_t) * (size_t)(L->npairs ? L->npairs : 1));
    L->pre = fd_bump(L, sizeof(int32_t) * (size_t)(L->npairs ? L->npairs : 1));
    L->post = fd_bump(L, sizeof(int32_t) * (size_t)(L->npairs ? L->npairs : 1));
    char *nb = fd_bump(L, (size_t)nv * 16);
    if (!L->names || !L->pkey || !L->ptk || !L->parg || !L->pre || !L->post || !nb) return false;
    for (int32_t v = 0; v < nv; v++) { snprintf(nb + (size_t)v * 16, 16, "m%d", v); L->names[v] = nb + (size_t)v * 16; }
    for (int32_t q = 0; q < npk; q++) { L->pkey[q] = pk[q]; L->ptk[q] = pktk[q]; L->parg[q] = pkarg[q]; L->pre[q] = 2 * q; L->post[q] = 2 * q + 1; }
    for (int32_t q = 0; q < nshared; q++) {
        L->pkey[npk + q] = vals[q] / 2; L->ptk[npk + q] = (uint8_t)(vals[q] % 2); L->parg[npk + q] = 0;
        L->pre[npk + q] = L->post[npk + q] = 2 * npk + q;
    }
    // 4. each path flattened, in the merged variables
    L->paths = fd_bump(L, sizeof(JVc *) * FD_MAXFLAT);
    L->edge = fd_bump(L, sizeof(int32_t) * FD_MAXFLAT);
    L->full = fd_bump(L, sizeof(JVc *) * FD_MAXFLAT);
    L->wrapped = fd_bump(L, FD_MAXFLAT);
    if (!L->paths || !L->edge || !L->full || !L->wrapped) return false;
    int32_t local = base_local, ndead = 0;
    for (int32_t k = 0; k < nraw; k++) {
        FdRaw *w = &raw[k];
        int32_t *map = fd_bump(L, sizeof(int32_t) * (size_t)(w->nv ? w->nv : 1));
        if (!map) return false;
        for (int32_t x = 0; x < w->nv; x++) {
            map[x] = -1;
            // (two pairs with ONE pre variable cannot happen: one pair per parameter or cell, each its
            // own value on entry, so a pre never needs the tie below)
            // a quantity the path leaves unchanged has ONE variable for both sides (a recursive call
            // passing a parameter through, `f(lo + 1, hi)`): it is the pre-state's, and the post's is
            // tied to it below, or `hi` would have no pre-state and no measure could read it. So is
            // one pair's pre that is ANOTHER's post (`p(m, r - 1, n)` passes n as r; a loop's `x = y`
            // leaves y): the pre-state's, every pre before any post, and the other's post tied to it
            // below. Mapped to whichever pair came last, the pre lost its variable (LeeJonesBen-Amram
            // Ex4: m + n + r falls at both calls, and no measure was found)
            for (int32_t q = 0; q < npk && map[x] < 0; q++) if (w->pre[q] == x) map[x] = 2 * q;
            for (int32_t q = 0; q < npk && map[x] < 0; q++) if (w->post[q] == x) map[x] = 2 * q + 1;
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
            // a guarded EACH claim's cases (`g <= k` or the bound, per element k) only say which
            // elements are bounded yet: a ranking or relation over the loop's counters reads none of
            // them, and sixteen two-way choices a claim crowd every other choice out (a loop that
            // keeps `each(a) upto(i)` at its own header lost its ranking to them)
            skip[h] = !touch || o->upto;
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
            bool wraps = false;                              // a wrap's case of k != 0: its equality's constant
            for (int32_t h = 0; h < w->nh; h++) {
                FdHyp *o = &w->h[h];
                if (!o->disj) { hy[m].origin = o->claim ? "path claim" : "path"; if (!fd_map_cons(L, &o->c.c, map, &hy[m].cons)) return false; m++; continue; }
                if (skip[h]) continue;                       // a dropped disjunction: the path, weakened
                int32_t pick = rest % o->nd; rest /= o->nd;
                int32_t z = 0;
                for (int32_t d = 0; d < pick; d++) z += o->dn[d];
                for (int32_t c = 0; c < o->dn[pick]; c++) {
                    hy[m].origin = "path case"; if (!fd_map_cons(L, &o->dc[z + c].c, map, &hy[m].cons)) return false; m++;
                    wraps |= o->wrap && o->dc[z + c].c.op == J_EQ && o->dc[z + c].c.rhs.n != 0;
                }
            }
            for (int32_t q = 0; q < npk; q++) {               // ...post - v <= 0 and v - post <= 0, v the
                int32_t at = map[w->post[q]];                // variable its post value was given
                if (at == 2 * q + 1) continue;
                int32_t lo = at < 2 * q + 1 ? at : 2 * q + 1, hi = at < 2 * q + 1 ? 2 * q + 1 : at;   // ascending
                for (int sg = 0; sg < 2; sg++) {
                    JTerm *t = fd_bump(L, sizeof(JTerm) * 2);
                    if (!t) return false;
                    t[0].var = lo; fd_jint(&t[0].coef, sg ? 1 : -1);
                    t[1].var = hi; fd_jint(&t[1].coef, sg ? -1 : 1);
                    memset(&hy[m], 0, sizeof hy[m]);
                    hy[m].origin = at == 2 * q ? "unchanged" : "passed"; hy[m].cons.t = t; hy[m].cons.nt = 2; hy[m].cons.op = J_LE; fd_jint(&hy[m].cons.rhs, 0);
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
            // under a sixteenth of the per-query budget, as the finder's other side questions: a stop
            // keeps the path, which is what the full budget's stop did, at 65e6 operations each on
            // isqrt.ln's `main` (`% 100000` and a wrap's case on six of its paths: 393e6 of 5e8)
            JWork dlim = F->lim; dlim.pivot /= 16; dlim.node /= 16; dlim.leaf /= 16;
            if (!F->keep_dead && lia_solve(vc, dlim, F->work, F->worksz).k == J_ANS_CERT) { ndead++; continue; }
            fd_jint(&vc->goal.rhs, 0);                       // 0 <= 0: nothing is asked of it
            JVc *fv = fd_bump(L, sizeof *fv); JHyp *fh = fd_bump(L, sizeof(JHyp) * (size_t)(m ? m : 1));
            if (!fv || !fh) return false;
            memcpy(fh, hy, sizeof(JHyp) * (size_t)m); *fv = *vc; fv->hyps = fh; fv->nhyps = m;
            L->full[L->npaths] = fv;
            vc->nhyps = fd_slice(L, hy, m, nv);
            bool dup = false;
            for (int32_t z = 0; z < L->npaths && !dup; z++) dup = L->edge[z] == w->from && fd_same_path(L->paths[z], vc);
            if (dup) continue;
            L->paths[L->npaths] = vc; L->edge[L->npaths] = w->from; L->wrapped[L->npaths] = wraps; L->npaths++;
        }
    }
    L->dead = L->npaths == 0 && ndead > 0;
    L->raw = raw; L->nraw = nraw;
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
// at once, once per header version: that one quantity ALONE first (its paths cut to the facts that
// touch it, then two rounds out), then with the values fixed at the header. A counter's `n - i` is found from i and n;
// `12 - month` from month alone, which matters once a loop reads a table: its cells' claims ride in
// with the other quantities and pass the system's capacity (convert_days' month loop: 16 pairs,
// 736 hypotheses, capacity; month alone ranks it).
// PE.13 (2), with the generator's MAX components (c36): one component that is the max of a few
// rows, for a loop that moves a quantity toward 0 from either side (CookSeeZuleger Fig8a's
// `while x != 0`, x stepping down when positive and up when negative: max(x, -x)) or lowers two at
// once under `x >= 0 or y >= 0` (max(x, y)). Templates over the versions that step by a constant:
// max(v, -v), max(v, w), max(-v, -w). Each is checked here first, on every back-edge path, with
// the generator's own VC shapes (keep/fall per row under its premises, the bound under its
// premises): a ranking claim that fails is a refusal, where no claim is only "outside".
typedef struct { int32_t q[2]; int8_t c[2]; int32_t n; } FdRow;      // sum c[i] * pair q[i]
static int32_t fd_lin(JTerm *t, int32_t nt, int32_t var, int64_t c) {   // add c*var, ascending, merged
    for (int32_t i = 0; i < nt; i++) if (t[i].var == var) {
        __int128 v = fd_int128(&t[i].coef) + c;
        fd_jint(&t[i].coef, v);
        if (v == 0) { for (int32_t j = i; j + 1 < nt; j++) t[j] = t[j + 1]; nt--; }
        return nt;
    }
    int32_t at = nt;
    while (at > 0 && t[at - 1].var > var) { t[at] = t[at - 1]; at--; }
    t[at].var = var; fd_jint(&t[at].coef, c);
    return nt + 1;
}
// sp * row(pre or post) into t
static int32_t fd_row_terms(JTerm *t, int32_t nt, const FdLoop *L, const FdRow *r, bool post, int64_t sp) {
    for (int32_t i = 0; i < r->n; i++) nt = fd_lin(t, nt, post ? L->post[r->q[i]] : L->pre[r->q[i]], sp * r->c[i]);
    return nt;
}
static bool fd_max_holds(JFinder *F, const FdLoop *L, const FdRow *R, int32_t nr) {
    int32_t last = nr - 1;
    for (int32_t k = 0; k < L->npaths; k++) {
        const JVc *P = L->paths[k];
        JHyp *h = malloc(sizeof *h * (size_t)(P->nhyps + nr + 1));
        JTerm *tt = malloc(sizeof *tt * (size_t)(nr + 1) * 8);
        if (!h || !tt) { free(h); free(tt); F->oom = true; return false; }
        bool ok = true;
        for (int32_t a = 0; ok && a <= nr; a++) {             // a < nr: row a's fall; a == nr: the bound
            int32_t m = 0, used = 0;
            for (int32_t x = 0; x < P->nhyps; x++) h[m++] = P->hyps[x];
            for (int32_t b = 0; b < last; b++) {             // the premises: every row but the last
                JTerm *t = tt + (size_t)used * 8; int32_t nt = 0;
                memset(&h[m], 0, sizeof h[m]); h[m].origin = "premise";
                if (a < nr) { nt = fd_row_terms(t, nt, L, &R[b], false, 1); nt = fd_row_terms(t, nt, L, &R[a], true, -1); fd_jint(&h[m].cons.rhs, 0); }
                else { nt = fd_row_terms(t, nt, L, &R[b], false, 1); fd_jint(&h[m].cons.rhs, -1); }
                h[m].cons.t = t; h[m].cons.nt = nt; h[m].cons.op = J_LE;
                used++; m++;
            }
            JVc q = *P; q.hyps = h; q.nhyps = m;
            JTerm *g = tt + (size_t)used * 8; int32_t ng = 0;
            memset(&q.goal, 0, sizeof q.goal); q.goal.op = J_LE;
            if (a < nr) { ng = fd_row_terms(g, ng, L, &R[a], true, 1); ng = fd_row_terms(g, ng, L, &R[last], false, -1); fd_jint(&q.goal.rhs, -1); }
            else { ng = fd_row_terms(g, ng, L, &R[last], false, -1); fd_jint(&q.goal.rhs, 0); }
            q.goal.t = g; q.goal.nt = ng;
            ok = lia_solve(&q, F->lim, F->work, F->worksz).k == J_ANS_CERT;
        }
        free(h); free(tt);
        if (!ok) return false;
    }
    return true;
}
// A two-row MIN (c37): per pre row b, the last row's post at most f_b(pre) - 1, under the premise
// that row 0 did not do it, `f_0(post) >= f_0(pre) or f_0(post) >= f_1(pre)`, a disjunction the
// solver here does not read: split into its two cases, each its own question. Bounds: every row
// >= 0, no premise. Piecewise: min(p, q), q falling where q < p, p where p < q.
static bool fd_min_holds(JFinder *F, const FdLoop *L, const FdRow *R) {
    for (int32_t k = 0; k < L->npaths; k++) {
        const JVc *P = L->paths[k];
        JHyp *h = malloc(sizeof *h * (size_t)(P->nhyps + 2));
        JTerm *tt = malloc(sizeof *tt * 32);
        if (!h || !tt) { free(h); free(tt); F->oom = true; return false; }
        bool ok = true;
        for (int32_t b = 0; ok && b < 2; b++)                // the falls, one per pre row
            for (int32_t cs = 0; ok && cs < 2; cs++) {       // the premise's case: 0 vs row 0, 1 vs row 1
                int32_t m = 0;
                for (int32_t x = 0; x < P->nhyps; x++) h[m++] = P->hyps[x];
                JTerm *t = tt; int32_t nt = 0;               // f_cs(pre) - f_0(post) <= 0
                nt = fd_row_terms(t, nt, L, &R[cs], false, 1); nt = fd_row_terms(t, nt, L, &R[0], true, -1);
                memset(&h[m], 0, sizeof h[m]); h[m].origin = "premise"; h[m].cons.t = t; h[m].cons.nt = nt;
                h[m].cons.op = J_LE; fd_jint(&h[m].cons.rhs, 0); m++;
                JVc q = *P; q.hyps = h; q.nhyps = m;
                JTerm *g = tt + 16; int32_t ng = 0;          // f_1(post) - f_b(pre) <= -1
                ng = fd_row_terms(g, ng, L, &R[1], true, 1); ng = fd_row_terms(g, ng, L, &R[b], false, -1);
                memset(&q.goal, 0, sizeof q.goal); q.goal.op = J_LE; q.goal.t = g; q.goal.nt = ng; fd_jint(&q.goal.rhs, -1);
                ok = lia_solve(&q, F->lim, F->work, F->worksz).k == J_ANS_CERT;
            }
        for (int32_t b = 0; ok && b < 2; b++) {              // the bounds: -f_b(pre) <= 0
            JVc q = *P; JTerm *g = tt; int32_t ng = fd_row_terms(g, 0, L, &R[b], false, -1);
            memset(&q.goal, 0, sizeof q.goal); q.goal.op = J_LE; q.goal.t = g; q.goal.nt = ng; fd_jint(&q.goal.rhs, 0);
            ok = lia_solve(&q, F->lim, F->work, F->worksz).k == J_ANS_CERT;
        }
        free(h); free(tt);
        if (!ok) return false;
    }
    return true;
}
static bool fd_rank_max(JFinder *F, IrFunc *f, int32_t H, const FdLoop *L, const uint8_t *step, JRank *out) {
    if (L->nedges > JRANK_MAXE) return false;
    int32_t sv[4], ns = 0;
    for (int32_t z = 0; z < L->npk && ns < 4; z++) if (step[z] && L->ptk[z] == JTERM_VALUE) sv[ns++] = z;
    FdRow R[2];
    for (int32_t t = 0; t < ns + ns * (ns - 1); t++) {
        if (t < ns) { R[0] = (FdRow){ { sv[t], 0 }, { 1, 0 }, 1 }; R[1] = (FdRow){ { sv[t], 0 }, { -1, 0 }, 1 }; }
        else {
            int32_t u = t - ns, i = u / (ns - 1 > 0 ? ns - 1 : 1), j = u % (ns - 1 > 0 ? ns - 1 : 1);
            if (j >= i) j++;
            if (i >= j) continue;                            // each pair once, both signs below
            int8_t sg = (u % 2) ? -1 : 1;
            R[0] = (FdRow){ { sv[i], 0 }, { sg, 0 }, 1 }; R[1] = (FdRow){ { sv[j], 0 }, { sg, 0 }, 1 };
        }
        uint8_t kind = 1;                                    // 1: a max, 2: a min
        if (!fd_max_holds(F, L, R, 2)) {
            if (F->oom) return false;
            if (t < ns || R[0].c[0] < 0 || !fd_min_holds(F, L, R)) { if (F->oom) return false; continue; }
            kind = 2;                                        // min(v, w): both rows positive
        }
        memset(out, 0, sizeof *out);
        out->blk = H; out->fn = f; out->ncomp = 2; out->nedge = L->nedges;
        out->nt = R[0].q[0] == R[1].q[0] ? 1 : 2;
        out->key[0] = L->pkey[R[0].q[0]]; out->tk[0] = L->ptk[R[0].q[0]];
        if (out->nt == 2) { out->key[1] = L->pkey[R[1].q[0]]; out->tk[1] = L->ptk[R[1].q[0]]; }
        out->coef[0][0] = R[0].c[0];
        if (out->nt == 1) out->coef[1][0] = R[1].c[0]; else out->coef[1][1] = R[1].c[0];
        out->cont[1] = kind;
        for (int32_t e = 0; e < L->nedges; e++) { out->from[e] = L->edges[e]; out->fall[e] = 0; }
        return true;
    }
    return false;
}
// PE.13 (2), with the generator's REGIONS (c38): a PIECEWISE lexicographic ranking, for a loop whose
// paths test the sign of one header version and rank differently on each side (UrbanMine-ESOP2014
// Fig3: in x >= 1 the pair <x, y> falls, in x <= -1 the pair <y, -x>, and one path may jump from the
// second side to the first, never back). Two regions, each guarded by one atom `s*v >= t` that the
// paths themselves state on v's pre value, of opposite signs; region 1 is the side trips may leave
// for region 0. Every obligation the generator will owe is asked here first, on the flattened paths,
// which hold no more facts than its VCs (slices and dropped disjunctions only weaken them), so what
// holds here holds there: every trip starts in a region (rank.cover), none moves up (rank.region),
// and the paths that stay in a region fall in a tuple of their own (rank.lex: lia_rank_lex over
// those paths with both of the region's guards added). A ranking claim the judge refutes refuses
// the loop, so nothing is proposed that these questions did not show.
// A guard may also compare the version with another pair (CookSeeZuleger Fig8b's `x > M`, M fixed
// at the header): s*(v - w) >= t. And a region no path STAYS in (Fig8b's x > M: every trip from it
// sets x = 0 < M) owes its tuple only under guards no path meets: its own guard row serves.
typedef struct { int32_t q, w; int8_t s; int64_t t; } FdAtom;        // s * (pair q - pair w) >= t; w -1: s * q >= t
#define FD_MAXATOM 32
static void fd_atom_hyp(JHyp *h, JTerm *t, const FdLoop *L, const FdAtom *a, bool post, bool neg) {
    memset(h, 0, sizeof *h); h->origin = "region";                  // -s*v <= -t; negated, s*v <= t - 1
    int32_t vq = post ? L->post[a->q] : L->pre[a->q], nt = 1;
    t[0].var = vq; fd_jint(&t[0].coef, neg ? a->s : -a->s);
    if (a->w >= 0) {                                                // the terms ascending by variable
        int32_t vw = post ? L->post[a->w] : L->pre[a->w];
        t[1].var = vw; fd_jint(&t[1].coef, neg ? -a->s : a->s); nt = 2;
        if (vw < vq) { JTerm x = t[0]; t[0] = t[1]; t[1] = x; }
    }
    h->cons.t = t; h->cons.nt = nt; h->cons.op = J_LE; fd_jint(&h->cons.rhs, neg ? (__int128)a->t - 1 : -(__int128)a->t);
}
// P with the atoms added (each at pre or post, as stated or negated) is infeasible
static bool fd_refutes(JFinder *F, const JVc *P, const FdLoop *L, const FdAtom *const *a, const bool *post, const bool *neg, int32_t na) {
    JHyp *h = malloc(sizeof *h * (size_t)(P->nhyps + na)); JTerm *t = malloc(sizeof *t * (size_t)na * 2);
    if (!h || !t) { free(h); free(t); F->oom = true; return false; }
    if (P->nhyps) memcpy(h, P->hyps, sizeof *h * (size_t)P->nhyps);
    for (int32_t i = 0; i < na; i++) fd_atom_hyp(&h[P->nhyps + i], &t[2 * i], L, a[i], post[i], neg[i]);
    JVc q = *P; q.hyps = h; q.nhyps = P->nhyps + na;
    memset(&q.goal, 0, sizeof q.goal); q.goal.op = J_LE; fd_jint(&q.goal.rhs, -1);
    bool r = lia_solve(&q, F->lim, F->work, F->worksz).k == J_ANS_CERT;
    free(h); free(t);
    return r;
}
// Region g's tuple, its rows copied out of the solver's memory (the next search reuses it) into
// rc[j * npairs + i] and rc0[j]: false when no tuple the judge can state ranks the paths that stay
// in the region. With none staying, its own guard row.
static bool fd_region_tuple(JFinder *F, FdLoop *L, const FdAtom *g, __int128 *rc, __int128 *rc0, int32_t room, int32_t *n) {
    const JVc **sp = fd_bump(L, sizeof(JVc *) * (size_t)L->npaths);
    if (!sp) return false;
    int32_t ns = 0;
    for (int32_t k = 0; k < L->npaths; k++) {
        const JVc *P = L->paths[k];
        const FdAtom *a[2] = { g, g }; bool post[2] = { false, true }, neg[2] = { false, false };
        if (fd_refutes(F, P, L, a, post, neg, 2)) continue;          // leaves the region (or cannot run in it)
        if (F->oom) return false;
        JVc *v = fd_bump(L, sizeof *v); JHyp *h = fd_bump(L, sizeof(JHyp) * (size_t)(P->nhyps + 2));
        JTerm *t = fd_bump(L, sizeof(JTerm) * 4);
        if (!v || !h || !t) return false;
        if (P->nhyps) memcpy(h, P->hyps, sizeof *h * (size_t)P->nhyps);
        fd_atom_hyp(&h[P->nhyps], &t[0], L, g, false, false); fd_atom_hyp(&h[P->nhyps + 1], &t[2], L, g, true, false);
        *v = *P; v->hyps = h; v->nhyps = P->nhyps + 2;
        sp[ns++] = v;
    }
    if (ns == 0) {
        if (room < 1) return false;
        for (int32_t i = 0; i < L->npairs; i++) rc[i] = 0;
        rc[g->q] = g->s; if (g->w >= 0) rc[g->w] = -g->s;
        rc0[0] = -(__int128)g->t; *n = 1;
        return true;
    }
    LRankLex R = lia_rank_lex(sp, ns, L->pre, L->post, L->npairs, F->lim, F->work, F->worksz);
    bool fits = R.k == LIA_RANK_FOUND && R.ncomp <= room;
    for (int32_t j = 0; fits && j < R.ncomp; j++)
        for (int32_t i = 0; fits && i <= L->npairs; i++) {
            __int128 v;
            fits = fd_i128(i < L->npairs ? &R.coef[j][i] : &R.c0[j], &v);
        }
    if (R.k == LIA_RANK_FOUND && !fits)
        R = lia_rank_lex_bounded(sp, ns, L->pre, L->post, L->npairs, (int64_t)1 << 20, F->lim, F->work, F->worksz);
    if (R.k != LIA_RANK_FOUND || R.ncomp > room) return false;
    for (int32_t j = 0; j < R.ncomp; j++) {
        if (!fd_i128(&R.c0[j], &rc0[j])) return false;
        for (int32_t i = 0; i < L->npairs; i++) if (!fd_i128(&R.coef[j][i], &rc[(size_t)j * (size_t)L->npairs + (size_t)i])) return false;
    }
    *n = R.ncomp;
    return true;
}
static bool fd_rank_regions(JFinder *F, IrFunc *f, int32_t H, FdLoop *L, JRank *out) {
    if (L->nedges > JRANK_MAXE || L->npaths == 0) return false;
    FdAtom at[FD_MAXATOM]; int32_t na = 0, on[FD_MAXATOM], last[FD_MAXATOM];   // the atoms the paths state on a version's pre value, by how many paths
    for (int32_t i = 0; i < FD_MAXATOM; i++) last[i] = -1;
    for (int32_t k = 0; k < L->npaths; k++)
        for (int32_t x = 0; x < L->paths[k]->nhyps; x++) {
            const JVc *P = L->paths[k];
            const JCons *c = &P->hyps[x].cons;
            if (P->hyps[x].disj || c->op != J_LE || c->nt < 1 || c->nt > 2) continue;
            // `x > 0` reaches the path as x - z <= -1 beside z == 0 (the constant's own variable): a
            // term over a variable the path fixes to a constant is folded into the bound
            __int128 b, cf = 0, cw = 0; int32_t q = -1, w = -1; bool fold = true;
            if (!fd_i128(&c->rhs, &b)) continue;
            for (int32_t t = 0; t < c->nt && fold; t++) {
                int32_t v = c->t[t].var, z = -1;
                for (int32_t y = 0; y < L->npairs && z < 0; y++) if (L->pre[y] == v && L->ptk[y] == JTERM_VALUE) z = y;
                if (z >= 0 && q < 0) { q = z; cf = fd_int128(&c->t[t].coef); continue; }
                if (z >= 0 && w < 0) { w = z; cw = fd_int128(&c->t[t].coef); continue; }
                bool fixed = false;
                for (int32_t y = 0; y < P->nhyps && !fixed; y++) {
                    const JCons *e = &P->hyps[y].cons;
                    __int128 ev, ec;
                    if (P->hyps[y].disj || e->op != J_EQ || e->nt != 1 || e->t[0].var != v || !fd_i128(&e->rhs, &ev)) continue;
                    ec = fd_int128(&e->t[0].coef);
                    if (ec != 1 && ec != -1) continue;
                    b -= fd_int128(&c->t[t].coef) * (ec * ev); fixed = true;
                }
                fold = fixed;
            }
            // q the version of the lower index (a header's versions come first), w the other pair
            if (w >= 0 && w < q) { int32_t x = q; q = w; w = x; __int128 y = cf; cf = cw; cw = y; }
            if (!fold || q < 0 || q >= L->npk || (cf != 1 && cf != -1) || (w >= 0 && cw != -cf)
                || b > ((__int128)1 << 40) || b < -((__int128)1 << 40)) continue;
            FdAtom A = { q, w, (int8_t)(cf > 0 ? -1 : 1), (int64_t)-b };   // c*(v - w) <= b: -c*(v - w) >= -b
            int32_t seen = -1;
            for (int32_t i = 0; i < na && seen < 0; i++) if (at[i].q == A.q && at[i].w == A.w && at[i].s == A.s && at[i].t == A.t) seen = i;
            if (seen < 0 && na < FD_MAXATOM) { seen = na; at[na] = A; on[na++] = 0; }
            if (seen >= 0 && last[seen] != k) { last[seen] = k; on[seen]++; }
        }
    // a region tells paths apart: an atom every path states (a type's bound, the loop's guard) is
    // no case of the loop's, and eight of them crowded the cases out (an i32 port of Fig3)
    int32_t nk = 0;
    for (int32_t i = 0; i < na; i++) if (on[i] < L->npaths) at[nk++] = at[i];
    na = nk;
    int32_t np = L->npairs;
    __int128 *rc = fd_bump(L, sizeof(__int128) * (size_t)JRANK_MAXC * (size_t)(np ? np : 1) * 2);
    if (!rc) return false;
    __int128 *rc1 = rc + (size_t)JRANK_MAXC * (size_t)(np ? np : 1), rc0[2][JRANK_MAXC];
    char *mark = L->cur;                                     // each pair's scratch, reclaimed by the next
    for (int32_t i = 0; i < na; i++)
        for (int32_t j = 0; j < na; j++) {
            L->cur = mark;
            if (at[i].q != at[j].q || at[i].w != at[j].w || at[i].s == at[j].s) continue;   // one form, opposite sides
            const FdAtom *g0 = &at[i], *g1 = &at[j];
            bool ok = true;
            for (int32_t k = 0; k < L->npaths && ok; k++) {
                const FdAtom *a[2] = { g0, g1 };
                bool pre2[2] = { false, false }, neg2[2] = { true, true }, up[2] = { false, true }, as[2] = { false, false };
                ok = fd_refutes(F, L->paths[k], L, a, pre2, neg2, 2)        // rank.cover: in some region
                  && fd_refutes(F, L->paths[k], L, a, up, as, 2);           // rank.region: 0 never to 1
            }
            if (F->oom) return false;
            int32_t n0 = 0, n1 = 0;
            if (!ok || !fd_region_tuple(F, L, g0, rc, rc0[0], JRANK_MAXC - 3, &n0)) { if (F->oom) return false; continue; }
            if (!fd_region_tuple(F, L, g1, rc1, rc0[1], JRANK_MAXC - 2 - n0, &n1)) { if (F->oom) return false; continue; }
            // the rows: region 0's guard and tuple, then region 1's; the terms, every pair a row reads
            int32_t idx[JRANK_MAXT], nt = 0;
            for (int32_t z = 0; z < np; z++) {
                bool used = z == g0->q || z == g0->w;
                for (int32_t r = 0; r < n0 && !used; r++) used = rc[(size_t)r * (size_t)np + (size_t)z] != 0;
                for (int32_t r = 0; r < n1 && !used; r++) used = rc1[(size_t)r * (size_t)np + (size_t)z] != 0;
                if (!used) continue;
                if (nt == JRANK_MAXT) { nt = -1; break; }
                idx[nt++] = z;
            }
            if (nt < 1) continue;
            memset(out, 0, sizeof *out);
            out->blk = H; out->fn = f; out->nedge = L->nedges; out->nt = nt; out->ncomp = 2 + n0 + n1;
            for (int32_t t = 0; t < nt; t++) { out->key[t] = L->pkey[idx[t]]; out->tk[t] = L->ptk[idx[t]]; }
            for (int32_t e = 0; e < L->nedges; e++) { out->from[e] = L->edges[e]; out->fall[e] = -1; }
            out->nreg = 2;
            out->reg[0].g0 = 0; out->reg[0].ng = 1; out->reg[0].t0 = 1; out->reg[0].nt = (uint8_t)n0;
            out->reg[1].g0 = (uint8_t)(1 + n0); out->reg[1].ng = 1; out->reg[1].t0 = (uint8_t)(2 + n0); out->reg[1].nt = (uint8_t)n1;
            for (int32_t g = 0; g < 2; g++) {
                const FdAtom *ga = g ? g1 : g0; int32_t row = out->reg[g].g0, n = g ? n1 : n0;
                const __int128 *src = g ? rc1 : rc;
                for (int32_t t = 0; t < nt; t++) {                       // s*(v - w) - t >= 0
                    if (idx[t] == ga->q) out->coef[row][t] = ga->s;
                    if (idx[t] == ga->w) out->coef[row][t] = -ga->s;
                }
                out->c0[row] = -(__int128)ga->t;
                for (int32_t r = 0; r < n; r++) {
                    out->c0[out->reg[g].t0 + r] = rc0[g][r];
                    for (int32_t t = 0; t < nt; t++) out->coef[out->reg[g].t0 + r][t] = src[(size_t)r * (size_t)np + (size_t)idx[t]];
                }
            }
            return true;
        }
    return false;
}
// PE.13, with c41's MIN components on free edges: versions a trip rotates OUTWARD
// (4NestedWith3Variables: (a, b) <- (3a - 4b, 4a + 3b) scales |a| + |b| by at least 3.5, or leaves
// (0, 0) where it is while q falls). The ranking <min(C - s1*v - s2*w), the rest>: the min over the
// sign rows is C - (|v| + |w|), C the most s1*v + s2*w is on any path (the types), and the rest
// ranks the trips that keep |v| + |w|. Checked here first, per back-edge path and each pair of sign
// cases before and after the trip: |v'| + |w'| >= |v| + |w| + 1, or, under |v'| + |w'| <= |v| + |w|,
// equality and the rest's own ranking (lia_rank_lex over exactly those cases). Before the trip each
// version is >= 1, <= -1 or 0 (cases of x >= 0 all hold 0, where nothing grows, and every case fell to
// the rest: 32 paths, past lia_rank_lex's budget); after it, >= 0 or <= 0. One version alone is the
// two rows C - v and C + v.
static bool fd_case_holds(JFinder *F, const JVc *P, const JCons *ex, int32_t nex, const JCons *goal, JWork lim) {
    JHyp *h = malloc(sizeof *h * (size_t)(P->nhyps + nex + 1));
    if (!h) { F->oom = true; return false; }
    if (P->nhyps) memcpy(h, P->hyps, sizeof *h * (size_t)P->nhyps);
    for (int32_t i = 0; i < nex; i++) { memset(&h[P->nhyps + i], 0, sizeof *h); h[P->nhyps + i].origin = "case"; h[P->nhyps + i].cons = ex[i]; }
    JVc q = *P; q.hyps = h; q.nhyps = P->nhyps + nex;
    memset(&q.goal, 0, sizeof q.goal);
    if (goal) q.goal = *goal; else { q.goal.op = J_LE; fd_jint(&q.goal.rhs, -1); }   // none: refute
    bool r = lia_solve(&q, lim, F->work, F->worksz).k == J_ANS_CERT;
    free(h);
    return r;
}
static JCons fd_cons_of(JTerm *t, int32_t nt, __int128 rhs) {
    JCons c; memset(&c, 0, sizeof c); c.t = t; c.nt = nt; c.op = J_LE; fd_jint(&c.rhs, rhs); return c;
}
static int fd_no_templates;                                 // fd_rank inside a template: its search only
static bool fd_rank(JFinder *F, IrFunc *f, int32_t H, FdLoop *L, JRank *out);
// a kept case: path k with the case's facts added, sliced (as L->paths) and whole (as L->full)
static const JVc *fd_with_case(FdLoop *L, const JVc *P, const JCons *ex, int32_t ne, const JTerm *tt, int32_t ntt) {
    JVc *v = fd_bump(L, sizeof *v); JHyp *h = fd_bump(L, sizeof(JHyp) * (size_t)(P->nhyps + ne));
    JTerm *rt = fd_bump(L, sizeof(JTerm) * (size_t)ntt);
    if (!v || !h || !rt) return NULL;
    memcpy(rt, tt, sizeof(JTerm) * (size_t)ntt);
    if (P->nhyps) memcpy(h, P->hyps, sizeof *h * (size_t)P->nhyps);
    for (int32_t e = 0; e < ne; e++) {
        JHyp *y = &h[P->nhyps + e];
        memset(y, 0, sizeof *y); y->origin = "case"; y->cons = ex[e];
        y->cons.t = rt + (ex[e].t - tt);                     // into the copy
    }
    *v = *P; v->hyps = h; v->nhyps = P->nhyps + ne;
    return v;
}
static bool fd_rank_absgrow(JFinder *F, IrFunc *f, int32_t H, FdLoop *L, JRank *out) {
    if (L->nedges > JRANK_MAXE || L->npaths == 0 || L->npaths > 8) return false;
    JWork slim = F->lim; slim.pivot /= 16; slim.node /= 16; slim.leaf /= 16;
    // the candidates: versions a trip changes, each with M, a bound on |v| before a trip: on every
    // path, the least of the bounds it states on v alone (its type's, at the least). Any upper bound
    // serves (it only keeps the rows >= 0), and reading the stated ones costs no search: maximizing
    // on the whole paths took most of this template's time on 4NestedWith3Variables
    int32_t cand[6], nc = 0; __int128 M[6];
    for (int32_t z = 0; z < L->npk && nc < 6; z++) {
        if (L->ptk[z] != JTERM_VALUE || !L->moves || !L->moves[z]) continue;
        bool okM = true; __int128 m = 0;
        for (int32_t k = 0; k < L->npaths && okM; k++) {
            __int128 hi = 0, lo = 0; bool hh = false, hl = false;
            for (int32_t x = 0; x < L->paths[k]->nhyps; x++) {
                const JHyp *y = &L->paths[k]->hyps[x];
                if (y->disj || y->cons.op != J_LE || y->cons.nt != 1 || y->cons.t[0].var != L->pre[z]) continue;
                __int128 c = fd_int128(&y->cons.t[0].coef), r;
                if (!fd_i128(&y->cons.rhs, &r)) continue;
                if (c == 1 && (!hh || r < hi)) { hi = r; hh = true; }          // v <= r
                if (c == -1 && (!hl || -r > lo)) { lo = -r; hl = true; }       // v >= -r
            }
            if (!hh || !hl) { okM = false; break; }
            __int128 b = hi > -lo ? hi : -lo;
            if (b > m) m = b;
        }
        if (okM && m <= ((__int128)1 << 39)) { M[nc] = m; cand[nc++] = z; }
    }
    char *mark = L->cur;                                     // each pair's scratch, reclaimed by the next
    for (int32_t i = 0; i < nc; i++)
        for (int32_t j = i; j < nc; j++) {                   // j == i: the version alone
            L->cur = mark;
            int32_t qv[2] = { cand[i], j == i ? -1 : cand[j] }, nq = j == i ? 1 : 2, ns = 1 << nq;
            __int128 C = M[i] + (j == i ? 0 : M[j]);         // at least the most s . (v, w) before a trip
            int32_t npre = nq == 1 ? 3 : 9;                  // before: per version 0 (>= 1), 1 (<= -1), 2 (= 0)
            int32_t cap = L->npaths * npre * ns;
            const JVc **resp = fd_bump(L, sizeof(JVc *) * (size_t)cap), **resf = fd_bump(L, sizeof(JVc *) * (size_t)cap);
            int32_t *rese = fd_bump(L, sizeof(int32_t) * (size_t)cap);
            if (!resp || !resf || !rese) return false;
            int32_t nres = 0; bool ok = true;
            uint8_t rep[8 * 9];                              // per (path, signs before): a same case kept
            memset(rep, 0, sizeof rep);
            for (int32_t k = 0; k < L->npaths && ok; k++)
                for (int32_t a = 0; a < npre && ok; a++)
                    for (int32_t b = 0; b < ns && ok; b++) {
                        JTerm tt[16]; JCons ex[8];           // this case's scratch; a kept case is copied out
                        int32_t ne = 0, used = 0;
                        int sa[2];
                        for (int32_t u = 0; u < nq; u++) {   // sign cases, before and after
                            int da = u ? a / 3 : a % 3, sb = (b >> u) & 1 ? -1 : 1;
                            sa[u] = da == 0 ? 1 : da == 1 ? -1 : 0;
                            JTerm *t1 = tt + used; used += 1; t1[0].var = L->pre[qv[u]];
                            if (sa[u]) { fd_jint(&t1[0].coef, -sa[u]); ex[ne++] = fd_cons_of(t1, 1, -1); }   // -s*x <= -1
                            else {                            // x <= 0 and -x <= 0
                                fd_jint(&t1[0].coef, 1); ex[ne++] = fd_cons_of(t1, 1, 0);
                                JTerm *t3 = tt + used; used += 1; t3[0].var = L->pre[qv[u]]; fd_jint(&t3[0].coef, -1);
                                ex[ne++] = fd_cons_of(t3, 1, 0);
                            }
                            JTerm *t2 = tt + used; used += 1; t2[0].var = L->post[qv[u]]; fd_jint(&t2[0].coef, -sb);
                            ex[ne++] = fd_cons_of(t2, 1, 0);
                        }
                        if (fd_case_holds(F, L->paths[k], ex, ne, NULL, slim)) continue;   // no such case
                        if (F->oom) return false;
                        JTerm *g = tt + used; int32_t ng = 0;   // A - B <= -1: |v'| + |w'| >= |v| + |w| + 1
                        for (int32_t u = 0; u < nq; u++) {
                            if (sa[u]) ng = fd_lin(g, ng, L->pre[qv[u]], sa[u]);
                            ng = fd_lin(g, ng, L->post[qv[u]], (b >> u) & 1 ? 1 : -1);
                        }
                        used += 4;
                        JCons grow = fd_cons_of(g, ng, -1);
                        if (fd_case_holds(F, L->paths[k], ex, ne, &grow, slim)) continue;
                        if (F->oom) return false;
                        JTerm *kp = tt + used;               // otherwise B - A <= 0, and then A - B <= 0 too
                        for (int32_t u = 0; u < ng; u++) { kp[u] = g[u]; fd_jint(&kp[u].coef, -fd_int128(&g[u].coef)); }
                        used += 4;
                        ex[ne++] = fd_cons_of(kp, ng, 0);
                        JCons keep = fd_cons_of(g, ng, 0);
                        if (!fd_case_holds(F, L->paths[k], ex, ne, &keep, slim)) { ok = false; break; }
                        // a kept case that leaves v and w as they were (4Nested's (0, 0)) is one case per
                        // path and pre-trip signs, whatever the signs after: the others repeat it. The
                        // first FEASIBLE one stands for them: with v <= -1 kept, the case of every
                        // version >= 0 after the trip cannot run, and taking it as the representative
                        // dropped v's case from the rest's search (the Secondary Compiler Worker's review)
                        bool same = true;
                        for (int32_t u = 0; u < nq && same; u++)
                            for (int sg = 1; sg >= -1 && same; sg -= 2) {
                                JTerm d[2]; int32_t nd = fd_lin(d, 0, L->pre[qv[u]], sg); nd = fd_lin(d, nd, L->post[qv[u]], -sg);
                                JCons eq = fd_cons_of(d, nd, 0);
                                same = fd_case_holds(F, L->paths[k], ex, ne, &eq, slim);
                            }
                        if (same && rep[k * npre + a]) continue;
                        if (same) rep[k * npre + a] = 1;
                        resp[nres] = fd_with_case(L, L->paths[k], ex, ne, tt, used);
                        resf[nres] = fd_with_case(L, L->full[k], ex, ne, tt, used);
                        if (!resp[nres] || !resf[nres]) return false;
                        rese[nres++] = L->edge[k];
                    }
            if (F->oom) return false;
            if (!ok) continue;
            // the rest: the finder's own search over the kept cases, a loop of their own (its
            // templates aside); none kept, the min alone
            JRank rt; memset(&rt, 0, sizeof rt);
            if (nres > 0) {
                FdLoop V = *L;
                V.paths = resp; V.full = (JVc **)resf; V.edge = rese; V.npaths = nres; V.dead = false; V.wrapped = NULL;
                fd_no_templates++;
                bool got = fd_rank(F, f, H, &V, &rt);
                fd_no_templates--;
                if (F->oom) return false;
                // the rest's own groups stay as they are (c41 reads several in one tuple); its first
                // row starts a component of its own
                if (!got || rt.nreg || ns + rt.ncomp > JRANK_MAXC || rt.cont[0] != 0) continue;
            }
            // the terms: v, w, and the rest's
            int32_t nt = 0, key[JRANK_MAXT]; uint8_t tk[JRANK_MAXT]; bool room = true;
            for (int32_t u = 0; u < nq; u++) { key[nt] = L->pkey[qv[u]]; tk[nt] = JTERM_VALUE; nt++; }
            for (int32_t t = 0; t < rt.nt && room; t++) {
                bool seen = false;
                for (int32_t x = 0; x < nt && !seen; x++) seen = key[x] == rt.key[t] && tk[x] == rt.tk[t];
                if (seen) continue;
                if (nt == JRANK_MAXT) room = false; else { key[nt] = rt.key[t]; tk[nt] = rt.tk[t]; nt++; }
            }
            if (!room) continue;
            memset(out, 0, sizeof *out);
            out->blk = H; out->fn = f; out->nedge = L->nedges; out->nt = nt; out->ncomp = ns + rt.ncomp;
            for (int32_t t = 0; t < nt; t++) { out->key[t] = key[t]; out->tk[t] = tk[t]; }
            for (int32_t e = 0; e < L->nedges; e++) { out->from[e] = L->edges[e]; out->fall[e] = -1; }
            for (int32_t sg = 0; sg < ns; sg++) {             // row sg: C - s . (v, w), a MIN group
                out->c0[sg] = C; out->cont[sg] = sg ? 2 : 0;
                for (int32_t u = 0; u < nq; u++) out->coef[sg][u] = (sg >> u) & 1 ? 1 : -1;
            }
            for (int32_t r = 0; r < rt.ncomp; r++) {
                out->c0[ns + r] = rt.c0[r]; out->cont[ns + r] = rt.cont[r];
                for (int32_t t = 0; t < rt.nt; t++)
                    for (int32_t x = 0; x < nt; x++) if (key[x] == rt.key[t] && tk[x] == rt.tk[t]) out->coef[ns + r][x] = rt.coef[r][t];
            }
            return true;
        }
    return false;
}
// PE.13 kind (4), with c43's regions at a call: a recursion whose calls all pass one parameter on
// UNCHANGED and rank differently by its sign (TwoWay: `terminate` picks n falling, n >= 0 the
// bound, or n rising to the type's bound). Three regions, z >= 1, z <= -1 and z = 0 (its two atoms),
// each its own tuple from the finder's own search over the calls its guard leaves (templates off),
// a region no call meets its first guard row. No call moves between them (z is passed on, the same
// variable on both sides), they cover the integers, and each tuple ranks exactly the calls in its
// region: what c43 owes. On a LOOP (c38's regions), a read-only version is split only where every
// back edge, as the generator gives it, already rules out z = 0 (one question per edge): the
// Toulouse ports' x is 1 or -1, set before the loop, and the join's facts say so per path; a loop
// whose cell may be 0 would pay three searches for a region that cannot rank, on every loop the
// finder cannot rank.
// Raw path k with lo <= z <= hi added (z a version, on its pre variable) is infeasible: asked of the path
// as the generator gave it, every disjunction whole, as the judge's own VCs hold it. The flattened
// paths drop disjunctions past FD_MAXFLAT, and a sign carried through a chain of joins went with them
// (Toulouse-MultiBranchesToLoop-2: nine joins, x's sign only through all of them).
static bool fd_raw_refutes(JFinder *F, const FdLoop *L, int32_t k, int32_t z, int64_t lo, int64_t hi) {
    const FdRaw *w = &L->raw[k];
    int32_t nconj = 0;                                       // a decided disjunction's conjuncts, one hypothesis each
    for (int32_t i = 0; i < w->nh; i++) if (w->h[i].disj && w->h[i].nd == 1) nconj += w->h[i].dn[0];
    JHyp *h = calloc((size_t)(w->nh + nconj + 2), sizeof *h);
    JConj *cj = NULL; JCons *cc = NULL; int32_t ncj = 0, ncc = 0;
    for (int32_t i = 0; i < w->nh; i++) if (w->h[i].disj) { ncj += w->h[i].nd; for (int32_t d = 0; d < w->h[i].nd; d++) ncc += w->h[i].dn[d]; }
    cj = calloc((size_t)(ncj ? ncj : 1), sizeof *cj); cc = calloc((size_t)(ncc ? ncc : 1), sizeof *cc);
    if (!h || !cj || !cc) { free(h); free(cj); free(cc); F->oom = true; return false; }
    int32_t uj = 0, uc = 0, m = 0;
    for (int32_t i = 0; i < w->nh; i++) {
        const FdHyp *o = &w->h[i];
        if (!o->disj) { h[m].cons = o->c.c; h[m].origin = "path"; m++; continue; }
        if (o->nd == 1) {                                    // a case decided in fd_loop_from: its conjuncts
            for (int32_t c = 0; c < o->dn[0]; c++) { h[m].cons = o->dc[c].c; h[m].origin = "path case"; m++; }
            continue;
        }
        h[m].disj = true; h[m].dis = cj + uj; h[m].ndis = o->nd; h[m].origin = "path case";
        for (int32_t d = 0, z0 = 0; d < o->nd; d++) {
            cj[uj].c = cc + uc; cj[uj].nc = o->dn[d];
            for (int32_t c = 0; c < o->dn[d]; c++) cc[uc++] = o->dc[z0++].c;
            uj++;
        }
        m++;
    }
    JTerm tz[2]; tz[0].var = tz[1].var = w->pre[z]; fd_jint(&tz[0].coef, 1); fd_jint(&tz[1].coef, -1);
    memset(&h[m], 0, sizeof h[m]); h[m].origin = "case"; h[m].cons = fd_cons_of(&tz[0], 1, hi); m++;     // z <= hi
    memset(&h[m], 0, sizeof h[m]); h[m].origin = "case"; h[m].cons = fd_cons_of(&tz[1], 1, -lo); m++;    // -z <= -lo
    JVc q; memset(&q, 0, sizeof q); q.vars = NULL; q.nvars = w->nv; q.hyps = h; q.nhyps = m;
    q.goal.op = J_LE; fd_jint(&q.goal.rhs, -1);
    bool r = lia_solve(&q, F->lim, F->work, F->worksz).k == J_ANS_CERT;
    free(h); free(cj); free(cc);
    return r;
}
static bool fd_rank_split(JFinder *F, IrFunc *f, int32_t H, FdLoop *L, JRank *out) {
    if (!L->moves || L->npaths == 0) return false;
    char *mark = L->cur;
    for (int32_t z = 0; z < L->npk; z++) {
        L->cur = mark;
        if (L->ptk[z] != JTERM_VALUE || L->moves[z]) continue;
        if (H != JRANK_ENTRY) {                              // a loop: z = 0 refuted on every raw path
            bool nz = L->raw && L->nraw > 0;
            for (int32_t k = 0; k < L->nraw && nz; k++) nz = fd_raw_refutes(F, L, k, z, 0, 0);
            if (F->oom) return false;
            if (!nz) continue;
        }
        // the three guards, each row s*z - t >= 0: z - 1, -z - 1; z and -z
        static const int8_t gs[4] = { 1, -1, 1, -1 }; static const int8_t gt[4] = { 1, 1, 0, 0 };
        static const int8_t g0[3] = { 0, 1, 2 }, gn[3] = { 1, 1, 2 };
        JRank rt[3]; bool ok = true;
        for (int32_t k = 0; k < 3 && ok; k++) {
            memset(&rt[k], 0, sizeof rt[k]);
            const JVc **rp = fd_bump(L, sizeof(JVc *) * (size_t)L->npaths), **rf = fd_bump(L, sizeof(JVc *) * (size_t)L->npaths);
            int32_t *re = fd_bump(L, sizeof(int32_t) * (size_t)L->npaths);
            if (!rp || !rf || !re) return false;
            int32_t nr = 0;
            // a loop's z = 0 region was refuted on every raw path above; the flattened ones may have
            // lost why (a dropped join), so it is taken as empty rather than searched
            for (int32_t p = 0; p < L->npaths && !(H != JRANK_ENTRY && k == 2); p++) {
                JTerm tt[4]; JCons ex[4]; int32_t ne = 0;
                memset(tt, 0, sizeof tt);
                for (int32_t a = 0; a < gn[k]; a++) {       // the guard before and after (z the same)
                    int32_t gi = g0[k] + a;
                    tt[2 * a].var = L->pre[z]; fd_jint(&tt[2 * a].coef, -gs[gi]);
                    ex[ne++] = fd_cons_of(&tt[2 * a], 1, -gt[gi]);
                    tt[2 * a + 1].var = L->post[z]; fd_jint(&tt[2 * a + 1].coef, -gs[gi]);
                    ex[ne++] = fd_cons_of(&tt[2 * a + 1], 1, -gt[gi]);
                }
                if (fd_case_holds(F, L->paths[p], ex, ne, NULL, F->lim)) continue;   // no such call
                if (F->oom) return false;
                rp[nr] = fd_with_case(L, L->paths[p], ex, ne, tt, 4);
                rf[nr] = fd_with_case(L, L->full[p], ex, ne, tt, 4);
                if (!rp[nr] || !rf[nr]) return false;
                re[nr++] = L->edge[p];
            }
            if (nr == 0) {                                   // no call in the region: its first guard row
                rt[k].ncomp = 1; rt[k].nt = 1; rt[k].key[0] = L->pkey[z]; rt[k].tk[0] = JTERM_VALUE;
                rt[k].coef[0][0] = gs[g0[k]]; rt[k].c0[0] = -gt[g0[k]];
                continue;
            }
            FdLoop V = *L;
            V.paths = rp; V.full = (JVc **)rf; V.edge = re; V.npaths = nr; V.dead = false; V.wrapped = NULL;
            fd_no_templates++;
            bool got = fd_rank(F, f, H, &V, &rt[k]);
            fd_no_templates--;
            if (F->oom) return false;
            ok = got && !rt[k].nreg && rt[k].cont[0] == 0;
        }
        if (!ok || 4 + rt[0].ncomp + rt[1].ncomp + rt[2].ncomp > JRANK_MAXC) continue;
        // the terms: z, then every tuple's
        int32_t nt = 1, key[JRANK_MAXT]; uint8_t tk[JRANK_MAXT]; bool room = true;
        key[0] = L->pkey[z]; tk[0] = JTERM_VALUE;
        for (int32_t k = 0; k < 3 && room; k++)
            for (int32_t t = 0; t < rt[k].nt && room; t++) {
                bool seen = false;
                for (int32_t x = 0; x < nt && !seen; x++) seen = key[x] == rt[k].key[t] && tk[x] == rt[k].tk[t];
                if (seen) continue;
                if (nt == JRANK_MAXT) room = false; else { key[nt] = rt[k].key[t]; tk[nt] = rt[k].tk[t]; nt++; }
            }
        if (!room) continue;
        memset(out, 0, sizeof *out);
        out->blk = H; out->fn = f; out->nedge = L->nedges; out->nt = nt; out->nreg = 3;
        for (int32_t t = 0; t < nt; t++) { out->key[t] = key[t]; out->tk[t] = tk[t]; }
        for (int32_t e = 0; e < L->nedges; e++) { out->from[e] = L->edges[e]; out->fall[e] = -1; }
        int32_t row = 0;
        for (int32_t k = 0; k < 3; k++) {
            out->reg[k].g0 = (uint8_t)row; out->reg[k].ng = (uint8_t)gn[k];
            for (int32_t a = 0; a < gn[k]; a++, row++) { out->coef[row][0] = gs[g0[k] + a]; out->c0[row] = -gt[g0[k] + a]; }
            out->reg[k].t0 = (uint8_t)row; out->reg[k].nt = (uint8_t)rt[k].ncomp;
            for (int32_t r = 0; r < rt[k].ncomp; r++, row++) {
                out->c0[row] = rt[k].c0[r]; out->cont[row] = rt[k].cont[r];
                for (int32_t t = 0; t < rt[k].nt; t++)
                    for (int32_t x = 0; x < nt; x++) if (key[x] == rt[k].key[t] && tk[x] == rt[k].tk[t]) out->coef[row][x] = rt[k].coef[r][t];
            }
        }
        out->ncomp = row;
        return true;
    }
    return false;
}
// PE.13 kind (4), with c42's max groups at a call: a recursion falling in the MAX of two affine
// rows (LeeJonesBen-Amram POPL 2001 Ex5: f(y, y - 1) and f(y, x - 1) fall in max(2x, 2y + 1)). Rows
// a0*v + b0 + C and a1*w + b1 + C over two parameters, a in {1, 2} and b in {0, 1}, C the most a*|v|
// is (the bounds the calls state, the types at the least), so every row is >= 0. Checked first per
// call and per row k that is the max before it: every row after the call is at most row k before,
// less one; so the max falls and is bounded, what c42 owes. Recursion only, as fd_rank_split.
static bool fd_rank_affmax(JFinder *F, IrFunc *f, int32_t H, FdLoop *L, JRank *out) {
    if (H != JRANK_ENTRY || L->npaths == 0 || L->npaths > 8) return false;
    int32_t cand[4], nc = 0; __int128 M[4];
    for (int32_t z = 0; z < L->npk && nc < 4; z++) {      // the parameters, each with a bound on |z|
        if (L->ptk[z] != JTERM_VALUE) continue;
        bool okM = true; __int128 m = 0;
        for (int32_t k = 0; k < L->npaths && okM; k++) {
            __int128 hi = 0, lo = 0; bool hh = false, hl = false;
            for (int32_t x = 0; x < L->paths[k]->nhyps; x++) {
                const JHyp *y = &L->paths[k]->hyps[x];
                if (y->disj || y->cons.op != J_LE || y->cons.nt != 1 || y->cons.t[0].var != L->pre[z]) continue;
                __int128 c = fd_int128(&y->cons.t[0].coef), r;
                if (!fd_i128(&y->cons.rhs, &r)) continue;
                if (c == 1 && (!hh || r < hi)) { hi = r; hh = true; }
                if (c == -1 && (!hl || -r > lo)) { lo = -r; hl = true; }
            }
            if (!hh || !hl) { okM = false; break; }
            __int128 b = hi > -lo ? hi : -lo;
            if (b > m) m = b;
        }
        if (okM && m <= ((__int128)1 << 38)) { M[nc] = m; cand[nc++] = z; }
    }
    for (int32_t i = 0; i < nc; i++)
        for (int32_t j = 0; j < nc; j++) {
            if (i == j) continue;
            int32_t qv[2] = { cand[i], cand[j] };
            __int128 C = 2 * (M[i] > M[j] ? M[i] : M[j]) + 1;
            for (int32_t g = 0; g < 16; g++) {               // a0, a1 in {1, 2}; b0, b1 in {0, 1}
                int a[2] = { 1 + (g & 1), 1 + ((g >> 1) & 1) }, b[2] = { (g >> 2) & 1, (g >> 3) & 1 };
                bool ok = true;
                for (int32_t p = 0; p < L->npaths && ok; p++)
                    for (int32_t k = 0; k < 2 && ok; k++) {  // row k the max before the call...
                        JTerm tk[4]; int32_t nk = 0;         // row (1-k) - row k <= 0, before
                        nk = fd_lin(tk, nk, L->pre[qv[1 - k]], a[1 - k]); nk = fd_lin(tk, nk, L->pre[qv[k]], -a[k]);
                        JCons ex = fd_cons_of(tk, nk, b[k] - b[1 - k]);
                        if (fd_case_holds(F, L->paths[p], &ex, 1, NULL, F->lim)) continue;   // never the max
                        if (F->oom) return false;
                        for (int32_t r = 0; r < 2 && ok; r++) {   // ...every row after: row r' <= row k - 1
                            JTerm tg[4]; int32_t ng = 0;
                            ng = fd_lin(tg, ng, L->post[qv[r]], a[r]); ng = fd_lin(tg, ng, L->pre[qv[k]], -a[k]);
                            JCons goal = fd_cons_of(tg, ng, b[k] - b[r] - 1);
                            ok = fd_case_holds(F, L->paths[p], &ex, 1, &goal, F->lim);
                            if (F->oom) return false;
                        }
                    }
                if (!ok) continue;
                memset(out, 0, sizeof *out);
                out->blk = H; out->fn = f; out->nedge = L->nedges; out->nt = 2; out->ncomp = 2;
                for (int32_t u = 0; u < 2; u++) { out->key[u] = L->pkey[qv[u]]; out->tk[u] = JTERM_VALUE; out->coef[u][u] = a[u]; out->c0[u] = b[u] + C; }
                out->cont[1] = 1;                            // one MAX group
                for (int32_t e = 0; e < L->nedges; e++) { out->from[e] = L->edges[e]; out->fall[e] = 0; }
                return true;
            }
        }
    return false;
}
// A late template in an arena of its own: the passes before it fill L's (4 MB less 11 KB left on
// 4NestedWith3Variables), and a template that cannot allocate proposes nothing, silently. L's paths
// and pairs stay where they are; only what the template bumps goes to the new arena, freed after.
static bool fd_in_arena(JFinder *F, IrFunc *f, int32_t H, FdLoop *L, JRank *out,
                        bool (*fn)(JFinder *, IrFunc *, int32_t, FdLoop *, JRank *)) {
    size_t cap = (size_t)1 << 22;
    char *arena = malloc(cap), *cur = L->cur, *end = L->end;
    if (!arena) { F->oom = true; return false; }
    L->cur = arena; L->end = arena + cap;
    bool got = fn(F, f, H, L, out);
    L->cur = cur; L->end = end;
    free(arena);
    return got;
}
// jvc --rank-stages: which stage of fd_rank's cascade ranked a loop or a recursion, a label no
// decision reads. Tracked only when asked (fd_track_stages); fd_stage is the last success's.
static bool fd_track_stages = false;
static const char *fd_stage = "?";
static const char *fd_stage_in_pass(int pass, const char *s) {
    static const char *const t[4][3] = {
        { "pass0 linear", "pass0 lex", "pass0 lex-bounded" }, { "pass1 linear", "pass1 lex", "pass1 lex-bounded" },
        { "pass2 linear", "pass2 lex", "pass2 lex-bounded" }, { "pass3 linear", "pass3 lex", "pass3 lex-bounded" } };
    int k = !strcmp(s, "linear") ? 0 : !strcmp(s, "lex") ? 1 : 2;
    return pass >= 0 && pass < 4 ? t[pass][k] : s;
}
// F->rk just grew by one (or by m, the last m): their labels
static bool fd_stage_note(JFinder *F, int32_t m, const char *s) {
    if (!fd_track_stages) return true;
    const char **x = realloc(F->rkstage, sizeof *x * (size_t)(F->nrk ? F->nrk : 1));
    if (!x) return false;
    F->rkstage = x;
    for (int32_t i = F->nrk - m; i < F->nrk; i++) F->rkstage[i] = s;
    return true;
}
static bool fd_rank(JFinder *F, IrFunc *f, int32_t H, FdLoop *L, JRank *out) {
    if (fd_rank_on(F, f, H, L, out)) return true;
    int32_t np = L->npaths;
    const JVc **sl = fd_bump(L, sizeof(JVc *) * (size_t)(np ? np : 1));
    uint8_t *on = fd_bump(L, (size_t)(L->npairs ? L->npairs : 1));
    if (!sl || !on) return false;
    // pass 0: the quantity alone, its paths cut to the facts that touch it (its step and its
    // guard); pass 1: alone, two rounds out; pass 2: with the values fixed at the header; pass 3:
    // every version that steps by a constant on some path, TOGETHER, the rest left out (CookSeeZuleger
    // Fig7b's <y, z, x>: each path lowers one counter and sets another arbitrarily, so no counter
    // alone ranks it, and beside the loop's other versions (its `nondet()` temporaries) the search
    // found only a ranking past what the judge states)
    int32_t base_pairs = 2 * L->npk + (L->npairs - L->npk);   // the pairs' and fixed values' variables
    uint8_t *step = fd_bump(L, (size_t)(L->npairs ? L->npairs : 1));
    if (!step) return false;
    // a stepper needs ONE path with a constant step: four paths looked at, each question on the facts
    // connected to the pair and a sixteenth of the budget (every pair on every path cost Gothenburg
    // 130 maximizations, 46 s, more than the rest of its proof)
    int32_t nstep = 0;
    JWork slim = F->lim; slim.pivot /= 16; slim.node /= 16; slim.leaf /= 16;
    for (int32_t z = 0; z < L->npk; z++) {
        for (int32_t k = 0; k < np && k < 4 && !step[z]; k++) {
            int64_t lo, hi;
            step[z] = rk_delta_sliced(L->paths[k], L->pre[z], L->post[z], slim, F->work, F->worksz, &lo, &hi) && lo == hi && lo != 0;
        }
        nstep += step[z];
    }
    int32_t nreal = 0;                                       // the versions, ENTRY pairs aside (c40)
    for (int32_t z = 0; z < L->npk; z++) nreal += L->ptk[z] != JTERM_ENTRY;
    for (int32_t pass = 0; pass < 4; pass++)
    for (int32_t q = 0; q < L->npk; q++) {
        if (pass == 2 && L->npairs == L->npk) break;       // no value fixed at the header to add
        if (pass == 3 && (q > 0 || nstep < 2 || nstep == nreal)) break;    // once; none alone, not all
        if (pass < 3 && L->ptk[q] == JTERM_ENTRY) continue;   // fixed in the loop: alone it ranks nothing
        for (int32_t z = 0; z < L->npairs; z++) on[z] = pass == 3 ? step[z] : z == q || (pass == 2 && z >= L->npk);
        bool ok = true; int32_t ns = 0;
        int32_t *sedge = fd_bump(L, sizeof(int32_t) * (size_t)(np ? np : 1));
        if (!sedge) break;
        for (int32_t k = 0; k < np && ok; k++) {
            JVc *v = fd_bump(L, sizeof *v); JHyp *h = fd_bump(L, sizeof(JHyp) * (size_t)(L->full[k]->nhyps ? L->full[k]->nhyps : 1));
            if (!v || !h) { ok = false; break; }
            int32_t nh0 = 0;
            for (int32_t x = 0; x < L->full[k]->nhyps; x++) {
                const JHyp *src = &L->full[k]->hyps[x];
                // the quantity alone (passes 0 and 1): a claim relating it to ANOTHER quantity of the
                // header (a table cell, a fixed value) says nothing its own measure reads, and a
                // loop reading a 12-entry table carries hundreds of them (convert_days' month loop);
                // nor does one naming none of its variables (each element of an array the loop
                // reads by the counter, `each(a) <= 25`, reached through the read's cases)
                if (pass < 2 && src->origin && !strcmp(src->origin, "path claim")) {
                    bool other = false, mine = false;
                    for (int32_t t = 0; t < src->cons.nt && !other; t++) {
                        int32_t vv = src->cons.t[t].var;
                        if (vv == 2 * q || vv == 2 * q + 1) mine = true;
                        else other = vv < base_pairs;
                    }
                    if (other || !mine) continue;
                }
                if (pass == 3 && src->origin && !strcmp(src->origin, "path claim")) {
                    bool other = false;                      // a claim about a version left out
                    for (int32_t t = 0; t < src->cons.nt && !other; t++) {
                        int32_t vv = src->cons.t[t].var;
                        if (vv < base_pairs) other = !on[vv < 2 * L->npk ? vv / 2 : L->npk + (vv - 2 * L->npk)];
                    }
                    if (other) continue;
                }
                h[nh0++] = *src;
            }
            // pass 3 follows a version's chain of copies to its end (a counter's new value reaches
            // its old one through several join versions); its other versions are already left out
            *v = *L->full[k]; v->hyps = h; v->nhyps = fd_slice_on(L, h, nh0, L->nv, on, pass == 0 ? 1 : pass == 3 ? -1 : 2);
            bool dup = false;
            for (int32_t z = 0; z < ns && !dup; z++) dup = sedge[z] == L->edge[k] && fd_same_path(sl[z], v);
            if (dup) continue;
            sl[ns] = v; sedge[ns] = L->edge[k]; ns++;
        }
        if (!ok) break;
        FdLoop V = *L; int32_t nn = 0;                       // only the pairs `on` names
        V.moves = NULL; V.raw = NULL; V.nraw = 0; V.wrapped = NULL; V.parg = NULL;
        V.paths = sl; V.npaths = ns; V.edge = sedge;
        V.pkey = fd_bump(L, sizeof(int32_t) * (size_t)L->npairs); V.ptk = fd_bump(L, (size_t)L->npairs);
        V.pre = fd_bump(L, sizeof(int32_t) * (size_t)L->npairs); V.post = fd_bump(L, sizeof(int32_t) * (size_t)L->npairs);
        if (!V.pkey || !V.ptk || !V.pre || !V.post) break;
        for (int32_t z = 0; z < L->npairs; z++) if (on[z]) { V.pkey[nn] = L->pkey[z]; V.ptk[nn] = L->ptk[z]; V.pre[nn] = L->pre[z]; V.post[nn] = L->post[z]; nn++; }
        V.npairs = nn; V.npk = pass == 3 ? nstep : 1;
        bool got = fd_rank_on(F, f, H, &V, out);
        if (got) { fd_stage = fd_stage_in_pass(pass, fd_stage); return true; }
    }
    if (fd_rank_max(F, f, H, L, step, out)) { fd_stage = "max"; return true; }   // no tuple: a max component (c36)
    if (fd_no_templates) return false;                     // inside a template: its search only
    if (!F->oom && fd_in_arena(F, f, H, L, out, fd_rank_regions)) { fd_stage = "regions"; return true; }   // nor a max: a tuple per region (c38)
    if (!F->oom && fd_in_arena(F, f, H, L, out, fd_rank_absgrow)) { fd_stage = "absgrow"; return true; }   // nor regions: |v| + |w| growing (c41)
    if (!F->oom && fd_in_arena(F, f, H, L, out, fd_rank_split)) { fd_stage = "split"; return true; }       // a recursion split by a parameter (c43)
    if (!F->oom && fd_rank_affmax(F, f, H, L, out)) { fd_stage = "affmax"; return true; }                 // or a max of two affine rows (c42)
    return false;
}
// PE.11 (KroeningSharyginaTsitovichWintersteiger-CAV2010-Fig1): a trip that steps a cell by its
// residue, `if x % 2 != 0 { x = x - 1 } else { x = x + 2 }` (an odd x steps down once, an even one
// climbs), falls in <mod(x, k), the rest>, where no linear tuple does. gen's paths_mod gives the
// paths with a pair per residue the loop computes: x's floor-mod k before and after the trip
// (JTERM_MOD, k in parg). Only this stage asks for them: as any other pair, a residue would be a
// term of every claim and template, and a claim over one is refused. Each path is split on
// (r, r') = (c, c'), every c and c' in 0 .. k - 1, and a case is kept only if the solver cannot
// refute it, over the integers: rationally r is free in [0, k - 1], and a Farkas search reads
// nothing of it. No kept case may raise r; one that lowers it needs nothing more (r >= 0 bounds
// it), and the cases that keep it are ranked by the finder's own search (templates off) over the
// other pairs. Every edge free (rank.lex): the paths of one edge lower different components.
#define FD_RESIDUE_MAXK 4
static bool fd_residue_tuple(JFinder *F, IrFunc *f, int32_t H, FdLoop *M, int32_t z, JRank *out) {
    int32_t k = M->parg[z], np = M->npaths, nres = 0;
    if (M->nedges > JRANK_MAXE || np == 0 || np * k * k > 64) return false;
    JWork slim = F->lim; slim.pivot /= 16; slim.node /= 16; slim.leaf /= 16;
    const JVc **resp = fd_bump(M, sizeof(JVc *) * (size_t)(np * k)), **resf = fd_bump(M, sizeof(JVc *) * (size_t)(np * k));
    int32_t *rese = fd_bump(M, sizeof(int32_t) * (size_t)(np * k));
    if (!resp || !resf || !rese) return false;
    for (int32_t p = 0; p < np; p++)
        for (int32_t c = 0; c < k; c++)
            for (int32_t c2 = 0; c2 < k; c2++) {
                JTerm tt[4]; JCons ex[4];                    // r = c and r' = c2, two rows each
                for (int32_t u = 0; u < 4; u++) { tt[u].var = u < 2 ? M->pre[z] : M->post[z]; fd_jint(&tt[u].coef, u % 2 ? -1 : 1); }
                ex[0] = fd_cons_of(&tt[0], 1, c); ex[1] = fd_cons_of(&tt[1], 1, -c);
                ex[2] = fd_cons_of(&tt[2], 1, c2); ex[3] = fd_cons_of(&tt[3], 1, -c2);
                if (fd_case_holds(F, M->paths[p], ex, 4, NULL, slim)) continue;   // no such case
                if (F->oom || c2 > c) return false;          // the residue may rise
                if (c2 < c) continue;                        // it falls: the first component
                resp[nres] = fd_with_case(M, M->paths[p], ex, 4, tt, 4);
                resf[nres] = fd_with_case(M, M->full[p], ex, 4, tt, 4);
                if (!resp[nres] || !resf[nres]) return false;
                rese[nres++] = M->edge[p];
            }
    JRank rt; memset(&rt, 0, sizeof rt);
    if (nres > 0) {                                          // the rest: the cases that keep r
        // over M's pairs as numbered (fd_rank reads pair q's variables as 2q and 2q + 1, so none is
        // dropped or renumbered: that read past its arrays, fuzz_loops seed 3268998), only the edges
        // a kept case runs on, and no residue among its terms
        FdLoop V = *M;
        V.paths = resp; V.full = (JVc **)resf; V.edge = rese; V.npaths = nres; V.dead = false;
        V.moves = NULL; V.raw = NULL; V.nraw = 0; V.wrapped = NULL; V.nedges = 0;
        for (int32_t e = 0; e < M->nedges; e++) {
            bool runs = false;
            for (int32_t q = 0; q < nres && !runs; q++) runs = rese[q] == M->edges[e];
            if (runs) V.edges[V.nedges++] = M->edges[e];
        }
        fd_no_templates++;
        bool got = fd_rank(F, f, H, &V, &rt);
        fd_no_templates--;
        if (F->oom || !got || rt.nreg || 1 + rt.ncomp > JRANK_MAXC || 1 + rt.nt > JRANK_MAXT || rt.cont[0] != 0) return false;
        for (int32_t t = 0; t < rt.nt; t++) if (rt.tk[t] == JTERM_MOD) return false;
    }
    memset(out, 0, sizeof *out);
    out->blk = H; out->fn = f; out->nedge = M->nedges; out->ncomp = 1 + rt.ncomp; out->nt = 1 + rt.nt;
    out->key[0] = M->pkey[z]; out->tk[0] = JTERM_MOD; out->targ[0] = k; out->coef[0][0] = 1;
    for (int32_t t = 0; t < rt.nt; t++) { out->key[1 + t] = rt.key[t]; out->tk[1 + t] = rt.tk[t]; }
    for (int32_t e = 0; e < M->nedges; e++) { out->from[e] = M->edges[e]; out->fall[e] = -1; }
    for (int32_t r = 0; r < rt.ncomp; r++) {
        out->c0[1 + r] = rt.c0[r]; out->cont[1 + r] = rt.cont[r];
        for (int32_t t = 0; t < rt.nt; t++) out->coef[1 + r][1 + t] = rt.coef[r][t];
    }
    return true;
}
// ...the loop's paths with its residues (paths_mod), each residue tried in turn
static bool fd_rank_residue(JFinder *F, IrFunc *f, int32_t H, const JClaim *cl, int32_t ncl, JRank *out) {
    if (!F->gen->paths_mod || H == JRANK_ENTRY) return false;
    char err[200]; JPaths P; FdLoop M; bool got = false;
    memset(&M, 0, sizeof M);
    size_t cap = (size_t)1 << 22;
    if (!(M.mem = malloc(cap))) { F->oom = true; return false; }
    M.cur = M.mem; M.end = M.mem + cap;
    if (F->gen->paths_mod(F->gen->ctx, f, cl, ncl, H, &P, err, sizeof err) && !P.refused && fd_loop_from(F, f, H, &P, &M, JRANK_MAXE))
        for (int32_t z = 0; z < M.npk && !got && !F->oom; z++)
            if (M.ptk[z] == JTERM_MOD && M.parg[z] >= 2 && M.parg[z] <= FD_RESIDUE_MAXK) got = fd_residue_tuple(F, f, H, &M, z, out);
    free(M.mem);
    return got;
}
static bool fd_rank_on(JFinder *F, IrFunc *f, int32_t H, const FdLoop *L, JRank *out) {
    memset(out, 0, sizeof *out);
    if (L->nedges > JRANK_MAXE) return false;
    out->blk = H; out->fn = f; out->nedge = L->nedges;
    for (int32_t e = 0; e < L->nedges; e++) out->from[e] = L->edges[e];
    LRank R1 = lia_rank(L->paths, L->npaths, L->pre, L->post, L->npairs, F->lim, F->work, F->worksz);
    JInt *coef[JRANK_MAXC]; JInt c0[JRANK_MAXC]; int32_t ncomp = 0;
    int32_t fall_of_edge[JRANK_MAXE];
    // a linear ranking the judge cannot state is no ranking: under bounded types a lexicographic one
    // linearizes by powers of two (Fig7b's <y, z, x> as about 2^63*y + 2^32*z + x), past the 2^66 a
    // ranking claim may carry, and the lexicographic search must run instead
    if (R1.k == LIA_RANK_FOUND) {
        const __int128 big = (__int128)1 << 66;
        for (int32_t i = 0; i <= L->npairs && R1.k == LIA_RANK_FOUND; i++) {
            __int128 v;
            if (!fd_i128(&R1.coef[i], &v) || v > big || v < -big) R1.k = LIA_RANK_NONE;
        }
    }
    if (R1.k == LIA_RANK_FOUND) {
        fd_stage = "linear";
        coef[0] = R1.coef; c0[0] = R1.c0; ncomp = 1;
        for (int32_t e = 0; e < L->nedges; e++) fall_of_edge[e] = 0;
    } else {
        LRankLex R2 = lia_rank_lex(L->paths, L->npaths, L->pre, L->post, L->npairs, F->lim, F->work, F->worksz);
        // a tuple the judge cannot state (a coefficient or constant past 2^66): again, bounded
        bool fits = R2.k == LIA_RANK_FOUND && R2.ncomp <= JRANK_MAXC;
        for (int32_t j = 0; fits && j < R2.ncomp; j++)
            for (int32_t i = 0; fits && i <= L->npairs; i++) {
                __int128 v;
                const JInt *x = i < L->npairs ? &R2.coef[j][i] : &R2.c0[j];
                fits = fd_i128(x, &v) && v <= ((__int128)1 << 66) && v >= -((__int128)1 << 66);
            }
        fd_stage = "lex";
        if (R2.k == LIA_RANK_FOUND && !fits) {
            R2 = lia_rank_lex_bounded(L->paths, L->npaths, L->pre, L->post, L->npairs, (int64_t)1 << 20, F->lim, F->work, F->worksz);
            fd_stage = "lex-bounded";
        }
        if (R2.k != LIA_RANK_FOUND || R2.ncomp > JRANK_MAXC) return false;
        ncomp = R2.ncomp;
        for (int32_t j = 0; j < ncomp; j++) { coef[j] = R2.coef[j]; c0[j] = R2.c0[j]; }
        for (int32_t e = 0; e < L->nedges; e++) {
            fall_of_edge[e] = -2;                            // -2: no path yet, -1: paths disagree
            for (int32_t k = 0; k < L->npaths; k++) {
                if (L->edge[k] != L->edges[e]) continue;
                if (fall_of_edge[e] == -2) fall_of_edge[e] = R2.fall[k];
                else if (fall_of_edge[e] != R2.fall[k]) fall_of_edge[e] = -1;
            }
            // the paths of one edge lower different components (Fig7b's <y, z, x>: one path lowers
            // each): the edge is FREE, and the judge owes the lexicographic decrease itself
            // (rank.lex, c35). lia_rank_lex bounds every component on every path, which is more
            // than the free edge's per-disjunct bound asks.
            if (fall_of_edge[e] == -2) return false;
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
// ── PE.12: inputs that may make a loop run forever (untrusted: the caller runs each) ───────────
// For a header the judge refused, a model of: the entry edge E (the state on arrival, in terms of
// f's parameters), then `stem` laps, then `period` laps that end in the state they began in. Each
// is one lia_solve on the paths gen->paths gives, stitched: one lap's post is the next one's pre,
// pair by pair, and the cycle's last post its first pre. A value fixed at the header (a parameter,
// anything defined before the loop: term_kinds 1) is ONE variable on every path; every other
// variable is its own path's (a temporary is a fresh value each lap). The paths do not say what an
// unclaimed call returns, so a model may describe a run that cannot happen: a candidate is a
// witness only once the interpreter has run it into a repeated state (ir_interpret_witness).
// A cell only the loop's own counting reads (lap_only.h) is no link: the run leaves it out of the
// state it compares, and a lap counter would leave the cycle with no model at all.
#define FD_WMAXP 16
#define FD_WMAXQ 32
#define FD_WMAXK 8
// key[0..nkey): the header's versions of integer cells (JTERM_VALUE pairs), for whoever reports
typedef struct { __int128 val[FD_WMAXP]; uint8_t given[FD_WMAXP]; int32_t stem, period, nkey, key[FD_WMAXK]; } FdWitness;
typedef struct { JVc vc; JPathPair *pairs; int32_t npairs; bool back; } FdWPath;
static bool fd_wcopy_cons(FdLoop *R, JCons *d, const JCons *s) {
    *d = *s; d->t = fd_bump(R, sizeof(JTerm) * (size_t)(s->nt ? s->nt : 1));
    if (!d->t) return false;
    if (s->nt) memcpy(d->t, s->t, sizeof(JTerm) * (size_t)s->nt);
    return true;
}
static bool fd_wcopy_path(FdLoop *R, FdWPath *d, const JPath *s) {
    const JVc *v = s->vc;
    memset(d, 0, sizeof *d);
    d->vc = *v; d->back = s->back; d->npairs = s->npairs;
    d->vc.vars = fd_bump(R, sizeof(char *) * (size_t)(v->nvars ? v->nvars : 1));
    d->vc.hyps = fd_bump(R, sizeof(JHyp) * (size_t)(v->nhyps ? v->nhyps : 1));
    d->pairs = fd_bump(R, sizeof(JPathPair) * (size_t)(s->npairs ? s->npairs : 1));
    if (!d->vc.vars || !d->vc.hyps || !d->pairs) return false;
    for (int32_t i = 0; i < v->nvars; i++) {
        size_t n = v->vars[i] ? strlen(v->vars[i]) + 1 : 1;
        char *c = fd_bump(R, n);
        if (!c) return false;
        if (v->vars[i]) memcpy(c, v->vars[i], n); else c[0] = 0;
        d->vc.vars[i] = c;
    }
    for (int32_t i = 0; i < v->nhyps; i++) {
        const JHyp *h = &v->hyps[i]; JHyp *o = &d->vc.hyps[i];
        *o = *h; o->origin = NULL;
        if (!h->disj) { if (!fd_wcopy_cons(R, &o->cons, &h->cons)) return false; continue; }
        o->dis = fd_bump(R, sizeof(JConj) * (size_t)h->ndis);
        if (!o->dis) return false;
        for (int32_t q = 0; q < h->ndis; q++) {
            o->dis[q].nc = h->dis[q].nc;
            o->dis[q].c = fd_bump(R, sizeof(JCons) * (size_t)(h->dis[q].nc ? h->dis[q].nc : 1));
            if (!o->dis[q].c) return false;
            for (int32_t k = 0; k < h->dis[q].nc; k++) if (!fd_wcopy_cons(R, &o->dis[q].c[k], &h->dis[q].c[k])) return false;
        }
    }
    if (s->npairs) memcpy(d->pairs, s->pairs, sizeof(JPathPair) * (size_t)s->npairs);
    return true;
}
static const JPathPair *fd_wpair(const FdWPath *p, const JPathPair *of) {
    for (int32_t k = 0; k < p->npairs; k++) if (p->pairs[k].key == of->key && p->pairs[k].tk == of->tk) return &p->pairs[k];
    return NULL;
}
// a - b = 0
static bool fd_weq(FdLoop *R, JHyp *h, int32_t a, int32_t b) {
    memset(h, 0, sizeof *h); h->origin = "witness";
    h->cons.op = J_EQ; fd_jint(&h->cons.rhs, 0);
    h->cons.t = fd_bump(R, sizeof(JTerm) * 2);
    if (!h->cons.t) return false;
    if (a == b) { h->cons.nt = 0; return true; }
    int32_t lo = a < b ? a : b, hi = a < b ? b : a;
    h->cons.nt = 2;
    h->cons.t[0].var = lo; fd_jint(&h->cons.t[0].coef, lo == a ? 1 : -1);
    h->cons.t[1].var = hi; fd_jint(&h->cons.t[1].coef, hi == a ? 1 : -1);
    return true;
}
// One stitched query: E, then laps seq[0..n) (the first `stem` lead in), the rest a cycle. True with
// *out filled on a model.
static bool fd_wquery(JFinder *F, IrFunc *f, FdLoop *R, const FdWPath *E, const FdWPath *const *seq, int32_t n,
                      int32_t stem, const int32_t *shared, int32_t nshared, const uint8_t *lap, int32_t nlap,
                      FdWitness *out) {
    char *mark = R->cur;
    int32_t np = n + 1;
    const FdWPath *P[1 + 4];
    P[0] = E; for (int32_t j = 0; j < n; j++) P[1 + j] = seq[j];
    int32_t *base = fd_bump(R, sizeof(int32_t) * (size_t)np), nv = nshared;
    if (!base) return false;
    for (int32_t j = 0; j < np; j++) { base[j] = nv; nv += P[j]->vc.nvars; }
    int32_t **map = fd_bump(R, sizeof(int32_t *) * (size_t)np), nh = 0;
    if (!map) { R->cur = mark; return false; }
    for (int32_t j = 0; j < np; j++) {
        map[j] = fd_bump(R, sizeof(int32_t) * (size_t)(P[j]->vc.nvars ? P[j]->vc.nvars : 1));
        if (!map[j]) { R->cur = mark; return false; }
        for (int32_t x = 0; x < P[j]->vc.nvars; x++) {
            int32_t id = fd_value_id(P[j]->vc.vars[x]), s = -1;
            for (int32_t q = 0; q < nshared && s < 0 && id >= 0; q++) if (shared[q] == id) s = q;
            map[j][x] = s >= 0 ? s : base[j] + x;
        }
        nh += P[j]->vc.nhyps;
    }
    nh += 2 * np * (E->npairs + 1);
    JVc q; memset(&q, 0, sizeof q);
    q.id = E->vc.id; q.nvars = nv;
    q.vars = fd_bump(R, sizeof(char *) * (size_t)(nv ? nv : 1));
    q.hyps = fd_bump(R, sizeof(JHyp) * (size_t)(nh ? nh : 1));
    if (!q.vars || !q.hyps) { R->cur = mark; return false; }
    for (int32_t j = 0; j < np; j++)
        for (int32_t x = 0; x < P[j]->vc.nvars; x++) q.vars[map[j][x]] = P[j]->vc.vars[x];
    for (int32_t i = 0; i < nv; i++) if (!q.vars[i]) q.vars[i] = "";
    int32_t h = 0;
    for (int32_t j = 0; j < np; j++)
        for (int32_t i = 0; i < P[j]->vc.nhyps; i++) {
            const JHyp *src = &P[j]->vc.hyps[i]; JHyp *dst = &q.hyps[h++];
            *dst = *src;
            if (!src->disj) { if (!fd_map_cons(R, &src->cons, map[j], &dst->cons)) { R->cur = mark; return false; } continue; }
            dst->dis = fd_bump(R, sizeof(JConj) * (size_t)src->ndis);
            if (!dst->dis) { R->cur = mark; return false; }
            for (int32_t d = 0; d < src->ndis; d++) {
                dst->dis[d].nc = src->dis[d].nc;
                dst->dis[d].c = fd_bump(R, sizeof(JCons) * (size_t)(src->dis[d].nc ? src->dis[d].nc : 1));
                if (!dst->dis[d].c) { R->cur = mark; return false; }
                for (int32_t k = 0; k < src->dis[d].nc; k++)
                    if (!fd_map_cons(R, &src->dis[d].c[k], map[j], &dst->dis[d].c[k])) { R->cur = mark; return false; }
            }
        }
    // the links: E's post is the first lap's pre, each lap's post the next one's pre, and the
    // cycle's last post its first pre; pair by pair, a pair a path lacks is no link (it says less)
    for (int32_t k = 0; k < E->npairs; k++) {
        const JPathPair *ep = &E->pairs[k];
        if (ep->key >= 0 && ep->key < nlap && lap[ep->key]) continue;
        int32_t prev = ep->post >= 0 ? map[0][ep->post] : -1, first = -1;
        for (int32_t j = 1; j < np && prev >= 0; j++) {
            const JPathPair *lp = fd_wpair(P[j], ep);
            if (!lp || lp->pre < 0 || lp->post < 0) { prev = -1; break; }
            if (!fd_weq(R, &q.hyps[h++], prev, map[j][lp->pre])) { R->cur = mark; return false; }
            if (j == stem + 1) first = map[j][lp->pre];
            prev = map[j][lp->post];
        }
        if (prev >= 0 && first >= 0 && !fd_weq(R, &q.hyps[h++], prev, first)) { R->cur = mark; return false; }
    }
    q.nhyps = h;
    q.goal.op = J_LE; q.goal.nt = 0; fd_jint(&q.goal.rhs, -1);   // 0 <= -1 is false: a model is the answer
    JWork lim = F->lim; lim.pivot /= 16; lim.node /= 16; lim.leaf /= 16;
    JAnswer A = lia_solve(&q, lim, F->work, F->worksz);
    bool got = A.k == J_ANS_MODEL;
    if (got) {
        memset(out, 0, sizeof *out); out->stem = stem; out->period = n - stem;
        for (int32_t k = 0; k < E->npairs && out->nkey < FD_WMAXK; k++)
            if (E->pairs[k].tk == JTERM_VALUE && !(E->pairs[k].key >= 0 && E->pairs[k].key < nlap && lap[E->pairs[k].key]))
                out->key[out->nkey++] = E->pairs[k].key;
        int32_t k = 0;
        for (IrParam *pp = f->params; pp && k < FD_WMAXP; pp = pp->next, k++) {
            if (!pp->value) continue;
            char nm[24]; snprintf(nm, sizeof nm, "%%v%d", pp->value->id);
            for (int32_t i = 0; i < nv && !out->given[k]; i++)
                if (q.vars[i] && !strcmp(q.vars[i], nm)) { out->val[k] = fd_int128(&A.model.val[i]); out->given[k] = 1; }
        }
    }
    R->cur = mark;
    return got;
}
// A candidate's variants, tried in order until one runs into a repeat: 0 the model as it is; 1 every
// integer or bool parameter the model leaves free set to 1 (true); 2 every free signed one to -1.
// A free parameter is one no path's facts name: the generator states a path's facts for the
// header's versions, and `while flag {}` has none, so its test on `flag` was in no VC and g(true)
// was never tried. False when variant k is the same input as an earlier one.
static bool fd_witness_variant(const FdWitness *in, IrFunc *f, int k, FdWitness *out) {
    *out = *in;
    if (k == 0) return true;
    bool changed = false; int q = 0;
    for (IrParam *p = f->params; p && q < FD_WMAXP; p = p->next, q++) {
        const IrType *ty = p->value ? p->value->type : NULL;
        if (in->given[q] || !ty || (ty->kind != IRT_INT && ty->kind != IRT_BOOL)) continue;
        if (k == 1) { out->val[q] = 1; out->given[q] = 1; changed = true; }
        else if (k == 2 && ty->kind == IRT_INT && ty->is_signed) { out->val[q] = -1; out->given[q] = 1; changed = true; }
    }
    return changed;
}
// PE.12 w5: inputs for a recursion to be run on, smallest first: each of f's first two integer or
// bool parameters over 0, 1, -1, 2, 3, -2, 4 (a bool over false, true; an unsigned parameter
// skips the negative ones), the others 0. Any input a run confirms is a witness however it was
// proposed: the run decides, not this list. The number written to out (at most max).
static int32_t fd_witness_enum(IrFunc *f, FdWitness *out, int32_t max) {
    static const int64_t vals[] = { 0, 1, -1, 2, 3, -2, 4 };
    int32_t slot[2] = { -1, -1 }, ns = 0, q = 0, n[2] = { 1, 1 };
    for (IrParam *p = f->params; p && q < FD_WMAXP; p = p->next, q++) {
        const IrType *ty = p->value ? p->value->type : NULL;
        if (ns < 2 && ty && (ty->kind == IRT_INT || ty->kind == IRT_BOOL)) {
            n[ns] = ty->kind == IRT_BOOL ? 2 : 7; slot[ns++] = q;
        }
    }
    int32_t got = 0;
    for (int32_t a = 0; a < n[0] && got < max; a++)
        for (int32_t b = 0; b < n[1] && got < max; b++) {
            FdWitness w; memset(&w, 0, sizeof w);
            bool ok = true; q = 0;
            for (IrParam *p = f->params; p && q < FD_WMAXP; p = p->next, q++) {
                const IrType *ty = p->value ? p->value->type : NULL;
                if (!ty || (ty->kind != IRT_INT && ty->kind != IRT_BOOL)) continue;
                int64_t v = q == slot[0] ? vals[a] : q == slot[1] ? vals[b] : 0;
                if (v < 0 && (ty->kind == IRT_BOOL || !ty->is_signed)) ok = false;
                w.val[q] = v; w.given[q] = 1;
            }
            if (ok) out[got++] = w;
        }
    return got;
}
// Up to `max` candidates for header H of f under the claims cl (the judge's): the number found.
static int32_t fd_witness(JFinder *F, IrFunc *f, int32_t H, const JClaim *cl, int32_t ncl, FdWitness *out, int32_t max) {
    char err[200];
    FdLoop R; memset(&R, 0, sizeof R);
    size_t cap = (size_t)1 << 22;
    if (!(R.mem = malloc(cap))) return 0;
    R.cur = R.mem; R.end = R.mem + cap;
    JPaths P;
    if (!F->gen->paths || !F->gen->paths(F->gen->ctx, f, cl, ncl, H, &P, err, sizeof err) || P.refused) { free(R.mem); return 0; }
    // copied out first: the generator's next call (term_kinds) reuses its region
    FdWPath *W = fd_bump(&R, sizeof *W * (size_t)(P.n ? P.n : 1));
    int32_t nw = 0;
    for (int32_t k = 0; W && k < P.n; k++) if (fd_wcopy_path(&R, &W[nw], &P.p[k])) nw++;
    if (!W || nw != P.n) { free(R.mem); return 0; }
    // the values fixed at the header, across every path: one variable each
    int32_t nval = 0, *vals = fd_bump(&R, sizeof(int32_t) * 256), *vid = fd_bump(&R, sizeof(int32_t) * 256);
    uint8_t *vtk = fd_bump(&R, 256), *kind = fd_bump(&R, 256);
    if (!vals || !vid || !vtk || !kind) { free(R.mem); return 0; }
    for (int32_t k = 0; k < nw; k++)
        for (int32_t x = 0; x < W[k].vc.nvars; x++) {
            int32_t id = fd_value_id(W[k].vc.vars[x]); bool seen = false;
            if (id < 0) continue;
            for (int32_t q = 0; q < nval && !seen; q++) seen = vals[q] == id;
            if (!seen && nval < 256) vals[nval++] = id;
        }
    for (int32_t q = 0; q < nval; q++) { vid[q] = vals[q] / 2; vtk[q] = (uint8_t)(vals[q] % 2); }
    if (nval && (!F->gen->term_kinds || !F->gen->term_kinds(F->gen->ctx, f, H, vid, vtk, nval, kind, err, sizeof err))) { free(R.mem); return 0; }
    int32_t nshared = 0;
    for (int32_t q = 0; q < nval; q++) if (kind[q] == 1) vals[nshared++] = vals[q];
    const FdWPath *back[FD_MAXEDGES]; int32_t nb = 0;
    for (int32_t k = 0; k < nw && nb < FD_MAXEDGES; k++) if (W[k].back) back[nb++] = &W[k];
    int32_t nlap = f->next_value_id > 0 ? f->next_value_id : 1;
    uint8_t *lap = fd_bump(&R, (size_t)nlap);
    const IrBlock *hb = NULL;
    for (const IrBlock *b = f->blocks; b && !hb; b = b->next) if (b->id == H) hb = b;
    if (!lap || !ir_lap_only(f, hb, lap)) nlap = 0;
    int32_t got = 0, asked = 0;
    // stem 0 or 1 lap, then a cycle of 1 or 2: the shortest first
    static const int32_t shape[][2] = { {0, 1}, {0, 2}, {1, 1}, {1, 2} };
    for (int32_t s = 0; s < 4 && got < max; s++) {
        int32_t stem = shape[s][0], n = stem + shape[s][1], combos = 1;
        for (int32_t j = 0; j < n; j++) combos *= nb;
        for (int32_t e = 0; e < nw && got < max; e++) {
            if (W[e].back) continue;
            for (int32_t c = 0; c < combos && got < max && asked < FD_WMAXQ; c++) {
                const FdWPath *seq[4]; int32_t r = c;
                for (int32_t j = 0; j < n; j++) { seq[j] = back[r % nb]; r /= nb; }
                asked++;
                if (fd_wquery(F, f, &R, &W[e], seq, n, stem, vals, nshared, lap, nlap, &out[got])) got++;
            }
        }
    }
    free(R.mem);
    return got;
}
// PE.7: the two-variable relations the loop keeps (lia_relations), as candidates for the header's
// claims, bounded like any other (bound_candidates) and kept by Houdini only if inductive. One of
// the two terms must be a version the header makes (a claim names one); a relation between two
// values fixed at the header says nothing a claim may state.
struct FdNest;
static bool fd_elem_covered(const struct FdNest *N, IrFunc *f, int32_t H, int32_t key, const JClaim *as, int32_t nv);
static bool fd_relation_candidates(JFinder *F, IrFunc *f, int32_t H, const FdLoop *L, int32_t npk,
                                   const struct FdNest *N, const JClaim *as, int32_t nv,
                                   JCand **cd, int32_t *n, int32_t *cap) {
    LRel rel[32];
    // a sixteenth of the budget per question: a trip's change is asked per pair and path (Gothenburg:
    // 220 maximizations a header over paths carrying every verified claim, 7 times the whole run's
    // time), and one that runs out only costs that pair its relation
    JWork lim = F->lim; lim.pivot /= 16; lim.node /= 16; lim.leaf /= 16;
    int32_t nr = lia_relations(L->paths, L->npaths, L->pre, L->post, L->npairs, lim, F->work, F->worksz, rel, 32);
    for (int32_t q = 0; q < nr; q++) {
        if (rel[q].t >= npk && rel[q].j >= npk) continue;
        if (fd_elem_covered(N, f, H, L->pkey[rel[q].t], as, nv) || fd_elem_covered(N, f, H, L->pkey[rel[q].j], as, nv)) continue;
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

// A function's loops, computed once per proposal round (fd_nest_build): per loop header, its
// natural loop by block id (fd_loop_body) and its size, so the innermost loop around a block is
// the smallest body containing it. Empty (nh 0) when the table would pass 1 << 24 bytes, which
// costs the candidates that need it, nothing else.
// ebase[v]: the array (its address value) whose element address v is, an ELEM_PTR's result, else -1.
typedef struct FdNest { int32_t nbk, nh, *hid, *sz; uint8_t *in; int32_t nid, *ebase; } FdNest;
static bool fd_loop_body(IrFunc *f, int32_t H, uint8_t *in, int32_t nb);
static void fd_nest_free(FdNest *N) { free(N->hid); free(N->sz); free(N->in); free(N->ebase); memset(N, 0, sizeof *N); }
static bool fd_nest_build(IrFunc *f, FdNest *N) {
    memset(N, 0, sizeof *N);
    for (IrBlock *b = f->blocks; b; b = b->next)
        for (IrInstr *i = b->instrs; i; i = i->next) if (i->result && i->result->id >= N->nid) N->nid = i->result->id + 1;
    if (N->nid > 0) {
        if (!(N->ebase = malloc(sizeof *N->ebase * (size_t)N->nid))) return false;
        for (int32_t v = 0; v < N->nid; v++) N->ebase[v] = -1;
        for (IrBlock *b = f->blocks; b; b = b->next)
            for (IrInstr *i = b->instrs; i; i = i->next)
                if (i->op == IR_ELEM_PTR && i->result && i->result->id >= 0 && i->n_operands >= 1 && i->operands[0])
                    N->ebase[i->result->id] = i->operands[0]->id;
    }
    for (IrBlock *b = f->blocks; b; b = b->next) { if (b->id >= N->nbk) N->nbk = b->id + 1; if (b->is_loop_header) N->nh++; }
    if (N->nh == 0 || N->nbk <= 0) { N->nh = 0; return true; }
    if ((int64_t)N->nh * N->nbk > ((int64_t)1 << 24)) { N->nh = 0; return true; }
    N->hid = malloc(sizeof *N->hid * (size_t)N->nh); N->sz = malloc(sizeof *N->sz * (size_t)N->nh);
    N->in = malloc((size_t)N->nh * (size_t)N->nbk);
    if (!N->hid || !N->sz || !N->in) { fd_nest_free(N); return false; }
    int32_t h = 0;
    for (IrBlock *b = f->blocks; b; b = b->next) {
        if (!b->is_loop_header) continue;
        uint8_t *row = N->in + (size_t)h * (size_t)N->nbk;
        if (!fd_loop_body(f, b->id, row, N->nbk)) { memset(row, 0, (size_t)N->nbk); row[b->id] = 1; }
        N->hid[h] = b->id; N->sz[h] = 0;
        for (int32_t q = 0; q < N->nbk; q++) N->sz[h] += row[q];
        h++;
    }
    return true;
}
// Header H's loop (NULL when H is not one), and the innermost loop around block B other than
// `except`'s (-1: none)
static const uint8_t *fd_nest_body(const FdNest *N, int32_t H) {
    for (int32_t h = 0; h < N->nh; h++) if (N->hid[h] == H) return N->in + (size_t)h * (size_t)N->nbk;
    return NULL;
}
// Is value `key` an element of an array that a verified plain EACH claim at block H already bounds?
// Then a candidate about that element alone repeats it (`a[2] <= 10` beside `each(a) <= 10`), and a
// relation of it with a counter (`a[2] - 10*i <= 0`) is noise: each one rides into every VC that
// names the element, and they made forloop1's solves five times slower.
static bool fd_elem_covered(const struct FdNest *N, IrFunc *f, int32_t H, int32_t key, const JClaim *as, int32_t nv) {
    if (!N || key < 0 || key >= N->nid || !N->ebase || N->ebase[key] < 0) return false;
    int32_t base = N->ebase[key];
    for (int32_t v = 0; v < nv; v++)
        if (as[v].fn == f && as[v].blk == H && as[v].nt == 1 && as[v].tk[0] == JTERM_EACH && as[v].key[0] == base) return true;
    return false;
}
static int32_t fd_nest_around(const FdNest *N, int32_t B, int32_t except) {
    int32_t best = -1;
    if (B < 0 || B >= N->nbk) return -1;
    for (int32_t h = 0; h < N->nh; h++) {
        if (N->hid[h] == except || !N->in[(size_t)h * (size_t)N->nbk + (size_t)B]) continue;
        if (best < 0 || N->sz[h] < N->sz[best]) best = h;
    }
    return best < 0 ? -1 : N->hid[best];
}
// The natural loop of header H: in[b] = 1 for H and every block that reaches one of its back edges
// without passing H; a back edge's source is a predecessor of H that the entry reaches only through
// H (H dominates it). A sketch from the IR's predecessor lists: a wrong answer costs candidates,
// never a verdict (each is owed). False when f's blocks are not numbered below `nb`.
static bool fd_loop_body(IrFunc *f, int32_t H, uint8_t *in, int32_t nb) {
    memset(in, 0, (size_t)nb);
    IrBlock **by = calloc((size_t)nb, sizeof *by);
    int32_t *nsu = calloc((size_t)nb + 1, sizeof *nsu), *su = NULL, *at = NULL, *st = malloc(sizeof *st * (size_t)nb);
    uint8_t *seen = calloc((size_t)nb, 1);
    bool ok = by && nsu && st && seen && f->entry;
    for (IrBlock *b = f->blocks; ok && b; b = b->next) { if (b->id < 0 || b->id >= nb) ok = false; else by[b->id] = b; }
    // successors from the predecessor lists, in CSR form
    int32_t ne = 0;
    for (IrBlock *b = f->blocks; ok && b; b = b->next)
        for (IrEdge *e = b->preds; e; e = e->next) { if (!e->block || e->block->id < 0 || e->block->id >= nb) ok = false; else { nsu[e->block->id + 1]++; ne++; } }
    if (ok) { for (int32_t x = 0; x < nb; x++) nsu[x + 1] += nsu[x]; su = malloc(sizeof *su * (size_t)(ne ? ne : 1)); at = malloc(sizeof *at * (size_t)nb); ok = su && at; }
    if (ok) {
        memcpy(at, nsu, sizeof *at * (size_t)nb);
        for (IrBlock *b = f->blocks; b; b = b->next) for (IrEdge *e = b->preds; e; e = e->next) su[at[e->block->id]++] = b->id;
        // what the entry reaches without passing H
        int32_t sp = 0;
        if (f->entry->id != H) { seen[f->entry->id] = 1; st[sp++] = f->entry->id; }
        while (sp > 0) {
            int32_t x = st[--sp];
            for (int32_t k = nsu[x]; k < nsu[x + 1]; k++) if (su[k] != H && !seen[su[k]]) { seen[su[k]] = 1; st[sp++] = su[k]; }
        }
        // back to H from each back edge's source, never through H
        in[H] = 1;
        for (IrEdge *e = by[H] ? by[H]->preds : NULL; e; e = e->next)
            if (!seen[e->block->id] && !in[e->block->id]) { in[e->block->id] = 1; st[sp++] = e->block->id; }
        while (sp > 0) {
            int32_t x = st[--sp];
            for (IrEdge *e = by[x] ? by[x]->preds : NULL; e; e = e->next)
                if (!in[e->block->id]) { in[e->block->id] = 1; st[sp++] = e->block->id; }
        }
    }
    free(by); free(nsu); free(su); free(at); free(st); free(seen);
    return ok;
}
// One block's candidates (the slots sl[i..j), all at one block), into `cd`.
// Whether cell `key` of f holds one CONSTANT wherever it is read: the element (or field) of local
// storage that only constant stores at constant positions ever write, each position once, and
// whose address reaches nothing but loads, stores, element or field addresses, slices of it, a
// local slot holding only such views, and direct calls whose callee neither writes nor retains
// that parameter (its C5 footprints; `matmul(a, b, var c)` reads a and b). A header version paired with such a cell (`x - c <= k`) says
// what the version's own bound says with the constant folded in, so the pair is not proposed:
// a 32-byte string literal read in a loop was 32 such terms at every header, 8 candidates a
// byte, and the function's claims overflowed the generator's region and were all dropped (TCW's
// real programs, R2: 132 of the 181 claims kept at 1 GB were these). A candidate filter only: a
// cell wrongly taken as constant costs a candidate, never a fact.
static IrInstr *fd_def_in(IrFunc *f, const IrValue *v) {
    for (IrBlock *b = f->blocks; v && b; b = b->next)
        for (IrInstr *x = b->instrs; x; x = x->next) if (x->result == v) return x;
    return NULL;
}
static const IrValue *fd_storage_root_d(IrFunc *f, const IrValue *v, int depth);
// A local slot that only ever holds views of ONE storage (`var data = "..."` keeps a slice of the
// literal's copy): every store to it stores a value of that storage, and its own address reaches
// nothing but those stores and loads. That storage, or NULL.
static const IrValue *fd_slot_root(IrFunc *f, const IrValue *slot, int depth) {
    IrInstr *sd = fd_def_in(f, slot);
    if (!sd || sd->op != IR_ALLOCA || depth > 4) return NULL;
    const IrValue *r = NULL;
    for (IrBlock *b = f->blocks; b; b = b->next)
        for (IrInstr *x = b->instrs; x; x = x->next)
            for (int o = 0; o < x->n_operands; o++) {
                if (x->operands[o] != slot) continue;
                if (x->op == IR_LOAD && o == 0) continue;
                if (x->op != IR_STORE || o != 0 || x->n_operands < 2) return NULL;
                const IrValue *r2 = fd_storage_root_d(f, x->operands[1], depth + 1);
                if (!r2 || (r && r2 != r)) return NULL;
                r = r2;
            }
    return r;
}
static const IrValue *fd_storage_root(IrFunc *f, const IrValue *v) { return fd_storage_root_d(f, v, 0); }
static const IrValue *fd_storage_root_d(IrFunc *f, const IrValue *v, int depth) {
    for (int hop = 0; v && hop < 8; hop++) {
        IrInstr *d = fd_def_in(f, v);
        if (!d) return NULL;
        if (d->op == IR_ALLOCA) return v;
        if (d->op == IR_LOAD && d->n_operands >= 1) return fd_slot_root(f, d->operands[0], depth);
        if ((d->op != IR_ELEM_PTR && d->op != IR_FIELD_PTR && d->op != IR_MAKE_SLICE && d->op != IR_SLICE_DATA) ||
            d->n_operands < 1) return NULL;                     // a slice of it is still it (`data[i]`, `data.len`)
        v = d->operands[0];
    }
    return NULL;
}
static bool fd_const_cell(const JFinder *F, IrFunc *f, int32_t key) {
    const IrValue *kv = NULL;
    for (IrBlock *b = f->blocks; b && !kv; b = b->next)
        for (IrInstr *x = b->instrs; x && !kv; x = x->next) if (x->result && x->result->id == key) kv = x->result;
    IrInstr *kd = kv ? fd_def_in(f, kv) : NULL;
    if (!kd || (kd->op != IR_ELEM_PTR && kd->op != IR_FIELD_PTR)) return false;
    const IrValue *root = fd_storage_root(f, kv);
    if (!root) return false;
    __int128 seen[64]; int nseen = 0;
    for (IrBlock *b = f->blocks; b; b = b->next)
        for (IrInstr *x = b->instrs; x; x = x->next)
            for (int o = 0; o < x->n_operands; o++) {
                const IrValue *ov = x->operands[o];
                if (!ov || fd_storage_root(f, ov) != root) continue;
                if ((x->op == IR_LOAD || x->op == IR_SLICE_LEN || x->op == IR_MAKE_SLICE || x->op == IR_SLICE_DATA) && o == 0) continue;
                if ((x->op == IR_ELEM_PTR || x->op == IR_FIELD_PTR) && o == 0) {
                    if (x->op == IR_ELEM_PTR) {                 // a position known, or no promise
                        IrInstr *ix = x->n_operands >= 2 ? fd_def_in(f, x->operands[1]) : NULL;
                        bool stored = false;                    // ...needed only where it is stored to
                        for (IrBlock *b2 = f->blocks; b2 && !stored; b2 = b2->next)
                            for (IrInstr *y = b2->instrs; y && !stored; y = y->next)
                                stored = y->op == IR_STORE && y->n_operands >= 1 && y->operands[0] == x->result;
                        if (stored && (!ix || ix->op != IR_CONST)) return false;
                    }
                    continue;
                }
                if (x->op == IR_STORE && o == 1 && fd_slot_root(f, x->operands[0], 0) == root) continue;
                if (x->op == IR_CALL) {                         // a direct call that neither writes nor
                    const IrFunc *c = fd_callee(F, x);          // keeps that parameter (C5's footprints,
                    if (c && o < 64 && c->param_writes_done && c->param_retains_done &&   // where the
                        !((c->param_writes >> o) & 1) && !((c->param_retains >> o) & 1)) continue;   // analyses
                }                                                // computed them; else it may write)
                if (x->op == IR_STORE && o == 0 && x->n_operands >= 2) {
                    IrInstr *vd = fd_def_in(f, x->operands[1]);
                    IrInstr *ad = fd_def_in(f, ov);
                    if (!vd || vd->op != IR_CONST || !ad || ad->op != IR_ELEM_PTR || ad->n_operands < 2) return false;
                    IrInstr *ix = fd_def_in(f, ad->operands[1]);
                    if (!ix || ix->op != IR_CONST) return false;
                    __int128 at = fd_const(ix);
                    for (int q = 0; q < nseen; q++) if (seen[q] == at) return false;   // stored twice
                    if (nseen == 64) return false;
                    seen[nseen++] = at;
                    continue;
                }
                return false;                                   // a call, the address stored, a cast
            }
    return true;
}
// Are two addresses one (gen.h's jg_same_addr, copied: untrusted): one value, or the same FIELD_PTR
// (one field) or ELEM_PTR of operands that are pairwise one or equal constants, to a small depth.
// Each `l.pos` computes its own FIELD_PTR; the slot's key is one of them.
static bool fd_same_addr(IrFunc *f, IrValue *a, IrValue *b, int depth) {
    if (a == b) return true;
    if (!a || !b || depth > 4) return false;
    IrInstr *x = fd_def_in(f, a), *y = fd_def_in(f, b);
    if (!x || !y || x->op != y->op || (x->op != IR_ELEM_PTR && x->op != IR_FIELD_PTR) || x->n_operands != y->n_operands) return false;
    if (x->op == IR_FIELD_PTR && x->aux.field_idx != y->aux.field_idx) return false;
    for (int k = 0; k < x->n_operands; k++) {
        IrInstr *cx = fd_def_in(f, x->operands[k]), *cy = fd_def_in(f, y->operands[k]);
        if (cx && cy && cx->op == IR_CONST && cy->op == IR_CONST) { if (fd_const(cx) != fd_const(cy)) return false; continue; }
        if (!fd_same_addr(f, x->operands[k], y->operands[k], depth + 1)) return false;
    }
    return true;
}
static IrValue *fd_value_by_id(IrFunc *f, int32_t id) {
    for (IrParam *p = f->params; p; p = p->next) if (p->value && p->value->id == id) return p->value;
    for (IrBlock *b = f->blocks; b; b = b->next)
        for (IrInstr *i = b->instrs; i; i = i->next) if (i->result && i->result->id == id) return i->result;
    return NULL;
}
// Whether value v reads cell `key` as it is (a LOAD of its address, or a cast of one); key -1: any
// cell. *under: the value below the casts.
static bool fd_reads_cell(IrFunc *f, IrValue *v, int32_t key, IrValue **under) {
    for (int hop = 0; hop < 3 && v; hop++) {
        if (under) *under = v;
        IrInstr *d = fd_def_in(f, v);
        if (!d) return false;                                // a parameter
        if (d->op == IR_LOAD && d->n_operands >= 1 && d->operands[0])
            return key < 0 || d->operands[0]->id == key || fd_same_addr(f, d->operands[0], fd_value_by_id(f, key), 0);
        if (d->op != IR_CAST || d->n_operands < 1) return false;
        v = d->operands[0];
    }
    return false;
}
// Whether instruction i's result is stored back into the cell its minuend reads (`x = x - d`: the
// cell's own update, not a measure of how far it went). Followed through casts, further arithmetic
// (`q = (q -? y) -? 1`, LeikeHeizmann-TACAS2014-Ex1) and the slots the lowering makes for itself: a
// checked `x -? c else panic(...)` subtracts in i64, casts back, and reaches x through an unnamed
// alloca (Mysore-2: x's own update searched as a measure).
static bool fd_stores_back(IrFunc *f, IrInstr *i, IrValue *minuend) {
    IrInstr *ld = fd_def_in(f, minuend);
    while (ld && ld->op == IR_CAST && ld->n_operands >= 1) ld = fd_def_in(f, ld->operands[0]);
    if (!ld || ld->op != IR_LOAD || ld->n_operands < 1 || !i->result) return false;
    IrValue *cell = ld->operands[0], *vs[16]; int nvs = 0;
    vs[nvs++] = i->result;
    for (int at = 0; at < nvs; at++)
        for (IrBlock *b = f->blocks; b; b = b->next)
            for (IrInstr *s = b->instrs; s; s = s->next) {
                bool uses = false;
                for (int o = 0; o < s->n_operands && !uses; o++) uses = s->operands[o] == vs[at];
                if (!uses) continue;
                if ((s->op == IR_CAST || s->op == IR_ADD || s->op == IR_SUB || s->op == IR_MUL) && s->result && nvs < 16) {
                    vs[nvs++] = s->result;                   // `q = (q -? y) -? 1`: still q's update
                    continue;
                }
                if (s->op != IR_STORE || s->n_operands < 2 || s->operands[1] != vs[at]) continue;
                if (fd_same_addr(f, s->operands[0], cell, 0)) return true;
                IrInstr *ad = fd_def_in(f, s->operands[0]);
                if (!ad || ad->op != IR_ALLOCA || s->operands[0]->src_name) continue;   // a temporary: its loads
                for (IrBlock *b2 = f->blocks; b2; b2 = b2->next)
                    for (IrInstr *l2 = b2->instrs; l2 && nvs < 16; l2 = l2->next)
                        if (l2->op == IR_LOAD && l2->n_operands >= 1 && l2->operands[0] == s->operands[0] && l2->result) {
                            bool have = false;
                            for (int q = 0; q < nvs && !have; q++) have = vs[q] == l2->result;
                            if (!have) vs[nvs++] = l2->result;
                        }
            }
    return false;
}
// Whether value v reads a slot the lowering made for itself (an alloca with no source name: `x +? a
// else panic(...)` keeps its sum in one), not a variable or a field the program named. Gothenburg_v2-1's
// `x = ((x +? a) -? b) -? 1` subtracts from two such slots: searched, they doubled its finder work.
static bool fd_temp_cell(IrFunc *f, IrValue *v) {
    for (int hop = 0; hop < 3 && v; hop++) {
        IrInstr *d = fd_def_in(f, v);
        if (!d) return false;
        if (d->op == IR_LOAD && d->n_operands >= 1 && d->operands[0]) {
            IrInstr *ad = fd_def_in(f, d->operands[0]);
            return ad && ad->op == IR_ALLOCA && !d->operands[0]->src_name;
        }
        if (d->op != IR_CAST || d->n_operands < 1) return false;
        v = d->operands[0];
    }
    return false;
}
// What f subtracts from cell `key` (-1: any cell), or puts below it as a subslice's lower end, to
// MEASURE it: `p - p0`, `l.pos - start`, `s[p0..p]`. Each such operation owes the cell at least the
// other side. Not the cell's own update (`x = x - d`, stored back) and not a constant (a bound is a
// single version's): every decrement in every loop searched cost the trap ports 22% more finder work
// and CookSeeZuleger-TACAS2013-Fig7b its ranking. Up to `max` values (below their casts; a load's
// cell itself), each once, with their types. A value not fixed where a claim would name it is
// filtered by term_kinds there.
static int fd_sub_partners(IrFunc *f, int32_t key, int32_t *out, const IrType **ty, int max) {
    int n = 0;
    for (IrBlock *b = f->blocks; b; b = b->next)
        for (IrInstr *i = b->instrs; i && n < max; i = i->next) {
            IrValue *a = i->op == IR_SUB && i->n_operands >= 2 ? i->operands[0] : i->op == IR_SUBSLICE && i->n_operands >= 3 ? i->operands[2] : NULL;
            IrValue *o = i->op == IR_SUB && i->n_operands >= 2 ? i->operands[1] : i->op == IR_SUBSLICE && i->n_operands >= 3 ? i->operands[1] : NULL;
            if (!a || !o || !fd_reads_cell(f, a, key, NULL) || (i->op == IR_SUB && fd_stores_back(f, i, a)) || fd_temp_cell(f, a)) continue;
            IrValue *ov = o;
            for (int hop = 0; hop < 3; hop++) {              // below the casts; a load's cell itself
                IrInstr *d = fd_def_in(f, ov);              // (`start = l.pos` is a slot of its own,
                if (d && d->op == IR_LOAD && d->n_operands >= 1 && d->operands[0]) { ov = d->operands[0]; break; }   // loaded at the `-`)
                if (!d || d->op != IR_CAST || d->n_operands < 1) break;
                ov = d->operands[0];
            }
            IrInstr *od = fd_def_in(f, ov);
            if (!ov || (od && od->op == IR_CONST) || ov->id == key) continue;
            bool seen = false;
            for (int q = 0; q < n && !seen; q++) seen = out[q] == ov->id;
            if (seen) continue;
            if (!out) return 1;                              // asked only whether there is one
            out[n] = ov->id; ty[n] = od && od->op == IR_ALLOCA ? od->aux.alloca_ty : ov->type; n++;
        }
    return n;
}
static bool block_candidates(JFinder *F, IrFunc *f, const FdNest *N, const JClaim *as, int32_t nv, const JSlot *sl, int32_t i, int32_t j,
                             JCand **cd, int32_t *ncd, int32_t *ccd, char *err, size_t errcap) {
    int32_t H = sl[i].blk, nt = 0;
    bool head = sl[i].head;
    JSlot *tm = NULL;
    uint8_t *cst = NULL;                                     // terms that are one constant (fd_const_cell)
    uint8_t *inl = NULL;                                     // the header's loop, by block id
    int32_t nbk = 0;
    *ncd = 0;
    {   // the terms: a header's pair with its versions below, and both kinds' compared values
        JSlot *t0;
        if (!F->gen->terms(F->gen->ctx, f, H, &t0, &nt, err, (uint32_t)errcap)) return false;
        if (!(tm = malloc(sizeof *tm * (size_t)(nt ? nt : 1)))) goto oom;
        memcpy(tm, t0, sizeof *tm * (size_t)nt);
    }
    if (!(cst = calloc((size_t)(nt ? nt : 1), 1))) goto oom;
    for (int32_t t = 0; head && t < nt; t++) cst[t] = fd_const_cell(F, f, tm[t].key);
    for (int32_t k = i; k < j; k++) {
        if (fd_elem_covered(N, f, H, sl[k].key, as, nv)) continue;
        if (!cand_push(cd, ncd, ccd, H, sl[k].key, 1, -1, 0, sl[k].type, NULL) ||
            !cand_push(cd, ncd, ccd, H, sl[k].key, -1, -1, 0, sl[k].type, NULL)) goto oom;
        for (int32_t t = 0; head && t < nt; t++) {
            if (tm[t].key == sl[k].key || cst[t]) continue;
            bool own = false;                                // two of the header's versions: one pair
            for (int32_t k2 = i; k2 < j; k2++) if (sl[k2].key == tm[t].key) own = true;
            if (own && tm[t].key < sl[k].key) continue;
            if (!cand_push(cd, ncd, ccd, H, sl[k].key, 1, tm[t].key, -1, sl[k].type, tm[t].type) ||
                !cand_push(cd, ncd, ccd, H, sl[k].key, -1, tm[t].key, 1, sl[k].type, tm[t].type)) goto oom;
        }
    }
    // ...and each version against what f later subtracts from its cell, or puts below it as a
    // subslice's lower end (fd_sub_partners: `p - p0`, `s[p0..p]`), at a join as at a header. Such an
    // operation owes the cell at least that value, and a join's claims are all that is said of its
    // version past it: `p = p0 + 1` on both arms of an inner `else if` was lost at that join, its
    // claims only `p >= 1`, and `return p - p0` failed (the handwritten lexer's ident, number and
    // one-character branches). A header's terms pair it already where they list it.
    for (int32_t k = i; k < j; k++) {
        int32_t pw[8]; const IrType *pt[8]; uint8_t pk[8];
        int npw = fd_sub_partners(f, sl[k].key, pw, pt, 8);
        if (npw == 0 || !F->gen->term_kinds(F->gen->ctx, f, H, pw, NULL, npw, pk, err, (uint32_t)errcap)) continue;
        for (int q = 0; q < npw; q++) {
            if (!pk[q]) continue;                            // not nameable here: no claim can say it
            bool have = false;
            for (int32_t t = 0; head && t < nt && !have; t++) have = tm[t].key == pw[q];
            if (have) continue;
            if (!cand_push(cd, ncd, ccd, H, sl[k].key, 1, pw[q], -1, sl[k].type, pt[q]) ||
                !cand_push(cd, ncd, ccd, H, sl[k].key, -1, pw[q], 1, sl[k].type, pt[q])) goto oom;
        }
    }
    // ...and each version made here against the SAME cell at the header of the innermost loop around
    // the block (its own loop excluded): s11's HEAD term, `head(i) - i <= -1` at the join after an
    // `if` whose branch runs a loop that only raises i. Without it nothing on the enclosing loop's
    // path relates i after that join to i at the header, and simdlex's three outer loops had no
    // ranking (R3c, TCW's real programs). Both sides, bounded as the pairs above are.
    // Only for a cell the enclosing loop's own test compares (`while i < n`), the cells its ranking
    // reads: every version against its header at every join was enough candidates to overflow the
    // generator's region in simdlex's `tokenize` (1445 proved -> 1034).
    {   int32_t encl = head ? fd_nest_around(N, H, H) : fd_nest_around(N, H, -1);
        int32_t tk_[8]; int ntk = 0;
        IrBlock *eb = NULL;
        for (IrBlock *b = encl >= 0 ? f->blocks : NULL; b && !eb; b = b->next) if (b->id == encl) eb = b;
        IrInstr *ec = eb && eb->term.cond ? fd_def_in(f, eb->term.cond) : NULL;
        for (int o = 0; ec && ec->op == IR_ICMP && o < ec->n_operands && o < 2; o++) {
            IrInstr *ld = fd_def_in(f, ec->operands[o]);
            if (ld && ld->op == IR_LOAD && ld->n_operands >= 1 && ld->operands[0] && ntk < 8) tk_[ntk++] = ld->operands[0]->id;
        }
        for (int32_t k = i; encl >= 0 && k < j; k++) {
            bool tested = false;
            for (int q = 0; q < ntk && !tested; q++) tested = tk_[q] == sl[k].key;
            if (!tested || !fd_is_int(sl[k].type)) continue;
            for (int sg = 1; sg >= -1; sg -= 2) {
                int32_t before = *ncd;
                if (!cand_push(cd, ncd, ccd, H, sl[k].key, sg, sl[k].key, -sg, sl[k].type, sl[k].type)) goto oom;
                if (*ncd == before + 1) (*cd)[before].c.tk[1] = JTERM_HEAD;
                // ...and fixed, as a monotone counter's claims are: `head(i) - i <= 0` and `<= -1`
                // (and the mirror for one that falls). The maximized bound alone lost them where a
                // function is larger: simdlex's join kept `head(i) - i <= 4095`, no ranking reads
                for (__int128 rhs = 0; rhs >= -1; rhs--) {
                    int32_t b2 = *ncd;
                    if (!cand_push(cd, ncd, ccd, H, sl[k].key, -sg, sl[k].key, sg, sl[k].type, sl[k].type)) goto oom;
                    if (*ncd == b2 + 1) { (*cd)[b2].c.tk[1] = JTERM_HEAD; (*cd)[b2].fixed = true; (*cd)[b2].c.rhs = rhs; }
                }
            }
        }
    }
    // the comparisons in the block's loop (a header's own; a join's innermost): a version made here
    // against a constant or another term, each side at the compared value give or take one (fixed
    // bounds, nothing to maximize). One in the body as well as the header's: `if n < m { n = n + 1 }`
    // keeps `n <= m`, and nothing maximized finds it, at the header (the claim is its own back
    // edge's hypothesis) or at the join after the `if` (its bound needs the header's)
    {   int32_t around = head ? H : fd_nest_around(N, H, -1);
        const uint8_t *body = around >= 0 ? fd_nest_body(N, around) : NULL;
        if (body) { nbk = N->nbk; if (!(inl = malloc((size_t)nbk))) goto oom; memcpy(inl, body, (size_t)nbk); }
    }
    // a compared value the terms do not list (a join's lists its own block's only) may still be
    // one a claim here can name (a parameter, a value fixed at the block): asked once, together
    int32_t xk[64]; const IrType *xt[64]; uint8_t xok[64]; int32_t nx = 0;
    for (IrBlock *hb = inl ? f->blocks : NULL; hb; hb = hb->next) {
        if (hb->id < 0 || hb->id >= nbk || !inl[hb->id]) continue;
        for (IrInstr *ic = hb->instrs; ic; ic = ic->next) {
            if (ic->op != IR_ICMP || ic->n_operands < 2) continue;
            for (int k = 0; k < 2; k++) {
                IrValue *o = ic->operands[k];
                IrInstr *d = NULL;
                for (IrInstr *x = hb->instrs; x && x != ic; x = x->next) if (x->result == o) d = x;
                if (!o || (d && (d->op == IR_CONST || d->op == IR_LOAD)) || !fd_is_int(o->type)) continue;
                bool have = false;
                for (int32_t t = 0; t < nt && !have; t++) have = tm[t].key == o->id;
                for (int32_t q = 0; q < nx && !have; q++) have = xk[q] == o->id;
                if (!have && nx < 64) { xk[nx] = o->id; xt[nx] = o->type; nx++; }
            }
        }
    }
    if (nx > 0 && !F->gen->term_kinds(F->gen->ctx, f, H, xk, NULL, nx, xok, err, (uint32_t)errcap)) nx = 0;
    for (IrBlock *hb = inl ? f->blocks : NULL; hb; hb = hb->next) {
      if (hb->id < 0 || hb->id >= nbk || !inl[hb->id]) continue;
      for (IrInstr *ic = hb->instrs; ic; ic = ic->next) {
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
            for (int32_t q = 0; q < nx && key[k] < 0; q++) if (xk[q] == kk && xok[q]) { key[k] = kk; ty[k] = xt[q]; }
        }
        // in the body, only a comparison that GUARDS a write to the cell it compares: its block's
        // branch, one way of which stores to that cell (`if n < m { n = n + 1 }`). Every comparison
        // of a loop at every cut point in it was 1786 candidates in the SIMD lexer's `tokenize`,
        // past what the generator holds in one function's VCs (the function lost every claim)
        bool guards[2] = { hb->id == H, hb->id == H };
        if (hb->id != H && hb->term.kind == IR_TERM_BR_COND && hb->term.cond == ic->result)
            for (int k = 0; k < 2; k++)
                for (int w = 0; w < 2 && !guards[k]; w++) {
                    IrBlock *t = w ? hb->term.b : hb->term.a;
                    for (IrInstr *st = t ? t->instrs : NULL; st && !guards[k]; st = st->next)
                        guards[k] = st->op == IR_STORE && st->n_operands >= 1 && st->operands[0] && st->operands[0]->id == key[k];
                }
        for (int k = 0; k < 2; k++) {
            int32_t x = -1;                                  // which header version this side is
            for (int32_t s2 = i; s2 < j; s2++) if (sl[s2].key == key[k]) x = s2;
            if (x < 0 || !guards[k] || (!isc[1 - k] && key[1 - k] < 0)) continue;
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
    }
    free(inl); inl = NULL;
    // VRA's relations at this header. VRA names a slice's length by the value an IR_SLICE_LEN
    // computed (`hi <= %3` with %3 = a.len before the loop); the generator names it len(<slice>)
    // (jb_len), so such a variable becomes that JTERM_LEN term, or the relation would have no
    // term the proposer can name and `hi <= a.len` (a binary search's bound) was never proposed.
    for (int h = 0; head && h < F->nhdr; h++) {
        if (F->hdr[h].f != f || F->hdr[h].bid != H || F->hdr[h].n == 0) continue;
        int32_t n2 = F->hdr[h].n * 2;
        int32_t *keys = malloc(sizeof *keys * (size_t)n2);
        uint8_t *kind = malloc((size_t)n2), *ktk = malloc((size_t)n2);
        const IrType **lty = malloc(sizeof *lty * (size_t)n2);
        if (!keys || !kind || !ktk || !lty) { free(keys); free(kind); free(ktk); free(lty); goto oom; }
        for (int r = 0; r < F->hdr[h].n; r++) {
            int32_t ab[2] = { F->hdr[h].r[r].a, F->hdr[h].r[r].b >= 0 ? F->hdr[h].r[r].b : F->hdr[h].r[r].a };
            for (int q = 0; q < 2; q++) {
                keys[2 * r + q] = ab[q]; ktk[2 * r + q] = JTERM_VALUE; lty[2 * r + q] = NULL;
                const IrType *lt = NULL;
                int32_t sv = fd_len_of(f, ab[q], &lt);
                if (sv >= 0) { keys[2 * r + q] = sv; ktk[2 * r + q] = JTERM_LEN; lty[2 * r + q] = lt; }
            }
        }
        if (!F->gen->term_kinds(F->gen->ctx, f, H, keys, ktk, n2, kind, err, (uint32_t)errcap)) { free(keys); free(kind); free(ktk); free(lty); free(tm); free(cst); return false; }
        for (int r = 0; r < F->hdr[h].n; r++) {
            const FdRel *v = &F->hdr[h].r[r];
            if (!kind[2 * r] || !kind[2 * r + 1] || (kind[2 * r] != 2 && kind[2 * r + 1] != 2)) continue;
            if ((v->sa != 1 && v->sa != -1) || (v->b >= 0 && v->sb != 1 && v->sb != -1)) continue;
            int32_t ka = keys[2 * r], kb = v->b >= 0 ? keys[2 * r + 1] : -1;
            const IrType *ta = lty[2 * r], *tb = v->b >= 0 ? lty[2 * r + 1] : NULL;
            for (int32_t t = 0; t < nt; t++) { if (!ta && tm[t].key == v->a) ta = tm[t].type; if (!tb && tm[t].key == v->b) tb = tm[t].type; }
            for (int32_t k = i; k < j; k++) { if (!ta && sl[k].key == v->a) ta = sl[k].type; if (!tb && sl[k].key == v->b) tb = sl[k].type; }
            if (!ta || (v->b >= 0 && !tb)) continue;         // a term whose type the proposer cannot name
            int32_t before = *ncd;
            if (!cand_push(cd, ncd, ccd, H, ka, v->sa, kb, v->sb, ta, tb)) { free(keys); free(kind); free(ktk); free(lty); goto oom; }
            if (*ncd == before + 1) {
                (*cd)[before].fixed = true; (*cd)[before].c.rhs = v->c;
                (*cd)[before].c.tk[0] = ktk[2 * r];
                if (kb >= 0) (*cd)[before].c.tk[1] = ktk[2 * r + 1];
            }
        }
        free(keys); free(kind); free(ktk); free(lty);
    }
    // PE.7: the relations the loop keeps between its versions (and values fixed at it), each
    // trip's change bounded by the claims verified so far (`s += a[j]` rises by at most the bound
    // a verified each(a) claim gives the element)
    if (head) {
        FdLoop L;
        // the claims the trip's change needs: those of OTHER cut points (an EACH bound on the array
        // a sum reads, made by the loop that filled it) and the header's claims on versions it does
        // not make (c34's fixed ones), not the header's own invariants, which every back-edge path
        // would carry into every one of the relation search's maximizations (Gothenburg: 114 s)
        JClaim *rc = nv ? malloc(sizeof *rc * (size_t)nv) : NULL;
        int32_t nrc = 0;
        if (nv && !rc) goto oom;
        for (int32_t v = 0; v < nv; v++) {
            bool own = as[v].fn == f && as[v].blk == H;
            if (own) {                                       // fixed: every term a version H does not make
                for (int32_t k2 = i; k2 < j && own; k2++)
                    for (int32_t t = 0; t < as[v].nt && own; t++) own = !(as[v].tk[t] == JTERM_VALUE && as[v].key[t] == sl[k2].key);
                own = !own;
            }
            if (!own) rc[nrc++] = as[v];
        }
        bool built = fd_loop_build(F, f, H, rc, nrc, &L);
        free(rc);
        bool ok = !built || fd_relation_candidates(F, f, H, &L, L.npk, N, as, nv, cd, ncd, ccd);
        free(L.mem);
        if (!ok) goto oom;
    }
    free(tm); free(cst);
    return true;
oom:
    free(tm); free(inl); free(cst); snprintf(err, errcap, "out of memory");
    return false;
}
// A claim the generator refuses is refused with every claim of its function (houdini_module marks
// them all bad), so a candidate past the generator's documented limits never goes in: one to
// JCLAIM_MAXT terms, no key twice, coefficients nonzero and bound within 2^66 (gen.h's `big`).
static bool fd_claim_shape_ok(const JClaim *c) {
    const __int128 big = (__int128)1 << 66;
    if (c->nt < 1 || c->nt > JCLAIM_MAXT || c->rhs > big || c->rhs < -big) return false;
    for (int32_t i = 0; i < c->nt; i++) {
        if (c->coef[i] == 0 || c->coef[i] > big || c->coef[i] < -big) return false;
        // as gen.h: one key twice only as a cell beside its own ENTRY value (c40) or its version at
        // the enclosing loop's header (s11's HEAD), never one of those twice
        // ...and s15's POST and PRE (a var parameter's part after and before the call) are two more
        // classes, a part told apart by its fld: gen.h's jg_tk_class and jg_claim_shape. Taken for
        // the same term twice, every frame summary was filtered out here, unproposed
        for (int32_t j = i + 1; j < c->nt; j++) {
            int ci = c->tk[i] == JTERM_ENTRY ? 1 : c->tk[i] == JTERM_HEAD ? 2 : c->tk[i] == JTERM_POST ? 3 : c->tk[i] == JTERM_PRE ? 4 : 0;
            int cj = c->tk[j] == JTERM_ENTRY ? 1 : c->tk[j] == JTERM_HEAD ? 2 : c->tk[j] == JTERM_POST ? 3 : c->tk[j] == JTERM_PRE ? 4 : 0;
            bool part = c->key[i] == JKEY_RET || ci >= 3;
            if (c->key[i] == c->key[j] && ci == cj && (!part || c->fld[i] == c->fld[j])) return false;
        }
    }
    // ...and a claim of ENTRY terms only is made by no header: gen.h refuses it, and one refused
    // candidate costs every candidate of its round in the function. gen.h's made rule proper (a
    // version the header MAKES) is each source's to keep: Karr's columns read-only versions cannot
    // enter (their coefficient is forced to 0), its entry equality and fd_monotone_candidates name
    // only cells with an ENTRY pair (offered for written head versions only), and a relation's two
    // versions both change. This test is the part one line can state.
    bool made = false;
    for (int32_t i = 0; i < c->nt; i++) made |= c->tk[i] != JTERM_ENTRY && c->tk[i] != JTERM_HEAD;
    return made;
}
// Each candidate's bound, from the VCs o[0..n) of a claim set whose candidates start at `base`:
// the largest value its objective takes over its VCs (a fixed one keeps its own). What survives,
// unless a verified claim is as tight or this very claim was proposed already, goes into cl.
static bool bound_candidates(JFinder *F, const JCand *cd, int32_t ncd, const JClaimVc *o, int32_t n, int32_t base,
                             const JClaim *as, int32_t nv, JClaim **cl, int32_t *nc, int32_t *ccl) {
    // ★ The threshold test below is a SIDE QUESTION: a budget answer drops the candidate, as a model
    // does, so a query the full budget stops costs that budget for nothing. isqrt.ln's `main` asked
    // four such (its loop's inv.keep at the `bad` counter, 55 to 84 hypotheses), 110e6 operations
    // each, 450e6 of the 590e6 its search spent before its account ran out. Under a sixteenth of the
    // budget, as the finder's other side questions (a dead path's refutation, fd_loop_from).
    JWork tlim = F->lim; tlim.pivot /= 16; tlim.node /= 16; tlim.leaf /= 16;
    for (int32_t c = 0; c < ncd; c++) {
        JClaim got = cd[c].c;
        if (!cd[c].fixed) {
            __int128 best = 0;
            bool ok = true, any = false;
            for (int32_t q = 0; q < n && ok; q++) {
                if (o[q].claim != base + c) continue;
                JVc v = o[q].vc;
                fd_jint(&v.goal.rhs, cd[c].tb - 1);
                if (!fd_implied_in(F, &v, tlim)) { ok = false; break; }
                LMax x = fd_max_goal(F, &o[q].vc);
                if (x.k == LIA_MAX_EMPTY) continue;
                if (x.k != LIA_MAX_BOUND) { ok = false; break; }
                __int128 bb = fd_int128(&x.b);
                if (!any || bb > best) best = bb;
                any = true;
            }
            if (!ok || !any) continue;
            got.rhs = best;
        }
        if (!fd_claim_shape_ok(&got)) continue;
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
// here can use them in. A return that may wrap is owed about the value it lands as (gen.h, s8).
// A function returning a SLICE (s14's len(ret), the returned slice's length): each side of
// len(ret), and of its difference with each integer parameter (`prefix(s, n)` returns n bytes),
// bounded as an integer return's candidates are. `go() u8[] { return "xyz" }` then
// `s.len as i32` was E086 under the judge: no return claim could name the length, and VRA,
// which evaluates the call, had accepted it (census_gate's type-var-return cell). Built here, not
// by cand_push, which reads its trial bound from an integer type: a length's is past any bound
// the generator states (an object holds at most PTRDIFF_MAX bytes), and 0 below.
static bool ret_len_candidates(IrFunc *f, JCand **cd, int32_t *ncd, int32_t *ccd) {
    const __int128 far = (__int128)1 << 64;
    for (int32_t q = -1; q < 0 || f->params; q++) {
        IrParam *p = f->params;
        for (int32_t k = 0; q >= 0 && p && k < q; k++) p = p->next;
        if (q >= 0 && !p) break;
        if (q >= 0 && (!p->value || !fd_is_int(p->value->type))) continue;
        FdRange pr = { 0, 0, false, 0 };
        if (q >= 0 && !fd_range(p->value->type, &pr)) continue;
        for (int sr = 1; sr >= -1; sr -= 2)
            for (int sp = (q >= 0 ? -1 : 0); sp <= (q >= 0 ? 1 : 0); sp += 2) {
                if (*ncd == *ccd) {
                    int32_t nc = *ccd ? *ccd * 2 : 32;
                    JCand *x = realloc(*cd, sizeof *x * (size_t)nc);
                    if (!x) return false;
                    *cd = x; *ccd = nc;
                }
                JCand *c = &(*cd)[(*ncd)++]; memset(c, 0, sizeof *c);
                c->c.blk = JCLAIM_RET; c->c.fn = f; c->c.nt = q >= 0 ? 2 : 1;
                c->c.key[0] = JKEY_RET; c->c.tk[0] = JTERM_LEN; c->c.coef[0] = sr;
                c->tb = sr > 0 ? far : 1;
                if (q >= 0) {
                    c->c.key[1] = p->value->id; c->c.tk[1] = JTERM_VALUE; c->c.coef[1] = sp;
                    c->tb += sp > 0 ? pr.hi : -pr.lo;
                }
                if (q < 0) break;
            }
    }
    return true;
}
// FRAME SUMMARIES (s15's POST and PRE terms): per integer part of a `var` parameter, the pointee
// itself or an integer field of a struct pointee, as the generator takes them (gen.h's
// jg_var_part_ok, copied: untrusted), how far the call moves it, `post(f) - pre(f)`, both sides,
// and that move against the result, `post(f) - pre(f) +- ret` (each integer field of a returned
// struct, `ret.k`), both sides; bounded as the other return candidates are. A call `take(var q)`
// havocs q's cells unless a summary says otherwise: ring_buffer's `n + r.count = 16` was not
// inductive across `pop(var r)` (TCW's real programs, its last authority E086), where pop's
// `post(r.count) - pre(r.count) + ret.ok = 0` keeps it.
static bool fd_var_part_ok(const IrParam *pp, int16_t fld) {
    const IrType *pt = pp && pp->value ? pp->value->type : NULL;
    if (!pt || pt->kind != IRT_PTR || pt->is_raw || !pt->ptr_mut || !pt->elem) return false;
    if (fld < 0) return fld == -1 && pt->elem->kind == IRT_INT;
    return pt->elem->kind == IRT_STRUCT && fld < pt->elem->n_fields && pt->elem->fields[fld] && pt->elem->fields[fld]->kind == IRT_INT;
}
static bool fd_frame_push(JCand **cd, int32_t *ncd, int32_t *ccd, IrFunc *f, int32_t nt, const int32_t *key,
                          const uint8_t *tk, const int16_t *fld, const int *coef, __int128 tb) {
    if (*ncd == *ccd) {
        int32_t nc = *ccd ? *ccd * 2 : 32;
        JCand *x = realloc(*cd, sizeof *x * (size_t)nc);
        if (!x) return false;
        *cd = x; *ccd = nc;
    }
    JCand *c = &(*cd)[(*ncd)++]; memset(c, 0, sizeof *c);
    c->c.blk = JCLAIM_RET; c->c.fn = f; c->c.nt = nt; c->tb = tb;
    for (int32_t k = 0; k < nt; k++) { c->c.key[k] = key[k]; c->c.tk[k] = tk[k]; c->c.fld[k] = fld[k]; c->c.coef[k] = coef[k]; }
    return true;
}
// Whether a function of the module calls f from inside one of its loops: where a frame summary can
// keep a caller's loop invariant across the call (ring_buffer's pop). Called outside every loop, a
// summary helps no invariant, and its candidates cost the callee's proof: zune.ln's convert_days
// (`var t SystemTime`, four fields: 24 candidates, each bounded over its loop's VCs), which main
// calls once, took the finder 162e6 -> 226e6 operations (+40%, 10.5 -> 15.6 s; TCW).
static bool fd_called_in_loop(const JFinder *F, const IrFunc *f) {
    for (IrFunc *g = F->mod; g; g = g->next) {
        if (g->is_extern || !g->entry || !fd_same_module(g, f)) continue;
        FdNest NN; memset(&NN, 0, sizeof NN);
        if (!fd_nest_build(g, &NN)) { fd_nest_free(&NN); return true; }   // no room to ask: as before
        bool hit = false;
        for (IrBlock *b = g->blocks; b && !hit; b = b->next)
            for (IrInstr *i = b->instrs; i && !hit; i = i->next)
                hit = i->op == IR_CALL && fd_callee(F, i) == f && fd_nest_around(&NN, b->id, -1) >= 0;
        fd_nest_free(&NN);
        if (hit) return true;
    }
    return false;
}
static bool fd_frame_candidates(const JFinder *F, IrFunc *f, JCand **cd, int32_t *ncd, int32_t *ccd) {
    if (!fd_called_in_loop(F, f)) return true;
    // the result's terms: the integer returned (fld unread), or each integer field of a struct
    const IrType *rt = f->ret_type;
    int16_t rf[8]; FdRange rr[8]; int nr = 0;
    if (rt && rt->kind == IRT_STRUCT) {
        for (int16_t k = 0; k < rt->n_fields && nr < 8; k++)
            if (rt->fields[k] && fd_is_int(rt->fields[k]) && fd_range(rt->fields[k], &rr[nr])) rf[nr++] = k;
    } else if (rt && fd_range(rt, &rr[0])) { rf[0] = 0; nr = 1; }
    for (IrParam *pp = f->params; pp; pp = pp->next) {
        const IrType *pt = pp->value ? pp->value->type : NULL;
        if (!pt || pt->kind != IRT_PTR || !pt->elem) continue;
        int16_t parts[8]; int np = 0;
        if (fd_var_part_ok(pp, -1)) parts[np++] = -1;
        else if (pt->elem->kind == IRT_STRUCT)
            for (int16_t k = 0; k < pt->elem->n_fields && np < 8; k++) if (fd_var_part_ok(pp, k)) parts[np++] = k;
        for (int q = 0; q < np; q++) {
            const IrType *ft = parts[q] < 0 ? pt->elem : pt->elem->fields[parts[q]];
            FdRange fr;
            if (!fd_range(ft, &fr)) continue;
            __int128 span = fr.hi - fr.lo;
            int32_t key[3] = { pp->value->id, pp->value->id, JKEY_RET };
            uint8_t tk[3] = { JTERM_POST, JTERM_PRE, JTERM_VALUE };
            for (int sd = 1; sd >= -1; sd -= 2) {                // post - pre, each side
                int16_t fld[3] = { parts[q], parts[q], 0 };
                int coef[3] = { sd, -sd, 0 };
                if (!fd_frame_push(cd, ncd, ccd, f, 2, key, tk, fld, coef, span + 1)) return false;
                for (int r = 0; r < nr; r++)                      // ...+- the result, each side
                    for (int sr = 1; sr >= -1; sr -= 2) {
                        int16_t fld3[3] = { parts[q], parts[q], rf[r] };
                        int coef3[3] = { sd, -sd, sd * sr };
                        __int128 rspan = sd * sr > 0 ? rr[r].hi : -rr[r].lo;
                        if (!fd_frame_push(cd, ncd, ccd, f, 3, key, tk, fld3, coef3, span + rspan + 1)) return false;
                    }
            }
        }
    }
    return true;
}
static bool ret_candidates(JFinder *F, IrFunc *f, JCand **cd, int32_t *ncd, int32_t *ccd) {
    *ncd = 0;
    if (!f->incomplete && called_here(F, f) && !fd_frame_candidates(F, f, cd, ncd, ccd)) return false;
    if (f->ret_type && f->ret_type->kind == IRT_SLICE && !f->incomplete && called_here(F, f))
        return ret_len_candidates(f, cd, ncd, ccd);
    if (!fd_is_int(f->ret_type) || f->incomplete || !called_here(F, f)) return true;
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
    // PE.13 kind (4), nested recursion (NestedRecursion_1b/1c/1d): a function whose value stays in a
    // small range, r0 below a guard on a parameter and one more above it (rec1 is 0 for i <= 0, else
    // 1). A caller's measure needs both facts at once, and neither is a bound a maximization finds:
    // each holds only given the other, at the function's own calls. So they are FIXED candidates,
    // for Houdini to keep together or drop: ret <= r + d and ret >= r - d for each constant r the
    // function returns (d = 0, 1, 2), and per guard `x <= t` on an integer parameter (either branch)
    // the threshold claim K*ret - x <= K*r0 - t + K - 1, K = t - min(x) + 1, which at x <= t forces
    // ret <= r0 and at x >= t + 1 allows ret <= r0 + 1 (the Secondary Compiler Worker's
    // (2^31 + 1)*ret - x <= 2^31 for rec1)
    __int128 rk[4]; int32_t nr = 0;
    for (IrBlock *b = f->blocks; b; b = b->next) {
        if (b->term.kind != IR_TERM_RET || !b->term.cond) continue;
        IrInstr *d = NULL;
        for (IrBlock *b2 = f->blocks; b2 && !d; b2 = b2->next)
            for (IrInstr *x = b2->instrs; x && !d; x = x->next) if (x->result == b->term.cond) d = x;
        if (!d || d->op != IR_CONST) continue;
        __int128 v = fd_const(d); bool seen = false;
        for (int32_t k = 0; k < nr && !seen; k++) seen = rk[k] == v;
        if (!seen && nr < 4 && v < ((__int128)1 << 40) && v > -((__int128)1 << 40)) rk[nr++] = v;
    }
    struct { int32_t id; __int128 t, lo; } th[4]; int32_t nth = 0;
    for (IrBlock *b = f->blocks; b && nr > 0; b = b->next)
        for (IrInstr *i = b->instrs; i; i = i->next) {
            if (i->op != IR_ICMP || i->n_operands < 2 || !i->operands[0] || !i->operands[1]) continue;
            int pk = -1; IrParam *pp = NULL;                 // which side is a parameter, the other a constant
            for (int s2 = 0; s2 < 2 && pk < 0; s2++)
                for (IrParam *p = f->params; p && pk < 0; p = p->next) if (p->value == i->operands[s2]) { pk = s2; pp = p; }
            if (pk < 0 || !fd_is_int(pp->value->type)) continue;
            IrInstr *d = NULL;
            for (IrBlock *b2 = f->blocks; b2 && !d; b2 = b2->next)
                for (IrInstr *x = b2->instrs; x && !d; x = x->next) if (x->result == i->operands[1 - pk]) d = x;
            FdRange pr;
            if (!d || d->op != IR_CONST || !fd_range(pp->value->type, &pr)) continue;
            __int128 c = fd_const(d), t;
            int cmp = i->aux.cmp;                            // as `x cmp c`
            if (pk == 1) cmp = cmp == IR_CMP_SLT ? IR_CMP_SGT : cmp == IR_CMP_SLE ? IR_CMP_SGE : cmp == IR_CMP_SGT ? IR_CMP_SLT : cmp == IR_CMP_SGE ? IR_CMP_SLE : -1;
            if (cmp == IR_CMP_SLE || cmp == IR_CMP_SGT) t = c;
            else if (cmp == IR_CMP_SLT || cmp == IR_CMP_SGE) t = c - 1;
            else continue;
            bool seen = false;
            for (int32_t k = 0; k < nth && !seen; k++) seen = th[k].id == pp->value->id && th[k].t == t;
            if (!seen && nth < 4) { th[nth].id = pp->value->id; th[nth].t = t; th[nth].lo = pr.lo; nth++; }
        }
    for (int32_t k = 0; k < nr; k++) {
        for (int32_t dd = 0; dd <= 2; dd++)
            for (int sd = 1; sd >= -1; sd -= 2) {            // ret <= r + d; -ret <= -(r - d)
                if (*ncd == *ccd) { int32_t nc = *ccd ? *ccd * 2 : 32; JCand *x = realloc(*cd, sizeof *x * (size_t)nc); if (!x) return false; *cd = x; *ccd = nc; }
                JCand *c = &(*cd)[(*ncd)++]; memset(c, 0, sizeof *c);
                c->c.blk = JCLAIM_RET; c->c.nt = 1; c->c.key[0] = JKEY_RET; c->c.coef[0] = sd;
                c->c.rhs = sd > 0 ? rk[k] + dd : -(rk[k] - dd); c->tb = c->c.rhs; c->fixed = true;
            }
        for (int32_t h = 0; h < nth; h++) {
            __int128 K = th[h].t - th[h].lo + 1;
            if (K < 2 || K > ((__int128)1 << 40)) continue;
            if (*ncd == *ccd) { int32_t nc = *ccd ? *ccd * 2 : 32; JCand *x = realloc(*cd, sizeof *x * (size_t)nc); if (!x) return false; *cd = x; *ccd = nc; }
            JCand *c = &(*cd)[(*ncd)++]; memset(c, 0, sizeof *c);
            c->c.blk = JCLAIM_RET; c->c.nt = 2;
            c->c.key[0] = JKEY_RET; c->c.coef[0] = K; c->c.key[1] = th[h].id; c->c.coef[1] = -1;
            c->c.rhs = K * rk[k] - th[h].t + K - 1; c->tb = c->c.rhs; c->fixed = true;
        }
    }
    for (int32_t c = 0; c < *ncd; c++) (*cd)[c].c.fn = f;
    return true;
}
// The module's claims, in ROUNDS (each under the claims the previous one verified only): every
// function's cut-point candidates and return candidates, bounded, then Houdini over them all, since
// a return claim may need its function's loop invariants and a loop a callee's return claim.

// ── A RECURSION CYCLE OF SEVERAL FUNCTIONS, ranked jointly (spec 12; the judge's rec.*) ─────────
// The judge owes, at every call between two functions of a component, that the callee's measure at
// the arguments is at most the caller's at its entry (rec.le) or below it (rec.fall), the caller's
// at least 0 (rec.bound), with the calls not strictly lowered closing no cycle. One measure per
// member, each over its own parameters: so the search is ONE Farkas system over every call of the
// component in the program-counter encoding. A slot per (member, parameter) and an indicator per
// member; a call from member i to member j is a transition whose pre-state holds i's parameters at
// its entry in i's slots, 1 in i's indicator, 0 elsewhere, and whose post-state holds the arguments
// in j's slots, 1 in j's indicator, 0 elsewhere. A linear r over the slots is then, per member, a
// measure of its own parameters (the indicator's coefficient its constant), and r(pre) - r(post) on
// a call is exactly m_j(args) - m_i(entry) negated. The first round of the lexicographic search
// (rk_build with a delta per path, lia_maximize over their sum) gives an r that rises on no call,
// is at least 0 before every call, and falls on as many calls as it can: the calls it does not lower
// are LISTED (rec.le), and the finder checks itself that they close no cycle, since a list the judge
// refuses would refuse the function. Untrusted like everything here: the judge owes all of it.
typedef struct { int32_t mem, callee, blk, idx; } FdCall;
static bool fd_copy_str(FdLoop *L, const char *src, const char **dst) {
    size_t n = src ? strlen(src) : 0;
    char *d = fd_bump(L, n + 1);
    if (!d) return false;
    if (n) memcpy(d, src, n);
    *dst = d; return true;
}
static bool fd_copy_jcons(FdLoop *L, const JCons *src, JCons *dst) {
    *dst = *src; dst->t = fd_bump(L, sizeof(JTerm) * (size_t)(src->nt ? src->nt : 1));
    if (!dst->t) return false;
    if (src->nt) memcpy(dst->t, src->t, sizeof(JTerm) * (size_t)src->nt);
    return true;
}
// `src`, out of the generator's region, with room for `xv` more variables and `xh` more hypotheses
static bool fd_copy_vc(FdLoop *L, const JVc *src, int32_t xv, int32_t xh, JVc *dst) {
    memset(dst, 0, sizeof *dst);
    dst->id.fn = "cycle"; dst->id.irhash = "0"; dst->id.point = "call"; dst->id.kind = "call"; dst->id.ordinal = "0";
    dst->vars = fd_bump(L, sizeof(char *) * (size_t)(src->nvars + xv + 1));
    dst->hyps = fd_bump(L, sizeof(JHyp) * (size_t)(src->nhyps + xh + 1));
    if (!dst->vars || !dst->hyps) return false;
    for (int32_t v = 0; v < src->nvars; v++) if (!fd_copy_str(L, src->vars[v], &dst->vars[v])) return false;
    dst->nvars = src->nvars;
    for (int32_t h = 0; h < src->nhyps; h++) {
        const JHyp *a = &src->hyps[h]; JHyp *b = &dst->hyps[h];
        b->disj = a->disj; b->origin = "path";
        if (!a->disj) { if (!fd_copy_jcons(L, &a->cons, &b->cons)) return false; continue; }
        b->ndis = a->ndis; b->dis = fd_bump(L, sizeof(JConj) * (size_t)(a->ndis ? a->ndis : 1));
        if (!b->dis) return false;
        for (int32_t d = 0; d < a->ndis; d++) {
            b->dis[d].nc = a->dis[d].nc; b->dis[d].c = fd_bump(L, sizeof(JCons) * (size_t)(a->dis[d].nc ? a->dis[d].nc : 1));
            if (!b->dis[d].c) return false;
            for (int32_t c = 0; c < a->dis[d].nc; c++) if (!fd_copy_jcons(L, &a->dis[d].c[c], &b->dis[d].c[c])) return false;
        }
    }
    dst->nhyps = src->nhyps;
    dst->goal.op = J_LE; dst->goal.nt = 0; fd_jint(&dst->goal.rhs, 0);
    return true;
}
// a fresh variable `name` of vc, equal to variable `y` (y >= 0) or to the constant k (y < 0)
static int32_t fd_fresh_eq(FdLoop *L, JVc *vc, const char *name, int32_t y, int64_t k) {
    int32_t x = vc->nvars;
    if (!fd_copy_str(L, name, &vc->vars[x])) return -1;
    vc->nvars++;
    JHyp *h = &vc->hyps[vc->nhyps++];
    memset(h, 0, sizeof *h); h->origin = "slot"; h->cons.op = J_EQ;
    h->cons.t = fd_bump(L, sizeof(JTerm) * 2);
    if (!h->cons.t) return -1;
    if (y >= 0) {                                    // x - y = 0, ascending: y < x always (x is the newest)
        h->cons.t[0].var = y; fd_jint(&h->cons.t[0].coef, -1);
        h->cons.t[1].var = x; fd_jint(&h->cons.t[1].coef, 1);
        h->cons.nt = 2; fd_jint(&h->cons.rhs, 0);
    } else {
        h->cons.t[0].var = x; fd_jint(&h->cons.t[0].coef, 1);
        h->cons.nt = 1; fd_jint(&h->cons.rhs, k);
    }
    return x;
}
// the parameter slots of member i: its integer parameters (JTERM_VALUE) and slices (JTERM_LEN), as
// the generator's call paths name them
static int32_t fd_cycle_slots(IrFunc *f, int32_t *key, uint8_t *tk, int32_t cap) {
    int32_t n = 0;
    for (IrParam *p = f->params; p; p = p->next) {
        if (!p->value || !p->value->type) continue;
        uint8_t t = p->value->type->kind == IRT_INT ? JTERM_VALUE : p->value->type->kind == IRT_SLICE ? JTERM_LEN : 2;
        if (t > JTERM_LEN) continue;
        if (n == cap) return -1;
        key[n] = p->value->id; tk[n] = t; n++;
    }
    return n;
}
#define FD_CYC_MAXM 64                               // the finder's own room, not a rule of the language
#define FD_CYC_MAXS 192
static bool fd_rank_cycle(JFinder *F, IrFunc **mem, int32_t m, const JClaim *cl, int32_t ncl, JRank *out) {
    if (m < 2 || m > FD_CYC_MAXM || !F->gen->call_paths) return false;
    FdLoop Lb, *L = &Lb; memset(L, 0, sizeof *L);
    size_t cap = (size_t)1 << 23;
    if (!(L->mem = malloc(cap))) { F->oom = true; return false; }
    L->cur = L->mem; L->end = L->mem + cap;
    bool ok = false;
    char err[200];
    // the slots: member i's parameters at sbase[i] .. sbase[i] + ns[i], its indicator at nslot + i
    int32_t skey[FD_CYC_MAXS]; uint8_t stk[FD_CYC_MAXS]; int32_t sbase[FD_CYC_MAXM], ns[FD_CYC_MAXM], nslot = 0;
    for (int32_t i = 0; i < m; i++) {
        sbase[i] = nslot;
        ns[i] = fd_cycle_slots(mem[i], skey + nslot, stk + nslot, FD_CYC_MAXS - nslot);
        if (ns[i] < 0) goto out;
        nslot += ns[i];
    }
    int32_t NP = nslot + m;
    JPath *sp = fd_bump(L, sizeof(JPath) * FD_MAXEDGES);
    FdCall *calls = fd_bump(L, sizeof(FdCall) * FD_MAXEDGES);
    if (!sp || !calls) goto out;
    int32_t nc = 0;
    for (int32_t i = 0; i < m; i++) {
        JPaths P;
        if (!F->gen->call_paths(F->gen->ctx, mem[i], cl, ncl, &P, err, sizeof err) || P.refused) goto out;
        for (int32_t k = 0; k < P.n; k++) {
            const JPath *pp = &P.p[k];
            int32_t j = 0;
            while (j < m && mem[j] != pp->callee) j++;
            if (j == m || nc == FD_MAXEDGES) goto out;          // a callee outside the list: not this component
            JVc *vc = fd_bump(L, sizeof *vc);
            JPathPair *pr = fd_bump(L, sizeof(JPathPair) * (size_t)NP);
            if (!vc || !pr || !fd_copy_vc(L, pp->vc, 2 * NP, 2 * NP, vc)) goto out;
            for (int32_t q = 0; q < NP; q++) {
                char nm[32];
                // pre: i's slot from i's entry, i's indicator 1, every other 0
                int32_t y = -1; int64_t kk = 0;
                if (q < nslot && q >= sbase[i] && q < sbase[i] + ns[i]) {
                    for (int32_t z = 0; z < pp->nentry && y < 0; z++)
                        if (pp->entry[z].key == skey[q] && pp->entry[z].tk == stk[q]) y = pp->entry[z].pre;
                    if (y < 0) goto out;
                } else if (q == nslot + i) kk = 1;
                snprintf(nm, sizeof nm, "slot%d.pre", (int)q);
                int32_t vpre = fd_fresh_eq(L, vc, nm, y, kk);
                // post: j's slot from the argument, j's indicator 1, every other 0
                y = -1; kk = 0;
                if (q < nslot && q >= sbase[j] && q < sbase[j] + ns[j]) {
                    for (int32_t z = 0; z < pp->npairs && y < 0; z++)
                        if (pp->pairs[z].key == skey[q] && pp->pairs[z].tk == stk[q]) y = pp->pairs[z].post;
                    if (y < 0) goto out;
                } else if (q == nslot + j) kk = 1;
                snprintf(nm, sizeof nm, "slot%d.post", (int)q);
                int32_t vpost = fd_fresh_eq(L, vc, nm, y, kk);
                if (vpre < 0 || vpost < 0) goto out;
                pr[q].key = q; pr[q].tk = JTERM_VALUE; pr[q].pre = vpre; pr[q].post = vpost;
            }
            sp[nc].vc = vc; sp[nc].back = true; sp[nc].from = nc; sp[nc].pairs = pr; sp[nc].npairs = NP;
            sp[nc].at = -1; sp[nc].callee = NULL; sp[nc].entry = NULL; sp[nc].nentry = 0;
            calls[nc].mem = i; calls[nc].callee = j; calls[nc].blk = pp->from; calls[nc].idx = pp->at;
            nc++;
        }
    }
    if (nc == 0) goto out;
    // a WRITTEN member's coefficients are its measure's, as the generator reads it (JudgeGen.measure):
    // fixed below, so the search chooses only which calls fall. The generator owes the calls against
    // the written measure and reads only this proposal's `le` list; without one it owes rec.fall at
    // every call, and `a -> b(n) -> c(n) -> a(n - 1)`, each `decreasing n`, was refused at a -> b
    // where spec 12 asks only that n not rise.
    bool wr[FD_CYC_MAXM]; __int128 wc[FD_CYC_MAXS], w0[FD_CYC_MAXM]; int32_t nw = 0;
    for (int32_t i = 0; i < m; i++) {
        JRank W;
        wr[i] = F->gen->measure && F->gen->measure(F->gen->ctx, mem[i], &W, err, sizeof err);
        if (!wr[i]) continue;
        int32_t used = 0;
        for (int32_t q = sbase[i]; q < sbase[i] + ns[i]; q++) {
            wc[q] = 0;
            for (int32_t t = 0; t < W.nt; t++) if (W.key[t] == skey[q] && W.tk[t] == stk[q]) { wc[q] = W.coef[0][t]; used++; }
        }
        if (used != W.nt) goto out;                           // a term no slot holds: nothing to fix it to
        w0[i] = W.c0[0]; nw++;
    }
    {   JPaths SP = { sp, nc, NULL };
        if (!fd_loop_from(F, mem[0], JRANK_ENTRY, &SP, L, FD_MAXEDGES)) goto out; }
    {   char *cur = (char *)F->work, *end = (char *)F->work + F->worksz / 4;
        if (L->npairs != NP) goto out;
        // per slot and indicator its coefficient, cf[NP] the constant; and which calls fall
        __int128 cf[FD_CYC_MAXS + FD_CYC_MAXM + 1];
        bool strict[FD_MAXEDGES];
        for (int32_t c = 0; c < nc; c++) strict[c] = true;
        if (nw == m) {
            // every member WRITTEN: nothing to search. A call falls when each flattened path of it
            // does under the written measures, the generator's rec.fall: sum W_q (post_q - pre_q) <= -1,
            // an indicator's W its member's constant. One small VC per path, so the cycle's length
            // costs no LP (a search over 17 members stops on its capacity)
            for (int32_t q = 0; q < nslot; q++) cf[q] = wc[q];
            for (int32_t i = 0; i < m; i++) cf[nslot + i] = w0[i];
            cf[NP] = 0;
            JTerm *gt = (JTerm *)rk_bump(&cur, end, sizeof(JTerm) * (size_t)(2 * NP));
            int32_t *gv = (int32_t *)rk_bump(&cur, end, sizeof(int32_t) * (size_t)(2 * NP));
            __int128 *gc = (__int128 *)rk_bump(&cur, end, sizeof(__int128) * (size_t)(2 * NP));
            if (!gt || !gv || !gc) goto out;
            // off the module's meter (FD_WORK): the cycles come last, and a meter spent on other
            // functions would make every call here "not falling" and refuse a written measure the
            // rule accepts, with no word of a budget. One query per call, each under F->lim as the
            // judge's own are: what this costs is bounded by the calls written
            bool mon = ls_meter_on;
            ls_meter_on = false;
            for (int32_t k = 0; k < L->npaths; k++) {
                int32_t n = 0;
                for (int32_t q = 0; q < NP; q++) {          // merged by variable, ascending, zeros dropped
                    if (cf[q] == 0) continue;
                    for (int s2 = 0; s2 < 2; s2++) {
                        int32_t v = s2 ? L->pre[q] : L->post[q]; __int128 c = s2 ? -cf[q] : cf[q];
                        int32_t z = 0;
                        while (z < n && gv[z] < v) z++;
                        if (z < n && gv[z] == v) { gc[z] += c; continue; }
                        for (int32_t y = n; y > z; y--) { gv[y] = gv[y - 1]; gc[y] = gc[y - 1]; }
                        gv[z] = v; gc[z] = c; n++;
                    }
                }
                int32_t nt = 0;
                for (int32_t z = 0; z < n; z++) if (gc[z]) { gt[nt].var = gv[z]; fd_jint(&gt[nt].coef, gc[z]); nt++; }
                JVc q = *L->paths[k];
                q.goal.t = gt; q.goal.nt = nt; q.goal.op = J_LE; fd_jint(&q.goal.rhs, -1);
                if (lia_solve(&q, F->lim, cur, (size_t)((char *)F->work + F->worksz - cur)).k != J_ANS_CERT) strict[L->edge[k]] = false;
            }
            ls_meter_on = mon;
        } else {
            // the first round of the lexicographic search: rises on no call, >= 0 before each, falls
            // on most. When it finds none, each call in turn (`only`) is the only one that may fall,
            // bounded there, and the others owe only that the measure not rise, with no bound (spec
            // 12's cut: such a call is listed, rec.le). calc.ln's primary -> expr -> term -> primary
            // raises `depth` once a round, at primary's call `expr(var p, depth + 1)` under `depth <=
            // 64`, and passes it on unchanged at the others, where nothing bounds it: no measure was
            // bounded before every call, and 65 - depth falls at the one call and rises at none
            // (TCW's real_programs, B3). Whether the unlowered calls close a cycle is checked below.
            bool on[FD_MAXFLAT];
            for (int32_t k = 0; k < L->npaths; k++) on[k] = true;
            uint8_t unb[FD_MAXFLAT];
            char *mark = cur;
            bool found = false;
            for (int32_t only = -1; only < nc && only < 16 && !found; only++) {
                cur = mark;
                for (int32_t k = 0; k < L->npaths; k++) unb[k] = only >= 0 && L->edge[k] != only;
                int32_t *dl = (int32_t *)rk_bump(&cur, end, sizeof(int32_t) * (size_t)L->npaths);
                JVc lp;
                rk_unbounded = only >= 0 ? unb : NULL;
                bool built = dl && rk_build(L->paths, L->npaths, on, L->pre, L->post, L->npairs, true, 0, &cur, end, &lp, dl);
                rk_unbounded = NULL;
                if (!built) goto out;
                if (nw) {                                        // the written members' rows, fixed: c_q = w_q,
                    JHyp *hx = (JHyp *)rk_bump(&cur, end, sizeof(JHyp) * (size_t)(lp.nhyps + NP));   // and
                    JTerm *tx = (JTerm *)rk_bump(&cur, end, sizeof(JTerm) * (size_t)(2 * NP));     // indicator
                    if (!hx || !tx) goto out;                                                      // + c0 = w0
                    memcpy(hx, lp.hyps, sizeof(JHyp) * (size_t)lp.nhyps);
                    int32_t nh = lp.nhyps, ntx = 0;
                    for (int32_t i = 0; i < m; i++) {
                        if (!wr[i]) continue;
                        for (int32_t q = sbase[i]; q <= sbase[i] + ns[i]; q++) {
                            bool ind = q == sbase[i] + ns[i];      // past the slots: the indicator's row
                            JHyp *h = &hx[nh++]; memset(h, 0, sizeof *h);
                            h->origin = "written measure"; h->cons.op = J_EQ; h->cons.t = &tx[ntx];
                            tx[ntx].var = ind ? nslot + i : q; fd_jint(&tx[ntx].coef, 1); ntx++; h->cons.nt = 1;
                            if (ind) { tx[ntx].var = L->npairs; fd_jint(&tx[ntx].coef, 1); ntx++; h->cons.nt = 2; }
                            fd_jint(&h->cons.rhs, ind ? w0[i] : wc[q]);
                        }
                    }
                    lp.hyps = hx; lp.nhyps = nh;
                }
                JTerm *obj = (JTerm *)rk_bump(&cur, end, sizeof(JTerm) * (size_t)L->npaths);
                if (!obj) goto out;
                int32_t nt = 0;
                for (int32_t k = 0; k < L->npaths; k++) if (dl[k] >= 0) { obj[nt].var = dl[k]; fd_jint(&obj[nt].coef, 1); nt++; }
                LMax X = lia_maximize(&lp, obj, nt, F->lim, cur, (size_t)((char *)F->work + F->worksz - cur));
                if (X.k != LIA_MAX_BOUND || X.b.n == 0) continue;
                // a call is strict when every flattened path of it falls
                for (int32_t c = 0; c < nc; c++) strict[c] = true;
                for (int32_t k = 0; k < L->npaths; k++) if (X.model.val[dl[k]].n == 0) strict[L->edge[k]] = false;
                for (int32_t q = 0; q <= NP; q++) if (!fd_i128(&X.model.val[q], &cf[q])) goto out;
                found = true;
            }
            if (!found) goto out;
        }
        // the calls left unlowered must close no cycle among the members (a self-call is one)
        int32_t indeg[FD_CYC_MAXM]; bool gone[FD_CYC_MAXM];
        for (int32_t i = 0; i < m; i++) { indeg[i] = 0; gone[i] = false; }
        for (int32_t c = 0; c < nc; c++) if (!strict[c]) indeg[calls[c].callee]++;
        for (int32_t round = 0; round < m; round++) {       // Kahn: remove members no listed call enters
            int32_t pick = -1;
            for (int32_t i = 0; i < m && pick < 0; i++) if (!gone[i] && indeg[i] == 0) pick = i;
            if (pick < 0) break;
            gone[pick] = true;
            for (int32_t c = 0; c < nc; c++) if (!strict[c] && calls[c].mem == pick) indeg[calls[c].callee]--;
        }
        for (int32_t i = 0; i < m; i++) if (!gone[i]) goto out;      // the listed calls close a cycle
        // per member: its slots' coefficients, its indicator's plus the constant, its listed calls
        for (int32_t i = 0; i < m; i++) {
            JRank *r = &out[i]; memset(r, 0, sizeof *r);
            r->blk = JRANK_ENTRY; r->fn = mem[i]; r->ncomp = 1;
            r->c0[0] = cf[nslot + i] + cf[NP];
            for (int32_t q = sbase[i]; q < sbase[i] + ns[i]; q++) {
                if (cf[q] == 0) continue;
                if (r->nt == JRANK_MAXT) goto out;
                r->key[r->nt] = skey[q]; r->tk[r->nt] = stk[q]; r->coef[0][r->nt] = cf[q]; r->nt++;
            }
            for (int32_t c = 0; c < nc; c++) if (!strict[c] && calls[c].mem == i) {
                if (r->nle == JRANK_MAXL) goto out;
                r->le_blk[r->nle] = calls[c].blk; r->le_idx[r->nle] = calls[c].idx; r->nle++;
            }
        }
        ok = true;
    }
out:
    free(L->mem);
    return ok;
}

// PE.14: an accumulator of a nested loop. Inside, `ct*s + cj*j` does not rise (lia_relations, j
// the inner counter, rising: cj < 0) and j stays in [E, U] at the inner header (verified claims),
// so one run of the inner loop raises ct*s by at most |cj|*(U - E). An outer counter o, unchanged
// inside and stepping by d (one sign) per outer trip, then bounds it: `ct*s - c*o <= K1` at the
// outer header, c = ceil(|cj|*(U - E) / |d|), K1 that sum's greatest value on the outer loop's
// entry; and `ct*s + cj*j - c*o <= K1 + cj*E` at the inner header. Neither holds alone (each is the
// other's back-edge hypothesis), so both are proposed with fixed bounds and Houdini keeps them
// only together. Nothing here is believed: a wrong guess (s also changed outside the inner loop,
// the inner loop run twice per trip) is a pair of refused candidates.
static bool fd_tight1(const JClaim *as, int32_t nv, const IrFunc *f, int32_t H, int32_t key, int sgn, __int128 *b) {
    bool any = false;
    for (int32_t v = 0; v < nv; v++) {
        const JClaim *c = &as[v];
        if (c->fn != f || c->blk != H || c->nt != 1 || c->key[0] != key || c->tk[0] != JTERM_VALUE || c->coef[0] != sgn) continue;
        if (!any || c->rhs < *b) *b = c->rhs;
        any = true;
    }
    return any;
}
// The greatest value of claim `cand`'s sum on the edges entering its loop (inv.entry), under the
// verified claims; false when it has none or one is unbounded.
static bool fd_entry_max(JFinder *F, IrFunc *f, const JClaim *as, int32_t nv, const JClaim *cand, __int128 *out) {
    char err[200];
    JClaim *q = malloc(sizeof *q * (size_t)(nv + 1));
    if (!q) { F->oom = true; return false; }
    if (nv) memcpy(q, as, sizeof *q * (size_t)nv);
    q[nv] = *cand;
    JClaimVc *o; int32_t n;
    bool ok = F->gen->claim_vcs(F->gen->ctx, f, q, nv + 1, F->hgc, F->nhgc, cand->blk, &o, &n, err, sizeof err);
    free(q);
    if (!ok) return false;
    bool any = false; __int128 best = 0;
    for (int32_t k = 0; k < n; k++) {
        if (o[k].claim != nv || !o[k].vc.id.kind || strcmp(o[k].vc.id.kind, "inv.entry") != 0) continue;
        LMax x = fd_max_goal(F, &o[k].vc);
        if (x.k == LIA_MAX_EMPTY) continue;
        if (x.k != LIA_MAX_BOUND) return false;
        __int128 b = fd_int128(&x.b);
        if (!any || b > best) best = b;
        any = true;
    }
    if (any) *out = best;
    return any;
}
static bool fd_push_claim(JFinder *F, const JClaim *got, const JClaim *as, int32_t nv, JClaim **cl, int32_t *nc, int32_t *ccl) {
    if (!fd_claim_shape_ok(got)) return true;
    // as bound_candidates: a verified claim as tight covers it, a candidate only when the same (a
    // tighter candidate may fail: `y >= 2` from y's entry value pushed before `y >= 1` kept the one
    // Houdini proves from ever being proposed, HeizmannHoenickeLeikePodelski-ATVA2013-Fig5)
    for (int32_t v = 0; v < nv; v++) if (same_form(&as[v], got) && as[v].fn == got->fn && as[v].rhs <= got->rhs) return true;
    for (int32_t v = nv; v < *nc; v++) if (same_form(&(*cl)[v], got) && (*cl)[v].fn == got->fn && (*cl)[v].rhs == got->rhs) return true;
    if (*nc == *ccl) {
        int32_t nn = *ccl ? *ccl * 2 : 64;
        JClaim *x = realloc(*cl, sizeof *x * (size_t)nn);
        if (!x) return false;
        *cl = x; *ccl = nn;
    }
    (*cl)[(*nc)++] = *got;
    F->proposed++;
    return true;
}
static bool fd_nested_candidates(JFinder *F, IrFunc *f, const FdNest *N, const JClaim *as, int32_t nv, JClaim **cl, int32_t *nc, int32_t *ccl) {
    for (IrBlock *h2 = f->blocks; h2; h2 = h2->next) {
        if (!h2->is_loop_header) continue;
        int32_t H2 = h2->id, H1 = fd_nest_around(N, H2, H2);   // the innermost loop around H2's
        if (H1 < 0) continue;
        FdLoop L2, L1;
        bool b2 = fd_loop_build(F, f, H2, as, nv, &L2);
        LRel rel[32];
        int32_t nr = b2 ? lia_relations(L2.paths, L2.npaths, L2.pre, L2.post, L2.npairs, F->lim, F->work, F->worksz, rel, 32) : 0;
        bool c1 = nr > 0, b1 = c1 && fd_loop_build(F, f, H1, as, nv, &L1);
        // each outer counter's step on the outer loop's paths: the same for every relation, so asked
        // once (asked per relation, 14 relations times every counter and path cost 94e6 of a
        // 37-line fuzz_window program's 383e6, and the judge's compile a minute)
        int8_t *ost = b1 ? calloc((size_t)(L1.npk ? L1.npk : 1), 1) : NULL;    // 0 unknown, 1 one step, 2 not
        int64_t *ostep = b1 ? calloc((size_t)(L1.npk ? L1.npk : 1), sizeof *ostep) : NULL;
        if (b1 && (!ost || !ostep)) { free(ost); free(ostep); goto fail; }
        for (int32_t q = 0; b1 && q < nr; q++) {
            const LRel *r = &rel[q];
            if (r->t >= L2.npk || r->j >= L2.npk || L2.ptk[r->t] != JTERM_VALUE || L2.ptk[r->j] != JTERM_VALUE || r->cj >= 0) continue;
            int32_t jk = L2.pkey[r->j], sk = L2.pkey[r->t];
            __int128 U, Eb;
            if (!fd_tight1(as, nv, f, H2, jk, 1, &U) || !fd_tight1(as, nv, f, H2, jk, -1, &Eb)) continue;
            __int128 E = -Eb, T = U - E;
            if (T <= 0 || T > ((__int128)1 << 20)) continue;
            for (int32_t o = 0; o < L1.npk; o++) {                 // an outer counter: one step a trip
                int32_t okey = L1.pkey[o];                          // (one that also moves inside has no
                if (okey == sk || okey == jk || L1.ptk[o] != JTERM_VALUE) continue;   // such step, as a rule)
                if (!ost[o]) {
                    int64_t d0 = 0; bool cst = L1.npaths > 0;
                    for (int32_t k = 0; k < L1.npaths && cst; k++) {
                        int64_t lo, hi;
                        if (!rk_delta(L1.paths[k], L1.pre[o], L1.post[o], F->lim, F->work, F->worksz, &lo, &hi) || lo != hi || lo == 0 ||
                            (d0 && (lo > 0) != (d0 > 0))) cst = false;
                        else if (!d0 || (lo > 0 ? lo : -lo) < (d0 > 0 ? d0 : -d0)) d0 = lo;
                    }
                    ost[o] = cst ? 1 : 2; ostep[o] = d0;
                }
                if (ost[o] != 1) continue;
                int64_t d = ostep[o];
                __int128 ad = d > 0 ? d : -d, rise = (__int128)(-r->cj) * T;
                __int128 c = rise / ad + (rise % ad != 0);
                if (c <= 0 || c > ((__int128)1 << 40)) continue;
                JClaim out1; memset(&out1, 0, sizeof out1);
                out1.blk = H1; out1.fn = f; out1.nt = 2;
                out1.key[0] = sk; out1.tk[0] = JTERM_VALUE; out1.coef[0] = r->ct;
                out1.key[1] = okey; out1.tk[1] = JTERM_VALUE; out1.coef[1] = d > 0 ? -c : c;
                __int128 K1;
                if (!fd_entry_max(F, f, as, nv, &out1, &K1)) { if (F->oom) { free(ost); free(ostep); goto fail; } continue; }
                out1.rhs = K1;
                JClaim in2; memset(&in2, 0, sizeof in2);
                in2.blk = H2; in2.fn = f; in2.nt = 3;
                in2.key[0] = sk; in2.tk[0] = JTERM_VALUE; in2.coef[0] = r->ct;
                in2.key[1] = jk; in2.tk[1] = JTERM_VALUE; in2.coef[1] = r->cj;
                in2.key[2] = okey; in2.tk[2] = JTERM_VALUE; in2.coef[2] = out1.coef[1];
                in2.rhs = K1 + (__int128)r->cj * E;
                if (!fd_push_claim(F, &out1, as, nv, cl, nc, ccl) || !fd_push_claim(F, &in2, as, nv, cl, nc, ccl)) { free(ost); free(ostep); goto fail; }
            }
        }
        free(ost); free(ostep);
        if (c1) free(L1.mem);
        free(L2.mem);
        continue;
    fail:
        if (c1) free(L1.mem);
        free(L2.mem);
        return false;
    }
    return true;
}
// PE.14 with JTERM_EACH: every element of a small fixed array the loop writes, bounded at its
// header. Maximizing cannot bound it (an element the trip leaves alone is bounded on the back edge
// only by the claim itself), so the bound is iterated: from its greatest value on the loop's entry,
// raised to its greatest value after a trip under the bound so far, until it holds (at most four
// raises; then none). `a[i] = 10` from zeros: 0, then 10, which a trip keeps. Fixed, so Houdini
// checks it as any other.
static bool fd_iter_bound(JFinder *F, IrFunc *f, const JClaim *as, int32_t nv, JClaim *cand) {
    char err[200];
    JClaim *q = malloc(sizeof *q * (size_t)(nv + 1));
    if (!q) { F->oom = true; return false; }
    if (nv) memcpy(q, as, sizeof *q * (size_t)nv);
    const __int128 big = (__int128)1 << 66;
    // the bound so far: none until an edge gives one (a guarded claim's instances are all off on
    // the loop's entry, `i <= k` with i = 0, so only a trip bounds it)
    __int128 B = -big; bool have = false, ok = false;
    for (int it = 0; it < 6 && !ok; it++) {
        cand->rhs = have ? B : -big; q[nv] = *cand;
        JClaimVc *o; int32_t n;
        if (!F->gen->claim_vcs(F->gen->ctx, f, q, nv + 1, F->hgc, F->nhgc, cand->blk, &o, &n, err, sizeof err)) break;
        __int128 M = 0; bool any = false, unb = false;
        for (int32_t k = 0; k < n && !unb; k++) {
            if (o[k].claim != nv || !o[k].vc.id.kind) continue;
            bool entry = !strcmp(o[k].vc.id.kind, "inv.entry");
            if (!entry && strcmp(o[k].vc.id.kind, "inv.keep") != 0) continue;
            LMax x = fd_max_goal(F, &o[k].vc);
            if (x.k == LIA_MAX_EMPTY) continue;
            if (x.k != LIA_MAX_BOUND) { unb = true; break; }
            __int128 b = fd_int128(&x.b);
            if (!any || b > M) M = b;
            any = true;
        }
        if (unb || !any) break;                              // unbounded, or no edge says anything
        if (have && M <= B) ok = true;
        else { B = have && B > M ? B : M; have = true; }
        if (B > big || B < -big) break;
    }
    free(q);
    if (ok) cand->rhs = B;
    return ok;
}
// The cells whose value indexes a store into array `akey` inside a loop's body: a store
// through an ELEM_PTR on the array, its index a cell's load, through casts and the addition or
// subtraction of a constant (a comprehension writes `a[x - 0]`). The guards an UPTO claim is
// worth trying: the counter that fills the array (every integer version the header makes, as a
// guard, cost the SIMD lexer's `tokenize` every claim it had).
static int32_t fd_store_guards(IrFunc *f, const uint8_t *body, int32_t nbk, int32_t akey, int32_t *g, int32_t cap) {
    int32_t n = 0;
    for (IrBlock *b = f->blocks; b; b = b->next) {
        if (!body || b->id < 0 || b->id >= nbk || !body[b->id]) continue;
        for (IrInstr *i = b->instrs; i; i = i->next) {
            if (i->op != IR_STORE || i->n_operands < 1 || !i->operands[0]) continue;
            IrInstr *ep = NULL;
            for (IrInstr *x = b->instrs; x && x != i; x = x->next) if (x->result == i->operands[0]) ep = x;
            if (!ep || ep->op != IR_ELEM_PTR || ep->n_operands < 2 || !ep->operands[0] || ep->operands[0]->id != akey) continue;
            IrValue *v = ep->operands[1];
            for (int hop = 0; v && hop < 8; hop++) {
                IrInstr *d = NULL;
                for (IrInstr *x = b->instrs; x && x != i; x = x->next) if (x->result == v) d = x;
                if (!d) break;
                if (d->op == IR_LOAD && d->n_operands >= 1 && d->operands[0]) {
                    int32_t key = d->operands[0]->id; bool seen = false;
                    for (int32_t q = 0; q < n && !seen; q++) seen = g[q] == key;
                    if (!seen && n < cap) g[n++] = key;
                    break;
                }
                if (d->op == IR_CAST && d->n_operands >= 1) { v = d->operands[0]; continue; }
                if ((d->op == IR_ADD || d->op == IR_SUB) && d->n_operands >= 2) { v = d->operands[0]; continue; }
                break;
            }
        }
    }
    return n;
}
// PE.13 (3): a loop's affine EQUALITIES (Karr). On each back-edge path the definitions are
// equations; row-reduced with the path's own variables first, each header version's new value is
// an affine function of the old ones (or undetermined: a nondeterministic value, a load). A vector
// c over the versions with c.(x' - x) = 0 for every old x on every path keeps c.x fixed; such c
// are the nullspace of one linear system, and each basis vector of at most JCLAIM_MAXT terms is
// proposed as two fixed claims, c.x between its least and greatest value on the loop's entry.
// MenloPark keeps 2y - z = 199 (y -= z, z = -z) and with it y >= 99, which its ranking needs. The
// arithmetic is exact (fraction-free, gcd-reduced); past 2^60 the loop gets no candidate.
#define FD_KARR_BIG ((__int128)1 << 60)
static __int128 fd_gcd128(__int128 a, __int128 b) { if (a < 0) a = -a; if (b < 0) b = -b; while (b) { __int128 t = a % b; a = b; b = t; } return a; }
static bool fd_row_norm(__int128 *r, int32_t w) {
    __int128 g = 0;
    for (int32_t x = 0; x < w; x++) g = fd_gcd128(g, r[x]);
    if (g > 1) for (int32_t x = 0; x < w; x++) r[x] /= g;
    for (int32_t x = 0; x < w; x++) if (r[x] > FD_KARR_BIG || r[x] < -FD_KARR_BIG) return false;
    return true;
}
// Row-reduce A (nr x w, the last column the constant) over columns in the order `ord` (nord of
// them); piv[col] = its pivot row or -1. False on overflow.
static bool fd_rref(__int128 *A, int32_t nr, int32_t w, const int32_t *ord, int32_t nord, int32_t *piv) {
    int32_t used = 0;
    for (int32_t x = 0; x < w; x++) piv[x] = -1;
    for (int32_t oi = 0; oi < nord && used < nr; oi++) {
        int32_t col = ord[oi], p = -1;
        for (int32_t r = used; r < nr && p < 0; r++) if (A[(size_t)r * w + col] != 0) p = r;
        if (p < 0) continue;
        if (p != used) for (int32_t x = 0; x < w; x++) { __int128 t = A[(size_t)p * w + x]; A[(size_t)p * w + x] = A[(size_t)used * w + x]; A[(size_t)used * w + x] = t; }
        __int128 *P = A + (size_t)used * w;
        for (int32_t r = 0; r < nr; r++) {
            if (r == used || A[(size_t)r * w + col] == 0) continue;
            __int128 *R = A + (size_t)r * w, a = P[col], b = R[col];
            for (int32_t x = 0; x < w; x++) {
                if ((P[x] && (b > FD_KARR_BIG || b < -FD_KARR_BIG)) || (R[x] && (a > FD_KARR_BIG || a < -FD_KARR_BIG))) return false;
                R[x] = a * R[x] - b * P[x];
            }
            if (!fd_row_norm(R, w)) return false;
        }
        piv[col] = used++;
    }
    return true;
}
static bool fd_karr_candidates(JFinder *F, IrFunc *f, int32_t H, const JClaim *as, int32_t nv, JClaim **cl, int32_t *nc, int32_t *ccl) {
    FdLoop L;
    if (!fd_loop_build(F, f, H, as, nv, &L)) { free(L.mem); return !F->oom; }
    // the columns: the header's versions, its ENTRY pairs aside (c40: fixed in the loop, each would
    // be a free column of its own, and a claim of it alone is refused); col[q] is column q's pair
    int32_t col[16], K = 0, W = L.nv + 1;
    for (int32_t q = 0; q < L.npk && K <= 16; q++) if (L.ptk[q] != JTERM_ENTRY) { if (K < 16) col[K] = q; K++; }
    bool ok = K >= 2 && K <= 16 && L.npaths >= 1 && L.nv <= 512;
    // the system on c: one row per (path, old variable) and per path its constant, plus c_q = 0 for
    // a version some path leaves undetermined; at most 32 paths
    int32_t cap = ok ? (L.npaths * (L.nv + 1) + K) : 0;
    __int128 *C = ok ? calloc((size_t)cap * (size_t)(K + 1), sizeof *C) : NULL;
    int32_t ncr = 0;
    ok = ok && C;
    // a path on which a wrapping operation took a case past its type (`y -% z` as y - z - 2^32)
    // breaks every equality the in-range cases keep (MenloPark's 2y - z = 199 under `-%`), and the
    // equality is what rules those cases out: so they are left out where some path stays in range.
    // Houdini checks the candidate on every path.
    bool plain = false;
    for (int32_t k = 0; k < L.npaths; k++) plain |= !L.wrapped || !L.wrapped[k];
    for (int32_t k = 0; ok && k < L.npaths; k++) {
        if (plain && L.wrapped && L.wrapped[k]) continue;
        const JVc *P = L.paths[k];
        int32_t ne = 0;
        for (int32_t h = 0; h < P->nhyps; h++) if (!P->hyps[h].disj && P->hyps[h].cons.op == J_EQ) ne++;
        __int128 *A = calloc((size_t)(ne ? ne : 1) * (size_t)W, sizeof *A);
        int32_t *piv = malloc(sizeof *piv * (size_t)W), *ord = malloc(sizeof *ord * (size_t)W);
        uint8_t *base = calloc((size_t)W, 1);
        if (!A || !piv || !ord || !base) { free(A); free(piv); free(ord); free(base); ok = false; break; }
        int32_t r = 0;
        for (int32_t h = 0; h < P->nhyps && ok; h++) {
            const JHyp *y = &P->hyps[h];
            if (y->disj || y->cons.op != J_EQ) continue;
            for (int32_t t = 0; t < y->cons.nt; t++) {
                int32_t v = y->cons.t[t].var;
                if (v < 0 || v >= L.nv) { ok = false; break; }
                A[(size_t)r * W + v] = fd_int128(&y->cons.t[t].coef);
            }
            A[(size_t)r * W + L.nv] = fd_int128(&y->cons.rhs);
            r++;
        }
        // the old values are the base: every pair's pre (a value fixed at the header is its own pair)
        for (int32_t q = 0; q < L.npairs; q++) base[L.pre[q]] = 1;
        int32_t no = 0;
        for (int32_t v = 0; v < L.nv; v++) if (!base[v]) ord[no++] = v;     // the path's own first
        for (int32_t v = 0; v < L.nv; v++) if (base[v]) ord[no++] = v;
        if (ok) ok = fd_rref(A, r, W, ord, no, piv);
        // per version q: d*post = rhs - sum_b a_b * b, or undetermined
        for (int32_t q = 0; ok && q < K; q++) {
            int32_t pv = L.post[col[q]], pr = piv[pv];
            bool det = pr >= 0;
            for (int32_t v = 0; det && v < L.nv; v++) if (v != pv && A[(size_t)pr * W + v] != 0 && !base[v]) det = false;
            if (!det) { if (ncr < cap) { C[(size_t)ncr * (K + 1) + q] = 1; ncr++; } continue; }
        }
        // c.(post - pre) = 0: scale each version's expression to the path's common denominator D
        __int128 D = 1;
        for (int32_t q = 0; ok && q < K; q++) {
            int32_t pr = piv[L.post[col[q]]];
            if (pr < 0) continue;
            __int128 d = A[(size_t)pr * W + L.post[col[q]]], g = fd_gcd128(D, d);
            if (d < 0) d = -d;
            D = D / g * d;
            if (D > FD_KARR_BIG) ok = false;
        }
        for (int32_t b = 0; ok && b <= L.nv; b++) {                // per base variable, and the constant
            if (b < L.nv && !base[b]) continue;
            if (ncr >= cap) { ok = false; break; }
            __int128 *row = C + (size_t)ncr * (K + 1);
            bool nz = false;
            for (int32_t q = 0; q < K; q++) {
                int32_t pv = L.post[col[q]], pr = piv[pv];
                __int128 coef = 0;
                if (pr >= 0) {
                    __int128 d = A[(size_t)pr * W + pv], m = D / (d < 0 ? -d : d), sgn = d < 0 ? -1 : 1;
                    // post = (rhs - sum a_b b) / d: coefficient of b is -a_b/d, the constant rhs/d
                    __int128 a = b < L.nv ? -A[(size_t)pr * W + b] : A[(size_t)pr * W + L.nv];
                    coef = a * m * sgn;
                }
                if (b < L.nv && b == L.pre[col[q]]) coef -= D;
                if (coef > FD_KARR_BIG || coef < -FD_KARR_BIG) { ok = false; break; }
                row[q] = coef; nz |= coef != 0;
            }
            if (nz) { if (!fd_row_norm(row, K + 1)) ok = false; else ncr++; }
            else memset(row, 0, sizeof *row * (size_t)(K + 1));
        }
        free(A); free(piv); free(ord); free(base);
    }
    // the nullspace of C (ncr x K): each free column a basis vector
    bool okall = true;
    if (ok && ncr > 0) {
        int32_t *piv = malloc(sizeof *piv * (size_t)(K + 1)), *ord = calloc((size_t)K, sizeof *ord);
        if (!piv || !ord) { free(piv); free(ord); free(C); free(L.mem); F->oom = true; return false; }
        for (int32_t q = 0; q < K; q++) ord[q] = q;
        ok = fd_rref(C, ncr, K + 1, ord, K, piv);
        for (int32_t fq = 0; ok && okall && fq < K; fq++) {
            if (piv[fq] >= 0) continue;                          // a free column: c_fq = 1, pivots solve
            __int128 vec[16], den = 1;
            for (int32_t q = 0; q < K; q++) if (piv[q] >= 0) { __int128 d = C[(size_t)piv[q] * (K + 1) + q]; den = den / fd_gcd128(den, d) * (d < 0 ? -d : d); }
            if (den > FD_KARR_BIG) continue;
            int32_t nzt = 0; bool bad = false;
            for (int32_t q = 0; q < K; q++) {
                if (q == fq) vec[q] = den;
                else if (piv[q] >= 0) { __int128 d = C[(size_t)piv[q] * (K + 1) + q]; vec[q] = -C[(size_t)piv[q] * (K + 1) + fq] * (den / d); }
                else vec[q] = 0;
                if (vec[q] > FD_KARR_BIG || vec[q] < -FD_KARR_BIG) bad = true;
                nzt += vec[q] != 0;
            }
            if (bad || nzt < 1 || nzt > JCLAIM_MAXT) continue;
            __int128 g = 0;
            for (int32_t q = 0; q < K; q++) g = fd_gcd128(g, vec[q]);
            JClaim c; memset(&c, 0, sizeof c);
            c.blk = H; c.fn = f;
            int32_t ent[JCLAIM_MAXT]; bool all_ent = true;
            for (int32_t q = 0; q < K; q++) if (vec[q]) {
                c.key[c.nt] = L.pkey[col[q]]; c.tk[c.nt] = L.ptk[col[q]]; c.coef[c.nt] = vec[q] / g;
                ent[c.nt] = -1;                                  // the term's ENTRY pair, if the path offers one
                for (int32_t e = 0; e < L.npk; e++) if (L.ptk[e] == JTERM_ENTRY && L.pkey[e] == c.key[c.nt]) ent[c.nt] = e;
                all_ent &= ent[c.nt] >= 0;
                c.nt++;
            }
            __int128 hi, lo;
            JClaim neg = c;
            for (int32_t t = 0; t < neg.nt; t++) neg.coef[t] = -neg.coef[t];
            bool bounded = fd_entry_max(F, f, as, nv, &c, &hi) && fd_entry_max(F, f, as, nv, &neg, &lo);
            if (F->oom) { okall = false; continue; }
            if (bounded) {
                c.rhs = hi; neg.rhs = lo;                        // c.x <= hi, -c.x <= lo
                okall = fd_push_claim(F, &c, as, nv, cl, nc, ccl) && fd_push_claim(F, &neg, as, nv, cl, nc, ccl);
            }
            // not one constant on entry (LarrazOliveras Fig1's inner loop keeps x + z, whatever x and
            // z were when it began): the same sum against its value on the loop's entry edge, an
            // equality c.x - c.entry(x) = 0, both sides, where every term has its ENTRY pair (c40)
            if (okall && (!bounded || hi != -lo) && all_ent && 2 * c.nt <= JCLAIM_MAXT) {
                JClaim e1 = c; e1.rhs = 0;
                for (int32_t t = 0; t < c.nt; t++) { e1.key[c.nt + t] = c.key[t]; e1.tk[c.nt + t] = JTERM_ENTRY; e1.coef[c.nt + t] = -c.coef[t]; }
                e1.nt = 2 * c.nt;
                JClaim e2 = e1;
                for (int32_t t = 0; t < e2.nt; t++) e2.coef[t] = -e2.coef[t];
                okall = fd_push_claim(F, &e1, as, nv, cl, nc, ccl) && fd_push_claim(F, &e2, as, nv, cl, nc, ccl);
            }
        }
        free(piv); free(ord);
    }
    free(C); free(L.mem);
    return okall;
}
// A header version ONE constant c on every entry edge (`var y i32 = 2` before the loop): c - 1 <= y
// and y <= c + 1, fixed, for Houdini to keep the inductive ones (c itself, and c +- 2, cost the
// corpus 29% more judge time and proved nothing more). A bound the trip's
// own step keeps (HeizmannHoenickeLeikePodelski-ATVA2013-Fig5: y = (y + 1) / 2 from 2 keeps y >= 1,
// and x - y then falls) has no comparison in the program to start from, and fd_iter_bound's
// iteration from no bound finds none: y' = (y + 1) / 2 is unbounded below until y is.
static bool fd_entry_candidates(JFinder *F, IrFunc *f, const JClaim *as, int32_t nv, JClaim **cl, int32_t *nc, int32_t *ccl) {
    char err[200];
    JSlot *s0 = NULL; int32_t ns = 0;
    if (!F->gen->slots(F->gen->ctx, f, &s0, &ns, err, sizeof err) || ns == 0) return true;
    JSlot *sl = malloc(sizeof *sl * (size_t)ns);
    if (!sl) { F->oom = true; return false; }
    memcpy(sl, s0, sizeof *sl * (size_t)ns);
    bool okall = true;
    for (int32_t g = 0; g < ns && okall; g++) {
        if (!sl[g].head || !fd_is_int(sl[g].type)) continue;
        JClaim c; memset(&c, 0, sizeof c);
        c.blk = sl[g].blk; c.fn = f; c.nt = 1; c.key[0] = sl[g].key; c.tk[0] = JTERM_VALUE; c.coef[0] = 1;
        JClaim neg = c; neg.coef[0] = -1;
        __int128 hi, lo;
        if (!fd_entry_max(F, f, as, nv, &c, &hi) || !fd_entry_max(F, f, as, nv, &neg, &lo) || hi != -lo) {
            if (F->oom) okall = false;
            continue;
        }
        c.rhs = hi + 1; neg.rhs = lo + 1;
        okall = fd_push_claim(F, &c, as, nv, cl, nc, ccl) && fd_push_claim(F, &neg, as, nv, cl, nc, ccl);
    }
    free(sl);
    return okall;
}
// A SUM the program computes (PE.11's Singapore-2: `if (x + y) <= 0` before `while x > 0 { x = (x
// + x) + y; y = y - 1 }`): per header and pair of its integer versions that an IR_ADD of the
// function reads (each through its load), the sum against its greatest value on the loop's entry
// edge, and its negation against its least, for Houdini to keep the inductive ones. Here
// x + y <= 0 (x' + y' = 2(x + y) - 1), which then keeps x <= 65535 (x' = x + (x + y)). The
// header's own pair candidates are differences, and VRA's octagon loses the sum at `x = 2x + y`.
// Only the sums the program writes: a sum for every pair at every header was f68's cost.
static bool fd_sum_entry_candidates(JFinder *F, IrFunc *f, const JClaim *as, int32_t nv, JClaim **cl, int32_t *nc, int32_t *ccl) {
    char err[200];
    JSlot *s0 = NULL; int32_t ns = 0;
    if (!F->gen->slots(F->gen->ctx, f, &s0, &ns, err, sizeof err) || ns == 0) return true;
    JSlot *sl = malloc(sizeof *sl * (size_t)ns);
    if (!sl) { F->oom = true; return false; }
    memcpy(sl, s0, sizeof *sl * (size_t)ns);
    int32_t done[64][3], ndone = 0;                          // (header, cell, cell): asked once
    bool okall = true;
    for (IrBlock *b = f->blocks; b && okall; b = b->next)
        for (IrInstr *i = b->instrs; i && okall; i = i->next) {
            if (i->op != IR_ADD || i->n_operands < 2) continue;
            int32_t ck[2] = { -1, -1 };                      // the cell each operand loads, below its casts
            for (int k = 0; k < 2; k++)
                for (IrValue *v = i->operands[k]; v; ) {
                    IrInstr *d = fd_def_in(f, v);
                    if (d && d->op == IR_LOAD && d->n_operands >= 1 && d->operands[0]) { ck[k] = d->operands[0]->id; break; }
                    v = d && d->op == IR_CAST && d->n_operands >= 1 ? d->operands[0] : NULL;
                }
            if (ck[0] < 0 || ck[1] < 0 || ck[0] == ck[1]) continue;
            int32_t k0 = ck[0] < ck[1] ? ck[0] : ck[1], k1 = ck[0] < ck[1] ? ck[1] : ck[0];
            for (int32_t g = 0; g < ns && okall; g++) {      // each header with both as its versions
                if (!sl[g].head || sl[g].key != k0 || !fd_is_int(sl[g].type)) continue;
                bool other = false, seen = false;
                for (int32_t h = 0; h < ns && !other; h++) other = sl[h].head && sl[h].blk == sl[g].blk && sl[h].key == k1 && fd_is_int(sl[h].type);
                for (int32_t q = 0; q < ndone && !seen; q++) seen = done[q][0] == sl[g].blk && done[q][1] == k0 && done[q][2] == k1;
                if (!other || seen || ndone == 64) continue;
                done[ndone][0] = sl[g].blk; done[ndone][1] = k0; done[ndone][2] = k1; ndone++;
                JClaim c; memset(&c, 0, sizeof c);
                c.blk = sl[g].blk; c.fn = f; c.nt = 2;
                c.key[0] = k0; c.tk[0] = JTERM_VALUE; c.coef[0] = 1;
                c.key[1] = k1; c.tk[1] = JTERM_VALUE; c.coef[1] = 1;
                JClaim neg = c; neg.coef[0] = neg.coef[1] = -1;
                __int128 hi, lo;
                bool bh = fd_entry_max(F, f, as, nv, &c, &hi), bl = !F->oom && fd_entry_max(F, f, as, nv, &neg, &lo);
                if (F->oom) { okall = false; break; }
                if (bh) { c.rhs = hi; okall = fd_push_claim(F, &c, as, nv, cl, nc, ccl); }
                if (okall && bl) { neg.rhs = lo; okall = fd_push_claim(F, &neg, as, nv, cl, nc, ccl); }
            }
        }
    free(sl);
    return okall;
}
static bool fd_each_candidates(JFinder *F, IrFunc *f, const FdNest *N, const JClaim *as, int32_t nv, JClaim **cl, int32_t *nc, int32_t *ccl) {
    char err[200];
    int32_t keys[64]; uint8_t tk[64], kind[64]; int32_t nk = 0;
    FdRange er[64];                                          // each array's element type's range
    for (IrBlock *b = f->blocks; b; b = b->next)               // the arrays a claim may name: allocas
        for (IrInstr *i = b->instrs; i && nk < 64; i = i->next) {
            const IrType *at = i->op == IR_ALLOCA && i->result ? i->aux.alloca_ty : NULL;   // what it allocates
            if (!at || at->kind != IRT_ARRAY || !fd_range(at->elem, &er[nk])) continue;
            keys[nk] = i->result->id; tk[nk] = JTERM_EACH; nk++;
        }
    if (nk == 0) return true;
    JSlot *s0 = NULL, *sl = NULL; int32_t ns = 0;              // a guard: an integer version a header makes
    if (F->gen->slots(F->gen->ctx, f, &s0, &ns, err, sizeof err) && ns > 0) {
        if (!(sl = malloc(sizeof *sl * (size_t)ns))) { F->oom = true; return false; }
        memcpy(sl, s0, sizeof *sl * (size_t)ns);
    }
    bool okall = true;
    for (IrBlock *h = f->blocks; h && okall; h = h->next) {
        if (!h->is_loop_header) continue;
        if (!F->gen->term_kinds(F->gen->ctx, f, h->id, keys, tk, nk, kind, err, sizeof err)) continue;
        for (int32_t a = 0; a < nk && okall; a++) {
            if (kind[a] != 2) continue;                         // no element made at this header
            for (int sg = 1; sg >= -1 && okall; sg -= 2) {
                JClaim c; memset(&c, 0, sizeof c);
                c.blk = h->id; c.fn = f; c.nt = 1; c.key[0] = keys[a]; c.tk[0] = JTERM_EACH; c.coef[0] = sg;
                // a bound no tighter than the element's type says nothing, and its instances ride
                // into every path that names an element
                __int128 tb = sg > 0 ? er[a].hi : -er[a].lo;
                if (fd_iter_bound(F, f, as, nv, &c) && c.rhs < tb) { okall = fd_push_claim(F, &c, as, nv, cl, nc, ccl); continue; }
                if (F->oom) { okall = false; break; }
                // ...or each element below a counter: a fill of storage that starts with no value
                // (`var a i32[8]`), whose elements are bounded only once the loop has written them
                int32_t gk[8], ng = fd_store_guards(f, fd_nest_body(N, h->id), N->nbk, keys[a], gk, 8);
                for (int32_t g = 0; g < ns && okall; g++) {
                    if (sl[g].blk != h->id || !sl[g].head || !fd_is_int(sl[g].type)) continue;
                    bool guard = false;
                    for (int32_t q = 0; q < ng && !guard; q++) guard = gk[q] == sl[g].key;
                    if (!guard) continue;
                    JClaim u = c; u.nt = 2;
                    u.key[1] = sl[g].key; u.tk[1] = JTERM_UPTO; u.coef[1] = 1;
                    if (fd_iter_bound(F, f, as, nv, &u) && u.rhs < tb) okall = fd_push_claim(F, &u, as, nv, cl, nc, ccl);
                    else if (F->oom) okall = false;
                }
            }
        }
    }
    free(sl);
    return okall;
}
// PE.13 kind (3), nested loops (AliasDarteFeautrierGonnord-SAS2010 nestedLoop-1): a cell that a
// nested loop only RAISES (`i = k`, with k >= i from the loop inside it) never falls below its value
// on the loop's entry, `i - entry(i) >= 0` (c40's ENTRY term), and the enclosing loop's own
// `i = i + 1` after it then lowers `n - i` by at least one a trip. Per nested header and version that
// never falls (or never rises) on any back-edge path, and has its ENTRY pair: the version against
// it, one side. A candidate only: Houdini keeps it if its keep holds (its entry always does). The
// first round has no claims for the loops inside (k >= i), so this runs from the second, as
// fd_nested_candidates does.
// A SINGLE loop too, for a cell the function returns (bench/simd_lexer's `scan_to`, `u32 >= start`:
// `i` only grows from `start` through two loops one after the other, TCW's real programs): there,
// besides its entry, the version against each integer parameter the generator can name at the
// header, `p - v <= 0` (never falls); the second loop's entry is the first's exit, so only the
// parameter carries `i >= start` across both. Only returned cells: every version of every loop
// against every parameter is a search per header the corpus would pay for nothing.
static bool fd_returns_cell(IrFunc *f, int32_t key);
// Whether cell `key` (-1: any) is what f subtracts from to measure it, or a subslice's upper end
// (fd_sub_partners)
static bool fd_cell_feeds_sub(IrFunc *f, int32_t key) { return fd_sub_partners(f, key, NULL, NULL, 1) > 0; }
static bool fd_monotone_candidates(JFinder *F, IrFunc *f, const FdNest *N, const JClaim *as, int32_t nv, JClaim **cl, int32_t *nc, int32_t *ccl) {
    JWork slim = F->lim; slim.pivot /= 16; slim.node /= 16; slim.leaf /= 16;
    char err[200];
    for (IrBlock *h2 = f->blocks; h2; h2 = h2->next) {
        bool nested = h2->is_loop_header && fd_nest_around(N, h2->id, h2->id) >= 0;
        if (!h2->is_loop_header || (!nested && !fd_returns_cell(f, -1) && !fd_cell_feeds_sub(f, -1))) continue;
        FdLoop L;
        if (!fd_loop_build(F, f, h2->id, as, nv, &L)) { free(L.mem); if (F->oom) return false; continue; }
        for (int32_t v = 0; v < L.npk; v++) {
            if (L.ptk[v] != JTERM_VALUE) continue;
            int32_t e = -1;
            for (int32_t z = 0; z < L.npk && e < 0; z++) if (L.ptk[z] == JTERM_ENTRY && L.pkey[z] == L.pkey[v]) e = z;
            if (e < 0 || !L.moves || !L.moves[v] || (!nested && !fd_returns_cell(f, L.pkey[v]) && !fd_cell_feeds_sub(f, L.pkey[v]))) continue;
            int dir = 0;                                     // +1: never falls, -1: never rises
            for (int sg = 1; sg >= -1 && !dir; sg -= 2) {
                bool all = L.npaths > 0;
                for (int32_t k = 0; k < L.npaths && all; k++) {
                    JTerm t[2]; JVc q = *L.paths[k];         // sg*(pre - post) <= 0, terms ascending
                    int32_t a = L.pre[v], b = L.post[v];
                    t[0].var = a < b ? a : b; t[1].var = a < b ? b : a;
                    fd_jint(&t[0].coef, a < b ? sg : -sg); fd_jint(&t[1].coef, a < b ? -sg : sg);
                    memset(&q.goal, 0, sizeof q.goal); q.goal.op = J_LE; q.goal.t = t; q.goal.nt = 2; fd_jint(&q.goal.rhs, 0);
                    all = lia_solve(&q, slim, F->work, F->worksz).k == J_ANS_CERT;
                }
                if (all) dir = sg;
            }
            if (!dir) continue;
            JClaim c; memset(&c, 0, sizeof c);               // +1: entry(v) - v <= 0; -1: v - entry(v) <= 0
            c.blk = h2->id; c.fn = f; c.nt = 2; c.rhs = 0;
            c.key[0] = L.pkey[v]; c.tk[0] = JTERM_VALUE; c.coef[0] = -dir;
            c.key[1] = L.pkey[v]; c.tk[1] = JTERM_ENTRY; c.coef[1] = dir;
            if (!fd_push_claim(F, &c, as, nv, cl, nc, ccl)) { free(L.mem); return false; }
            if (nested) continue;
            int32_t pk[16]; uint8_t kind[16]; int32_t np = 0;     // ...and against each parameter
            for (IrParam *p = f->params; p && np < 16; p = p->next)
                if (p->value && fd_is_int(p->value->type)) pk[np++] = p->value->id;
            if (np == 0 || !F->gen->term_kinds(F->gen->ctx, f, h2->id, pk, NULL, np, kind, err, sizeof err)) continue;
            for (int32_t q = 0; q < np; q++) {
                if (kind[q] != 1 || pk[q] == L.pkey[v]) continue;      // a value fixed at the header
                JClaim d; memset(&d, 0, sizeof d);
                d.blk = h2->id; d.fn = f; d.nt = 2; d.rhs = 0;
                int32_t lo = pk[q] < L.pkey[v] ? pk[q] : L.pkey[v], hi = pk[q] < L.pkey[v] ? L.pkey[v] : pk[q];
                d.key[0] = lo; d.tk[0] = JTERM_VALUE; d.coef[0] = lo == L.pkey[v] ? -dir : dir;
                d.key[1] = hi; d.tk[1] = JTERM_VALUE; d.coef[1] = hi == L.pkey[v] ? -dir : dir;
                if (!fd_push_claim(F, &d, as, nv, cl, nc, ccl)) { free(L.mem); return false; }
            }
        }
        free(L.mem);
    }
    return true;
}
// A RATE (TCW's real programs, isqrt.ln's `main`: `bad` grows by at most 2 a lap of `for k in
// 0..2000`, so `bad + 1` fits): per header, a counter j whose step is one constant of one sign on
// every back-edge path, bounded the way it steps by a verified claim (k <= 2000), against each
// version t that no verified claim bounds near its range (|bound| <= 2^24) on either side: if t's
// step is bounded on every path, L*t - c*j never rises (rank.h's lia_relations), so it stays at most
// its greatest value on the loop's entry, `bad - 2*k <= 0`. A candidate only, from the second round
// (the counter's bound is the first's). lia_relations asks two maximizations per pair and path, so
// it is asked of those pairs alone, and only where both kinds exist: a sum for every pair at every
// header (f68) cost +248% finder work on the trap leg.
#define FD_RATE_NEAR ((__int128)1 << 24)
// Whether f stores into cell `key` a checked `+` or `-` that reads it (`bad = bad + 1`): an
// accumulator whose step owes that it fit. A `+%` wraps by its mode and owes nothing, nor does a
// trap leg's `+?` (it panics past the type): a rate claim there bounds nothing anyone is owed.
static bool fd_checked_accumulator(IrFunc *f, int32_t key) {
    for (IrBlock *b = f->blocks; b; b = b->next)
        for (IrInstr *i = b->instrs; i; i = i->next) {
            if (i->op != IR_STORE || i->n_operands < 2 || !i->operands[0] || i->operands[0]->id != key) continue;
            IrInstr *d = fd_def_in(f, i->operands[1]);
            if (d && (d->op == IR_ADD || d->op == IR_SUB) && d->wrap == IR_WRAP_CHECK && d->n_operands >= 2 &&
                (fd_reads_cell(f, d->operands[0], key, NULL) || fd_reads_cell(f, d->operands[1], key, NULL))) return true;
        }
    return false;
}
static bool fd_rate_candidates(JFinder *F, IrFunc *f, const JClaim *as, int32_t nv, JClaim **cl, int32_t *nc, int32_t *ccl) {
    JWork slim = F->lim; slim.pivot /= 16; slim.node /= 16; slim.leaf /= 16;
    for (IrBlock *h = f->blocks; h; h = h->next) {
        if (!h->is_loop_header) continue;
        FdLoop L;
        if (!fd_loop_build(F, f, h->id, as, nv, &L)) { free(L.mem); if (F->oom) return false; continue; }
        int32_t sel[16], ns = 0, ncount = 0, nfree = 0;      // the pairs asked: a counter, the free versions
        int8_t role[16];
        for (int32_t v = 0; v < L.npk && L.npk <= 16 && L.npaths <= 8; v++) {
            if (L.ptk[v] != JTERM_VALUE || !L.moves || !L.moves[v]) continue;
            __int128 U, D;                                   // the verified bounds first: no search
            bool hu = fd_tight1(as, nv, f, h->id, L.pkey[v], 1, &U), hd = fd_tight1(as, nv, f, h->id, L.pkey[v], -1, &D);
            if ((!(hu && U <= FD_RATE_NEAR && U >= -FD_RATE_NEAR) || !(hd && D <= FD_RATE_NEAR && D >= -FD_RATE_NEAR)) &&
                fd_checked_accumulator(f, L.pkey[v])) {
                sel[ns] = v; role[ns++] = 0; nfree++;
            }
            if (ncount || !nfree || (!hu && !hd)) continue;  // one counter, once an accumulator is free
            int64_t d0 = 0; bool cst = true;                 // one constant step of one sign
            for (int32_t k = 0; k < L.npaths && cst; k++) {
                int64_t lo, hi;
                cst = rk_delta_sliced(L.paths[k], L.pre[v], L.post[v], slim, F->work, F->worksz, &lo, &hi) && lo == hi && lo != 0 &&
                      (!d0 || (lo > 0) == (d0 > 0));
                d0 = lo;
            }
            if (!cst || !(d0 > 0 ? hu : hd)) continue;      // bounded the way it steps
            if (ns && sel[ns - 1] == v) { role[ns - 1] = 1; nfree--; } else sel[ns++] = v, role[ns - 1] = 1;
            ncount++;
        }
        LRel rel[32]; int32_t spre[16], spost[16], nr = 0;
        for (int32_t q = 0; q < ns; q++) { spre[q] = L.pre[sel[q]]; spost[q] = L.post[sel[q]]; }
        if (ncount > 0 && nfree > 0) nr = lia_relations(L.paths, L.npaths, spre, spost, ns, slim, F->work, F->worksz, rel, 32);
        for (int32_t q = 0; q < nr; q++) {
            const LRel *r = &rel[q];
            if (role[r->j] != 1 || role[r->t] != 0) continue;   // a free version against a bounded counter
            int32_t t = sel[r->t], j = sel[r->j];
            __int128 B, K;
            if (!fd_tight1(as, nv, f, h->id, L.pkey[j], r->cj < 0 ? 1 : -1, &B)) continue;   // j unbounded that way
            JClaim c; memset(&c, 0, sizeof c);
            c.blk = h->id; c.fn = f; c.nt = 2;
            c.key[0] = L.pkey[t]; c.tk[0] = JTERM_VALUE; c.coef[0] = r->ct;
            c.key[1] = L.pkey[j]; c.tk[1] = JTERM_VALUE; c.coef[1] = r->cj;
            if (!fd_entry_max(F, f, as, nv, &c, &K)) { if (F->oom) { free(L.mem); return false; } continue; }
            c.rhs = K;
            if (!fd_push_claim(F, &c, as, nv, cl, nc, ccl)) { free(L.mem); return false; }
        }
        free(L.mem);
    }
    return true;
}
// The same for a SUM of two versions (Euclid's `t = b; b = a % b; a = t`: neither a nor b is
// monotone, a + b never rises, since a % b <= a): per header with at most FD_SUMV moving versions
// that have their ENTRY pairs, each pair whose sum never rises (or never falls) on any back-edge
// path, against the sum of their entry values, one side; only for a pair one of which the function
// returns as it is (the bound is for its return; every pair at every header cost Gothenburg_v2-1 a
// second search), and not for two versions monotone alone the same way (their own claims say as
// much). With it gcd(a0, b0)'s return is at most
// a0 + b0 (and a guarded claim of gcd(48, 36) at most 84). A candidate only, from the second round
// as fd_monotone_candidates.
#define FD_SUMV 8
// Whether f returns cell `key` as it is (`return a`: a LOAD of it, or a cast of one, at a return);
// key -1: any cell
static bool fd_returns_cell(IrFunc *f, int32_t key) {
    for (IrBlock *b = f->blocks; b; b = b->next) {
        if (b->term.kind != IR_TERM_RET || !b->term.cond) continue;
        IrValue *v = b->term.cond;
        for (int hop = 0; hop < 2 && v; hop++) {
            IrInstr *d = NULL;
            for (IrBlock *c = f->blocks; c && !d; c = c->next)
                for (IrInstr *i = c->instrs; i && !d; i = i->next) if (i->result == v) d = i;
            if (!d) break;
            if (d->op == IR_LOAD && d->n_operands >= 1 && d->operands[0] && (key < 0 || d->operands[0]->id == key)) return true;
            v = d->op == IR_CAST && d->n_operands >= 1 ? d->operands[0] : NULL;
        }
    }
    return false;
}
static bool fd_sum_monotone_candidates(JFinder *F, IrFunc *f, const JClaim *as, int32_t nv, JClaim **cl, int32_t *nc, int32_t *ccl) {
    JWork slim = F->lim; slim.pivot /= 16; slim.node /= 16; slim.leaf /= 16;
    if (!fd_returns_cell(f, -1)) return true;                // no cell returned: nothing to bound
    for (IrBlock *h = f->blocks; h; h = h->next) {
        if (!h->is_loop_header) continue;
        FdLoop L;
        if (!fd_loop_build(F, f, h->id, as, nv, &L)) { free(L.mem); if (F->oom) return false; continue; }
        int32_t mv[FD_SUMV], me[FD_SUMV], md[FD_SUMV], nm = 0;
        bool many = false, ret = false;                      // only where the function returns one of them
        for (int32_t v = 0; v < L.npk && !ret; v++)
            ret = L.ptk[v] == JTERM_VALUE && L.moves && L.moves[v] && fd_returns_cell(f, L.pkey[v]);
        if (!ret) { free(L.mem); continue; }
        for (int32_t v = 0; v < L.npk && !many; v++) {
            if (L.ptk[v] != JTERM_VALUE || !L.moves || !L.moves[v]) continue;
            int32_t e = -1;
            for (int32_t z = 0; z < L.npk && e < 0; z++) if (L.ptk[z] == JTERM_ENTRY && L.pkey[z] == L.pkey[v]) e = z;
            if (e < 0) continue;
            // its own direction, if it is monotone alone: two versions monotone the same way need no
            // sum (their own claims say as much), and every such pair cost Gothenburg_v2-1 6 s
            int mono = 0;
            for (int sg = 1; sg >= -1 && !mono; sg -= 2) {
                bool all = L.npaths > 0;
                for (int32_t k = 0; k < L.npaths && all; k++) {
                    JTerm t[2]; JVc q = *L.paths[k];
                    int32_t a = L.pre[v], b = L.post[v];
                    t[0].var = a < b ? a : b; t[1].var = a < b ? b : a;
                    fd_jint(&t[0].coef, a < b ? sg : -sg); fd_jint(&t[1].coef, a < b ? -sg : sg);
                    memset(&q.goal, 0, sizeof q.goal); q.goal.op = J_LE; q.goal.t = t; q.goal.nt = 2; fd_jint(&q.goal.rhs, 0);
                    all = lia_solve(&q, slim, F->work, F->worksz).k == J_ANS_CERT;
                }
                if (all) mono = sg;
            }
            if (nm == FD_SUMV) { many = true; break; }
            mv[nm] = v; me[nm] = e; md[nm] = mono; nm++;
        }
        for (int32_t i = 0; i < nm && !many; i++)
            for (int32_t j = i + 1; j < nm; j++) {
                int32_t u = mv[i], w = mv[j];
                if (md[i] && md[i] == md[j]) continue;
                if (!fd_returns_cell(f, L.pkey[u]) && !fd_returns_cell(f, L.pkey[w])) continue;
                int dir = 0;                                 // +1: never falls, -1: never rises
                for (int sg = 1; sg >= -1 && !dir; sg -= 2) {
                    bool all = L.npaths > 0;
                    for (int32_t k = 0; k < L.npaths && all; k++) {
                        int32_t vs[4] = { L.pre[u], L.pre[w], L.post[u], L.post[w] };   // sg*(pre - post) <= 0
                        int cs[4] = { sg, sg, -sg, -sg };
                        JTerm t[4]; int32_t nt = 0;
                        for (int32_t a = 0; a < 4; a++) {    // ascending by variable, a variable once
                            int32_t p = nt;
                            while (p > 0 && t[p - 1].var > vs[a]) p--;
                            if (p > 0 && t[p - 1].var == vs[a]) { all = false; break; }
                            for (int32_t q = nt; q > p; q--) t[q] = t[q - 1];
                            t[p].var = vs[a]; fd_jint(&t[p].coef, cs[a]); nt++;
                        }
                        if (!all) break;
                        JVc q = *L.paths[k];
                        memset(&q.goal, 0, sizeof q.goal); q.goal.op = J_LE; q.goal.t = t; q.goal.nt = nt; fd_jint(&q.goal.rhs, 0);
                        all = lia_solve(&q, slim, F->work, F->worksz).k == J_ANS_CERT;
                    }
                    if (all) dir = sg;
                }
                if (!dir) continue;
                JClaim c; memset(&c, 0, sizeof c);           // +1: entries - sum <= 0; -1: sum - entries <= 0
                c.blk = h->id; c.fn = f; c.nt = 4; c.rhs = 0;
                c.key[0] = L.pkey[u]; c.tk[0] = JTERM_VALUE; c.coef[0] = -dir;
                c.key[1] = L.pkey[w]; c.tk[1] = JTERM_VALUE; c.coef[1] = -dir;
                c.key[2] = L.pkey[me[i]]; c.tk[2] = JTERM_ENTRY; c.coef[2] = dir;
                c.key[3] = L.pkey[me[j]]; c.tk[3] = JTERM_ENTRY; c.coef[3] = dir;
                if (!fd_push_claim(F, &c, as, nv, cl, nc, ccl)) { free(L.mem); return false; }
            }
        free(L.mem);
    }
    return true;
}
// A call argument that is a constant where the call is made: a CONST, or a cast of one into an
// integer type that holds it (`f(3)` passes 3 as cast(i32 3)), as the generator reads a literal
// (jg_const_of): a guarded claim is stated as a bound only where the generator sees the constant.
static bool fd_arg_const(IrFunc *f, IrValue *v, __int128 *c, int depth) {
    if (!v || depth > 4) return false;
    IrInstr *d = NULL;
    for (IrBlock *b = f->blocks; b && !d; b = b->next)
        for (IrInstr *i = b->instrs; i && !d; i = i->next) if (i->result == v) d = i;
    if (!d) return false;
    if (d->op == IR_CONST) { if (!fd_is_int(d->result->type)) return false; *c = fd_const(d); return true; }
    if (d->op == IR_CAST && d->n_operands >= 1 && fd_is_int(d->result->type) && fd_arg_const(f, d->operands[0], c, depth + 1)) {
        FdRange r;
        return fd_range(d->result->type, &r) && r.lo <= *c && *c <= r.hi;
    }
    return false;
}
static bool fd_calls_itself(const JFinder *F, IrFunc *h) {
    for (IrBlock *b = h->blocks; b; b = b->next)
        for (IrInstr *i = b->instrs; i; i = i->next) if (fd_callee(F, i) == h) return true;
    return false;
}
// B1 (s21): GUARDED return claims that hold where the RETURNED VALUE says so. A callee that returns
// a SENTINEL on some path and something else on another (peek's 0 past the end; find_from's
// hay.len when nothing is found), the sentinel a constant, an integer parameter or a slice
// parameter's length, and whose result a caller compares (as it is, or read back from the cell it
// was stored to) with a constant, or with what it passed for that parameter: each side of the
// sentinel such a comparison selects is a `when` (`peek(p) == 40`: -ret <= -1, ret above 0;
// `k >= hay.len`: ret - len(hay) <= -1 and its mirror). The claims are the sums t1 - t2 and +-t1 of
// the callee's return terms (ret, its integer parameters, its slice parameters' lengths, a
// reference parameter's integer or slice parts as it was entered), each at its greatest value over
// the returns `when` admits, kept only where that is below the greatest over every return (the
// same sums under a `when` every return meets), at most FD_WHEN_PER a side: the furthest below
// first, then those naming ret. `pre(p.pos) - pre(p.src) <= -1 where -ret <= -1` (calc.ln's four
// `p.pos + 1`), `from - ret <= 0 where ret - len(hay) <= -1` (search.ln's loop advances). Each
// callee on its facts' account. Untrusted like s7's claims, and checked as one set with them.
#define FD_WHEN_MAXT 6
static void fd_meter_to(const IrFunc *f, bool rank);
#define FD_WHEN_PER 2
#define FD_MAXWHEN 32
typedef struct { int32_t key; uint8_t tk; int16_t fld; } FdRTerm;
// h's return terms, ret first (FD_WHEN_MAXT at most), as jg_ret_terms_ok admits them
static int32_t fd_ret_terms(const IrFunc *h, FdRTerm *t) {
    int32_t n = 0;
    t[n].key = JKEY_RET; t[n].tk = JTERM_VALUE; t[n].fld = -1; n++;
    for (IrParam *p = h->params; p && n < FD_WHEN_MAXT; p = p->next) {
        const IrType *pt = p->value ? p->value->type : NULL;
        if (!pt) continue;
        if (pt->kind == IRT_INT) { t[n].key = p->value->id; t[n].tk = JTERM_VALUE; t[n].fld = -1; n++; }
        else if (pt->kind == IRT_SLICE) { t[n].key = p->value->id; t[n].tk = JTERM_LEN; t[n].fld = -1; n++; }
        else if (pt->kind == IRT_PTR && !pt->is_raw && pt->elem && pt->elem->kind == IRT_INT) {
            t[n].key = p->value->id; t[n].tk = JTERM_PRE; t[n].fld = -1; n++;
        } else if (pt->kind == IRT_PTR && !pt->is_raw && pt->elem && pt->elem->kind == IRT_STRUCT)
            for (int k = 0; k < pt->elem->n_fields && n < FD_WHEN_MAXT; k++) {
                const IrType *ft = pt->elem->fields[k];
                if (!ft || (ft->kind != IRT_INT && ft->kind != IRT_SLICE)) continue;
                t[n].key = p->value->id; t[n].tk = JTERM_PRE; t[n].fld = (int16_t)k; n++;
            }
    }
    return n;
}
// The return term a value of h is (its index in t), or -1: an integer parameter, a slice
// parameter's length
static int32_t fd_ret_term_of(IrFunc *h, const IrValue *v, const FdRTerm *t, int32_t nt) {
    IrInstr *d = fd_def_in(h, v);
    for (int32_t k = 1; k < nt; k++) {
        if (t[k].tk == JTERM_VALUE && v && v->id == t[k].key) return k;
        if (t[k].tk == JTERM_LEN && d && d->op == IR_SLICE_LEN && d->n_operands >= 1 && d->operands[0] && d->operands[0]->id == t[k].key) return k;
    }
    return -1;
}
// A return term's box: its type's range, a slice's length in [0, MAX_LEN] (gen.h's jg_len_max)
static void fd_rterm_box(const IrFunc *h, const FdRTerm *t, __int128 *lo, __int128 *hi) {
    const IrType *ty = NULL;
    if (t->key == JKEY_RET) ty = h->ret_type;
    for (IrParam *p = h->params; p && !ty; p = p->next) {
        if (!p->value || p->value->id != t->key) continue;
        ty = p->value->type;
        if (t->tk == JTERM_PRE && ty && ty->elem) ty = t->fld < 0 ? ty->elem : t->fld < ty->elem->n_fields ? ty->elem->fields[t->fld] : NULL;
    }
    FdRange r;
    if (ty && ty->kind == IRT_SLICE) {
        int64_t es = ty->elem && ty->elem->kind == IRT_INT && ty->elem->bits >= 8 ? ty->elem->bits / 8 : 1;
        *lo = 0; *hi = INT64_MAX / es;
    } else if (fd_range(ty, &r)) { *lo = r.lo; *hi = r.hi; }
    else { *lo = -((__int128)1 << 100); *hi = (__int128)1 << 100; }
}
typedef struct { bool isc; __int128 c; int32_t t; bool other, lo, hi; } FdSent;
static void fd_when_set(JClaim *w, IrFunc *h, const FdRTerm *t, int32_t a, __int128 ca, int32_t b, __int128 cb, __int128 rhs) {
    memset(w, 0, sizeof *w);
    w->blk = JCLAIM_RET; w->fn = h; w->rhs = rhs;
    w->key[0] = t[a].key; w->tk[0] = t[a].tk; w->fld[0] = t[a].fld; w->coef[0] = ca; w->nt = 1;
    if (b >= 0) { w->key[1] = t[b].key; w->tk[1] = t[b].tk; w->fld[1] = t[b].fld; w->coef[1] = cb; w->nt = 2; }
}
static bool fd_when_claims(JFinder *F, IrCallGraph *G, const JClaim *as, int32_t nv, JGuardClaim **gcp, int32_t *ngcp) {
    char err[200];
    int32_t added = 0;
    for (IrFunc *h = F->mod; h && added < FD_MAXWHEN; h = h->next) {
        FdRange rr;
        if (h->is_extern || !h->entry || h->incomplete || !h->ret_type || h->ret_type->kind != IRT_INT || !fd_range(h->ret_type, &rr)) continue;
        int k = ir_cg_index(G, h);
        if (k < 0 || ir_cg_members(G, k, NULL, 0) != 1 || fd_calls_itself(F, h)) continue;
        FdRTerm t[FD_WHEN_MAXT]; int32_t nt = fd_ret_terms(h, t);
        FdSent S[8]; int32_t ns = 0, nret = 0;
        for (IrBlock *b = h->blocks; b; b = b->next) {         // the returns' sentinels
            if (b->term.kind != IR_TERM_RET || !b->term.cond) continue;
            nret++;
            FdSent s; memset(&s, 0, sizeof s); s.t = -1;
            if (fd_arg_const(h, b->term.cond, &s.c, 0)) s.isc = s.c >= rr.lo && s.c <= rr.hi;
            else s.t = fd_ret_term_of(h, b->term.cond, t, nt);
            if (!s.isc && s.t < 0) continue;
            bool seen = false;
            for (int32_t q = 0; q < ns && !seen; q++) seen = S[q].isc == s.isc && (s.isc ? S[q].c == s.c : S[q].t == s.t);
            if (!seen && ns < 8) S[ns++] = s;
        }
        for (int32_t q = 0; q < ns; q++) {                     // a return other than it
            int32_t same = 0;
            for (IrBlock *b = h->blocks; b; b = b->next) {
                if (b->term.kind != IR_TERM_RET || !b->term.cond) continue;
                __int128 c;
                if (S[q].isc ? fd_arg_const(h, b->term.cond, &c, 0) && c == S[q].c : fd_ret_term_of(h, b->term.cond, t, nt) == S[q].t) same++;
            }
            S[q].other = same < nret;
        }
        if (ns == 0) continue;
        // the sides the callers' comparisons select
        for (IrFunc *f = F->mod; f; f = f->next) {
            if (f->is_extern || !f->entry) continue;
            for (IrBlock *b = f->blocks; b; b = b->next)
                for (IrInstr *i = b->instrs; i; i = i->next) {
                    if (fd_callee(F, i) != h || !i->result) continue;
                    const IrValue *rd[16]; int32_t nrd = 0; rd[nrd++] = i->result;
                    for (int pass = 0; pass < 2; pass++)          // stored to a cell, read back, cast
                        for (IrBlock *b2 = f->blocks; b2; b2 = b2->next)
                            for (IrInstr *x = b2->instrs; x && nrd < 16; x = x->next) {
                                bool in = false;
                                if (x->op == IR_CAST && x->n_operands >= 1 && x->result)
                                    for (int32_t r = 0; r < nrd && !in; r++) in = x->operands[0] == rd[r];
                                if (x->op == IR_LOAD && x->n_operands >= 1 && x->result)
                                    for (IrBlock *b3 = f->blocks; b3 && !in; b3 = b3->next)
                                        for (IrInstr *y = b3->instrs; y && !in; y = y->next)
                                            in = y->op == IR_STORE && y->n_operands >= 2 && y->operands[0] == x->operands[0] && y->operands[1] == i->result;
                                bool dup = false;
                                for (int32_t r = 0; r < nrd && !dup; r++) dup = rd[r] == x->result;
                                if (in && !dup) rd[nrd++] = x->result;
                            }
                    for (IrBlock *b2 = f->blocks; b2; b2 = b2->next)
                        for (IrInstr *x = b2->instrs; x; x = x->next) {
                            if (x->op != IR_ICMP || x->n_operands < 2) continue;
                            for (int sd = 0; sd < 2; sd++) {
                                bool mine = false;
                                for (int32_t r = 0; r < nrd && !mine; r++) mine = x->operands[sd] == rd[r];
                                if (!mine) continue;
                                IrValue *o = x->operands[1 - sd];
                                __int128 c;
                                bool isc = fd_arg_const(f, o, &c, 0);
                                int32_t ot = -1;                  // the term the caller passed it as
                                IrInstr *od = isc ? NULL : fd_def_in(f, o);
                                int pk = 0;
                                for (IrParam *p = h->params; p && !isc && ot < 0; p = p->next, pk++) {
                                    if (pk >= i->n_operands || !p->value) continue;
                                    for (int32_t q = 1; q < nt && ot < 0; q++) {
                                        if (t[q].key != p->value->id) continue;
                                        if (t[q].tk == JTERM_VALUE && o == i->operands[pk]) ot = q;
                                        if (t[q].tk == JTERM_LEN && od && od->op == IR_SLICE_LEN && od->n_operands >= 1 && od->operands[0] == i->operands[pk]) ot = q;
                                    }
                                }
                                for (int32_t q = 0; q < ns; q++) {
                                    if (S[q].isc && isc) { if (c <= S[q].c) S[q].lo = true; if (c >= S[q].c) S[q].hi = true; }
                                    if (!S[q].isc && ot >= 0 && S[q].t == ot) S[q].lo = S[q].hi = true;
                                }
                            }
                        }
                }
        }
        JClaim W[17]; int32_t nw = 0;
        fd_when_set(&W[nw++], h, t, 0, 1, -1, 0, rr.hi);       // every return: ret <= its type's top
        for (int32_t q = 0; q < ns && nw + 2 <= 17; q++) {
            if (!S[q].other) continue;
            if (S[q].isc) {
                if (S[q].hi && S[q].c + 1 <= rr.hi) fd_when_set(&W[nw++], h, t, 0, -1, -1, 0, -(S[q].c + 1));
                if (S[q].lo && S[q].c - 1 >= rr.lo) fd_when_set(&W[nw++], h, t, 0, 1, -1, 0, S[q].c - 1);
            } else if (S[q].lo || S[q].hi) {
                fd_when_set(&W[nw++], h, t, 0, 1, S[q].t, -1, -1);
                fd_when_set(&W[nw++], h, t, 0, -1, S[q].t, 1, -1);
            }
        }
        if (nw < 2) continue;
        // the sums: t_a - t_b and +-t_a
        enum { NG = FD_WHEN_MAXT * (FD_WHEN_MAXT + 1) };
        JGuardClaim B[NG]; int32_t ng = 0;
        for (int32_t a = 0; a < nt; a++)
            for (int32_t b = -1; b < nt; b++) {
                if (b == a) continue;
                for (int sg = 1; sg >= -1; sg -= 2) {
                    if (b >= 0 && sg < 0) continue;           // a pair once each way (b, a gives the other)
                    JGuardClaim c; memset(&c, 0, sizeof c);
                    c.fn = h; c.nt = 1; c.key[0] = t[a].key; c.tk[0] = t[a].tk; c.fld[0] = t[a].fld; c.coef[0] = sg;
                    if (b >= 0) { c.nt = 2; c.key[1] = t[b].key; c.tk[1] = t[b].tk; c.fld[1] = t[b].fld; c.coef[1] = -1; }
                    if (ng < NG) B[ng++] = c;
                }
            }
        fd_meter_to(h, false);
        __int128 U[NG]; bool Ub[NG];                           // over every return; bounded
        for (int32_t w = 0; w < nw && added < FD_MAXWHEN; w++) {
            __int128 best[NG]; bool any[NG], bnd[NG];
            for (int32_t q = 0; q < ng; q++) { B[q].when = W[w]; any[q] = false; bnd[q] = true; best[q] = 0; }
            JClaimVc *o; int32_t n;
            if (!F->gen->claim_vcs(F->gen->ctx, h, as, nv, B, ng, JGEN_POST, &o, &n, err, sizeof err)) break;
            for (int32_t q = 0; q < n; q++) {
                int32_t s = o[q].claim - nv;
                if (s < 0 || s >= ng || !bnd[s]) continue;
                LMax x = fd_max_goal(F, &o[q].vc);
                if (x.k == LIA_MAX_EMPTY) continue;
                if (x.k != LIA_MAX_BOUND) { bnd[s] = false; continue; }
                __int128 v = fd_int128(&x.b);
                if (!any[s] || v > best[s]) best[s] = v;
                any[s] = true;
            }
            if (w == 0) { for (int32_t q = 0; q < ng; q++) { U[q] = best[q]; Ub[q] = bnd[q] && any[q]; } continue; }
            int32_t pick[FD_WHEN_PER]; __int128 gain[FD_WHEN_PER]; int32_t np = 0;
            for (int32_t q = 0; q < ng; q++) {
                if (!any[q] || !bnd[q] || best[q] > ((__int128)1 << 66) || best[q] < -((__int128)1 << 66)) continue;
                bool same = B[q].nt == W[w].nt;              // the `when` itself, or its mirror
                for (int32_t a = 0; a < B[q].nt && same; a++) {
                    bool in = false;
                    for (int32_t b = 0; b < W[w].nt && !in; b++) in = B[q].key[a] == W[w].key[b] && B[q].tk[a] == W[w].tk[b] && B[q].fld[a] == W[w].fld[b];
                    same = in;
                }
                if (same || (Ub[q] && best[q] >= U[q])) continue;
                // ...nor where `when` and the terms' boxes alone give the bound (`len(hay) - ret <=
                // MAX_LEN - 1 where ret >= 1`): the boxes, `when` tightening each of its terms by
                // the other's box, the sum's greatest value over them
                __int128 lo[FD_WHEN_MAXT], hi[FD_WHEN_MAXT], lo0[FD_WHEN_MAXT], hi0[FD_WHEN_MAXT];
                for (int32_t a = 0; a < nt; a++) { fd_rterm_box(h, &t[a], &lo[a], &hi[a]); lo0[a] = lo[a]; hi0[a] = hi[a]; }
                int32_t wi[2] = { -1, -1 };
                for (int32_t b = 0; b < W[w].nt && b < 2; b++)
                    for (int32_t a = 0; a < nt; a++)
                        if (t[a].key == W[w].key[b] && t[a].tk == W[w].tk[b] && t[a].fld == W[w].fld[b]) wi[b] = a;
                for (int32_t b = 0; b < W[w].nt && b < 2; b++) {
                    if (wi[b] < 0) continue;
                    __int128 rest = W[w].rhs;                  // coef_b * x_b <= rhs - (the other's least)
                    if (W[w].nt == 2 && wi[1 - b] >= 0) rest -= W[w].coef[1 - b] > 0 ? W[w].coef[1 - b] * lo0[wi[1 - b]] : W[w].coef[1 - b] * hi0[wi[1 - b]];
                    if (W[w].coef[b] == 1 && rest < hi[wi[b]]) hi[wi[b]] = rest;
                    if (W[w].coef[b] == -1 && -rest > lo[wi[b]]) lo[wi[b]] = -rest;
                }
                __int128 bx = 0;
                for (int32_t a = 0; a < B[q].nt; a++)
                    for (int32_t z = 0; z < nt; z++)
                        if (t[z].key == B[q].key[a] && t[z].tk == B[q].tk[a] && t[z].fld == B[q].fld[a])
                            bx += B[q].coef[a] > 0 ? B[q].coef[a] * hi[z] : B[q].coef[a] * lo[z];
                if (best[q] >= bx) continue;
                __int128 g = Ub[q] ? U[q] - best[q] : (__int128)1 << 100;
                bool ret = B[q].key[0] == JKEY_RET || (B[q].nt > 1 && B[q].key[1] == JKEY_RET);
                int32_t at = np;                               // the furthest below first, then naming ret
                while (at > 0) {
                    int32_t p = pick[at - 1];
                    bool pret = B[p].key[0] == JKEY_RET || (B[p].nt > 1 && B[p].key[1] == JKEY_RET);
                    if (gain[at - 1] > g || (gain[at - 1] == g && (pret || !ret))) break;
                    at--;
                }
                if (at >= FD_WHEN_PER) continue;
                if (np < FD_WHEN_PER) np++;
                for (int32_t z = np - 1; z > at; z--) { pick[z] = pick[z - 1]; gain[z] = gain[z - 1]; }
                pick[at] = q; gain[at] = g;
            }
            for (int32_t z = 0; z < np && added < FD_MAXWHEN; z++) {
                JGuardClaim *x2 = realloc(*gcp, sizeof *x2 * (size_t)(*ngcp + 1));
                if (!x2) { fd_meter_to(NULL, false); return false; }
                *gcp = x2;
                JGuardClaim c = B[pick[z]]; c.rhs = best[pick[z]];
                (*gcp)[(*ngcp)++] = c; added++;
            }
        }
        fd_meter_to(NULL, false);
    }
    return true;
}
// s7: GUARDED return claims, one pair per call with constant integer arguments to a function of the
// module that is on no recursion cycle (its inner calls would need the call tree's tuples): ret's
// greatest and least value at the callee's returns under its parameters at those constants
// (`t[dbl(3)]`: ret = 6 where a = 3, though no bound holds for every a, since a +% a wraps). Each
// bound is the maximum of the claim's own `post` VCs, so it is implied there; the set is asked again
// whole and a claim any of whose VCs is not implied is dropped. At most two guard parameters
// (JGUARD_MAXP), the first constant ones; at most FD_MAXGUARD claims a module. Untrusted: the judge
// owes each at the callee's returns under its guard, and uses it where the arguments are those
// constants.
#define FD_MAXGUARD 64
static bool fd_guarded_claims(JFinder *F, const JClaim *as, int32_t nv) {
    char err[200];
    IrCallGraph G; memset(&G, 0, sizeof G);
    if (!ir_cg_build(&G, F->mod)) { F->oom = true; return false; }
    JGuardClaim *gc = NULL; int32_t ngc = 0;
    bool ok = true;
    for (IrFunc *f = F->mod; f && ok; f = f->next) {
        if (f->is_extern || !f->entry) continue;
        for (IrBlock *b = f->blocks; b && ok; b = b->next)
            for (IrInstr *i = b->instrs; i && ok && ngc + 2 <= FD_MAXGUARD; i = i->next) {
                IrFunc *h = fd_callee(F, i);
                if (!h || h->is_extern || !h->entry || !fd_same_module(h, f) || !h->ret_type || !fd_is_int(h->ret_type)) continue;
                int k = ir_cg_index(&G, h);
                if (k < 0 || ir_cg_members(&G, k, NULL, 0) != 1 || fd_calls_itself(F, h)) continue;
                JGuardClaim c; memset(&c, 0, sizeof c);
                c.fn = h; c.nt = 1; c.key[0] = JKEY_RET; c.tk[0] = JTERM_VALUE; c.coef[0] = 1;
                int pk = 0;
                for (IrParam *p = h->params; p && c.ng < JGUARD_MAXP; p = p->next, pk++) {
                    __int128 v;
                    if (!p->value || !fd_is_int(p->value->type) || pk >= i->n_operands) continue;
                    if (!fd_arg_const(f, i->operands[pk], &v, 0)) continue;
                    FdRange tr;
                    if (!fd_range(p->value->type, &tr) || v < tr.lo || v > tr.hi) continue;
                    c.gkey[c.ng] = p->value->id; c.gval[c.ng] = v; c.ng++;
                }
                if (c.ng == 0) continue;
                bool dup = false;                            // one pair per guard
                for (int32_t q = 0; q < ngc && !dup; q += 2) {
                    dup = gc[q].fn == h && gc[q].ng == c.ng;
                    for (int g2 = 0; g2 < c.ng && dup; g2++) dup = gc[q].gkey[g2] == c.gkey[g2] && gc[q].gval[g2] == c.gval[g2];
                }
                if (dup) continue;
                JGuardClaim pair[2] = { c, c };
                pair[1].coef[0] = -1;
                JClaimVc *o; int32_t n;
                if (!F->gen->claim_vcs(F->gen->ctx, h, as, nv, pair, 2, JGEN_POST, &o, &n, err, sizeof err)) continue;
                __int128 best[2]; bool any[2] = { false, false }, bounded = true;
                for (int32_t q = 0; q < n && bounded; q++) {
                    int32_t s = o[q].claim - nv;
                    if (s < 0 || s > 1) continue;
                    LMax x = fd_max_goal(F, &o[q].vc);
                    if (x.k == LIA_MAX_EMPTY) continue;
                    if (x.k != LIA_MAX_BOUND) { bounded = false; break; }
                    __int128 v = fd_int128(&x.b);
                    if (!any[s] || v > best[s]) best[s] = v;
                    any[s] = true;
                }
                if (!bounded || !any[0] || !any[1]) continue;
                JGuardClaim *x2 = realloc(gc, sizeof *x2 * (size_t)(ngc + 2));
                if (!x2) { ok = false; break; }
                gc = x2;
                pair[0].rhs = best[0]; pair[1].rhs = best[1];
                gc[ngc++] = pair[0]; gc[ngc++] = pair[1];
            }
    }
    if (ok && !fd_when_claims(F, &G, as, nv, &gc, &ngc)) ok = false;
    // the set whole, once: a claim one of whose VCs is not implied goes (no claim uses another)
    if (ok && ngc > 0) {
        uint8_t *bad = calloc((size_t)ngc, 1);
        if (!bad) ok = false;
        for (IrFunc *h = F->mod; ok && h; h = h->next) {
            if (h->is_extern || !h->entry) continue;
            bool mine = false;
            for (int32_t q = 0; q < ngc && !mine; q++) mine = gc[q].fn == h;
            if (!mine) continue;
            JClaimVc *o; int32_t n;
            if (!F->gen->claim_vcs(F->gen->ctx, h, as, nv, gc, ngc, JGEN_POST, &o, &n, err, sizeof err)) {
                for (int32_t q = 0; q < ngc; q++) if (gc[q].fn == h) bad[q] = 1;
                continue;
            }
            for (int32_t q = 0; q < n; q++) {
                int32_t s = o[q].claim - nv;
                if (s >= 0 && s < ngc && !bad[s] && !fd_implied(F, &o[q].vc)) bad[s] = 1;
            }
        }
        int32_t w = 0;
        for (int32_t q = 0; ok && q < ngc; q++) if (!bad[q]) gc[w++] = gc[q];
        if (ok) ngc = w;
        free(bad);
    }
    ir_cg_free(&G);
    if (!ok) { free(gc); F->oom = true; return false; }
    F->gc = gc; F->ngc = ngc;
    return true;
}
static JClaim *finder_propose_run(JFinder *F, int32_t *ncl);
// The finder's work, metered (lia.h): FD_WORK rational operations an ACCOUNT (below), then every query
// that account pays for answers budget and what was found so far is proposed. Set from the corpus and
// the PE.11 ports' three legs at 8405d47: the most any of them spends is 3.8e8 (tests/trust/
// judge_signed_division_pass.ln, 16 s); a u8 loop with two `if`s of wrapping steps (fuzz_loops'
// wrap mode, seed 2490187, k 216) spent 2.1e9, 146 s, and still had no ranking.
#define FD_WORK 500000000ull
// ...in ACCOUNTS: two per function, one for the facts proposed for it (its candidates) and one for
// its termination (its rankings), and one for the module's work (Houdini over every function's
// candidates, the guarded claims, the cycles): one function's search must not cost another its
// basic facts, nor a function's hard facts its loops' rankings (a `for` over a range is ranked as
// soon as its bounds are kept, which the first round does cheaply). With one meter for the module, isqrt's
// Newton loop spent all of it and `main`'s `for k in 0..2000` lost the claims that rank it (E011,
// TCW's real programs, 2026-10-20). A program's cost is then bounded by its size, as a per-function
// limit is; a verdict no longer depends on the code around it.
static IrFunc **fd_acct_fn; static uint64_t *fd_acct_left; static int32_t fd_acct_n, fd_acct_cur;
// Charge f's account from here on: its facts' (rank false) or its termination's (rank true); NULL:
// the module's. Account i >= 1 is fd_acct_fn[i]'s facts; i + fd_acct_n - 1 its termination.
static void fd_meter_to(const IrFunc *f, bool rank) {
    int32_t k = 0;
    for (int32_t i = 1; f && i < fd_acct_n; i++) if (fd_acct_fn[i] == f) { k = rank ? i + fd_acct_n - 1 : i; break; }
    if (!fd_acct_n || k == fd_acct_cur) return;
    fd_acct_left[fd_acct_cur] = ls_meter_left;
    ls_meter_left = fd_acct_left[k]; fd_acct_cur = k;
}
static JClaim *finder_propose(JFinder *F, int32_t *ncl) {
    uint64_t cap = F->meter_cap ? F->meter_cap : FD_WORK;
    int32_t n = 1;
    for (IrFunc *f = F->mod; f; f = f->next) n++;
    fd_acct_fn = malloc(sizeof *fd_acct_fn * (size_t)n); fd_acct_left = malloc(sizeof *fd_acct_left * (size_t)(2 * n));
    if (!fd_acct_fn || !fd_acct_left) { free(fd_acct_fn); free(fd_acct_left); fd_acct_fn = NULL; fd_acct_left = NULL; F->oom = true; *ncl = 0; return NULL; }
    fd_acct_n = n; fd_acct_cur = 0; fd_acct_fn[0] = NULL;
    n = 1;
    for (IrFunc *f = F->mod; f; f = f->next) fd_acct_fn[n++] = f;
    for (int32_t i = 0; i < 2 * fd_acct_n - 1; i++) fd_acct_left[i] = cap;
    ls_meter_on = true; ls_meter_left = cap; ls_meter_spent = 0;
    struct FdMemo memo; memset(&memo, 0, sizeof memo);
    F->memo = &memo;
    JClaim *r = finder_propose_run(F, ncl);
    F->memo = NULL; free(memo.e);
    ls_meter_on = false;
    fd_acct_left[fd_acct_cur] = ls_meter_left;
    F->meter = ls_meter_spent; F->meter_out = false;
    F->nout_fn = 0; F->out_more = false; F->out_module = fd_acct_left[0] == 0;
    for (int32_t i = 0; i < fd_acct_n; i++) {
        bool out = fd_acct_left[i] == 0 || (i > 0 && fd_acct_left[i + fd_acct_n - 1] == 0);
        if (!out) continue;
        F->meter_out = true;
        if (i == 0) continue;
        if (F->nout_fn < (int32_t)(sizeof F->out_fn / sizeof F->out_fn[0])) F->out_fn[F->nout_fn++] = fd_acct_fn[i];
        else F->out_more = true;
    }
    free(fd_acct_fn); free(fd_acct_left); fd_acct_fn = NULL; fd_acct_left = NULL; fd_acct_n = fd_acct_cur = 0;
    return r;
}
static bool fd_claim_same(const JClaim *a, const JClaim *b) {
    if (a->blk != b->blk || a->nt != b->nt || a->rhs != b->rhs || a->fn != b->fn) return false;
    for (int32_t k = 0; k < a->nt; k++)
        if (a->key[k] != b->key[k] || a->tk[k] != b->tk[k] || a->coef[k] != b->coef[k] || a->fld[k] != b->fld[k]) return false;
    return true;
}
// One round's candidates for f, then pushed to cl[0..nc): its cut points' and returns' bounded
// under the verified claims `as`, then every other kind (some from the second round on); f's facts'
// account pays. False when memory ran out.
static bool fd_fn_candidates(JFinder *F, IrFunc *f, FdNest *N, const JClaim *as, int32_t nv, int round,
                             JCand **cd, int32_t *ncd, int32_t *ccd, JClaim **cl, int32_t *nc, int32_t *ccl) {
    char err[200];
    fd_meter_to(f, false);                                   // f's candidates: its facts' account
    JSlot *s0; int32_t ns = 0;
    JSlot *sl = NULL;
    if (!fd_nest_build(f, N)) return false;
    if (F->gen->slots(F->gen->ctx, f, &s0, &ns, err, sizeof err) && ns > 0) {
        if (!(sl = malloc(sizeof *sl * (size_t)ns))) return false;
        memcpy(sl, s0, sizeof *sl * (size_t)ns);
    }
    // the cut points, block by block, then the returns; each under the verified claims
    for (int32_t i = 0, j; i <= ns; i = j) {
        int32_t only;
        if (i < ns) {
            for (j = i; j < ns && sl[j].blk == sl[i].blk; j++) ;
            if (!block_candidates(F, f, N, as, nv, sl, i, j, cd, ncd, ccd, err, sizeof err)) continue;
            for (int32_t c = 0; c < *ncd; c++) (*cd)[c].c.fn = f;
            only = sl[i].blk;
        } else {
            j = ns + 1;
            if (!ret_candidates(F, f, cd, ncd, ccd)) { free(sl); return false; }
            only = JGEN_POST;
        }
        if (*ncd == 0) continue;
        JClaim *q2 = malloc(sizeof *q2 * (size_t)(nv + *ncd + 1));
        if (!q2) { free(sl); return false; }
        if (nv) memcpy(q2, as, sizeof *q2 * (size_t)nv);
        for (int32_t c = 0; c < *ncd; c++) q2[nv + c] = (*cd)[c].c;
        JClaimVc *o; int32_t n;
        bool gok = F->gen->claim_vcs(F->gen->ctx, f, q2, nv + *ncd, F->hgc, F->nhgc, only, &o, &n, err, sizeof err);
        free(q2);
        if (gok && !bound_candidates(F, *cd, *ncd, o, n, nv, as, nv, cl, nc, ccl)) { free(sl); return false; }
        // a header's back edge from ANY previous trip: each side of each header version
        // alone, under the verified claims only (at their placeholders the header's
        // comparisons, give or take one, contradict each other, and the back edge bounds
        // nothing): `k = next(k)` with `next(k) <= 7` keeps `k <= 7` whatever k was
        if (gok && i < ns && sl[i].head) {
            int32_t n1 = 0;
            for (int32_t c = 0; c < *ncd; c++) if (!(*cd)[c].fixed && (*cd)[c].c.nt == 1) (*cd)[n1++] = (*cd)[c];
            if (n1 > 0) {
                if (!(q2 = malloc(sizeof *q2 * (size_t)(nv + n1)))) { free(sl); return false; }
                if (nv) memcpy(q2, as, sizeof *q2 * (size_t)nv);
                for (int32_t c = 0; c < n1; c++) { q2[nv + c] = (*cd)[c].c; q2[nv + c].rhs = (*cd)[c].tb; }
                gok = F->gen->claim_vcs(F->gen->ctx, f, q2, nv + n1, F->hgc, F->nhgc, only, &o, &n, err, sizeof err);
                free(q2);
                if (gok && !bound_candidates(F, *cd, n1, o, n, nv, as, nv, cl, nc, ccl)) { free(sl); return false; }
            }
        }
    }
    free(sl);
    if (round > 0 && !fd_nested_candidates(F, f, N, as, nv, cl, nc, ccl)) return false;
    if (round > 0 && !fd_monotone_candidates(F, f, N, as, nv, cl, nc, ccl)) return false;
    if (round > 0 && !fd_sum_monotone_candidates(F, f, as, nv, cl, nc, ccl)) return false;
    if (round == 1 && !fd_rate_candidates(F, f, as, nv, cl, nc, ccl)) return false;
    if (!fd_each_candidates(F, f, N, as, nv, cl, nc, ccl)) return false;
    if (!fd_entry_candidates(F, f, as, nv, cl, nc, ccl)) return false;
    if (!fd_sum_entry_candidates(F, f, as, nv, cl, nc, ccl)) return false;
    for (IrBlock *kb = f->blocks; kb; kb = kb->next)
        if (kb->is_loop_header && !fd_karr_candidates(F, f, kb->id, as, nv, cl, nc, ccl)) return false;
    fd_nest_free(N);
    return true;
}
static JClaim *finder_propose_run(JFinder *F, int32_t *ncl) {
    IrFunc *mod = F->mod;
    JClaim *cl = NULL, *as = NULL;
    JCand *cd = NULL;
    int32_t nc = 0, ccl = 0, nv = 0, ncd = 0, ccd = 0;
    FdNest N; memset(&N, 0, sizeof N);                       // the current function's loops
    *ncl = 0;
    F->oom = false;
    F->rk = NULL; F->nrk = 0;                                // the caller frees the last call's
    free(F->rkstage); F->rkstage = NULL;                     // ...and this its labels (fd_track_stages)
    F->gc = NULL; F->ngc = 0;
    for (int round = 0; round < 3; round++) {
        if (nv > ccl) { JClaim *x = realloc(cl, sizeof *x * (size_t)nv); if (!x) goto oom; cl = x; ccl = nv; }
        if (nv) memcpy(cl, as, sizeof *cl * (size_t)nv);
        nc = nv;
        for (IrFunc *f = mod; f; f = f->next) {
            if (f->is_extern || !f->entry) continue;
            if (!fd_fn_candidates(F, f, &N, as, nv, round, &cd, &ncd, &ccd, &cl, &nc, &ccl)) goto oom;
        }
        if (nc == nv) break;                                 // nothing new to verify
        fd_meter_to(NULL, false);                                  // Houdini, over every function's: the module's
        int32_t kept = houdini_module(F, cl, nc, nv, NULL);
        if (kept < 0) goto oom;
        bool grew = kept > nv;
        free(as); as = malloc(sizeof *as * (size_t)(kept ? kept : 1));
        if (!as) goto oom;
        memcpy(as, cl, sizeof *as * (size_t)kept);
        nv = kept;
        if (!grew) break;
    }
    fd_meter_to(NULL, false);
    if (!fd_guarded_claims(F, as, nv)) goto oom;
    // the rankings' paths state them at their calls, as the judge's VCs do (B1: search.ln's loop
    // advances by what find_from returns, which is at least `at` where it is below hay.len)
    if (F->gen->guarded) F->gen->guarded(F->gen->ctx, F->gc, F->ngc);
    // B1: a LAST ROUND for each function that calls one with a `when` claim inside a loop, every
    // claim VC stating the guarded claims at its calls (F->hgc): count_byte's `n <= at` holds only
    // because `at` moves past what find_byte returns, which is at least `at` where it is below
    // hay.len. Houdini solves those functions first, as the only ones whose VCs changed: a guarded
    // claim stated at a call only adds a fact, so the claims kept before stay implied, and the
    // guarded claims, verified under those, stay verified under more.
    {
        int32_t nfn = 0;
        for (IrFunc *f = mod; f; f = f->next) nfn++;
        uint8_t *mark = calloc((size_t)(nfn ? nfn : 1), 1);
        if (!mark) goto oom;
        bool any = false;
        int32_t fk = -1;
        for (IrFunc *f = mod; f; f = f->next) {
            fk++;
            if (f->is_extern || !f->entry || !fd_nest_build(f, &N)) continue;
            for (IrBlock *b = f->blocks; b && !mark[fk]; b = b->next) {
                bool inl = fd_nest_around(&N, b->id, -1) >= 0;
                for (IrInstr *i = b->instrs; i && inl && !mark[fk]; i = i->next) {
                    const IrFunc *h = fd_callee(F, i);
                    for (int32_t q = 0; h && q < F->ngc && !mark[fk]; q++) mark[fk] = F->gc[q].fn == h && F->gc[q].when.nt > 0;
                }
            }
            fd_nest_free(&N);
            any |= mark[fk];
        }
        if (any) {
            if (nv > ccl) { JClaim *x = realloc(cl, sizeof *x * (size_t)nv); if (!x) { free(mark); goto oom; } cl = x; ccl = nv; }
            if (nv) memcpy(cl, as, sizeof *cl * (size_t)nv);
            nc = nv;
            F->hgc = F->gc; F->nhgc = F->ngc;
            fk = -1;
            for (IrFunc *f = mod; f; f = f->next) {
                fk++;
                if (!mark[fk]) continue;
                if (!fd_fn_candidates(F, f, &N, as, nv, 1, &cd, &ncd, &ccd, &cl, &nc, &ccl)) { free(mark); F->hgc = NULL; F->nhgc = 0; goto oom; }
            }
            fd_meter_to(NULL, false);
            int32_t kept = nc > nv ? houdini_module(F, cl, nc, nv, mark) : nv;
            F->hgc = NULL; F->nhgc = 0;
            if (kept < 0) { free(mark); goto oom; }
            // the claims kept before lead, all of them (else the round is dropped: the guarded
            // claims were verified under them)
            bool all = kept >= nv;
            for (int32_t k = 0; k < nv && all; k++) all = fd_claim_same(&cl[k], &as[k]);
            if (kept > nv && all) {
                free(as); as = malloc(sizeof *as * (size_t)kept);
                if (!as) { free(mark); goto oom; }
                memcpy(as, cl, sizeof *as * (size_t)kept);
                nv = kept;
            }
        }
        free(mark);
    }
    free(cd); free(cl); cd = NULL; cl = NULL;
    // PE.5: a ranking claim per loop header, under the claims Houdini kept (owed, never believed)
    IrCallGraph CG0; memset(&CG0, 0, sizeof CG0);
    bool cg0 = ir_cg_build(&CG0, mod);
    for (IrFunc *f = mod; f; f = f->next) {
        if (f->is_extern || !f->entry) continue;
        fd_meter_to(f, true);                                // f's rankings: its termination's
        for (IrBlock *b = f->blocks; b; b = b->next) {
            if (!b->is_loop_header) continue;
            FdLoop L; JRank r;
            bool built = fd_loop_build(F, f, b->id, as, nv, &L);
            bool got = built && fd_rank(F, f, b->id, &L, &r);
            const char *stage = fd_stage;
            if (!got && built && !F->oom && fd_rank_residue(F, f, b->id, as, nv, &r)) { got = true; stage = "residue"; }
            if (!built && L.dead && L.nedges > 0 && L.nedges <= JRANK_MAXE) {
                // no back edge can be taken (a constant guard, c2): the constant measure 0, which
                // falls on every edge only because none runs; the judge owes it as any other. The
                // solver refuted the WHOLE paths, and the judge states for a measure only the facts
                // its terms are made of, none for 0: a refutation that needs another (`a[i]` reads
                // an element only at an index in bounds, and the claims put i at -3:
                // for_signed_range_negative_index_fail.ln) left the judge 0 <= -1 to prove, a false
                // E011 for a loop that ends. So the refuted paths are ranked first, as if they ran,
                // and 0 is proposed only when that finds nothing.
                int32_t ne = L.nedges, ed[JRANK_MAXE];
                for (int32_t e = 0; e < ne; e++) ed[e] = L.edges[e];
                free(L.mem);
                F->keep_dead = true;
                got = fd_loop_build(F, f, b->id, as, nv, &L) && fd_rank(F, f, b->id, &L, &r);
                F->keep_dead = false;
                stage = fd_stage;
                if (!got) {
                    memset(&r, 0, sizeof r);
                    r.blk = b->id; r.fn = f; r.ncomp = 1; r.nedge = ne;
                    for (int32_t e = 0; e < ne; e++) { r.from[e] = ed[e]; r.fall[e] = 0; }
                    got = true; stage = "dead";
                }
            }
            free(L.mem);
            if (!got) continue;
            JRank *x = realloc(F->rk, sizeof *x * (size_t)(F->nrk + 1));
            if (!x) { ir_cg_free(&CG0); goto oom; }
            F->rk = x; F->rk[F->nrk++] = r;
            if (!fd_stage_note(F, 1, stage)) { ir_cg_free(&CG0); goto oom; }
        }
        // ...and a measure for its recursion, if it has one to rank: one component over its
        // parameters at entry, falling at every self-call (the judge owes rec.fall and rec.bound)
        FdLoop L; JRank r;
        // a measure of several rows (a tuple, groups, regions: c42/c43) only for a function whose
        // component is itself alone: gen.h refuses one in a component of several, from either side
        bool alone = cg0 && ir_cg_members(&CG0, ir_cg_index(&CG0, f), NULL, 0) == 1;
        bool got = fd_loop_build(F, f, JRANK_ENTRY, as, nv, &L) && fd_rank(F, f, JRANK_ENTRY, &L, &r) && (r.ncomp == 1 || alone);
        free(L.mem);
        if (!got) continue;
        JRank *x = realloc(F->rk, sizeof *x * (size_t)(F->nrk + 1));
        if (!x) { ir_cg_free(&CG0); goto oom; }
        F->rk = x; F->rk[F->nrk++] = r;
        if (!fd_stage_note(F, 1, fd_stage)) { ir_cg_free(&CG0); goto oom; }
    }
    ir_cg_free(&CG0);
    // ...and a cycle of several functions, jointly (fd_rank_cycle), over the module's components
    fd_meter_to(NULL, false);
    {   IrCallGraph G; memset(&G, 0, sizeof G);
        if (ir_cg_build(&G, mod)) {
            bool *done = calloc((size_t)G.n + 1, 1);
            if (!done) { ir_cg_free(&G); goto oom; }
            for (int32_t k = 0; k < G.n; k++) {
                if (done[k]) continue;
                IrFunc *memb[FD_CYC_MAXM];
                int32_t m = ir_cg_members(&G, k, memb, FD_CYC_MAXM);
                for (int32_t q = 0; q < G.n; q++) if (G.comp[q] == G.comp[k]) done[q] = true;
                if (m < 2 || m > FD_CYC_MAXM) continue;
                bool body = true;
                for (int32_t i = 0; i < m; i++) body = body && !memb[i]->is_extern && memb[i]->entry;
                if (!body) continue;
                JRank rr[FD_CYC_MAXM];
                if (!fd_rank_cycle(F, memb, m, as, nv, rr)) continue;
                int32_t w = 0;                               // a member's own entry rank, if any, gives way
                for (int32_t r = 0; r < F->nrk; r++) {
                    bool mine = false;
                    for (int32_t i = 0; i < m; i++) mine |= F->rk[r].blk == JRANK_ENTRY && F->rk[r].fn == memb[i];
                    if (!mine) { if (F->rkstage) F->rkstage[w] = F->rkstage[r]; F->rk[w++] = F->rk[r]; }
                }
                F->nrk = w;
                JRank *x = realloc(F->rk, sizeof *x * (size_t)(F->nrk + m));
                if (!x) { free(done); ir_cg_free(&G); goto oom; }
                F->rk = x;
                for (int32_t i = 0; i < m; i++) F->rk[F->nrk++] = rr[i];
                if (!fd_stage_note(F, m, "cycle")) { free(done); ir_cg_free(&G); goto oom; }
            }
            free(done);
        }
        ir_cg_free(&G);
    }
    if (F->gen->guarded) F->gen->guarded(F->gen->ctx, NULL, 0);   // F->gc is the caller's from here
    F->kept += nv;
    *ncl = nv;
    if (nv == 0) { free(as); return NULL; }
    return as;
oom:
    if (F->gen->guarded) F->gen->guarded(F->gen->ctx, NULL, 0);
    fd_nest_free(&N);
    free(cd); free(cl); free(as); free(F->rk); F->rk = NULL; F->nrk = 0; free(F->gc); F->gc = NULL; F->ngc = 0;
    F->oom = true;
    return NULL;
}


#endif // LAIN_SOLVER_FINDER_H
