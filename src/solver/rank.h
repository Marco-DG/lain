#ifndef LAIN_SOLVER_RANK_H
#define LAIN_SOLVER_RANK_H
// rank.h — UNTRUSTED: a linear ranking function for a loop, the candidate the judge checks (PE.5).
//
// A loop terminates if some f(x) = c·x + c0 is non-negative whenever a trip is taken and falls by
// at least 1 on every trip. The judge owes both as ordinary VCs on a RANK claim, and checks them;
// this file only PROPOSES c and c0, so nothing it answers is believed.
//
// Each path round the loop is a conjunction M z <= m over its variables z, among them a pair for
// every quantity f may read: its value at the header (pre) and after the trip (post); a variable
// the loop never writes is its own pair, pre == post, and its terms cancel in the decrease. Farkas'
// lemma turns each implication into linear constraints with fresh multipliers (Podelski and
// Rybalchenko 2004; Bradley, Manna and Sipma 2005, for the bound):
//   decrease:   M z <= m  =>  f(post) - f(pre) <= -1   iff  exists mu >= 0: mu M = a, mu m <= -1
//   bound:      M z <= m  =>  -f(pre) <= 0             iff  exists nu >= 0: nu M = b, nu m <= c0
// with a(v) = sum of -c_i over the pairs whose pre is v and of +c_i over those whose post is v, and
// b(v) = sum of -c_i over the pairs whose pre is v. A multiplier on an equality row is free. Every
// path has its own mu and nu, and c, c0 are shared. The system is linear in (c, c0, mu, nu), and
// lia_solve decides it: a model is a ranking function, and a certificate PROVES there is none
// (none linear; a lexicographic one may exist). Everything is integer and homogeneous but for the
// -1, so a rational solution scales to an integer one, and the integer solver loses nothing.
#include <stdio.h>
#include "solver/lia.h"

typedef enum { LIA_RANK_FOUND, LIA_RANK_NONE, LIA_RANK_STOP, LIA_RANK_UNSUPPORTED } LRankKind;
typedef struct {
    LRankKind k;      // FOUND: coef, c0. NONE: no linear ranking function, with a certificate in
                      // `none` (see `by`). STOP: a budget or capacity ran out.
                      // UNSUPPORTED: a path holds a disjunctive hypothesis (split it into paths).
    JInt *coef;       // coef[i] multiplies pair i's pre value; coef[ncoef] is c0 again (the Farkas model's
                      // layout, which the finder reads up to ncoef inclusive)
    int32_t ncoef;
    JInt c0;
    JCert none;       // by 0: lia_solve's refutation of the Farkas LP; by 1: of rk_cegis's SAMPLE LP,
                      // a certificate of that LP alone. No caller reads it beyond its presence: the
                      // finder reads `k`, and test_rank.c checks the certificate is there
    uint8_t by;       // the search that answered FOUND or NONE: 0 the Farkas LP, 1 the samples (rk_cegis)
    JWork used;
    JAnswerKind stop;
} LRank;

static void *rk_bump(char **cur, char *end, size_t n) {
    uintptr_t at = ((uintptr_t)*cur + 15u) & ~(uintptr_t)15u;
    if (at > (uintptr_t)end || n > (size_t)((uintptr_t)end - at)) return NULL;
    *cur = (char *)at + n;
    memset((void *)at, 0, n);
    return (void *)at;
}
static void rk_int(JInt *x, int64_t v) { ls_to_j((LSi)v, x); }
static bool rk_nonzero(const JInt *x) { return x->n != 0; }

// The coefficient of VC variable v in a row, 0 when it has none.
static const JInt *rk_coef(const JCons *row, int32_t v) {
    for (int32_t t = 0; t < row->nt; t++) if (row->t[t].var == v) return &row->t[t].coef;
    return NULL;
}

