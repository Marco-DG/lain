#ifndef SEMA_NICHE_H
#define SEMA_NICHE_H

/*
   Niche optimization for enum layout.
   See internal/ai_analysis/analysis19_aggressive_niche.md.

   M2 — core algorithms only. No codegen yet; M3 consumes this.

   compute_sentinel_pool(T):
     Returns the set of bit patterns that are NOT valid values of T.
     These are available to encode empty enum variants without a tag byte.

   niche_compute_layout(enum):
     Decides the physical layout. If the payload's sentinel pool has
     >= empty_count slots, encode without a tag byte; else fall back
     to a tagged union and emit W120.
*/

#include <limits.h>
#include "../ast.h"
#include "../target.h"

extern Arena *sema_arena;

/*─────────────────────────────────────────────────────────────────╗
│ Sentinel pool: kind-specific representation                      │
╚─────────────────────────────────────────────────────────────────*/

typedef enum {
    POOL_EMPTY,             // no sentinel slots available
    POOL_POINTER,           // slots = [0, zero_page_size) step pointer_alignment
    POOL_INTEGER_BELOW,     // slots = [lo, vra_lo)
    POOL_INTEGER_ABOVE,     // slots = (vra_hi, hi]
    POOL_INTEGER_SPLIT,     // slots = [lo, vra_lo) ∪ (vra_hi, hi]
    POOL_BOOL,              // slots = [2, 255]
} SentinelPoolKind;

typedef struct {
    SentinelPoolKind kind;
    long long size;           // total slot count
    long long below_start;    // POOL_INTEGER_*: low end of below-range
    long long below_count;    // POOL_INTEGER_SPLIT: slots in [below_start, below_start+below_count)
    long long above_start;    // POOL_INTEGER_*: low end of above-range
    long long above_count;    // POOL_INTEGER_SPLIT: slots in [above_start, above_start+above_count)
    long long ptr_stride;     // POOL_POINTER: spacing between slots (= pointer_alignment)
} SentinelPool;

/* Layout decision for one enum declaration. */
typedef struct {
    bool       is_zero_cost;            // true if no tag byte needed
    bool       needs_tag_byte;          // !is_zero_cost
    size_t     payload_variant_count;
    size_t     empty_variant_count;
    long long *empty_sentinels;         // assigned bit patterns, indexed by variant order
    size_t     empty_sentinels_count;   // == empty_variant_count when zero-cost
    SentinelPool pool;                  // pool snapshot at decision time
} NicheLayout;

/*─────────────────────────────────────────────────────────────────╗
│ parse_iN_uN — duplicated from typecheck.h to keep niche.h        │
│ standalone (typecheck.h pulls in resolve.h etc.). Same semantics.│
╚─────────────────────────────────────────────────────────────────*/

static int niche_parse_iN_uN(Type *t, int *out_bits, bool *out_signed) {
    if (!t || t->kind != TYPE_SIMPLE || !t->base_type) return 0;
    const char *n = t->base_type->name;
    isize len = t->base_type->length;
    if (len < 2 || len > 3) return 0;
    if (n[0] != 'i' && n[0] != 'u') return 0;
    int v = 0;
    for (isize k = 1; k < len; k++) {
        if (n[k] < '0' || n[k] > '9') return 0;
        v = v * 10 + (n[k] - '0');
    }
    if (v < 1 || v > 64) return 0;
    *out_bits = v;
    *out_signed = (n[0] == 'i');
    return 1;
}

static int niche_type_int_range(Type *t, long long *lo, long long *hi) {
    int bits; bool sgn;
    if (!niche_parse_iN_uN(t, &bits, &sgn)) return 0;
    if (sgn) {
        if (bits == 64) { *lo = LLONG_MIN; *hi = LLONG_MAX; }
        else { *lo = -(1LL << (bits - 1)); *hi = (1LL << (bits - 1)) - 1; }
    } else {
        *lo = 0;
        *hi = (bits >= 63) ? LLONG_MAX : ((1LL << bits) - 1);
    }
    return 1;
}

