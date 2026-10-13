#ifndef LAIN_JUDGE_JUDGE_H
#define LAIN_JUDGE_JUDGE_H
// judge.h — the proof engine's ENTRY POINT (local/internal/design/judge_interfaces.md §8). The
// compiler calls judge_module once per module, after VRA, with the module as it lowered it (static
// evaluation first). TRUSTED, with the generator (gen.h) and the kernel (kernel.h) beneath it:
//   1. it asks the untrusted PROPOSER for the module's claims (a callback; src/solver/finder.h is
//      one), offering it the generator as calls;
//   2. it certifies the claims: every VC each claim owes (hull, inv.entry, inv.keep, post) must
//      have a certificate the KERNEL accepts. If one does not, the claim set is unstable, and every
//      obligation of the module is reported unproved with that claim named: a correct proposer only
//      hands over claims Houdini found inductive, so this is a proposer bug, and it is loud;
//   3. it generates every function's obligations under the claims, asks the untrusted SOLVER for
//      each (a callback) and decides:
//      - `proved` when the kernel accepts the certificate;
//      - `not-implied` with a model only after evaluating every hypothesis and g at it exactly
//        (A6: kernel.h's integers, nothing wraps);
//      - `solver-bug` for a certificate the kernel refuses or a model that fails, either one an
//        answer that cannot be shown;
//      - `outside` for a VC the generator cannot state, `budget` or `capacity` as the solver said;
//   4. it hands one record per VC to the caller's SINK, in a fixed order (the claims' VCs first,
//      then the obligations; functions in module order, VCs in generation order), so two runs give
//      the same records.
// No global state, no exit(), no output of its own (G5): its memory is the caller's region, and
// everything it says is a record, or `err` when it cannot run at all.
#include <string.h>
#include <stdio.h>
#include "judge/format.h"
#include "judge/mem.h"
#include "judge/gen.h"
#include "judge/kernel.h"

typedef struct { JWork lim; } JudgeLimits;   // the solver's budget per VC (§3: pivot, node, leaf)
// The untrusted callbacks. `solve` answers one VC within `lim` (format.h's JAnswer); its answer
// lives until its next call. `propose` hands over the module's claims and its ranking claims (it
// may keep them until its next call; the judge copies them), false when it failed (the judge then
// reports that, and judges with no claims). Either may be NULL: no solver proves nothing; no
// proposer proposes nothing, and a loop owed termination with no ranking claim is `outside`.
typedef struct {
    void *ctx;
    bool (*solve)(void *ctx, const JVc *vc, JWork lim, JAnswer *out);
    bool (*propose)(void *ctx, IrFunc *mod, const JudgeGen *gen, JClaim **out, int32_t *nout, JRank **rk, int32_t *nrk);
} JudgeCalls;

typedef enum {
    JREC_PROVED, JREC_NOT_IMPLIED, JREC_OUTSIDE, JREC_BUDGET, JREC_CAPACITY,
    JREC_SOLVER_BUG,              // an answer the judge cannot show: a refused certificate, a failed model
    JREC_UNCERTIFIED,             // the module's claims are unstable (`reason` names the claim's VC)
    JREC_REFUSED,                 // the generator refuses the function (no obligation id: `fn` only)
    JREC_NO_CLAIMS,               // the proposer failed: the module is judged with no claims
} JRecordKind;
// One record. Its pointers (the id, the VC, the model, the reason) live until the sink returns:
// a sink that keeps them copies them.
typedef struct {
    JRecordKind k;
    IrFunc *fn;                   // NULL for a module's record (JREC_NO_CLAIMS)
    const JId *id;                // the obligation (NULL for JREC_REFUSED and JREC_NO_CLAIMS)
    IrInstr *at; IrBlock *blk; int32_t idx;   // where it is owed (`at` NULL at a block's end)
    const JVc *vc;                // its VC (NULL when outside or refused)
    const JModel *model;          // JREC_NOT_IMPLIED: the checked model
    JWork used;                   // the solver's work for this VC
    int32_t steps;                // JREC_PROVED: the certificate's steps
    const char *reason;           // OUTSIDE, SOLVER_BUG, UNCERTIFIED, REFUSED, NO_CLAIMS
} JRecord;
typedef void (*JudgeSink)(void *ctx, const JRecord *r);

