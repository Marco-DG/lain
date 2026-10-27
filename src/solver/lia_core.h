// lia_core.h — THE UNTRUSTED SOLVER's search, over one number type (lia.h includes it twice).
//
// No include guard: lia.h includes this file once per number type. The 128-bit instance (LSi,
// names as they are) is the solver; the 512-bit one (LSW, names ending `_w`) is its wide retry.
// The two run the SAME search: the tableau, Bland's rule, the gcd test, the Diophantine step,
// branch and bound, the case splits, the certificate written. Every decision is a comparison of
// exact rationals, so a run at either width takes the steps an unbounded one would, until it
// overflows, and the two answer byte for byte alike wherever the 128-bit run finishes
// (solver_harness.py checks it).
//
// Every operation on a number goes through the includer's macros, never a C operator:
//   N_T            the integer type             CN(x)            this instance's name for x
//   N_I(v)         the number v (an int64_t)    N_SGN(a)         -1, 0 or 1
//   N_CMP(a, b)    -1, 0 or 1                   N_ADD/N_SUB/N_MUL(S, a, b), N_NEG(S, a): checked
//   N_GCD(S, a, b) gcd of |a| and |b|, checked  N_DIV(a, b)      a / b toward zero (b divides a, or
//   N_DIVIDES(a, b) b divides a (b != 0)                         the quotient only is used)
//   N_FLOORDIV(S, a, b)  floor(a / b), b > 0    N_FROMJ(S, x), N_TOJ(v, x): JInt, checked in
// A failed check sets S->cap and S->ovf (a number left the type): the wide retry's cue. On the
// wide instance N_T is a struct, so a C operator on a number does not compile there, and none can
// creep into this file unnoticed (judge_includes.py also checks that the raw types are never named).

// ── rationals: n / d in lowest terms, d >= 1 ──────────────────────────────────────────────────────
typedef struct { N_T n, d; } CN(LQ);

static CN(LQ) CN(lq_int)(N_T v) { CN(LQ) q; q.n = v; q.d = N_I(1); return q; }
static CN(LQ) CN(lq_make)(LSol *S, N_T n, N_T d) {              // d != 0
    if (N_SGN(d) < 0) { n = N_NEG(S, n); d = N_NEG(S, d); }
    N_T g = N_GCD(S, n, d);
    if (N_CMP(g, N_I(1)) > 0) { n = N_DIV(n, g); d = N_DIV(d, g); }
    CN(LQ) q; q.n = n; q.d = d;
    if (S->cap || N_SGN(d) <= 0) { S->cap = true; q.n = N_I(0); q.d = N_I(1); }
    return q;
}
static CN(LQ) CN(lq_add)(LSol *S, CN(LQ) a, CN(LQ) b) {
    if (N_SGN(a.n) == 0) return b;
    if (N_SGN(b.n) == 0) return a;
    N_T g = N_GCD(S, a.d, b.d);
    N_T n = N_ADD(S, N_MUL(S, a.n, N_DIV(b.d, g)), N_MUL(S, b.n, N_DIV(a.d, g)));
    return CN(lq_make)(S, n, N_MUL(S, N_DIV(a.d, g), b.d));
}
static CN(LQ) CN(lq_neg)(LSol *S, CN(LQ) a) { a.n = N_NEG(S, a.n); return a; }
static CN(LQ) CN(lq_sub)(LSol *S, CN(LQ) a, CN(LQ) b) { return CN(lq_add)(S, a, CN(lq_neg)(S, b)); }
static CN(LQ) CN(lq_mul)(LSol *S, CN(LQ) a, CN(LQ) b) {
    if (N_SGN(a.n) == 0 || N_SGN(b.n) == 0) return CN(lq_int)(N_I(0));
    N_T g1 = N_GCD(S, a.n, b.d), g2 = N_GCD(S, b.n, a.d);
    return CN(lq_make)(S, N_MUL(S, N_DIV(a.n, g1), N_DIV(b.n, g2)), N_MUL(S, N_DIV(a.d, g2), N_DIV(b.d, g1)));
}
static CN(LQ) CN(lq_div)(LSol *S, CN(LQ) a, CN(LQ) b) {          // b != 0
    return CN(lq_mul)(S, a, CN(lq_make)(S, b.d, b.n));
}
static int CN(lq_cmp)(LSol *S, CN(LQ) a, CN(LQ) b) {
    CN(LQ) d = CN(lq_sub)(S, a, b);
    return N_SGN(d.n);
}

// ── constraints: Σ a[j]·x_j (<= | =) b, stated by the fact `ref` ──────────────────────────────
typedef struct { N_T *a; N_T b; bool eq; JRef ref; } CN(LCon);

static bool CN(ls_con)(LSol *S, const JCons *c, JRef ref, CN(LCon) *out) {
    if (!(out->a = (N_T *)ls_scratch(S, sizeof(N_T), S->nv))) return false;
    for (int32_t j = 0; j < S->nv; j++) out->a[j] = N_I(0);
    for (int32_t t = 0; t < c->nt; t++) {
        int32_t v = c->t[t].var;
        if (v < 0 || v >= S->nv) { S->cap = true; return false; }   // not a VC the parser accepts
        out->a[v] = N_ADD(S, out->a[v], N_FROMJ(S, &c->t[t].coef));
    }
    out->b = N_FROMJ(S, &c->rhs);
    out->eq = c->op == J_EQ;
    out->ref = ref;
    return !S->cap;
}

// f<new> = lin Σ m[k]·ref[k] over the nonzero m, in constraint order. Returns the new f, or 0.
static int32_t CN(ls_emit_lin)(LSol *S, LSteps *L, const JRef *ref, const N_T *m, int32_t n) {
    int32_t nz = 0;
    for (int32_t k = 0; k < n; k++) if (N_SGN(m[k]) != 0) nz++;
    int32_t i = ls_step(S, L);
    if (i < 0) return 0;
    JMul *mul = (JMul *)ls_keep(S, sizeof(JMul), nz);
    if (!mul) return 0;
    JStep *st = &L->s[i];
    st->k = J_LIN; st->f = ++S->nf; st->mul = mul; st->nmul = nz;
    for (int32_t k = 0, q = 0; k < n; k++)
        if (N_SGN(m[k]) != 0) { N_TOJ(m[k], &mul[q].m); mul[q].r = ref[k]; q++; }
    return st->f;
}
static int32_t CN(ls_emit_lin1)(LSol *S, LSteps *L, JRef r, N_T m) { return CN(ls_emit_lin)(S, L, &r, &m, 1); }

