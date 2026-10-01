// containment.h — THE ANALYSIS'S INVARIANT, CHECKED ON EVERY EXECUTION (plan I.18).
//
// The range analysis computes, for every block, a state that must contain every concrete state
// any execution can have there: its INVARIANT. The per-operation harness tests one transfer at a
// time, and the interpreter checks the obligations the analysis discharged; this checks the
// invariant itself, at every block entry of a real run: each tracked value's interval and each
// finite difference bound between two of them. So every program the interpreter runs tests the
// whole fixpoint (joins, guard refinement, widening, the constant table), not one operation. It is
// the dynamic half of the certificate checker (local/internal/design/certificates.md, C.3): the
// same entailment, against concrete states instead of abstract ones.
//
// Included after analysis/vra.h and ir/interp.h; `lain --interpret --check-invariants`.
// The analysis's state at a block is closed once and its finite constraints listed, so an entry
// costs one comparison per constraint; a block is checked on its first 64 entries and then at
// every power of two, which keeps a long loop's cost down and its early and late states covered.
#ifndef LAIN_CONTAINMENT_H
#define LAIN_CONTAINMENT_H

typedef struct { int a, b; int64_t ub; } CtCon;       // v[a] - v[b] <= ub; a or b = -1 is zero
typedef struct { bool ready, dead; int n; CtCon *c; long visits; } CtBlock;
typedef struct CtFunc { IrFunc *f; Vra *V; CtBlock *blk; int nblk; struct CtFunc *next; } CtFunc;
static CtFunc *ct_funcs = NULL;
static long ct_checked = 0, ct_constraints = 0;

static CtFunc *ct_for(IrFunc *f) {
    for (CtFunc *c = ct_funcs; c; c = c->next) if (c->f == f) return c;
    CtFunc *c = calloc(1, sizeof *c);
    c->f = f; c->V = vra_analyze(f);
    int nb = 0;
    for (IrBlock *b = f->blocks; b; b = b->next) if (b->id + 1 > nb) nb = b->id + 1;
    c->blk = calloc((size_t)(nb > 0 ? nb : 1), sizeof(CtBlock)); c->nblk = nb;
    c->next = ct_funcs; ct_funcs = c;
    return c;
}

static bool ct_tracked(Vra *V, int id) {
    if (id < 0 || id >= V->nvar || V->odim[id] < 0 || !V->val[id] || !V->val[id]->type) return false;
    IrInstr *d = V->def[id];
    if (d && d->op == IR_ALLOCA)                            // a scalar CELL: its content is the dimension
        return d->aux.alloca_ty && (d->aux.alloca_ty->kind == IRT_INT || d->aux.alloca_ty->kind == IRT_BOOL);
    return V->val[id]->type->kind == IRT_INT || V->val[id]->type->kind == IRT_BOOL;
}

static void ct_prepare(CtFunc *c, IrBlock *b) {
    CtBlock *k = &c->blk[b->id]; Vra *V = c->V;
    k->ready = true;
    if (!V->reached[b->id]) { k->dead = true; return; }
    int64_t *m = malloc((size_t)V->dsz * 8 + 8);
    memcpy(m, V->in[b->id], (size_t)V->dsz * 8);
    Octagon W; memset(&W, 0, sizeof W); W.nvar = V->noct; W.dim = 2 * V->noct; W.m = m;
    const int *saved = oct_map; oct_map = V->odim;
    oct_close(&W);
    if (oct_is_bottom(&W)) { k->dead = true; oct_map = saved; free(m); return; }
    int cap = 64; k->c = malloc(sizeof(CtCon) * (size_t)cap);
    #define CT_ADD(A, B, U) do { if (k->n == cap) { cap *= 2; k->c = realloc(k->c, sizeof(CtCon) * (size_t)cap); } \
                                 k->c[k->n].a = (A); k->c[k->n].b = (B); k->c[k->n].ub = (U); k->n++; } while (0)
    for (int a = 0; a < V->nvar; a++) {
        if (!ct_tracked(V, a)) continue;
        int64_t lo, hi; bool hl, hh;
        vra_interval(V, &W, a, &lo, &hl, &hi, &hh);
        if (hh && hi < OCT_INF / 2) CT_ADD(a, -1, hi);
        if (hl && lo > -(OCT_INF / 2)) CT_ADD(-1, a, -lo);
        for (int bb = 0; bb < V->nvar; bb++) {
            if (bb == a || !ct_tracked(V, bb)) continue;
            int64_t ub = vra_diff_ub(V, &W, a, bb);
            if (ub < OCT_INF / 2) CT_ADD(a, bb, ub);
        }
    }
    #undef CT_ADD
    ct_constraints += k->n;
    oct_map = saved; free(m);
}

