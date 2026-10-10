// src/tools/octagon_ref.h — the STRONG octagon, as src/analysis/octagon.h was before I.89: Miné's
// strong closure writes the strengthening of every pair into the matrix. Kept, renamed, as the
// oracle test_octagon.c checks the sparse implementation against, entry for entry: every value
// the new oct_get reads must be the value this matrix holds. Not used by the compiler. Only the
// operations the oracle replays are kept (no copy, plain widening or interval read).
// src/analysis/octagon.h — the relational numeric domain (Phase 2.1 of the rebuild).
//
// An octagon is a conjunction of constraints ±x ± y ≤ c over machine-integer variables,
// reasoned about in ℤ (overflow is a *separate* obligation — see
// local/internal/design/vra-octagon.md §2.6). It is stored as a Difference Bound Matrix
// (DBM) over 2n "dimensions": each variable v_i gets a positive form (dim 2i, meaning
// +v_i) and a negative form (dim 2i+1, meaning −v_i). Entry m[i][j] is an upper bound on
// e(j) − e(i) where e(2k)=+v_k and e(2k+1)=−v_k. Thus:
//
//     v_a − v_b ≤ c   ⟺  m[pos b][pos a] = c        (and its coherent twin)
//     v_a + v_b ≤ c   ⟺  m[neg b][pos a] = c
//     v_a       ≤ c   ⟺  m[neg a][pos a] = 2c       (stored doubled, Miné)
//    −v_a       ≤ c   ⟺  m[pos a][neg a] = 2c
//
// Every constraint has a coherent twin (negate both sides): e(j)−e(i) ≤ c is the
// same as e(bar i)−e(bar j) ≤ c, where bar switches +/−. The setters keep twins
// equal; closure and the lattice ops rely on it. +∞ (REF_OCT_INF) = "no constraint".
// ⊥ (the empty octagon) shows up as a negative diagonal after closure.
//
// γ(m) = { x ∈ ℤⁿ | every encoded constraint holds } is the soundness anchor:
// every operation over-approximates (γ only ever grows relative to the concrete
// set). test_octagon.c checks this by brute force.
#ifndef LAIN_OCTAGON_REF_H
#define LAIN_OCTAGON_REF_H

#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

// Large enough to act as +∞ yet leave headroom so INF+INF doesn't overflow.
#define REF_OCT_INF (INT64_MAX/4)

typedef struct {
    int      nvar;   // number of variables
    int      dim;    // 2*nvar
    int64_t *m;      // dim×dim, row-major; m[i*dim+j]
    // ★ KNOWN CLOSED: nothing has tightened the matrix since the last ref_oct_close, so closing it
    // again changes nothing (the closure is idempotent). 88% of the analysis was ref_oct_close and
    // about half of those calls were such no-ops. FALSE is the safe value, and the default of
    // every aggregate initializer: a writer that forgets to clear it can only cost a closure's
    // PRECISION (a skipped close leaves valid but looser bounds), never soundness.
    bool     clean;
    // ★ INCREMENTAL: the matrix was closed, and since then only the entries listed in `pend`
    // (each with its coherent twin) have been tightened. Re-closing then needs only the paths
    // through those edges, O(dim²) each, instead of O(dim³): after the known-closed skips, 99%
    // of the remaining closure work was of exactly this shape — a closed state plus the two or
    // three constraints one transfer adds. `incr` false is the safe value (full closure) and the
    // default of every aggregate initializer; ref_oct_set_clean is how a writer that replaces the
    // matrix says what it knows.
    bool     incr;
    int      npend;
    int      pend[16];  // row*dim + col of each tightened entry
} RefOctagon;
#define REF_OCT_PEND_MAX 16
static inline void ref_oct_set_clean(RefOctagon *o, bool closed) { o->clean = closed; o->incr = false; o->npend = 0; }