// The Farkas system over the paths with on[k] set, in `lp`, built in [*cur, end). With `delta`, path
// k must fall by at least delta_k, 0 <= delta_k <= 1, one more variable per path (its id in
// dl_out[k], -1 for a path not in play); without it every path falls by at least 1. Variables: c_0..c_{npairs-1},
// c0, then per path its mu rows, its nu rows and (with `delta`) its delta. False: out of room.
// Per path, when set: the path owes no bound and does not fall (its delta is 0), only that the
// function not rise on it. fd_rank_cycle's cut search sets it around one call: under spec 12's cut a
// call that only keeps the measure from rising (rec.le) owes no bound, and a depth passed on unchanged
// (`term(var p, depth)`) bounds nothing there. NULL: every path bounded, as before.
static const uint8_t *rk_unbounded = NULL;
static bool rk_build(const JVc *const *paths, int32_t npaths, const bool *on, const int32_t *pre,
                     const int32_t *post, int32_t npairs, bool delta, int64_t cbound, char **cur, char *end,
                     JVc *lp, int32_t *dl_out) {
    int32_t nv = paths[0]->nvars;
    int32_t nvars = npairs + 1;
    int32_t *mu0 = (int32_t *)rk_bump(cur, end, sizeof(int32_t) * (size_t)npaths);
    int32_t *nu0 = (int32_t *)rk_bump(cur, end, sizeof(int32_t) * (size_t)npaths);
    int32_t *dl  = (int32_t *)rk_bump(cur, end, sizeof(int32_t) * (size_t)npaths);
    if (!mu0 || !nu0 || !dl) return false;
    for (int32_t k = 0; k < npaths; k++) {
        if (!on[k]) { mu0[k] = nu0[k] = dl[k] = -1; continue; }
        mu0[k] = nvars; nvars += paths[k]->nhyps;
        nu0[k] = nvars; nvars += paths[k]->nhyps;
        dl[k] = delta ? nvars++ : -1;
    }
    if (dl_out) for (int32_t k = 0; k < npaths; k++) dl_out[k] = dl[k];
    const char **names = (const char **)rk_bump(cur, end, sizeof(char *) * (size_t)nvars);
    char *nbuf = (char *)rk_bump(cur, end, (size_t)nvars * 24);
    int64_t ncap = cbound > 0 ? 2 * (int64_t)npairs : 0;      // the coefficients' bounds
    for (int32_t k = 0; k < npaths; k++) if (on[k]) ncap += 2 * (int64_t)nv + 4 + 2 * paths[k]->nhyps;
    JHyp *hyps = (JHyp *)rk_bump(cur, end, sizeof(JHyp) * (size_t)(ncap > 0 ? ncap : 1));
    if (!names || !nbuf || !hyps) return false;
    for (int32_t v = 0; v < nvars; v++) {
        char *sn = nbuf + (size_t)v * 24;
        if (v < npairs) snprintf(sn, 24, "c.%d", v);
        else if (v == npairs) snprintf(sn, 24, "c0");
        else {
            int32_t k = 0;
            for (int32_t q = 0; q < npaths; q++) if (on[q] && v >= mu0[q]) k = q;
            if (v == dl[k]) snprintf(sn, 24, "d.%d", k);
            else if (v < nu0[k]) snprintf(sn, 24, "mu.%d.%d", k, v - mu0[k]);
            else snprintf(sn, 24, "nu.%d.%d", k, v - nu0[k]);
        }
        names[v] = sn;
    }
    int32_t nh = 0;
    // a constraint under construction: its terms, ascending by variable
    #define RK_NEW(OP, RHS) do { JHyp *H_ = &hyps[nh]; H_->origin = "rank"; H_->cons.op = (OP);      \
        rk_int(&H_->cons.rhs, (RHS));                                                            \
        H_->cons.t = (JTerm *)rk_bump(cur, end, sizeof(JTerm) * (size_t)(npairs + 2 + tcap));    \
        if (!H_->cons.t) return false; } while (0)
    #define RK_TERM(VAR, COEFP) do { JHyp *H_ = &hyps[nh]; JTerm *T_ = &H_->cons.t[H_->cons.nt++]; \
        T_->var = (VAR); T_->coef = *(COEFP); } while (0)
    // a row is the region's last allocation until the next: once complete, its unused room goes
    // back (RK_NEW reserves for the widest row, and most rows hold one or two terms)
    #define RK_DONE() do { JHyp *H_ = &hyps[nh]; *cur = (char *)(H_->cons.t + (H_->cons.nt ? H_->cons.nt : 1)); nh++; } while (0)
    for (int32_t k = 0; k < npaths; k++) {
        if (!on[k]) continue;
        const JVc *P = paths[k];
        int32_t tcap = P->nhyps;
        for (int pass = 0; pass < 2; pass++) {              // 0: the decrease (mu), 1: the bound (nu)
            if (pass == 1 && rk_unbounded && rk_unbounded[k]) break;   // no bound owed on this path
            int32_t base = pass == 0 ? mu0[k] : nu0[k];
            for (int32_t v = 0; v < nv; v++) {               // sum_r M[r][v] y_r  +  sum_i s_i c_i  =  0
                char *undo = *cur;                           // an equation with no term gives its room back:
                RK_NEW(J_EQ, 0);                             // the paths' shared list holds every path's own
                                                             // variables, most absent from any one path
                for (int32_t i = 0; i < npairs; i++) {
                    int64_t sg = (pre[i] == v ? 1 : 0) - (pass == 0 && post[i] == v ? 1 : 0);
                    if (sg) { JInt one; rk_int(&one, sg); RK_TERM(i, &one); }
                }
                for (int32_t r = 0; r < P->nhyps; r++) {
                    const JInt *a = rk_coef(&P->hyps[r].cons, v);
                    if (a && rk_nonzero(a)) RK_TERM(base + r, a);
                }
                if (hyps[nh].cons.nt > 0) RK_DONE();         // an all-zero equation says nothing
                else *cur = undo;
            }
            // decrease: sum_r m_r mu_r <= -1, or + delta_k <= 0;  bound: sum_r m_r nu_r - c0 <= 0
            RK_NEW(J_LE, pass == 0 && !delta ? -1 : 0);
            if (pass == 1) { JInt m1; rk_int(&m1, -1); RK_TERM(npairs, &m1); }
            for (int32_t r = 0; r < P->nhyps; r++)
                if (rk_nonzero(&P->hyps[r].cons.rhs)) RK_TERM(base + r, &P->hyps[r].cons.rhs);
            if (pass == 0 && delta) { JInt one; rk_int(&one, 1); RK_TERM(dl[k], &one); }
            RK_DONE();
            for (int32_t r = 0; r < P->nhyps; r++)          // an inequality row's multiplier: >= 0
                if (P->hyps[r].cons.op == J_LE) {
                    RK_NEW(J_LE, 0);
                    JInt m1; rk_int(&m1, -1); RK_TERM(base + r, &m1);
                    RK_DONE();
                }
        }
        if (delta) {                                         // 0 <= delta_k <= 1
            JInt one, m1; rk_int(&one, 1); rk_int(&m1, -1);
            RK_NEW(J_LE, 0); RK_TERM(dl[k], &m1); RK_DONE();
            RK_NEW(J_LE, rk_unbounded && rk_unbounded[k] ? 0 : 1); RK_TERM(dl[k], &one); RK_DONE();   // unbounded: 0
        }
    }
    if (cbound > 0) {                                        // |c_i| <= cbound (lia_rank_lex_bounded)
        int32_t tcap = 1;
        JInt one, m1; rk_int(&one, 1); rk_int(&m1, -1);
        for (int32_t i = 0; i < npairs; i++) {
            RK_NEW(J_LE, cbound); RK_TERM(i, &one); RK_DONE();
            RK_NEW(J_LE, cbound); RK_TERM(i, &m1); RK_DONE();
        }
    }
    #undef RK_NEW
    #undef RK_TERM
    #undef RK_DONE
    memset(lp, 0, sizeof *lp);
    lp->id = paths[0]->id; lp->vars = names; lp->nvars = nvars; lp->hyps = hyps; lp->nhyps = nh;
    lp->goal.op = J_LE; rk_int(&lp->goal.rhs, -1);            // `0 <= -1`: any model will do
    return true;
}
static bool rk_check_paths(const JVc *const *paths, int32_t npaths) {
    if (npaths < 1) return false;
    for (int32_t k = 0; k < npaths; k++) {
        if (paths[k]->nvars != paths[0]->nvars) return false;
        for (int32_t h = 0; h < paths[k]->nhyps; h++) if (paths[k]->hyps[h].disj) return false;
    }
    return true;
}