// ── A6: a model, evaluated exactly ─────────────────────────────────────────────────────────────
// sum(coef * x) at `x`, or false when it does not fit 512 bits.
static bool jm_sum(const JCons *c, const KInt *x, int32_t nvars, KInt *out) {
    KInt s; kint_set(&s, 0);
    for (int32_t i = 0; i < c->nt; i++) {
        if (c->t[i].var < 0 || c->t[i].var >= nvars) return false;
        KInt p; if (!kint_mul(&p, &c->t[i].coef, &x[c->t[i].var]) || !kint_add(&s, &s, &p)) return false;
    }
    *out = s; return true;
}
static int kint_cmp(const KInt *a, const KInt *b) {
    if (a->neg != b->neg) return a->neg ? -1 : 1;
    int c = kmag_cmp(a, b);
    return a->neg ? -c : c;
}
// 1 holds, 0 does not, -1 cannot be evaluated
static int jm_holds(const JCons *c, const KInt *x, int32_t nvars) {
    KInt s; if (!jm_sum(c, x, nvars, &s)) return -1;
    int r = kint_cmp(&s, &c->rhs);
    return c->op == J_LE ? r <= 0 : c->op == J_EQ ? r == 0 : r != 0;
}
// Does model `md` satisfy every hypothesis of `v` and g, the goal's negation?
static bool jm_model_ok(const JVc *v, const JModel *md, const char **why) {
    if (md->n != v->nvars) { *why = "a model without one value per variable"; return false; }
    for (int32_t i = 0; i < md->n; i++) if (!kint_canon(&md->val[i])) { *why = "a model value that is not canonical"; return false; }
    for (int32_t h = 0; h < v->nhyps; h++) {
        const JHyp *y = &v->hyps[h];
        int r;
        if (!y->disj) r = jm_holds(&y->cons, md->val, v->nvars);
        else {
            r = 0;
            for (int32_t d = 0; d < y->ndis && r != 1; d++) {
                int all = 1;
                for (int32_t c = 0; c < y->dis[d].nc && all == 1; c++) all = jm_holds(&y->dis[d].c[c], md->val, v->nvars);
                if (all < 0) { r = -1; break; }
                if (all == 1) r = 1;
            }
        }
        if (r < 0) { *why = "a model the judge cannot evaluate in 512 bits"; return false; }
        if (r == 0) { *why = "a model a hypothesis is false at"; return false; }
    }
    int g = jm_holds(&v->goal, md->val, v->nvars);       // g holds where the goal does not
    if (g < 0) { *why = "a model the judge cannot evaluate in 512 bits"; return false; }
    if (g == 1) { *why = "a model at which the goal holds"; return false; }
    return true;
}
static int32_t jcert_steps(const JSteps *b) {
    int32_t n = 0;
    for (int32_t i = 0; b && b->s && i < b->n; i++) {
        n++;
        for (int32_t k = 0; b->s[i].br && k < b->s[i].nbr; k++) n += jcert_steps(&b->s[i].br[k]);
    }
    return n;
}

// One VC decided: the solver's answer, checked. `kbuf` is the kernel's scratch, reused per VC.
static JRecordKind jj_decide(const JudgeCalls *calls, const JudgeLimits *lim, const JVc *vc,
                             char *kbuf, size_t kcap, JRecord *r, JModel *mout, char *why, size_t whycap) {
    if (!calls || !calls->solve) { r->reason = "no solver"; return JREC_BUDGET; }
    JAnswer a; memset(&a, 0, sizeof a);
    if (!calls->solve(calls->ctx, vc, lim->lim, &a)) { snprintf(why, whycap, "the solver failed"); r->reason = why; return JREC_SOLVER_BUG; }
    r->used = a.used;
    switch (a.k) {
        case J_ANS_CERT: {
            JMem km = jmem_over(kbuf, kcap);
            if (!jkernel_check(&km, vc, &a.cert, why, whycap)) { r->reason = why; return JREC_SOLVER_BUG; }
            r->steps = jcert_steps(&a.cert.body);
            return JREC_PROVED;
        }
        case J_ANS_MODEL: {
            const char *w = NULL;
            if (!jm_model_ok(vc, &a.model, &w)) { r->reason = w; return JREC_SOLVER_BUG; }
            *mout = a.model; r->model = mout;                 // its values: the solver's until its next call
            return JREC_NOT_IMPLIED;
        }
        case J_ANS_BUDGET: return JREC_BUDGET;
        case J_ANS_CAPACITY: return JREC_CAPACITY;
    }
    r->reason = "an answer of no known kind";
    return JREC_SOLVER_BUG;
}