// ── the gcd test, as a constraint enters ──────────────────────────────────────────────────────
// Over the integers Σ a·x <= b, with g the gcd of the a, is Σ (a/g)·x <= floor(b/g): the `rnd`
// cut. A constraint with no variables is decided on its constant, and an equality whose gcd does
// not divide its constant is refuted by two cuts that meet at 0 <= -1. 0: kept (maybe tightened,
// its ref then the cut); 1: the branch is closed; -1: capacity.
static int CN(ls_tighten)(LSol *S, LSteps *L, CN(LCon) *c) {
    N_T g = N_I(0);
    for (int32_t j = 0; j < S->nv; j++) g = N_GCD(S, g, c->a[j]);
    if (S->cap) return -1;
    if (N_SGN(g) == 0) {
        if (c->eq ? N_SGN(c->b) == 0 : N_SGN(c->b) >= 0) return 0;
        int32_t f = CN(ls_emit_lin1)(S, L, c->ref, N_I(c->eq && N_SGN(c->b) > 0 ? -1 : 1));
        return ls_emit_close(S, L, f) ? 1 : -1;
    }
    if (N_CMP(g, N_I(1)) == 0 || N_DIVIDES(c->b, g)) return 0;
    if (!c->eq) {
        int32_t f = CN(ls_emit_lin1)(S, L, c->ref, N_I(1)), r = f ? ls_emit_rnd(S, L, f) : 0;
        if (!r) return -1;
        for (int32_t j = 0; j < S->nv; j++) c->a[j] = N_DIV(c->a[j], g);
        c->b = N_FLOORDIV(S, c->b, g);
        c->ref = ls_ref(J_REF_F, r, 0);
        return S->cap ? -1 : 0;
    }
    int32_t f1 = CN(ls_emit_lin1)(S, L, c->ref, N_I(1)), f2 = f1 ? ls_emit_rnd(S, L, f1) : 0;
    int32_t f3 = f2 ? CN(ls_emit_lin1)(S, L, c->ref, N_I(-1)) : 0, f4 = f3 ? ls_emit_rnd(S, L, f3) : 0;
    if (!f4) return -1;
    JRef two[2] = { ls_ref(J_REF_F, f2, 0), ls_ref(J_REF_F, f4, 0) };
    N_T one[2]; one[0] = N_I(1); one[1] = N_I(1);
    return ls_emit_close(S, L, CN(ls_emit_lin)(S, L, two, one, 2)) ? 1 : -1;
}

// ── the general simplex ───────────────────────────────────────────────────────────────────────
// Variables 0..n-1 are the VC's, n+k is constraint k's slack. Row r says its basic variable is
// Σ_c T[r][c]·var_c over the nonbasic ones (a basic variable's own column is zero).
typedef struct {
    int32_t n, m, w;
    CN(LQ) *T, *val, *lo, *hi;
    bool *hlo, *hhi;
    int32_t *bas, *row;
} CN(LTab);

static bool CN(lt_build)(LSol *S, CN(LTab) *t, const CN(LCon) *cons, int32_t m) {
    t->n = S->nv; t->m = m; t->w = t->n + m;
    int64_t w = t->w;
    t->T = (CN(LQ) *)ls_scratch(S, sizeof(CN(LQ)), (int64_t)m * w);
    t->val = (CN(LQ) *)ls_scratch(S, sizeof(CN(LQ)), w);
    t->lo = (CN(LQ) *)ls_scratch(S, sizeof(CN(LQ)), w);
    t->hi = (CN(LQ) *)ls_scratch(S, sizeof(CN(LQ)), w);
    t->hlo = (bool *)ls_scratch(S, sizeof(bool), w);
    t->hhi = (bool *)ls_scratch(S, sizeof(bool), w);
    t->bas = (int32_t *)ls_scratch(S, sizeof(int32_t), m);
    t->row = (int32_t *)ls_scratch(S, sizeof(int32_t), w);
    if (S->cap) return false;
    for (int64_t i = 0; i < (int64_t)m * w; i++) t->T[i] = CN(lq_int)(N_I(0));
    for (int32_t v = 0; v < t->w; v++) { t->val[v] = t->lo[v] = t->hi[v] = CN(lq_int)(N_I(0)); t->row[v] = -1; }
    for (int32_t r = 0; r < m; r++) {
        int32_t s = t->n + r;
        t->bas[r] = s; t->row[s] = r;
        for (int32_t j = 0; j < t->n; j++) t->T[r * w + j] = CN(lq_int)(cons[r].a[j]);
        t->hhi[s] = true; t->hi[s] = CN(lq_int)(cons[r].b);
        if (cons[r].eq) { t->hlo[s] = true; t->lo[s] = CN(lq_int)(cons[r].b); }
    }
    return true;
}