// ── VARIABLE PACKING (rebuild item 2.2) ──────────────────────────────────────
// The octagon's cost is cubic in its dimension and its storage quadratic, and the dimension
// was 2 x EVERY SSA VALUE IN THE FUNCTION — including every element pointer, slice, struct
// and unit, none of which can ever appear in a numeric relation. A 128-element array literal
// gave dim 645, a 3.3 MB matrix PER BLOCK, and a closure of 268M steps.
//
// `ref_oct_map` translates a VALUE id to its packed slot, or −1 for a value that has none. Every
// caller reaches the matrix through ref_oct_pos/ref_oct_neg, so doing it here covers all ~127 call
// sites at once; an unmapped value yields −1 and every accessor treats that as ⊤ (unknown),
// which is exactly the right answer for a value the domain does not track.
static const int *ref_oct_map = NULL;      // NULL = identity (unit tests build raw octagons)
static inline int ref_oct_slot(int i) { return ref_oct_map ? ref_oct_map[i] : i; }

// ── dimension helpers ────────────────────────────────────────────────────────
static inline int ref_oct_pos(int i) { int s = ref_oct_slot(i); return s<0 ? -1 : 2*s;   }
static inline int ref_oct_neg(int i) { int s = ref_oct_slot(i); return s<0 ? -1 : 2*s+1; }
static inline int ref_oct_bar(int d) { return d ^ 1; }   // switch +/−
static inline int64_t ref_oct_min64(int64_t a, int64_t b) { return a<b?a:b; }
static inline int64_t ref_oct_max64(int64_t a, int64_t b) { return a>b?a:b; }
// floor division toward −∞ (C's / truncates toward 0). It negated `a`, undefined for INT64_MIN
// (I.170, a UBSan-built compiler on nine corpus programs); truncate, then step down for a negative
// odd value, which no input can overflow.
static inline int64_t ref_oct_fdiv2(int64_t a) { int64_t q = a / 2; return (a < 0 && (a & 1)) ? q - 1 : q; }

// An UNMAPPED dimension (−1) reads as ⊤ and absorbs writes: the domain simply does not
// track that value, which is sound in both directions.
static int64_t ref_oct_sink;
static inline int64_t *ref_oct_at(RefOctagon *o, int i, int j) {
    if (i<0 || j<0) { ref_oct_sink = REF_OCT_INF; return &ref_oct_sink; }
    return &o->m[(size_t)i*o->dim + j];
}
static inline int64_t  ref_oct_get(const RefOctagon *o, int i, int j) {
    if (i<0 || j<0) return REF_OCT_INF;
    return o->m[(size_t)i*o->dim + j];
}

// ── construction ─────────────────────────────────────────────────────────────
// ⊤ (no constraints): all +∞ off the diagonal, 0 on it.
static void ref_oct_init_top(RefOctagon *o, int nvar, int64_t *storage) {
    o->nvar = nvar; o->dim = 2*nvar; o->m = storage; ref_oct_set_clean(o, false);
    for (int i=0;i<o->dim;i++)
        for (int j=0;j<o->dim;j++)
            *ref_oct_at(o,i,j) = (i==j) ? 0 : REF_OCT_INF;
}

