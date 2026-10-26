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
// equal; closure and the lattice ops rely on it. +∞ (OCT_INF) = "no constraint".
// ⊥ (the empty octagon) shows up as a negative diagonal after closure.
//
// ★ STRENGTHENING IS IMPLICIT (I.89). Two bounded variables imply a bound on their sum and their
// difference: e(j) − e(i) ≤ ⌊m[i][bar i]/2⌋ + ⌊m[bar j][j]/2⌋. Miné's strong closure WROTE that
// value into every pair, so every bounded value was related to every other, the matrix was dense
// in all of them, and each incremental closure cost dim² however few real relations there were:
// a function of 240 straight-line calls took 38 s, cubic in its length. The matrix now stores
// only what the paths derive (the closure tightens the unary entries and stops), and oct_get, the
// accessor every reader goes through, adds the strengthening when it reads. Reading gives the
// same value the strongly closed matrix held (Bagnara, Hill, Zaffanella 2008: tight closure is
// shortest paths, then tightening, then ONE strengthening, which the read applies). The lattice
// operations work on the read values and store back only the pairs tighter than their two unary
// bounds imply, which is exactly the join's "crossing" pairs (Jourdan's weakly closed octagons,
// Crab's split normal form). src/tools/test_octagon.c checks every operation against the strong
// implementation it replaced (src/tools/octagon_ref.h), entry for entry.
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
#include "../utils/common/predef.h"   // maybe_unused: oct_copy/meet/widen serve test_octagon.c, not the compiler

// Large enough to act as +∞ yet leave headroom so INF+INF doesn't overflow.
#define OCT_INF (INT64_MAX/4)

typedef struct {
    int      nvar;   // number of variables
    int      dim;    // 2*nvar
    int64_t *m;      // dim×dim, row-major; m[i*dim+j]
    // ★ KNOWN CLOSED: nothing has tightened the matrix since the last oct_close, so closing it
    // again changes nothing (the closure is idempotent). 88% of the analysis was oct_close and
    // about half of those calls were such no-ops. FALSE is the safe value, and the default of
    // every aggregate initializer: a writer that forgets to clear it can only cost a closure's
    // PRECISION (a skipped close leaves valid but looser bounds), never soundness.
    bool     clean;
    // ★ INCREMENTAL: the matrix was closed, and since then only the entries listed in `pend`
    // (each with its coherent twin) have been tightened. Re-closing then needs only the paths
    // through those edges, O(dim²) each, instead of O(dim³): after the known-closed skips, 99%
    // of the remaining closure work was of exactly this shape — a closed state plus the two or
    // three constraints one transfer adds. `incr` false is the safe value (full closure) and the
    // default of every aggregate initializer; oct_set_clean is how a writer that replaces the
    // matrix says what it knows.
    bool     incr;
    int      npend;
    int      pend[16];  // row*dim + col of each tightened entry
} Octagon;
#define OCT_PEND_MAX 16
static inline void oct_set_clean(Octagon *o, bool closed) { o->clean = closed; o->incr = false; o->npend = 0; }

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
// floor division toward −∞ (C's / truncates toward 0). It negated `a`, undefined for INT64_MIN
// (I.170, a UBSan-built compiler on nine corpus programs); truncate, then step down for a negative
// odd value, which no input can overflow.
static inline int64_t oct_fdiv2(int64_t a) { int64_t q = a / 2; return (a < 0 && (a & 1)) ? q - 1 : q; }

// An UNMAPPED dimension (−1) reads as ⊤ and absorbs writes: the domain simply does not
// track that value, which is sound in both directions.
static int64_t oct_sink;
static inline int64_t *oct_at(Octagon *o, int i, int j) {
    if (i<0 || j<0) { oct_sink = OCT_INF; return &oct_sink; }
    return &o->m[(size_t)i*o->dim + j];
}
// The STORED entry: what the paths derived, without the strengthening. Only this file reads it.
static inline int64_t  oct_raw(const Octagon *o, int i, int j) {
    if (i<0 || j<0) return OCT_INF;
    return o->m[(size_t)i*o->dim + j];
}
// ⌊a/2⌋ + ⌊b/2⌋ for two finite entries, each half at least −2^62 (I.170); +∞ if either is.
static inline int64_t oct_halves(int64_t a, int64_t b) {
    if (a >= OCT_INF || b >= OCT_INF) return OCT_INF;
    int64_t s;
    if (__builtin_add_overflow(oct_fdiv2(a), oct_fdiv2(b), &s)) s = INT64_MIN / 2;
    return s;
}
// The bound on e(j) − e(i) that the octagon implies: the stored entry, or the strengthening of
// the two unary bounds, whichever is tighter. On a closed octagon it is the strongly closed
// matrix's entry. Every reader outside this file asks this.
static inline int64_t  oct_get(const Octagon *o, int i, int j) {
    if (i<0 || j<0) return OCT_INF;
    int64_t m = o->m[(size_t)i*o->dim + j];
    if (i == j) return m;
    int64_t s = oct_halves(o->m[(size_t)i*o->dim + oct_bar(i)], o->m[(size_t)oct_bar(j)*o->dim + j]);
    return s < m ? s : m;
}