// ── by counterexamples (s2, PE.13): when the Farkas system does not fit ───────────────────────
// lia_rank's one LP holds two multipliers per hypothesis of every path, in lia_solve's dense
// tableau. On Gothenburg_v2-1, 12 flattened paths of about 300 facts, that is 7250 variables by 7968
// rows, and it stops on the region at pivot 0, with y's version alone as large. The conditions it
// encodes can be asked path by path instead, and each path's own VC fits. A candidate (c, c0) comes
// from the SAMPLES found so far; then each path gets the fall VC `sum c_i (post_i - pre_i) <= -1` and
// the bound VC `-sum c_i pre_i <= c0` over its facts. A path's model is a transition of that path
// the candidate does not rank, and its pairs' values become a sample:
//   fall   d = post - pre:   sum d_i c_i <= -1
//   bound  p = pre:          -sum p_i c_i - c0 <= 0
// - A round that finds no sample is FOUND.
// - A sample LP with no solution is NONE: the samples are models of the paths, so no linear function
//   ranks the paths either. Its certificate refutes the SAMPLE LP, not the Farkas system (`by` 1).
// - A candidate past 2^66 (what a ranking claim may state), or RK_CG_ROUNDS rounds without either,
//   is STOP. A loop that does not terminate need not make its samples infeasible: under x > 0,
//   x = x + 1 only raises c0, round after round.
// The sample LP is closed under scaling, so lia_cone answers it over the rationals. One budget for
// the whole search; the samples, the LP and one solve region all in the caller's region;
// deterministic (paths in order, samples in the order found).
#define RK_CG_ROUNDS 16
#define RK_CG_MAXS 2048
static LRank rk_cegis(const JVc *const *paths, int32_t npaths, const int32_t *pre, const int32_t *post,
                      int32_t npairs, JWork limit, void *mem, size_t size) {
    LRank R; memset(&R, 0, sizeof R);
    R.k = LIA_RANK_STOP; R.stop = J_ANS_CAPACITY; R.by = 1;
    int32_t nv = paths[0]->nvars, n1 = npairs + 1;
    int64_t want = 2 * (int64_t)npaths * RK_CG_ROUNDS;
    int32_t maxs = want < RK_CG_MAXS ? (int32_t)want : RK_CG_MAXS, ns = 0;
    char *cur = (char *)mem, *end = (char *)mem + size;
    JInt *coef = (JInt *)rk_bump(&cur, end, sizeof(JInt) * (size_t)n1);          // the answer's, kept
    LSi *smp = (LSi *)rk_bump(&cur, end, sizeof(LSi) * (size_t)maxs * (size_t)npairs);
    uint8_t *isb = (uint8_t *)rk_bump(&cur, end, (size_t)maxs);
    JHyp *lh = (JHyp *)rk_bump(&cur, end, sizeof(JHyp) * (size_t)maxs);
    JTerm *lt = (JTerm *)rk_bump(&cur, end, sizeof(JTerm) * (size_t)maxs * (size_t)n1);
    const char **ln = (const char **)rk_bump(&cur, end, sizeof(char *) * (size_t)n1);
    char *lnb = (char *)rk_bump(&cur, end, (size_t)n1 * 24);
    LSi *cand = (LSi *)rk_bump(&cur, end, sizeof(LSi) * (size_t)n1);
    LSi *acc = (LSi *)rk_bump(&cur, end, sizeof(LSi) * (size_t)(nv > 0 ? nv : 1));
    JTerm *gt = (JTerm *)rk_bump(&cur, end, sizeof(JTerm) * (size_t)(nv > 0 ? nv : 1));
    char *wrk = (char *)(((uintptr_t)cur + 15u) & ~(uintptr_t)15u);
    if (!coef || !smp || !isb || !lh || !lt || !ln || !lnb || !cand || !acc || !gt || wrk + 4096 > end) return R;
    size_t wsz = (size_t)(end - wrk);
    for (int32_t i = 0; i < n1; i++) { snprintf(lnb + (size_t)i * 24, 24, i < npairs ? "c.%d" : "c0", i); ln[i] = lnb + (size_t)i * 24; }
    LSol N; memset(&N, 0, sizeof N);                         // checked arithmetic only
    const LSi big = (LSi)1 << 66;
    #define RK_CG_LEFT(W) do { (W).pivot = limit.pivot > R.used.pivot ? limit.pivot - R.used.pivot : 0;              \
        (W).node = limit.node > R.used.node ? limit.node - R.used.node : 0;                                           \
        (W).leaf = limit.leaf > R.used.leaf ? limit.leaf - R.used.leaf : 0; } while (0)
    #define RK_CG_USE(A) do { R.used.pivot += (A).used.pivot; R.used.node += (A).used.node; R.used.leaf += (A).used.leaf; } while (0)
    for (int32_t round = 0; round < RK_CG_ROUNDS; round++) {
        // the candidate: the sample LP over c_0 .. c_{n-1}, c0
        int32_t nh = 0, tu = 0;
        for (int32_t k = 0; k < ns; k++) {
            JHyp *h = &lh[nh++]; memset(h, 0, sizeof *h);
            h->origin = isb[k] ? "sample bound" : "sample fall"; h->cons.op = J_LE; h->cons.t = &lt[tu];
            for (int32_t i = 0; i < npairs; i++) {
                LSi a = smp[(size_t)k * (size_t)npairs + (size_t)i];
                if (a) { lt[tu].var = i; ls_to_j(isb[k] ? ls_sub(&N, 0, a) : a, &lt[tu].coef); tu++; h->cons.nt++; }
            }
            if (isb[k]) { lt[tu].var = npairs; ls_to_j(-1, &lt[tu].coef); tu++; h->cons.nt++; }
            ls_to_j(isb[k] ? 0 : -1, &h->cons.rhs);
        }
        if (N.cap) return R;
        JVc lp; memset(&lp, 0, sizeof lp);
        lp.id = paths[0]->id; lp.vars = ln; lp.nvars = n1; lp.hyps = lh; lp.nhyps = nh;
        lp.goal.op = J_LE; ls_to_j(-1, &lp.goal.rhs);
        JWork left; RK_CG_LEFT(left);
        JAnswer A = lia_cone(&lp, left, wrk, wsz);
        RK_CG_USE(A);
        if (A.k == J_ANS_CERT) { R.k = LIA_RANK_NONE; R.none = A.cert; return R; }
        if (A.k != J_ANS_MODEL) { R.stop = A.k; return R; }
        for (int32_t i = 0; i < n1; i++) {
            cand[i] = ls_from_j(&N, &A.model.val[i]);
            if (N.cap || cand[i] > big || cand[i] < -big) return R;
        }
        // the candidate on each path: its fall, then its bound
        int32_t added = 0;
        for (int32_t k = 0; k < npaths; k++)
            for (int what = 0; what < 2; what++) {
                memset(acc, 0, sizeof(LSi) * (size_t)nv);
                for (int32_t i = 0; i < npairs; i++) {
                    if (what == 0) acc[post[i]] = ls_add(&N, acc[post[i]], cand[i]);
                    acc[pre[i]] = ls_sub(&N, acc[pre[i]], cand[i]);
                }
                int32_t m = 0;
                for (int32_t v = 0; v < nv; v++) if (acc[v]) { gt[m].var = v; ls_to_j(acc[v], &gt[m].coef); m++; }
                if (N.cap) return R;
                JVc q = *paths[k];
                q.goal.t = gt; q.goal.nt = m; q.goal.op = J_LE; ls_to_j(what == 0 ? -1 : cand[npairs], &q.goal.rhs);
                RK_CG_LEFT(left);
                JAnswer B = lia_solve(&q, left, wrk, wsz);
                RK_CG_USE(B);
                if (B.k == J_ANS_CERT) continue;
                if (B.k != J_ANS_MODEL) { R.stop = B.k; return R; }
                if (ns == maxs) { R.stop = J_ANS_BUDGET; return R; }
                for (int32_t i = 0; i < npairs; i++) {
                    LSi a = ls_from_j(&N, &B.model.val[pre[i]]), b = ls_from_j(&N, &B.model.val[post[i]]);
                    smp[(size_t)ns * (size_t)npairs + (size_t)i] = what == 0 ? ls_sub(&N, b, a) : a;
                }
                if (N.cap) return R;
                isb[ns++] = (uint8_t)what; added++;
            }
        if (!added) {
            for (int32_t i = 0; i < n1; i++) ls_to_j(cand[i], &coef[i]);
            R.k = LIA_RANK_FOUND; R.coef = coef; R.ncoef = npairs; ls_to_j(cand[npairs], &R.c0);
            return R;
        }
    }
    #undef RK_CG_LEFT
    #undef RK_CG_USE
    R.stop = J_ANS_BUDGET;
    return R;
}