// The module's records, to `sink`. False with `err` when it cannot run at all (its region is full).
static bool judge_module(IrFunc *mod, const JudgeLimits *lim, const JudgeCalls *calls,
                         JudgeSink sink, void *sink_ctx, JMem *region, char *err, size_t errcap) {
    if (err && errcap) err[0] = 0;
    // the region: the claims and the records' VCs at its start, then two scratch halves (the
    // generator's and the kernel's) carved once
    size_t room = (size_t)(region->end - region->cur);
    size_t kcap = room / 4, gcap = room / 4;
    char *kbuf = JMEM_NEW(region, char, kcap), *gbuf = JMEM_NEW(region, char, gcap);
    if (!kbuf || !gbuf) { snprintf(err, errcap, "the judge's region is full"); return false; }
    char why[256];
    // 1. the claims
    JClaim *cl = NULL; int32_t ncl = 0;
    JRank *rk = NULL; int32_t nrk = 0;                    // ranking claims: owed, never facts
    if (calls && calls->propose) {
        JGenOffer offer = { gbuf, gcap, mod };
        JudgeGen gen = jgen_offer(&offer);
        JClaim *pc = NULL; int32_t pn = 0;
        JRank *pr = NULL; int32_t prn = 0;
        if (!calls->propose(calls->ctx, mod, &gen, &pc, &pn, &pr, &prn)) {
            // a proposer that failed hands over nothing: neither its claims nor its ranks
            JRecord r; memset(&r, 0, sizeof r); r.k = JREC_NO_CLAIMS; r.reason = "the proposer failed: the module is judged with no claims";
            sink(sink_ctx, &r);
        } else {
            if (pn > 0 && pc) {
                if (!(cl = JMEM_NEW(region, JClaim, pn))) { snprintf(err, errcap, "the judge's region is full"); return false; }
                memcpy(cl, pc, sizeof *cl * (size_t)pn); ncl = pn;
            }
            if (prn > 0 && pr) {
                if (!(rk = JMEM_NEW(region, JRank, prn))) { snprintf(err, errcap, "the judge's region is full"); return false; }
                memcpy(rk, pr, sizeof *rk * (size_t)prn); nrk = prn;
            }
        }
    }
    // 2. the claims certified: each VC every claim owes, in every function
    const char *unstable = NULL; char ubuf[400];
    for (IrFunc *f = mod; ncl && f && !unstable; f = f->next) {
        if (f->is_extern || !f->entry) continue;
        char *mark = region->cur;
        JObl *o; int32_t n;
        if (!jgen_func(region, f, mod, cl, ncl, NULL, 0, NULL, JGEN_CLAIMS, &o, &n, why, sizeof why)) {
            snprintf(ubuf, sizeof ubuf, "the generator refuses the claims in %.*s: %s", (int)f->name->length, f->name->name, why);
            unstable = ubuf; break;
        }
        for (int32_t i = 0; i < n && !unstable; i++) {
            JRecord r; memset(&r, 0, sizeof r);
            r.fn = f; r.id = &o[i].vc.id; r.at = o[i].at; r.blk = o[i].blk; r.idx = o[i].idx; r.vc = &o[i].vc;
            JModel md;
            r.k = jj_decide(calls, lim, &o[i].vc, kbuf, kcap, &r, &md, why, sizeof why);
            sink(sink_ctx, &r);                               // a claim's VC is a record too
            if (r.k != JREC_PROVED) {
                snprintf(ubuf, sizeof ubuf, "the claim VC %s %s %s %s is not certified", o[i].vc.id.fn, o[i].vc.id.point, o[i].vc.id.kind, o[i].vc.id.ordinal);
                unstable = ubuf;
            }
        }
        region->cur = mark;                                   // the claims' VCs are not kept
    }
    // 3. every function's obligations, its VCs given back once its records are out; the module's
    // call graph built once (a recursion's obligations read it), or each function builds its own
    IrCallGraph cg; memset(&cg, 0, sizeof cg);
    bool have_cg = ir_cg_build(&cg, mod);
    for (IrFunc *f = mod; f; f = f->next) {
        if (f->is_extern || !f->entry) continue;
        char *mark = region->cur;
        JObl *o; int32_t n;
        if (!jgen_func(region, f, mod, unstable ? NULL : cl, unstable ? 0 : ncl, rk, nrk, have_cg ? &cg : NULL, JGEN_ALL,
                       &o, &n, why, sizeof why)) {
            JRecord r; memset(&r, 0, sizeof r); r.k = JREC_REFUSED; r.fn = f;
            char *rs = JMEM_NEW(region, char, strlen(why) + 1);
            if (!rs) { ir_cg_free(&cg); snprintf(err, errcap, "the judge's region is full"); return false; }
            strcpy(rs, why); r.reason = rs;
            sink(sink_ctx, &r);
            region->cur = mark;
            continue;
        }
        for (int32_t i = 0; i < n; i++) {
            if (o[i].claim >= 0) continue;                    // a claim's own VC: certified above
            JRecord r; memset(&r, 0, sizeof r);
            r.fn = f; r.id = &o[i].vc.id; r.at = o[i].at; r.blk = o[i].blk; r.idx = o[i].idx;
            if (o[i].outside) { r.k = JREC_OUTSIDE; r.reason = o[i].outside; sink(sink_ctx, &r); continue; }
            r.vc = &o[i].vc;
            if (unstable) { r.k = JREC_UNCERTIFIED; r.reason = unstable; sink(sink_ctx, &r); continue; }
            JModel md;
            r.k = jj_decide(calls, lim, &o[i].vc, kbuf, kcap, &r, &md, why, sizeof why);
            sink(sink_ctx, &r);
        }
        region->cur = mark;
    }
    ir_cg_free(&cg);
    return true;
}

#endif // LAIN_JUDGE_JUDGE_H
