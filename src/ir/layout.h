#ifndef LAIN_IR_LAYOUT_H
#define LAIN_IR_LAYOUT_H
// layout.h — how a sum is REPRESENTED, decided once, from the IR.
//
// ★ WHY THIS FILE EXISTS, AND WHAT IT COST TO LEARN. Layout was decided in the FRONT END
// (src/sema/niche.h, over the AST) and re-derived in the backend. That was survivable while
// there was one backend. The day a second one shipped, the two chose differently: the old
// emitter packed `*u8 | none` into a single pointer, the new one emitted
// `struct { int32_t tag; union {...} data; }`, and **29 sums in the corpus silently lost the
// niche optimization while all ten gates stayed green** — because both representations are
// correct and the programs print the same thing (D-62, scripts/gates/layout_gate.sh).
//
// The plan had predicted precisely that, as an argument for this file: "if two backends chose
// differently, the same program would have different runtime semantics under each, and nothing
// would catch it — there is no cross-backend layout test because there has never been a second
// backend." So: ONE decision, below the IR, that every backend reads.
//
// ★ AND WHY THE ANSWER IS NOT A FIELD ON IrType. ir.h argues, correctly, that recording the
// niche on the type "would make a sum indistinguishable from a pointer with an odd range and
// destroy the discrimination every analysis depends on". Both things are true at once: the
// TYPE stays semantic (a sum is a sum), and the REPRESENTATION is a separate answer computed
// from that type on demand. This file is that answer; it never mutates a type.
//
// The algorithm is src/sema/niche.h's, ported to IrType. It is deliberately the same
// algorithm and not a better one: the immediate job is for the two backends to AGREE, and a
// port that improves on the original silently reintroduces the divergence it exists to close.
#include "ir.h"
#include "../target.h"
#include "../layout_core.h"   // the sentinel algebra, shared with src/sema/niche.h

/* The pool algebra is src/layout_core.h's — ONE definition for both backends. Keeping a
   second copy here is exactly how D-62 and D-63 happened: each side was individually
   defensible and they answered the same question differently. */

#define IR_LAYOUT_MAX_VARIANTS 64

typedef struct {
    bool      packed;         // representable with NO tag: the payload IS the value
    bool      all_empty;      // every variant is payload-less: a plain small integer
    int       payload_count, empty_count;
    int       primary;        // the variant whose payload type is the backing (-1 if none)
    IrType   *backing;        // that payload's single field type (NULL when all_empty)
    long long sentinel[IR_LAYOUT_MAX_VARIANTS];   // per empty variant, by variant index
    bool      has_sentinel[IR_LAYOUT_MAX_VARIANTS];
    SentinelPool pool;
} IrLayout;

static IrLayout ir_layout_of(IrType *sum);   // forward: the cascade recurses

// The single field of a payload-carrying variant, or NULL when the variant is payload-less
// or carries more than one field (which no niche in this scheme can encode).
static IrType *ir_layout_single_field(IrType *sum, int k) {
    if (!sum || k < 0 || k >= sum->n_fields) return NULL;
    IrType *pl = sum->fields[k];
    if (!pl || pl->n_fields != 1) return NULL;
    return pl->fields[0];
}

static bool ir_layout_int_range(const IrType *t, long long *lo, long long *hi) {
    if (!t || t->kind != IRT_INT) return false;
    return sentinel_int_range(t->bits, t->is_signed, lo, hi);   /* shared: layout_core.h */
}

static SentinelPool ir_pool_int_refined(const IrType *t, long long rlo, long long rhi) {
    SentinelPool p = {0}; p.kind = POOL_EMPTY;
    long long tlo, thi;
    if (!ir_layout_int_range(t, &tlo, &thi)) return p;
    return sentinel_pool_int_refined(tlo, thi, rlo, rhi);       /* shared: layout_core.h */
}