// ── construction ─────────────────────────────────────────────────────────────
// ⊤ (no constraints): all +∞ off the diagonal, 0 on it.
static void oct_init_top(Octagon *o, int nvar, int64_t *storage) {
    o->nvar = nvar; o->dim = 2*nvar; o->m = storage; oct_set_clean(o, false);
    for (int i=0;i<o->dim;i++)
        for (int j=0;j<o->dim;j++)
            *oct_at(o,i,j) = (i==j) ? 0 : OCT_INF;
}
static maybe_unused void oct_copy(Octagon *dst, const Octagon *src) {
    dst->nvar=src->nvar; dst->dim=src->dim; oct_set_clean(dst, false);
    memcpy(dst->m, src->m, (size_t)src->dim*src->dim*sizeof(int64_t));
}

// ── constraint setters (all go through the coherent tighten) ─────────────────
// Tighten m[i][j] (and its coherent twin m[bar j][bar i]) to ≤ c.
static void oct_tighten(Octagon *o, int i, int j, int64_t c) {
    if (c >= OCT_INF || i < 0 || j < 0) return;          // an untracked dimension absorbs it
    bool changed = false;
    int64_t *a = oct_at(o,i,j);            if (c < *a) { *a = c; changed = true; }
    int bi=oct_bar(j), bj=oct_bar(i);
    int64_t *b = oct_at(o,bi,bj);          if (c < *b) { *b = c; changed = true; }
    if (!changed) return;
    if (o->clean) { o->clean = false; o->incr = true; o->npend = 0; }
    if (!o->incr) return;
    if (o->npend < OCT_PEND_MAX) o->pend[o->npend++] = i*o->dim + j;
    else o->incr = false;                                 // too many: the full closure is cheaper
}
// ★ A UNARY BOUND IS STORED DOUBLED, so it must be clamped BEFORE the doubling. `2*c` on
// c = INT64_MAX is −2 in two's complement (and undefined in C): `var x i64 = 9223372036854775807`
// was recorded as x ≤ −1, `if x < 0 { return }` read as always taken, and the rest of the
// function was dead — a division by zero after it compiled. i64::MIN became x ≥ 0 the same way.
// Outside ±OCT_INF/2 a bound is WEAKENED toward the representable range, never strengthened:
// an upper bound above it is dropped and one below it raised to the floor; a lower bound the
// mirror image. The constant table keeps the exact value for every reader that asks it.
#define OCT_BOUND_MAX (OCT_INF/2 - 1)
static void oct_add_ub(Octagon *o, int v, int64_t c)  {                                         //  v ≤ c
    if (c > OCT_BOUND_MAX) return;
    if (c < -OCT_BOUND_MAX) c = -OCT_BOUND_MAX;
    oct_tighten(o, oct_neg(v), oct_pos(v), 2*c);
}
static void oct_add_lb(Octagon *o, int v, int64_t c)  {                                         //  v ≥ c
    if (c < -OCT_BOUND_MAX) return;
    if (c > OCT_BOUND_MAX) c = OCT_BOUND_MAX;
    oct_tighten(o, oct_pos(v), oct_neg(v), -2*c);
}
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

