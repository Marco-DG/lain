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
// The algorithm was src/sema/niche.h's, ported to IrType, and that file is now deleted: this is
// the only layout decision, and W120, --dump-niche and E064 read it (ir_emit_layout_report).
#include "ir.h"
#include "../target.h"
#include "../layout_core.h"   // the sentinel algebra

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

// A payload the packed paths can carry as the sum itself: they convert with a C cast `(T)v`,
// which C allows for a scalar and refuses for a struct, a slice or an array.
static bool ir_layout_scalar(IrType *t) {
    if (!t) return false;
    if (t->kind == IRT_INT || t->kind == IRT_BOOL || t->kind == IRT_PTR || t->kind == IRT_FLOAT)
        return true;
    return t->kind == IRT_SUM && ir_layout_of(t).packed;    // a scalar typedef itself
}

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

// ── THE TYPES C SEES (DECIDE-U) ──────────────────────────────────────────────────────────
// Kept here because what C reads is a LAYOUT question: a struct's or a sum payload's storage
// order, and whether an enumeration may donate its spare values (ir_pool_for). The emitter
// fills it (ir_iface_collect) before the first layout question is asked of a module.
#define IR_IFACE_MAX 512
static IrName *ir_iface_name[IR_IFACE_MAX];
static int     ir_iface_n = 0;
static bool ir_iface_has(const IrName *n) {
    if (!n) return false;
    for (int k = 0; k < ir_iface_n; k++)
        if (ir_iface_name[k]->length == n->length && memcmp(ir_iface_name[k]->name, n->name, (size_t)n->length) == 0)
            return true;
    return false;
}
static bool ir_layout_iface(const IrType *t) { return ir_iface_n < 0 || ir_iface_has(t->sname); }

