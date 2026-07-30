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
    long long size;           // total slot count (already adjusted for index_offset)
    long long below_start;    // POOL_INTEGER_*: low end of below-range
    long long below_count;    // POOL_INTEGER_SPLIT: slots in [below_start, below_start+below_count)
    long long above_start;    // POOL_INTEGER_*: low end of above-range
    long long above_count;    // POOL_INTEGER_SPLIT: slots in [above_start, above_start+above_count)
    long long ptr_stride;     // POOL_POINTER: spacing between slots (= pointer_alignment)
    long long index_offset;   // M6 cascade: skip the first N slots (already used by inner enum)
} SentinelPool;

/* Layout decision for one enum declaration. */
typedef struct {
    bool       is_zero_cost;            // true if no tag byte needed
    bool       needs_tag_byte;          // !is_zero_cost
    size_t     payload_variant_count;
    size_t     empty_variant_count;
    long long *empty_sentinels;         // assigned bit patterns, indexed by variant order
    size_t     empty_sentinels_count;   // == empty_variant_count when zero-cost
    bool       pool_was_short;          // set when payload exists but pool < empties

    /* Sprint D follow-up: multi-payload niche.
       When two payload variants exist and one of them has a single
       field of type "enum-of-pure-empties", that variant can be
       encoded by mapping its inner enum value into the primary's
       sentinel pool. */
    Variant   *primary_variant;         // the variant whose payload type becomes the backing
    Variant   *secondary_variant;       // the variant that gets sentinel-encoded
    Decl      *secondary_subenum_decl;  // the inner enum decl (all empties)
    long long *secondary_sentinels;     // sentinel for each sub-variant (declaration order)
    size_t     secondary_sentinels_count;

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
    long long i = (long long)index + p->index_offset;
    switch (p->kind) {
        case POOL_POINTER:
            return p->ptr_stride * i;

        case POOL_INTEGER_BELOW:
            /* descending from below_start + below_count - 1 */
            return (p->below_start + p->below_count - 1) - i;

        case POOL_INTEGER_ABOVE:
            return p->above_start + i;

        case POOL_INTEGER_SPLIT: {
            if (i < p->below_count) {
                return (p->below_start + p->below_count - 1) - i;
            }
            long long over = i - p->below_count;
            return p->above_start + over;
        }

        case POOL_BOOL:
            return 2 + i;

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

/* Forward declarations needed by niche_pool_for_field cascade. */
static NicheLayout niche_compute_layout(DeclEnum *e);
static Variant *enum_payload_variant(DeclEnum *e);

/* Look up an enum declaration by the field type's base name.
   M6 cascade uses this to detect nested niche-optimized enums. */
static Decl *niche_find_enum_by_name(Type *t) {
    extern DeclList *sema_decls;
    if (!t || t->kind != TYPE_SIMPLE || !t->base_type) return NULL;
    const char *n = t->base_type->name;
    isize L = t->base_type->length;
    for (DeclList *dl = sema_decls; dl; dl = dl->next) {
        if (!dl->decl || dl->decl->kind != DECL_ENUM) continue;
        Id *en = dl->decl->as.enum_decl.type_name;
        if (!en) continue;
        if (en->length == L && memcmp(en->name, n, L) == 0) {
            return dl->decl;
        }
    }
    return NULL;
}

/* Look up a type alias declaration by name. Sprint D follow-up
   uses this to extract refinement bounds for integer payload niche. */
static Decl *niche_find_alias_by_name(Type *t) {
    extern DeclList *sema_decls;
    if (!t || t->kind != TYPE_SIMPLE || !t->base_type) return NULL;
    const char *n = t->base_type->name;
    isize L = t->base_type->length;
    for (DeclList *dl = sema_decls; dl; dl = dl->next) {
        if (!dl->decl || dl->decl->kind != DECL_TYPE_ALIAS) continue;
        Id *an = dl->decl->as.type_alias_decl.name;
        if (!an) continue;
        if (an->length == L && memcmp(an->name, n, L) == 0) {
            return dl->decl;
        }
    }
    return NULL;
}

/* Extract refinement bounds [lo, hi] from a type alias's constraint
   list. Returns true and writes the bounds if at least one is found. */
static bool niche_extract_refinement_bounds(Decl *alias_decl,
                                            long long *out_lo,
                                            long long *out_hi) {
    if (!alias_decl || alias_decl->kind != DECL_TYPE_ALIAS) return false;
    ExprList *cs = alias_decl->as.type_alias_decl.constraints;
    if (!cs) return false;
    bool found = false;
    long long lo = LLONG_MIN, hi = LLONG_MAX;
    for (ExprList *c = cs; c; c = c->next) {
        if (!c->expr || c->expr->kind != EXPR_BINARY) continue;
        Expr *rhs = c->expr->as.binary_expr.right;
        if (!rhs || rhs->kind != EXPR_LITERAL) continue;
        long long k = rhs->as.literal_expr.value;
        switch (c->expr->as.binary_expr.op) {
            case TOKEN_ANGLE_BRACKET_LEFT_EQUAL:   // <= k
                if (hi > k)     hi = k;     found = true; break;
            case TOKEN_ANGLE_BRACKET_LEFT:         // < k → <= k-1
                if (hi > k - 1) hi = k - 1; found = true; break;
            case TOKEN_ANGLE_BRACKET_RIGHT_EQUAL:  // >= k
                if (lo < k)     lo = k;     found = true; break;
            case TOKEN_ANGLE_BRACKET_RIGHT:        // > k → >= k+1
                if (lo < k + 1) lo = k + 1; found = true; break;
            case TOKEN_EQUAL_EQUAL:                // == k
                lo = hi = k; found = true; break;
            default: break;  // != and other ops ignored for bounds
        }
    }
    if (found) { *out_lo = lo; *out_hi = hi; }
    return found;
}

/* Resolve a type alias to its underlying base type, if it points to
   an integer type. Returns the underlying Type* or NULL. */
static Type *niche_alias_underlying_int(Decl *alias_decl) {
    if (!alias_decl || alias_decl->kind != DECL_TYPE_ALIAS) return NULL;
    Expr *e = alias_decl->as.type_alias_decl.expr;
    if (!e) return NULL;
    if (e->kind == EXPR_TYPE && e->as.type_expr.type_value) {
        Type *t = e->as.type_expr.type_value;
        int bits; bool sgn;
        if (niche_parse_iN_uN(t, &bits, &sgn)) return t;
        // accept `int` alias too
        if (t->kind == TYPE_SIMPLE && t->base_type &&
            t->base_type->length == 3 &&
            memcmp(t->base_type->name, "int", 3) == 0) {
            return t;
        }
    }
    return NULL;
}

static SentinelPool niche_pool_for_field(Type *field_ty) {
    if (!field_ty) {
        SentinelPool p = {0};
        return p;
    }
    /* Direct pool query for pointer / slice / bool. */
    SentinelPool p = compute_sentinel_pool(field_ty);
    if (p.kind != POOL_EMPTY) return p;

    /* M6 cascade: the field type might be another niche-optimized enum.
       Its physical layout is the payload type with empties already
       occupying the first N sentinel slots — skip them. */
    Decl *inner_enum = niche_find_enum_by_name(field_ty);
    if (inner_enum) {
        NicheLayout inner = niche_compute_layout(&inner_enum->as.enum_decl);
        if (inner.is_zero_cost) {
            Variant *pv = enum_payload_variant(&inner_enum->as.enum_decl);
            if (pv) {
                Type *phys = pv->fields->decl->as.variable_decl.type;
                SentinelPool base = compute_sentinel_pool(phys);
                if (base.kind != POOL_EMPTY) {
                    base.index_offset = (long long)inner.empty_sentinels_count;
                    long long used = (long long)inner.empty_sentinels_count;
                    base.size = base.size > used ? base.size - used : 0;
                    if (base.size == 0) {
                        SentinelPool empty = {0};
                        return empty;
                    }
                    return base;
                }
            }
        }
    }

    /* Sprint D follow-up: integer refinement payload niche.
       If the field type is a TypeAlias referencing iN/uN with
       refinement constraints, derive a split pool from those
       bounds. Example: `type Pressure = i32 >= 0 and <= 1000`
       gives pool = [INT_MIN, -1] ∪ [1001, INT_MAX]. */
    Decl *alias = niche_find_alias_by_name(field_ty);
    if (alias) {
        Type *under = niche_alias_underlying_int(alias);
        long long ref_lo, ref_hi;
        if (under && niche_extract_refinement_bounds(alias, &ref_lo, &ref_hi)) {
            return compute_sentinel_pool_integer_refined(under, ref_lo, ref_hi);
        }
    }

    return p;
}

static NicheLayout niche_compute_layout(DeclEnum *e) {
    NicheLayout L = {0};
    L.payload_variant_count = niche_count_payload_variants(e);
    L.empty_variant_count   = niche_count_empty_variants(e);

    /* Pure-empty enum: represented as a small integer with sequential
       sentinels 0, 1, 2, ... assigned in declaration order. Trivially
       zero-cost. */
    if (L.payload_variant_count == 0) {
        L.is_zero_cost = true;
        L.needs_tag_byte = false;
        SentinelPool p = {0};
        p.kind = POOL_EMPTY;
        L.pool = p;
        L.empty_sentinels = arena_push_many_aligned(
            sema_arena, long long, L.empty_variant_count);
        L.empty_sentinels_count = L.empty_variant_count;
        for (size_t i = 0; i < L.empty_variant_count; i++) {
            L.empty_sentinels[i] = (long long)i;
        }
        return L;
    }

    /* M2 conservative: single payload variant with a single field. */
    Variant *payload_v = NULL;
    for (Variant *v = e->variants; v; v = v->next) {
        if (v->fields) { payload_v = v; break; }
    }

    /* Sprint D follow-up: 2-payload niche.
       Pattern: outer has exactly 2 payload variants and 0 empties.
       One payload (primary) has a single field with non-empty pool;
       the other (secondary) has a single field of type "enum of pure
       empties". Encode each sub-empty as a sentinel of the primary. */
    if (L.payload_variant_count == 2 && L.empty_variant_count == 0) {
        Variant *p1 = NULL, *p2 = NULL;
        for (Variant *v = e->variants; v; v = v->next) {
            if (v->fields) {
                if (!p1) p1 = v;
                else if (!p2) { p2 = v; break; }
            }
        }
        for (int swap = 0; swap < 2; swap++) {
            Variant *prim = swap ? p2 : p1;
            Variant *sec  = swap ? p1 : p2;
            if (!prim || !sec) continue;
            if (!prim->fields || prim->fields->next != NULL) continue;
            if (!sec->fields || sec->fields->next != NULL) continue;
            Type *prim_ty = prim->fields->decl->as.variable_decl.type;
            Type *sec_ty  = sec->fields->decl->as.variable_decl.type;
            SentinelPool pool = niche_pool_for_field(prim_ty);
            if (pool.kind == POOL_EMPTY) continue;
            Decl *sub_enum = niche_find_enum_by_name(sec_ty);
            if (!sub_enum) continue;
            if (niche_count_payload_variants(&sub_enum->as.enum_decl) != 0) continue;
            size_t sub_count = niche_count_empty_variants(&sub_enum->as.enum_decl);
            if (sub_count == 0 || (long long)sub_count > pool.size) continue;
            /* Success. */
            L.pool = pool;
            L.is_zero_cost = true;
            L.needs_tag_byte = false;
            L.primary_variant   = prim;
            L.secondary_variant = sec;
            L.secondary_subenum_decl = sub_enum;
            L.secondary_sentinels = arena_push_many_aligned(
                sema_arena, long long, sub_count);
            L.secondary_sentinels_count = sub_count;
            for (size_t i = 0; i < sub_count; i++) {
                L.secondary_sentinels[i] = sentinel_pick(&pool, i);
            }
            return L;
        }
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

    /* W120: pool insufficient (some/all empty variants need a tag byte). */
    if (L.needs_tag_byte && L.payload_variant_count > 0) {
        L.pool_was_short = true;  // signal to the caller to emit W120
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
│ Codegen helpers (consumed by emit/decl.h and emit/stmt.h)        │
╚─────────────────────────────────────────────────────────────────*/

/* True if this enum should use niche-optimized layout. */
static bool enum_is_zero_cost_niche(Decl *d, NicheLayout *out) {
    if (!d || d->kind != DECL_ENUM) return false;
    NicheLayout L = niche_compute_layout(&d->as.enum_decl);
    if (out) *out = L;
    return L.is_zero_cost && !L.needs_tag_byte;
}

/* The single payload variant of a niche-optimized enum (NULL if
   pure-empty). For pure-empty enums the layout has 0 payloads. */
static Variant *enum_payload_variant(DeclEnum *e) {
    for (Variant *v = e->variants; v; v = v->next) {
        if (v->fields) return v;
    }
    return NULL;
}

/* Sentinel for the i-th empty variant of a niche layout (by declaration
   order in the enum). Returns 0 fallback if out of range or not zero-cost. */
static long long niche_sentinel_for_variant(DeclEnum *e, Variant *target,
                                            NicheLayout *L) {
    if (!L->is_zero_cost || !L->empty_sentinels || target->fields) return 0;
    size_t idx = 0;
    for (Variant *v = e->variants; v; v = v->next) {
        if (v->fields) continue;
        if (v == target) return L->empty_sentinels[idx];
        idx++;
    }
    return 0;
}

/*─────────────────────────────────────────────────────────────────╗
│ W120 — warning emitted when niche pool insufficient              │
╚─────────────────────────────────────────────────────────────────*/

static void niche_emit_w120(DeclEnum *e, NicheLayout *L) {
    if (!L->pool_was_short) return;
    Id *en = e->type_name;
    fprintf(stderr,
        "[W120] Warning: enum '%.*s' not fully zero-cost.\n"
        "       Payload provides %lld sentinel slot(s); %zu empty variant(s) require %zu.\n"
        "       Layout falls back to 1 tag byte + payload union.\n"
        "       To eliminate the tag byte: reduce empty variants, constrain the\n"
        "       payload type with a refinement, or change payload to a type with\n"
        "       larger sentinel space (i8: 255, i16: 65535, *T: %zu).\n",
        en ? (int)en->length : 1, en ? en->name : "?",
        (long long)L->pool.size,
        L->empty_variant_count, L->empty_variant_count,
        target.zero_page_size / (target.pointer_alignment ? target.pointer_alignment : 1));
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
    if (L->is_zero_cost && L->secondary_sentinels) {
        Id *sn = L->secondary_variant->name;
        fprintf(stderr, "[niche]   multi-payload secondary=%.*s sub_count=%zu\n",
                (int)sn->length, sn->name, L->secondary_sentinels_count);
        for (size_t i = 0; i < L->secondary_sentinels_count; i++) {
            fprintf(stderr, "[niche]     %.*s[%zu] = %lld\n",
                    (int)sn->length, sn->name, i, L->secondary_sentinels[i]);
        }
    }
}

#endif /* SEMA_NICHE_H */