// INCREMENTAL CLOSURE of a closed matrix after the pending tightenings: IncClose (Chawdhary, Robbins
// and King, "Simple and efficient algorithms for octagons", APLAS 2014), whose theorem is for ONE
// new constraint; applying it to the pending edges one after another (up to 16) rests on our own
// argument, below. Each pending entry is the edge i→j of weight c together with its coherent twin
// bar j→bar i. The matrix was closed without them, so a shortest path uses each at most once, and
//   a ⇝ j through a new edge:     P1[a] = min(m[a][i] + c,  m[a][bar j] + c + m[bar i][i] + c)
//   a ⇝ bar i through a new edge: P2[a] = min(m[a][bar j] + c,  m[a][i] + c + m[j][bar j] + c)
//   m[a][b] = min(m[a][b], P1[a] + m[j][b], P2[a] + m[bar i][b])
// all read before the edge's update. Processing the edges one after another is exact even though
// the later ones are still in the matrix raw: each step's matrix is sound and at least as tight
// as the closure so far. The shortest-path closure is then tightened and strengthened by the SAME
// pass as the full closure, which is Bagnara, Hill and Zaffanella's tight closure (2008): the two
// agree entry for entry on a
// satisfiable octagon (src/tools/test_octagon.c checks it on random ones). Every entry written
// is a real path's length, so the result is sound whatever the inputs.
// It returns false, the matrix restored, and the caller closes in full, in two cases:
//  • a NEGATIVE CYCLE (⊥). Floyd–Warshall spreads it through every entry it reaches, this
//    update only along the new edges; both are ⊥, but a query in the dead code after an
//    infeasible `assume` read the two differently (tests/vra/invariant/struct_in_fixed_ctor_fail
//    gained an E085). Every entry the path phase writes is logged, and undone. A ⊥ that only
//    the integer strengthening produces needs no undo: without a negative cycle the path
//    closure is canonical, and the strengthening pass below is the full closure's own.
//  • an entry within a factor 4 of -OCT_INF, where a sum of four could overflow.
// The end of a closure: every unary entry even (2x <= c is x <= ⌊c/2⌋ over the integers), and an
// integer ⊥ that only the tightening shows (2x <= 1 and −2x <= −1) written on the diagonal, where
// oct_is_bottom looks. The strengthening of every PAIR is what oct_get adds when it reads.
static void oct_tighten_unary(Octagon *o) {
    int d = o->dim;
    for (int x = 0; x < d; x++) {
        int64_t *u = &o->m[(size_t)x*d + oct_bar(x)];
        if (*u < OCT_INF) *u = 2 * oct_fdiv2(*u);
    }
    int64_t lo = 0;
    for (int x = 0; x < d; x++) {
        int64_t s = oct_halves(o->m[(size_t)x*d + oct_bar(x)], o->m[(size_t)oct_bar(x)*d + x]);
        if (s < o->m[(size_t)x*d + x]) o->m[(size_t)x*d + x] = s;
        if (o->m[(size_t)x*d + x] < lo) lo = o->m[(size_t)x*d + x];
    }
    // ⊥ on EVERY diagonal. The strong closure spread a contradiction through the strengthening into
    // every bounded pair, so forgetting one variable left the octagon empty; marked on the offending
    // variable's diagonal alone, a forget of that variable made a dead state live again.
    if (lo < 0) for (int x = 0; x < d; x++) if (o->m[(size_t)x*d + x] > lo) o->m[(size_t)x*d + x] = lo;
}