// The spare bit patterns of a payload type — the ones that cannot be a legitimate value, so
// they are free to mean "this is one of the payload-less variants".
static SentinelPool ir_pool_for(IrType *t) {
    SentinelPool p = {0}; p.kind = POOL_EMPTY;
    if (!t) return p;

    // A pointer: the zero page, which no user mapping can occupy.
    //
    // ⚠ A SLICE IS NOT INCLUDED, though src/sema/niche.h includes it. The reasoning there is
    // sound — a slice's data pointer is never null (D-N2), so the zero page is spare — but the
    // REPRESENTATION does not follow: a slice is a two-word struct, and a sentinel is an
    // integer, so `(Slice_u8)(uintptr_t)0` is not a conversion C will do. Packing one produces
    // code that does not compile; the old backend does it and emits `typedef Slice_u8_0 ...`
    // naming a type it never defines. Both are broken, neither is in the corpus, and this side
    // fails CLOSED: a tagged struct is correct, merely not free. Packing a slice needs the
    // sentinel to live in the data POINTER FIELD, which is a different emit, not a wider cast.
    if (t->kind == IRT_PTR)  return sentinel_pool_pointer();   /* shared */
    if (t->kind == IRT_BOOL) return sentinel_pool_bool();      /* shared */

    // An integer is only a niche source when something has NARROWED it: a plain i32 uses
    // every bit pattern it has. `IrType.has_refine` is that narrowing, carried on the type
    // (B4) — which is exactly why this pass can live below the IR at all.
    if (t->kind == IRT_INT && t->has_refine)
        return ir_pool_int_refined(t, t->refine_lo, t->refine_hi);

    // The cascade: the payload is itself a packed sum, so its own empties already occupy the
    // first N slots of the underlying pool. Take what is left.
    if (t->kind == IRT_SUM) {
        IrLayout inner = ir_layout_of(t);
        if (inner.packed && inner.backing) {
            SentinelPool base = ir_pool_for(inner.backing);
            if (base.kind != POOL_EMPTY) {
                long long used = inner.empty_count;
                base.index_offset = used;
                base.size = base.size > used ? base.size - used : 0;
                if (base.size == 0) { SentinelPool e = {0}; return e; }
                return base;
            }
        }
    }
    return p;
}

// Deterministic assignment, same order as src/sema/niche.h — the two backends must pick the
// SAME bit pattern for the same variant, not merely both pick some pattern.
/* ir_sentinel_pick: gone. The ASSIGNMENT is sentinel_pick() in layout_core.h — two backends
   that both pack but choose different patterns disagree about what `none` IS. */

// THE decision. Cheap and pure, so callers may ask per use rather than caching.
static IrLayout ir_layout_of(IrType *sum) {
    IrLayout L = {0};
    L.primary = -1;
    if (!sum || sum->kind != IRT_SUM || sum->n_fields <= 0) return L;
    if (sum->n_fields > IR_LAYOUT_MAX_VARIANTS) return L;

    for (int k = 0; k < sum->n_fields; k++) {
        if (sum->fields[k]) { L.payload_count++; if (L.primary < 0) L.primary = k; }
        else                  L.empty_count++;
    }

    // Every variant payload-less: a plain integer, sentinels 0,1,2,... in declaration order.
    if (L.payload_count == 0) {
        L.packed = true; L.all_empty = true;
        for (int k = 0, n = 0; k < sum->n_fields; k++) {
            L.sentinel[k] = n++; L.has_sentinel[k] = true;
        }
        return L;
    }

    // More than one payload, or a payload with more than one field: nothing to pack into.
    if (L.payload_count != 1) { L.primary = -1; return L; }
    IrType *back = ir_layout_single_field(sum, L.primary);
    if (!back) { L.primary = -1; return L; }

    L.pool = ir_pool_for(back);
    if (L.pool.kind == POOL_EMPTY || (long long)L.empty_count > L.pool.size) {
        L.primary = -1;          // the pool is too small: a tag byte it is
        return L;
    }

    L.packed  = true;
    L.backing = back;
    for (int k = 0, n = 0; k < sum->n_fields; k++) {
        if (sum->fields[k]) continue;                 // the payload variant carries no sentinel
        L.sentinel[k] = sentinel_pick(&L.pool, n++);
        L.has_sentinel[k] = true;
    }
    return L;
}