// `paths` share one variable list (paths[0]'s); pre[i] and post[i] index it. The Farkas LP first;
// when it stops on CAPACITY (it did not fit the region, or its numbers left 128 bits), rk_cegis
// over the same paths in the whole region, its budget what the first attempt left.
static LRank lia_rank(const JVc *const *paths, int32_t npaths, const int32_t *pre, const int32_t *post,
                      int32_t npairs, JWork limit, void *mem, size_t size) {
    LRank R; memset(&R, 0, sizeof R);
    if (npairs < 1 || !rk_check_paths(paths, npaths) || npaths > 4096) { R.k = LIA_RANK_UNSUPPORTED; return R; }
    char *cur = (char *)mem, *end = (char *)mem + size / 4;    // the system; the solver gets the rest
    bool on[4096];
    for (int32_t k = 0; k < npaths; k++) on[k] = true;
    JVc lp;
    if (!rk_build(paths, npaths, on, pre, post, npairs, false, 0, &cur, end, &lp, NULL)) {
        R.k = LIA_RANK_STOP; R.stop = J_ANS_CAPACITY;
    } else {
        JAnswer A = lia_solve(&lp, limit, cur, (size_t)((char *)mem + size - cur));
        R.used = A.used;
        if (A.k == J_ANS_MODEL) {
            R.k = LIA_RANK_FOUND; R.coef = A.model.val; R.ncoef = npairs; R.c0 = A.model.val[npairs];
        } else if (A.k == J_ANS_CERT) {
            R.k = LIA_RANK_NONE; R.none = A.cert;
        } else { R.k = LIA_RANK_STOP; R.stop = A.k; }
    }
    if (R.k != LIA_RANK_STOP || R.stop != J_ANS_CAPACITY) return R;
    JWork left = { limit.pivot > R.used.pivot ? limit.pivot - R.used.pivot : 0,
                   limit.node > R.used.node ? limit.node - R.used.node : 0,
                   limit.leaf > R.used.leaf ? limit.leaf - R.used.leaf : 0 };
    LRank C = rk_cegis(paths, npaths, pre, post, npairs, left, mem, size);
    C.used.pivot += R.used.pivot; C.used.node += R.used.node; C.used.leaf += R.used.leaf;
    return C;
}

