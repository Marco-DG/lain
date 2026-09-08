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

// ── VARIABLE PACKING (rebuild item 2.2) ──────────────────────────────────────
// The octagon's cost is cubic in its dimension and its storage quadratic, and the dimension
// was 2 x EVERY SSA VALUE IN THE FUNCTION — including every element pointer, slice, struct
// and unit, none of which can ever appear in a numeric relation. A 128-element array literal
// gave dim 645, a 3.3 MB matrix PER BLOCK, and a closure of 268M steps.
//
// `oct_map` translates a VALUE id to its packed slot, or −1 for a value that has none. Every
// caller reaches the matrix through oct_pos/oct_neg, so doing it here covers all ~127 call
// sites at once; an unmapped value yields −1 and every accessor treats that as ⊤ (unknown),
// which is exactly the right answer for a value the domain does not track.
static const int *oct_map = NULL;      // NULL = identity (unit tests build raw octagons)
static inline int oct_slot(int i) { return oct_map ? oct_map[i] : i; }

// ── dimension helpers ────────────────────────────────────────────────────────
static inline int oct_pos(int i) { int s = oct_slot(i); return s<0 ? -1 : 2*s;   }
static inline int oct_neg(int i) { int s = oct_slot(i); return s<0 ? -1 : 2*s+1; }
static inline int oct_bar(int d) { return d ^ 1; }   // switch +/−
static inline int64_t oct_min64(int64_t a, int64_t b) { return a<b?a:b; }
static inline int64_t oct_max64(int64_t a, int64_t b) { return a>b?a:b; }
// floor division toward −∞ (C's / truncates toward 0)
static inline int64_t oct_fdiv2(int64_t a) { return a>=0 ? a/2 : -((-a+1)/2); }