// ── constraint setters (all go through the coherent tighten) ─────────────────
// Tighten m[i][j] (and its coherent twin m[bar j][bar i]) to ≤ c.
static void ref_oct_tighten(RefOctagon *o, int i, int j, int64_t c) {
    if (c >= REF_OCT_INF || i < 0 || j < 0) return;          // an untracked dimension absorbs it
    bool changed = false;
    int64_t *a = ref_oct_at(o,i,j);            if (c < *a) { *a = c; changed = true; }
    int bi=ref_oct_bar(j), bj=ref_oct_bar(i);
    int64_t *b = ref_oct_at(o,bi,bj);          if (c < *b) { *b = c; changed = true; }
    if (!changed) return;
    if (o->clean) { o->clean = false; o->incr = true; o->npend = 0; }
    if (!o->incr) return;
    if (o->npend < REF_OCT_PEND_MAX) o->pend[o->npend++] = i*o->dim + j;
    else o->incr = false;                                 // too many: the full closure is cheaper
}
// ★ A UNARY BOUND IS STORED DOUBLED, so it must be clamped BEFORE the doubling. `2*c` on
// c = INT64_MAX is −2 in two's complement (and undefined in C): `var x i64 = 9223372036854775807`
// was recorded as x ≤ −1, `if x < 0 { return }` read as always taken, and the rest of the
// function was dead — a division by zero after it compiled. i64::MIN became x ≥ 0 the same way.
// Outside ±REF_OCT_INF/2 a bound is WEAKENED toward the representable range, never strengthened:
// an upper bound above it is dropped and one below it raised to the floor; a lower bound the
// mirror image. The constant table keeps the exact value for every reader that asks it.
#define REF_OCT_BOUND_MAX (REF_OCT_INF/2 - 1)
static void ref_oct_add_ub(RefOctagon *o, int v, int64_t c)  {                                         //  v ≤ c
    if (c > REF_OCT_BOUND_MAX) return;
    if (c < -REF_OCT_BOUND_MAX) c = -REF_OCT_BOUND_MAX;
    ref_oct_tighten(o, ref_oct_neg(v), ref_oct_pos(v), 2*c);
}
static void ref_oct_add_lb(RefOctagon *o, int v, int64_t c)  {                                         //  v ≥ c
    if (c < -REF_OCT_BOUND_MAX) return;
    if (c > REF_OCT_BOUND_MAX) c = REF_OCT_BOUND_MAX;
    ref_oct_tighten(o, ref_oct_pos(v), ref_oct_neg(v), -2*c);
}
static void ref_oct_add_const(RefOctagon *o, int v, int64_t c){ ref_oct_add_ub(o,v,c); ref_oct_add_lb(o,v,c); }          //  v = c
// v_a − v_b ≤ c
static void ref_oct_add_diff_le(RefOctagon *o, int a, int b, int64_t c) { ref_oct_tighten(o, ref_oct_pos(b), ref_oct_pos(a), c); }
// v_a + v_b ≤ c
static void ref_oct_add_sum_le (RefOctagon *o, int a, int b, int64_t c) { ref_oct_tighten(o, ref_oct_neg(b), ref_oct_pos(a), c); }
// −v_a − v_b ≤ c   (i.e. v_a + v_b ≥ −c)
static void ref_oct_add_negsum_le(RefOctagon *o, int a, int b, int64_t c){ ref_oct_tighten(o, ref_oct_pos(b), ref_oct_neg(a), c); }

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
static int *ref_oct_active_scratch = NULL; static int ref_oct_active_cap = 0;