// ── lexicographic ranking functions (Alias, Darte, Feautrier and Gonnord 2010) ─────────────────
// A tuple (f_1, ..., f_n): every f_j is non-negative on every path still in play and never rises on
// one, and each path is strictly decreased (by at least 1) by exactly one component, `fall[k]`,
// after which the later components are free on it. Each round finds the component that strictly
// decreases the MOST remaining paths (lia_maximize over sum delta_k), and those paths leave the
// game. A round that decreases none ends it: no lexicographic ranking of this kind. The judge then
// owes, per path k, f_j(post) <= f_j(pre) for j < fall[k], f_fall(post) <= f_fall(pre) - 1 and
// f_fall(pre) >= 0: plain LIA VCs, since the component is named, not chosen.
#define LIA_RANK_MAXLEX 8
typedef struct {
    LRankKind k;                     // FOUND, NONE (no tuple: some round decreased no path), STOP, UNSUPPORTED
    int32_t ncomp;
    JInt *coef[LIA_RANK_MAXLEX];     // component j: coef[j][i] multiplies pair i's pre value
    JInt c0[LIA_RANK_MAXLEX];
    int32_t *fall;                   // per path: the component that strictly decreases it
    JWork used;
    JAnswerKind stop;
} LRankLex;

// ...with every coefficient within [-cbound, cbound] (0: unbounded). Under bounded types a
// lexicographic ranking also exists LINEARIZED by powers of two (CookSeeZuleger Fig7b's <y, z, x> as
// about 2^63*y + 2^32*z + x), and the first round, maximizing the paths it lowers, finds that one:
// past what a ranking claim may state. Bounded, the rounds find the tuple.
static LRankLex lia_rank_lex_bounded(const JVc *const *paths, int32_t npaths, const int32_t *pre, const int32_t *post,
                                     int32_t npairs, int64_t cbound, JWork limit, void *mem, size_t size) {
    LRankLex R; memset(&R, 0, sizeof R);
    if (npairs < 1 || !rk_check_paths(paths, npaths) || npaths > 4096) { R.k = LIA_RANK_UNSUPPORTED; return R; }
    // the answer's memory first, kept across rounds; each round's system and solver the rest
    char *keep = (char *)mem, *kend = (char *)mem + size / 16;
    R.fall = (int32_t *)rk_bump(&keep, kend, sizeof(int32_t) * (size_t)npaths);
    for (int32_t j = 0; j < LIA_RANK_MAXLEX && R.fall; j++)
        if (!(R.coef[j] = (JInt *)rk_bump(&keep, kend, sizeof(JInt) * (size_t)npairs))) R.fall = NULL;
    if (!R.fall) { R.k = LIA_RANK_STOP; R.stop = J_ANS_CAPACITY; return R; }
    bool on[4096]; int32_t left = npaths;
    for (int32_t k = 0; k < npaths; k++) { on[k] = true; R.fall[k] = -1; }
    char *rbase = keep;
    while (left > 0) {
        if (R.ncomp == LIA_RANK_MAXLEX) { R.k = LIA_RANK_STOP; R.stop = J_ANS_BUDGET; return R; }
        char *cur = rbase, *end = rbase + ((char *)mem + size - rbase) / 4;
        JVc lp;
        int32_t *dl = (int32_t *)rk_bump(&cur, end, sizeof(int32_t) * (size_t)npaths);
        if (!dl || !rk_build(paths, npaths, on, pre, post, npairs, true, cbound, &cur, end, &lp, dl)) {
            R.k = LIA_RANK_STOP; R.stop = J_ANS_CAPACITY; return R;
        }
        JTerm *obj = (JTerm *)rk_bump(&cur, end, sizeof(JTerm) * (size_t)left);
        if (!obj) { R.k = LIA_RANK_STOP; R.stop = J_ANS_CAPACITY; return R; }
        int32_t nt = 0;                                       // maximise sum delta_k, ascending ids
        for (int32_t k = 0; k < npaths; k++) if (dl[k] >= 0) { obj[nt].var = dl[k]; rk_int(&obj[nt].coef, 1); nt++; }
        JWork rest = { limit.pivot - R.used.pivot, limit.node - R.used.node, limit.leaf - R.used.leaf };
        LMax X = lia_maximize(&lp, obj, nt, rest, cur, (size_t)((char *)mem + size - cur));
        R.used.pivot += X.used.pivot; R.used.node += X.used.node; R.used.leaf += X.used.leaf;
        if (X.k == LIA_MAX_STOP) { R.k = LIA_RANK_STOP; R.stop = X.stop; return R; }
        if (X.k != LIA_MAX_BOUND || X.b.n == 0) { R.k = LIA_RANK_NONE; return R; }   // no path falls
        int32_t j = R.ncomp++;
        for (int32_t i = 0; i < npairs; i++) R.coef[j][i] = X.model.val[i];
        R.c0[j] = X.model.val[npairs];
        for (int32_t k = 0; k < npaths; k++)
            if (on[k] && X.model.val[dl[k]].n != 0) { R.fall[k] = j; on[k] = false; left--; }
    }
    R.k = LIA_RANK_FOUND;
    return R;
}
static LRankLex lia_rank_lex(const JVc *const *paths, int32_t npaths, const int32_t *pre, const int32_t *post,
                             int32_t npairs, JWork limit, void *mem, size_t size) {
    return lia_rank_lex_bounded(paths, npaths, pre, post, npairs, 0, limit, mem, size);
}