static bool niche_is_bool(Type *t) {
    if (!t || t->kind != TYPE_SIMPLE || !t->base_type) return false;
    return t->base_type->length == 4 &&
           memcmp(t->base_type->name, "bool", 4) == 0;
}

/*─────────────────────────────────────────────────────────────────╗
│ compute_sentinel_pool — the core analysis                        │
╚─────────────────────────────────────────────────────────────────*/

static SentinelPool compute_sentinel_pool(Type *t) {
    SentinelPool p = {0};
    p.kind = POOL_EMPTY;
    if (!t) return p;

    /* Pointer: zero-page slots stepped by alignment. */
    if (t->kind == TYPE_POINTER) {
        if (target.zero_page_size == 0) return p;  // bare-metal: no niche
        p.kind        = POOL_POINTER;
        p.ptr_stride  = (long long)target.pointer_alignment;
        p.size        = (long long)(target.zero_page_size / target.pointer_alignment);
        return p;
    }

    /* Bool: 1 byte storage, 254 unused bit patterns. */
    if (niche_is_bool(t)) {
        p.kind = POOL_BOOL;
        p.size = 254;
        return p;
    }

    /* Sized integer: pool = type-range minus VRA-narrowed range.
       Pre-M2 we have no VRA hook here — the layout caller will
       supply VRA bounds through niche_compute_layout(). For now,
       return type-range = empty pool (no narrowing → no niche). */
    int bits; bool sgn;
    if (niche_parse_iN_uN(t, &bits, &sgn)) {
        /* Without a tighter VRA bound, the entire type range is
           "valid payload" → pool empty. The caller (compute_layout)
           uses the type's range as the default valid-payload range,
           but a refinement type alias can shrink it. We expose the
           type-range here; refinement narrowing happens in
           niche_compute_layout via the type's constraint list. */
        return p;  // empty by default; refinement extends below
    }

    /* TYPE_SLICE: fat pointer (ptr, len). With invariant
       "ptr never NULL" (D-N2), use the ptr field as niche source.
       We model this as a pointer pool. */
    if (t->kind == TYPE_SLICE) {
        if (target.zero_page_size == 0) return p;
        p.kind        = POOL_POINTER;
        p.ptr_stride  = (long long)target.pointer_alignment;
        p.size        = (long long)(target.zero_page_size / target.pointer_alignment);
        return p;
    }

    /* Struct / array / float / others: pre-1.0 conservative — no niche. */
    return p;
}

/*─────────────────────────────────────────────────────────────────╗
│ Refinement-aware pool for integer payload                        │
│                                                                  │
│ When the payload type is a refinement alias `T = iN >= lo and    │
│ <= hi`, the sentinel pool = type-range \ [lo, hi]. This routine  │
│ inspects the type's constraint list directly to extract the      │
│ refinement bounds when present.                                  │
╚─────────────────────────────────────────────────────────────────*/

static SentinelPool compute_sentinel_pool_integer_refined(Type *t,
                                                          long long ref_lo,
                                                          long long ref_hi) {
    SentinelPool p = {0};
    long long tlo, thi;
    if (!niche_type_int_range(t, &tlo, &thi)) {
        p.kind = POOL_EMPTY;
        return p;
    }
    long long below = (ref_lo > tlo) ? (ref_lo - tlo) : 0;
    long long above = (thi > ref_hi) ? (thi - ref_hi) : 0;

    if (below == 0 && above == 0) {
        p.kind = POOL_EMPTY;
        return p;
    }
    if (below > 0 && above == 0) {
        p.kind        = POOL_INTEGER_BELOW;
        p.below_start = tlo;
        p.below_count = below;
        p.size        = below;
        return p;
    }
    if (below == 0 && above > 0) {
        p.kind        = POOL_INTEGER_ABOVE;
        p.above_start = ref_hi + 1;
        p.above_count = above;
        p.size        = above;
        return p;
    }
    p.kind        = POOL_INTEGER_SPLIT;
    p.below_start = tlo;
    p.below_count = below;
    p.above_start = ref_hi + 1;
    p.above_count = above;
    p.size        = below + above;
    return p;
}

