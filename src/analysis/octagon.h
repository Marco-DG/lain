// src/analysis/octagon.h — the relational numeric domain (Phase 2.1 of the rebuild).
//
// An octagon is a conjunction of constraints  ±x ± y ≤ c  over machine-integer
// variables, reasoned about in ℤ (overflow is a *separate* obligation — see
// design/vra-octagon.md §2.6). It is stored as a Difference Bound Matrix (DBM)
// over 2n "dimensions": each variable v_i gets a positive form (dim 2i, meaning
// +v_i) and a negative form (dim 2i+1, meaning −v_i). Entry m[i][j] is an upper
// bound on  e(j) − e(i)  where e(2k)=+v_k and e(2k+1)=−v_k. Thus:
//
//     v_a − v_b ≤ c   ⟺  m[pos b][pos a] = c        (and its coherent twin)
//     v_a + v_b ≤ c   ⟺  m[neg b][pos a] = c
//     v_a       ≤ c   ⟺  m[neg a][pos a] = 2c       (stored doubled, Miné)
//    −v_a       ≤ c   ⟺  m[pos a][neg a] = 2c
//
// Every constraint has a coherent twin (negate both sides): e(j)−e(i) ≤ c is the
// same as e(bar i)−e(bar j) ≤ c, where bar switches +/−. The setters keep twins
// equal; closure and the lattice ops rely on it. +∞ (OCT_INF) = "no constraint".
// ⊥ (the empty octagon) shows up as a negative diagonal after closure.
//
// γ(m) = { x ∈ ℤⁿ | every encoded constraint holds } is the soundness anchor:
// every operation over-approximates (γ only ever grows relative to the concrete
// set). test_octagon.c checks this by brute force.
#ifndef LAIN_OCTAGON_H
#define LAIN_OCTAGON_H

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

// Large enough to act as +∞ yet leave headroom so INF+INF doesn't overflow.
#define OCT_INF (INT64_MAX/4)

typedef struct {
    int      nvar;   // number of variables
    int      dim;    // 2*nvar
    int64_t *m;      // dim×dim, row-major; m[i*dim+j]
} Octagon;

// ── dimension helpers ────────────────────────────────────────────────────────
static inline int oct_pos(int i) { return 2*i;   }
static inline int oct_neg(int i) { return 2*i+1; }
static inline int oct_bar(int d) { return d ^ 1; }   // switch +/−
static inline int64_t oct_min64(int64_t a, int64_t b) { return a<b?a:b; }
static inline int64_t oct_max64(int64_t a, int64_t b) { return a>b?a:b; }
// floor division toward −∞ (C's / truncates toward 0)
static inline int64_t oct_fdiv2(int64_t a) { return a>=0 ? a/2 : -((-a+1)/2); }

static inline int64_t *oct_at(Octagon *o, int i, int j) { return &o->m[(size_t)i*o->dim + j]; }
static inline int64_t  oct_get(const Octagon *o, int i, int j) { return o->m[(size_t)i*o->dim + j]; }

// ── construction ─────────────────────────────────────────────────────────────
// ⊤ (no constraints): all +∞ off the diagonal, 0 on it.
static void oct_init_top(Octagon *o, int nvar, int64_t *storage) {
    o->nvar = nvar; o->dim = 2*nvar; o->m = storage;
    for (int i=0;i<o->dim;i++)
        for (int j=0;j<o->dim;j++)
            *oct_at(o,i,j) = (i==j) ? 0 : OCT_INF;
}
static void oct_copy(Octagon *dst, const Octagon *src) {
    dst->nvar=src->nvar; dst->dim=src->dim;
    memcpy(dst->m, src->m, (size_t)src->dim*src->dim*sizeof(int64_t));
}

// ── constraint setters (all go through the coherent tighten) ─────────────────
// Tighten m[i][j] (and its coherent twin m[bar j][bar i]) to ≤ c.
static void oct_tighten(Octagon *o, int i, int j, int64_t c) {
    if (c >= OCT_INF) return;
    int64_t *a = oct_at(o,i,j);            if (c < *a) *a = c;
    int bi=oct_bar(j), bj=oct_bar(i);
    int64_t *b = oct_at(o,bi,bj);          if (c < *b) *b = c;
}
static void oct_add_ub(Octagon *o, int v, int64_t c)  { oct_tighten(o, oct_neg(v), oct_pos(v), 2*c);  }   //  v ≤ c
static void oct_add_lb(Octagon *o, int v, int64_t c)  { oct_tighten(o, oct_pos(v), oct_neg(v), -2*c); }   //  v ≥ c
static void oct_add_const(Octagon *o, int v, int64_t c){ oct_add_ub(o,v,c); oct_add_lb(o,v,c); }          //  v = c
// v_a − v_b ≤ c
static void oct_add_diff_le(Octagon *o, int a, int b, int64_t c) { oct_tighten(o, oct_pos(b), oct_pos(a), c); }
// v_a + v_b ≤ c
static void oct_add_sum_le (Octagon *o, int a, int b, int64_t c) { oct_tighten(o, oct_neg(b), oct_pos(a), c); }
// −v_a − v_b ≤ c   (i.e. v_a + v_b ≥ −c)
static void oct_add_negsum_le(Octagon *o, int a, int b, int64_t c){ oct_tighten(o, oct_pos(b), oct_neg(a), c); }

