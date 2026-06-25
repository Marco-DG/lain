#ifndef SEMA_OMEGA_H
#define SEMA_OMEGA_H

/*
 * Fourier-Motzkin linear arithmetic prover for array bounds.
 *
 * Given a VRA context (RangeTable) and two expressions, proves:
 *   index_expr < bound_expr
 *
 * Method (proof by contradiction):
 *   1. Decompose both expressions into linear forms over named variables.
 *      x.len references are mapped to the synthetic __len_x VRA entry.
 *   2. Form the negated query: bound - index ≤ 0  (i.e. index ≥ bound).
 *   3. Extract all VRA range / difference constraints for those variables.
 *   4. Run Fourier-Motzkin variable elimination.
 *   5. If the resulting system contains "0 ≤ negative_constant" → UNSAT
 *      → the negation is impossible → index < bound is always true → safe.
 *
 * No external dependencies.  Self-contained ~250 LOC, polynomial time for
 * the small systems (2-6 vars, 5-15 ineqs) typical in slice manipulation.
 */

#include "../ast.h"
#include "ranges.h"
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* ── limits ──────────────────────────────────────────────────────────────── */

#define OMEGA_MAX_VARS  12
#define OMEGA_FM_BUF    80   /* max ineqs in the working FM buffer           */

/* ── data structures ─────────────────────────────────────────────────────── */

/* Inequality:  Σ coeff[i]*var[i]  ≤  rhs  */
typedef struct {
    int64_t coeff[OMEGA_MAX_VARS];
    int64_t rhs;
} OmegaIneq;

typedef struct {
    int     n_vars;
    int     vlen [OMEGA_MAX_VARS];
    char    vname[OMEGA_MAX_VARS][72]; /* enough for "__len_" + 64-char id   */

    int      n_ineqs;
    OmegaIneq ineqs[OMEGA_FM_BUF];
} OmegaSystem;

/* Linear form:  Σ coeff[i]*var[i]  +  const_term  */
typedef struct {
    int64_t coeff[OMEGA_MAX_VARS];
    int64_t const_term;
} OmegaLin;

/* ── variable management ─────────────────────────────────────────────────── */

static int omega_find_var(OmegaSystem *s, const char *n, int nl) {
    for (int i = 0; i < s->n_vars; i++)
        if (s->vlen[i] == nl && memcmp(s->vname[i], n, nl) == 0) return i;
    return -1;
}

static int omega_add_var(OmegaSystem *s, const char *n, int nl) {
    int idx = omega_find_var(s, n, nl);
    if (idx >= 0) return idx;
    if (s->n_vars >= OMEGA_MAX_VARS) return -1;
    idx = s->n_vars++;
    s->vlen[idx] = nl;
    int cp = nl < 71 ? nl : 71;
    memcpy(s->vname[idx], n, cp);
    s->vname[idx][cp] = '\0';
    return idx;
}

/* Register an Id* directly. */
static int omega_add_id(OmegaSystem *s, Id *id) {
    return id ? omega_add_var(s, id->name, (int)id->length) : -1;
}

/* Register __len_REF for a `.len` member reference. */
static int omega_add_len_ref(OmegaSystem *s, Id *ref) {
    if (!ref) return -1;
    char k[72];
    int  kl = 6 + (int)ref->length;
    if (kl > 71) return -1;
    memcpy(k, "__len_", 6);
    memcpy(k + 6, ref->name, ref->length);
    return omega_add_var(s, k, kl);
}

/* ── expression → linear form ────────────────────────────────────────────── */

/*
 * Recursively decomposes `e` into `lin`, multiplied by `scale`.
 * Works on both body expressions (resolved) and type-annotation size_expr
 * nodes (unresolved) because we only read raw Id names, never decl/type.
 * Returns false if `e` is non-linear (e.g. var*var) or unsupported.
 */
static bool omega_decompose(OmegaSystem *s, Expr *e, OmegaLin *lin,
                            int64_t scale) {
    if (!e) return false;
    switch (e->kind) {

    case EXPR_LITERAL:
        lin->const_term += scale * e->as.literal_expr.value;
        return true;

    case EXPR_IDENTIFIER: {
        Id *id = e->as.identifier_expr.id;
        if (!id) return false;
        int idx = omega_add_id(s, id);
        if (idx < 0) return false;
        lin->coeff[idx] += scale;
        return true;
    }

    case EXPR_MEMBER: {
        /* x.len  →  __len_x */
        Expr *tgt = e->as.member_expr.target;
        Id   *mem = e->as.member_expr.member;
        if (!tgt || !mem) return false;
        if (tgt->kind != EXPR_IDENTIFIER) return false;
        if (mem->length != 3 || memcmp(mem->name, "len", 3) != 0) return false;
        Id *ref = tgt->as.identifier_expr.id;
        if (!ref) return false;
        int idx = omega_add_len_ref(s, ref);
        if (idx < 0) return false;
        lin->coeff[idx] += scale;
        return true;
    }

    case EXPR_BINARY: {
        TokenKind op = e->as.binary_expr.op;
        if (op == TOKEN_PLUS)
            return omega_decompose(s, e->as.binary_expr.left,  lin,  scale) &&
                   omega_decompose(s, e->as.binary_expr.right, lin,  scale);
        if (op == TOKEN_MINUS)
            return omega_decompose(s, e->as.binary_expr.left,  lin,  scale) &&
                   omega_decompose(s, e->as.binary_expr.right, lin, -scale);
        /* constant scaling: k * expr  or  expr * k */
        if (op == TOKEN_ASTERISK) {
            Expr *L = e->as.binary_expr.left, *R = e->as.binary_expr.right;
            if (L && L->kind == EXPR_LITERAL)
                return omega_decompose(s, R, lin,
                           sat_mul_i64(scale, L->as.literal_expr.value));
            if (R && R->kind == EXPR_LITERAL)
                return omega_decompose(s, L, lin,
                           sat_mul_i64(scale, R->as.literal_expr.value));
        }
        return false; /* non-linear */
    }

    case EXPR_UNARY:
        if (e->as.unary_expr.op == TOKEN_MINUS)
            return omega_decompose(s, e->as.unary_expr.right, lin, -scale);
        return false;

    default:
        return false;
    }
}