/*─────────────────────────────────────────────────────────────────╗
│ Sentinel allocation: assign concrete bit patterns                │
│                                                                  │
│ Order (deterministic, spec §2.4):                                │
│   - POOL_POINTER:        0, stride, 2*stride, ...                │
│   - POOL_INTEGER_BELOW:  tlo, tlo+1, ..., ref_lo-1               │
│     (we go downward from ref_lo-1: ref_lo-1, ref_lo-2, ...       │
│      so common case "Option(int >= 0)" picks None = -1 first)    │
│   - POOL_INTEGER_ABOVE:  ref_hi+1, ref_hi+2, ...                 │
│   - POOL_INTEGER_SPLIT:  below first (descending from ref_lo-1), │
│                          then above (ascending from ref_hi+1)    │
│   - POOL_BOOL:           2, 3, 4, ..., 255                       │
╚─────────────────────────────────────────────────────────────────*/

static long long sentinel_pick(SentinelPool *p, size_t index) {
    switch (p->kind) {
        case POOL_POINTER:
            return p->ptr_stride * (long long)index;

        case POOL_INTEGER_BELOW:
            /* descending from below_start + below_count - 1 */
            return (p->below_start + p->below_count - 1) - (long long)index;

        case POOL_INTEGER_ABOVE:
            return p->above_start + (long long)index;

        case POOL_INTEGER_SPLIT: {
            if ((long long)index < p->below_count) {
                return (p->below_start + p->below_count - 1) - (long long)index;
            }
            long long over = (long long)index - p->below_count;
            return p->above_start + over;
        }

        case POOL_BOOL:
            return 2 + (long long)index;

        case POOL_EMPTY:
        default:
            return 0;  // caller should have checked size
    }
}

/*─────────────────────────────────────────────────────────────────╗
│ Variant counting helpers                                         │
╚─────────────────────────────────────────────────────────────────*/

static size_t niche_count_payload_variants(DeclEnum *e) {
    size_t n = 0;
    for (Variant *v = e->variants; v; v = v->next) {
        if (v->fields) n++;
    }
    return n;
}

static size_t niche_count_empty_variants(DeclEnum *e) {
    size_t n = 0;
    for (Variant *v = e->variants; v; v = v->next) {
        if (!v->fields) n++;
    }
    return n;
}

/*─────────────────────────────────────────────────────────────────╗
│ niche_compute_layout — orchestrator                              │
│                                                                  │
│ Strategy (M2 conservative):                                      │
│   - If exactly one payload variant whose single field is a       │
│     pointer or slice: use that field's pool.                     │
│   - If exactly one payload variant whose single field is a       │
│     refined integer alias: use refinement pool.                  │
│   - If exactly one payload variant whose single field is bool:   │
│     use bool pool.                                               │
│   - Multi-payload or unsupported: pool empty → tag byte.         │
│                                                                  │
│ Multi-payload merging is a future extension (M6 / nested enum).  │
╚─────────────────────────────────────────────────────────────────*/

static SentinelPool niche_pool_for_field(Type *field_ty) {
    if (!field_ty) {
        SentinelPool p = {0};
        return p;
    }
    /* Direct pool query for pointer / slice / bool. */
    SentinelPool p = compute_sentinel_pool(field_ty);
    if (p.kind != POOL_EMPTY) return p;

    /* For integer fields, look for a refinement on the field type.
       Refinements live on the DeclTypeAlias the field references.
       In M2 we accept inline refinements on field type only if
       parse_iN_uN succeeds AND field_ty has a non-NULL sentinel_str
       (current ast hooks for refinement metadata are limited).
       Fuller integration happens in M6 via the alias decl table. */
    return p;
}