// The spare bit patterns of a payload type — the ones that cannot be a legitimate value, so
// they are free to mean "this is one of the payload-less variants".
static SentinelPool ir_pool_for(IrType *t) {
    SentinelPool p = {0}; p.kind = POOL_EMPTY;
    if (!t) return p;

    // A pointer: the zero page, which no user mapping can occupy.
    //
    // ⚠ A SLICE IS NOT INCLUDED, though the deleted src/sema/niche.h once did. The reasoning is
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
        // A PLAIN ENUMERATION with a DECLARED width (`type K u8 { A, B, C }`): its values are
        // 0..n-1, so n..2^w-1 are spare. Only a declared width: the default width is the
        // smallest that holds the variants, so one more variant could move every sentinel, and a
        // layout that depends on how many variants happen to exist today is not a layout a
        // program can rely on. A declared width fixes the pool.
        //
        // ★ AND ONLY AN ENUMERATION C CANNOT HAND US. The niche is sound only while no value of
        // the enum holds a spare pattern, and Lain can make one in no way (no cast reaches an
        // enum). C can: `extern func get() K` may return 3, and a callback `*func(K) i32` given
        // to C may be called with it. A K that holds 3 inside `Opt` would read as None. So an
        // enumeration an extern's signature reaches donates nothing, as a struct C reads keeps
        // its declared order (DECIDE-U).
        if (inner.packed && inner.all_empty && t->sum_backing_bits > 0 && t->n_fields > 0 &&
            !ir_layout_iface(t)) {
            long long tlo, thi;
            if (!sentinel_int_range(t->sum_backing_bits, false, &tlo, &thi)) return p;
            return sentinel_pool_int_refined(tlo, thi, 0, (long long)t->n_fields - 1);
        }
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

/* ir_sentinel_pick: gone. The ASSIGNMENT is sentinel_pick() in layout_core.h — two backends
   that both pack but choose different patterns disagree about what `none` IS. */

// THE decision. Cheap and pure, so callers may ask per use rather than caching.
static IrLayout ir_layout_of(IrType *sum) {
    IrLayout L = {0};
    L.primary = -1;
    if (!sum || sum->kind != IRT_SUM || sum->n_fields <= 0) return L;

    for (int k = 0; k < sum->n_fields; k++) {
        if (sum->fields[k]) { L.payload_count++; if (L.primary < 0) L.primary = k; }
        else                  L.empty_count++;
    }

    // Every variant payload-less: a plain integer whose values are the ordinals 0,1,2,... in
    // declaration order, so it needs no sentinel table and no cap. ★ This was decided AFTER the
    // cap below, and a plain enum of 65 variants or more came out as `struct { int32_t tag; }`,
    // four bytes where spec 19 promises one, with every gate green (the behaviour is the same).
    // The tables are still filled where they fit; the emitter reads the ordinal, not the table.
    if (L.payload_count == 0) {
        L.packed = true; L.all_empty = true;
        for (int k = 0; k < sum->n_fields && k < IR_LAYOUT_MAX_VARIANTS; k++) {
            L.sentinel[k] = k; L.has_sentinel[k] = true;
        }
        return L;
    }
    if (sum->n_fields > IR_LAYOUT_MAX_VARIANTS) { L.primary = -1; return L; }

    // More than one payload, or a payload with more than one field: nothing to pack into.
    if (L.payload_count != 1) { L.primary = -1; return L; }
    IrType *back = ir_layout_single_field(sum, L.primary);
    if (!back) { L.primary = -1; return L; }

    // ONE variant, carrying one field, and nothing else: the sum IS its payload, with nothing to
    // tell apart and so nothing to store. It asked for a pool all the same, and `{ Only { x i32 } }`
    // got a tag because an i32 has no spare values, which it was never going to need. Only a
    // SCALAR payload: the packed paths convert with a C cast, which a struct or a slice refuses.
    if (L.empty_count == 0 && ir_layout_scalar(back)) {
        L.packed = true; L.backing = back;
        return L;
    }

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

// WHY a sum carries a tag, in words a programmer can act on. It reads the same tests as
// ir_layout_of in the same order, so the warning (W120) and the refusal (E064) cannot say one
// thing while the emitter does another: they were decided in the front end (src/sema/niche.h),
// which believed in a multi-payload niche this backend never built, and so stayed silent over
// `Result(*T, Err)`'s tag, and reported "0 empty variant(s) require 0" over a tag nothing needed.
// Returns false, writing nothing, when the sum is packed. `*short_pool` says the reason is the
// payload's spare values, the one a refinement or a different payload type can fix.
static bool ir_layout_why_tagged(IrType *sum, char *buf, size_t n, bool *short_pool) {
    *short_pool = false;
    IrLayout L = ir_layout_of(sum);
    if (L.packed || !sum || sum->kind != IRT_SUM) return false;
    if (sum->n_fields > IR_LAYOUT_MAX_VARIANTS) {
        snprintf(buf, n, "it has %d variants, and spare values are searched for at most %d",
                 sum->n_fields, IR_LAYOUT_MAX_VARIANTS);
        return true;
    }
    if (L.payload_count != 1) {
        snprintf(buf, n, "%d of its variants carry a payload, and only a sum with ONE "
                 "payload-carrying variant can store the others in the payload's spare values",
                 L.payload_count);
        return true;
    }
    int k = 0; while (k < sum->n_fields && !sum->fields[k]) k++;
    IrName *vn = sum->field_names ? sum->field_names[k] : NULL;
    int vl = vn ? (int)vn->length : 1; const char *vs = vn ? vn->name : "?";
    IrType *back = ir_layout_single_field(sum, k);
    if (!back) {
        snprintf(buf, n, "variant '%.*s' carries %d fields, and only a single-field payload can "
                 "be the sum itself", vl, vs, sum->fields[k] ? sum->fields[k]->n_fields : 0);
        return true;
    }
    if (L.empty_count == 0 && !ir_layout_scalar(back)) {
        snprintf(buf, n, "variant '%.*s' carries a %s, and only a scalar payload can be the sum "
                 "itself", vl, vs, back->kind == IRT_SLICE ? "slice" : back->kind == IRT_ARRAY ?
                 "fixed array" : back->kind == IRT_VECTOR ? "vector" : "struct");
        return true;
    }
    // A declared-width enumeration that C can reach has spare values Lain cannot rely on.
    if (back->kind == IRT_SUM && back->sum_backing_bits > 0 && back->sname &&
        ir_layout_of(back).all_empty && ir_layout_iface(back)) {
        snprintf(buf, n, "its payload is the enumeration '%.*s', which an extern's signature reaches, "
                 "so C may hand it any value of its width and none is spare",
                 (int)back->sname->length, back->sname->name);
        return true;
    }
    SentinelPool pool = ir_pool_for(back);
    long long spare = pool.kind == POOL_EMPTY ? 0 : (long long)pool.size;
    *short_pool = true;
    char who[96];                    // a `T | markers` union's payload variant is internal
    if (vl >= 2 && vs[0] == '_' && vs[1] == '_') snprintf(who, sizeof who, "the value type");
    else snprintf(who, sizeof who, "the payload of '%.*s'", vl, vs);
    snprintf(buf, n, "%s has %lld spare value%s, and %d payload-less variant%s",
             who, spare, spare == 1 ? "" : "s", L.empty_count,
             L.empty_count == 1 ? " needs one" : "s need one each");
    return true;
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

// A plain enumeration is the unsigned integer its declaration states (`type K u16 { A, B }`,
// the parser checked that the variants fit), or else the smallest that holds its tags 0..n-1
// (a405172).
static int ir_plain_enum_bits(const IrType *sum) {
    if (sum->sum_backing_bits) return sum->sum_backing_bits;
    int n = sum->n_fields;
    return n <= 256 ? 8 : n <= 65536 ? 16 : 32;
}

// A tagged sum's tag (I.11) is the variant's number, sized as a plain enumeration is: the
// smallest unsigned integer that holds 0..n-1. It was always an `int32_t`, so a sum of byte
// payloads, `{ A { x u8 }, B { y u8 } }`, was 8 bytes where 2 hold it, and its 4-byte alignment
// padded every struct and array around it. A sum an extern's signature reaches keeps the
// `int32_t` C was written against. Every reader of the width asks here: the emitter's struct,
// the ordering alignment below, the interpreter's size model, W120.
static int ir_sum_tag_bits(const IrType *sum) {
    return ir_layout_iface(sum) ? 32 : ir_plain_enum_bits(sum);
}
static const char *ir_sum_tag_ctype(const IrType *sum) {
    if (ir_layout_iface(sum)) return "int32_t";
    switch (ir_plain_enum_bits(sum)) { case 8: return "uint8_t"; case 16: return "uint16_t";
                                       case 64: return "uint64_t"; default: return "uint32_t"; }
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
            if (L.all_empty) return ir_plain_enum_bits(t) / 8;
            if (L.backing && L.backing->kind == IRT_BOOL) return 1;     // stored as a uint8_t
            return ir_fixed_size(L.backing);
        }
        default: return 0;
    }
}

// ── DECIDE-U: A STRUCT'S STORAGE ORDER IS THE COMPILER'S ─────────────────────────────────────
// `type Token { kind TokenKind  pos u32  len u16 }` was 12 bytes in declaration order and is 8
// with its fields by decreasing alignment; nothing in the source said which, and a lexer had
// carried the first, commented "8 bytes", for six weeks. Construction and field access are BY
// NAME (the emitter writes `.pos = v1`), and Lain has no stored references, no pointer
// arithmetic on fields and no @offsetof, so inside a program the order is not observable —
// only its size is, and the smaller one is the one asked for. Where the layout IS an interface
// it keeps the declaration: [ordered] (a file or wire format), [packed] (bit order is the
// declaration), and any struct reachable from an extern's signature (the emitter decides that
// — it sees the module).

// The alignment this ordering plans with: C's natural alignment for every type the emitter
// spells. It decides ORDER only; any order is correct C, so an alignment the C compiler sees
// differently costs bytes, never correctness.
static int ir_order_align(const IrType *t) {
    if (!t) return 1;
    int ptr = target.pointer_alignment ? (int)target.pointer_alignment : 8;
    switch (t->kind) {
        case IRT_INT:    return ir_int_storage_bits(t->bits) / 8;
        case IRT_BOOL:   return 1;
        case IRT_FLOAT:  return t->float_bits == 32 ? 4 : 8;
        case IRT_VECTOR: { int64_t s = ir_fixed_size((IrType *)t); return s > 0 ? (int)s : 16; }
        case IRT_ARRAY:  return ir_order_align(t->elem);
        case IRT_STRUCT: {
            IrStructLayout P = ir_struct_layout(t);
            if (P.packed) return P.container_bits / 8;
            int a = 1;
            for (int k = 0; k < t->n_fields; k++) { int f = ir_order_align(t->fields[k]); if (f > a) a = f; }
            return a;
        }
        case IRT_SUM: {
            IrLayout L = ir_layout_of((IrType *)t);
            if (L.packed && L.all_empty) return ir_plain_enum_bits(t) / 8;
            if (L.packed && L.backing)
                return L.backing->kind == IRT_BOOL ? 1 : ir_order_align(L.backing);
            int a = ir_sum_tag_bits(t) / 8;              // the tag
            for (int k = 0; k < t->n_fields; k++) { int f = ir_order_align(t->fields[k]); if (f > a) a = f; }
            return a;
        }
        case IRT_UNIT: case IRT_NEVER: return 1;
        default:         return ptr;                     // pointers, slices, function pointers
    }
}

#define IR_REORDER_MAX_FIELDS 512
// The storage order of a struct's fields: ord[q] is the declared index of the field stored
// q-th. By decreasing alignment, equal alignments keeping their declaration order (spec 7,
// struct types). `interface` is the emitter's answer to "does C see this struct?".
static void ir_struct_storage_order(const IrType *st, bool interface, int *ord) {
    int n = st->n_fields;
    for (int k = 0; k < n; k++) ord[k] = k;
    if (interface || st->ordered_decl || st->packed_decl || n > IR_REORDER_MAX_FIELDS ||
        !st->field_names) return;
    for (int k = 0; k < n; k++) if (!st->field_names[k]) return;   // an unnamed field is positional
    for (int k = 1; k < n; k++) {                                   // stable: insertion sort
        int x = ord[k], ax = ir_order_align(st->fields[x]), j = k - 1;
        while (j >= 0 && ir_order_align(st->fields[ord[j]]) < ax) { ord[j + 1] = ord[j]; j--; }
        ord[j + 1] = x;
    }
}

// The storage order of variant k's payload in a tagged sum (`.data.Wide`): a struct's, decided
// by the SUM. A payload is built and read by field name (`.data.Wide = { .a = v1, ... }`,
// `v.data.Wide.b`), so it is no more observable than a struct's order. It was emitted in
// declaration order while the interpreter's size model reordered it: `{a u8, b i64, c u8}` made
// the sum 32 bytes in C and 24 in `lain --interpret`. `interface` is the emitter's answer for
// the sum itself; the payload's own name is its variant's, which a struct may share.
static void ir_sum_payload_order(const IrType *sum, int k, bool interface, int *ord) {
    ir_struct_storage_order(sum->fields[k], interface || sum->ordered_decl, ord);
}

#endif // LAIN_IR_LAYOUT_H