static int64_t *oct_incr_vec = NULL; static int oct_incr_cap = 0;
static int64_t *oct_undo = NULL; static size_t oct_undo_n = 0, oct_undo_cap = 0;
static void oct_undo_all(Octagon *o) {
    while (oct_undo_n > 0) { oct_undo_n -= 2; o->m[oct_undo[oct_undo_n]] = oct_undo[oct_undo_n+1]; }
}
static bool oct_close_incr(Octagon *o) {
    int d = o->dim;
    if (oct_incr_cap < d) {
        oct_incr_vec = (int64_t*)realloc(oct_incr_vec, (size_t)4*d*sizeof(int64_t));
        oct_incr_cap = d;
    }
    if (oct_active_cap < 2*d) {
        oct_active_scratch = (int*)realloc(oct_active_scratch, (size_t)2*d*sizeof(int));
        oct_active_cap = 2*d;
    }
    int64_t *P1 = oct_incr_vec, *P2 = P1 + d, *B1 = P2 + d, *B2 = B1 + d;
    int *rows = oct_active_scratch, *cols = rows + d;
    const int64_t LO = -(OCT_INF/4);
    oct_undo_n = 0;
    for (int t = 0; t < o->npend; t++) {
        int i = o->pend[t] / d, j = o->pend[t] % d;
        int64_t c = oct_raw(o, i, j);                 // current: a forget (⊤) or a later tighten wins
        if (c >= OCT_INF) continue;
        int bi = oct_bar(i), bj = oct_bar(j);
        int64_t jj = oct_raw(o, j, bj), ii = oct_raw(o, bi, i);
        if (c < LO || (jj < OCT_INF && jj < LO) || (ii < OCT_INF && ii < LO)) { oct_undo_all(o); return false; }
        int nr = 0, nc = 0;
        for (int a = 0; a < d; a++) {
            int64_t ai = oct_raw(o, a, i), abj = oct_raw(o, a, bj);
            int64_t jb = oct_raw(o, j, a), bib = oct_raw(o, bi, a);
            if ((ai < OCT_INF && ai < LO) || (abj < OCT_INF && abj < LO) ||
                (jb < OCT_INF && jb < LO) || (bib < OCT_INF && bib < LO)) { oct_undo_all(o); return false; }
            int64_t p1 = OCT_INF, p2 = OCT_INF;
            if (ai  < OCT_INF) { p1 = ai + c;  if (jj < OCT_INF) p2 = ai + c + jj + c; }
            if (abj < OCT_INF) { int64_t q = abj + c; if (q < p2) p2 = q;
                                 if (ii < OCT_INF) { q = abj + c + ii + c; if (q < p1) p1 = q; } }
            P1[a] = p1 < OCT_INF ? p1 : OCT_INF;  P2[a] = p2 < OCT_INF ? p2 : OCT_INF;
            B1[a] = jb; B2[a] = bib;
            if (P1[a] < OCT_INF || P2[a] < OCT_INF) rows[nr++] = a;
            if (jb < OCT_INF || bib < OCT_INF) cols[nc++] = a;
        }
        for (int ra = 0; ra < nr; ra++) { int a = rows[ra];
            int64_t p1 = P1[a], p2 = P2[a], *row = &o->m[(size_t)a*d];
            for (int cb = 0; cb < nc; cb++) { int b = cols[cb];
                int64_t sp = OCT_INF;
                if (p1 < OCT_INF && B1[b] < OCT_INF) sp = p1 + B1[b];
                if (p2 < OCT_INF && B2[b] < OCT_INF) { int64_t q = p2 + B2[b]; if (q < sp) sp = q; }
                if (sp < row[b]) {
                    if (oct_undo_n + 2 > oct_undo_cap) {
                        oct_undo_cap = oct_undo_cap ? 2*oct_undo_cap : 1024;
                        oct_undo = (int64_t*)realloc(oct_undo, oct_undo_cap*sizeof(int64_t));
                    }
                    oct_undo[oct_undo_n++] = (int64_t)a*d + b; oct_undo[oct_undo_n++] = row[b];
                    row[b] = sp;
                }
            }
        }
    }
    for (int x = 0; x < d; x++)
        if (oct_raw(o, x, x) < 0) { oct_undo_all(o); return false; }   // a negative cycle: ⊥
    oct_tighten_unary(o);
    return true;
}