// INCREMENTAL CLOSURE of a closed matrix after the pending tightenings (Bagnara, Hill, Zaffanella,
// "An improved tight closure algorithm for integer octagonal constraints", 2008). Each pending
// entry is the edge i→j of weight c together with its coherent twin bar j→bar i. The matrix was
// closed without them, so a shortest path uses each at most once, and
//   a ⇝ j through a new edge:     P1[a] = min(m[a][i] + c,  m[a][bar j] + c + m[bar i][i] + c)
//   a ⇝ bar i through a new edge: P2[a] = min(m[a][bar j] + c,  m[a][i] + c + m[j][bar j] + c)
//   m[a][b] = min(m[a][b], P1[a] + m[j][b], P2[a] + m[bar i][b])
// all read before the edge's update. Processing the edges one after another is exact even though
// the later ones are still in the matrix raw: each step's matrix is sound and at least as tight
// as the closure so far. The shortest-path closure is then tightened and strengthened by the SAME
// pass as the full closure, which is Bagnara's tight closure: the two agree entry for entry on a
// satisfiable octagon (src/tools/test_octagon.c checks it on random ones). Every entry written
// is a real path's length, so the result is sound whatever the inputs.
// It returns false, the matrix restored, and the caller closes in full, in two cases:
//  • a NEGATIVE CYCLE (⊥). Floyd–Warshall spreads it through every entry it reaches, this
//    update only along the new edges; both are ⊥, but a query in the dead code after an
//    infeasible `assume` read the two differently (tests/vra/invariant/struct_in_fixed_ctor_fail
//    gained an E085). Every entry the path phase writes is logged, and undone. A ⊥ that only
//    the integer strengthening produces needs no undo: without a negative cycle the path
//    closure is canonical, and the strengthening pass below is the full closure's own.
//  • an entry within a factor 4 of -REF_OCT_INF, where a sum of four could overflow.
static int64_t *ref_oct_incr_vec = NULL; static int ref_oct_incr_cap = 0;
static int64_t *ref_oct_undo = NULL; static size_t ref_oct_undo_n = 0, ref_oct_undo_cap = 0;
static void ref_oct_undo_all(RefOctagon *o) {
    while (ref_oct_undo_n > 0) { ref_oct_undo_n -= 2; o->m[ref_oct_undo[ref_oct_undo_n]] = ref_oct_undo[ref_oct_undo_n+1]; }
}
static bool ref_oct_close_incr(RefOctagon *o) {
    int d = o->dim;
    if (ref_oct_incr_cap < d) {
        ref_oct_incr_vec = (int64_t*)realloc(ref_oct_incr_vec, (size_t)4*d*sizeof(int64_t));
        ref_oct_incr_cap = d;
    }
    if (ref_oct_active_cap < 2*d) {
        ref_oct_active_scratch = (int*)realloc(ref_oct_active_scratch, (size_t)2*d*sizeof(int));
        ref_oct_active_cap = 2*d;
    }
    int64_t *P1 = ref_oct_incr_vec, *P2 = P1 + d, *B1 = P2 + d, *B2 = B1 + d;
    int *rows = ref_oct_active_scratch, *cols = rows + d;
    const int64_t LO = -(REF_OCT_INF/4);
    ref_oct_undo_n = 0;
    for (int t = 0; t < o->npend; t++) {
        int i = o->pend[t] / d, j = o->pend[t] % d;
        int64_t c = ref_oct_get(o, i, j);                 // current: a forget (⊤) or a later tighten wins
        if (c >= REF_OCT_INF) continue;
        int bi = ref_oct_bar(i), bj = ref_oct_bar(j);
        int64_t jj = ref_oct_get(o, j, bj), ii = ref_oct_get(o, bi, i);
        if (c < LO || (jj < REF_OCT_INF && jj < LO) || (ii < REF_OCT_INF && ii < LO)) { ref_oct_undo_all(o); return false; }
        int nr = 0, nc = 0;
        for (int a = 0; a < d; a++) {
            int64_t ai = ref_oct_get(o, a, i), abj = ref_oct_get(o, a, bj);
            int64_t jb = ref_oct_get(o, j, a), bib = ref_oct_get(o, bi, a);
            if ((ai < REF_OCT_INF && ai < LO) || (abj < REF_OCT_INF && abj < LO) ||
                (jb < REF_OCT_INF && jb < LO) || (bib < REF_OCT_INF && bib < LO)) { ref_oct_undo_all(o); return false; }
            int64_t p1 = REF_OCT_INF, p2 = REF_OCT_INF;
            if (ai  < REF_OCT_INF) { p1 = ai + c;  if (jj < REF_OCT_INF) p2 = ai + c + jj + c; }
            if (abj < REF_OCT_INF) { int64_t q = abj + c; if (q < p2) p2 = q;
                                 if (ii < REF_OCT_INF) { q = abj + c + ii + c; if (q < p1) p1 = q; } }
            P1[a] = p1 < REF_OCT_INF ? p1 : REF_OCT_INF;  P2[a] = p2 < REF_OCT_INF ? p2 : REF_OCT_INF;
            B1[a] = jb; B2[a] = bib;
            if (P1[a] < REF_OCT_INF || P2[a] < REF_OCT_INF) rows[nr++] = a;
            if (jb < REF_OCT_INF || bib < REF_OCT_INF) cols[nc++] = a;
        }
        for (int ra = 0; ra < nr; ra++) { int a = rows[ra];
            int64_t p1 = P1[a], p2 = P2[a], *row = &o->m[(size_t)a*d];
            for (int cb = 0; cb < nc; cb++) { int b = cols[cb];
                int64_t sp = REF_OCT_INF;
                if (p1 < REF_OCT_INF && B1[b] < REF_OCT_INF) sp = p1 + B1[b];
                if (p2 < REF_OCT_INF && B2[b] < REF_OCT_INF) { int64_t q = p2 + B2[b]; if (q < sp) sp = q; }
                if (sp < row[b]) {
                    if (ref_oct_undo_n + 2 > ref_oct_undo_cap) {
                        ref_oct_undo_cap = ref_oct_undo_cap ? 2*ref_oct_undo_cap : 1024;
                        ref_oct_undo = (int64_t*)realloc(ref_oct_undo, ref_oct_undo_cap*sizeof(int64_t));
                    }
                    ref_oct_undo[ref_oct_undo_n++] = (int64_t)a*d + b; ref_oct_undo[ref_oct_undo_n++] = row[b];
                    row[b] = sp;
                }
            }
        }
    }
    for (int x = 0; x < d; x++)
        if (ref_oct_get(o, x, x) < 0) { ref_oct_undo_all(o); return false; }   // a negative cycle: ⊥
    // tighten + strengthen: the full closure's pass, over the dimensions that have a unary bound
    // (every one of them is in the full closure's active set, and no other contributes).
    int nu = 0, nv = 0;
    for (int x = 0; x < d; x++) {
        if (ref_oct_get(o, x, ref_oct_bar(x)) < REF_OCT_INF) rows[nu++] = x;
        if (ref_oct_get(o, ref_oct_bar(x), x) < REF_OCT_INF) cols[nv++] = x;
    }
    for (int ia = 0; ia < nu; ia++) { int i = rows[ia];
        int64_t a = ref_oct_get(o, i, ref_oct_bar(i));
        for (int ja = 0; ja < nv; ja++) { int j = cols[ja];
            int64_t b = ref_oct_get(o, ref_oct_bar(j), j);
            int64_t s = ref_oct_fdiv2(a) + ref_oct_fdiv2(b);
            int64_t *ij = ref_oct_at(o, i, j);
            if (s < *ij) *ij = s;
        }
    }
    return true;
}