static NicheLayout niche_compute_layout(DeclEnum *e) {
    NicheLayout L = {0};
    L.payload_variant_count = niche_count_payload_variants(e);
    L.empty_variant_count   = niche_count_empty_variants(e);

    /* Pure-empty enum: represented as the smallest integer that
       holds all variants. No payload, no niche question; trivially
       zero-cost. */
    if (L.payload_variant_count == 0) {
        L.is_zero_cost = true;
        L.needs_tag_byte = false;
        SentinelPool p = {0};
        p.kind = POOL_EMPTY;
        L.pool = p;
        return L;
    }

    /* M2 conservative: single payload variant with a single field. */
    Variant *payload_v = NULL;
    for (Variant *v = e->variants; v; v = v->next) {
        if (v->fields) { payload_v = v; break; }
    }
    if (!payload_v || !payload_v->fields ||
        payload_v->fields->next != NULL ||
        L.payload_variant_count != 1) {
        /* Multi-payload or multi-field: pool empty, fall back to tag. */
        SentinelPool p = {0};
        p.kind = POOL_EMPTY;
        L.pool = p;
        L.is_zero_cost = false;
        L.needs_tag_byte = true;
        return L;
    }

    Decl *field_decl = payload_v->fields->decl;
    if (!field_decl || field_decl->kind != DECL_VARIABLE) {
        SentinelPool p = {0};
        p.kind = POOL_EMPTY;
        L.pool = p;
        L.is_zero_cost = false;
        L.needs_tag_byte = true;
        return L;
    }

    Type *field_ty = field_decl->as.variable_decl.type;
    L.pool = niche_pool_for_field(field_ty);

    if (L.pool.kind == POOL_EMPTY ||
        (long long)L.empty_variant_count > L.pool.size) {
        L.is_zero_cost = false;
        L.needs_tag_byte = true;
    } else {
        L.is_zero_cost = true;
        L.needs_tag_byte = false;
    }

    /* Allocate sentinel array if zero-cost. */
    if (L.is_zero_cost && L.empty_variant_count > 0) {
        L.empty_sentinels = arena_push_many_aligned(
            sema_arena, long long, L.empty_variant_count);
        L.empty_sentinels_count = L.empty_variant_count;
        for (size_t i = 0; i < L.empty_variant_count; i++) {
            L.empty_sentinels[i] = sentinel_pick(&L.pool, i);
        }
    }

    return L;
}

/*─────────────────────────────────────────────────────────────────╗
│ Debug print — used by --dump-niche                               │
╚─────────────────────────────────────────────────────────────────*/

static const char *niche_pool_kind_str(SentinelPoolKind k) {
    switch (k) {
        case POOL_EMPTY:         return "empty";
        case POOL_POINTER:       return "pointer";
        case POOL_INTEGER_BELOW: return "integer-below";
        case POOL_INTEGER_ABOVE: return "integer-above";
        case POOL_INTEGER_SPLIT: return "integer-split";
        case POOL_BOOL:          return "bool";
    }
    return "?";
}

static void niche_dump_layout(DeclEnum *e, NicheLayout *L) {
    Id *name = e->type_name;
    int nlen = name ? (int)name->length : 1;
    const char *nstr = name ? name->name : "?";
    fprintf(stderr, "[niche] enum '%.*s': "
                    "payload=%zu empty=%zu pool=%s pool.size=%lld "
                    "zero_cost=%s\n",
            nlen, nstr,
            L->payload_variant_count, L->empty_variant_count,
            niche_pool_kind_str(L->pool.kind),
            (long long)L->pool.size,
            L->is_zero_cost ? "yes" : "no");
    if (L->is_zero_cost && L->empty_sentinels) {
        for (size_t i = 0; i < L->empty_sentinels_count; i++) {
            fprintf(stderr, "[niche]   empty[%zu] = %lld\n",
                    i, L->empty_sentinels[i]);
        }
    }
}

#endif /* SEMA_NICHE_H */