// Move nonbasic j so that row r's basic variable reaches `target`, then exchange the two.
// Returns the rational operations it did (the finder's meter, lia.h): one per row it moves and one
// per entry it updates, so a sparse row costs what it holds, not the tableau's width.
static uint64_t CN(lt_pivot)(LSol *S, CN(LTab) *t, int32_t r, int32_t j, CN(LQ) target) {
    int32_t w = t->w, b = t->bas[r];
    uint64_t ops = (uint64_t)w;
    CN(LQ) *R = &t->T[(int64_t)r * w];
    CN(LQ) a = R[j];
    CN(LQ) theta = CN(lq_div)(S, CN(lq_sub)(S, target, t->val[b]), a);
    t->val[b] = target;
    t->val[j] = CN(lq_add)(S, t->val[j], theta);
    for (int32_t r2 = 0; r2 < t->m; r2++) {
        CN(LQ) e = t->T[(int64_t)r2 * w + j];
        if (r2 == r || N_SGN(e.n) == 0) continue;
        t->val[t->bas[r2]] = CN(lq_add)(S, t->val[t->bas[r2]], CN(lq_mul)(S, e, theta));
        ops++;
    }
    // row r now defines j:  j = (1/a)·b − Σ_{c≠j} (R[c]/a)·c
    CN(LQ) inv = CN(lq_div)(S, CN(lq_int)(N_I(1)), a);
    for (int32_t c = 0; c < w; c++) R[c] = c == j ? CN(lq_int)(N_I(0)) : CN(lq_neg)(S, CN(lq_mul)(S, R[c], inv));
    R[b] = inv;
    for (int32_t r2 = 0; r2 < t->m; r2++) {
        CN(LQ) *R2 = &t->T[(int64_t)r2 * w];
        CN(LQ) e = R2[j];
        if (r2 == r || N_SGN(e.n) == 0) continue;
        for (int32_t c = 0; c < w; c++)
            if (c != j && N_SGN(R[c].n) != 0) { R2[c] = CN(lq_add)(S, R2[c], CN(lq_mul)(S, e, R[c])); ops++; }
        R2[j] = CN(lq_int)(N_I(0));
    }
    t->bas[r] = j; t->row[j] = r; t->row[b] = -1;
    return ops;
}