static void ref_oct_close(RefOctagon *o) {
    if (o->clean) return;                  // closed, and nothing tightened since
    if (o->incr && ref_oct_close_incr(o)) { ref_oct_set_clean(o, true); return; }
    int d = o->dim;
    if (ref_oct_active_cap < d) {
        ref_oct_active_scratch = (int*)realloc(ref_oct_active_scratch, (size_t)d*sizeof(int));
        ref_oct_active_cap = d;
    }
    int *act = ref_oct_active_scratch, na = 0;
    for (int x=0; x<d; x++) {
        bool any = false;
        for (int y=0; y<d && !any; y++) {
            if (y==x) continue;
            if (ref_oct_get(o,x,y) < REF_OCT_INF || ref_oct_get(o,y,x) < REF_OCT_INF) any = true;
        }
        if (any) act[na++] = x;
    }
    for (int ka=0;ka<na;ka++) { int k=act[ka];
        for (int ia=0;ia<na;ia++) { int i=act[ia];
            int64_t ik = ref_oct_get(o,i,k);
            if (ik >= REF_OCT_INF) continue;
            for (int ja=0;ja<na;ja++) { int j=act[ja];
                int64_t kj = ref_oct_get(o,k,j);
                if (kj >= REF_OCT_INF) continue;
                // Entries are capped above by REF_OCT_INF, not below, and two large negative ones
                // overflowed here (undefined in C; I.170, found by a UBSan-built compiler). The true
                // sum is below INT64_MIN: any value above it is a weaker, still sound, bound.
                int64_t s;
                if (__builtin_add_overflow(ik, kj, &s)) s = INT64_MIN / 2;
                int64_t *ij = ref_oct_at(o,i,j);
                if (s < *ij) *ij = s;
            }
        }
    }
    // strong closure: v_i and v_j both bounded ⇒ tighten their difference using
    // the doubled unary entries. m[i][j] ≤ ⌊m[i][bar i]/2⌋ + ⌊m[bar j][j]/2⌋.
    // A ⊤ dimension has no unary bound, so the active set applies here too.
    for (int ia=0;ia<na;ia++) { int i=act[ia];
        int64_t a = ref_oct_get(o,i,ref_oct_bar(i));
        if (a >= REF_OCT_INF) continue;
        for (int ja=0;ja<na;ja++) { int j=act[ja];
            int64_t b = ref_oct_get(o,ref_oct_bar(j),j);
            if (b >= REF_OCT_INF) continue;
            int64_t s;                                   // each half is at least -2^62 (I.170)
            if (__builtin_add_overflow(ref_oct_fdiv2(a), ref_oct_fdiv2(b), &s)) s = INT64_MIN / 2;
            int64_t *ij = ref_oct_at(o,i,j);
            if (s < *ij) *ij = s;
        }
    }
    ref_oct_set_clean(o, true);
}