// An UNMAPPED dimension (−1) reads as ⊤ and absorbs writes: the domain simply does not
// track that value, which is sound in both directions.
static int64_t oct_sink;
static inline int64_t *oct_at(Octagon *o, int i, int j) {
    if (i<0 || j<0) { oct_sink = OCT_INF; return &oct_sink; }
    return &o->m[(size_t)i*o->dim + j];
}
static inline int64_t  oct_get(const Octagon *o, int i, int j) {
    if (i<0 || j<0) return OCT_INF;
    return o->m[(size_t)i*o->dim + j];
}

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
// ACTIVE SET. The closure is O(dim^3) and `dim` is 2 x (every SSA value in the function),
// but the overwhelming majority of those dimensions are entirely unconstrained: an element
// pointer, a struct, a slice, a value already forgotten. A dimension whose row AND column
// are ⊤ off the diagonal can neither relax another pair (m[i][k] + m[k][j] is ⊤ through it)
// nor be relaxed itself (every path into it is ⊤), so skipping it changes no result.
//
// This is not a micro-optimisation. A 128-element array literal made the closure 645^3 and
// the analysis took THIRTY SECONDS on one small program — the new engine cannot become the
// authoritative middle-end at that cost, and nothing had measured it because the corpus's
// programs are small. Restricting the loops to the active set is exact, not approximate.
static int *oct_active_scratch = NULL; static int oct_active_cap = 0;
static void oct_close(Octagon *o) {
    int d = o->dim;
    if (oct_active_cap < d) {
        oct_active_scratch = (int*)realloc(oct_active_scratch, (size_t)d*sizeof(int));
        oct_active_cap = d;
    }
    int *act = oct_active_scratch, na = 0;
    for (int x=0; x<d; x++) {
        bool any = false;
        for (int y=0; y<d && !any; y++) {
            if (y==x) continue;
            if (oct_get(o,x,y) < OCT_INF || oct_get(o,y,x) < OCT_INF) any = true;
        }
        if (any) act[na++] = x;
    }
    for (int ka=0;ka<na;ka++) { int k=act[ka];
        for (int ia=0;ia<na;ia++) { int i=act[ia];
            int64_t ik = oct_get(o,i,k);
            if (ik >= OCT_INF) continue;
            for (int ja=0;ja<na;ja++) { int j=act[ja];
                int64_t kj = oct_get(o,k,j);
                if (kj >= OCT_INF) continue;
                int64_t s = ik + kj;
                int64_t *ij = oct_at(o,i,j);
                if (s < *ij) *ij = s;
            }
        }
    }
    // strong closure: v_i and v_j both bounded ⇒ tighten their difference using
    // the doubled unary entries. m[i][j] ≤ ⌊m[i][bar i]/2⌋ + ⌊m[bar j][j]/2⌋.
    // A ⊤ dimension has no unary bound, so the active set applies here too.
    for (int ia=0;ia<na;ia++) { int i=act[ia];
        int64_t a = oct_get(o,i,oct_bar(i));
        if (a >= OCT_INF) continue;
        for (int ja=0;ja<na;ja++) { int j=act[ja];
            int64_t b = oct_get(o,oct_bar(j),j);
            if (b >= OCT_INF) continue;
            int64_t s = oct_fdiv2(a) + oct_fdiv2(b);
            int64_t *ij = oct_at(o,i,j);
            if (s < *ij) *ij = s;
        }
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
// Selective widening: widen only entries touching a variable MODIFIED inside the loop
// (mod[v]); for an entry between two loop-invariant variables keep the join `b`. This is
// what stops an outer-loop induction var (invariant in the inner loop, but growing 0..k
// across outer iterations) from being wrongly blown to +∞ at an inner header. Sound: a
// var genuinely modified in the loop is still widened, so termination holds; an invariant
// var's join stabilizes on its own. `mod` NULL ⇒ widen everything (standard widening).
// ★ WIDENING WITH THRESHOLDS. Plain widening sends any bound that grew straight to ⊤, which
// is sound and, on a loop whose bound is a CONSTANT IN THE PROGRAM, needlessly destructive:
//
//     var hi usize = 64
//     while lo < hi { mid = lo + (hi-lo)/2 ; if flag { lo = mid+1 } else { hi = mid } }
//
// `hi ≤ 64` is inductive — hi only ever becomes `mid ≤ hi` — but hi is loop-modified, so the
// first widening threw the 64 away, `lo` became unbounded with it, and `lo + (hi-lo)/2` was
// then a REAL overflow in the abstract state. Give the widening a ladder of candidate values
// (the constants the program itself mentions) and it stops at the first one above the joined
// bound instead of at infinity. Standard technique (Astrée's), and it is what makes an
// ordinary binary search provable.
//
// Sound because any T ≥ the joined value gives a WEAKER constraint, i.e. a larger state; and
// terminating because each entry climbs a finite ladder before reaching ⊤.
static void oct_widen_thr(Octagon *dst, const Octagon *a, const Octagon *b, const char *mod,
                          const int64_t *thr, int nthr) {
    dst->nvar=a->nvar; dst->dim=a->dim;
    for (int i=0;i<a->dim;i++)
        for (int j=0;j<a->dim;j++) {
            int64_t av=oct_get(a,i,j), bv=oct_get(b,i,j);
            bool widen = !mod || mod[i/2] || mod[j/2];   // DBM index i ↔ var i/2
            if (!widen)      { *oct_at(dst,i,j) = bv; continue; }
            if (bv <= av)    { *oct_at(dst,i,j) = av; continue; }   // stable: keep it
            int64_t up = OCT_INF;                                   // else climb the ladder
            for (int t=0; t<nthr; t++) if (thr[t] >= bv) { up = thr[t]; break; }
            *oct_at(dst,i,j) = up;
        }
}
static void oct_widen_sel(Octagon *dst, const Octagon *a, const Octagon *b, const char *mod) {
    oct_widen_thr(dst, a, b, mod, NULL, 0);
}
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
    if (oct_slot(v) < 0) return;                 // untracked: nothing to forget
    // NB: no pre-close. Forgetting without closing is SOUND (it can only lose implied
    // constraints, never invent one) and — as used here, always on a FRESH SSA result
    // being (re)defined — it is also LOSSLESS: a fresh id has no prior constraints for a
    // close to materialize. Closing here was O(dim^3) PER INSTRUCTION (via the per-transfer
    // forget), which made large functions (e.g. a 64-elem array literal, dim≈400) take
    // ~20s. Closure that checks/propagation actually need still happens at block boundaries
    // (the fixpoint's per-block closes) and before every obligation (the final pass).
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