/* ── system construction ─────────────────────────────────────────────────── */

static bool omega_add_ineq(OmegaSystem *s, int64_t *coeff, int64_t rhs) {
    if (s->n_ineqs >= OMEGA_FM_BUF) return false;
    OmegaIneq *q = &s->ineqs[s->n_ineqs++];
    memcpy(q->coeff, coeff, OMEGA_MAX_VARS * sizeof(int64_t));
    q->rhs = rhs;
    return true;
}

/*
 * Extract range and difference constraints from the VRA for variables
 * already registered in `s`.  Unregistered variables are silently skipped
 * (they cannot appear in the query expressions and are irrelevant).
 */
static void omega_extract_vra(OmegaSystem *s, RangeTable *ctx) {
    if (!ctx) return;

    /* Range bounds: var ∈ [lo, hi] → two ineqs */
    for (RangeEntry *re = ctx->head; re; re = re->next) {
        if (!re->range.known) continue;
        int idx = omega_find_var(s, re->var->name, (int)re->var->length);
        if (idx < 0) continue;

        int64_t c[OMEGA_MAX_VARS];
        memset(c, 0, sizeof(c));

        if (re->range.max < INT64_MAX) {
            c[idx] = 1;           /* var ≤ hi */
            omega_add_ineq(s, c, re->range.max);
            memset(c, 0, sizeof(c));
        }
        if (re->range.min > INT64_MIN) {
            c[idx] = -1;          /* -var ≤ -lo  ↔  var ≥ lo */
            omega_add_ineq(s, c, -re->range.min);
            memset(c, 0, sizeof(c));
        }
    }

    /* Difference constraints: v1 - v2 ≤ max_diff */
    for (ConstraintEntry *ce = ctx->constraints; ce; ce = ce->next) {
        int i1 = omega_find_var(s, ce->v1->name, (int)ce->v1->length);
        int i2 = omega_find_var(s, ce->v2->name, (int)ce->v2->length);
        if (i1 < 0 || i2 < 0) continue;

        int64_t c[OMEGA_MAX_VARS];
        memset(c, 0, sizeof(c));
        c[i1] =  1;
        c[i2] = -1;
        omega_add_ineq(s, c, ce->max_diff);
    }
}

/* ── Fourier-Motzkin elimination ─────────────────────────────────────────── */

/*
 * Returns false (UNSAT) if the system has no integer solution.
 * Returns true  (SAT or unknown) otherwise.
 *
 * Eliminates variables one by one.  For each variable v:
 *   - Neutral ineqs (coeff[v]=0) pass through unchanged.
 *   - For each pair (upper U where coeff[v]>0, lower L where coeff[v]<0),
 *     generate:  b*(U without v) + a*(L without v)  ≤  b*rhs_U + a*rhs_L
 *     where a = coeff_U[v] > 0,  b = -coeff_L[v] > 0.
 * A zero-variable ineq  0 ≤ negative  → immediate UNSAT.
 * If the FM buffer overflows, we stop early and conservatively return true.
 */