// ── PE.7: two-variable relations a loop keeps (delta synthesis) ───────────────────────────────
// A running total s that rises by at most b_k on path k, beside a counter j that changes by a
// nonzero constant d_k of ONE sign on every path, keeps L*s + c*j from rising when
//     L*b_k + c*d_k <= 0  on every path k.
// With j rising (d_k > 0) that is c = -ceil(max_k L*b_k/d_k), `s - 30*i` for s += a[i] with a[i] in
// [5, 30]; with j falling (d_k < 0, a measure) c = +ceil(max_k L*b_k/|d_k|), `365*year + days` for
// the Zune loop, where days falls by 365 or 366 while year rises by 1. L is the least |d_k|, which
// makes both exact when the counter's step is one constant. A lower bound is the same with -s and
// the most s can FALL. Each answer is a candidate shape only: the finder computes its bound at the
// loop's entry and Houdini keeps it only if the judge's VCs show it inductive, so nothing here is
// believed and a wrong delta costs a refused candidate.
typedef struct { int32_t t, j; int64_t ct, cj; } LRel;     // ct*x_t + cj*x_j does not rise (ct may be < 0)

// The least and greatest value of post - pre on one path, by lia_maximize; false when either is
// unbounded, the path is empty, or a budget ran out.
static bool rk_delta(const JVc *P, int32_t pre, int32_t post, JWork limit, void *mem, size_t size,
                     int64_t *lo, int64_t *hi) {
    if (pre == post) { *lo = *hi = 0; return true; }
    for (int side = 0; side < 2; side++) {                    // 0: max(post - pre), 1: max(pre - post)
        JTerm t[2]; int32_t a = pre < post ? pre : post, b = pre < post ? post : pre;
        int64_t ca = (a == post) == (side == 0) ? 1 : -1;
        t[0].var = a; rk_int(&t[0].coef, ca);
        t[1].var = b; rk_int(&t[1].coef, -ca);
        LMax X = lia_maximize(P, t, 2, limit, mem, size);
        if (X.k != LIA_MAX_BOUND) return false;
        LSol N; memset(&N, 0, sizeof N);
        LSi v = ls_from_j(&N, &X.b);
        if (N.cap || v > INT64_MAX / 4 || v < INT64_MIN / 4) return false;
        if (side == 0) *hi = (int64_t)v; else *lo = -(int64_t)v;
    }
    return true;
}
static int64_t rk_ceil_div(int64_t a, int64_t b) { return a / b + ((a % b != 0) && ((a > 0) == (b > 0))); }   // b > 0