static void oct_close(Octagon *o) {
    if (o->clean) return;                  // closed, and nothing tightened since
    if (o->incr && oct_close_incr(o)) { oct_set_clean(o, true); return; }
    int d = o->dim;
    if (oct_active_cap < d) {
        oct_active_scratch = (int*)realloc(oct_active_scratch, (size_t)d*sizeof(int));
        oct_active_cap = d;
    }
    if (oct_active_cap < 3*d) {
        oct_active_scratch = (int*)realloc(oct_active_scratch, (size_t)3*d*sizeof(int));
        oct_active_cap = 3*d;
    }
    int *act = oct_active_scratch, na = 0;
    for (int x=0; x<d; x++) {
        bool any = false;
        for (int y=0; y<d && !any; y++) {
            if (y==x) continue;
            if (oct_raw(o,x,y) < OCT_INF || oct_raw(o,y,x) < OCT_INF) any = true;
        }
        if (any) act[na++] = x;
    }
    // Floyd–Warshall over the STORED entries, each pivot visiting only its finite in- and
    // out-neighbours (the matrix is sparse now that the strengthening is not stored).
    int *in = act + d, *out = in + d;
    for (int ka=0;ka<na;ka++) { int k=act[ka];
        int ni = 0, no = 0;
        for (int xa=0;xa<na;xa++) { int x=act[xa];
            if (oct_raw(o,x,k) < OCT_INF) in[ni++] = x;
            if (oct_raw(o,k,x) < OCT_INF) out[no++] = x;
        }
        for (int ia=0;ia<ni;ia++) { int i=in[ia];
            int64_t ik = oct_raw(o,i,k);
            if (ik >= OCT_INF) continue;
            for (int ja=0;ja<no;ja++) { int j=out[ja];
                int64_t kj = oct_raw(o,k,j);
                if (kj >= OCT_INF) continue;
                // Entries are capped above by OCT_INF, not below, and two large negative ones
                // overflowed here (undefined in C; I.170, found by a UBSan-built compiler). The true
                // sum is below INT64_MIN: any value above it is a weaker, still sound, bound.
                int64_t s;
                if (__builtin_add_overflow(ik, kj, &s)) s = INT64_MIN / 2;
                int64_t *ij = oct_at(o,i,j);
                if (s < *ij) *ij = s;
            }
        }
    }
    oct_tighten_unary(o);
    oct_set_clean(o, true);
}

// ⊥ test — a variable's own dimension shows a negative self-distance.
static bool oct_is_bottom(const Octagon *o) {
    for (int i=0;i<o->dim;i++) if (oct_raw(o,i,i) < 0) return true;
    return false;
}

// Store `v`, the value a lattice operation computed for e(j) − e(i), only where it says more than
// the two unary bounds already stored in `dst` imply: elsewhere oct_get recomputes it, at least as
// tight. The unary entries must be written first.
static inline void oct_store_pair(Octagon *dst, int i, int j, int64_t v) {
    int64_t s = oct_halves(oct_raw(dst, i, oct_bar(i)), oct_raw(dst, oct_bar(j), j));
    *oct_at(dst, i, j) = v < s ? v : OCT_INF;
}

// ── lattice operations (operands assumed closed) ─────────────────────────────
// Join ⊔ — entrywise max: the tightest octagon containing both (the φ merge).
// On the values the operands IMPLY: the unary entries first, then every other pair where its
// max is tighter than the joined unary bounds imply (the "crossing" pairs, whose bounds moved
// in opposite directions between the operands). Its oct_get is the strong join's entry.
// The halves of an octagon's unary entries, once per operation: h[i] = ⌊m[i][bar i]/2⌋ (the row
// side of a pair's strengthening) and h[d + j] = ⌊m[bar j][j]/2⌋ (the column side), +∞ where the
// entry is. A pair's implied value is then min(m[i][j], h[i] + h[d + j]) from two array reads: the
// join and the order read it for every pair, and recomputing the halves there was most of their cost.
static int64_t *oct_half_buf = NULL; static int oct_half_cap = 0;
static int64_t *oct_halves_of(const Octagon *o, int slot) {
    int d = o->dim;
    if (oct_half_cap < 6*d) {
        oct_half_buf = (int64_t*)realloc(oct_half_buf, (size_t)6*d*sizeof(int64_t));
        oct_half_cap = 6*d;
    }
    int64_t *h = oct_half_buf + (size_t)slot*2*d;
    for (int i=0;i<d;i++) {
        int64_t u = o->m[(size_t)i*d + oct_bar(i)], w = o->m[(size_t)oct_bar(i)*d + i];
        h[i]     = u < OCT_INF ? oct_fdiv2(u) : OCT_INF;
        h[d + i] = w < OCT_INF ? oct_fdiv2(w) : OCT_INF;
    }
    return h;
}
static inline int64_t oct_hsum(int64_t x, int64_t y) {
    if (x >= OCT_INF || y >= OCT_INF) return OCT_INF;
    int64_t s;
    if (__builtin_add_overflow(x, y, &s)) s = INT64_MIN / 2;
    return s;
}
static void oct_join(Octagon *dst, const Octagon *a, const Octagon *b) {
    dst->nvar=a->nvar; dst->dim=a->dim; oct_set_clean(dst, false);
    int d = a->dim;
    for (int i=0;i<d;i++) {
        *oct_at(dst,i,i) = oct_max64(oct_raw(a,i,i), oct_raw(b,i,i));
        *oct_at(dst,i,oct_bar(i)) = oct_max64(oct_get(a,i,oct_bar(i)), oct_get(b,i,oct_bar(i)));
    }
    const int64_t *ha = oct_halves_of(a, 0), *hb = oct_halves_of(b, 1), *hd = oct_halves_of(dst, 2);
    for (int i=0;i<d;i++) {
        const int64_t *ra = a->m + (size_t)i*d, *rb = b->m + (size_t)i*d;
        int64_t *rd = dst->m + (size_t)i*d;
        int bi = oct_bar(i);
        for (int j=0;j<d;j++) {
            if (j == i || j == bi) continue;
            int64_t sa = oct_min64(ra[j], oct_hsum(ha[i], ha[d + j]));
            int64_t sb = oct_min64(rb[j], oct_hsum(hb[i], hb[d + j]));
            int64_t v = oct_max64(sa, sb), imp = oct_hsum(hd[i], hd[d + j]);
            rd[j] = v < imp ? v : OCT_INF;
        }
    }
}
// Meet ⊓ — entrywise min (adds both constraint sets); caller re-closes.
static maybe_unused void oct_meet(Octagon *dst, const Octagon *a, const Octagon *b) {
    dst->nvar=a->nvar; dst->dim=a->dim; oct_set_clean(dst, false);
    for (int i=0;i<a->dim;i++)
        for (int j=0;j<a->dim;j++)
            *oct_at(dst,i,j) = oct_min64(oct_raw(a,i,j), oct_raw(b,i,j));
}
// Widening ∇ — keep a's entry where b does not exceed it, else drop to +∞.
// Guarantees termination of the ascending chain (no re-closing of the result).
static void oct_widen_thr(Octagon *dst, const Octagon *a, const Octagon *b, const char *mod,
                          const int64_t *thr, int nthr);   // fwd