// ── closure ──────────────────────────────────────────────────────────────────
// Shortest-path (Floyd–Warshall) closure over the 2n dimensions, then Miné's
// strong (integer-tight) step folding unary coherence in. Sound and idempotent.
static void oct_close(Octagon *o) {
    int d = o->dim;
    for (int k=0;k<d;k++)
        for (int i=0;i<d;i++) {
            int64_t ik = oct_get(o,i,k);
            if (ik >= OCT_INF) continue;
            for (int j=0;j<d;j++) {
                int64_t kj = oct_get(o,k,j);
                if (kj >= OCT_INF) continue;
                int64_t s = ik + kj;
                int64_t *ij = oct_at(o,i,j);
                if (s < *ij) *ij = s;
            }
        }
    // strong closure: v_i and v_j both bounded ⇒ tighten their difference using
    // the doubled unary entries. m[i][j] ≤ ⌊m[i][bar i]/2⌋ + ⌊m[bar j][j]/2⌋.
    for (int i=0;i<d;i++)
        for (int j=0;j<d;j++) {
            int64_t a = oct_get(o,i,oct_bar(i)), b = oct_get(o,oct_bar(j),j);
            if (a >= OCT_INF || b >= OCT_INF) continue;
            int64_t s = oct_fdiv2(a) + oct_fdiv2(b);
            int64_t *ij = oct_at(o,i,j);
            if (s < *ij) *ij = s;
        }
}

// ⊥ test — a variable's own dimension shows a negative self-distance.
static bool oct_is_bottom(const Octagon *o) {
    for (int i=0;i<o->dim;i++) if (oct_get(o,i,i) < 0) return true;
    return false;
}

// ── lattice operations (operands assumed closed) ─────────────────────────────
// Join ⊔ — entrywise max: the tightest octagon containing both (the φ merge).
static void oct_join(Octagon *dst, const Octagon *a, const Octagon *b) {
    dst->nvar=a->nvar; dst->dim=a->dim;
    for (int i=0;i<a->dim;i++)
        for (int j=0;j<a->dim;j++)
            *oct_at(dst,i,j) = oct_max64(oct_get(a,i,j), oct_get(b,i,j));
}
// Meet ⊓ — entrywise min (adds both constraint sets); caller re-closes.
static void oct_meet(Octagon *dst, const Octagon *a, const Octagon *b) {
    dst->nvar=a->nvar; dst->dim=a->dim;
    for (int i=0;i<a->dim;i++)
        for (int j=0;j<a->dim;j++)
            *oct_at(dst,i,j) = oct_min64(oct_get(a,i,j), oct_get(b,i,j));
}
// Widening ∇ — keep a's entry where b does not exceed it, else drop to +∞.
// Guarantees termination of the ascending chain (no re-closing of the result).
static void oct_widen(Octagon *dst, const Octagon *a, const Octagon *b) {
    dst->nvar=a->nvar; dst->dim=a->dim;
    for (int i=0;i<a->dim;i++)
        for (int j=0;j<a->dim;j++) {
            int64_t av=oct_get(a,i,j), bv=oct_get(b,i,j);
            *oct_at(dst,i,j) = (bv <= av) ? av : OCT_INF;
        }
}
// Order ⊑ — a ⊑ b (a tighter) iff entrywise a ≤ b (operands closed).
static bool oct_leq(const Octagon *a, const Octagon *b) {
    for (int i=0;i<a->dim;i++)
        for (int j=0;j<a->dim;j++)
            if (oct_get(a,i,j) > oct_get(b,i,j)) return false;
    return true;
}

// ── projection & queries ─────────────────────────────────────────────────────
// forget v — drop everything known about v (both its dimensions → ⊤ rows/cols).
// Close first so facts *implied* through v survive among the other variables.
static void oct_forget(Octagon *o, int v) {
    oct_close(o);
    int p=oct_pos(v), n=oct_neg(v);
    for (int k=0;k<o->dim;k++) {
        *oct_at(o,p,k)=OCT_INF; *oct_at(o,k,p)=OCT_INF;
        *oct_at(o,n,k)=OCT_INF; *oct_at(o,k,n)=OCT_INF;
    }
    *oct_at(o,p,p)=0; *oct_at(o,n,n)=0;
}
// Interval [lo,hi] of v from the (doubled) unary entries of a *closed* octagon.
// Returns false for an unbounded side (lo=−∞ / hi=+∞ left untouched by caller).
static void oct_interval(const Octagon *o, int v, int64_t *lo, bool *has_lo,
                                                 int64_t *hi, bool *has_hi) {
    int64_t ub = oct_get(o, oct_neg(v), oct_pos(v));   // 2v ≤ ub
    int64_t lbdoub = oct_get(o, oct_pos(v), oct_neg(v));// -2v ≤ lbdoub
    if (ub < OCT_INF) { *hi = oct_fdiv2(ub); *has_hi = true; } else *has_hi = false;
    if (lbdoub < OCT_INF) { *lo = -oct_fdiv2(lbdoub); *has_lo = true; } else *has_lo = false;
}

#endif // LAIN_OCTAGON_H