// The concrete value of a tracked id in this frame, or false when there is none yet.
static bool ct_value(Vra *V, IVal *vals, int nv, int id, __int128 *out) {
    if (id < 0 || id >= nv) return false;
    IVal *x = &vals[id];
    IrInstr *d = V->def[id];
    const IrType *t = V->val[id]->type;
    if (d && d->op == IR_ALLOCA) {                           // a cell: read what it holds
        if (x->k != IV_PTR || !x->p.o || !x->p.o->live) return false;
        IPtr p = x->p; IVal *par = &p.o->root;
        if (p.d != 1 || par->k != IV_AGG || p.p[0] < 0 || p.p[0] >= par->n) return false;
        x = &par->e[p.p[0]];
        t = d->aux.alloca_ty;
    }
    if (x->k != IV_INT) return false;
    if (t->kind == IRT_BOOL) { *out = x->i != 0; return true; }
    *out = it_wrap_bits((__int128)x->i, it_bits(t), it_signed(t));
    return true;
}

static void ct_i128(__int128 x, char *buf) {          // exact, for a value outside int64
    char t[48]; int n = 0, j = 0; unsigned __int128 u = x < 0 ? -(unsigned __int128)x : (unsigned __int128)x;
    do { t[n++] = (char)('0' + (int)(u % 10)); u /= 10; } while (u);
    if (x < 0) buf[j++] = '-';
    while (n) buf[j++] = t[--n];
    buf[j] = 0;
}
static void ct_on_block(IrFunc *f, IrBlock *b, IVal *vals, int nv) {
    CtFunc *c = ct_for(f);
    if (!c->V || b->id < 0 || b->id >= c->nblk) return;
    CtBlock *k = &c->blk[b->id];
    k->visits++;
    if (k->visits > 64 && (k->visits & (k->visits - 1))) return;   // 1..64, then powers of two
    if (!k->ready) ct_prepare(c, b);
    if (k->dead)
        ii_fail(99, "INVARIANT VIOLATED", NULL, "bb%d of %.*s runs, but the analysis found it unreachable",
                b->id, (int)f->name->length, f->name->name);
    ct_checked++;
    for (int q = 0; q < k->n; q++) {
        CtCon *cc = &k->c[q]; __int128 va = 0, vb = 0;
        if (cc->a >= 0 && !ct_value(c->V, vals, nv, cc->a, &va)) continue;
        if (cc->b >= 0 && !ct_value(c->V, vals, nv, cc->b, &vb)) continue;
        if (va - vb <= (__int128)cc->ub) continue;
        char lhs[160], sa[48], sb[48], sd[48];
        ct_i128(va, sa); ct_i128(vb, sb); ct_i128(va - vb, sd);
        if (cc->b < 0) snprintf(lhs, sizeof lhs, "%%%d = %s", cc->a, sa);
        else if (cc->a < 0) snprintf(lhs, sizeof lhs, "-%%%d = -(%s)", cc->b, sb);
        else snprintf(lhs, sizeof lhs, "%%%d - %%%d = %s - %s = %s", cc->a, cc->b, sa, sb, sd);
        ii_fail(99, "INVARIANT VIOLATED", NULL, "at the entry of bb%d of %.*s, %s, but the analysis's state there says <= %lld",
                b->id, (int)f->name->length, f->name->name, lhs, (long long)cc->ub);
    }
}

#endif // LAIN_CONTAINMENT_H