static bool omega_fm_sat(OmegaSystem *s) {
    int      ni  = s->n_ineqs;
    OmegaIneq buf[OMEGA_FM_BUF];
    memcpy(buf, s->ineqs, ni * sizeof(OmegaIneq));

    for (int v = 0; v < s->n_vars; v++) {
        int      new_ni = 0;
        OmegaIneq newbuf[OMEGA_FM_BUF];

        /* 1. Pass neutral ineqs (coeff[v] == 0) */
        for (int i = 0; i < ni; i++) {
            if (buf[i].coeff[v] != 0) continue;
            if (new_ni >= OMEGA_FM_BUF) goto fm_overflow;
            newbuf[new_ni++] = buf[i];
        }

        /* 2. Cross upper × lower */
        for (int i = 0; i < ni; i++) {
            int64_t ci = buf[i].coeff[v];
            if (ci <= 0) continue;               /* upper bound: coeff > 0 */

            for (int j = 0; j < ni; j++) {
                int64_t cj = buf[j].coeff[v];
                if (cj >= 0) continue;           /* lower bound: coeff < 0 */

                int64_t a = ci;       /* > 0 */
                int64_t b = -cj;      /* > 0 */

                if (new_ni >= OMEGA_FM_BUF) goto fm_overflow;
                OmegaIneq *nq = &newbuf[new_ni++];

                nq->rhs = sat_add_i64(sat_mul_i64(b, buf[i].rhs),
                                      sat_mul_i64(a, buf[j].rhs));

                bool all_zero = true;
                for (int k = 0; k < OMEGA_MAX_VARS; k++) {
                    if (k == v) { nq->coeff[k] = 0; continue; }
                    nq->coeff[k] = sat_add_i64(sat_mul_i64(b, buf[i].coeff[k]),
                                               sat_mul_i64(a, buf[j].coeff[k]));
                    if (nq->coeff[k] != 0) all_zero = false;
                }

                /* Early UNSAT: trivial  0 ≤ negative */
                if (all_zero && nq->rhs < 0) return false;
            }
        }

        memcpy(buf, newbuf, new_ni * sizeof(OmegaIneq));
        ni = new_ni;
        continue;

fm_overflow:
        /* Buffer full: give up on this variable, leave remaining buf as-is */
        break;
    }

    /* Final scan: any constant inequality violated? */
    for (int i = 0; i < ni; i++) {
        bool all_zero = true;
        for (int k = 0; k < OMEGA_MAX_VARS; k++)
            if (buf[i].coeff[k] != 0) { all_zero = false; break; }
        if (all_zero && buf[i].rhs < 0) return false;
    }
    return true; /* sat or gave up — conservative */
}

/* ── public entry points ─────────────────────────────────────────────────── */

/*
 * Try to prove  index_expr >= 0  given the VRA context `ctx`.
 * Equivalent to: the assumption  index_expr <= -1  is UNSAT.
 * Used to rescue the interval-based non-negativity check when the index is
 * a linear expression (e.g. n-i-1) whose interval range includes negatives
 * but which is always ≥ 0 given the loop constraints.
 */
static bool omega_prove_nonneg(RangeTable *ctx, Expr *index_expr) {
    if (!ctx || !index_expr) return false;

    OmegaSystem sys;
    memset(&sys, 0, sizeof(sys));

    OmegaLin lin;
    memset(&lin, 0, sizeof(lin));
    if (!omega_decompose(&sys, index_expr, &lin, +1)) return false;

    /* Pure constant */
    if (sys.n_vars == 0) return lin.const_term >= 0;

    /* Negated assumption: index_expr ≤ -1
     * i.e. coeff*vars ≤ -1 - const_term  */
    omega_add_ineq(&sys, lin.coeff, -1 - lin.const_term);
    omega_extract_vra(&sys, ctx);
    return !omega_fm_sat(&sys);
}

/*
 * Try to prove  index_expr < bound_expr  given the VRA context `ctx`.
 *
 * Returns true  → proved safe.
 * Returns false → cannot prove (not necessarily unsafe; existing checks
 *                 should still emit E085 if no other proof was found).
 *
 * Both `index_expr` and `bound_expr` may be type-annotation AST nodes
 * (unresolved decl/type pointers) — only raw identifier names are read.
 */
static bool omega_prove_lt(RangeTable *ctx, Expr *index_expr,
                           Expr *bound_expr) {
    if (!ctx || !index_expr || !bound_expr) return false;

    OmegaSystem sys;
    memset(&sys, 0, sizeof(sys));

    /* Decompose both sides */
    OmegaLin idx_lin, bnd_lin;
    memset(&idx_lin, 0, sizeof(idx_lin));
    memset(&bnd_lin, 0, sizeof(bnd_lin));

    if (!omega_decompose(&sys, index_expr, &idx_lin, +1)) return false;
    if (!omega_decompose(&sys, bound_expr, &bnd_lin, +1)) return false;

    /* Pure-constant case: no FM needed */
    if (sys.n_vars == 0)
        return idx_lin.const_term < bnd_lin.const_term;

    /*
     * Add the negated query as an inequality:
     *   index ≥ bound
     *   ↔  bound - index ≤ 0
     *   ↔  (bnd_coeff - idx_coeff)*vars  ≤  idx_const - bnd_const
     */
    {
        int64_t qc[OMEGA_MAX_VARS];
        for (int i = 0; i < OMEGA_MAX_VARS; i++)
            qc[i] = bnd_lin.coeff[i] - idx_lin.coeff[i];
        omega_add_ineq(&sys, qc, idx_lin.const_term - bnd_lin.const_term);
    }

    /* Pull in VRA constraints for the registered variables */
    omega_extract_vra(&sys, ctx);

    /*
     * If the system (negated query + VRA) is UNSAT, the assumption
     * "index ≥ bound" contradicts the known facts → index < bound QED.
     */
    return !omega_fm_sat(&sys);
}

#endif /* SEMA_OMEGA_H */