// ⊥ test — a variable's own dimension shows a negative self-distance.
static bool ref_oct_is_bottom(const RefOctagon *o) {
    for (int i=0;i<o->dim;i++) if (ref_oct_get(o,i,i) < 0) return true;
    return false;
}

// ── lattice operations (operands assumed closed) ─────────────────────────────
// Join ⊔ — entrywise max: the tightest octagon containing both (the φ merge).
static void ref_oct_join(RefOctagon *dst, const RefOctagon *a, const RefOctagon *b) {
    dst->nvar=a->nvar; dst->dim=a->dim; ref_oct_set_clean(dst, false);
    for (int i=0;i<a->dim;i++)
        for (int j=0;j<a->dim;j++)
            *ref_oct_at(dst,i,j) = ref_oct_max64(ref_oct_get(a,i,j), ref_oct_get(b,i,j));
}
// Meet ⊓ — entrywise min (adds both constraint sets); caller re-closes.
static void ref_oct_meet(RefOctagon *dst, const RefOctagon *a, const RefOctagon *b) {
    dst->nvar=a->nvar; dst->dim=a->dim; ref_oct_set_clean(dst, false);
    for (int i=0;i<a->dim;i++)
        for (int j=0;j<a->dim;j++)
            *ref_oct_at(dst,i,j) = ref_oct_min64(ref_oct_get(a,i,j), ref_oct_get(b,i,j));
}
// Widening ∇ — keep a's entry where b does not exceed it, else drop to +∞.
// Guarantees termination of the ascending chain (no re-closing of the result).
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
static void ref_oct_widen_thr(RefOctagon *dst, const RefOctagon *a, const RefOctagon *b, const char *mod,
                          const int64_t *thr, int nthr) {
    dst->nvar=a->nvar; dst->dim=a->dim; ref_oct_set_clean(dst, false);
    for (int i=0;i<a->dim;i++)
        for (int j=0;j<a->dim;j++) {
            int64_t av=ref_oct_get(a,i,j), bv=ref_oct_get(b,i,j);
            bool widen = !mod || mod[i/2] || mod[j/2];   // DBM index i ↔ var i/2
            if (!widen)      { *ref_oct_at(dst,i,j) = bv; continue; }
            if (bv <= av)    { *ref_oct_at(dst,i,j) = av; continue; }   // stable: keep it
            int64_t up = REF_OCT_INF;                                   // else climb the ladder
            for (int t=0; t<nthr; t++) if (thr[t] >= bv) { up = thr[t]; break; }
            *ref_oct_at(dst,i,j) = up;
        }
}
static bool ref_oct_leq(const RefOctagon *a, const RefOctagon *b) {
    for (int i=0;i<a->dim;i++)
        for (int j=0;j<a->dim;j++)
            if (ref_oct_get(a,i,j) > ref_oct_get(b,i,j)) return false;
    return true;
}

// ── projection & queries ─────────────────────────────────────────────────────
// forget v — drop everything known about v (both its dimensions → ⊤ rows/cols).
// Close first so facts *implied* through v survive among the other variables.
static void ref_oct_forget(RefOctagon *o, int v) {
    if (ref_oct_slot(v) < 0) return;                 // untracked: nothing to forget
    // NB: no pre-close. Forgetting without closing is SOUND (it can only lose implied
    // constraints, never invent one) and — as used here, always on a FRESH SSA result
    // being (re)defined — it is also LOSSLESS: a fresh id has no prior constraints for a
    // close to materialize. Closing here was O(dim^3) PER INSTRUCTION (via the per-transfer
    // forget), which made large functions (e.g. a 64-elem array literal, dim≈400) take
    // ~20s. Closure that checks/propagation actually need still happens at block boundaries
    // (the fixpoint's per-block closes) and before every obligation (the final pass).
    int p=ref_oct_pos(v), n=ref_oct_neg(v);
    for (int k=0;k<o->dim;k++) {
        *ref_oct_at(o,p,k)=REF_OCT_INF; *ref_oct_at(o,k,p)=REF_OCT_INF;
        *ref_oct_at(o,n,k)=REF_OCT_INF; *ref_oct_at(o,k,n)=REF_OCT_INF;
    }
    *ref_oct_at(o,p,p)=0; *ref_oct_at(o,n,n)=0;
}

#endif // LAIN_OCTAGON_REF_H