static maybe_unused void oct_widen(Octagon *dst, const Octagon *a, const Octagon *b) {
    oct_widen_thr(dst, a, b, NULL, NULL, 0);
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
// On the values the operands IMPLY, like the join, so a pair that one operand knew only through
// its bounds and the other states keeps its value: `i + j <= 9` known from the entry's constants
// and stated by the loop body is a two-pointer loop's invariant.
static int64_t oct_widen_entry(const Octagon *a, const Octagon *b, int i, int j, const char *mod,
                               const int64_t *thr, int nthr) {
    int64_t av=oct_get(a,i,j), bv=oct_get(b,i,j);
    bool widen = !mod || mod[i/2] || mod[j/2];   // DBM index i ↔ var i/2
    if (!widen)   return bv;
    if (bv <= av) return av;                                     // stable: keep it
    for (int t=0; t<nthr; t++) if (thr[t] >= bv) return thr[t];   // else climb the ladder
    return OCT_INF;
}
static void oct_widen_thr(Octagon *dst, const Octagon *a, const Octagon *b, const char *mod,
                          const int64_t *thr, int nthr) {
    dst->nvar=a->nvar; dst->dim=a->dim; oct_set_clean(dst, false);
    int d = a->dim;
    for (int i=0;i<d;i++) {
        *oct_at(dst,i,i) = oct_widen_entry(a, b, i, i, mod, thr, nthr);
        *oct_at(dst,i,oct_bar(i)) = oct_widen_entry(a, b, i, oct_bar(i), mod, thr, nthr);
    }
    for (int i=0;i<d;i++)
        for (int j=0;j<d;j++)
            if (j != i && j != oct_bar(i)) oct_store_pair(dst, i, j, oct_widen_entry(a, b, i, j, mod, thr, nthr));
}
static bool oct_leq(const Octagon *a, const Octagon *b) {
    int d = a->dim;
    const int64_t *ha = oct_halves_of(a, 0), *hb = oct_halves_of(b, 1);
    for (int i=0;i<d;i++) {
        const int64_t *ra = a->m + (size_t)i*d, *rb = b->m + (size_t)i*d;
        for (int j=0;j<d;j++) {
            int64_t sa = ra[j], sb = rb[j];
            if (i != j) {
                sa = oct_min64(sa, oct_hsum(ha[i], ha[d + j]));
                sb = oct_min64(sb, oct_hsum(hb[i], hb[d + j]));
            }
            if (sa > sb) return false;
        }
    }
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