// 1: every bound holds; 0: row *cr cannot be repaired, its basic variable below its lower bound
// (*below) or above its upper; -1: a budget or capacity stop.
static int CN(lt_check)(LSol *S, CN(LTab) *t, int32_t *cr, bool *below) {
    for (;;) {
        if (S->cap) return -1;
        int32_t bv = -1; bool lowv = false;
        for (int32_t v = 0; v < t->w && bv < 0; v++) {
            if (t->row[v] < 0) continue;
            if (t->hlo[v] && CN(lq_cmp)(S, t->val[v], t->lo[v]) < 0) { bv = v; lowv = true; }
            else if (t->hhi[v] && CN(lq_cmp)(S, t->val[v], t->hi[v]) > 0) { bv = v; lowv = false; }
        }
        if (bv < 0) return S->cap ? -1 : 1;
        int32_t r = t->row[bv], j = -1;
        CN(LQ) *R = &t->T[(int64_t)r * t->w];
        for (int32_t c = 0; c < t->w && j < 0; c++) {
            if (t->row[c] >= 0 || N_SGN(R[c].n) == 0) continue;
            bool raise = lowv == (N_SGN(R[c].n) > 0);       // the way c must move to move bv the right way
            if (raise ? (!t->hhi[c] || CN(lq_cmp)(S, t->val[c], t->hi[c]) < 0)
                      : (!t->hlo[c] || CN(lq_cmp)(S, t->val[c], t->lo[c]) > 0)) j = c;
        }
        if (j < 0) { *cr = r; *below = lowv; return S->cap ? -1 : 0; }
        if (S->used.pivot >= S->limit.pivot) { S->out = true; return -1; }
        if (ls_meter_on && ls_meter_left == 0) { S->out = true; return -1; }   // the finder's run (lia.h)
        S->used.pivot++;
        uint64_t ops = CN(lt_pivot)(S, t, r, j, lowv ? t->lo[bv] : t->hi[bv]);
        if (ls_meter_on) {
            ls_meter_left = ops >= ls_meter_left ? 0 : ls_meter_left - ops; ls_meter_spent += ops;
        }
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
static bool CN(lt_explain)(LSol *S, const CN(LTab) *t, int32_t r, bool below, N_T *m) {
    int32_t b = t->bas[r], n = t->n;
    const CN(LQ) *R = &t->T[(int64_t)r * t->w];
    if (b < n) { S->cap = true; return false; }
    CN(LQ) *q = (CN(LQ) *)ls_scratch(S, sizeof(CN(LQ)), t->m);
    if (!q) return false;
    for (int32_t k = 0; k < t->m; k++) q[k] = CN(lq_int)(N_I(0));
    q[b - n] = CN(lq_int)(N_I(below ? -1 : 1));
    for (int32_t c = 0; c < t->w; c++) {
        if (N_SGN(R[c].n) == 0) continue;
        if (c < n) { S->cap = true; return false; }   // an unbounded variable: not a conflict
        q[c - n] = below ? R[c] : CN(lq_neg)(S, R[c]);
    }
    N_T l = N_I(1);
    for (int32_t k = 0; k < t->m; k++)
        if (N_SGN(q[k].n) != 0) l = N_MUL(S, N_DIV(l, N_GCD(S, l, q[k].d)), q[k].d);
    for (int32_t k = 0; k < t->m; k++) m[k] = N_SGN(q[k].n) != 0 ? N_MUL(S, q[k].n, N_DIV(l, q[k].d)) : N_I(0);
    return !S->cap;
}

// ── equalities over the integers: a Diophantine step before a split ──────────────────────────────
// A node whose rational model is not integral is split (below), unless its equalities have no
// integer solution already: `x = 2q`, `x' = 2q' + 1` and `x' = x + 2` give 2q - 2q' = -1, which the
// rationals satisfy, and so does each branch of a split on an unbounded q, for ever.
// First the BOUNDS a row gives a variable of coefficient +-1 from the bounds of its others (an
// inequality one side, an equality both), passes over the rows until none tightens (at most
// LS_DIOPH_PASSES): `r' - r >= 1` beside 0 <= r, r' <= 1 puts r at 0 and r' at 1. Each bound is a
// record of its row and of the records it read, so a bound's derivation is a combination of rows
// that reaches back only to older ones. A variable whose two bounds meet is FIXED. Then the
// equalities, the fixed variables at their values, are eliminated on unit coefficients (a
// unit-pivot form of Griggio's Diophantine step), rows and variables in order, each row an integer
// combination of the equalities whose multipliers it carries. A row whose coefficients' gcd does
// not divide its constant has no integer solution: f1 = lin of the equalities by its multipliers
// and of each fixed variable's bound derivation that cancels it; f2 = rnd f1; f3 and f4 the same
// from the negated multipliers; f2 + f4 is 0 <= -1. 1: refuted (the branch is closed); 0: no such
// row found, or the step not tried (a system past LS_DIOPH_CELLS, records past their room, a
// derivation past LS_DIOPH_EXPAND), alike at either width; -1: capacity.
// Found none, it names in *sv the variable to split instead of a fractional one, if any: the first
// RESIDUE, a variable not fixed whose bounds are at most LS_DIOPH_DOMAIN apart and which an equality
// names with a coefficient of +-1 beside a coefficient past +-1 (r in `x = 2q + r`, 0 <= r <= 1),
// split at its lower bound *sfl. Each branch fixes it or shrinks its domain, so these splits end;
// and a residue the equalities make odd is refuted in one branch and fixed in the other (`x = 2q + r`
// beside `x = 2p + 1`). Not the quotient: q's domain is often small too (a byte's x / 16), and
// splitting on every quotient value cost fnv.ln 0.3 -> 79 s. At most LS_DIOPH_RSPLITS such splits
// on one branch: a search for a MODEL across laps meets a residue per lap (a binary search's mid),
// and taking each first cost Binary_Search-2's ranking search 7e6 -> 285e6 operations.
#define LS_DIOPH_DOMAIN 3
#define LS_DIOPH_RSPLITS 2
#define LS_DIOPH_CELLS 8192
#define LS_DIOPH_PASSES 8
#define LS_DIOPH_EXPAND 4096
typedef struct { int32_t v, k, dep; int8_t up, sg; } CN(LBnd);   // x_v <= or >= the row k times sg, its others by deps
typedef struct { CN(LBnd) *r; N_T *val; int32_t n, cap, *deps, nd, dcap, *lo, *hi, spent; } CN(LBnds);
// mm += c times record i's derivation: its row times its sign, then each other variable's term
// cancelled by the bound its record names. False when the expansion is past its room (*room).
static bool CN(ls_dioph_bound)(LSol *S, const CN(LCon) *cons, CN(LBnds) *B, int32_t i, N_T c, N_T *mm) {
    if (++B->spent > LS_DIOPH_EXPAND) return false;
    const CN(LBnd) *b = &B->r[i];
    const CN(LCon) *r = &cons[b->k];
    N_T cs = b->sg > 0 ? c : N_NEG(S, c);
    mm[b->k] = N_ADD(S, mm[b->k], cs);
    for (int32_t u = 0, d = 0; u < S->nv && !S->cap; u++) {
        if (u == b->v || N_SGN(r->a[u]) == 0) continue;
        N_T kap = N_MUL(S, cs, r->a[u]);             // kap > 0: its lower bound cancels it; < 0: its upper
        if (!CN(ls_dioph_bound)(S, cons, B, B->deps[b->dep + d++], N_SGN(kap) > 0 ? kap : N_NEG(S, kap), mm)) return false;
    }
    return true;
}
static int CN(ls_dioph)(LSol *S, LSteps *L, const CN(LCon) *cons, int32_t ncons, int32_t *sv, N_T *sfl) {
    int32_t nv = S->nv, neq = 0, ncol = 0, nnz = 0;
    *sv = -1;
    // only where an equality in play has a coefficient past +-1: unit equalities alone (definitions,
    // `x' = x + 1`) meet a gcd past 1 only by rare combinations, and the step would cost every node
    // of every search (Binary_Search-2's ranking search: 7e6 -> 285e6 operations, dense)
    bool wide = false;
    for (int32_t k = 0; k < ncons; k++)
        for (int32_t v = 0; v < nv; v++) {
            if (N_SGN(cons[k].a[v]) == 0) continue;
            nnz++;
            if (cons[k].eq && N_CMP(cons[k].a[v], N_I(1)) != 0 && N_CMP(cons[k].a[v], N_I(-1)) != 0) wide = true;
        }
    for (int32_t k = 0; k < ncons; k++) neq += cons[k].eq;
    if (neq == 0 || !wide) return 0;
    char *mark = S->lo;
    CN(LBnds) B; memset(&B, 0, sizeof B);
    B.cap = 4 * nv + 64; B.dcap = 16 * B.cap;
    int32_t *col = (int32_t *)ls_scratch(S, sizeof(int32_t), nv), *eqk = (int32_t *)ls_scratch(S, sizeof(int32_t), neq);
    int32_t *rs = (int32_t *)ls_scratch(S, sizeof(int32_t), (int64_t)ncons + 1);
    int32_t *rv = (int32_t *)ls_scratch(S, sizeof(int32_t), nnz ? nnz : 1);
    B.lo = (int32_t *)ls_scratch(S, sizeof(int32_t), nv); B.hi = (int32_t *)ls_scratch(S, sizeof(int32_t), nv);
    B.r = (CN(LBnd) *)ls_scratch(S, sizeof(CN(LBnd)), B.cap); B.val = (N_T *)ls_scratch(S, sizeof(N_T), B.cap);
    B.deps = (int32_t *)ls_scratch(S, sizeof(int32_t), B.dcap);
    if (!col || !eqk || !rs || !rv || !B.lo || !B.hi || !B.r || !B.val || !B.deps) return -1;
    for (int32_t v = 0; v < nv; v++) { col[v] = -1; B.lo[v] = B.hi[v] = -1; }
    for (int32_t k = 0, n = 0; k < ncons; k++) {            // each row's variables, once
        rs[k] = n;
        for (int32_t v = 0; v < nv; v++) if (N_SGN(cons[k].a[v]) != 0) rv[n++] = v;
        rs[k + 1] = n;
    }
    #define CN_FIXED(v) (B.lo[v] >= 0 && B.hi[v] >= 0 && N_CMP(B.val[B.lo[v]], B.val[B.hi[v]]) == 0)
    bool room = true;
    for (int pass = 0, grew = 1; grew && room && pass < LS_DIOPH_PASSES; pass++) {
        grew = 0;
        for (int32_t k = 0; k < ncons && room; k++)
            for (int sg = 1; sg >= (cons[k].eq ? -1 : 1) && room; sg -= 2) {   // sg * row <= sg * b
                // the row's least value over the variables bounded on the side each needs, and the
                // one variable, if one, that is not: a unit variable's bound is sg*b less the others'
                int32_t miss = -1, nmiss = 0;
                N_T tot = N_I(0);
                for (int32_t q = rs[k]; q < rs[k + 1] && nmiss < 2; q++) {
                    int32_t u = rv[q];
                    N_T au = sg > 0 ? cons[k].a[u] : N_NEG(S, cons[k].a[u]);
                    int32_t ri = N_SGN(au) > 0 ? B.lo[u] : B.hi[u];
                    if (ri < 0) { miss = u; nmiss++; } else tot = N_ADD(S, tot, N_MUL(S, au, B.val[ri]));
                }
                if (S->cap) return -1;
                if (nmiss > 1) continue;
                for (int32_t q = rs[k]; q < rs[k + 1] && room; q++) {
                    int32_t v = rv[q];
                    N_T av = cons[k].a[v];
                    if ((N_CMP(av, N_I(1)) != 0 && N_CMP(av, N_I(-1)) != 0) || (nmiss == 1 && v != miss)) continue;
                    N_T sav = sg > 0 ? av : N_NEG(S, av), bb = sg > 0 ? cons[k].b : N_NEG(S, cons[k].b);
                    if (nmiss == 0) {                        // its own term out of the total
                        int32_t ri = N_SGN(sav) > 0 ? B.lo[v] : B.hi[v];
                        bb = N_SUB(S, bb, N_SUB(S, tot, N_MUL(S, sav, B.val[ri])));
                    } else bb = N_SUB(S, bb, tot);
                    if (S->cap) return -1;
                    bool up = (sg > 0) == (N_SGN(av) > 0);
                    N_T x = up ? bb : N_NEG(S, bb);         // up: x_v <= bb; else -x_v <= bb, x_v >= -bb
                    int32_t cur = up ? B.hi[v] : B.lo[v];
                    if (cur >= 0 && (up ? N_CMP(x, B.val[cur]) >= 0 : N_CMP(x, B.val[cur]) <= 0)) continue;
                    int32_t nd = rs[k + 1] - rs[k] - 1;
                    if (B.n == B.cap || B.nd + nd > B.dcap) { room = false; break; }
                    CN(LBnd) *nb = &B.r[B.n];
                    nb->v = v; nb->k = k; nb->dep = B.nd; nb->up = up; nb->sg = (int8_t)sg;
                    for (int32_t q2 = rs[k]; q2 < rs[k + 1]; q2++) {
                        int32_t u = rv[q2];
                        if (u == v) continue;
                        N_T au = sg > 0 ? cons[k].a[u] : N_NEG(S, cons[k].a[u]);
                        B.deps[B.nd++] = N_SGN(au) > 0 ? B.lo[u] : B.hi[u];
                    }
                    B.val[B.n] = x;
                    if (up) B.hi[v] = B.n; else B.lo[v] = B.n;
                    B.n++; grew = 1;
                    if (nmiss == 0) {                        // the total moved with v's own bound
                        tot = N_I(0); nmiss = 0;
                        for (int32_t q2 = rs[k]; q2 < rs[k + 1]; q2++) {
                            int32_t u = rv[q2];
                            N_T au = sg > 0 ? cons[k].a[u] : N_NEG(S, cons[k].a[u]);
                            int32_t ri = N_SGN(au) > 0 ? B.lo[u] : B.hi[u];
                            if (ri < 0) nmiss++; else tot = N_ADD(S, tot, N_MUL(S, au, B.val[ri]));
                        }
                        if (nmiss) break;
                    }
                }
            }
    }
    // the variable to split, should no row be refuted
    for (int32_t k = 0; k < ncons && *sv < 0; k++) {
        if (!cons[k].eq) continue;
        bool wk = false;
        for (int32_t q = rs[k]; q < rs[k + 1] && !wk; q++)
            wk = N_CMP(cons[k].a[rv[q]], N_I(1)) != 0 && N_CMP(cons[k].a[rv[q]], N_I(-1)) != 0;
        for (int32_t q = rs[k]; q < rs[k + 1] && wk && *sv < 0; q++) {
            int32_t v = rv[q];
            if ((N_CMP(cons[k].a[v], N_I(1)) == 0 || N_CMP(cons[k].a[v], N_I(-1)) == 0) &&
                B.lo[v] >= 0 && B.hi[v] >= 0 && !CN_FIXED(v) &&
                N_CMP(N_SUB(S, B.val[B.hi[v]], B.val[B.lo[v]]), N_I(LS_DIOPH_DOMAIN)) <= 0) {
                *sv = v; *sfl = B.val[B.lo[v]];
            }
        }
    }
    // the columns: the variables the equalities name, the fixed ones aside
    for (int32_t e = 0, k = 0; k < ncons; k++) {
        if (!cons[k].eq) continue;
        eqk[e++] = k;
        for (int32_t q = rs[k]; q < rs[k + 1]; q++) if (col[rv[q]] < 0 && !CN_FIXED(rv[q])) col[rv[q]] = ncol++;
    }
    if (S->cap) return -1;
    if (ncol == 0 || (int64_t)neq * (int64_t)(ncol + neq) > LS_DIOPH_CELLS) { S->lo = mark; return 0; }
    // row e: a[e][0..ncol) . x = b[e], the combination mu[e][0..neq) of the equalities
    int32_t w = ncol + neq;
    N_T *R = (N_T *)ls_scratch(S, sizeof(N_T), (int64_t)neq * w), *b = (N_T *)ls_scratch(S, sizeof(N_T), neq);
    uint8_t *on = (uint8_t *)ls_scratch(S, 1, neq), *dirty = (uint8_t *)ls_scratch(S, 1, neq);
    int32_t *nzq = (int32_t *)ls_scratch(S, sizeof(int32_t), w);
    if (!R || !b || !on || !dirty || !nzq) return -1;
    for (int32_t e = 0; e < neq; e++) {
        const CN(LCon) *c = &cons[eqk[e]];
        N_T *r = &R[(int64_t)e * w];
        for (int32_t q = 0; q < w; q++) r[q] = N_I(0);
        r[ncol + e] = N_I(1);
        b[e] = c->b;
        for (int32_t q = rs[eqk[e]]; q < rs[eqk[e] + 1]; q++) {
            int32_t v = rv[q];
            if (col[v] >= 0) r[col[v]] = c->a[v];
            else b[e] = N_SUB(S, b[e], N_MUL(S, c->a[v], B.val[B.lo[v]]));   // a fixed variable, at its value
        }
        on[e] = dirty[e] = 1;
    }
    if (S->cap) return -1;
    for (;;) {
        int32_t bad = -1;                            // the gcd test, on the rows changed since
        for (int32_t e = 0; e < neq && bad < 0; e++) {
            if (!on[e] || !dirty[e]) continue;
            dirty[e] = 0;
            N_T g = N_I(0);
            for (int32_t q = 0; q < ncol; q++) g = N_GCD(S, g, R[(int64_t)e * w + q]);
            if (S->cap) return -1;
            if (N_SGN(g) == 0) { on[e] = 0; continue; }   // 0 = b: the rationals' (they agreed)
            if (N_CMP(g, N_I(1)) != 0 && !N_DIVIDES(b[e], g)) bad = e;
        }
        if (bad >= 0) {
            // the combination's coefficient on each fixed variable, which its bounds cancel
            JRef *ref = (JRef *)ls_scratch(S, sizeof(JRef), ncons);
            N_T *mm = (N_T *)ls_scratch(S, sizeof(N_T), ncons), *cf = (N_T *)ls_scratch(S, sizeof(N_T), nv);
            if (!ref || !mm || !cf) return -1;
            for (int32_t k = 0; k < ncons; k++) ref[k] = cons[k].ref;
            for (int32_t v = 0; v < nv; v++) cf[v] = N_I(0);
            for (int32_t e = 0; e < neq; e++) {
                N_T mu = R[(int64_t)bad * w + ncol + e];
                if (N_SGN(mu) == 0) continue;
                for (int32_t q = rs[eqk[e]]; q < rs[eqk[e] + 1]; q++) {
                    int32_t v = rv[q];
                    if (col[v] < 0) cf[v] = N_ADD(S, cf[v], N_MUL(S, mu, cons[eqk[e]].a[v]));
                }
            }
            int32_t f[2];
            for (int side = 0; side < 2; side++) {   // f1 from the multipliers, f3 from their negation
                for (int32_t k = 0; k < ncons; k++) mm[k] = N_I(0);
                for (int32_t e = 0; e < neq; e++) {
                    N_T mu = R[(int64_t)bad * w + ncol + e];
                    mm[eqk[e]] = N_ADD(S, mm[eqk[e]], side ? N_NEG(S, mu) : mu);
                }
                for (int32_t v = 0; v < nv && !S->cap; v++) {
                    N_T c = side ? N_NEG(S, cf[v]) : cf[v];
                    if (N_SGN(c) == 0) continue;
                    // c > 0 is cancelled by -x_v <= -lo (its lower bound), c < 0 by x_v <= hi
                    bool pos = N_SGN(c) > 0;
                    if (!CN(ls_dioph_bound)(S, cons, &B, pos ? B.lo[v] : B.hi[v], pos ? c : N_NEG(S, c), mm)) {
                        if (S->cap) return -1;
                        S->lo = mark; return 0;      // past its room: not tried
                    }
                }
                if (S->cap) return -1;
                int32_t fl = CN(ls_emit_lin)(S, L, ref, mm, ncons);
                f[side] = fl ? ls_emit_rnd(S, L, fl) : 0;
                if (!f[side]) return -1;
            }
            JRef two[2] = { ls_ref(J_REF_F, f[0], 0), ls_ref(J_REF_F, f[1], 0) };
            N_T one[2]; one[0] = N_I(1); one[1] = N_I(1);
            if (!ls_emit_close(S, L, CN(ls_emit_lin)(S, L, two, one, 2))) return -1;
            S->lo = mark;
            return 1;
        }
        int32_t pe = -1, pq = -1;                    // a unit pivot: the first row, its first such column
        for (int32_t e = 0; e < neq && pe < 0; e++) {
            if (!on[e]) continue;
            for (int32_t q = 0; q < ncol && pq < 0; q++)
                if (N_CMP(R[(int64_t)e * w + q], N_I(1)) == 0 || N_CMP(R[(int64_t)e * w + q], N_I(-1)) == 0) pq = q;
            if (pq >= 0) pe = e;
        }
        if (pe < 0) break;
        N_T s = R[(int64_t)pe * w + pq], *rp = &R[(int64_t)pe * w];   // s: +-1
        int32_t nq = 0;
        for (int32_t q = 0; q < w; q++) if (N_SGN(rp[q]) != 0) nzq[nq++] = q;
        for (int32_t e = 0; e < neq; e++) {
            if (!on[e] || e == pe || N_SGN(R[(int64_t)e * w + pq]) == 0) continue;
            N_T k = N_MUL(S, R[(int64_t)e * w + pq], s), *re = &R[(int64_t)e * w];   // row e -= k * row pe
            for (int32_t i = 0; i < nq; i++) re[nzq[i]] = N_SUB(S, re[nzq[i]], N_MUL(S, k, rp[nzq[i]]));
            b[e] = N_SUB(S, b[e], N_MUL(S, k, b[pe]));
            dirty[e] = 1;
            if (S->cap) return -1;
            if (ls_meter_on) {                       // the finder's meter (lia.h), one per entry
                uint64_t ops = (uint64_t)nq;
                ls_meter_left = ops >= ls_meter_left ? 0 : ls_meter_left - ops; ls_meter_spent += ops;
            }
        }
        on[pe] = 0;                                  // x_pq is free elsewhere: its row binds nothing more
    }
    #undef CN_FIXED
    S->lo = mark;
    return 0;
}

// ── the search ────────────────────────────────────────────────────────────────────────────────
static bool CN(ls_holds)(LSol *S, const JCons *c, const N_T *x) {
    N_T s = N_I(0);
    for (int32_t t = 0; t < c->nt; t++) s = N_ADD(S, s, N_MUL(S, N_FROMJ(S, &c->t[t].coef), x[c->t[t].var]));
    N_T b = N_FROMJ(S, &c->rhs);
    int k = N_CMP(s, b);
    return c->op == J_EQ ? k == 0 : c->op == J_LE ? k <= 0 : k != 0;
}

// One node: cons[0..fresh) entered at an ancestor, cons[fresh..ncons) enter here. On LS_UNSAT the
// node's refutation is in L; on LS_SAT the integer model is in `model`.
static LRes CN(ls_node)(LSol *S, const JVc *vc, CN(LCon) *cons, int32_t ncons, int32_t fresh,
                        LSteps *L, N_T *model) {
    char *mark = S->lo;
    for (int32_t k = fresh; k < ncons; k++) {
        int r = CN(ls_tighten)(S, L, &cons[k]);
        if (r < 0 || S->cap) return LS_STOP;
        if (r == 1) { S->lo = mark; return LS_UNSAT; }
    }
    char *tabmark = S->lo;
    CN(LTab) t;
    if (!CN(lt_build)(S, &t, cons, ncons)) return LS_STOP;
    int32_t cr = 0; bool below = false;
    int st = CN(lt_check)(S, &t, &cr, &below);
    if (st < 0) return LS_STOP;
    if (st == 0) {
        N_T *m = (N_T *)ls_scratch(S, sizeof(N_T), ncons);
        JRef *ref = (JRef *)ls_scratch(S, sizeof(JRef), ncons);
        if (!m || !ref || !CN(lt_explain)(S, &t, cr, below, m)) return LS_STOP;
        for (int32_t k = 0; k < ncons; k++) ref[k] = cons[k].ref;
        if (!ls_emit_close(S, L, CN(ls_emit_lin)(S, L, ref, m, ncons))) return LS_STOP;
        S->lo = mark; return LS_UNSAT;
    }
    // The rationals agree. The first variable that is not an integer is split at its floor.
    int32_t j = -1; N_T fl = N_I(0);
    for (int32_t v = 0; v < t.n && j < 0; v++)
        if (N_CMP(t.val[v].d, N_I(1)) != 0) { j = v; fl = N_FLOORDIV(S, t.val[v].n, t.val[v].d); }
    N_T *x = (N_T *)ls_scratch(S, sizeof(N_T), S->nv);
    if (!x || S->cap) return LS_STOP;
    for (int32_t v = 0; v < t.n; v++) x[v] = t.val[v].n;
    if (j >= 0 && S->cone) {                         // a cone's: the rational model times its denominators' lcm
        N_T l = N_I(1);
        for (int32_t v = 0; v < t.n; v++) l = N_MUL(S, N_DIV(l, N_GCD(S, l, t.val[v].d)), t.val[v].d);
        for (int32_t v = 0; v < t.n; v++) x[v] = N_MUL(S, t.val[v].n, N_DIV(l, t.val[v].d));
        if (S->cap) return LS_STOP;
        memcpy(model, x, sizeof(N_T) * (size_t)S->nv);
        return LS_SAT;
    }
    if (j >= 0) {
        // no integer solution to its equalities: no split (ls_dioph, while the tableau's values hold)
        int32_t sv = -1; N_T sfl = N_I(0);
        int dz = CN(ls_dioph)(S, L, cons, ncons, &sv, &sfl);
        if (dz < 0 || S->cap) return LS_STOP;
        if (dz == 1) { S->lo = mark; return LS_UNSAT; }
        // a residue first, its branch holding the rational model searched first: a search that
        // finds a model finds it there (Binary_Search-2's laps, one residue each)
        bool rs = S->residue && sv >= 0 && S->rsplit < LS_DIOPH_RSPLITS;
        int first = 0;
        if (rs) { first = CN(lq_cmp)(S, t.val[sv], CN(lq_int)(sfl)) > 0; j = sv; fl = sfl; }
        S->lo = tabmark;
        if (S->used.node >= S->limit.node) { S->out = true; return LS_STOP; }
        S->used.node++;
        int32_t sk = ++S->ns, si = ls_step(S, L);
        JTerm *term = (JTerm *)ls_keep(S, sizeof(JTerm), 1);
        JSteps *br = (JSteps *)ls_keep(S, sizeof(JSteps), 2);
        CN(LCon) *c2 = (CN(LCon) *)ls_scratch(S, sizeof(CN(LCon)), (int64_t)ncons + 1);
        if (si < 0 || !term || !br || !c2) return LS_STOP;
        term->var = j; N_TOJ(N_I(1), &term->coef);
        JStep *sp = &L->s[si];
        sp->k = J_SPLIT; sp->s = sk; sp->br = br; sp->nbr = 2;
        sp->split.t = term; sp->split.nt = 1; sp->split.op = J_LE; N_TOJ(fl, &sp->split.rhs);
        memcpy(c2, cons, sizeof(CN(LCon)) * (size_t)ncons);
        if (!(c2[ncons].a = (N_T *)ls_scratch(S, sizeof(N_T), S->nv))) return LS_STOP;
        for (int32_t v = 0; v < S->nv; v++) c2[ncons].a[v] = N_I(0);
        c2[ncons].ref = ls_ref(J_REF_S, sk, 0);
        S->rsplit += rs;
        for (int pass = 0; pass < 2; pass++) {         // le: x_j <= fl;  gt: −x_j <= −fl − 1
            int side = pass ^ first;
            c2[ncons].a[j] = N_I(side ? -1 : 1);
            c2[ncons].b = side ? N_SUB(S, N_NEG(S, fl), N_I(1)) : fl;
            c2[ncons].eq = false;
            LSteps sub; memset(&sub, 0, sizeof sub);
            LRes r = CN(ls_node)(S, vc, c2, ncons + 1, ncons, &sub, model);
            if (r != LS_UNSAT) { S->rsplit -= rs; return r; }
            br[side].s = sub.s; br[side].n = sub.n;
        }
        S->rsplit -= rs;
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
            for (int32_t q = 0; q < H->dis[d].nc && all; q++) all = CN(ls_holds)(S, &H->dis[d].c[q], x);
            any = all;
        }
        if (S->cap) return LS_STOP;
        if (!any) hd = h;
    }
    if (hd < 0) { memcpy(model, x, sizeof(N_T) * (size_t)S->nv); return LS_SAT; }
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
        CN(LCon) *c2 = (CN(LCon) *)ls_scratch(S, sizeof(CN(LCon)), (int64_t)ncons + D->nc);
        if (!c2) return LS_STOP;
        memcpy(c2, cons, sizeof(CN(LCon)) * (size_t)ncons);
        for (int32_t q = 0; q < D->nc; q++)
            if (!CN(ls_con)(S, &D->c[q], ls_ref(J_REF_SC, sk, q + 1), &c2[ncons + q])) return LS_STOP;
        LSteps sub; memset(&sub, 0, sizeof sub);
        LRes r = CN(ls_node)(S, vc, c2, ncons + D->nc, ncons, &sub, model);
        if (r != LS_UNSAT) return r;
        br[d].s = sub.s; br[d].n = sub.n;
        S->lo = dmark;
    }
    S->lo = mark; return LS_UNSAT;
}

