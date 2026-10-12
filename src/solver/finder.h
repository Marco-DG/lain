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
        if (!F->gen->term_kinds(F->gen->ctx, f, H, keys, F->hdr[h].n * 2, kind, err, (uint32_t)errcap)) { free(keys); free(kind); free(tm); return false; }
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
    F->kept += nv;
    *ncl = nv;
    if (nv == 0) { free(as); return NULL; }
    return as;
oom:
    free(cd); free(cl); free(as);
    F->oom = true;
    return NULL;
}


#endif // LAIN_SOLVER_FINDER_H