// ── A `[packed]` STRUCT: bit-exact fields in one integer (spec 07, DECIDE-N) ──────────────────
// Fields in declaration order from bit 0, each exactly its declared width, in the smallest of
// uint8/16/32/64_t that holds the total. The emitter reads a field by shift and mask (sign-
// extending an iN), writes one by read-modify-write, and builds one by OR-ing shifted fields.
// It was the old AST emitter's behaviour, lost when src/emit/ was deleted (cf702c5) — the IR
// backend emitted an ordinary struct and the three [packed] tests, asserting nothing about
// layout, stayed green.
#define IR_PACKED_MAX_FIELDS 64
typedef struct {
    bool packed;              // a valid [packed] layout: every field an integer, total <= 64 bits
    int  container_bits;      // 8, 16, 32 or 64
    int  n;
    int  off[IR_PACKED_MAX_FIELDS], width[IR_PACKED_MAX_FIELDS];
    bool sgn[IR_PACKED_MAX_FIELDS];
} IrStructLayout;
static IrStructLayout ir_struct_layout(const IrType *st) {
    IrStructLayout L; memset(&L, 0, sizeof L);
    if (!st || st->kind != IRT_STRUCT || !st->packed_decl || st->n_fields <= 0 ||
        st->n_fields > IR_PACKED_MAX_FIELDS) return L;
    int bit = 0;
    for (int k = 0; k < st->n_fields; k++) {
        IrType *ft = st->fields[k];
        if (!ft || ft->kind != IRT_INT || ft->bits < 1 || ft->bits > 64) return L;   // sema: E121
        L.off[k] = bit; L.width[k] = ft->bits; L.sgn[k] = ft->is_signed;
        bit += ft->bits;
    }
    if (bit > 64) return L;                                                          // sema: E121
    L.n = st->n_fields;
    L.container_bits = bit <= 8 ? 8 : bit <= 16 ? 16 : bit <= 32 ? 32 : 64;
    L.packed = true;
    return L;
}

// ── THE SIZES C FIXES, and the sizes Lain chooses ─────────────────────────────────────────────
// Every decision below is printed by the emitter AND read by the analysis, so it is made once.

// The C integer that stores an iN/uN: `u3` is a `uint8_t`, `i40` an `int64_t`.
static int ir_int_storage_bits(int bits) { return bits<=8?8 : bits<=16?16 : bits<=32?32 : 64; }

// A plain enumeration is the smallest unsigned integer that holds its tags 0..n-1 (a405172).
static int ir_plain_enum_bits(int n_variants) {
    return n_variants <= 256 ? 8 : n_variants <= 65536 ? 16 : 32;
}

// A vector lane's bytes. ★ A float lane's width is in `float_bits`, not `bits`: the vector
// typedef read `bits`, found 0 and fell back to 4 bytes, so `Vec(4, f64)` was emitted as
// `vector_size(16)` — TWO doubles — and its lanes 2 and 3 read past the vector, silently.
static int ir_lane_bytes(const IrType *e) {
    if (!e) return 0;
    if (e->kind == IRT_FLOAT) return e->float_bits == 32 ? 4 : 8;
    if (e->kind == IRT_INT)   return ir_int_storage_bits(e->bits) / 8;
    return 0;
}

// The size in bytes of a type whose C spelling has an EXACT size: an exact-width integer (C
// gives intN_t no padding bits), a type Lain lays out itself as one (a plain enumeration, a
// [packed] struct's container, a niche-packed sum over a fixed-size backing), a vector (its
// `vector_size` is the number), or an array of any of these (no padding between elements).
// 0 when the C compiler's ABI decides — a struct, a pointer, a slice, a `_Bool`, a float — and
// then nothing about the size is claimed but that it is at least 1.
static int64_t ir_fixed_size(IrType *t) {
    if (!t) return 0;
    switch (t->kind) {
        case IRT_INT:    return ir_int_storage_bits(t->bits) / 8;
        case IRT_VECTOR: { int lb = ir_lane_bytes(t->elem);
                           return (lb > 0 && t->array_len > 0) ? (int64_t)t->array_len * lb : 0; }
        case IRT_ARRAY:  { int64_t e = ir_fixed_size(t->elem);
                           return (e > 0 && t->array_len > 0) ? t->array_len * e : 0; }
        case IRT_STRUCT: { IrStructLayout P = ir_struct_layout(t);
                           return P.packed ? P.container_bits / 8 : 0; }
        case IRT_SUM: {
            IrLayout L = ir_layout_of(t);
            if (!L.packed) return 0;
            if (L.all_empty) return ir_plain_enum_bits(t->n_fields) / 8;
            if (L.backing && L.backing->kind == IRT_BOOL) return 1;     // stored as a uint8_t
            return ir_fixed_size(L.backing);
        }
        default: return 0;
    }
}

#endif // LAIN_IR_LAYOUT_H