// rk_delta on the facts CONNECTED to the pair (pre, post) only: a fact sharing no variable, directly
// or through others, with the pair cannot bound post - pre, and a path carrying a loop's every claim
// made each maximization pay for all of them (Gothenburg's relations: 806 maximizations, 114 s). A
// disjunctive fact is left out, as lia_maximize leaves it. Exact for a satisfiable path; for one
// whose other facts contradict, the slice answers where the whole would be empty (a candidate only).
static bool rk_delta_sliced(const JVc *P, int32_t pre, int32_t post, JWork limit, void *mem, size_t size,
                            int64_t *lo, int64_t *hi) {
    if (pre == post) { *lo = *hi = 0; return true; }
    uint8_t *on = calloc((size_t)(P->nvars > 0 ? P->nvars : 1), 1);
    JHyp *h = malloc(sizeof *h * (size_t)(P->nhyps > 0 ? P->nhyps : 1));
    uint8_t *took = calloc((size_t)(P->nhyps > 0 ? P->nhyps : 1), 1);
    if (!on || !h || !took) { free(on); free(h); free(took); return rk_delta(P, pre, post, limit, mem, size, lo, hi); }
    on[pre] = on[post] = 1;
    for (bool grew = true; grew; ) {
        grew = false;
        for (int32_t r = 0; r < P->nhyps; r++) {
            if (took[r] || P->hyps[r].disj) continue;
            bool touch = false;
            for (int32_t t = 0; t < P->hyps[r].cons.nt && !touch; t++) touch = on[P->hyps[r].cons.t[t].var];
            if (!touch) continue;
            took[r] = 1; grew = true;
            for (int32_t t = 0; t < P->hyps[r].cons.nt; t++) on[P->hyps[r].cons.t[t].var] = 1;
        }
    }
    int32_t m = 0;
    for (int32_t r = 0; r < P->nhyps; r++) if (took[r]) h[m++] = P->hyps[r];
    JVc q = *P; q.hyps = h; q.nhyps = m;
    bool ok = rk_delta(&q, pre, post, limit, mem, size, lo, hi);
    free(on); free(h); free(took);
    return ok;
}