// One search over the hypotheses `on` names (NULL: every one). A reference keeps its hypothesis's
// index in the full list, so a certificate over a slice is the full VC's. *ovf: it stopped because
// a number left the type (not the region, not the budget).
static JAnswer CN(ls_solve1)(const JVc *vc, const uint8_t *on, bool cone, JWork limit, void *mem, size_t size, bool *ovf,
                             bool residue) {
    JAnswer A; memset(&A, 0, sizeof A);
    LSol S; memset(&S, 0, sizeof S);
    S.lo = (char *)mem; S.hi = (char *)mem + size; S.limit = limit; S.nv = vc->nvars; S.on = on; S.cone = cone;
    S.residue = residue;
    int32_t nc = 1;
    for (int32_t h = 0; h < vc->nhyps; h++) if (!vc->hyps[h].disj && (!on || on[h])) nc++;
    CN(LCon) *cons = (CN(LCon) *)ls_scratch(&S, sizeof(CN(LCon)), nc);
    N_T *model = (N_T *)ls_scratch(&S, sizeof(N_T), S.nv);
    LRes r = LS_STOP;
    LSteps body; memset(&body, 0, sizeof body);
    if (cons && model) {
        int32_t k = 0;
        for (int32_t h = 0; h < vc->nhyps && !S.cap; h++)
            if (!vc->hyps[h].disj && (!on || on[h])) CN(ls_con)(&S, &vc->hyps[h].cons, ls_ref(J_REF_H, h + 1, 0), &cons[k++]);
        // g, the goal's negation: `-lin <= -c - 1` for `lin <= c`, `lin = c` for `lin != c`
        if (!S.cap && CN(ls_con)(&S, &vc->goal, ls_ref(J_REF_G, 0, 0), &cons[k])) {
            if (vc->goal.op == J_LE) {
                for (int32_t j = 0; j < S.nv; j++) cons[k].a[j] = N_NEG(&S, cons[k].a[j]);
                cons[k].b = N_SUB(&S, N_NEG(&S, cons[k].b), N_I(1));
            } else cons[k].eq = true;
        }
        if (!S.cap) r = CN(ls_node)(&S, vc, cons, nc, 0, &body, model);
    }
    if (r == LS_SAT) {
        JInt *val = (JInt *)ls_keep(&S, sizeof(JInt), S.nv);
        if (val) for (int32_t j = 0; j < S.nv; j++) N_TOJ(model[j], &val[j]);
        if (val) { A.k = J_ANS_MODEL; A.model.id = vc->id; A.model.val = val; A.model.n = S.nv; }
        else r = LS_STOP;
    } else if (r == LS_UNSAT) {
        A.k = J_ANS_CERT; A.cert.id = vc->id; A.cert.body.s = body.s; A.cert.body.n = body.n;
    }
    if (r == LS_STOP) A.k = S.cap || !S.out ? J_ANS_CAPACITY : J_ANS_BUDGET;
    if (ovf) *ovf = r == LS_STOP && S.ovf;
    A.used = S.used;
    return A;
}
// The search, then, for a VC it stopped on the budget whose equalities hold a RESIDUE (`x = 2q + r`,
// a coefficient past +-1 beside one of +-1), the search again with residues split first
// (ls_dioph), under a sixteenth of the limit: a parity refutation that splitting the unbounded
// quotient never ends (Kroening's ranking: 363 and 976 pivots so) is found there, and a search that
// finds a model without it never gets there (taking every residue first cost Binary_Search-2's
// satisfiable path questions their budget, 100000 pivots each). Its capacity is the answer, as the
// first search's would be, so that the retries after it are the same at either width.
static bool CN(ls_residue_shaped)(const JVc *vc, const uint8_t *on) {
    for (int32_t h = 0; h < vc->nhyps; h++) {
        const JCons *c = &vc->hyps[h].cons;
        if (vc->hyps[h].disj || (on && !on[h]) || c->op != J_EQ) continue;
        bool unit = false, wide = false;
        for (int32_t q = 0; q < c->nt; q++) {
            bool one = c->t[q].coef.n == 1 && c->t[q].coef.limb[0] == 1;
            unit |= one; wide |= !one && c->t[q].coef.n != 0;
        }
        if (unit && wide) return true;
    }
    return false;
}
static JAnswer CN(ls_solve)(const JVc *vc, const uint8_t *on, bool cone, JWork limit, void *mem, size_t size, bool *ovf) {
    JAnswer A = CN(ls_solve1)(vc, on, cone, limit, mem, size, ovf, false);
    if (A.k != J_ANS_BUDGET || cone || !CN(ls_residue_shaped)(vc, on)) return A;
    JWork l2 = limit; l2.pivot /= 16; l2.node /= 16; l2.leaf /= 16;
    if (!l2.pivot || !l2.node || !l2.leaf) return A;
    bool ovf2 = false;
    JAnswer B = CN(ls_solve1)(vc, on, cone, l2, mem, size, &ovf2, true);
    B.used.pivot += A.used.pivot; B.used.node += A.used.node; B.used.leaf += A.used.leaf;
    if (B.k == J_ANS_CERT || B.k == J_ANS_MODEL || B.k == J_ANS_CAPACITY) { if (ovf) *ovf = ovf2; return B; }
    A.used = B.used;
    return A;
}
