// lia_core.h — THE UNTRUSTED SOLVER's search, over one number type (lia.h includes it twice).
//
// No include guard: lia.h includes this file once per number type. The 128-bit instance (LSi,
// names as they are) is the solver; the 512-bit one (LSW, names ending `_w`) is its wide retry.
// The two run the SAME search: the tableau, Bland's rule, the gcd test, branch and bound, the case
// splits, the certificate written. Every decision is a comparison of exact rationals, so a run at
// either width takes the steps an unbounded one would, until it overflows, and the two answer byte
// for byte alike wherever the 128-bit run finishes (solver_harness.py checks it).
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
        for (int side = 0; side < 2; side++) {         // le: x_j <= fl;  gt: −x_j <= −fl − 1
            c2[ncons].a[j] = N_I(side ? -1 : 1);
            c2[ncons].b = side ? N_SUB(S, N_NEG(S, fl), N_I(1)) : fl;
            c2[ncons].eq = false;
            LSteps sub; memset(&sub, 0, sizeof sub);
            LRes r = CN(ls_node)(S, vc, c2, ncons + 1, ncons, &sub, model);
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
static JAnswer CN(ls_solve)(const JVc *vc, const uint8_t *on, bool cone, JWork limit, void *mem, size_t size, bool *ovf) {
    JAnswer A; memset(&A, 0, sizeof A);
    LSol S; memset(&S, 0, sizeof S);
    S.lo = (char *)mem; S.hi = (char *)mem + size; S.limit = limit; S.nv = vc->nvars; S.on = on; S.cone = cone;
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