// Every (t, j) relation of this kind over the pairs of a loop's back-edge paths (sharing one
// variable list, as lia_rank's), at most `cap` into `out`. The deltas are asked once per pair and path.
static int32_t lia_relations(const JVc *const *paths, int32_t npaths, const int32_t *pre, const int32_t *post,
                             int32_t npairs, JWork limit, void *mem, size_t size, LRel *out, int32_t cap) {
    if (npairs < 2 || npaths < 1 || npaths > 64 || npairs > 64 || !rk_check_paths(paths, npaths)) return 0;
    int64_t lo[64][64], hi[64][64]; bool ok[64];
    for (int32_t i = 0; i < npairs; i++) {
        ok[i] = true;
        for (int32_t k = 0; k < npaths && ok[i]; k++)
            ok[i] = rk_delta_sliced(paths[k], pre[i], post[i], limit, mem, size, &lo[i][k], &hi[i][k]);
    }
    int32_t n = 0;
    for (int32_t j = 0; j < npairs; j++) {
        if (!ok[j]) continue;
        int sgn = 0; int64_t L = 0; bool exact = true;              // j: a nonzero constant of one sign
        for (int32_t k = 0; k < npaths && exact; k++) {
            if (lo[j][k] != hi[j][k] || lo[j][k] == 0) { exact = false; break; }
            int sk = lo[j][k] > 0 ? 1 : -1;
            if (sgn && sk != sgn) exact = false;
            sgn = sk;
            int64_t m = lo[j][k] > 0 ? lo[j][k] : -lo[j][k];
            if (!L || m < L) L = m;
        }
        if (!exact || L > (1 << 20)) continue;
        for (int32_t t = 0; t < npairs; t++) {
            if (t == j || !ok[t]) continue;
            for (int up = 1; up >= 0; up--) {                       // an upper bound on t, then a lower one
                int64_t worst = INT64_MIN;                          // max_k ceil(L * b_k / |d_k|), b_k the rise of (+/-)t
                for (int32_t k = 0; k < npaths; k++) {
                    int64_t b = up ? hi[t][k] : -lo[t][k], d = lo[j][k] > 0 ? lo[j][k] : -lo[j][k];
                    if (b > INT64_MAX / L || b < INT64_MIN / L) { worst = INT64_MAX; break; }
                    int64_t q = rk_ceil_div(L * b, d);
                    if (q > worst) worst = q;
                }
                if (worst <= 0 || worst == INT64_MAX || worst > (1 << 20)) continue;   // t never rises (or no use)
                if (n == cap) return n;
                out[n].t = t; out[n].j = j;
                out[n].ct = up ? L : -L;
                out[n].cj = sgn > 0 ? -worst : worst;
                n++;
            }
        }
    }
    return n;
}

#endif // LAIN_SOLVER_RANK_H
