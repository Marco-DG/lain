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
    LRankKind k;      // FOUND: coef, c0. NONE: no linear ranking function (lia_solve's certificate
                      // of that, over the LP, is in `none`). STOP: a budget or capacity ran out.
                      // UNSUPPORTED: a path holds a disjunctive hypothesis (split it into paths).
    JInt *coef;       // coef[i] multiplies pair i's pre value
    int32_t ncoef;
    JInt c0;
    JCert none;
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
static bool rk_build(const JVc *const *paths, int32_t npaths, const bool *on, const int32_t *pre,
                     const int32_t *post, int32_t npairs, bool delta, char **cur, char *end,
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
    int64_t ncap = 0;
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
    for (int32_t k = 0; k < npaths; k++) {
        if (!on[k]) continue;
        const JVc *P = paths[k];
        int32_t tcap = P->nhyps;
        for (int pass = 0; pass < 2; pass++) {              // 0: the decrease (mu), 1: the bound (nu)
            int32_t base = pass == 0 ? mu0[k] : nu0[k];
            for (int32_t v = 0; v < nv; v++) {               // sum_r M[r][v] y_r  +  sum_i s_i c_i  =  0
                RK_NEW(J_EQ, 0);
                for (int32_t i = 0; i < npairs; i++) {
                    int64_t sg = (pre[i] == v ? 1 : 0) - (pass == 0 && post[i] == v ? 1 : 0);
                    if (sg) { JInt one; rk_int(&one, sg); RK_TERM(i, &one); }
                }
                for (int32_t r = 0; r < P->nhyps; r++) {
                    const JInt *a = rk_coef(&P->hyps[r].cons, v);
                    if (a && rk_nonzero(a)) RK_TERM(base + r, a);
                }
                if (hyps[nh].cons.nt > 0) nh++;              // an all-zero equation says nothing
            }
            // decrease: sum_r m_r mu_r <= -1, or + delta_k <= 0;  bound: sum_r m_r nu_r - c0 <= 0
            RK_NEW(J_LE, pass == 0 && !delta ? -1 : 0);
            if (pass == 1) { JInt m1; rk_int(&m1, -1); RK_TERM(npairs, &m1); }
            for (int32_t r = 0; r < P->nhyps; r++)
                if (rk_nonzero(&P->hyps[r].cons.rhs)) RK_TERM(base + r, &P->hyps[r].cons.rhs);
            if (pass == 0 && delta) { JInt one; rk_int(&one, 1); RK_TERM(dl[k], &one); }
            nh++;
            for (int32_t r = 0; r < P->nhyps; r++)          // an inequality row's multiplier: >= 0
                if (P->hyps[r].cons.op == J_LE) {
                    RK_NEW(J_LE, 0);
                    JInt m1; rk_int(&m1, -1); RK_TERM(base + r, &m1);
                    nh++;
                }
        }
        if (delta) {                                         // 0 <= delta_k <= 1
            JInt one, m1; rk_int(&one, 1); rk_int(&m1, -1);
            RK_NEW(J_LE, 0); RK_TERM(dl[k], &m1); nh++;
            RK_NEW(J_LE, 1); RK_TERM(dl[k], &one); nh++;
        }
    }
    #undef RK_NEW
    #undef RK_TERM
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

// `paths` share one variable list (paths[0]'s); pre[i] and post[i] index it.
static LRank lia_rank(const JVc *const *paths, int32_t npaths, const int32_t *pre, const int32_t *post,
                      int32_t npairs, JWork limit, void *mem, size_t size) {
    LRank R; memset(&R, 0, sizeof R);
    if (npairs < 1 || !rk_check_paths(paths, npaths) || npaths > 4096) { R.k = LIA_RANK_UNSUPPORTED; return R; }
    char *cur = (char *)mem, *end = (char *)mem + size / 4;    // the system; the solver gets the rest
    bool on[4096];
    for (int32_t k = 0; k < npaths; k++) on[k] = true;
    JVc lp;
    if (!rk_build(paths, npaths, on, pre, post, npairs, false, &cur, end, &lp, NULL)) {
        R.k = LIA_RANK_STOP; R.stop = J_ANS_CAPACITY; return R;
    }
    JAnswer A = lia_solve(&lp, limit, cur, (size_t)((char *)mem + size - cur));
    R.used = A.used;
    if (A.k == J_ANS_MODEL) {
        R.k = LIA_RANK_FOUND; R.coef = A.model.val; R.ncoef = npairs; R.c0 = A.model.val[npairs];
    } else if (A.k == J_ANS_CERT) {
        R.k = LIA_RANK_NONE; R.none = A.cert;
    } else { R.k = LIA_RANK_STOP; R.stop = A.k; }
    return R;
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

static LRankLex lia_rank_lex(const JVc *const *paths, int32_t npaths, const int32_t *pre, const int32_t *post,
                             int32_t npairs, JWork limit, void *mem, size_t size) {
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
        if (!dl || !rk_build(paths, npaths, on, pre, post, npairs, true, &cur, end, &lp, dl)) {
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

#endif // LAIN_SOLVER_RANK_H
