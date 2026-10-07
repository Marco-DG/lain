#ifndef SEMA_TYPECHECK_H
#define SEMA_TYPECHECK_H

#include <limits.h>
#include "../ast.h"
#include "resolve.h"
#include "resolve.h"
#include "ranges.h" // Range analysis

extern Arena *sema_arena;
extern DeclList *sema_decls;
extern Type *current_return_type;
static Type *union_lower(Type *u);   // sema/monomorph.h: `T | M` as its synthesized enum
extern Decl *current_function_decl; // Defined in sema.h
extern RangeTable *sema_ranges;     // Defined in sema.h
extern bool sema_in_unsafe_block;   // Defined in sema.h
extern bool sema_walk_phase;        // Defined in sema.h
extern bool sema_addr_of_context;   // Defined in sema.h — set by EXPR_ADDR to relax &arr[len]

// ...



// ...



/*─────────────────────────────────────────────────────────────────╗
│ 1) Helpers to get a builtin “int” Type* only once               │
╚─────────────────────────────────────────────────────────────────*/

// The default for a naked literal is i32 — but a literal whose VALUE does not fit i32 is not
// an i32, and typing it one silently TRUNCATED it: `var x i64 = 5000000000` became 705032704.
// The old back end never noticed because it pastes the literal into a C initializer and lets
// the C compiler pick the type; anything that materialises a typed value sees the lie.
static Type *get_builtin_i64_type(void) {
  static Type *i64_ty = NULL;
  if (!i64_ty) {
    Id *id = arena_push_aligned(sema_arena, Id);
    id->name = "i64";
    id->length = 3;
    i64_ty = type_simple(sema_arena, id);
  }
  return i64_ty;
}
static Type *sa_u64_type(void) {                // a shift of constants that needs all 64 bits
  static Type *u64_ty = NULL;
  if (!u64_ty) {
    Id *id = arena_push_aligned(sema_arena, Id);
    id->name = "u64";
    id->length = 3;
    u64_ty = type_simple(sema_arena, id);
  }
  return u64_ty;
}
Type *get_builtin_i32_type(void) {
  // Q-002 / int-removal: the default integer type for naked literals
  // is i32. The function name is kept for historical reasons but the
  // returned Type is concretely i32.
  static Type *int_ty = NULL;
  if (!int_ty) {
    Id *id = arena_push_aligned(sema_arena, Id);
    id->name = "i32";
    id->length = 3;
    int_ty = type_simple(sema_arena, id);
  }
  return int_ty;
}

// ★ `bool` IS NOT AN INTEGER (spec 07: "an implicit conversion between integer types and bool
// is ill-formed"). The front end typed `true`, `false`, every comparison and every `and`/`or`/`!`
// as i32, so the rule could not be enforced — refusing int→bool would have refused
// `var b bool = false` — and `take(true)` for a usize, `take(1)` for a bool, `return a < b` from
// an i32 function and `n + (a < b)` all compiled. A boolean is now typed `bool` where it is made.
static Type *get_builtin_bool_type(void) {
  static Type *bool_ty = NULL;
  if (!bool_ty) {
    Id *id = arena_push_aligned(sema_arena, Id);
    id->name = "bool";
    id->length = 4;
    bool_ty = type_simple(sema_arena, id);
  }
  return bool_ty;
}

Type *get_builtin_u8_type(void) {
  static Type *u8_ty = NULL;
  if (!u8_ty) {
    // make a fake Id for “u8”
    Id *id = arena_push_aligned(sema_arena, Id);
    id->name = "u8";
    id->length = 2;
    u8_ty = type_simple(sema_arena, id);
  }
  return u8_ty;
}

/*─────────────────────────────────────────────────────────────────╗
│ 1b) Implicit integer widening helpers                          │
╚─────────────────────────────────────────────────────────────────*/

// Q-002 helpers: parse iN / uN. Returns 0 if not an iN/uN type.
// On success, *out_bits is the N (1..64), *out_signed is true for iN.
static int parse_iN_uN(Type *t, int *out_bits, bool *out_signed) {
    if (!t || t->kind != TYPE_SIMPLE || !t->base_type) return 0;
    // P2/Stage0: width is authoritative — set eagerly in type_simple. The lazy
    // branch remains only as a fallback for any Type built without the
    // constructor (int_width_cache still 0 = uncomputed there).
    if (t->int_width_cache == 0) {
        ast_parse_int_width(t->base_type->name, t->base_type->length,
                            &t->int_width_cache, &t->int_signed_cache);
    }
    if (t->int_width_cache < 0) return 0;
    *out_bits   = t->int_width_cache;
    *out_signed = t->int_signed_cache;
    return 1;
}

static bool is_integer_type(Type *t) {
    if (!t || t->kind != TYPE_SIMPLE || !t->base_type) return false;
    const char *n = t->base_type->name;
    isize len = t->base_type->length;
    // Generic iN / uN (Q-002: N=1..64)
    int bits; bool sgn;
    if (parse_iN_uN(t, &bits, &sgn)) return true;
    // Pointer-sized.
    if (len == 5 && (memcmp(n,"usize",5)==0 || memcmp(n,"isize",5)==0)) return true;
    // `int` documented alias of i32.
    if (len == 3 && memcmp(n, "int", 3) == 0) return true;
    return false;
}

// An integer type's representation, width and signedness: `usize`/`isize` at the target's pointer
// width, `int` as i32, a refinement alias as its base. Two integer types with one shape are one
// type to an operator with an overflow policy (I.56).
static bool sema_int_shape(Type *t, int *bits, bool *sgn) {
    t = resolve_type_alias(t);
    if (!t || !is_integer_type(t)) return false;
    if (parse_iN_uN(t, bits, sgn)) return true;
    const char *n = t->base_type->name;
    if (t->base_type->length == 5) { *bits = (int)target.pointer_size * 8; *sgn = n[0] == 'i'; return true; }
    *bits = 32; *sgn = true;                                    // `int`
    return true;
}
// An integer literal, negated or not: it has no type of its own to insist on.
static bool sema_int_literal_operand(Expr *x) {
    if (x && x->kind == EXPR_UNARY && x->as.unary_expr.op == TOKEN_MINUS) x = x->as.unary_expr.right;
    return x && x->kind == EXPR_LITERAL && !x->as.literal_expr.is_bool;
}
// Plain arithmetic, `+ - * /`: its type is the one the compiler chose so that it cannot overflow
// (`x + 1` on an i32 is an i33, spec 07 rank), not one the program states.
static bool sema_widened_operand(Expr *x) {
    if (!x || x->kind != EXPR_BINARY) return false;
    TokenKind k = x->as.binary_expr.op;
    return k == TOKEN_PLUS || k == TOKEN_MINUS || k == TOKEN_ASTERISK || k == TOKEN_SLASH;
}
// Every value of an integer type of shape (bb, bs) is one of shape (ab, as).
static bool sema_int_shape_holds(int ab, bool as, int bb, bool bs) {
    if (as == bs) return ab >= bb;
    return as && ab > bb;                                    // a signed type holds a narrower unsigned one
}

static bool is_bool_type(Type *t) {
    while (t && t->kind == TYPE_COMPTIME) t = t->element_type;
    if (!t || t->kind != TYPE_SIMPLE || !t->base_type) return false;
    return t->base_type->length == 4 && memcmp(t->base_type->name, "bool", 4) == 0;
}

static bool is_float_type(Type *t);
// A float literal, negated or not, takes the float type `t` (I.161): the literal node and every
// `-` above it, so the constant is lowered, and rounds, in that type.
static void sema_float_literal_as(Expr *x, Type *t) {
    for (Expr *q = x; q; q = (q->kind == EXPR_UNARY && q->as.unary_expr.op == TOKEN_MINUS) ? q->as.unary_expr.right : NULL) {
        if (q->kind != EXPR_UNARY && q->kind != EXPR_FLOAT_LITERAL) return;
        q->type = t;
        if (q->kind == EXPR_FLOAT_LITERAL) return;
    }
}
static bool is_float_type(Type *t) {
    if (!t || t->kind != TYPE_SIMPLE || !t->base_type) return false;
    return t->base_type->length == 3 &&
           (memcmp(t->base_type->name, "f32", 3) == 0 ||
            memcmp(t->base_type->name, "f64", 3) == 0);
}

// P2/S3: reject an implicit float<->int conversion at a boundary (lossy — Lain
// requires an explicit `as` cast). Exits with E012 on violation.
static void reject_float_int_mismatch(Type *from, Type *to, isize line, isize col,
                                      const char *what, const char *label) {
    if ((is_float_type(from) && is_integer_type(to)) ||
        (is_integer_type(from) && is_float_type(to))) {
        fprintf(stderr, "[E012] Error Ln %li, Col %li: implicit conversion between "
            "float and integer in %s '%s' — use an explicit 'as' cast.\n",
            (long)line, (long)col, what, label ? label : "");
        diagnostic_show_line(line, col);
        exit(1);
    }
}

// P2/S2: the refinement interval carried ON a type. Reads the cached `refine`
// field; if empty, derives it from a refinement alias's constraints (`type Small
// = i32 >= 0 and <= 9`) and CACHES it on the node — the type-level home of the
// value-range refinement, dual-written with the alias-constraint machinery. The
// canonical `refine` field is what a later subsumption relation (S3) and the
// LLVM `!range` lowering will read, instead of a name-keyed side lookup.
static ExprList *alias_constraints_for(Type *t);   // defined below (needs sema_lookup)
static Refinement type_refine_interval(Type *t) {
    Refinement none = { false, 0, 0 };
    if (!t) return none;
    while (t && t->kind == TYPE_COMPTIME) t = t->element_type;
    if (!t) return none;
    if (t->refine.known) return t->refine;
    ExprList *cs = alias_constraints_for(t);
    if (cs) {
        Range r = range_from_refinement_constraints(cs);
        if (r.known) {
            t->refine.known = true; t->refine.lo = r.min; t->refine.hi = r.max;
            return t->refine;
        }
    }
    return none;
}

// Q-002 Phase 5 helpers: range of a sized integer type and fit check.
// Returns 1 if t has a known fixed-width integer range; sets *out_lo/*out_hi.
// Handles iN/uN with N ∈ [1, 64] plus legacy `int` (treated as i32).
// Non-static so ranges.h (included earlier) can forward-declare it.
int type_integer_range(Type *t, long long *out_lo, long long *out_hi) {
    if (!t || t->kind != TYPE_SIMPLE || !t->base_type) return 0;
    int bits; bool sgn;
    if (parse_iN_uN(t, &bits, &sgn)) {
        if (sgn) {
            // i64: half-bound trick to avoid signed overflow with 1LL<<63.
            if (bits == 64) {
                *out_lo = LLONG_MIN;
                *out_hi = LLONG_MAX;
            } else {
                *out_lo = -(1LL << (bits - 1));
                *out_hi =  (1LL << (bits - 1)) - 1;
            }
        } else {
            *out_lo = 0;
            if (bits >= 63) {
                *out_hi = LLONG_MAX;  // uN with N ∈ [63, 64] approximated
            } else {
                *out_hi = (1LL << bits) - 1;
            }
        }
        return 1;
    }
    // `int` alias → i32 range.
    const char *n = t->base_type->name;
    isize len = t->base_type->length;
    if (len == 3 && memcmp(n, "int", 3) == 0) {
        *out_lo = -2147483648LL;
        *out_hi =  2147483647LL;
        return 1;
    }
    // P2/S2: a refinement alias (`type Small = i32 >= 0 and <= 9`) has no builtin
    // width, but its refinement interval IS its integer range — read it from the
    // type. Additive: this path previously returned 0 (unknown), so callers that
    // fell back to a rank/permissive comparison now get the tighter, sound range
    // that already travels with the type.
    Refinement rf = type_refine_interval(t);
    if (rf.known) {
        *out_lo = rf.lo;
        *out_hi = rf.hi;
        return 1;
    }
    return 0;
}

// ★ `check_value_fits_type` IS DELETED. It was the legacy BOUNDARY-FIT check — "does this value's
// range fit the target type" — at assignment, return, call argument, struct field initialiser, enum
// payload, arithmetic result, negation and cast. With the seam set it returned true immediately, so
// every one of those rejections was already dead, and the sovereign engine answers them as
// VRA_OVERFLOW / vra_check_narrow. Each site was verified with a written violation the engine refuses
// before this came out, which is what closes D-47 (it recorded three of them as UNCOVERED).
//
// Its fail-open WINDOW is worth remembering rather than mourning: a range whose extremum sat within
// 4096 of INT64_MAX was treated as "no info", which is how an unsigned product wrap was missed once.
// A per-site obligation in the IR has no such window.

// P2/S3: true static integer-type subsumption — does EVERY value of `from`
// fit in `to`? Compares the exact [lo,hi] type ranges, so it is correct for
// BOTH width and signedness (unlike rank comparison, which wrongly makes
// i32 <: u32 and i8 <: u8). Returns false if either type has no fixed range
// (usize/isize/unknown) — the caller decides what to do with that.
static bool int_type_subsumes(Type *from, Type *to) {
    long long flo, fhi, tlo, thi;
    if (!type_integer_range(from, &flo, &fhi)) return false;
    if (!type_integer_range(to,   &tlo, &thi)) return false;
    return flo >= tlo && fhi <= thi;
}

// An implicit integer narrowing is the IR's question now (vra_check_narrow, at every narrowing
// site: assignment, return, argument, field, payload). The front end's reject_lossy_int_conversion
// returned on every path once that landed, and was deleted with its two helpers.

// Rank in the implicit widening order (Q-002 extended).
// Rank is essentially the container bit-width category:
//   N=1..8  → 1     (8-bit container)
//   N=9..16 → 2     (16-bit container)
//   N=17..32→ 3     (32-bit container)
//   N=33..64→ 4     (64-bit container)
//   usize/isize → 5 (pointer-sized, may be 32 or 64)
//   int → 3         (default i32 monomorphization)
int integer_rank(Type *t);
int integer_rank(Type *t) {
    if (!t || t->kind != TYPE_SIMPLE || !t->base_type) return 0;
    int bits; bool sgn;
    if (parse_iN_uN(t, &bits, &sgn)) {
        if (bits <= 8) return 1;
        if (bits <= 16) return 2;
        if (bits <= 32) return 3;
        return 4;  // 33..64
    }
    const char *n = t->base_type->name;
    isize len = t->base_type->length;
    if (len == 5 && (memcmp(n,"usize",5)==0 || memcmp(n,"isize",5)==0)) return 5;
    if (len == 3 && memcmp(n, "int", 3) == 0) return 3;  // alias of i32
    return 0;
}

static Type *wider_integer_type(Type *a, Type *b) {
    return integer_rank(a) >= integer_rank(b) ? a : b;
}

// F3.5 Path-F: build a fresh iN/uN Lain type by width (the interned constructor
// dedups). `id()` does not copy the name bytes, so the buffer is arena-owned.
static Type *make_int_type(int width, bool is_signed) {
    char buf[8];
    int n = snprintf(buf, sizeof buf, "%c%d", is_signed ? 'i' : 'u', width);
    char *stored = arena_push_many(sema_arena, char, (isize)n);
    memcpy(stored, buf, (size_t)n);
    return type_simple(sema_arena, id(sema_arena, (isize)n, stored));
}

// F3.5 Path-F: the result type of `a <op> b` is WIDE ENOUGH to hold every value,
// so `+ - *` cannot overflow at the operation — overflow becomes possible only at
// a *narrowing* (checked there, with the cast tiers as the escape). Widths use a
// closed formula (no range arithmetic, so no long-long overflow): add/sub grow by
// one bit (plus a sign bit when an unsigned operand feeds a signed result);
// multiply sums the operand widths. Beyond 64 bits (the i128 ceiling) we fall
// back to max(operands) and the op-level overflow check still guards it. Platform
// types (usize/isize) and non-fixed-width operands are not widened.
static Type *resolve_type_alias(Type *t);   // fwd
static Type *path_f_result_type(Type *a, Type *b, TokenKind op) {
    int wa, wb; bool sa, sb;
    // A division by an ALIAS of a signed integer (`type NonZero = i32 != 0`) is a division by
    // that integer, and must widen like one — or its TYPE_MIN / -1 case stays at native width.
    if (op == TOKEN_SLASH) { a = resolve_type_alias(a); b = resolve_type_alias(b); }
    if (!parse_iN_uN(a, &wa, &sa) || !parse_iN_uN(b, &wb, &sb))
        return wider_integer_type(a, b);
    bool rsigned = (op == TOKEN_MINUS) || sa || sb;
    int w;
    if (op == TOKEN_SLASH) {
        // DECIDE-M: a SIGNED quotient needs one bit more than its operands — only
        // TYPE_MIN / -1 uses it, and that is exactly the case C leaves undefined. Computed in
        // the widened type the operation has no UB; the question moves to the narrowing, where
        // the quotient's range decides it. An unsigned quotient never exceeds its dividend.
        if (!(sa && sb)) return wider_integer_type(a, b);
        w = (wa > wb ? wa : wb) + 1;
    } else if (op == TOKEN_ASTERISK) {
        w = wa + wb;                                   // product: sum of widths
    } else { // + or -
        if (rsigned) {
            int ea = sa ? wa : wa + 1;                 // unsigned operand needs +1 bit signed
            int eb = sb ? wb : wb + 1;
            w = (ea > eb ? ea : eb) + 1;               // +1 for carry/borrow
        } else {
            w = (wa > wb ? wa : wb) + 1;
        }
    }
    if (w > 64) return wider_integer_type(a, b);       // ceiling: op-level check guards
    if (w < 1) w = 1;
    // Fresh per-occurrence node (not the interned core) so the Path-F marker does
    // not pollute every iN; canon still points to the interned core.
    Type *t = arena_push_aligned(sema_arena, Type);
    *t = *make_int_type(w, rsigned);
    t->arith_widened = true;
    return t;
}

// (Q-002 Phase 4 paradigm_b_result_type was reverted.
//  Rationale: `iN op iM = iK` smallest-container widening was
//  cognitively surprising (i8 + i8 = i9). The result type now
//  follows Sprint 10 widening (`iN op iM = max(iN, iM)`); overflow
//  is caught at the assignment boundary via Phase 5 (E086) when
//  the source VRA range doesn't fit the target type.)

static bool can_widen_to(Type *from, Type *to) {
    if (!is_integer_type(from) || !is_integer_type(to)) return false;
    if (from == to) return true;
    long long a, b, c, d;
    // Platform-width types (usize/isize) have no fixed range: fall back to the
    // conservative rank rule for them.
    if (!type_integer_range(from, &a, &b) || !type_integer_range(to, &c, &d))
        return integer_rank(from) <= integer_rank(to);
    // Fixed-width: true range subsumption (correct for width AND signedness,
    // so i32 does NOT widen to u32, nor i8 to u8).
    return int_type_subsumes(from, to);
}

/* ─────────────────────────────────────────────────────────────────╗
│ Sprint 5 (Q-004 step A): pointer-bearing type detection           │
│ A type is "pointer-bearing" if any of its values can carry a      │
│ raw pointer (and therefore borrow tracking is relevant).          │
╚─────────────────────────────────────────────────────────────────*/

// F-022 support: structural type compatibility for argument-vs-field check.
// Conservative: returns true for same simple type, integer widening,
// pointer-to-same-element, or if either operand has no inferred type.
// Returns false on clear mismatches (int vs bool, struct A vs struct B, etc.).
// P2/S1: sound, TOTAL structural core identity — do two types denote the same
// canonical CORE (mode- and refinement-stripped)? The interned `canon` pointer
// is an O(1) fast-path: it is set only to a structurally-identical representative,
// so canon-equal ⇒ core-identical, soundly (mov i32, var i32, i32 all share it).
// When canon is unset or unequal we fall back to a structural, mode-agnostic
// comparison that reads the CURRENT fields — so it stays correct even after an
// in-place mode change (EXPR_ADDR) or generic substitution (which mutate a node
// after construction and would otherwise leave a dedup'd canon stale). Never
// compares ownership mode; sized arrays are core-identical only by node identity
// (their size is a refinement, discharged separately).
static bool core_identical_depth(Type *a, Type *b, int depth) {
    if (a == b) return true;
    if (!a || !b) return false;
    if (depth > 32) return false;                       // cycle/blowup guard
    while (a && a->kind == TYPE_COMPTIME) a = a->element_type;
    while (b && b->kind == TYPE_COMPTIME) b = b->element_type;
    if (a == b) return true;
    if (!a || !b) return false;
    if (a->canon && b->canon && a->canon == b->canon) return true;   // O(1) sound fast-path
    if (a->kind != b->kind) return false;
    switch (a->kind) {
        case TYPE_SIMPLE: {
            if (!a->base_type || !b->base_type) return false;
            if (!id_bytes_equal(a->base_type, b->base_type)) return false;
            // Generic applications: Vec(i32) and Vec(u8) share a name, differ in args.
            if ((a->type_args == NULL) != (b->type_args == NULL)) return false;
            TypeList *pa = a->type_args, *pb = b->type_args;
            while (pa && pb) {
                if (!core_identical_depth(pa->type, pb->type, depth + 1)) return false;
                pa = pa->next; pb = pb->next;
            }
            return pa == NULL && pb == NULL;
        }
        case TYPE_POINTER:
            return core_identical_depth(a->element_type, b->element_type, depth + 1);
        case TYPE_ARRAY:
            if (a->size_expr || b->size_expr) return false;  // sized cores: same-node identity only
            if (a->array_len != b->array_len) return false;
            return core_identical_depth(a->element_type, b->element_type, depth + 1);
        case TYPE_VECTOR:
            if (a->array_len != b->array_len) return false;  // lane count
            return core_identical_depth(a->element_type, b->element_type, depth + 1);
        case TYPE_SLICE:
            if (a->has_sentinel != b->has_sentinel) return false;
            return core_identical_depth(a->element_type, b->element_type, depth + 1);
        case TYPE_UNION: {
            if (!core_identical_depth(a->element_type, b->element_type, depth + 1)) return false;
            IdList *ma = a->union_markers, *mb = b->union_markers;
            while (ma && mb) {
                if (!id_bytes_equal(ma->id, mb->id)) return false;
                ma = ma->next; mb = mb->next;
            }
            return ma == NULL && mb == NULL;
        }
        case TYPE_FUNC: {
            if (a->func_effects != b->func_effects) return false;   // the arrow's row is part of the type
            if (!core_identical_depth(a->element_type, b->element_type, depth + 1)) return false; // return (NULL=void)
            TypeList *pa = a->func_params, *pb = b->func_params;
            while (pa && pb) {
                if (!core_identical_depth(pa->type, pb->type, depth + 1)) return false;
                pa = pa->next; pb = pb->next;
            }
            return pa == NULL && pb == NULL;
        }
        case TYPE_META:
            return true;                                 // every `type` meta is one core
        case TYPE_VARIANT:
            return a->variant == b->variant;
        default:
            return false;
    }
}
static bool core_identical(Type *a, Type *b) { return core_identical_depth(a, b, 0); }

// A scalar `as`-castable type: integer, float, or bool. `as` converts only
// between these (and raw pointers inside unsafe); casting an aggregate
// (struct/enum/array/slice) to/from a scalar is nonsense and emits broken C.
static bool is_castable_scalar(Type *t) {
    if (!t) return false;
    while (t && t->kind == TYPE_COMPTIME) t = t->element_type;
    if (!t || t->kind != TYPE_SIMPLE || !t->base_type) return false;
    if (is_integer_type(t) || is_float_type(t)) return true;  // iN/uN/int/usize/isize/fN
    if (t->base_type->length == 4 && memcmp(t->base_type->name, "bool", 4) == 0) return true;
    if (t->base_type->length == 5 && memcmp(t->base_type->name, "float", 5) == 0) return true; // alias of f64
    return false;
}

static bool types_compatible(Type *from, Type *to) {
    if (!from || !to) return true;  // missing info → skip
    // Unwrap comptime wrappers
    while (from && from->kind == TYPE_COMPTIME) from = from->element_type;
    while (to && to->kind == TYPE_COMPTIME) to = to->element_type;
    if (!from || !to) return true;
    // P2/Stage0: canonical CORE identity — total across ownership modes, so
    // `mov i32` matches `i32` by pointer instead of falling to strncmp.
    if (core_identical(from, to)) return true;
    // Integer widening
    if (is_integer_type(from) && is_integer_type(to)) {
        return can_widen_to(from, to);
    }
    if (from->kind != to->kind) return false;
    switch (from->kind) {
        case TYPE_SIMPLE: {
            if (!from->base_type || !to->base_type) return true;
            if (from->base_type->length != to->base_type->length) return false;
            return strncmp(from->base_type->name, to->base_type->name,
                           from->base_type->length) == 0;
        }
        case TYPE_POINTER:
            return types_compatible(from->element_type, to->element_type);
        case TYPE_ARRAY:
            // Same element; length must match when both known
            if (!types_compatible(from->element_type, to->element_type)) return false;
            if (from->array_len >= 0 && to->array_len >= 0 &&
                from->array_len != to->array_len) return false;
            return true;
        case TYPE_SLICE:
            return types_compatible(from->element_type, to->element_type);
        default:
            return false; // P2/S3: sound — an unhandled kind is not assumed compatible
    }
}

// ★ A FUNCTION NAMED AS A MEMBER, NOT CALLED: `p.helper` with `func helper(p P)` in scope. UFCS
// makes `p.helper()` the call `helper(p)`; without the parentheses there is nothing to call and no
// field to read. It reached lowering as an unresolved member and was refused as E100 "not
// supported by the code generator yet", which reads like a missing feature.
static bool sema_member_is_callee = false;
// The member expression an assignment STORES to (STMT_ASSIGN sets it while typing its target): its
// write through a raw pointer is E009's to report, with E009's text, not a read (I.125).
static Expr *sema_store_target = NULL;
static void type_describe(Type *t, char *buf, size_t cap);
static void sema_report_function_as_member(Expr *e, Type *t) {
    char tb[128]; type_describe(t, tb, sizeof tb);
    int ml = (int)e->as.member_expr.member->length; const char *mn = e->as.member_expr.member->name;
    fprintf(stderr, "[E128] Error Ln %li, Col %li: '%.*s' is a function, not a member of '%s': call "
            "it, `x.%.*s()`, which is `%.*s(x)`.\n", (long)e->line, (long)e->col, ml, mn, tb, ml, mn,
            ml, mn);
    diagnostic_show_line(e->line, e->col);
    exit(1);
}

// P2/S3: render a Type into `buf` for diagnostics (best-effort, a couple of
// levels of pointer/slice/array nesting; falls back to "?").
static void type_describe(Type *t, char *buf, size_t cap) {
    if (cap == 0) return;
    buf[0] = '\0';
    while (t && t->kind == TYPE_COMPTIME) t = t->element_type;
    if (!t) { snprintf(buf, cap, "?"); return; }
    char inner[96];
    switch (t->kind) {
        case TYPE_SIMPLE:
            // A union's synthesized enum (`__U_i32_NotFound`) is shown as the user wrote it,
            // `i32 | NotFound`: the mangled name is the compiler's, and a message comparing two
            // unions printed both mangled (I.96).
            if (t->base_type && t->base_type->length > 4 && strncmp(t->base_type->name, "__U_", 4) == 0) {
                char nm[256];
                int nl = (int)t->base_type->length < 255 ? (int)t->base_type->length : 255;
                memcpy(nm, t->base_type->name, (size_t)nl); nm[nl] = '\0';
                Symbol *us = sema_lookup(nm);
                Decl *ud = us ? us->decl : NULL;
                if (ud && ud->kind == DECL_ENUM && ud->as.enum_decl.is_union && ud->as.enum_decl.variants &&
                    ud->as.enum_decl.variants->fields && ud->as.enum_decl.variants->fields->decl) {
                    type_describe(ud->as.enum_decl.variants->fields->decl->as.variable_decl.type, inner, sizeof inner);
                    size_t o = (size_t)snprintf(buf, cap, "%s", inner);
                    for (Variant *m = ud->as.enum_decl.variants->next; m && o < cap; m = m->next)
                        o += (size_t)snprintf(buf + o, cap - o, " | %.*s", (int)m->name->length, m->name->name);
                    break;
                }
            }
            if (t->base_type)
                snprintf(buf, cap, "%.*s", (int)t->base_type->length, t->base_type->name);
            else snprintf(buf, cap, "?");
            break;
        case TYPE_POINTER:
            type_describe(t->element_type, inner, sizeof inner);
            snprintf(buf, cap, "*%s", inner);
            break;
        case TYPE_SLICE:
            type_describe(t->element_type, inner, sizeof inner);
            snprintf(buf, cap, t->has_sentinel ? "%s[:0]" : "%s[]", inner);
            break;
        case TYPE_ARRAY:
            type_describe(t->element_type, inner, sizeof inner);
            if (t->array_len >= 0) snprintf(buf, cap, "%s[%lld]", inner, (long long)t->array_len);
            else snprintf(buf, cap, "%s[]", inner);
            break;
        case TYPE_UNION: {  // `T | m1 | m2`: a message about a union said '?' (I.94)
            type_describe(t->element_type, inner, sizeof inner);
            size_t o = (size_t)snprintf(buf, cap, "%s", inner);
            for (IdList *m = t->union_markers; m && o < cap; m = m->next)
                o += (size_t)snprintf(buf + o, cap - o, " | %.*s", (int)m->id->length, m->id->name);
            break;
        }
        case TYPE_VECTOR:   // a message about a vector said '?' (a SIMD receiver's E128)
            type_describe(t->element_type, inner, sizeof inner);
            snprintf(buf, cap, "Vec(%lld, %s)", (long long)t->array_len, inner);
            break;
        case TYPE_FUNC: {   // the parameter types, then the return type; the row is not shown
            size_t o = (size_t)snprintf(buf, cap, "*func(");
            for (TypeList *pt = t->func_params; pt && o < cap; pt = pt->next) {
                type_describe(pt->type, inner, sizeof inner);
                o += (size_t)snprintf(buf + o, cap - o, "%s%s", inner, pt->next ? ", " : "");
            }
            if (o < cap) {
                if (t->element_type) type_describe(t->element_type, inner, sizeof inner);
                o += (size_t)snprintf(buf + o, cap - o, t->element_type ? ") %s" : ")", inner);
            }
            // The row is part of the type: two arrows that differ only in it were both described
            // as '*func(i32) i32', in a message refusing to convert one into the other.
            const char *sep = " effects ";
            if (t->func_effects & EFFECT_IO)      { if (o < cap) o += (size_t)snprintf(buf + o, cap - o, "%sio", sep); sep = ", "; }
            if (t->func_effects & EFFECT_DIVERGE) { if (o < cap) o += (size_t)snprintf(buf + o, cap - o, "%sdiverge", sep); sep = ", "; }
            if (t->func_effects & EFFECT_RAISES)  { if (o < cap) o += (size_t)snprintf(buf + o, cap - o, "%sraises", sep); sep = ", "; }
            if (t->func_effects & EFFECT_ALLOC)   { if (o < cap) o += (size_t)snprintf(buf + o, cap - o, "%salloc", sep); sep = ", "; }
            break;
        }
        default:
            snprintf(buf, cap, "?");
    }
}

// P2/S3: peel refinement/type-alias layers to the underlying base type. A
// refinement alias (`type SmallPos = i32 >= 1`) is registered as a symbol whose
// ->type is the base type and ->decl is the alias; the alias's own refinement
// is enforced separately (E086), so for kind-compatibility it IS its base.
static Type *resolve_type_alias(Type *t) {
    extern Symbol *sema_lookup(const char *name);
    for (int guard = 0; guard < 8; guard++) {
        if (!t || t->kind != TYPE_SIMPLE || !t->base_type) return t;
        if ((size_t)t->base_type->length >= 128) return t;
        char buf[128];
        memcpy(buf, t->base_type->name, t->base_type->length);
        buf[t->base_type->length] = '\0';
        Symbol *sym = sema_lookup(buf);
        if (sym && sym->decl && sym->decl->kind == DECL_TYPE_ALIAS &&
            sym->type && sym->type != t) {
            t = sym->type;   // peel one alias layer
            continue;
        }
        return t;
    }
    return t;
}

// ── Canonical-type keystone (K1) ─────────────────────────────────────────────
// The best-known CONSTANT interval for a type — the {lo,hi} of {ν:τ | lo≤ν≤hi}.
// Reads the on-type refinement / a refinement alias's constraints; failing that,
// an integer type is bounded by its own full range. ⊤ (known=false) otherwise.
// It is read in place of the name-keyed range table.
static Refinement type_interval(Type *t) {
    Refinement iv = type_refine_interval(t);         // refine field or alias constraints
    if (iv.known) return iv;
    Type *u = t;
    while (u && u->kind == TYPE_COMPTIME) u = u->element_type;
    long long lo, hi;
    if (u && type_integer_range(u, &lo, &hi)) {
        Refinement r = { true, lo, hi };             // an integer type's own range
        return r;
    }
    return iv;                                        // ⊤ — unknown/unbounded
}

// The unified numeric-boundary relation: integer→integer, does the value provably
// fit the target? Source interval = the SOURCE type's own constraint (alias bounds
// / range, sound since enforced) tightened by the VRA range `r`; target constraint
// = type_integer_range(to) (never a stray refine). No core requirement — this
// decides SAME-core refinement narrowing AND CROSS-core narrowing/widening with the
// one relation. Non-integer either side ⟹ false (falls through to the type/float/
// pointer/aggregate checks). `r ⊆ target` is exactly what the scattered numeric
// checks verify, so a true verdict is behavior-preserving.
static bool value_fits(Type *from, Range r, Type *to) {
    if (!from || !to) return false;
    // Peel comptime wrappers, as type_interval does — otherwise a comptime-wrapped
    // integer (e.g. a literal's `comptime i64`) reads as "not an integer" and the
    // boundary needlessly falls through to the permissive name-keyed path even for
    // a trivially-fitting value like 0.
    while (from && from->kind == TYPE_COMPTIME) from = from->element_type;
    while (to   && to->kind   == TYPE_COMPTIME) to   = to->element_type;
    if (!from || !to) return false;
    long long tlo, thi, flo, fhi;
    if (!type_integer_range(to,   &tlo, &thi)) return false;   // target not an integer type
    if (!type_integer_range(from, &flo, &fhi)) return false;   // source not an integer type
    long long slo = flo, shi = fhi;
    if (r.known) { if (r.min > slo) slo = r.min; if (r.max < shi) shi = r.max; }
    return slo >= tlo && shi <= thi;                           // source interval ⊆ target range
}

// ── Operation-precondition proof predicates (S4-lite) ────────────────────────
// Each operation whose safety depends on its operands' VALUES (division needs a
// nonzero divisor; a shift needs an in-range amount; …) states that precondition
// as ONE reusable predicate over the SAME proven interval the keystone reads —
// rather than an inline snippet re-derived per operation. A new operator's rule
// is then a single call, not a bespoke block, and every precondition of the same
// shape shares one proof path. Call sites keep their own diagnostics.

// Strict structural type equality (NO widening, NO decay). Used where variance
// is unsound — the element types behind a pointer/slice/array must match
// invariantly (so *i32 is NOT interchangeable with *u8).
static bool types_equal_exact(Type *a, Type *b) {
    if (a == b) return true;
    if (!a || !b) return true;                    // missing info: don't judge
    while (a && a->kind == TYPE_COMPTIME) a = a->element_type;
    while (b && b->kind == TYPE_COMPTIME) b = b->element_type;
    a = resolve_type_alias(a);
    b = resolve_type_alias(b);
    if (a == b) return true;
    if (!a || !b) return true;
    if (a->kind != b->kind) return false;
    switch (a->kind) {
        case TYPE_SIMPLE:
            if (!a->base_type || !b->base_type) return true;
            if (a->base_type->length != b->base_type->length) return false;
            return strncmp(a->base_type->name, b->base_type->name,
                           a->base_type->length) == 0;
        case TYPE_POINTER:
        case TYPE_SLICE:
            return types_equal_exact(a->element_type, b->element_type);
        case TYPE_ARRAY:
            if (!types_equal_exact(a->element_type, b->element_type)) return false;
            if (a->array_len >= 0 && b->array_len >= 0 &&
                a->array_len != b->array_len) return false;
            return true;
        default:
            return false;
    }
}

static bool is_zero_int_literal(Expr *e) {
    return e && e->kind == EXPR_LITERAL && e->as.literal_expr.value == 0;
}

// A TYPE_SIMPLE naming a user struct or enum (nominal aggregate), as opposed to
// a builtin scalar (iN/uN/bool/fN). Distinct nominal types never implicitly
// convert to each other or to a scalar.
static bool is_nominal_aggregate(Type *t) {
    if (!t || t->kind != TYPE_SIMPLE || !t->base_type) return false;
    if ((size_t)t->base_type->length >= 128) return false;
    char buf[128];
    memcpy(buf, t->base_type->name, t->base_type->length);
    buf[t->base_type->length] = '\0';
    extern Symbol *sema_lookup(const char *name);
    Symbol *sym = sema_lookup(buf);
    return sym && sym->decl &&
           (sym->decl->kind == DECL_STRUCT || sym->decl->kind == DECL_ENUM);
}

// ★ WHAT A `case` MATCHES ON (spec 15): an integer (a `u8` character too), `bool`, an enum or
// sum, or a string (`u8[]`, matched by bytes and length). Nothing else has patterns, and nothing
// said so: a `case` statement on an f64, a struct or an array was DROPPED by lowering, until it
// was refused as an unmodelled construct. Nor was a pattern checked against its scrutinee:
// `case x { "ab": … }` on an i32 compiled to `x == (Slice_u8){…}`, C that gcc rejects.
static bool sema_is_u8_string(Type *t) {
    t = resolve_type_alias(t);
    if (!t || (t->kind != TYPE_ARRAY && t->kind != TYPE_SLICE)) return false;
    Type *el = resolve_type_alias(t->element_type);
    int b; bool sg;
    return el && parse_iN_uN(el, &b, &sg) && b == 8 && !sg;
}
static void sema_check_case_scrutinee(Expr *val, isize line, isize col) {
    Type *t = val ? resolve_type_alias(val->type) : NULL;
    if (!t) return;
    const char *what = NULL;
    if (is_float_type(t))                                   what = "a float";
    else if (t->kind == TYPE_POINTER)                       what = "a pointer";
    else if (t->kind == TYPE_FUNC)                          what = "a function pointer";
    else if (t->kind == TYPE_VECTOR)                        what = "a vector";
    else if ((t->kind == TYPE_ARRAY || t->kind == TYPE_SLICE) && !sema_is_u8_string(t))
                                                            what = "an array that is not a `u8` string";
    else if (is_nominal_aggregate(t) && !find_enum_decl(t)) what = "a struct";
    if (!what) return;
    fprintf(stderr, "[E012] Error Ln %li, Col %li: a `case` cannot match on %s: the scrutinee "
            "must be an integer, `bool`, an enum or sum, or a `u8[]` string.\n", line, col, what);
    diagnostic_show_line(line, col);
    exit(1);
}
// spec 15: the arms of a `case` EXPRESSION have compatible types; spec 7: no implicit conversion
// between `bool` and an integer, or an integer and a float. Nothing compared the arms: the
// result took the FIRST arm's type, so `1: 5  else: true` was an i32 whose `true` arm became 1.
// Aggregates, strings and unions are checked where the value is used; an arm with no type of its
// own (a `panic`) has nothing to compare.
static bool sema_arm_types_agree(Type *a, Type *b) {
    a = resolve_type_alias(a); b = resolve_type_alias(b);
    if (!a || !b) return true;
    if (is_bool_type(a) || is_bool_type(b))       return is_bool_type(a) && is_bool_type(b);
    if (is_integer_type(a) || is_integer_type(b)) return is_integer_type(a) && is_integer_type(b);
    if (is_float_type(a) || is_float_type(b))     return is_float_type(a) && is_float_type(b);
    return true;
}
static void sema_check_case_pattern_kinds(Expr *val, ExprList *patterns) {
    bool str = val && sema_is_u8_string(val->type);
    for (ExprList *p = patterns; p; p = p->next) {
        Expr *pe = p->expr;
        if (!pe) continue;
        const char *bad = NULL;
        if (str && pe->kind != EXPR_STRING)       bad = "a pattern on a string scrutinee must be a string literal";
        else if (!str && pe->kind == EXPR_STRING) bad = "a string pattern needs a `u8[]` scrutinee";
        else if (pe->kind == EXPR_FLOAT_LITERAL)  bad = "a float literal is not a pattern";
        if (!bad) continue;
        fprintf(stderr, "[E012] Error Ln %li, Col %li: %s.\n", pe->line, pe->col, bad);
        diagnostic_show_line(pe->line, pe->col);
        exit(1);
    }
}

// P2/S3: reject POINTER type-confusion at a boundary — the memory-unsafe
// conversions with no legitimate implicit counterpart:
//   * two pointers with different pointee types (*i32 <-> *u8) — aliasing lie
//   * pointer <-> non-pointer scalar (int/float/struct <-> *T) — fabricating or
//     reinterpreting an address (so `func f(a i32) *i32 { return a }` is caught)
// Legitimate exceptions preserved: array/slice DECAY to a pointer (u8[] -> *u8,
// T[N] -> *T[N]), the null idiom (integer literal 0 -> *T), explicit `as`
// casts (source type already matches), and anything inside an `unsafe` block.
// Non-pointer mismatches (bool<->int, struct<->struct, array element policy)
// are intentionally deferred to the dedicated checks / the full subsumption
// relation — this pass is scoped to pointer safety only.
static void reject_incompatible_conversion(Type *from, Type *to, Expr *src_expr,
                                           isize line, isize col,
                                           const char *ctx, const char *label) {
    // Not waived by `unsafe`, which licenses memory operations, not conversions between
    // incompatible types: `unsafe { return s }` with `s u8[]` returned as an i32 was accepted.
    if (!from || !to) return;
    Type *f = from, *t = to;
    while (f && f->kind == TYPE_COMPTIME) f = f->element_type;
    while (t && t->kind == TYPE_COMPTIME) t = t->element_type;
    if (!f || !t) return;
    f = resolve_type_alias(f);
    t = resolve_type_alias(t);
    // ★ ONE UNION, TWO REPRESENTATIONS (I.96). A union-returning CALL still has the raw `T | M` type
    // here, while a written-out union (a parameter's, a binding's) was already lowered to its
    // synthesized niche-optimized enum, so `show(find())` was refused as converting a union into
    // itself. Lower the raw side the same way: union_lower deduplicates by the mangled name, so it
    // yields the very type the other side has, and two different unions still differ.
    if (f->kind == TYPE_UNION && t->kind != TYPE_UNION) f = union_lower(f);
    if (t->kind == TYPE_UNION && f->kind != TYPE_UNION) t = union_lower(t);
    if (f == t) return;
    bool f_ptr = (f->kind == TYPE_POINTER);
    bool t_ptr = (t->kind == TYPE_POINTER);
    bool ok = false;
    if (f_ptr && t_ptr) {
        ok = types_equal_exact(f->element_type, t->element_type);        // same pointee
    } else if (f_ptr || t_ptr) {
        Type *ptr   = f_ptr ? f : t;
        Type *other = f_ptr ? t : f;
        if (other->kind == TYPE_ARRAY || other->kind == TYPE_SLICE)
            ok = types_equal_exact(other->element_type, ptr->element_type)  // T[] -> *T
              || types_equal_exact(other, ptr->element_type);               // T[N] -> *T[N]
        if (!ok && t_ptr && is_zero_int_literal(src_expr)) ok = true;    // null idiom: 0 -> *T
    } else {
        // Neither is a pointer. Every boundary goes through here — argument, construction,
        // declaration, assignment, return — so this is where "these two types have no
        // representation in common" is decided, ONCE. It used to stop at nominal confusion
        // and let the rest through: `take(5)` for a `u8[]` parameter compiled to C that gcc
        // rejects, and `take(buf)` for a `usize` passed a POINTER as the integer and ran. (`take(1)`
        // for a `bool` contradicts the spec too — 07-types: an implicit integer/bool
        // conversion is ill-formed.) The struct constructor had a stricter check of its own —
        // stricter in the right places, and wrong about `W("abcd", 1)`. One relation now.
        //
        // AND the integer/bool half of that rule, now that booleans are typed `bool` where they
        // are made: `take(true)` for a usize and `take(1)` for a bool are refused, as spec 07
        // says; `as` converts explicitly.
        bool f_seq = (f->kind == TYPE_ARRAY || f->kind == TYPE_SLICE);
        bool t_seq = (t->kind == TYPE_ARRAY || t->kind == TYPE_SLICE);
        // (A vector type is neither a sequence nor a scalar here — an array literal initialises
        // one — so the sequence rule is asked only against a scalar or a nominal type.)
        bool f_scal = is_castable_scalar(f) || is_nominal_aggregate(f);
        bool t_scal = is_castable_scalar(t) || is_nominal_aggregate(t);
        if ((is_bool_type(f) && is_integer_type(t)) || (is_integer_type(f) && is_bool_type(t))) {
            ok = false;
        } else if ((f_seq && (t_seq || t_scal)) || (t_seq && (f_seq || f_scal))) {
            // a sequence flows only into a sequence of the SAME element type: an array decays
            // to a slice, a `u8[:0]` is a `u8[]` (the sentinel is dropped, never gained — see
            // reject_sentinel_fabrication), and an element type never converts.
            // An array LITERAL (or comprehension) is exempt from the element rule: its
            // elements are literals typed by default (`[1, 2]` is `i32[2]`) and each one is
            // checked against the element type where the literal is declared.
            bool lit = src_expr && (src_expr->kind == EXPR_ARRAY_LITERAL ||
                                    src_expr->kind == EXPR_ARRAY_COMPREHENSION);
            ok = f_seq && t_seq && (lit || types_equal_exact(f->element_type, t->element_type));
        } else {
            if (!is_nominal_aggregate(f) && !is_nominal_aggregate(t)) return;
            ok = types_compatible(f, t);   // same struct/enum name (mode-agnostic) is fine
        }
    }
    if (ok) return;
    char fb[128], tb[128];
    // A bare marker the target union does not declare (I.97). Its type is the tag of a union that
    // does declare it, which printed as 'i32'; name the marker and the union it belongs to.
    Decl *O = (src_expr && src_expr->kind == EXPR_IDENTIFIER) ? src_expr->decl : NULL;
    if (O && O->kind == DECL_ENUM && O->as.enum_decl.is_union && src_expr->as.identifier_expr.variant) {
        Variant *v = src_expr->as.identifier_expr.variant;
        isize vl = v->name->length;
        {
            type_describe(type_simple(sema_arena, O->as.enum_decl.type_name), fb, sizeof fb);
            type_describe(t, tb, sizeof tb);
            fprintf(stderr,
                "[E012] Error Ln %li, Col %li: %s '%s' has incompatible type: '%.*s' is a marker of "
                "'%s', and '%s' declares no marker by that name.\n",
                (long)line, (long)col, ctx, label ? label : "", (int)vl, v->name->name, fb, tb);
            diagnostic_show_line(line, col);
            exit(1);
        }
    }
    type_describe(f, fb, sizeof fb);
    type_describe(t, tb, sizeof tb);
    fprintf(stderr,
        "[E012] Error Ln %li, Col %li: %s '%s' has incompatible type: cannot implicitly "
        "convert '%s' to '%s'.\n",
        (long)line, (long)col, ctx, label ? label : "", fb, tb);
    diagnostic_show_line(line, col);
    exit(1);
}

// ── DECIDE-O: A MODULE-SCOPE `assert` ────────────────────────────────────────────────────────
// The predicate must be a CONSTANT: literals, arithmetic, comparisons, `and`/`or`/`!`, `as`, and
// @sizeof/@alignof. A predicate with no layout in it is decided here (E134 when false). One that
// measures a type is decided by the C compiler, the only party that knows the number; the
// lowering hands it to the emitter as a `_Static_assert` carrying the same code.
// A NAMED constant — an immutable module-level `N i32 = 64` — is a constant expression too:
// `assert BUF % 16 == 0` is the commonest layout claim there is. Its initialiser stands in for it
// (a C `static const` is not a C constant expression, so the emitter substitutes it as well).
static Expr *sa_named_constant(Expr *e) {
    if (!e || e->kind != EXPR_IDENTIFIER || !e->decl || e->decl->kind != DECL_VARIABLE) return NULL;
    if (e->decl->as.variable_decl.is_mutable) return NULL;
    return e->decl->as.variable_decl.init;
}
static int sa_depth = 0;
// I.8: a comprehension's index, bound while its body is evaluated at compile time.
static Id *sa_bound_idx = NULL; static __int128 sa_bound_val = 0;
static bool sa_is_bound(Expr *e) {
    Id *n = e->as.identifier_expr.id;
    return sa_bound_idx && n && n->length == sa_bound_idx->length &&
           strncmp(n->name, sa_bound_idx->name, (size_t)n->length) == 0;
}
// ── I.140: A WRAPPING OPERATION IS A CONSTANT EXPRESSION, wrapped at its type ──────────────────
// Spec 10's closed list had `as` and no wrapping operator, so a table written `[300 as% u8, 1]` or
// `[250 +% 10, 1]` was not static data: it lost its hull and its exact values (`assert T[0] == 44`
// was E012), and a module assert could not use one. A wrapping operation's value is fixed by its
// TYPE, which sema decided (a literal-only operation takes the destination's since I.141), so it is
// evaluated over the integers and wrapped to that type's width: two's complement for a signed one.
// Computed unsigned on 128 bits, whose wrap agrees with any narrower one. No type, no constant.
static bool sa_wrap_to(Type *t, __int128 *v) {
    while (t && t->kind == TYPE_COMPTIME) t = t->element_type;
    Type *ta = t ? resolve_type_alias(t) : NULL;
    int bits = 0; bool sgn = false;
    if (!ta || !parse_iN_uN(ta, &bits, &sgn) || bits < 1 || bits > 64) return false;
    unsigned __int128 m = (((unsigned __int128)1) << bits) - 1, u = ((unsigned __int128)*v) & m;
    *v = (sgn && ((u >> (bits - 1)) & 1)) ? (__int128)u - ((__int128)1 << bits) : (__int128)u;
    return true;
}
static bool sa_is_wrap_binop(TokenKind op) {
    return op == TOKEN_PLUS_PERCENT || op == TOKEN_MINUS_PERCENT || op == TOKEN_ASTERISK_PERCENT;
}
static bool sa_is_const(Expr *e, bool *layout) {
    if (!e) return false;
    switch (e->kind) {
        case EXPR_LITERAL: case EXPR_CHAR: return true;
        case EXPR_IDENTIFIER: {
            if (sa_is_bound(e)) return true;
            Expr *init = sa_named_constant(e);
            if (!init || sa_depth > 16) return false;      // a cycle is not a constant
            sa_depth++; bool ok = sa_is_const(init, layout); sa_depth--;
            return ok;
        }
        case EXPR_BUILTIN:
            if (e->as.builtin_expr.builtin_kind == BUILTIN_SIZEOF ||
                e->as.builtin_expr.builtin_kind == BUILTIN_ALIGNOF) { *layout = true; return true; }
            return false;
        case EXPR_UNARY: {
            TokenKind op = e->as.unary_expr.op;
            return (op == TOKEN_MINUS || op == TOKEN_BANG || op == TOKEN_TILDE) &&
                   sa_is_const(e->as.unary_expr.right, layout);
        }
        case EXPR_BINARY: {
            switch (e->as.binary_expr.op) {
                case TOKEN_PLUS: case TOKEN_MINUS: case TOKEN_ASTERISK: case TOKEN_SLASH:
                case TOKEN_PERCENT: case TOKEN_AMPERSAND: case TOKEN_PIPE: case TOKEN_CARET:
                case TOKEN_EQUAL_EQUAL: case TOKEN_BANG_EQUAL:
                case TOKEN_ANGLE_BRACKET_LEFT: case TOKEN_ANGLE_BRACKET_LEFT_EQUAL:
                case TOKEN_ANGLE_BRACKET_RIGHT: case TOKEN_ANGLE_BRACKET_RIGHT_EQUAL:
                case TOKEN_KEYWORD_AND: case TOKEN_KEYWORD_OR:
                case TOKEN_SHIFT_LEFT: case TOKEN_SHIFT_RIGHT:
                    return sa_is_const(e->as.binary_expr.left, layout) &&
                           sa_is_const(e->as.binary_expr.right, layout);
                case TOKEN_PLUS_PERCENT: case TOKEN_MINUS_PERCENT: case TOKEN_ASTERISK_PERCENT: {
                    __int128 probe = 0;                       // wrapped at a type it must have
                    return sa_wrap_to(e->type, &probe) &&
                           sa_is_const(e->as.binary_expr.left, layout) &&
                           sa_is_const(e->as.binary_expr.right, layout);
                }
                default: return false;
            }
        }
        case EXPR_CAST: {
            if (e->as.cast_expr.kind == CAST_WRAPPING) {          // `as%` (I.140): wrapped at its target
                __int128 probe = 0;
                return sa_wrap_to(e->type, &probe) && sa_is_const(e->as.cast_expr.expr, layout);
            }
            return e->as.cast_expr.kind == CAST_PROVEN && sa_is_const(e->as.cast_expr.expr, layout);
        }
        default: return false;
    }
}
// Evaluate a layout-free constant over the integers (a comparison is 0/1). False on a division
// by zero or a value past 128 bits, which the caller reports as not constant.
static bool sa_eval(Expr *e, __int128 *v) {
    __int128 a, b;
    switch (e->kind) {
        case EXPR_LITERAL: *v = e->as.literal_expr.value; return true;
        case EXPR_CHAR:    *v = (unsigned char)e->as.char_expr.value; return true;
        case EXPR_CAST:
            if (!sa_eval(e->as.cast_expr.expr, v)) return false;
            return e->as.cast_expr.kind == CAST_WRAPPING ? sa_wrap_to(e->type, v) : true;
        case EXPR_IDENTIFIER: {
            if (sa_is_bound(e)) { *v = sa_bound_val; return true; }
            Expr *init = sa_named_constant(e);
            if (!init || sa_depth > 16) return false;
            sa_depth++; bool ok = sa_eval(init, v); sa_depth--;
            return ok;
        }
        case EXPR_UNARY:
            if (!sa_eval(e->as.unary_expr.right, &a)) return false;
            // ★ `~` IS THE ONE OPERATOR WITH NO MEANING OVER THE INTEGERS. `+`, `-`, `*` agree with
            // the language wherever it does not refuse an overflow (Path F); `~` of an UNSIGNED
            // value is fixed by its width, 2^N - 1 - a. Evaluated as -a - 1, `~(0 as u32)` was -1,
            // so `assert ~(0 as u32) < 0` COMPILED, a false claim decided true (the same comparison
            // in a function is false), and `MAX u64 = ~(0 as u64)` was refused as "-1 does not fit
            // u64". A signed operand keeps -a - 1, which is two's complement at every width.
            if (e->as.unary_expr.op == TOKEN_TILDE) {
                Type *ot = e->as.unary_expr.right->type;
                while (ot && ot->kind == TYPE_COMPTIME) ot = ot->element_type;
                int bits = 0; bool sgn = true;
                if (ot && parse_iN_uN(ot, &bits, &sgn) && !sgn && bits >= 1 && bits <= 64) {
                    __int128 mask = ((__int128)1 << bits) - 1;
                    if (a < 0 || a > mask) return false;   // not a value of that type
                    *v = mask - a;
                    return true;
                }
                *v = ~a;
                return true;
            }
            *v = e->as.unary_expr.op == TOKEN_MINUS ? -a : !a;
            return true;
        case EXPR_BINARY: {
            TokenKind op = e->as.binary_expr.op;
            if (!sa_eval(e->as.binary_expr.left, &a)) return false;
            if (op == TOKEN_KEYWORD_AND && !a) { *v = 0; return true; }
            if (op == TOKEN_KEYWORD_OR  &&  a) { *v = 1; return true; }
            if (!sa_eval(e->as.binary_expr.right, &b)) return false;
            if (sa_is_wrap_binop(op)) {                   // I.140: over 128 bits, then wrapped
                unsigned __int128 ua = (unsigned __int128)a, ub = (unsigned __int128)b;
                unsigned __int128 r = op == TOKEN_PLUS_PERCENT ? ua + ub : op == TOKEN_MINUS_PERCENT ? ua - ub : ua * ub;
                *v = (__int128)r;
                return sa_wrap_to(e->type, v);
            }
            switch (op) {
                case TOKEN_PLUS: *v = a + b; return true;
                case TOKEN_MINUS: *v = a - b; return true;
                case TOKEN_ASTERISK: *v = a * b; return true;
                case TOKEN_SLASH: if (!b) return false; *v = a / b; return true;
                case TOKEN_PERCENT: if (!b) return false; *v = a % b; return true;
                case TOKEN_AMPERSAND: *v = a & b; return true;
                case TOKEN_PIPE: *v = a | b; return true;
                case TOKEN_CARET: *v = a ^ b; return true;
                case TOKEN_EQUAL_EQUAL: *v = a == b; return true;
                case TOKEN_BANG_EQUAL: *v = a != b; return true;
                case TOKEN_ANGLE_BRACKET_LEFT: *v = a < b; return true;
                case TOKEN_ANGLE_BRACKET_LEFT_EQUAL: *v = a <= b; return true;
                case TOKEN_ANGLE_BRACKET_RIGHT: *v = a > b; return true;
                case TOKEN_ANGLE_BRACKET_RIGHT_EQUAL: *v = a >= b; return true;
                case TOKEN_KEYWORD_AND: case TOKEN_KEYWORD_OR: *v = (b != 0); return true;
                // A shift over the integers: `(1 << 32) - 1` is 4294967295, whatever width the
                // operands would have in a function. Refused (not constant) where C's would be
                // undefined: a negative operand, a count past 126, a result past 127 bits.
                case TOKEN_SHIFT_LEFT: {
                    if (a < 0 || b < 0 || b > 126) return false;
                    unsigned __int128 r = (unsigned __int128)a << (int)b;
                    if ((r >> (int)b) != (unsigned __int128)a || (r >> 127)) return false;
                    *v = (__int128)r; return true;
                }
                case TOKEN_SHIFT_RIGHT:
                    if (a < 0 || b < 0 || b > 126) return false;
                    *v = a >> (int)b; return true;
                default: return false;
            }
        }
        default: return false;
    }
}
// A module constant's VALUE against its integer type, and an array constant's elements against
// theirs. A narrowing is the IR's obligation where a value lands in a slot (a local's store, an
// argument, a return), and a module constant never lands in one: it is folded into each use. So
// `X u8 = 300` compiled with X == 44. It is a compile-time constant, so it is checked here, exactly.
static void sema_check_one_const_fits(Type *ty, Expr *e, Decl *d, const char *what, const char *name) {
    int bits = 0; bool sgn = false, lay = false; __int128 v;
    Type *t = ty; while (t && t->kind == TYPE_COMPTIME) t = t->element_type;
    if (!t || !e || !parse_iN_uN(t, &bits, &sgn) || bits < 1 || bits > 64) return;
    if (!sa_is_const(e, &lay) || lay || !sa_eval(e, &v)) return;
    __int128 lo = sgn ? -((__int128)1 << (bits - 1)) : 0;
    __int128 hi = sgn ? ((__int128)1 << (bits - 1)) - 1 : (((__int128)1 << bits) - 1);
    if (v >= lo && v <= hi) return;
    char tb[64]; type_describe(t, tb, sizeof tb);
    isize ln = e->line ? e->line : d->line, cl = e->line ? e->col : d->col;
    if (v >= (__int128)INT64_MIN && v <= (__int128)INT64_MAX)
        fprintf(stderr, "[E086] Error Ln %li, Col %li: %s '%s' is %lld, which does not fit %s.\n",
                (long)ln, (long)cl, what, name, (long long)v, tb);
    else
        fprintf(stderr, "[E086] Error Ln %li, Col %li: %s '%s' is outside the 64-bit range, so it "
                "does not fit %s.\n", (long)ln, (long)cl, what, name, tb);
    diagnostic_show_line(ln, cl);
    exit(1);
}
static void sema_check_const_fits(Type *ty, Expr *init, Decl *d, const char *name) {
    Type *t = ty; while (t && t->kind == TYPE_COMPTIME) t = t->element_type;
    if (t && t->kind == TYPE_ARRAY && init && init->kind == EXPR_ARRAY_LITERAL) {
        for (ExprList *el = init->as.array_literal_expr.elements; el; el = el->next)
            sema_check_one_const_fits(t->element_type, el->expr, d, "an element of constant", name);
        return;
    }
    sema_check_one_const_fits(ty, init, d, "constant", name);
}
// I.8: `CTYPE u8[256] = [f(i) for i in 0..256]` at module scope. A module constant is folded
// into each use as its initialiser, and a comprehension there was never modelled: it lowered to
// an opaque (refused since G, before that the cause of E086/E011 on unrelated loops). When the
// range and the body are CONSTANT expressions of the index — the evaluator module asserts use,
// with the index bound — it IS an explicit list, and becomes one before it is typed, so the
// element-fits checks and everything downstream see exactly what a hand-written table gives
// them. Anything else is left alone and refused where it stands. Integer elements only: a
// value is a number here, and a bool or a float element would need its own literal.
#define SA_COMPREHENSION_MAX 65536
static Expr *sema_expand_const_comprehension(Expr *e, Type *decl_ty) {
    if (!e || e->kind != EXPR_ARRAY_COMPREHENSION) return NULL;
    Type *at = decl_ty; while (at && at->kind == TYPE_COMPTIME) at = at->element_type;
    if (!at || at->kind != TYPE_ARRAY || !is_integer_type(at->element_type)) return NULL;
    Expr *rg = e->as.array_comprehension_expr.range, *body = e->as.array_comprehension_expr.body;
    Id *idx = e->as.array_comprehension_expr.idx;
    if (!rg || rg->kind != EXPR_RANGE || !body || !idx) return NULL;
    bool lay = false; __int128 lo, hi;
    if (!sa_is_const(rg->as.range_expr.start, &lay) || !sa_is_const(rg->as.range_expr.end, &lay) || lay ||
        !sa_eval(rg->as.range_expr.start, &lo) || !sa_eval(rg->as.range_expr.end, &hi)) return NULL;
    if (rg->as.range_expr.inclusive) hi += 1;
    if (hi < lo || hi - lo > SA_COMPREHENSION_MAX) return NULL;
    sa_bound_idx = idx;
    bool ok = sa_is_const(body, &lay) && !lay;
    ExprList *head = NULL, **tail = &head;
    for (__int128 k = lo; ok && k < hi; k++) {
        __int128 v; sa_bound_val = k;
        if (!sa_eval(body, &v) || v < (__int128)INT64_MIN || v > (__int128)INT64_MAX) { ok = false; break; }
        Expr *lit = expr_literal(sema_arena, (int64_t)v);
        lit->line = body->line; lit->col = body->col;
        *tail = expr_list(sema_arena, lit); tail = &(*tail)->next;
    }
    sa_bound_idx = NULL;
    if (!ok) return NULL;
    Expr *arr = expr_array_literal(sema_arena, head);
    arr->line = e->line; arr->col = e->col;
    return arr;
}
static void sema_check_static_assert(Decl *d) {
    Expr *c = d->as.static_assert_decl.cond;
    bool layout = false;
    if (!c || !c->type || !is_bool_type(c->type) || !sa_is_const(c, &layout)) {
        // The reason names what IS evaluated, and what is not (I.107): it said a table element "exists
        // only at run time", which is false for a module constant table, and omitted named constants.
        fprintf(stderr, "[E133] Error Ln %li, Col %li: a module-scope `assert` takes a `bool` CONSTANT "
                "expression: literals, named integer constants, operators, `as` and `as%`, `@sizeof(T)` and "
                "`@alignof(T)`. An element of a constant table and a function call are not evaluated "
                "here. A fact about a run-time value is an `assert(...)` inside a function.\n",
                (long)d->line, (long)d->col);
        diagnostic_show_line(d->line, d->col);
        exit(1);
    }
    if (layout) return;                       // the C compiler decides it (_Static_assert)
    __int128 v;
    if (!sa_eval(c, &v)) {
        fprintf(stderr, "[E133] Error Ln %li, Col %li: this module-scope `assert` cannot be evaluated "
                "(a division by zero?).\n", (long)d->line, (long)d->col);
        diagnostic_show_line(d->line, d->col);
        exit(1);
    }
    if (!v) {
        fprintf(stderr, "[E134] Error Ln %li, Col %li: module-scope assertion is false.\n",
                (long)d->line, (long)d->col);
        diagnostic_show_line(d->line, d->col);
        exit(1);
    }
}

// Return a refinement type alias's constraint list if `t` names one, else NULL.
// (Constraints live on the DECL_TYPE_ALIAS, not on the using declaration.)
static ExprList *alias_constraints_for(Type *t) {
    if (!t || t->kind != TYPE_SIMPLE || !t->base_type) return NULL;
    if ((size_t)t->base_type->length >= 256) return NULL;
    char nm[256];
    memcpy(nm, t->base_type->name, t->base_type->length);
    nm[t->base_type->length] = '\0';
    extern Symbol *sema_lookup(const char *name);
    Symbol *sym = sema_lookup(nm);
    if (sym && sym->decl && sym->decl->kind == DECL_TYPE_ALIAS)
        return sym->decl->as.type_alias_decl.constraints;
    return NULL;
}


// P2/S3: THE scalar/pointer boundary conversion check — one call for "a value
// of static type `from` (VRA range r, source expr src_expr) flows into a slot
// of type `to`". Consolidates the boundary policy, in call order:
//   1. VRA range / overflow fit (literals, arithmetic)        [E086]
//   2. float <-> int rejection                                [E012]
//   3. integer narrowing / signedness soundness               [E086]
//   4. pointer & nominal (struct/enum) type-kind confusion    [E012]
//   5. refinement-type-alias constraints                      [E086]
// Each sub-check exits on violation. This is the boundary-level precursor to
// the full `subsumes(from, to, range)` relation (P2/S2). Callers pass the
// Zig-style sentinel discipline. A sentinel-terminated type `u8[:0]` promises its
// `.data` ends in the sentinel, which is what makes `.data` safe to hand to a
// NUL-scanning C API (`printf "%s"`, `open`, `strlen`). A value may LOSE that
// guarantee (a `u8[:0]` used as a plain `u8[]`) but may never GAIN one it lacks:
// coercing a non-terminated value — a general fixed `u8[N]`, a plain slice `u8[]`
// — into `u8[:0]` fabricates a terminator that is not in the backing storage, a
// buffer OVERREAD (potentially leaking memory) the moment `.data` reaches such an
// API. Reject it. Exempt: a source already carrying the SAME sentinel (string
// literals and string-literal-derived values ARE sentinel-terminated — Zig's
// `[N:0]u8`), a string/char LITERAL (terminated into a fresh buffer by the
// emitter), and `unsafe`.
static void reject_sentinel_fabrication(Type *from, Type *to, Expr *src_expr,
                                        isize line, isize col, const char *ctx) {
    if (sema_in_unsafe_block) return;
    if (!from || !to) return;
    Type *f = resolve_type_alias(from);
    Type *t = resolve_type_alias(to);
    if (!f || !t) return;
    bool to_sentinel = (t->kind == TYPE_ARRAY || t->kind == TYPE_SLICE) && t->has_sentinel;
    if (!to_sentinel) return;
    // Source already carries the SAME sentinel → safe (sentinel → sentinel). String
    // literals and string-literal-derived values are sentinel-terminated, so they
    // pass here; a plain `u8[]` slice or a general fixed `u8[N]` does not.
    if ((f->kind == TYPE_ARRAY || f->kind == TYPE_SLICE) && f->has_sentinel)
        return;
    // A string/char literal is terminated into a fresh buffer by the emitter.
    if (src_expr && src_expr->kind == EXPR_STRING) return;
    char tb[128]; type_describe(t, tb, sizeof tb);
    fprintf(stderr, "[E089] Error Ln %li, Col %li: cannot use a non-terminated value as the "
            "sentinel-terminated type '%s'%s%s: its trailing sentinel is not guaranteed present, "
            "so a C API that scans for one (printf \"%%s\", open, strlen, …) would read past the "
            "buffer. Pass a string literal or a value already of a matching sentinel-terminated "
            "type.\n",
            (long)line, (long)col, tb, ctx ? " for " : "", ctx ? ctx : "");
    diagnostic_show_line(line, col);
    exit(1);
}

// A string literal is a FIXED-length sentinel type `[N:0]u8` (Zig): its length N
// lives in the TYPE (array_len), and bounds proofs trust it (`s[2]`, `i < s.len`).
// So an inferred `var s = "abcd"` is length-4 and may only be reassigned another
// length-4 string — assigning a different length would make the type's array_len
// diverge from the runtime value, an OOB (`var s="abcd"; s="xy"; s[3]`). Reject the
// mismatch (matching the old fixed-array behaviour). Coercion INTO a DYNAMIC slice
// (`u8[:0]`, array_len 0 — declared explicitly for a variable-length string) is
// unaffected: the target keeps no compile-time length, so bounds use the runtime
// `.len` instead.
static void reject_fixed_string_length_mismatch(Type *from, Type *to,
                                                isize line, isize col) {
    if (!from || !to) return;
    Type *f = resolve_type_alias(from);
    Type *t = resolve_type_alias(to);
    if (!f || !t) return;
    if (t->kind == TYPE_SLICE && t->array_len > 0 &&
        f->kind == TYPE_SLICE && f->array_len > 0 &&
        f->array_len != t->array_len) {
        fprintf(stderr, "[E090] Error Ln %li, Col %li: cannot assign a length-%ld string to a "
                "length-%ld string. An inferred string binding is fixed-length; declare it "
                "`u8[:0]` for a variable-length string.\n",
                (long)line, (long)col, (long)f->array_len, (long)t->array_len);
        diagnostic_show_line(line, col);
        exit(1);
    }
}

// source expr where available (enables the null-literal idiom, 0 -> *T); NULL
// is fine (that sub-check simply won't fire).
// ★ `*T` IS READ-ONLY, `*var T` IS WRITABLE (4b08e44), and since the C is emitted from the IR
// nothing enforced it: the old emitter wrote `const T*` and gcc refused a write, the IR one writes
// `T*`. So `*p = 1` through a `*u8` compiled and wrote, and a `*u8` passed to a `*var u8`
// parameter was accepted — every read-only pointer could be laundered into a writable one with
// no `unsafe` in sight. The distinction is checked here, in the language, as it should have been.
static bool sema_is_readonly_ptr(Type *t) {
    while (t && t->kind == TYPE_COMPTIME) t = t->element_type;
    return t && t->kind == TYPE_POINTER && !t->pointee_mutable;
}
// A write to `target` that reaches its place THROUGH a read-only pointer: `*p`, `p.f`, `p[i]`
// (a pointer to an array), at any depth (`(*p).a.b`, `p.arr[i]`). E009 inside `unsafe` too:
// `unsafe` permits a raw dereference, not a write the pointer's type forbids.
// May the program write the place `e` names? A `var` binding or a `var` parameter; an element
// or a field of a writable place (or of a `var` slice parameter); what a `*var T` points to.
static bool sema_place_writable(Expr *e) {
    if (!e) return false;
    switch (e->kind) {
        case EXPR_IDENTIFIER: {
            if (e->local_writable) return e->local_writable == 1;   // resolve's answer (resolve.h)
            Id *id = e->as.identifier_expr.id;
            if (!id || id->length >= 256) return false;
            char buf[256]; memcpy(buf, id->name, (size_t)id->length); buf[id->length] = 0;
            extern Symbol *sema_lookup(const char *name);
            Symbol *sym = sema_lookup(buf);
            if (!sym) return false;
            if (sym->is_mutable) return true;
            return sym->decl && sym->decl->kind == DECL_VARIABLE &&
                   sym->decl->as.variable_decl.is_parameter && sym->decl->as.variable_decl.type &&
                   sym->decl->as.variable_decl.type->mode == MODE_MUTABLE;
        }
        case EXPR_INDEX: {
            Expr *b = e->as.index_expr.target;
            Type *bt = b ? b->type : NULL; while (bt && bt->kind == TYPE_COMPTIME) bt = bt->element_type;
            if (bt && bt->kind == TYPE_POINTER) return bt->pointee_mutable;
            return sema_place_writable(b) || (bt && bt->mode == MODE_MUTABLE);
        }
        case EXPR_MEMBER: {
            Expr *b = e->as.member_expr.target;
            Type *bt = b ? b->type : NULL; while (bt && bt->kind == TYPE_COMPTIME) bt = bt->element_type;
            if (bt && bt->kind == TYPE_POINTER) return bt->pointee_mutable;
            return sema_place_writable(b);
        }
        case EXPR_DEREF: {
            Type *pt = e->as.deref_expr.expr ? e->as.deref_expr.expr->type : NULL;
            while (pt && pt->kind == TYPE_COMPTIME) pt = pt->element_type;
            return pt && pt->kind == TYPE_POINTER && pt->pointee_mutable;
        }
        default: return false;
    }
}
static void sema_check_write_through_readonly(Expr *target, isize line, isize col) {
    for (Expr *e = target; e; ) {
        Expr *base = NULL;
        if (e->kind == EXPR_DEREF)       base = e->as.deref_expr.expr;
        else if (e->kind == EXPR_MEMBER) base = e->as.member_expr.target;
        else if (e->kind == EXPR_INDEX)  base = e->as.index_expr.target;
        else break;
        if (base && sema_is_readonly_ptr(base->type)) {
            Id *bid = base->kind == EXPR_IDENTIFIER ? base->as.identifier_expr.id : NULL;
            fprintf(stderr, "[E009] Error Ln %li, Col %li: cannot write through the read-only pointer%s%.*s%s: "
                    "a `*T` may be read, not written, inside `unsafe` too. Declare it `*var T` to "
                    "write through it.\n", (long)line, (long)col,
                    bid ? " `" : "", bid ? (int)bid->length : 0, bid ? bid->name : "", bid ? "`" : "");
            diagnostic_show_line(line, col);
            exit(1);
        }
        e = base;
    }
}
// ★ AN IMMUTABLE BINDING'S ELEMENTS AND FIELDS ARE IMMUTABLE TOO, and so is a module constant.
// Spec 07 ("writing to an immutable binding's field is ill-formed") and 09 ("the left-hand side
// shall be a mutable lvalue") said so, and only a write to the NAME was refused: `a[0] = 9` and
// `s.v = 9` on immutable bindings compiled, `f(var a)` lent an immutable array to be written,
// `var r = var x` made a writable view of an immutable `x`, and every one of them wrote a module
// constant table too, which is read-only static data. The place must be one the program may write
// (sema_place_writable), whatever the route, and inside `unsafe` as well.
// The immutable BINDING whose storage `place` is, or NULL: an immutable local or a module
// constant (`*global` says which), never a parameter of array or slice type (an output reference)
// and never anything reached through a pointer.
static Expr *sema_readonly_root(Expr *place, bool *global, Id **shown) {
    *global = false; *shown = NULL;
    if (!place || sema_place_writable(place)) return NULL;
    Expr *root = place;
    while (root && (root->kind == EXPR_INDEX || root->kind == EXPR_MEMBER)) {
        Expr *b = root->kind == EXPR_INDEX ? root->as.index_expr.target : root->as.member_expr.target;
        Type *bt = b ? b->type : NULL; while (bt && bt->kind == TYPE_COMPTIME) bt = bt->element_type;
        if (bt && bt->kind == TYPE_POINTER) return NULL;
        root = b;
    }
    if (!root || root->kind != EXPR_IDENTIFIER || !root->as.identifier_expr.id) return NULL;
    Decl *rd = root->decl;
    *shown = root->as.identifier_expr.id;
    if (rd && rd->kind == DECL_VARIABLE && rd->as.variable_decl.is_parameter) {
        Type *pt = rd->as.variable_decl.type;
        while (pt && pt->kind == TYPE_COMPTIME) pt = pt->element_type;
        if (pt && (pt->kind == TYPE_ARRAY || pt->kind == TYPE_SLICE)) return NULL;
    }
    if (rd && rd->kind == DECL_VARIABLE && !rd->as.variable_decl.is_parameter) {
        extern DeclList *sema_decls;
        for (DeclList *dl = sema_decls; dl && !*global; dl = dl->next) if (dl->decl == rd) *global = true;
        if (rd->as.variable_decl.name) *shown = rd->as.variable_decl.name;
    }
    return root;
}
static void sema_check_place_writable(Expr *place, isize line, isize col, const char *how) {
    if (!place || sema_place_writable(place)) return;
    Expr *root = place;
    while (root && (root->kind == EXPR_INDEX || root->kind == EXPR_MEMBER)) {
        Expr *b = root->kind == EXPR_INDEX ? root->as.index_expr.target : root->as.member_expr.target;
        Type *bt = b ? b->type : NULL; while (bt && bt->kind == TYPE_COMPTIME) bt = bt->element_type;
        if (bt && bt->kind == TYPE_POINTER) return;     // through a pointer: the read-only check's case
        root = b;
    }
    if (!root || root->kind != EXPR_IDENTIFIER || !root->as.identifier_expr.id) return;
    Id *id = root->as.identifier_expr.id;
    Decl *rd = root->decl;
    // An ARRAY or SLICE parameter is an output reference: `func set0(dst i32[3], v i32)` writes
    // `dst[0]` into the caller's array (tests/memory/fixed_array_outparam_pass.ln). That is the
    // language's convention for a written array parameter, not an immutable binding's elements.
    if (rd && rd->kind == DECL_VARIABLE && rd->as.variable_decl.is_parameter) {
        Type *pt = rd->as.variable_decl.type;
        while (pt && pt->kind == TYPE_COMPTIME) pt = pt->element_type;
        if (pt && (pt->kind == TYPE_ARRAY || pt->kind == TYPE_SLICE)) return;
    }
    bool global = false;
    if (rd && rd->kind == DECL_VARIABLE && !rd->as.variable_decl.is_parameter) {
        extern DeclList *sema_decls;
        for (DeclList *dl = sema_decls; dl && !global; dl = dl->next) if (dl->decl == rd) global = true;
        if (rd->as.variable_decl.name) id = rd->as.variable_decl.name;   // the name as written
    }
    char buf[256]; int l = id->length < 255 ? (int)id->length : 255; memcpy(buf, id->name, (size_t)l); buf[l] = 0;
    if (global)
        fprintf(stderr, "[E009] Error Ln %li, Col %li: cannot %s the module constant '%s': a module "
                "constant is read-only. Copy it into a `var` to change the copy.\n", (long)line, (long)col, how, buf);
    else
        fprintf(stderr, "[E009] Error Ln %li, Col %li: cannot %s '%s', an immutable binding: its "
                "elements and fields are immutable too. Declare it `var`.\n", (long)line, (long)col, how, buf);
    diagnostic_show_line(line, col);
    exit(1);
}
static void fnptr_assign_check(Type *target, Expr *rhs, isize line, isize col);
// ── I.141: A WRAPPING OPERATION ON LITERALS WRAPS AT THE TYPE IT IS GIVEN ──────────────────────
// `x u8 = 250 +% 10` was E086 "arithmetic is not provably free of overflow", a contradictory
// message for a wrapping operator: the two literals are i32, so the operation was an i32 260, and
// the narrowing to u8 failed. A module constant `K u8 = 250 +% 10` was accepted at its declaration
// and E086 at every use, the use's materialization typing it the same way. With every leaf a
// literal there is no operand type to wrap at, and the type the program means is the destination's:
// the boundary retypes such a tree (the literals and every wrapping node) to it, so it wraps there
// and evaluates to 4 in the declaration and at every use alike. Only when every literal is itself a
// value of the destination type: `300 +% 10` in a u8 keeps its refusal, 300 being no u8.
static bool sema_is_wrap_op(TokenKind op) {
    return op == TOKEN_PLUS_PERCENT || op == TOKEN_MINUS_PERCENT || op == TOKEN_ASTERISK_PERCENT ||
           op == TOKEN_SLASH_PERCENT || op == TOKEN_SHIFT_LEFT_PERCENT;
}
static bool sema_wrap_literal_leaf(Expr *e, int bits, bool sgn, int depth) {
    if (!e || depth > 32) return false;
    if (e->kind == EXPR_LITERAL && !e->as.literal_expr.is_bool) {
        int64_t v = (int64_t)e->as.literal_expr.value;
        __int128 lo = sgn ? -((__int128)1 << (bits - 1)) : 0;
        __int128 hi = sgn ? ((__int128)1 << (bits - 1)) - 1 : (((__int128)1) << bits) - 1;
        return (__int128)v >= lo && (__int128)v <= hi;
    }
    return e->kind == EXPR_BINARY && sema_is_wrap_op(e->as.binary_expr.op) &&
           sema_wrap_literal_leaf(e->as.binary_expr.left, bits, sgn, depth + 1) &&
           (e->as.binary_expr.op == TOKEN_SHIFT_LEFT_PERCENT ||   // a shift's amount only counts
            sema_wrap_literal_leaf(e->as.binary_expr.right, bits, sgn, depth + 1));
}
static void sema_retype_wrap_tree(Expr *e, Type *t) {
    if (!e) return;
    if (e->kind == EXPR_LITERAL) {
        Type *lt = arena_push_aligned(sema_arena, Type); *lt = *t;
        lt->refine.known = true; lt->refine.lo = lt->refine.hi = (int64_t)e->as.literal_expr.value;
        e->type = lt;
        return;
    }
    Type *bt = arena_push_aligned(sema_arena, Type); *bt = *t; bt->refine.known = false;
    sema_retype_wrap_tree(e->as.binary_expr.left, t);
    if (e->as.binary_expr.op != TOKEN_SHIFT_LEFT_PERCENT) sema_retype_wrap_tree(e->as.binary_expr.right, t);
    e->type = bt;
}

static void check_conversion(Type *from, Type *to, Range r, Expr *src_expr,
                             isize line, isize col,
                             const char *ctx, const char *label) {
    // ★ A FUNCTION BECOMES A FUNCTION-POINTER VALUE AT EVERY BOUNDARY, not only at a variable's
    // initialiser. fnptr_assign_check (row containment: the function may do no more than the arrow
    // admits) had ONE caller, the `var` initialiser, so an `effects io` function passed as an
    // argument, returned, stored in a struct field or a sum payload, or assigned later into a pure
    // `*func` slot was accepted: a `func` declared pure performed io through it, and a function
    // with `effects diverge` handed to a total function's callback made that function hang. Every
    // boundary already comes through here.
    { Type *tt = to; while (tt && tt->kind == TYPE_COMPTIME) tt = tt->element_type;
      if (tt && tt->kind == TYPE_FUNC && src_expr) fnptr_assign_check(tt, src_expr, line, col); }
    // I.141: a wrapping operation whose every leaf is a literal takes the destination's type
    if (src_expr && src_expr->kind == EXPR_BINARY && sema_is_wrap_op(src_expr->as.binary_expr.op) && to) {
        Type *tt = to; while (tt && tt->kind == TYPE_COMPTIME) tt = tt->element_type;
        Type *ta = tt ? resolve_type_alias(tt) : NULL;
        int bits = 0; bool sgn = false;
        if (ta && is_integer_type(ta) && parse_iN_uN(ta, &bits, &sgn) && bits >= 1 && bits <= 64 &&
            sema_wrap_literal_leaf(src_expr, bits, sgn, 0)) {
            sema_retype_wrap_tree(src_expr, ta);
            from = src_expr->type;
            r = range_unknown();
        }
    }
    if (sema_is_readonly_ptr(from)) {
        Type *tt = to; while (tt && tt->kind == TYPE_COMPTIME) tt = tt->element_type;
        if (tt && tt->kind == TYPE_POINTER && tt->pointee_mutable) {
            fprintf(stderr, "[E012] Error Ln %li, Col %li: a read-only pointer `*T` cannot become a "
                    "writable `*var T` in %s '%s'. Take a `*var T` where the pointer is made.\n",
                    (long)line, (long)col, ctx ? ctx : "this conversion", label ? label : "");
            diagnostic_show_line(line, col);
            exit(1);
        }
    }
    // ★ AN ARRAY LITERAL TAKES THE DESTINATION'S ELEMENT TYPE. `[1, 2, 3, 4]` types as i32[4]
    // (its elements are literals), and outside a declaration nothing retyped it: lowering then
    // materialised an int32_t[4], and a `u8[4]` parameter, a `u8[]` slice or a struct's `u8[4]`
    // field received its BYTES — `third([1, 2, 3, 4])` read 0 where it meant 3, accepted and
    // silently wrong. Here the destination is known, so each element is checked against the
    // element type (a value that does not fit is refused, as in a declaration) and the literal
    // is retyped to it, which is what lowering reads.
    if (src_expr && src_expr->kind == EXPR_ARRAY_LITERAL && to) {
        Type *tt = to;
        while (tt && tt->kind == TYPE_COMPTIME) tt = tt->element_type;
        tt = tt ? resolve_type_alias(tt) : NULL;
        if (tt && (tt->kind == TYPE_ARRAY || tt->kind == TYPE_SLICE) && tt->element_type) {
            int n = 0;
            for (ExprList *el = src_expr->as.array_literal_expr.elements; el; el = el->next, n++) {
                Expr *x = el->expr;
                if (!x || !x->type) continue;
                Range er = (x->kind == EXPR_LITERAL)
                    ? (Range){ x->as.literal_expr.value, x->as.literal_expr.value, true }
                    : (sema_ranges ? sema_eval_range(x, sema_ranges) : range_unknown());
                check_conversion(x->type, tt->element_type, er, x, x->line, x->col,
                                 "array element", label);
            }
            // A FIXED destination needs exactly its length, as a declaration does: a shorter
            // literal handed to a `u8[4]` parameter is a 3-element temporary the callee may read
            // at index 3.
            if (tt->kind == TYPE_ARRAY && tt->array_len >= 0 && n != tt->array_len) {
                fprintf(stderr, "[E012] Error Ln %li, Col %li: array literal has %d element(s) but "
                        "the %s%s%s has fixed length %lld.\n", (long)line, (long)col, n,
                        ctx ? ctx : "destination", label && *label ? " " : "", label ? label : "",
                        (long long)tt->array_len);
                diagnostic_show_line(line, col);
                exit(1);
            }
            if (src_expr->type && src_expr->type->kind == TYPE_ARRAY) {
                Type *nt = arena_push_aligned(sema_arena, Type);
                *nt = *src_expr->type;
                nt->element_type = tt->element_type;
                nt->array_len = n;
                src_expr->type = nt;
            }
            return;          // the element checks ARE this conversion
        }
    }
    // `T | markers` coercion: a union proven present (narrowed by `if r`) is used
    // as its payload T; using it unnarrowed where a non-union is expected is
    // rejected (it may be a marker). Construction (value/marker -> union) is done
    // earlier by sema_union_coerce at each boundary.
    {
        Type *tu = to;   while (tu && tu->kind == TYPE_COMPTIME) tu = tu->element_type;
        Type *fu = from; while (fu && fu->kind == TYPE_COMPTIME) fu = fu->element_type;
        Type *fpay = union_payload_type(fu);
        if (fpay && !union_payload_type(tu)) {
            // A bare marker IS a marker (I.103): there is no payload to test for or to read, under
            // `unsafe` or not, so name it rather than ask for a test.
            Variant *mv = (src_expr && src_expr->kind == EXPR_IDENTIFIER) ? src_expr->as.identifier_expr.variant : NULL;
            if (mv) {
                char fb[128], tb[128]; type_describe(fu, fb, sizeof fb); type_describe(to, tb, sizeof tb);
                fprintf(stderr, "[E063] Error Ln %li, Col %li: %s '%s': '%.*s' is a marker of '%s', not "
                        "a value of '%s'.\n", (long)line, (long)col, ctx ? ctx : "this", label ? label : "",
                        (int)mv->name->length, mv->name->name, fb, tb);
                diagnostic_show_line(line, col); exit(1);
            }
            if (sema_in_unsafe_block || sema_is_narrowed(src_expr)) {
                check_conversion(fpay, to, r, src_expr, line, col, ctx, label);
                return;
            }
            char tb[128]; type_describe(to, tb, sizeof tb);
            fprintf(stderr, "[E063] Error Ln %li, Col %li: %s value may be a marker; test it "
                    "(`if r { … }`) or match it (`case r { … }`) before using it as '%s'.\n",
                    (long)line, (long)col, ctx ? ctx : "this", tb);
            diagnostic_show_line(line, col); exit(1);
        }
    }
    // Keystone flip: EVERY integer numeric boundary — same-core refinement narrowing
    // AND cross-core narrowing/widening — is decided by ONE relation. When the value
    // provably fits the target (source interval ⊆ target range), the scattered checks
    // (fits / lossy / float-int / incompatible) would all accept, so route through
    // `value_fits` and only discharge the disequality residual (`!= k`, not an
    // interval). Behavior-preserving: a false verdict (non-integer, unknown/unfitting
    // range) falls through to the full checks below.
    if (value_fits(from, r, to)) return;

    reject_float_int_mismatch(from, to, line, col, ctx, label);
    // ★ f64 TO f32 IS WRITTEN (I.161, Marco's decision): it ROUNDS, and every other narrowing in
    // Lain is explicit. `x f32 = y` with y an f64 compiled. A float literal still takes an f32
    // destination's type (`x f32 = 1.5`), negated or not; anything computed needs `as f32`.
    {
        Type *fu = from, *tu = to;
        while (fu && fu->kind == TYPE_COMPTIME) fu = fu->element_type;
        while (tu && tu->kind == TYPE_COMPTIME) tu = tu->element_type;
        fu = resolve_type_alias(fu); tu = resolve_type_alias(tu);
        Expr *lit = src_expr;
        while (lit && lit->kind == EXPR_UNARY && lit->as.unary_expr.op == TOKEN_MINUS) lit = lit->as.unary_expr.right;
        if (fu && tu && is_float_type(fu) && is_float_type(tu) && memcmp(fu->base_type->name, "f64", 3) == 0 &&
            memcmp(tu->base_type->name, "f32", 3) == 0 && !(lit && lit->kind == EXPR_FLOAT_LITERAL)) {
            fprintf(stderr, "[E012] Error Ln %li, Col %li: implicit conversion from 'f64' to 'f32' in %s '%s' "
                    "rounds the value: write `as f32`.\n", (long)line, (long)col,
                    ctx ? ctx : "this conversion", label ? label : "");
            diagnostic_show_line(line, col);
            exit(1);
        }
    }
    reject_incompatible_conversion(from, to, src_expr, line, col, ctx, label);
    reject_sentinel_fabrication(from, to, src_expr, line, col, ctx);
    reject_fixed_string_length_mismatch(from, to, line, col);
    // (A refinement ALIAS's own constraint is the IR's obligation: the alias lowers to a refined
    // type — an interval, plus an excluded value for `!=` — enforced where a value narrows into it.)

    // Dual-run (measurement only, behind LAIN_KEYSTONE_DUALRUN — zero behavior
    // change). Reaching here means the scattered checks ACCEPTED an integer→integer
    // conversion that `value_fits` could NOT prove — a fall-through where the value's
    // range is unknown (a permissive accept) rather than proven. The count is the
    // residual reliance on the name table's permissiveness (the type-based-VRA gap).
    if (getenv("LAIN_KEYSTONE_DUALRUN")) {
        long long a, b, c, d;
        if (type_integer_range(from, &a, &b) && type_integer_range(to, &c, &d) &&
            !value_fits(from, r, to)) {
            char fb[96], tb[96];
            type_describe(from, fb, sizeof fb); type_describe(to, tb, sizeof tb);
            // Audit 2026-08-27: the whole residual is the arith-widened narrowing
            // path (i33/i34/i64 result of +/* narrowed to the declared type) with
            // an UNKNOWN result range — the deliberate overflow-ergonomics accept
            // (`x = x + 1` must compile). Sound (no UB/mem-unsafety; the KNOWN-range
            // overflow still rejects). Report range accurately so it is not mistaken
            // for a proven-small accept.
            bool eff_unbounded = !r.known ||
                r.min <= LLONG_MIN + 4096 || r.max >= LLONG_MAX - 4096;
            if (!eff_unbounded)
                fprintf(stderr, "[keystone-fallthrough] %s -> %s (r=[%lld,%lld] BOUNDED) — "
                        "a genuinely-bounded value the permissive path accepted but "
                        "value_fits could not prove — INVESTIGATE\n",
                        fb, tb, (long long)r.min, (long long)r.max);
            else
                fprintf(stderr, "[keystone-fallthrough] %s -> %s (range effectively unbounded) — "
                        "arith-widened narrowing, permissive accept (overflow-ergonomics)\n",
                        fb, tb);
        }
    }
}

// P2/S3: reject a type-confused comparison (==, !=, <, <=, >, >=). Comparing a
// pointer to a non-pointer (except the null idiom `p == 0`), two pointers with
// different pointee types, or a float to an integer are confusions that emit
// broken/mis-evaluated C. Integer signedness is intentionally NOT policed here
// — i32-vs-usize comparisons (`i < xs.len`) are idiomatic; struct/enum '==' is
// handled separately. `unsafe` does not bypass it: it licenses memory operations, not
// comparing an f64 with an i32 (accepted inside `unsafe` until 2026-10-01).
static void check_comparison_operands(Type *lt, Type *rt, Expr *le, Expr *re,
                                      const char *op, isize line, isize col) {
    if (!lt || !rt) return;
    Type *l = lt, *r = rt;
    while (l && l->kind == TYPE_COMPTIME) l = l->element_type;
    while (r && r->kind == TYPE_COMPTIME) r = r->element_type;
    if (!l || !r) return;
    l = resolve_type_alias(l);
    r = resolve_type_alias(r);
    if (l == r) return;
    bool l_ptr = (l->kind == TYPE_POINTER), r_ptr = (r->kind == TYPE_POINTER);
    bool bad = false;
    if ((is_float_type(l) && is_integer_type(r)) || (is_integer_type(l) && is_float_type(r))) {
        bad = true;                                          // float vs int
    } else if ((is_bool_type(l) && is_integer_type(r)) || (is_integer_type(l) && is_bool_type(r))) {
        bad = true;                                          // bool vs int: `flag == 1`
    } else if (l_ptr || r_ptr) {
        if (l_ptr && r_ptr) {
            if (types_equal_exact(l->element_type, r->element_type)) return;  // same pointee
        } else if (is_zero_int_literal(l_ptr ? re : le)) {
            return;                                          // null idiom: p == 0
        }
        bad = true;
    }
    if (!bad) return;
    char lb[128], rb[128];
    type_describe(l, lb, sizeof lb);
    type_describe(r, rb, sizeof rb);
    fprintf(stderr,
        "[E012] Error Ln %li, Col %li: incompatible operand types for '%s': '%s' and '%s'.\n",
        (long)line, (long)col, op, lb, rb);
    diagnostic_show_line(line, col);
    exit(1);
}

/*─────────────────────────────────────────────────────────────────╗
│ 2) Keep the top-level DeclList for struct lookups              │
╚─────────────────────────────────────────────────────────────────*/

/* lookup a struct Decl node by its name Id */
static DeclStruct *find_struct_decl(Id *struct_name) {
  if (!struct_name)
    return NULL;
  for (DeclList *dl = sema_decls; dl; dl = dl->next) {
    Decl *d = dl->decl;
    if (d && d->kind == DECL_STRUCT &&
        d->as.struct_decl.name->length == struct_name->length &&
        strncmp(d->as.struct_decl.name->name, struct_name->name,
                struct_name->length) == 0) {
      return &d->as.struct_decl;
    }
  }
  return NULL;
}

// The refinement constraints of struct field `field` on a value of `struct_type`
// (forward-declared in ranges.h so sema_eval_range can seed a refined field read).
static ExprList *sema_member_field_constraints(Type *struct_type, Id *field) {
    if (!struct_type || struct_type->kind != TYPE_SIMPLE || !struct_type->base_type || !field)
        return NULL;
    DeclStruct *sd = find_struct_decl(struct_type->base_type);
    if (!sd) return NULL;
    for (DeclList *f = sd->fields; f; f = f->next) {
        if (!f->decl || f->decl->kind != DECL_VARIABLE) continue;
        Id *fn = f->decl->as.variable_decl.name;
        if (fn && fn->length == field->length &&
            strncmp(fn->name, field->name, fn->length) == 0)
            return f->decl->as.variable_decl.constraints;
    }
    return NULL;
}

/* lookup a field’s Type* given a struct and field Id */
static Type *lookup_struct_field_type(Id *struct_name, Id *field) {
  if (!struct_name) {
    fprintf(stderr, "internal error: lookup_struct_field_type called "
                    "with NULL struct_name\n");
    exit(70);                                   // an internal error, not a refused program
  }

  DeclStruct *sd = find_struct_decl(struct_name);
  if (!sd) {   // the only caller looked the struct up first
    fprintf(stderr, "internal error: lookup_struct_field_type: '%.*s' is not a struct\n",
            (int)struct_name->length, struct_name->name);
    exit(70);
  }
  for (DeclList *fld = sd->fields; fld; fld = fld->next) {
    Decl *vd = fld->decl;
    if (vd->kind == DECL_VARIABLE) {
      Id *fname = vd->as.variable_decl.name;
      if (fname->length == field->length &&
          strncmp(fname->name, field->name, fname->length) == 0) {
        return vd->as.variable_decl.type;
      }
    }
  }
  return NULL;
}

/*
    type inference/checking logic
*/

/* Unwrap wrapper types to get the underlying type (struct/array/slice) */
static Type *resolve_type_alias(Type *t);
static Type *sema_unwrap_type(Type *t) {
    while (t) {
        // With the new OwnershipMode system, we only unwrap pointer/comptime
        // The mode is just a field on the type, not a wrapper
        if (t->kind == TYPE_POINTER) t = t->element_type;
        else if (t->kind == TYPE_COMPTIME) t = t->element_type;
        // An alias of an ARRAY type (`type Buf4 = u8[K]`) is the array: indexing, `.len` and a
        // literal's length all read it through here. A scalar alias stays itself, since its
        // refinement is enforced on the name.
        else if (t->kind == TYPE_SIMPLE) { Type *a = resolve_type_alias(t); if (a && a != t && a->kind == TYPE_ARRAY) t = a; else break; }
        else break;
    }
    return t;
}



/* lookup an ADT Decl node by its name Id */
static DeclEnum *find_adt_decl(Id *adt_name) {
  if (!adt_name) return NULL;
  for (DeclList *dl = sema_decls; dl; dl = dl->next) {
    Decl *d = dl->decl;
    if (d) {
        if (d->kind == DECL_ENUM &&
            d->as.enum_decl.type_name->length == adt_name->length &&
            strncmp(d->as.enum_decl.type_name->name, adt_name->name, adt_name->length) == 0) {
          return &d->as.enum_decl;
        }
    }
  }
  return NULL;
}

/* lookup a variant in an ADT */
static Variant *lookup_adt_variant(DeclEnum *adt, Id *variant_name) {
    for (Variant *v = adt->variants; v; v = v->next) {
        if (v->name->length == variant_name->length &&
            strncmp(v->name->name, variant_name->name, variant_name->length) == 0) {
            return v;
        }
    }
    return NULL;
}

/*──────────────────────────────────────────────────────────────────╗
│ Function-pointer typing (non-capturing `*func`/`*proc`)            │
╚──────────────────────────────────────────────────────────────────*/

// Synthesize the function-pointer type of a function decl.
static Type *fnptr_type_of_decl(Decl *d) {
    if (!d) return NULL;
    if (d->kind != DECL_FUNCTION && d->kind != DECL_EXTERN_FUNCTION) return NULL;
    TypeList *pts = NULL, *tail = NULL;
    for (DeclList *p = d->as.function_decl.params; p; p = p->next) {
        Type *pt = (p->decl && p->decl->kind == DECL_VARIABLE)
                   ? p->decl->as.variable_decl.type : NULL;
        TypeList *node = type_list(sema_arena, pt);
        if (!pts) pts = node; else tail->next = node;
        tail = node;
    }
    // The source function's own row is what the assignment is checked against. A declared row
    // is believed on an extern and checked on a definition, so reading it here is reading the
    // same fact either way; where none was written the row is ∅, which is what silence means.
    EffectSet srow = d->as.function_decl.effects_declared
                   ? d->as.function_decl.effects_bound : 0;
    return type_func(sema_arena, pts, d->as.function_decl.return_type, srow);
}

// Is a function-pointer value of type `from` assignable to target `to`?
// Equal arity, structurally-equal parameter/return types, and ROW CONTAINMENT: the source may
// do no more than the arrow admits. This subsumes the old two-point `func <: proc` subtyping
// (∅ ⊆ ⊤ holds, ⊤ ⊆ ∅ does not) and extends it to every intermediate row, so an arrow can now
// say "may print, but terminates" — a bound the boolean could not express.
// A written `void` is no return type, in an arrow as on a declaration (I.151): `*func(i32) void`
// did not match a function declared with nothing written after its parameters.
static Type *fnptr_ret(Type *t) {
    Type *r = t ? t->element_type : NULL;
    if (r && r->kind == TYPE_SIMPLE && r->base_type && r->base_type->length == 4 &&
        memcmp(r->base_type->name, "void", 4) == 0) return NULL;
    return r;
}
static bool fnptr_types_assignable(Type *to, Type *from) {
    if (!to || !from || to->kind != TYPE_FUNC || from->kind != TYPE_FUNC) return false;
    if (from->func_effects & ~to->func_effects) return false;
    Type *tr = fnptr_ret(to), *fr = fnptr_ret(from);
    if ((tr == NULL) != (fr == NULL)) return false;
    if (tr && !types_equal_exact(tr, fr)) return false;
    TypeList *a = to->func_params, *b = from->func_params;
    while (a && b) {
        if (!a->type || !b->type || !types_equal_exact(a->type, b->type)) return false;
        a = a->next; b = b->next;
    }
    return a == NULL && b == NULL;   // same arity
}

// Boundary check for initialising/assigning a function-pointer target.
static void fnptr_assign_check(Type *target, Expr *rhs, isize line, isize col) {
    if (!target || target->kind != TYPE_FUNC || !rhs) return;
    Type *from = (rhs->type && rhs->type->kind == TYPE_FUNC)
                 ? rhs->type                       // another fn-ptr value
                 : fnptr_type_of_decl(rhs->decl);  // a bare function name
    if (!from) {
        fprintf(stderr, "[E122] Error Ln %li, Col %li: a function-pointer must be initialised with "
                "a matching function or function-pointer.\n", (long)line, (long)col);
        diagnostic_show_line(line, col); exit(1);
    }
    if (!fnptr_types_assignable(target, from)) {
        fprintf(stderr, "[E122] Error Ln %li, Col %li: function does not match the function-pointer "
                "type — arity, parameter/return types, or the effect row differ (the function may "
                "do more than the arrow admits).\n",
                (long)line, (long)col);
        diagnostic_show_line(line, col); exit(1);
    }
}

// ── Call-site aliasing check (soundness of emitted `restrict`) ───────────────
// A reference parameter (array/slice/pointer) is lowered to C with `restrict`,
// which promises it does not alias. That promise is UNSOUND unless enforced at the
// call site: if the same array is passed to two parameters and at least one MUTATES
// it, a store through one restrict pointer aliases the other → undefined behaviour
// (and, under -O3, a real miscompile). This reinstates the exclusive-borrow the
// codegen assumes: reject such calls. (Two read-only references may alias — no
// write, no hazard — e.g. `dot(a, a)`.)
static bool sema_expr_rooted_at(Expr *e, Id *pn) {
    while (e) {
        if (e->kind == EXPR_IDENTIFIER)
            return e->as.identifier_expr.id && e->as.identifier_expr.id->length == pn->length &&
                   memcmp(e->as.identifier_expr.id->name, pn->name, (size_t)pn->length) == 0;
        else if (e->kind == EXPR_INDEX)  e = e->as.index_expr.target;
        else if (e->kind == EXPR_MEMBER) e = e->as.member_expr.target;
        else if (e->kind == EXPR_DEREF)  e = e->as.deref_expr.expr;
        else return false;
    }
    return false;
}

static bool sema_body_writes_id(StmtList *body, Id *pn) {
    for (StmtList *b = body; b; b = b->next) {
        Stmt *s = b->stmt; if (!s) continue;
        switch (s->kind) {
            case STMT_ASSIGN: if (sema_expr_rooted_at(s->as.assign_stmt.target, pn)) return true; break;
            case STMT_IF:     if (sema_body_writes_id(s->as.if_stmt.then_body, pn) ||
                                  sema_body_writes_id(s->as.if_stmt.else_branch, pn)) return true; break;
            case STMT_WHILE:  if (sema_body_writes_id(s->as.while_stmt.body, pn)) return true; break;
            case STMT_FOR:    if (sema_body_writes_id(s->as.for_stmt.body, pn)) return true; break;
            default: break;
        }
    }
    return false;
}

// ★ `check_call_aliasing` IS DELETED WHOLE. Every path through it now ends in `continue` — the
// sovereign borrow pass owns co-argument aliasing, correctly as of 2026-09-26. What is worth keeping
// from it is written down where the replacement lives: the SPAN test it did with
// `sema_arg_const_span` (disjoint iff `ihi <= jlo || jhi <= ilo`) is the reasoning the IR version had
// lost, and recovering it is what fixed the overlapping-sub-slice miscompile. A check that moves does
// not inherit the reasoning that was never written down as its own rule.


// Fail-closed check for a value-fallback `else` arm: it must convert to the
// result type (the payload T, or a checked op's value type). A `panic` arm
// aborts and an `else return` arm targets the enclosing return type, so both are
// exempt — they're validated elsewhere.
static void check_else_arm(Expr *e, Type *result_ty) {
    if (!sema_walk_phase) return;
    if (e->as.else_expr.is_panic || e->as.else_expr.arm_is_return) return;
    Expr *arm = e->as.else_expr.arm;
    if (!arm || !arm->type || !result_ty) return;
    Range r = (arm->kind == EXPR_LITERAL)
              ? (Range){ arm->as.literal_expr.value, arm->as.literal_expr.value, true }
              : (sema_ranges ? sema_eval_range(arm, sema_ranges) : range_unknown());
    check_conversion(arm->type, result_ty, r, arm, arm->line, arm->col, "else fallback value", "else");
}

// ── Recursion-measure INFERENCE ─────────────────────────────────────────────
// Collect every self-call (a call whose callee resolves to `fn`) reachable in the
// body, so the inference can require a candidate parameter to descend in ALL of
// them. Completeness is not a soundness requirement — check_recursion_measure still
// verifies each self-call at walk time — but a thorough scan avoids proposing a
// parameter that some unscanned call would violate.
#define MAX_SELF_CALLS 64
static void collect_self_calls_e(Expr *e, Decl *fn, Expr **out, int *n);
static void collect_self_calls_s(Stmt *s, Decl *fn, Expr **out, int *n);
static void collect_self_calls_list(StmtList *b, Decl *fn, Expr **out, int *n) {
    for (; b; b = b->next) collect_self_calls_s(b->stmt, fn, out, n);
}
static void collect_self_calls_e(Expr *e, Decl *fn, Expr **out, int *n) {
    if (!e || *n >= MAX_SELF_CALLS) return;
    switch (e->kind) {
    case EXPR_CALL:
        if (e->as.call_expr.callee && e->as.call_expr.callee->decl == fn && *n < MAX_SELF_CALLS)
            out[(*n)++] = e;
        for (ExprList *a = e->as.call_expr.args; a; a = a->next)
            collect_self_calls_e(a->expr, fn, out, n);
        break;
    case EXPR_BINARY:
        collect_self_calls_e(e->as.binary_expr.left, fn, out, n);
        collect_self_calls_e(e->as.binary_expr.right, fn, out, n); break;
    case EXPR_UNARY:  collect_self_calls_e(e->as.unary_expr.right, fn, out, n); break;
    case EXPR_INDEX:
        collect_self_calls_e(e->as.index_expr.target, fn, out, n);
        collect_self_calls_e(e->as.index_expr.index, fn, out, n); break;
    case EXPR_MEMBER: collect_self_calls_e(e->as.member_expr.target, fn, out, n); break;
    case EXPR_CAST:   collect_self_calls_e(e->as.cast_expr.expr, fn, out, n); break;
    case EXPR_ADDR:   collect_self_calls_e(e->as.addr_expr.expr, fn, out, n); break;
    case EXPR_MATCH:
        collect_self_calls_e(e->as.match_expr.value, fn, out, n);
        for (ExprMatchCase *c = e->as.match_expr.cases; c; c = c->next)
            collect_self_calls_e(c->body, fn, out, n); break;
    case EXPR_ARRAY_LITERAL:
        for (ExprList *el = e->as.array_literal_expr.elements; el; el = el->next)
            collect_self_calls_e(el->expr, fn, out, n); break;
    default: break;
    }
}
static void collect_self_calls_s(Stmt *s, Decl *fn, Expr **out, int *n) {
    if (!s || *n >= MAX_SELF_CALLS) return;
    switch (s->kind) {
    case STMT_RETURN: collect_self_calls_e(s->as.return_stmt.value, fn, out, n); break;
    case STMT_VAR:    collect_self_calls_e(s->as.var_stmt.expr, fn, out, n); break;
    case STMT_ASSIGN: collect_self_calls_e(s->as.assign_stmt.target, fn, out, n);
                      collect_self_calls_e(s->as.assign_stmt.expr, fn, out, n); break;
    case STMT_EXPR:   collect_self_calls_e(s->as.expr_stmt.expr, fn, out, n); break;
    case STMT_IF:     collect_self_calls_e(s->as.if_stmt.cond, fn, out, n);
                      collect_self_calls_list(s->as.if_stmt.then_body, fn, out, n);
                      collect_self_calls_list(s->as.if_stmt.else_branch, fn, out, n); break;
    case STMT_WHILE:  collect_self_calls_e(s->as.while_stmt.cond, fn, out, n);
                      collect_self_calls_list(s->as.while_stmt.body, fn, out, n); break;
    case STMT_FOR:    collect_self_calls_list(s->as.for_stmt.body, fn, out, n); break;
    case STMT_MATCH:  collect_self_calls_e(s->as.match_stmt.value, fn, out, n);
                      for (StmtMatchCase *c = s->as.match_stmt.cases; c; c = c->next)
                          collect_self_calls_list(c->body, fn, out, n); break;
    case STMT_UNSAFE: collect_self_calls_list(s->as.unsafe_stmt.body, fn, out, n); break;
    default: break;
    }
}

// Propose a single-parameter termination measure for a recursive `func` that has no
// explicit `decreasing` clause: an integer parameter `p` is a candidate iff EVERY
// self-call passes a SYNTACTICALLY strictly-smaller argument in p's position
// (`p - K` with K>=1, or `p / D` with D>=2). Returns a synthesized identifier Expr
// naming that parameter, or NULL. This only PROPOSES the measure the user could have
// written — check_recursion_measure still fully verifies strict descent AND
// well-foundedness (>= 0, i.e. a real base case) at each self-call, so inference
// never relaxes the totality proof; it only removes the ceremony for the common case.
static Expr *infer_recursion_measure(Decl *fn) {
    Expr *calls[MAX_SELF_CALLS]; int ncalls = 0;
    collect_self_calls_list(fn->as.function_decl.body, fn, calls, &ncalls);
    if (ncalls == 0) return NULL;
    int idx = 0;
    for (DeclList *p = fn->as.function_decl.params; p; p = p->next, idx++) {
        Decl *pd = p->decl;
        if (!pd || pd->kind != DECL_VARIABLE || !pd->as.variable_decl.name) continue;
        if (!pd->as.variable_decl.type || !is_integer_type(pd->as.variable_decl.type)) continue;
        bool all_descend = true;
        for (int c = 0; c < ncalls && all_descend; c++) {
            Expr *arg = NULL; int i = 0;
            for (ExprList *a = calls[c]->as.call_expr.args; a; a = a->next, i++)
                if (i == idx) { arg = a->expr; break; }
            bool ok = false;
            if (arg && arg->kind == EXPR_BINARY && arg->as.binary_expr.left &&
                arg->as.binary_expr.left->kind == EXPR_IDENTIFIER &&
                arg->as.binary_expr.left->decl == pd) {
                TokenKind op = arg->as.binary_expr.op;
                Expr *r = arg->as.binary_expr.right;
                if (r && r->kind == EXPR_LITERAL) {
                    if (op == TOKEN_MINUS && r->as.literal_expr.value >= 1) ok = true;
                    else if (op == TOKEN_SLASH && r->as.literal_expr.value >= 2) ok = true;
                }
            }
            if (!ok) all_descend = false;
        }
        if (all_descend) {
            Expr *m = arena_push_aligned(sema_arena, Expr);
            memset(m, 0, sizeof(Expr));
            m->kind = EXPR_IDENTIFIER;
            m->as.identifier_expr.id = pd->as.variable_decl.name;
            m->line = fn->line; m->col = fn->col;
            return m;
        }
    }
    return NULL;
}

// `func f(...) decreasing m` permits recursion iff every self-call strictly
// decreases the well-founded measure m. For a single-parameter measure, prove at
// the call site (a) the matching argument < m (strict descent) and (b) m >= 0
// (well-founded, so descent bottoms out) via VRA. A non-decreasing or
// unbounded-below call is rejected, keeping `func` total. Runs after the call's
// args are inferred so their ranges (from enclosing guards) are available.
static void check_recursion_measure(Decl *fn, Expr *call) {
    if (!sema_walk_phase || !sema_ranges) return;
    Expr *m = fn->as.function_decl.decreasing_measure;
    if (!m) return;
    if (m->kind != EXPR_IDENTIFIER) {
        // A difference measure `hi - lo` (two params) is verified by the term-
        // linearization machinery (binary-search recursion). Returns false only
        // when the measure is not a two-identifier difference — then E091 stands.
        if (verify_recursion_expr_measure(fn, call)) return;
        fprintf(stderr, "[E091] Error Ln %li, Col %li: a `decreasing` measure on a recursive "
                "`func` must be a single parameter (e.g. `decreasing n`) or a difference of two "
                "parameters (e.g. `decreasing hi - lo`).\n",
                (long)call->line, (long)call->col);
        diagnostic_show_line(call->line, call->col); exit(1);
    }
    Id *mvar = m->as.identifier_expr.id;
    int idx = 0, found = -1;
    for (DeclList *p = fn->as.function_decl.params; p; p = p->next, idx++) {
        if (p->decl && p->decl->kind == DECL_VARIABLE && p->decl->as.variable_decl.name &&
            p->decl->as.variable_decl.name->length == mvar->length &&
            memcmp(p->decl->as.variable_decl.name->name, mvar->name, (size_t)mvar->length) == 0) {
            found = idx; break;
        }
    }
    if (found < 0) {
        fprintf(stderr, "[E091] Error Ln %li, Col %li: `decreasing` measure '%.*s' must be a "
                "parameter of the function.\n", (long)call->line, (long)call->col,
                (int)mvar->length, mvar->name);
        diagnostic_show_line(call->line, call->col); exit(1);
    }
    Expr *arg = NULL; int i = 0;
    for (ExprList *a = call->as.call_expr.args; a; a = a->next, i++)
        if (i == found) { arg = a->expr; break; }
    if (!arg) return;   // arity mismatch reported elsewhere
    // (a) strict descent, detected SYNTACTICALLY relative to the measure `m` (an
    // interval subtraction `arg - m` loses the shared-`m` correlation and can't
    // prove `(n-1) - n = -1`). Mirrors the loop measure's assignment_direction:
    //   m - K  (K >= 1)  |  m + K (K <= -1)  |  m / D (D >= 2, m >= 1)  → strictly < m.
    bool strict = false;
    if (arg->kind == EXPR_BINARY && expr_struct_equal(arg->as.binary_expr.left, m)) {
        TokenKind op = arg->as.binary_expr.op;
        Range rr = sema_eval_range(arg->as.binary_expr.right, sema_ranges);
        if (op == TOKEN_MINUS && rr.known && rr.min >= 1)       strict = true;
        else if (op == TOKEN_PLUS && rr.known && rr.max <= -1)  strict = true;
        else if (op == TOKEN_SLASH && rr.known && rr.min >= 2) {
            Range mr0 = sema_eval_range(m, sema_ranges);
            if (mr0.known && mr0.min >= 1) strict = true;       // halving, m >= 1
        }
    }
    // (b) well-founded: m provably >= 0 at the call site.
    Range mr = sema_eval_range(m, sema_ranges);
    bool wellfounded = mr.known && mr.min >= 0;
    if (!strict || !wellfounded) {
        // The legacy E082 for a failing recursion measure is DELETED. It is a TERMINATION VERDICT
        // and the sovereign engine returns one (`vra_self_call_site` + `vra_recursion_terminates`,
        // reported by analysis/report.h), which distinguishes a measure that is absent from one
        // that is present and failing — the distinction Annex B makes normative. (E091, the SHAPE
        // of a `decreasing` clause, stays: that is front-end policy, not a termination question.)
        return;
    }
}

// A field of a [packed] struct is BITS inside one integer: it has no address, so a `var` reference
// (`f(var r.pin1)`, `var x = var r.pin1`) or an `&r.pin1` has nothing to point at — in `unsafe`
// too, because this is representation, not a safety policy (the C would be `&` of a bitfield).
static void sema_check_packed_field_addr(Expr *m, isize line, isize col, const char *what) {
    if (!m || m->kind != EXPR_MEMBER || !m->as.member_expr.target || !m->as.member_expr.member) return;
    Type *ot = m->as.member_expr.target->type;
    while (ot && ot->kind == TYPE_COMPTIME) ot = ot->element_type;
    if (!ot || ot->kind != TYPE_SIMPLE || !ot->base_type) return;
    char sn[256]; int snl = (int)ot->base_type->length;
    if (snl >= (int)sizeof(sn)) return;
    memcpy(sn, ot->base_type->name, snl); sn[snl] = '\0';
    Symbol *ss = sema_lookup(sn);
    if (!ss || !ss->decl || ss->decl->kind != DECL_STRUCT || !ss->decl->as.struct_decl.is_packed) return;
    Id *fld = m->as.member_expr.member;
    fprintf(stderr, "[E121] Error Ln %li, Col %li: %s to `%.*s`, a field of [packed] struct `%s` — it is bits "
            "inside one integer and has no address. Read it, assign it, or pass the whole struct.\n",
            (long)line, (long)col, what, (int)fld->length, fld->name, sn);
    diagnostic_show_line(line, col);
    exit(1);
}

// A `var` reference to a field that takes part in an `in` invariant is refused. The invariant is
// kept by checking every WRITE to the field or its container at the write (E121 in the IR), and a
// write through a reference is not a write to the field as far as that check can see: the callee,
// or the binding, stores to a bare `var usize`. `bump(var c.pos)` with `x = 4` in `bump` read four
// bytes past a four-byte slice under ASan, and was accepted. Passing the whole struct (`var c`)
// keeps every write where the check sees it.
static void sema_check_mut_invariant_field(Expr *e) {
    Expr *m = e ? e->as.mut_expr.expr : NULL;
    if (!m || m->kind != EXPR_MEMBER || !m->as.member_expr.target || !m->as.member_expr.member) return;
    Type *ot = m->as.member_expr.target->type;
    if (!ot || ot->kind != TYPE_SIMPLE || !ot->base_type) return;
    char sn[256]; int snl = (int)ot->base_type->length;
    if (snl >= (int)sizeof(sn)) return;
    memcpy(sn, ot->base_type->name, snl); sn[snl] = '\0';
    Symbol *ss = sema_lookup(sn);
    if (!ss || !ss->decl || ss->decl->kind != DECL_STRUCT) return;
    Id *fld = m->as.member_expr.member;
    // A RELATIONAL field invariant (`pos usize <= src.len`, `len usize <= cap`) is kept the same
    // way, so a reference to either of its fields escapes it the same way.
    for (DeclList *sf = ss->decl->as.struct_decl.fields; sf; sf = sf->next) {
        if (!sf->decl || sf->decl->kind != DECL_VARIABLE) continue;
        Id *fn = sf->decl->as.variable_decl.name;
        if (!fn) continue;
        for (ExprList *cn = sf->decl->as.variable_decl.constraints; cn; cn = cn->next) {
            Expr *con = cn->expr;
            if (!con || con->kind != EXPR_BINARY) continue;
            Expr *rhs = con->as.binary_expr.right;
            if (rhs && rhs->kind == EXPR_MEMBER) rhs = rhs->as.member_expr.target;
            if (!rhs || rhs->kind != EXPR_IDENTIFIER || !rhs->as.identifier_expr.id) continue;
            Id *gn = rhs->as.identifier_expr.id;
            bool is_f = fn->length == fld->length && strncmp(fn->name, fld->name, fn->length) == 0;
            bool is_g = gn->length == fld->length && strncmp(gn->name, fld->name, gn->length) == 0;
            if (!is_f && !is_g) continue;
            fprintf(stderr, "[E121] Error Ln %li, Col %li: a `var` reference to `%.*s` can break the struct's "
                    "invariant on `%.*s` (it relates `%.*s` to `%.*s`) — a write through the reference is "
                    "not checked against it. Pass the whole struct as `var` instead; its field writes "
                    "are checked.\n", (long)e->line, (long)e->col, (int)fld->length, fld->name,
                    (int)fn->length, fn->name, (int)fn->length, fn->name, (int)gn->length, gn->name);
            diagnostic_show_line(e->line, e->col);
            exit(1);
        }
    }
}


// A CLOSED constant: built only from number and character literals with unary and binary arithmetic,
// so its value is fixed when the program is compiled (`-3`, `0 - 3`, `'a' + 1`). Names are not
// followed: a membership list is a set written out (I.79).
static bool tc_closed_const(Expr *e, int depth) {
    if (!e || depth > 16) return false;
    switch (e->kind) {
    case EXPR_LITERAL: case EXPR_CHAR: return true;
    case EXPR_UNARY:
        return (e->as.unary_expr.op == TOKEN_MINUS || e->as.unary_expr.op == TOKEN_TILDE) &&
               tc_closed_const(e->as.unary_expr.right, depth + 1);
    case EXPR_BINARY: {
        TokenKind op = e->as.binary_expr.op;
        bool arith = op == TOKEN_PLUS || op == TOKEN_MINUS || op == TOKEN_ASTERISK || op == TOKEN_SLASH ||
                     op == TOKEN_PERCENT || op == TOKEN_AMPERSAND || op == TOKEN_PIPE || op == TOKEN_CARET;
        return arith && tc_closed_const(e->as.binary_expr.left, depth + 1) &&
               tc_closed_const(e->as.binary_expr.right, depth + 1);
    }
    default: return false;
    }
}

// `a.b.c` into buf when e is a name or a member chain (for a message that quotes the program);
// buf is left as it was otherwise.
static void tc_dotted_path(Expr *e, char *buf, size_t cap) {
    char tmp[96]; size_t n = 0;
    Expr *parts[8]; int np = 0;
    while (e && e->kind == EXPR_MEMBER && np < 7) { parts[np++] = e; e = e->as.member_expr.target; }
    if (!e || e->kind != EXPR_IDENTIFIER) return;
    Id *root = e->as.identifier_expr.id;
    if ((size_t)root->length >= sizeof tmp) return;
    memcpy(tmp, root->name, (size_t)root->length); n = (size_t)root->length;
    for (int k = np - 1; k >= 0; k--) {
        Id *m = parts[k]->as.member_expr.member;
        if (n + 1 + (size_t)m->length >= sizeof tmp) return;
        tmp[n++] = '.'; memcpy(tmp + n, m->name, (size_t)m->length); n += (size_t)m->length;
    }
    tmp[n] = '\0';
    if (n < cap) memcpy(buf, tmp, n + 1);
}

void sema_infer_expr(Expr *e) {
  if (!e) return;
// (removed debug print)
  switch (e->kind) {
  case EXPR_IDENTIFIER:
    // already set in resolve
    if (current_function_decl && current_function_decl->kind == DECL_FUNCTION) {
        if (e->is_global && e->decl && e->decl->kind == DECL_VARIABLE && e->decl->as.variable_decl.is_mutable) {
            fprintf(stderr, "[E011] Error Ln %li, Col %li: pure function '%.*s' cannot read "
                    "mutable global variable '%.*s' (its result would depend on hidden state).\n",
                    (long)e->line, (long)e->col,
                    (int)current_function_decl->as.function_decl.name->length, current_function_decl->as.function_decl.name->name,
                    (int)e->as.identifier_expr.id->length, e->as.identifier_expr.id->name);
            diagnostic_show_line(e->line, e->col);
            exit(1);
        }
    }
    // Keystone rung 1: a use of a variable/param whose declared constraints have a
    // CONSTANT bound (`x i32 < 9`, `>= 0 and <= 100`) carries that interval on its
    // type, so a boundary proves `x <: Digit` by subsumption. Relational bounds
    // (`x < k`) yield no constant interval and stay in the range table. Sound: the
    // declared constraint is enforced at every assignment, so it always holds. A
    // fresh copy — never mutate the decl's stored type.
    if (e->decl && e->decl->kind == DECL_VARIABLE && e->type && !e->type->refine.known &&
        e->decl->as.variable_decl.constraints) {
        Range cr = range_from_refinement_constraints(e->decl->as.variable_decl.constraints);
        if (cr.known) {
            Type *rt = arena_push_aligned(sema_arena, Type);
            *rt = *e->type;
            rt->refine.known = true; rt->refine.lo = cr.min; rt->refine.hi = cr.max;
            e->type = rt;
        }
    }
    break;

// ...

  case EXPR_MEMBER: {
    // Whether this member is the callee of a call (set by EXPR_CALL for its callee only, and
    // consumed here before the target is inferred, so `a.b.c()` makes only `.c` a callee).
    bool is_callee = sema_member_is_callee;
    sema_member_is_callee = false;
    sema_infer_expr(e->as.member_expr.target);
    Type *t = e->as.member_expr.target->type;
    // ★ A FIELD READ THROUGH A RAW POINTER IS A DEREFERENCE (I.125). `p.v` dereferences `p`
    // automatically, so it did not look like one: with `var p *S = 0`, `return p.v` compiled in
    // safe code and segfaulted, while `*p` was E060 and the write `p.v = 5` E009 (Documentation).
    // A borrow (`var s S`) is not a raw pointer, and stays safe.
    {   Type *bt = t; while (bt && bt->kind == TYPE_COMPTIME && bt->element_type) bt = bt->element_type;
        if (bt) bt = resolve_type_alias(bt);
        if (sema_walk_phase && !sema_in_unsafe_block && !is_callee && e != sema_store_target &&
            bt && bt->kind == TYPE_POINTER) {
            fprintf(stderr, "[E060] Error Ln %li, Col %li: reading a field through a raw pointer outside "
                    "'unsafe': `.` dereferences it. Read it inside 'unsafe { }', or take the struct as a "
                    "borrow (`func f(s S)` or `func f(var s S)`), which the compiler checks.\n",
                    (long)e->line, (long)e->col);
            diagnostic_show_line(e->line, e->col);
            exit(1);
        } }
    
    // Case 1: Accessing ADT Variant Constructor (e.g. Shape.Circle)
    // ONLY valid if the target resolves to the Enum declaration itself!
    if (e->as.member_expr.target->kind == EXPR_IDENTIFIER || e->as.member_expr.target->kind == EXPR_TYPE) {
        if (e->as.member_expr.target->decl && e->as.member_expr.target->decl->kind == DECL_ENUM) {
            DeclEnum *adt = &e->as.member_expr.target->decl->as.enum_decl;
            Variant *v = lookup_adt_variant(adt, e->as.member_expr.member);
            if (!v) {
                fprintf(stderr, "[E106] Error Ln %li, Col %li: ADT has no variant '%.*s'\n",
                        e->line, e->col,
                        (int)e->as.member_expr.member->length, e->as.member_expr.member->name);
                diagnostic_show_line(e->line, e->col);
                exit(1);
            }
            // ADT variant evaluates to the ADT instance type
            e->type = e->as.member_expr.target->as.type_expr.type_value;
            e->decl = e->as.member_expr.target->decl;
            return;
        }
        // If it's EXPR_IDENTIFIER but NOT an enum, it's a variable instance (e.g. `s.Circle`). Falls down.
    }

    if (!t) {
        // The target of a member access has no value. A module is qualified by the last segment
        // of its path or by the alias it is imported under (`math.max`, `m.max`), so when the
        // target is a module named another way the message says which qualifier works. It was
        // E102 (the Annex's code for an attribute name) at Ln 0, and it advised "imports share
        // a flat namespace: call 'math' directly": false since qualified access landed, and for
        // `std.math.max` an instruction to call a module.
        Expr *tgt = e->as.member_expr.target;
        int ml = (int)e->as.member_expr.member->length;
        const char *mn = e->as.member_expr.member->name;
        if (tgt && tgt->kind == EXPR_IDENTIFIER && tgt->as.identifier_expr.id && !tgt->decl) {
            int tl = (int)tgt->as.identifier_expr.id->length;
            const char *tn = tgt->as.identifier_expr.id->name;
            const char *mod; const char *q = NULL; size_t ql = 0;
            if ((mod = module_with_path_head(tn, (size_t)tl, mn, (size_t)ml)) &&
                (q = module_qualifier_of(current_module_path, mod, &ql)))
                fprintf(stderr, "[E106] Error Ln %li, Col %li: '%.*s' is not a value. A module is "
                        "named by the last segment of its path or by its alias, so '%s' is `%.*s` "
                        "here: write `%.*s.` and the name, not the whole path.\n",
                        e->line, e->col, tl, tn, mod, (int)ql, q, (int)ql, q);
            else if (mod)
                fprintf(stderr, "[E106] Error Ln %li, Col %li: '%.*s' is not a value, and module "
                        "'%s' is not imported here: import it, then qualify a name by the last "
                        "segment of its path.\n", e->line, e->col, tl, tn, mod);
            else if ((mod = module_with_last_segment(tn, (size_t)tl)) &&
                     (q = module_qualifier_of(current_module_path, mod, &ql)) &&
                     !(ql == (size_t)tl && strncmp(q, tn, ql) == 0))
                fprintf(stderr, "[E106] Error Ln %li, Col %li: '%.*s' is not a value. Module '%s' "
                        "is imported as '%.*s': write `%.*s.%.*s`.\n",
                        e->line, e->col, tl, tn, mod, (int)ql, q, (int)ql, q, ml, mn);
            else if (mod && !q)
                fprintf(stderr, "[E106] Error Ln %li, Col %li: '%.*s' is not a value, and module "
                        "'%s' is not imported here: `import %s` makes `%.*s.%.*s` available.\n",
                        e->line, e->col, tl, tn, mod, mod, tl, tn, ml, mn);
            else
                fprintf(stderr, "[E106] Error Ln %li, Col %li: cannot access member '%.*s' of "
                        "'%.*s': '%.*s' is neither a value nor an imported module.\n",
                        e->line, e->col, ml, mn, tl, tn, tl, tn);
        } else if (tgt && tgt->kind == EXPR_TYPE) {
            char tb[128]; type_describe(tgt->as.type_expr.type_value, tb, sizeof tb);
            fprintf(stderr, "[E128] Error Ln %li, Col %li: cannot access member '%.*s' of the "
                    "type '%s': a member is read from a value of the type.\n",
                    e->line, e->col, ml, mn, tb);
        } else {
            fprintf(stderr, "[E128] Error Ln %li, Col %li: cannot access member '%.*s': the "
                    "expression before the '.' has no value.\n", e->line, e->col, ml, mn);
        }
        diagnostic_show_line(e->line, e->col);
        exit(1);
    }

    // Unwrap wrappers (mut, mov, ptr)
    t = sema_unwrap_type(t);

    if (t->kind == TYPE_VARIANT) {
        // We are accessing a field of an ADT variant payload (e.g. radius in shape.Circle.radius)
        Variant *v = t->variant;
        for (DeclList *f = v->fields; f; f = f->next) {
            Id *fname = f->decl->as.variable_decl.name;
            if (fname->length == e->as.member_expr.member->length &&
                strncmp(fname->name, e->as.member_expr.member->name, fname->length) == 0) {
                e->type = f->decl->as.variable_decl.type;
                return;
            }
        }
        fprintf(stderr, "[E128] Error Ln %li, Col %li: variant '%.*s' has no field '%.*s'\n",
                e->line, e->col,
                (int)v->name->length, v->name->name,
                (int)e->as.member_expr.member->length, e->as.member_expr.member->name);
        diagnostic_show_line(e->line, e->col);
        exit(1);
    }
    
    // ADT Direct Unsafe Unpacking
    DeclEnum *adt_decl = NULL;
    if (t->kind == TYPE_SIMPLE && (adt_decl = find_adt_decl(t->base_type)) != NULL) {
        Variant *v = lookup_adt_variant(adt_decl, e->as.member_expr.member);
        if (v) {
            if (!sema_in_unsafe_block) {
                fprintf(stderr, "[E125] Error Ln %li, Col %li: direct ADT field access ('%.*s.%.*s') is only allowed inside an 'unsafe' block — destructure with `case` instead.\n",
                        e->line, e->col,
                        (int)t->base_type->length, t->base_type->name,
                        (int)e->as.member_expr.member->length, e->as.member_expr.member->name);
                diagnostic_show_line(e->line, e->col);
                exit(1);
            }
            
            Type *var_type = arena_push_aligned(sema_arena, Type);
            var_type->kind = TYPE_VARIANT;
            var_type->variant = v;
            e->type = var_type;
            return;
        }
    }

    // If target is a slice or array, handle the common fields: .len and .data
    if (t->kind == TYPE_ARRAY || t->kind == TYPE_SLICE) {
      Id *mem = e->as.member_expr.member;
      if (mem && mem->length == 3 && strncmp(mem->name, "len", 3) == 0) {
        // ★ .len is a usize, not an i32.
        //
        // It was an i32, and that one line propagated everywhere: `for i in 0..arr.len` typed
        // its counter from the range and got an i32, `while i < arr.len` compared an i32
        // against a length, and `i + 1` then had to be proved to fit in i32 — which it cannot,
        // because nothing bounds a length below INT32_MAX. The overflow check refused the most
        // ordinary loop in the language, CORRECTLY, for a reason the programmer could not see.
        //
        // The A1 precision survey attributed 12% of every unproven obligation in the corpus to
        // that shape. A length is a count of elements in memory; usize is what it has always
        // been in the emitted C (`size_t`), so this makes the type say what the value is.
        e->type = type_simple(sema_arena, id(sema_arena, 5, "usize"));
        break;
      }
      if (mem && mem->length == 4 && strncmp(mem->name, "data", 4) == 0) {
        // .data → pointer to element_type
        e->type = type_pointer(sema_arena, t->element_type);
        break;
      }
    }

    // ★ UFCS ON A NON-STRUCT. `x.is_even()` where x is an i32 is `is_even(x)` — the whole
    // point of universal call syntax — but the field lookup below EXITS when the receiver's
    // type names no struct, so it never reached the UFCS fallback and the user got a raw
    // internal message ("sema error: unknown struct 'i32'") with no code and no line. Ask
    // whether the member names a FUNCTION first; the call site rewrites it.
    if (!find_struct_decl(t->base_type)) {
        char ubuf[256];
        int ulen = e->as.member_expr.member->length < 255 ? e->as.member_expr.member->length : 255;
        memcpy(ubuf, e->as.member_expr.member->name, ulen); ubuf[ulen] = '\0';
        Symbol *usym = sema_lookup(ubuf);
        if (usym && usym->decl && (usym->decl->kind == DECL_FUNCTION
                                || usym->decl->kind == DECL_EXTERN_FUNCTION)) {
            if (!is_callee) sema_report_function_as_member(e, t);
            e->type = NULL;   // the parent EXPR_CALL turns this into `member(target, …)`
            return;
        }
        // Neither a field (the type is not a struct) nor a function. This used to fall into
        // `lookup_struct_field_type`, which exits with a raw internal line — no code, no
        // position, no suggestion. A user-facing failure deserves a user-facing diagnostic.
        // The type is DESCRIBED, not read as a name: an array or a slice has no base name, and
        // `a.zz` on `var a = [1, 2]` crashed the compiler printing one.
        char tb[128]; type_describe(t, tb, sizeof tb);
        fprintf(stderr, "[E128] Error Ln %li, Col %li: '%s' has no member '%.*s', and no "
                "function of that name is in scope to call as `%.*s(x, ...)`.\n",
                (long)e->line, (long)e->col, tb,
                (int)e->as.member_expr.member->length, e->as.member_expr.member->name,
                (int)e->as.member_expr.member->length, e->as.member_expr.member->name);
        diagnostic_show_line(e->line, e->col);
        exit(1);
    }
    // fall back to struct field lookup (existing behavior)
    e->type = lookup_struct_field_type(t->base_type, e->as.member_expr.member);
    
    if (!e->type) {
        // UFCS Fallback check: does a function exist?
        char mbuf[256];
        int mlen = e->as.member_expr.member->length < 255 ? e->as.member_expr.member->length : 255;
        memcpy(mbuf, e->as.member_expr.member->name, mlen);
        mbuf[mlen] = '\0';
        Symbol *sym = sema_lookup(mbuf);
        if (sym && sym->decl && (sym->decl->kind == DECL_FUNCTION || sym->decl->kind == DECL_EXTERN_FUNCTION)) {
            // It might be a UFCS method call (e.g., `l.consume()`).
            // We leave `e->type = NULL`. The parent `EXPR_CALL` will detect this
            // and rewrite the AST to `consume(l)`.
            if (!is_callee) sema_report_function_as_member(e, t);
        } else {
            fprintf(stderr, "[E128] Error Ln %li, Col %li: struct '%.*s' has no field '%.*s'\n",
                (long)e->line, (long)e->col, (int)t->base_type->length, t->base_type->name, 
                (int)e->as.member_expr.member->length, e->as.member_expr.member->name);
            diagnostic_show_line(e->line, e->col);
            exit(1);
        }
    }
    break;
  }

// ...

  case EXPR_CALL: {
    // ensure callee resolved & infer args
    sema_member_is_callee = true;   // a member callee may be a UFCS function (EXPR_MEMBER)
    sema_infer_expr(e->as.call_expr.callee); // Changed from resolve to infer to handle Shape.Circle
    sema_member_is_callee = false;

    // Call THROUGH a function pointer: the callee is a VALUE of TYPE_FUNC (a
    // variable/param/field), not a direct function reference. Check args
    // against the pointer's parameter types (same discipline as a direct call)
    // and take the result type from the pointer's return type.
    {
      Decl *cd = e->as.call_expr.callee->decl;
      bool callee_is_direct_fn = cd && (cd->kind == DECL_FUNCTION ||
                                        cd->kind == DECL_EXTERN_FUNCTION);
      // Soundness of the emitted `restrict` — a mutated reference parameter must not be aliased by
      // another argument — is the sovereign borrow pass's obligation now (analysis/borrow.h, phase A).
      Type *fnty = e->as.call_expr.callee->type;
      if (!callee_is_direct_fn && fnty && fnty->kind == TYPE_FUNC) {
        TypeList *pt = fnty->func_params;
        ExprList *arg = e->as.call_expr.args;
        while (pt && arg) {
          sema_infer_expr(arg->expr);
          sema_union_coerce(&arg->expr, pt->type);   // `T | markers` construction
          Range r = (arg->expr->kind == EXPR_LITERAL)
                    ? (Range){ arg->expr->as.literal_expr.value, arg->expr->as.literal_expr.value, true }
                    : (sema_ranges ? sema_eval_range(arg->expr, sema_ranges) : range_unknown());
          check_conversion(arg->expr->type, pt->type, r, arg->expr, arg->expr->line, arg->expr->col,
                           "function-pointer argument", "");
          pt = pt->next; arg = arg->next;
        }
        if (pt || arg) {
          fprintf(stderr, "[E123] Error Ln %li, Col %li: wrong number of arguments in call through "
                  "a function pointer.\n", (long)e->line, (long)e->col);
          diagnostic_show_line(e->line, e->col); exit(1);
        }
        e->type = fnty->element_type;   // return type (NULL = void)
        break;
      }
    }

    // Check if this is an ADT constructor call
    if (e->as.call_expr.callee->kind == EXPR_MEMBER) {
        Expr *target = e->as.call_expr.callee->as.member_expr.target;
        // The target of the ctor (`Shape` in `Shape.Circle(...)`) resolves to
        // either an identifier or — after type resolution — an EXPR_TYPE. Both
        // carry the enum decl in ->decl; only identifiers also support the
        // find_adt_decl fallback. Mirror the EXPR_MEMBER handler above.
        if (target->kind == EXPR_IDENTIFIER || target->kind == EXPR_TYPE) {
             DeclEnum *adt = NULL;
             if (target->decl && target->decl->kind == DECL_ENUM) {
                 adt = &target->decl->as.enum_decl;
             } else if (target->kind == EXPR_IDENTIFIER) {
                 adt = find_adt_decl(target->as.identifier_expr.id);
             }
             if (adt) {
                 // It IS an ADT constructor call: Shape.Circle(...)
                 // Verify arguments match fields
                 Variant *v = lookup_adt_variant(adt, e->as.call_expr.callee->as.member_expr.member);
                 assert(v && "Variant should have been found in EXPR_MEMBER");
                 
                 ExprList *arg = e->as.call_expr.args;
                 DeclList *field = v->fields;
                 
                 int arg_idx = 0;
                 while (arg && field) {
                     sema_infer_expr(arg->expr);
                     // Check the payload argument against the variant field's type,
                     // same as a struct constructor — was a TODO, so `Circle(3.9)`
                     // truncated float->int and `Circle(300)` overflowed a u8 field
                     // silently (and other mismatches emitted broken C).
                     Type *field_ty = (field->decl && field->decl->kind == DECL_VARIABLE)
                                      ? field->decl->as.variable_decl.type : NULL;
                     if (field_ty && arg->expr) {
                         Range r;
                         if (arg->expr->kind == EXPR_LITERAL)
                             r = (Range){ arg->expr->as.literal_expr.value,
                                          arg->expr->as.literal_expr.value, true };
                         else if (sema_ranges)
                             r = sema_eval_range(arg->expr, sema_ranges);
                         else
                             r = range_unknown();
                         char vlbl[64];
                         int vn = v->name ? (int)v->name->length : 0;
                         if (vn > 63) vn = 63;
                         if (vn) memcpy(vlbl, v->name->name, vn);
                         vlbl[vn] = '\0';
                         check_conversion(arg->expr->type, field_ty, r, arg->expr,
                             arg->expr->line, arg->expr->col, "enum variant field", vlbl);
                     }
                     arg = arg->next;
                     field = field->next;
                     arg_idx++;
                 }
                 
                 if (arg || field) {
                     fprintf(stderr, "[E012] Error Ln %li, Col %li: wrong number of arguments for variant constructor '%.*s'\n",
                             (long)e->line, (long)e->col, (int)v->name->length, v->name->name);
                     exit(1);
                 }
                 
                 e->type = e->as.call_expr.callee->type; // The ADT type
                 break;
             }
        }
    }
    
    // Check for UFCS: `a.method(b)` -> `method(a, b)`
    // If the callee is EXPR_MEMBER and it failed to find a field (type is NULL),
    // we assume it's a UFCS call. ★ So is a member that IS a field, unless the field holds a
    // function (handled above as a call through a pointer): a value is not callable, and the
    // check used to run only when the member had no type, so `p.x()` on an i32 field went on as
    // a direct call of a member and emitted C gcc rejects ("expected expression before ')'").
    if (e->as.call_expr.callee->kind == EXPR_MEMBER &&
        (!e->as.call_expr.callee->type || e->as.call_expr.callee->type->kind != TYPE_FUNC)) {
        Expr *target = e->as.call_expr.callee->as.member_expr.target;
        Id *method_name = e->as.call_expr.callee->as.member_expr.member;
        
        char mbuf[256];
        int mlen = method_name->length < 255 ? method_name->length : 255;
        memcpy(mbuf, method_name->name, mlen);
        mbuf[mlen] = '\0';
        
        Symbol *sym = sema_lookup(mbuf);
        if (sym && sym->decl && (sym->decl->kind == DECL_FUNCTION || sym->decl->kind == DECL_EXTERN_FUNCTION)) {
            // It is a valid function! We convert the AST node to represent a UFCS call.
            // 1. Change the callee to simply be the function identifier
            Expr *new_callee = arena_push(sema_arena, Expr);
            new_callee->kind = EXPR_IDENTIFIER;
            new_callee->as.identifier_expr.id = method_name;
            new_callee->line = e->as.call_expr.callee->line;
            new_callee->col = e->as.call_expr.callee->col;
            
            // 2. Prepend the target as the first argument.
            // F-021: UFCS auto-wraps the target to match the first param's
            // ownership mode (var/mov). Auto-wrapping was previously only
            // applied for MUTABLE, leaving OWNED to fail silently. Now both
            // are handled symmetrically.
            ExprList *new_arg = arena_push(sema_arena, ExprList);
            DeclList *params = sym->decl->as.function_decl.params;
            OwnershipMode first_mode = MODE_SHARED;
            if (params && params->decl->kind == DECL_VARIABLE &&
                params->decl->as.variable_decl.type) {
                first_mode = params->decl->as.variable_decl.type->mode;
            }
            if (first_mode == MODE_MUTABLE && target->kind != EXPR_MUT) {
                Expr *mut_target = arena_push(sema_arena, Expr);
                mut_target->kind = EXPR_MUT;
                mut_target->as.mut_expr.expr = target;
                mut_target->line = target->line;
                mut_target->col = target->col;
                new_arg->expr = mut_target;
            } else if (first_mode == MODE_OWNED && target->kind != EXPR_MOVE) {
                Expr *mov_target = arena_push(sema_arena, Expr);
                mov_target->kind = EXPR_MOVE;
                mov_target->as.move_expr.expr = target;
                mov_target->line = target->line;
                mov_target->col = target->col;
                new_arg->expr = mov_target;
            } else {
                new_arg->expr = target;
            }
            
            new_arg->next = e->as.call_expr.args;
            
            // 3. Update the call expression
            e->as.call_expr.callee = new_callee;
            e->as.call_expr.args = new_arg;
            
            // Now proceed with normal call logic
            sema_infer_expr(e->as.call_expr.callee);
        } else if (e->as.call_expr.callee->type) {
            char tb[128], fb[128];
            type_describe(target->type, tb, sizeof tb);
            type_describe(e->as.call_expr.callee->type, fb, sizeof fb);
            fprintf(stderr, "[E128] Error Ln %li, Col %li: '%.*s' is a member of '%s' of type '%s', "
                    "not a function, and no function '%.*s' is in scope to call as `%.*s(x, ...)`.\n",
                    (long)e->line, (long)e->col, (int)method_name->length, method_name->name, tb, fb,
                    (int)method_name->length, method_name->name,
                    (int)method_name->length, method_name->name);
            diagnostic_show_line(e->line, e->col);
            exit(1);
        } else {
            char tb[128]; type_describe(target->type, tb, sizeof tb);
            fprintf(stderr, "[E128] Error Ln %li, Col %li: struct field or UFCS method '%.*s' not found on type '%s'\n",
                    (long)e->line, (long)e->col, (int)method_name->length, method_name->name, tb);
            diagnostic_show_line(e->line, e->col);
            exit(1);
        }
    }
    
    // Normal function call logic...
    { bool callee_saved = sema_resolving_callee;   // I.137: the callee keeps its return type
      sema_resolving_callee = true;
      sema_resolve_expr(e->as.call_expr.callee);
      sema_resolving_callee = callee_saved; }
    
    // Purity check: func cannot call proc
    if (current_function_decl && current_function_decl->kind == DECL_FUNCTION) {
        Expr *callee = e->as.call_expr.callee;
        if (callee->decl) {
            // ★ The keyword-keyed purity check WAS DUPLICATED HERE, and the duplicate is
            // deleted with the original (see the note in resolve.h at the same rule). The two
            // copies had already drifted — this one carried a doubled comment, a residue of the
            // last time someone tried to reconcile them. One rule, one place: the effect row.
            // Termination Analysis: recursion in `func` is banned UNLESS the
            // function carries a `decreasing <measure>` clause — then it is allowed
            // and each self-call is verified to strictly decrease the measure
            // (check_recursion_measure, after the args below are inferred).
            if (callee->decl == current_function_decl &&
                !current_function_decl->as.function_decl.decreasing_measure) {
                // Try to INFER the measure from the self-calls (a parameter that
                // strictly descends in all of them). If found, install it and let
                // check_recursion_measure verify it fully below; if not, the loop
                // truly can't be shown total → E011 (add explicit `decreasing`).
                Expr *inferred = infer_recursion_measure(current_function_decl);
                if (inferred) {
                    current_function_decl->as.function_decl.decreasing_measure = inferred;
                }
                // No `else`: the sovereign engine raises the recursion obligation
                // (`vra_self_call_site` + `vra_recursion_terminates`, reported by
                // analysis/report.h) and says it with a position and the three ways out. The
                // legacy message here had none of that — "recursion is not allowed in pure
                // function 'f'" with no line — and it is deleted. Inference still runs above,
                // because a measure it can synthesise is what keeps ordinary recursion clause-free.
            }
        }
    }

    for (ExprList *a = e->as.call_expr.args; a; a = a->next) {
      sema_infer_expr(a->expr);
    }

    // Recursion in a `func` with a `decreasing` measure: verify this self-call
    // strictly decreases the (well-founded) measure. Args are inferred above so
    // their VRA ranges (from enclosing guards) are available.
    if (sema_walk_phase && current_function_decl &&
        current_function_decl->kind == DECL_FUNCTION &&
        e->as.call_expr.callee && e->as.call_expr.callee->decl == current_function_decl &&
        current_function_decl->as.function_decl.decreasing_measure) {
        check_recursion_measure(current_function_decl, e);
    }

    // Argument count check — skip extern functions (may be variadic like printf)
    {
        Decl *cd = e->as.call_expr.callee->decl;
        if (cd && (cd->kind == DECL_FUNCTION)) {
            int n_params = 0, n_args = 0;
            for (DeclList *p = cd->as.function_decl.params; p; p = p->next) n_params++;
            for (ExprList *a = e->as.call_expr.args; a; a = a->next) n_args++;
            if (n_args != n_params) {
                Id *fn_name = cd->as.function_decl.name;
                fprintf(stderr, "[E012] Error Ln %li, Col %li: function '%.*s' expects %d argument(s), got %d.\n",
                        (long)e->line, (long)e->col,
                        (int)fn_name->length, fn_name->name,
                        n_params, n_args);
                diagnostic_show_line(e->line, e->col);
                exit(1);
            }
        }
    }
    
    // Verify equation-style constraints at call site
    Decl *callee_decl = e->as.call_expr.callee->decl;
    // ★ D-16: a call to a name NOTHING declares. The emitted C leaned on C's implicit
    // declaration rule — removed as an error in C23 — so a TYPO compiled: `totally_made_up_fn(42)`
    // produced a call to a function that does not exist, and every analysis resolved that name
    // to nothing and treated the call as opaque, i.e. as permissively as possible. For a
    // compiler whose whole claim is prove-or-reject, accepting an unresolved call is the hole.
    //
    // Only a BARE IDENTIFIER callee is judged here: a call through a function POINTER has a
    // TYPE_FUNC and returned above, a variant/method callee is an EXPR_MEMBER, and `panic` is a
    // declless builtin the language defines itself.
    if (sema_walk_phase && !callee_decl) {
        Expr *cx = e->as.call_expr.callee;
        Type *cty = cx ? cx->type : NULL;
        Id   *cn  = (cx && cx->kind == EXPR_IDENTIFIER) ? cx->as.identifier_expr.id : NULL;
        bool is_panic = cn && cn->length == 5 && strncmp(cn->name, "panic", 5) == 0;
        if (cn && !is_panic && !(cty && cty->kind == TYPE_FUNC)) {
            fprintf(stderr, "[E127] Error Ln %li, Col %li: call to undeclared function '%.*s' — "
                    "nothing in scope declares it. Add an `extern func %.*s(...)` declaration, "
                    "import the module that defines it, or fix the spelling.\n",
                    (long)e->line, (long)e->col, (int)cn->length, cn->name,
                    (int)cn->length, cn->name);
            diagnostic_show_line(e->line, e->col);
            exit(1);
        }
    }
    if (callee_decl && (callee_decl->kind == DECL_FUNCTION ||
                        callee_decl->kind == DECL_EXTERN_FUNCTION)) {
        
        int param_idx = 0;
        DeclList *params = callee_decl->as.function_decl.params;
        
        for (DeclList *p = params; p; p = p->next) {
            


            // (A parameter's refinement is the IR's obligation: asserted at the call — ir_lower_call_requires.)

            // Q-002 Phase 5: overflow-at-boundary check (call argument).
            // Skip integer literals (polymorphic across iN/uN — trusted to fit).
            // Only fire during walk phase: during resolve, VRA hasn't yet
            // built up local variable ranges so we'd hit false positives.
            if (sema_walk_phase && p->decl->kind == DECL_VARIABLE && sema_ranges) {
                Type *ptype = p->decl->as.variable_decl.type;
                Expr *parg = NULL;
                int a_idx = 0;
                for (ExprList *a = e->as.call_expr.args; a; a = a->next) {
                    if (a_idx == param_idx) { parg = a->expr; break; }
                    a_idx++;
                }
                if (parg && ptype) {
                    // P2/S3: also range-check LITERAL arguments (previously skipped
                    // — same overflow gap as struct-field-init: take(300) with a u8
                    // parameter compiled silently).
                    Range r;
                    if (parg->kind == EXPR_LITERAL) {
                        // Resolve refinement aliases to their integer base so a
                        // literal arg gets its point range (needed for the alias
                        // constraint check inside check_conversion — fixes f(200)
                        // where p is a `type Pct = i32 >= 0 and <= 100`).
                        r = is_integer_type(resolve_type_alias(ptype))
                            ? (Range){ parg->as.literal_expr.value,
                                       parg->as.literal_expr.value, true }
                            : range_unknown();
                    } else {
                        r = sema_eval_range(parg, sema_ranges);
                    }
                    Id *pname = p->decl->as.variable_decl.name;
                    char buf[160];
                    int n = pname ? (int)pname->length : 0;
                    if (n > 159) n = 159;
                    if (n) memcpy(buf, pname->name, n);
                    buf[n] = '\0';
                    check_conversion(parg->type, ptype, r, parg, parg->line, parg->col,
                        "argument to parameter", buf);
                    // An immutable array handed to an ARRAY parameter: the callee may write it
                    // as an output reference. The IR decides, from the callee's write footprint
                    // (ir_check_readonly_args).
                    Type *pt0 = ptype; while (pt0 && pt0->kind == TYPE_COMPTIME) pt0 = pt0->element_type;
                    if (pt0 && (pt0->kind == TYPE_ARRAY || pt0->kind == TYPE_SLICE) && pt0->mode != MODE_MUTABLE &&
                        parg->kind != EXPR_MUT) {
                        bool gl; Id *sh;
                        if (sema_readonly_root(parg, &gl, &sh)) parg->ro_root = true;
                    }
                }
            }
            param_idx++;
        }
    } else if (callee_decl && callee_decl->kind == DECL_STRUCT) {
        // Validate struct constructor arguments
        DeclList *fields = callee_decl->as.struct_decl.fields;
        ExprList *args = e->as.call_expr.args;
        int field_count = 0;
        int arg_count = 0;
        
        DeclList *f = fields;
        ExprList *a = args;
        
        while (f && a) {
            // F-022 fix: verify argument type matches field type.
            if (f->decl && f->decl->kind == DECL_VARIABLE && a->expr) {
                Type *field_ty = f->decl->as.variable_decl.type;
                Type *arg_ty = a->expr->type;
                // Integer literals are polymorphic across integer types:
                // skip the widen check when the arg is a literal assigned to
                // an integer field (the literal value is trusted to fit).
                bool literal_to_int =
                    a->expr->kind == EXPR_LITERAL &&
                    field_ty && is_integer_type(field_ty);
                if (literal_to_int) {
                    // P2/S3: a literal assigned to an integer field must fit the
                    // field's type. Previously skipped entirely — a real overflow
                    // gap (e.g. S(300) with a u8 field compiled silently).
                    long long lit = a->expr->as.literal_expr.value;
                    Range r = { lit, lit, true };
                    Id *fn2 = f->decl->as.variable_decl.name;
                    char lbuf[160];
                    int ln2 = fn2 ? (int)fn2->length : 0;
                    if (ln2 > 159) ln2 = 159;
                    if (ln2) memcpy(lbuf, fn2->name, ln2);
                    lbuf[ln2] = '\0';
                } else if (field_ty && arg_ty && sema_walk_phase) {
                    // THE BOUNDARY RELATION every other boundary uses (check_conversion). A
                    // private `types_compatible` test here refused `Lexer(text, 0)` for a
                    // `u8[:0]` text and a `u8[]` field — a conversion every argument, declaration
                    // and assignment accepts — while the IR then mis-built the array case it
                    // did accept. Walk phase only, as for arguments: ranges are not built yet
                    // during resolve.
                    Id *fname = f->decl->as.variable_decl.name;
                    char fl[160]; int fln = fname ? (int)fname->length : 0;
                    if (fln > 159) fln = 159;
                    if (fln) memcpy(fl, fname->name, fln);
                    fl[fln] = '\0';
                    Range r = sema_ranges ? sema_eval_range(a->expr, sema_ranges) : range_unknown();
                    check_conversion(arg_ty, field_ty, r, a->expr, a->expr->line, a->expr->col,
                                     "struct field", fl);
                }
                // Q-002 Phase 5: overflow-at-boundary (struct field init).
                if (sema_walk_phase && sema_ranges && field_ty
                    && a->expr->kind != EXPR_LITERAL) {
                    Range r = sema_eval_range(a->expr, sema_ranges);
                    Id *fname = f->decl->as.variable_decl.name;
                    char buf[160];
                    int n = fname ? (int)fname->length : 0;
                    if (n > 159) n = 159;
                    if (n) memcpy(buf, fname->name, n);
                    buf[n] = '\0';
                }
            }
            f = f->next;
            a = a->next;
            field_count++;
            arg_count++;
        }
        
        while (f) { field_count++; f = f->next; }
        while (a) { arg_count++; a = a->next; }
        
        if (arg_count < field_count) {
            fprintf(stderr, "[E012] Error Ln %li, Col %li: Partial initialization of struct '%.*s'. Expected %d arguments, got %d.\n",
                    e->line, e->col,
                    (int)callee_decl->as.struct_decl.name->length, callee_decl->as.struct_decl.name->name,
                    field_count, arg_count);
            diagnostic_show_line(e->line, e->col);
            exit(1);
        } else if (arg_count > field_count) {
            fprintf(stderr, "[E012] Error Ln %li, Col %li: Too many arguments for struct '%.*s'. Expected %d, got %d.\n",
                    e->line, e->col,
                    (int)callee_decl->as.struct_decl.name->length, callee_decl->as.struct_decl.name->name,
                    field_count, arg_count);
            diagnostic_show_line(e->line, e->col);
            exit(1);
        }
    }
    // function-call expression type is the callee's type (return type)
    if (callee_decl && callee_decl->kind == DECL_STRUCT) {
        e->type = e->as.call_expr.callee->as.type_expr.type_value;
    } else if (callee_decl && callee_decl->kind == DECL_ENUM) {
        if (e->as.call_expr.callee->kind == EXPR_MEMBER) {
            e->type = e->as.call_expr.callee->as.member_expr.target->as.type_expr.type_value;
        } else {
            e->type = e->as.call_expr.callee->type;
        }
    } else {
        // A function's symbol type is its RETURN type as declared, captured before the
        // declaration's type applications were resolved: `func f() Option(i32)` reached the
        // caller as the GENERIC `Option`, so `var o = f()` declared an `Option` and assigned it an
        // `Option_i32` — C that gcc rejects. Only annotated bindings ever worked. Resolve here.
        Type *rt = e->as.call_expr.callee->type;
        e->type = (rt && rt->kind == TYPE_SIMPLE && rt->type_args) ? mono_resolve_type_apps_at(rt, e->line, e->col) : rt;
    }
    break;
  }

  case EXPR_BINARY:
    sema_infer_expr(e->as.binary_expr.left);

    // For 'and' chains: if LHS is/contains 'in', push in-guard before evaluating RHS
    // This allows: while l.pos in l.src and l.src[l.pos] != '"'
    if (e->as.binary_expr.op == TOKEN_KEYWORD_AND) {
        InGuardEntry *old_guards = sema_in_guards;
        sema_push_in_guards(e->as.binary_expr.left);
        // Within-`&&` flow: also apply the LHS's relational constraints (e.g. the
        // `j < n` in `j < n and src[j]`) before proving the RHS. Short-circuit &&
        // guarantees the LHS holds whenever the RHS is evaluated, so the bounds
        // prover may chain `j < n` with n's range to prove `src[j]` — no in-guard
        // needed. SCOPED: the constraints/ranges the LHS adds are restored right
        // after, so they never leak past this condition (soundness — a stale
        // `j < n` outside the `&&` must not keep proving anything).
        ConstraintEntry *old_cons = sema_ranges ? sema_ranges->constraints : NULL;
        RangeEntry      *old_head = sema_ranges ? sema_ranges->head        : NULL;
        // Enable member-path length keys ONLY here: they're created for the LHS
        // (`i < l.src.len`), used to prove the RHS read (`l.src[i]`), then dropped
        // with the constraint-list restore below — so a struct-field slice length
        // can never persist past this read and go stale (sound by construction).
        bool old_mk = sema_mk_scoped; sema_mk_scoped = true;
        if (sema_ranges) sema_apply_constraint(e->as.binary_expr.left, sema_ranges);
        sema_infer_expr(e->as.binary_expr.right);
        sema_mk_scoped = old_mk;
        if (sema_ranges) {
            sema_ranges->constraints = old_cons;
            sema_ranges->head        = old_head;
        }
        sema_in_guards = old_guards;
    } else {
        sema_infer_expr(e->as.binary_expr.right);
    }

    // `i in c` is an INDEX test, `0 <= i < c.len` (lowering compares against the length), and it
    // had no typing rule at all (I.75, Handwriting): `x in [1, 3, 5]` compiled and meant `x < 3`,
    // the literal's length, not membership (wrong for every member but 1); `x in n` with an
    // integer `n` and `"x" in a` emitted C gcc rejects; `true in a` compiled. The left side is an
    // integer and the right a container whose length bounds it; an array LITERAL on the right is
    // refused with what it would have meant.
    if (e->as.binary_expr.op == TOKEN_KEYWORD_IN) {
        e->type = get_builtin_bool_type();
        Expr *L = e->as.binary_expr.left, *R = e->as.binary_expr.right;
        Type *lt = L ? L->type : NULL, *rt = R ? R->type : NULL;
        if (lt && lt->kind == TYPE_SIMPLE) { Type *a = resolve_type_alias(lt); if (a) lt = a; }
        if (rt && rt->kind == TYPE_SIMPLE) {
            Type *a = resolve_type_alias(rt);
            if (a && (a->kind == TYPE_ARRAY || a->kind == TYPE_SLICE)) rt = a;
        }
        long ln = (long)(R && R->line ? R->line : e->line), cl = (long)(R && R->line ? R->col : e->col);
        // `x in lo..hi`: membership in a range of integers (I.77). Its bounds are integers.
        if (R && R->kind == EXPR_RANGE) {
            Expr *ends[2] = { R->as.range_expr.start, R->as.range_expr.end };
            for (int k = 0; k < 2; k++) {
                Expr *b = ends[k];
                if (!b) continue;
                sema_infer_expr(b);
                Type *bt = b->type;
                if (bt && bt->kind == TYPE_SIMPLE) { Type *a = resolve_type_alias(bt); if (a) bt = a; }
                if (bt && !is_integer_type(bt)) {
                    char tb[128]; type_describe(bt, tb, sizeof tb);
                    long bl = (long)(b->line ? b->line : ln), bc = (long)(b->line ? b->col : cl);
                    fprintf(stderr, "[E012] Error Ln %li, Col %li: a range's bounds are integers; this "
                            "one is '%s'.\n", bl, bc, tb);
                    diagnostic_show_line(bl, bc);
                    exit(1);
                }
            }
            if (lt && !is_integer_type(lt)) {
                char tb[128]; type_describe(lt, tb, sizeof tb);
                long l2 = (long)(L->line ? L->line : e->line), c2 = (long)(L->line ? L->col : e->col);
                fprintf(stderr, "[E012] Error Ln %li, Col %li: `x in lo..hi` tests an integer against a "
                        "range of integers; the left side is '%s'.\n", l2, c2, tb);
                diagnostic_show_line(l2, c2);
                exit(1);
            }
            break;
        }
        // ★ MEMBERSHIP IN A LIST OF CONSTANTS (DECIDE-X, I.79): `c in [' ', 9, 10, 13]`. The set is
        // known when the program is compiled, so the test is a chain of comparisons (gcc turns it
        // into one bitmask test) and needs no table and no scan. A list built at run time would be
        // an O(n) scan behind an operator; that is a function the program writes, so it is refused.
        if (R && R->kind == EXPR_ARRAY_COMPREHENSION) {
            fprintf(stderr, "[E012] Error Ln %li, Col %li: membership is tested against a list of "
                    "constants written out, `x in [a, b, c]`; a comprehension is built at run time.\n", ln, cl);
            diagnostic_show_line(ln, cl);
            exit(1);
        }
        if (R && R->kind == EXPR_ARRAY_LITERAL) {
            if (lt && !is_integer_type(lt)) {
                char tb[128]; type_describe(lt, tb, sizeof tb);
                long l2 = (long)(L->line ? L->line : e->line), c2 = (long)(L->line ? L->col : e->col);
                fprintf(stderr, "[E012] Error Ln %li, Col %li: `x in [a, b, c]` tests an integer against a "
                        "list of integers; the left side is '%s'.\n", l2, c2, tb);
                diagnostic_show_line(l2, c2);
                exit(1);
            }
            for (ExprList *el = R->as.array_literal_expr.elements; el; el = el->next) {
                Expr *x = el->expr;
                long l3 = (long)(x && x->line ? x->line : ln), c3 = (long)(x && x->line ? x->col : cl);
                if (!tc_closed_const(x, 0)) {
                    fprintf(stderr, "[E012] Error Ln %li, Col %li: each element of a membership list is a "
                            "constant (numbers and characters, and arithmetic on them); for a value known "
                            "only at run time, compare: `x == a or x == b`.\n", l3, c3);
                    diagnostic_show_line(l3, c3);
                    exit(1);
                }
                sema_infer_expr(x);
            }
            break;
        }
        // ★ `i in a` AS AN INDEX TEST IS RETIRED (DECIDE-X, I.78). `in` means membership in a set, and
        // `i in a` read as `i < a.len` while every other language reads it as "i is one of a's
        // elements". Both meanings compile, so the index test could not simply change meaning: it is
        // refused, with the replacement, and only after the corpus has migrated does `x in a` become
        // element membership (I.79). A container's indices are the range `0..a.len`.
        if (rt && (rt->kind == TYPE_ARRAY || rt->kind == TYPE_SLICE)) {
            char ln_s[96] = "i", rn_s[96] = "a";     // the program's own names when they are paths
            tc_dotted_path(L, ln_s, sizeof ln_s);
            tc_dotted_path(R, rn_s, sizeof rn_s);
            long l2 = (long)(e->line ? e->line : ln), c2 = (long)(e->line ? e->col : cl);
            fprintf(stderr, "[E100] Error Ln %li, Col %li: `%s in %s` as an index test was retired: `in` "
                    "means membership in a set, and %s's indices are the range `0..%s.len`. Write "
                    "`%s in 0..%s.len` (or `%s < %s.len`). Membership in a container's ELEMENTS is "
                    "a scan, which is a function you write; `in` takes a list of constants "
                    "(`x in [a, b]`).\n", l2, c2, ln_s, rn_s, rn_s, rn_s, ln_s, rn_s, ln_s, rn_s);
            diagnostic_show_line(l2, c2);
            exit(1);
        }
        {
            char tb[128] = "?"; if (rt) type_describe(rt, tb, sizeof tb);
            fprintf(stderr, "[E012] Error Ln %li, Col %li: `in` tests membership in a range `lo..hi`; "
                    "the right side here is '%s'.\n", ln, cl, tb);
            diagnostic_show_line(ln, cl);
            exit(1);
        }
    }

    // Pointer arithmetic: ptr ± integer → ptr (same type)
    {
        Type *lt = e->as.binary_expr.left  ? e->as.binary_expr.left->type  : NULL;
        Type *rt = e->as.binary_expr.right ? e->as.binary_expr.right->type : NULL;
        TokenKind op = e->as.binary_expr.op;
        if (lt && lt->kind == TYPE_POINTER &&
            (op == TOKEN_PLUS || op == TOKEN_MINUS) &&
            rt && rt->kind == TYPE_SIMPLE) {
            // ptr ± integer → same pointer type
            e->type = lt;
            break;
        }
        // Pointer subtraction: ptr - ptr → usize (offset between two pointers in same array)
        if (lt && lt->kind == TYPE_POINTER &&
            rt && rt->kind == TYPE_POINTER &&
            op == TOKEN_MINUS) {
            // Build usize type
            Id *usize_id = id(sema_arena, 5, "usize");
            e->type = type_simple(sema_arena, usize_id);
            break;
        }
        // SIMD: an elementwise op on vectors yields a vector of the same shape (a
        // comparison yields a same-width lane mask). Take the vector operand's
        // type so an inline `d == s` is itself a Vec(N,u8) that can feed
        // @movemask. Vector arithmetic is inherently lane-wise wrapping, so this
        // also (correctly) bypasses the scalar overflow check below.
        {
            Type *ltv = lt, *rtv = rt;
            while (ltv && ltv->kind == TYPE_COMPTIME) ltv = ltv->element_type;
            while (rtv && rtv->kind == TYPE_COMPTIME) rtv = rtv->element_type;
            Type *vt = (ltv && ltv->kind == TYPE_VECTOR) ? ltv
                     : (rtv && rtv->kind == TYPE_VECTOR) ? rtv : NULL;
            if (vt) {
                // A comparison of FLOAT lanes is a mask of INTEGER lanes of the same width
                // (an f32x4 comparison is an i32x4, all ones where it held): a float with every
                // bit set is a NaN, not a mask. Typed as the float vector, the mask's slot was
                // `float` lanes while the comparison produced `int` ones, and gcc refused the C.
                if (is_comparison_op(op) && vt->element_type && is_float_type(vt->element_type)) {
                    bool wide = memcmp(vt->element_type->base_type->name, "f64", 3) == 0;
                    e->type = type_vector(sema_arena, vt->array_len,
                                          type_simple(sema_arena, id(sema_arena, 3, wide ? "i64" : "i32")));
                    break;
                }
                e->type = vt; break;
            }
        }
    }




    // Struct equality check: == and != on struct/enum types is a compile error (§8.8)
    {
        TokenKind op = e->as.binary_expr.op;
        if (op == TOKEN_EQUAL_EQUAL || op == TOKEN_BANG_EQUAL) {
            Type *lt = e->as.binary_expr.left->type;
            // Check if either side is a struct type
            if (lt && lt->kind == TYPE_SIMPLE && lt->base_type) {
                char lbuf[256];
                int ll = lt->base_type->length < 255 ? lt->base_type->length : 255;
                memcpy(lbuf, lt->base_type->name, ll);
                lbuf[ll] = '\0';
                Symbol *lsym = sema_lookup(lbuf);
                if (lsym && lsym->decl && lsym->decl->kind == DECL_ENUM && lsym->decl->as.enum_decl.is_union) {
                    // A union, as written (its synthesized name is the compiler's), and the test that
                    // applies to it (I.103: `NotFound == 0` compared the niche).
                    char ub[256]; type_describe(lt, ub, sizeof ub);
                    fprintf(stderr, "[E012] Error Ln %li, Col %li: cannot use '%s' on the union '%s'. "
                            "Match it with `case` instead.\n", (long)e->line, (long)e->col,
                            op == TOKEN_EQUAL_EQUAL ? "==" : "!=", ub);
                    diagnostic_show_line(e->line, e->col);
                    exit(1);
                }
                if (lsym && lsym->decl && (lsym->decl->kind == DECL_STRUCT || lsym->decl->kind == DECL_ENUM)) {
                    fprintf(stderr, "[E012] Error Ln %li, Col %li: cannot use '%s' on struct/enum type '%s'. "
                            "Implement an 'equals' method and use it instead.\n",
                            (long)e->line, (long)e->col,
                            op == TOKEN_EQUAL_EQUAL ? "==" : "!=", lbuf);
                    diagnostic_show_line(e->line, e->col);
                    exit(1);
                }
            }
        }
    }

    {
        TokenKind bop = e->as.binary_expr.op;
        // Reject any operator on nominal aggregates (struct/enum). The ==/!=
        // case is handled just above with a tailored message; everything else
        // (arithmetic, bitwise, shift, relational) has no meaning on an
        // aggregate and would silently default the result to i32 and emit
        // broken C (`a + b` on two structs). Users must implement a method.
        {
            Type *alt = e->as.binary_expr.left->type;
            Type *art = e->as.binary_expr.right->type;
            bool l_agg = is_nominal_aggregate(alt) ||
                         (alt && (alt->kind == TYPE_ARRAY || alt->kind == TYPE_SLICE));
            bool r_agg = is_nominal_aggregate(art) ||
                         (art && (art->kind == TYPE_ARRAY || art->kind == TYPE_SLICE));
            if (l_agg || r_agg) {
                // Exception: slice/string ==/!= against a string LITERAL is a
                // content comparison lowered to a length check + memcmp in codegen.
                bool eqop = (bop == TOKEN_EQUAL_EQUAL || bop == TOKEN_BANG_EQUAL);
                bool str_lit_cmp = eqop &&
                    (e->as.binary_expr.left->kind == EXPR_STRING ||
                     e->as.binary_expr.right->kind == EXPR_STRING);
                if (!str_lit_cmp) {
                    const char *what = (is_nominal_aggregate(alt) || is_nominal_aggregate(art))
                                       ? "struct/enum" : "array/slice";
                    // ★ TELL THE TRUTH ABOUT THE SHAPE IN FRONT OF YOU. `a i32[4] >= 0 and <= 1000`
                    // reaches here as an ordinary comparison, and "implement a method instead" is
                    // advice for a different mistake entirely — the programmer is trying to bound the
                    // ELEMENTS, which the language cannot express. Saying so is worth more than a
                    // hint that cannot be followed.
                    bool elem_refine_attempt =
                        !eqop && (alt && (alt->kind == TYPE_ARRAY || alt->kind == TYPE_SLICE)) &&
                        e->as.binary_expr.right &&
                        e->as.binary_expr.right->kind == EXPR_LITERAL;
                    fprintf(stderr, "[E012] Error Ln %li, Col %li: operator '%s' is not "
                        "defined on %s types%s.%s\n",
                        (long)e->line, (long)e->col, token_kind_to_str(bop), what,
                        eqop ? " (compare a slice against a string literal, or write a helper)" : "",
                        elem_refine_attempt
                          ? "\n       A refinement here would bound the ARRAY, and there is no element"
                            "\n       refinement: write the bound on the element TYPE (`a u8[64]`, or a"
                            "\n       refined alias) or check the values where they are produced."
                          : " Implement a method instead.");
                    diagnostic_show_line(e->line, e->col);
                    exit(1);
                }
            }
        }
        bool is_cmp = (bop == TOKEN_EQUAL_EQUAL || bop == TOKEN_BANG_EQUAL ||
            bop == TOKEN_ANGLE_BRACKET_LEFT || bop == TOKEN_ANGLE_BRACKET_LEFT_EQUAL ||
            bop == TOKEN_ANGLE_BRACKET_RIGHT || bop == TOKEN_ANGLE_BRACKET_RIGHT_EQUAL);
        if (is_cmp) {
            check_comparison_operands(e->as.binary_expr.left->type,
                e->as.binary_expr.right->type, e->as.binary_expr.left,
                e->as.binary_expr.right, token_kind_to_str(bop), e->line, e->col);
        }
        if (bop == TOKEN_KEYWORD_AND || bop == TOKEN_KEYWORD_OR) {
            // spec 08: the operands of `and` and `or` shall have type bool, inside `unsafe` too
            // (it licenses memory operations, not implicit conversions; no corpus program used it).
            for (int side = 0; side < 2; side++) {
                Expr *o = side ? e->as.binary_expr.right : e->as.binary_expr.left;
                if (o && o->type && !is_bool_type(o->type)) {
                    char tb[128]; type_describe(o->type, tb, sizeof tb);
                    fprintf(stderr, "[E012] Error Ln %li, Col %li: an operand of `%s` has type '%s', not `bool` "
                            "(spec 08) — compare it: `n != 0`.\n", (long)o->line, (long)o->col,
                            bop == TOKEN_KEYWORD_AND ? "and" : "or", tb);
                    diagnostic_show_line(o->line, o->col);
                    exit(1);
                }
            }
        }
        if (is_cmp || bop == TOKEN_KEYWORD_AND || bop == TOKEN_KEYWORD_OR) {
            e->type = get_builtin_bool_type();
        } else if (is_bool_type(e->as.binary_expr.left->type) || is_bool_type(e->as.binary_expr.right->type)) {
            // Arithmetic — and bitwise, whose operands spec 08 requires to be integers — on a
            // boolean is an implicit bool→integer conversion (spec 07). `and` / `or` are the
            // boolean operators. Inside `unsafe` this silently TYPED the bool as an i32: the
            // type checker performing the conversion spec 07 forbids, in a block whose licence
            // is memory. No corpus program relied on it (Handwriting, M11).
            {
                fprintf(stderr, "[E012] Error Ln %li, Col %li: `%s` on a `bool` operand — a boolean is not an "
                        "integer (spec 07); use `and` / `or`, or convert explicitly: `(a < b) as i32`.\n",
                        (long)e->line, (long)e->col, token_kind_to_str(bop));
                diagnostic_show_line(e->line, e->col);
                exit(1);
            }
        } else {
            Type *lt = e->as.binary_expr.left->type;
            Type *rt = e->as.binary_expr.right->type;
            // spec 08: bitwise operators take integers, and `%` is defined on integers only. On
            // an f64 both were accepted: `a & a` emitted C that gcc rejects, and `a % b` was
            // refused only as a division whose divisor might be zero (E015), not for what it is.
            {
                TokenKind bk = e->as.binary_expr.op;
                bool bitwise = bk == TOKEN_AMPERSAND || bk == TOKEN_PIPE || bk == TOKEN_CARET ||
                               bk == TOKEN_SHIFT_LEFT || bk == TOKEN_SHIFT_RIGHT ||
                               bk == TOKEN_SHIFT_LEFT_PERCENT;
                Type *fl = (lt && is_float_type(resolve_type_alias(lt))) ? lt
                         : (rt && is_float_type(resolve_type_alias(rt))) ? rt : NULL;
                if ((bitwise || bk == TOKEN_PERCENT) && fl) {
                    char tb[128]; type_describe(fl, tb, sizeof tb);
                    fprintf(stderr, "[E012] Error Ln %li, Col %li: `%s` on a `%s` operand: %s (spec 08).\n",
                            (long)e->line, (long)e->col, token_kind_to_str(bk), tb,
                            bitwise ? "bitwise operators take integers" : "`%` is defined on integers only");
                    diagnostic_show_line(e->line, e->col);
                    exit(1);
                }
            }
            // Q-002 simplified (post Phase 4 rollback): result type follows
            // Sprint 10 widening — max(rank(lt), rank(rt)). Wrap/sat ops
            // keep the LHS type unchanged (the operation bounds the result
            // to that type). Overflow on plain ops is caught at the
            // assignment boundary by Phase 5 (E086).
            TokenKind aop = e->as.binary_expr.op;
            // Ops with an EXPLICIT overflow policy keep the operand type (they do
            // not Path-F-widen): wrapping (%), saturating (|), and checked (?).
            bool is_wrap_or_sat = (aop == TOKEN_PLUS_PERCENT  || aop == TOKEN_MINUS_PERCENT
                                || aop == TOKEN_ASTERISK_PERCENT || aop == TOKEN_PLUS_PIPE
                                || aop == TOKEN_MINUS_PIPE || aop == TOKEN_ASTERISK_PIPE
                                || aop == TOKEN_PLUS_QUESTION || aop == TOKEN_MINUS_QUESTION
                                || aop == TOKEN_ASTERISK_QUESTION
                                || aop == TOKEN_SLASH_PERCENT || aop == TOKEN_SLASH_PIPE
                                || aop == TOKEN_SHIFT_LEFT_PERCENT);
            if (is_wrap_or_sat && lt && is_integer_type(lt)) {
                // I.56: the operation has ONE integer type, the operands'. It took the LEFT one, so
                // a literal on the left made it an i32: `100 +% x` on an i64 wrapped at 2^32 (79,
                // not -9223372036854775729) and `100 +| x` on a u32 clamped at 2147483647, while the
                // C and the interpreter agreed (the IR was typed wrong, so no oracle over it could
                // see it). And of two typed operands the left won and the right was truncated in
                // silence, so `a +| b` and `b +| a` differed. Now (spec 07): a literal or plain
                // arithmetic takes the other operand's type, on either side; of two stated types,
                // the one that holds every value of the other; neither is E012. A shift's amount is
                // a count, not an operand.
                Expr *le = e->as.binary_expr.left, *re = e->as.binary_expr.right;
                Type *opty = lt;
                if (aop != TOKEN_SHIFT_LEFT_PERCENT && rt && is_integer_type(resolve_type_alias(rt))) {
                    // A literal, or plain arithmetic whose wide type the compiler chose, takes the
                    // other operand's type, and the policy applies to its exact value: `s +% x * y`
                    // on i32s wraps in i32 although `x * y` is an i64. Of two stated types, the one
                    // that holds every value of the other; neither is E012.
                    bool lfree = sema_int_literal_operand(le), rfree = sema_int_literal_operand(re);
                    if (!lfree && !rfree) { lfree = sema_widened_operand(le); rfree = sema_widened_operand(re); }
                    int lb = 0, rb = 0; bool ls = false, rs = false;
                    if (lfree && !rfree) opty = rt;
                    else if (!lfree && !rfree && sema_int_shape(lt, &lb, &ls) && sema_int_shape(rt, &rb, &rs) &&
                             (lb != rb || ls != rs)) {
                        if (sema_int_shape_holds(lb, ls, rb, rs)) opty = lt;
                        else if (sema_int_shape_holds(rb, rs, lb, ls)) opty = rt;
                        else {
                            char ta[128], tb[128]; type_describe(lt, ta, sizeof ta); type_describe(rt, tb, sizeof tb);
                            const char *os = token_kind_to_str(aop);
                            fprintf(stderr, "[E012] Error Ln %li, Col %li: `%s` takes its operands in one integer type, "
                                    "and of `%s` and `%s` neither holds every value of the other: convert one, e.g. "
                                    "`(a as %s) %s b`.\n", (long)e->line, (long)e->col, os, ta, tb, tb, os);
                            diagnostic_show_line(e->line, e->col);
                            exit(1);
                        }
                    }
                }
                e->type = opty;
                // Q1: a checked op (`+?`/`-?`/`*?`) must be handled inline by `else`
                // — a bare one would abort implicitly, and abort is explicit-only.
                // The enclosing `else` sets checked_ok before this runs.
                if (sema_walk_phase && !e->checked_ok &&
                    (aop == TOKEN_PLUS_QUESTION || aop == TOKEN_MINUS_QUESTION || aop == TOKEN_ASTERISK_QUESTION)) {
                    fprintf(stderr, "[E067] Error Ln %li, Col %li: a checked operator (`+?`/`-?`/`*?`) must be "
                            "handled with `else` — e.g. `a +? b else panic(\"overflow\")` or `else <fallback>`. "
                            "A bare checked op has no overflow policy.\n", (long)e->line, (long)e->col);
                    diagnostic_show_line(e->line, e->col);
                    exit(1);
                }
            } else if (aop == TOKEN_SLASH && lt && rt && is_integer_type(resolve_type_alias(lt)) &&
                       is_integer_type(resolve_type_alias(rt))) {
                // a division by an alias of an integer widens like one (path_f_result_type)
                e->type = path_f_result_type(lt, rt, aop);
            } else if (lt && rt && is_integer_type(lt) && is_integer_type(rt)) {
                // F3.5 Path-F: +,-,* widen to a type that holds the result (no
                // op-level overflow); other ops (/, %, &, |, ^, <<, >>) keep the
                // max-operand rule (their result already fits the wider operand).
                if (aop == TOKEN_SLASH) {
                    e->type = path_f_result_type(lt, rt, aop);   // signed: one bit wider
                } else if (aop == TOKEN_PLUS || aop == TOKEN_MINUS || aop == TOKEN_ASTERISK) {
                    e->type = path_f_result_type(lt, rt, aop);
                    // Keystone rung 2: propagate the interval through +,-,* by
                    // interval arithmetic on the operands' intervals, so the result
                    // carries a tight on-type range for the next boundary. The
                    // result type is fresh (Path-F), so setting refine is safe.
                    Refinement li = type_interval(lt), ri = type_interval(rt);
                    if (li.known && ri.known && e->type && !e->type->refine.known) {
                        Range a = range_make(li.lo, li.hi), b = range_make(ri.lo, ri.hi);
                        Range res = (aop == TOKEN_PLUS)  ? range_add(a, b)
                                  : (aop == TOKEN_MINUS) ? range_sub(a, b)
                                                         : range_mul(a, b);
                        if (res.known) {
                            e->type->refine.known = true;
                            e->type->refine.lo = res.min;
                            e->type->refine.hi = res.max;
                        }
                    }
                } else if (aop == TOKEN_SHIFT_LEFT || aop == TOKEN_SHIFT_RIGHT) {
                    // ★ A SHIFT HAS ITS LEFT OPERAND'S TYPE (spec 08, bitwise operators), whatever
                    // the amount's. It took the WIDER operand's, the rule for & | ^, so an i8 `x`
                    // shifted by a u32 `k` was computed as a u32: -3 << 3 was 4294967272, not
                    // -24, and the interpreter and the C agreed on the wrong value. The amount
                    // only counts positions.
                    e->type = lt;
                    // ★ ...and a LITERAL shifted left by a constant is typed by its VALUE, as a
                    // constant sum already is (`200 + 100` is an i33). `1 << 40` shifted an i32
                    // literal by 40 and was refused wherever it was a value (E086), while a module
                    // assert, whose evaluator computes over the integers, accepted
                    // `(1 << 40) == 1099511627776`: one expression, two meanings (Handwriting's M1,
                    // plan I.20). The literal and the shift take the narrowest of i64 and u64 that
                    // holds the value, so `1 << 40` is an i64 2^40 in an assert and in code alike.
                    Expr *L = e->as.binary_expr.left, *R = e->as.binary_expr.right;
                    bool lay = false; __int128 lv = 0, rv = 0; int lbits = 0; bool lsgn = false;
                    if (aop == TOKEN_SHIFT_LEFT && L && L->kind == EXPR_LITERAL && !L->as.literal_expr.is_bool
                        && R && sa_is_const(R, &lay) && !lay && sa_eval(R, &rv) && rv >= 0 && rv < 64
                        && sa_eval(L, &lv) && lv >= 0 && parse_iN_uN(lt, &lbits, &lsgn) && lbits >= 1 && lbits <= 64) {
                        __int128 v = lv << (int)rv;
                        __int128 tmax = lsgn ? (((__int128)1 << (lbits - 1)) - 1) : ((((__int128)1) << lbits) - 1);
                        if (v > tmax && v <= (__int128)UINT64_MAX) {
                            Type *base = v <= (__int128)INT64_MAX ? get_builtin_i64_type() : sa_u64_type();
                            Type *lt2 = arena_push_aligned(sema_arena, Type); *lt2 = *base;
                            lt2->refine.known = true; lt2->refine.lo = (int64_t)lv; lt2->refine.hi = (int64_t)lv;
                            L->type = lt2;
                            Type *et = arena_push_aligned(sema_arena, Type); *et = *base;
                            if (v <= (__int128)INT64_MAX) { et->refine.known = true; et->refine.lo = et->refine.hi = (int64_t)v; }
                            e->type = et;
                        }
                    }
                } else
                    e->type = wider_integer_type(lt, rt);
            } else {
                bool l_flt = lt && is_float_type(lt), r_flt = rt && is_float_type(rt);
                bool l_int = lt && is_integer_type(lt), r_int = rt && is_integer_type(rt);
                if ((l_flt && r_int) || (l_int && r_flt)) {
                    // float <op> int: the integer side must be a literal (which
                    // promotes to the float type); a typed integer operand needs
                    // an explicit `as` cast. Without this the result mis-typed as
                    // the integer, silently truncating (e.g. `x += f` desugars to
                    // `x = x + f`, hiding the float->int loss at the boundary).
                    Expr *int_side = l_int ? e->as.binary_expr.left : e->as.binary_expr.right;
                    if (int_side && int_side->kind == EXPR_LITERAL) {
                        e->type = l_flt ? lt : rt;
                    } else {
                        fprintf(stderr, "[E012] Error Ln %li, Col %li: mixed float/integer "
                            "arithmetic requires an explicit 'as' cast.\n",
                            (long)e->line, (long)e->col);
                        diagnostic_show_line(e->line, e->col);
                        exit(1);
                    }
                } else if (l_flt && r_flt) {
                    // ★ BOTH FLOATS (I.161). The result took the LEFT operand's type, so `x * 2.0` on
                    // an f32 was f32 and `2.0 * x` was f64, and `a * b` with a an f32 and b an f64
                    // was typed f32: the f64 narrowed silently. A float literal takes the other
                    // side's type, as an integer literal does (and is retyped, so an f32 constant
                    // rounds as one); otherwise f32 meets f64 in f64, which holds every f32.
                    Expr *le = e->as.binary_expr.left, *re = e->as.binary_expr.right;
                    while (le && le->kind == EXPR_UNARY && le->as.unary_expr.op == TOKEN_MINUS) le = le->as.unary_expr.right;
                    while (re && re->kind == EXPR_UNARY && re->as.unary_expr.op == TOKEN_MINUS) re = re->as.unary_expr.right;
                    bool llit = le && le->kind == EXPR_FLOAT_LITERAL, rlit = re && re->kind == EXPR_FLOAT_LITERAL;
                    bool l32 = memcmp(resolve_type_alias(lt)->base_type->name, "f32", 3) == 0;
                    bool r32 = memcmp(resolve_type_alias(rt)->base_type->name, "f32", 3) == 0;
                    if (llit && !rlit)      { e->type = rt; sema_float_literal_as(e->as.binary_expr.left, rt); }
                    else if (rlit && !llit) { e->type = lt; sema_float_literal_as(e->as.binary_expr.right, lt); }
                    else if (l32 && !r32)   e->type = rt;
                    else                    e->type = lt;
                } else if (l_flt || r_flt) {
                    e->type = l_flt ? lt : rt;
                } else {
                    e->type = l_int ? lt : (r_int ? rt : get_builtin_i32_type());
                }
            }
        }
    }
    // Overflow prove-or-reject: a plain +, -, * on integers must have a result
    // that provably fits its own type. Wrapping (+%,-%,*%) and saturating
    // (+|,-|,*|) ops define their overflow (their range is already clamped to
    // the type), so they are exempt; `unsafe` opts out. This makes "no silent
    // overflow" the default instead of only checking assignment boundaries —
    // `if a + b > 0` is caught here even with no assignment.
    {
        TokenKind aop2 = e->as.binary_expr.op;
        if ((aop2 == TOKEN_PLUS || aop2 == TOKEN_MINUS || aop2 == TOKEN_ASTERISK) &&
            sema_walk_phase && sema_ranges && !sema_in_unsafe_block &&
            e->type && is_integer_type(e->type)) {
            // C integer promotion: operands narrower than int (i8/i16/u8/u16) are
            // promoted to int for the operation. So the op is computed in `int`,
            // and it must be checked against the INT range — NOT skipped. u16*u16
            // (65535*65535 = 4.29e9) overflows int and is UB even though nothing
            // is stored back to a u16 (the store, if any, is caught separately at
            // the boundary). Checking narrow results against i32 both catches that
            // UB and stays a no-op for u8+u8 (fits int; its narrow store is caught
            // at the assignment boundary). i32/u32 and wider check against self.
            int abits; bool asgn;
            bool narrow = parse_iN_uN(e->type, &abits, &asgn) && abits < 32;
            Type *check_ty = narrow ? get_builtin_i32_type() : e->type;
            Range rr = sema_eval_range(e, sema_ranges);
            // Some parsed sub-expressions (notably arithmetic inside if/while
            // conditions) carry no line info; borrow the left operand's so the
            // diagnostic points somewhere useful instead of "Ln 0".
            long dl = e->line > 0 ? e->line
                      : (e->as.binary_expr.left ? (long)e->as.binary_expr.left->line : e->line);
            long dc = e->line > 0 ? e->col
                      : (e->as.binary_expr.left ? (long)e->as.binary_expr.left->col : e->col);
            // Wide multiplication soundness: range_mul saturates the i64 product,
            // so a genuinely-overflowing wide multiply (u32*u32 up to ~1.8e19)
            // saturates into check_value_fits_type's fail-open window and is
            // MISSED (silent unsigned wrap). Compute the exact product range in
            // 128-bit and check it directly. (i32*i32 is already caught by the
            // i64 range, so this only adds the wide/unsigned cases.)
            if (aop2 == TOKEN_ASTERISK && !e->idx2d_ovf_ok) {
                Range lrg = sema_eval_range(e->as.binary_expr.left, sema_ranges);
                Range rrg = sema_eval_range(e->as.binary_expr.right, sema_ranges);
                // The legacy wide/unsigned product check is DELETED: the sovereign engine refuses
                // both shapes (`u32` 1e5*1e5 and the `u64` 5e9*5e9 case this block was added for),
                // verified with each before removal.
                (void)lrg; (void)rrg;
            }
        }
    }
    break;

  case EXPR_UNARY:
    sema_infer_expr(e->as.unary_expr.right);
    if (e->as.unary_expr.op == TOKEN_ASTERISK) {
        // Dereference
        if (!e->as.unary_expr.right || !e->as.unary_expr.right->type) {
             // If operands are broken, we can't check
             // sema_resolve should have caught typical errors, but to be safe:
             // e->type = get_builtin_i32_type(); // fallback
             // return;
             // Actually let's exit to be consistent with previous panic
             fprintf(stderr, "internal error: deref operand untyped\n");
             exit(70);                          // an internal error, not a refused program
        }
        
        Type *t = e->as.unary_expr.right->type;
        while (t && t->kind == TYPE_COMPTIME) {
            t = t->element_type;
        }
        
        if (t->kind == TYPE_POINTER) {
            e->type = t->element_type;
            if (!sema_in_unsafe_block) {
                fprintf(stderr, "[E060] Error Ln %li, Col %li: dereference of a raw pointer outside an `unsafe` block.\n",
                        (long)e->line, (long)e->col);
                exit(1);
            }
        } else {
            // ★ ONLY A POINTER IS DEREFERENCED (I.123). This typed any other operand `i32` and went
            // on: inside `unsafe`, `x = *true` compiled to a `void` C variable that gcc refused,
            // and so did `*[1, 2]`, `*main`, `**5` and `*"ab"` (Secondary Compiler Worker).
            // `unsafe` licenses memory operations, not a dereference of something that is not an
            // address.
            char tb[128]; type_describe(e->as.unary_expr.right->type, tb, sizeof tb);
            fprintf(stderr, "[E012] Error Ln %li, Col %li: `*` dereferences a pointer, and its operand "
                    "is '%s'.\n", (long)e->line, (long)e->col, tb);
            diagnostic_show_line(e->line, e->col);
            exit(1);
        }
    } else {
        e->type = get_builtin_i32_type();
        if (e->as.unary_expr.op == TOKEN_BANG) {                                      // logical not
            e->type = get_builtin_bool_type();
            Expr *o = e->as.unary_expr.right;                  // spec 08: its operand is a bool
            // Inside `unsafe` too: `unsafe` licenses memory operations, not implicit conversions.
            if (o && o->type && !is_bool_type(o->type)) {
                char tb[128]; type_describe(o->type, tb, sizeof tb);
                fprintf(stderr, "[E012] Error Ln %li, Col %li: `!` applied to '%s', not `bool` (spec 08) — "
                        "compare it: `n == 0`.\n", (long)e->line, (long)e->col, tb);
                diagnostic_show_line(e->line, e->col);
                exit(1);
            }
        }
        // ★ BITWISE COMPLEMENT KEEPS ITS OPERAND'S TYPE. Every non-deref unary landed on
        // i32 here, which is wrong for `~` on any other integer type and wrong in a way
        // that only bites the UNSIGNED ones: `~x` on a uN is (2^N−1)−x, a value of that
        // same uN, but typing it i32 made the range come back NEGATIVE ([−2^31, −1] for a
        // u64 operand), so the narrowing check refused it:
        //     func f(a u64) u64 { return ~a }      // [E086] not provably free of overflow
        // while the identical expression on an iN compiled, because a negative i32 range
        // does fit an i64 slot. The corpus never caught it: bitwise_pass.ln inverts
        // `var a = 12` (signed `int`), and simd_lexer_pass.ln writes
        // `(~@movemask(ws)) & 65535`, where the AND re-bounds the result before anything
        // narrows it. `~` is width-defined, so the operand's type IS the result type.
        //
        // ★ AND SO DOES UNARY MINUS, where the same i32 default was not a false rejection
        // but a silent MISCOMPILE. `func neg(a i64) i64 { return -a }` emitted
        //     int32_t v1;  v1 = -v0;  return v1;
        // so the 64-bit negation was TRUNCATED to 32 bits and sign-extended back:
        // neg(5000000000) returned -705032704 instead of -5000000000, with no diagnostic.
        // The narrow types escaped it only by accident, because the negation-overflow
        // check below refuses `-x` on an iN with N < 32 before codegen ever runs.
        //
        // ★ A FLOAT TOO, for minus (I.154). The float operand fell to the same i32 default, so
        // `x f64 = -y` was E012 "implicit conversion between float and integer" and `y * -1.5`
        // was E012 "mixed float/integer arithmetic", while `0.0 - y` compiled: negating a float
        // could only be spelled as a subtraction.
        //
        // ★ AND ANY OTHER OPERAND IS REFUSED, as `*` refuses a non-pointer. It was typed i32 and
        // went on: `-b` and `~b` on a bool compiled, `~y` on an f64 or an f32x4 emitted C that gcc
        // refused ("wrong type argument to bit-complement"), and so did `-p` on a pointer inside
        // `unsafe`; outside it, `-p`, `-s` on a slice and `-c` on an enum met E086 only because
        // their ranges happened not to fit the i32. `~` complements an integer (spec 08: the
        // operands of the bitwise operators are integers); `-` negates a number.
        if (e->as.unary_expr.op == TOKEN_TILDE || e->as.unary_expr.op == TOKEN_MINUS) {
            bool tilde = e->as.unary_expr.op == TOKEN_TILDE;
            Type *ot = e->as.unary_expr.right ? e->as.unary_expr.right->type : NULL;
            while (ot && ot->kind == TYPE_COMPTIME) ot = ot->element_type;
            Type *rt = resolve_type_alias(ot), *el = rt && rt->kind == TYPE_VECTOR ? rt->element_type : rt;
            if (ot) {
                if (is_integer_type(el) || (!tilde && is_float_type(el))) e->type = ot;
                else {
                    char tb[128]; type_describe(e->as.unary_expr.right->type, tb, sizeof tb);
                    fprintf(stderr, "[E012] Error Ln %li, Col %li: %s, and its operand is '%s'.\n",
                            (long)e->line, (long)e->col,
                            tilde ? "`~` complements an integer" : "`-` negates a number", tb);
                    diagnostic_show_line(e->line, e->col);
                    exit(1);
                }
            }
        }
        // Unary negation overflow: `-x` overflows at the type minimum
        // (e.g. -INT_MIN is not representable). Check against the operand's
        // integer type. (The common abs idiom uses the binary form `0 - x`,
        // which the EXPR_BINARY check above already covers.)
        if (e->as.unary_expr.op == TOKEN_MINUS &&
            sema_walk_phase && sema_ranges && !sema_in_unsafe_block) {
            Type *ot = e->as.unary_expr.right ? e->as.unary_expr.right->type : NULL;
            int nbits; bool nsgn;
            bool narrow = ot && parse_iN_uN(ot, &nbits, &nsgn) && nbits < 32;
            if (ot && is_integer_type(ot) && !narrow) {
                Range rr = sema_eval_range(e, sema_ranges);
                long dl = e->line > 0 ? e->line
                          : (e->as.unary_expr.right ? (long)e->as.unary_expr.right->line : e->line);
                long dc = e->line > 0 ? e->col
                          : (e->as.unary_expr.right ? (long)e->as.unary_expr.right->col : e->col);
            }
        }
    }
    break;

  case EXPR_STRING: {
    // A compile-time fixed-length slice (string literal)
    size_t L = (size_t)e->as.string_expr.length;

    // element type = u8
    Type *elem = get_builtin_u8_type();

    // carve out the slice-type in the arena
    Type *slice_ty = arena_push_aligned(sema_arena, Type);
    slice_ty->kind = TYPE_SLICE;
    slice_ty->mode = MODE_SHARED;  // string literals are shared by default
    slice_ty->element_type = elem;

    // A string literal is a SENTINEL-TERMINATED slice of KNOWN length (Zig's
    // `[N:0]u8`): its backing buffer carries a trailing NUL, so `.data` is safe to
    // hand to a C API that scans for it. The compile-time length L lives in
    // array_len.
    slice_ty->has_sentinel       = true;
    slice_ty->array_len          = (isize)L;

    e->type = slice_ty;
    break;
  }

  case EXPR_ARRAY_LITERAL: {
    ExprList *elems = e->as.array_literal_expr.elements;
    if (!elems) {
        fprintf(stderr, "[E100] Error Ln %li, Col %li: empty array literal\n", e->line, e->col);
        diagnostic_show_line(e->line, e->col);
        exit(1);
    }
    Type *elem_type = NULL;
    isize count = 0;
    for (ExprList *el = elems; el; el = el->next) {
        sema_infer_expr(el->expr);
        if (!elem_type) {
            elem_type = el->expr->type;
        }
        count++;
    }
    e->type = type_array(sema_arena, elem_type, count);
    break;
  }

  case EXPR_ARRAY_COMPREHENSION: {
    // [ body for idx in start..end ] : T[end-start], T = type of body.
    Expr *body  = e->as.array_comprehension_expr.body;
    Expr *range = e->as.array_comprehension_expr.range;
    sema_infer_expr(body);
    sema_infer_expr(range);
    if (!range || range->kind != EXPR_RANGE) {
        fprintf(stderr, "[E100] Error Ln %li, Col %li: array comprehension requires a range `start..end`\n", e->line, e->col);
        diagnostic_show_line(e->line, e->col); exit(1);
    }
    Expr *lo = range->as.range_expr.start, *hi = range->as.range_expr.end;
    // The bounds give the array its LENGTH, so they are compile-time constants: a literal, or a
    // constant expression (`0..N` with `N usize = 64`), folded to its literal here so that
    // everything downstream reads a literal. Anything else is E137; this was an uncoded
    // "sema error" that a harness grepping for codes could not see (Handwriting's M6).
    {
        bool lay = false; __int128 lv = 0, hv = 0;
        bool ok = lo && hi && sa_is_const(lo, &lay) && sa_is_const(hi, &lay) && !lay
                  && sa_eval(lo, &lv) && sa_eval(hi, &hv)
                  && lv >= INT32_MIN && lv <= INT32_MAX && hv >= INT32_MIN && hv <= INT32_MAX;
        if (!ok) {
            fprintf(stderr, "[E137] Error Ln %li, Col %li: an array comprehension's range bounds give the "
                    "array its length, so they must be compile-time constants\n", e->line, e->col);
            diagnostic_show_line(e->line, e->col); exit(1);
        }
        if (lo->kind != EXPR_LITERAL) { Expr *l = expr_literal(sema_arena, (int64_t)lv); l->type = lo->type; l->line = lo->line; l->col = lo->col; range->as.range_expr.start = lo = l; }
        if (hi->kind != EXPR_LITERAL) { Expr *h = expr_literal(sema_arena, (int64_t)hv); h->type = hi->type; h->line = hi->line; h->col = hi->col; range->as.range_expr.end = hi = h; }
    }
    isize n = (isize)hi->as.literal_expr.value - (isize)lo->as.literal_expr.value;
    if (range->as.range_expr.inclusive) n += 1;
    if (n <= 0) {
        fprintf(stderr, "[E100] Error Ln %li, Col %li: array comprehension length must be positive\n", e->line, e->col);
        diagnostic_show_line(e->line, e->col); exit(1);
    }
    e->type = type_array(sema_arena, body->type, n);
    break;
  }

  case EXPR_LITERAL: {
    if (e->as.literal_expr.is_bool) { e->type = get_builtin_bool_type(); break; }
    // Keystone: an integer literal carries its exact value as its type's interval
    // ({ν:i32 | ν=k}), so a boundary can prove `k <: Digit` by subsumption without
    // consulting the name-keyed range table. A fresh copy — never mutate the shared
    // builtin. core_identical still sees plain i32 (refinement stripped).
    int64_t lv = (int64_t)e->as.literal_expr.value;
    // ...and the base is chosen by the value, not assumed: a literal outside i32's range is
    // an i64. Without this the refinement below recorded a value its own type cannot hold.
    Type *base = (lv < INT32_MIN || lv > INT32_MAX) ? get_builtin_i64_type()
                                                    : get_builtin_i32_type();
    Type *lt = arena_push_aligned(sema_arena, Type);
    *lt = *base;
    lt->refine.known = true;
    lt->refine.lo = lt->refine.hi = lv;
    e->type = lt;
    break;
  }

  case EXPR_CHAR:
    e->type = get_builtin_u8_type();
    break;

  case EXPR_FLOAT_LITERAL: {
    // Float literals infer to f64
    static Type *f64_ty = NULL;
    if (!f64_ty) {
      Id *id = arena_push_aligned(sema_arena, Id);
      id->name = "f64";
      id->length = 3;
      f64_ty = type_simple(sema_arena, id);
    }
    e->type = f64_ty;
    break;
  }

  case EXPR_RANGE:
    sema_infer_expr(e->as.range_expr.start);
    sema_infer_expr(e->as.range_expr.end);
    // leave e->type NULL if never used
    break;

  case EXPR_INDEX: {
    sema_infer_expr(e->as.index_expr.target);
    // The 2D flat-index overflow exemption (`a[i*w+j]` over `a[h*w]`) lived here to keep the
    // LEGACY overflow check from firing on `i*w`. That check is the IR's now, and it models
    // the same fact itself — so the exemption went with src/sema/bounds.h.
    // Capture and clear the addr-of flag before inferring the index sub-expression
    // so that nested array accesses within the index are NOT treated as addr-of.
    bool _is_addr_of = sema_addr_of_context;
    sema_addr_of_context = false;
    sema_infer_expr(e->as.index_expr.index);

    Type *t = sema_unwrap_type(e->as.index_expr.target->type);
    if (!t) {
         // Error or just return?
         break;
    }

    if (t->kind == TYPE_ARRAY || t->kind == TYPE_SLICE) {
        if (e->as.index_expr.index->kind == EXPR_RANGE) {
            // Q-003.B: result type carries size_expr = end - start so that
            // subsequent accesses can be VRA-proven without unsafe.
            Expr *rs = e->as.index_expr.index->as.range_expr.start;
            Expr *re = e->as.index_expr.index->as.range_expr.end;
            Expr *len_expr = expr_binary(sema_arena, TOKEN_MINUS, re, rs);
            e->type = type_sized_array(sema_arena, t->element_type, len_expr, TOKEN_EQUAL_EQUAL);
            if (!sema_in_unsafe_block &&           // syntactic (Annex B E087 (1)): stays in the front end
                rs && re &&
                rs->kind == EXPR_LITERAL && re->kind == EXPR_LITERAL) {
                long long sv = (long long)rs->as.literal_expr.value;
                long long ev = (long long)re->as.literal_expr.value;
                if (sv > ev) {
                    fprintf(stderr,
                        "[E087] Error Ln %li, Col %li: sub-slice start (%lld) is "
                        "greater than end (%lld). Range must satisfy start <= end.\n",
                        (long)e->line, (long)e->col, sv, ev);
                    diagnostic_show_line(e->line, e->col);
                    exit(1);
                }
            }
        } else {
            e->type = t->element_type;
        }
        // STATIC BOUNDS CHECK (only during walk phase, skipped in unsafe/in-guarded)
        if (sema_ranges && sema_walk_phase && !sema_in_unsafe_block) {
            // Bounds are the IR's obligation (src/analysis/vra.h). The legacy check that
            // stood here was stood down by `g_vra_suppress_bounds` from 2026-09-17 and its
            // implementation was deleted with src/sema/bounds.h on 2026-09-23.
        }
    } else if (t->kind == TYPE_VECTOR) {
        // SIMD lane access v[i]: result is the element type. The vector has a
        // fixed lane count (array_len); a constant lane outside [0, N) is UB in
        // C, so reject it at compile time.
        e->type = t->element_type;
        Expr *ix = e->as.index_expr.index;
        if (!sema_in_unsafe_block && ix && ix->kind == EXPR_LITERAL) {
            long long li = (long long)ix->as.literal_expr.value;
            if (li < 0 || li >= t->array_len) {
                fprintf(stderr, "[E085] Error Ln %li, Col %li: vector lane %lld is out of "
                        "range for Vec(%ld, ...).\n",
                        (long)e->line, (long)e->col, li, (long)t->array_len);
                diagnostic_show_line(e->line, e->col);
                exit(1);
            }
        }
    } else if (t->kind == TYPE_POINTER) {
        // Pointer indexing syntax `ptr[i]` (unsafe or restricted?)
        // Lain spec says pointers are unsafe. But let's allow indexing if it mimics C.
        // But we have no bounds info.
        e->type = t->element_type;
    } else {
        // ★ This was a printed "sema error" that did NOT stop the compile — the `exit` was commented
        // out — and lowering then indexed a `void*`, C that gcc refuses. (The pointer branch above
        // is unreachable: sema_unwrap_type strips pointers, so `p[i]` on a `*u8` landed here.)
        Type *ot = e->as.index_expr.target->type;
        char tb[96]; type_describe(ot, tb, sizeof tb);
        bool rawp = ot && ot->kind == TYPE_POINTER;
        fprintf(stderr, "[E012] Error Ln %li, Col %li: cannot index a value of type '%s'%s.\n",
                (long)e->line, (long)e->col, tb,
                rawp ? ": a raw pointer has no length. Index an array or a slice, or read through the "
                       "pointer with `*p` (or `@load` inside `unsafe`)" : "");
        diagnostic_show_line(e->line, e->col);
        exit(1);
    }
    break;
  }


  case EXPR_MATCH: {
    sema_infer_expr(e->as.match_expr.value);
    sema_check_case_scrutinee(e->as.match_expr.value, e->line, e->col);
    Type *inferred_type = NULL;
    for (ExprMatchCase *c = e->as.match_expr.cases; c; c = c->next) {
        sema_push_scope();
        for (ExprList *p = c->patterns; p; p = p->next) {
            sema_infer_expr(p->expr);
        }
        sema_check_variant_patterns(e->as.match_expr.value ? e->as.match_expr.value->type : NULL,
                                    c->patterns);
        sema_check_case_pattern_kinds(e->as.match_expr.value, c->patterns);
        sema_infer_expr(c->body);
        sema_pop_scope();
        
        if (!inferred_type && c->body->type) {
            inferred_type = c->body->type;
        } else if (inferred_type && c->body && c->body->type &&
                   !sema_arm_types_agree(inferred_type, c->body->type)) {
            char ta[128], tb[128];
            type_describe(inferred_type, ta, sizeof ta); type_describe(c->body->type, tb, sizeof tb);
            fprintf(stderr, "[E012] Error Ln %li, Col %li: the arms of a `case` have incompatible "
                    "types: '%s' and '%s' (spec 15).\n", (long)c->body->line, (long)c->body->col, ta, tb);
            diagnostic_show_line(c->body->line, c->body->col);
            exit(1);
        }
    }
    
    { ExprList *arms[256]; int n = 0;
      for (ExprMatchCase *c = e->as.match_expr.cases; c && n < 256; c = c->next) arms[n++] = c->patterns;
      sema_check_arms_once(arms, n, e->line, e->col); }
    if (!sema_check_expr_match_exhaustive(e)) {
        sema_report_nonexhaustive_match_expr(e);
        exit(1);
    }
    
    e->type = inferred_type ? inferred_type : get_builtin_i32_type();
    break;
  }

  case EXPR_MOVE:
    sema_infer_expr(e->as.move_expr.expr);
    if (!e->as.move_expr.expr->type) {
        fprintf(stderr, "[E012] Error Ln %li, Col %li: `mov` expects an owned value, but its operand "
                "has no value type.\n", (long)e->line, (long)e->col);
        diagnostic_show_line(e->line, e->col); exit(1);
    }
    e->type = type_move(sema_arena, e->as.move_expr.expr->type);
    break;

  case EXPR_MUT:
    sema_infer_expr(e->as.mut_expr.expr);
    e->type = type_mut(sema_arena, e->as.mut_expr.expr->type);
    if (sema_walk_phase) sema_check_place_writable(e->as.mut_expr.expr, e->line, e->col, "lend as `var`");
    if (sema_walk_phase) sema_check_packed_field_addr(e->as.mut_expr.expr, e->line, e->col, "a `var` reference");
    if (sema_walk_phase && !sema_in_unsafe_block) sema_check_mut_invariant_field(e);
    break;

  case EXPR_CAST: {
    sema_infer_expr(e->as.cast_expr.expr);
    // Q1: a checked cast (`as?`) must be handled inline by `else` — a bare one
    // aborts implicitly on out-of-range, and abort is explicit-only.
    if (sema_walk_phase && !e->checked_ok && e->as.cast_expr.kind == CAST_CHECKED) {
        fprintf(stderr, "[E067] Error Ln %li, Col %li: a checked cast (`as?`) must be handled with "
                "`else` — e.g. `x as? u8 else panic(\"out of range\")` or `else <fallback>`.\n",
                (long)e->line, (long)e->col);
        diagnostic_show_line(e->line, e->col);
        exit(1);
    }
    // F-028: basic cast validity — pointer-related casts require `unsafe`.
    Type *src = e->as.cast_expr.expr ? e->as.cast_expr.expr->type : NULL;
    Type *tgt = e->as.cast_expr.target_type;

    // ★ THE PROVEN TIER MUST ACTUALLY BE PROVEN.
    // spec/chapters/08-expressions.tex, on the four cast tiers:
    //     proven  `as`   narrows only if VRA proves it fits, else E086
    // It did not. `var x i32 = 500; x as u8` compiled and produced 244 — a silent
    // truncation, in a language whose entire claim is that narrowing cannot lose
    // information without saying so. The other three tiers are total and owe nothing:
    // `as?` traps, `as%` wraps, `as|` clamps.
    if (sema_walk_phase && e->as.cast_expr.kind == CAST_PROVEN && !sema_in_unsafe_block &&
        src && tgt && sema_ranges) {
        int sb = 0, tb = 0; bool ss = false, ts = false;
        if (parse_iN_uN(src, &sb, &ss) && parse_iN_uN(tgt, &tb, &ts) && sb > tb) {
            Range r = sema_eval_range(e->as.cast_expr.expr, sema_ranges);
        }
    }
    Type *src_u = src, *tgt_u = tgt;
    while (src_u && src_u->kind == TYPE_COMPTIME) src_u = src_u->element_type;
    while (tgt_u && tgt_u->kind == TYPE_COMPTIME) tgt_u = tgt_u->element_type;
    bool src_is_ptr = src_u && src_u->kind == TYPE_POINTER;
    bool tgt_is_ptr = tgt_u && tgt_u->kind == TYPE_POINTER;
    if ((src_is_ptr || tgt_is_ptr) && !sema_in_unsafe_block) {
        fprintf(stderr, "[E012] Error Ln %li, Col %li: cast involving a raw pointer requires an 'unsafe' block.\n",
                (long)e->line, (long)e->col);
        diagnostic_show_line(e->line, e->col);
        exit(1);
    }
    // Reject incompatible `as` casts between an aggregate (struct/enum/array/
    // slice) and a scalar, or between distinct aggregates — they emit broken C
    // ("aggregate value used where an integer was expected"). `as` is for
    // numeric<->numeric only (raw-pointer casts handled above; same core is a
    // no-op). Refinement aliases resolve to their scalar base first.
    if (!src_is_ptr && !tgt_is_ptr) {
        Type *src_r = resolve_type_alias(src_u);
        Type *tgt_r = resolve_type_alias(tgt_u);
        if (src_r && tgt_r && !core_identical(src_r, tgt_r) &&
            (!is_castable_scalar(src_r) || !is_castable_scalar(tgt_r))) {
            char sb[128], tb[128];
            type_describe(src_r, sb, sizeof sb);
            type_describe(tgt_r, tb, sizeof tb);
            fprintf(stderr, "[E012] Error Ln %li, Col %li: cannot cast '%s' to '%s' with 'as' — "
                "'as' converts between numeric types only.\n",
                (long)e->line, (long)e->col, sb, tb);
            diagnostic_show_line(e->line, e->col);
            exit(1);
        }
    }
    // ★ `as%` WRAPS AN INTEGER'S BITS, and a float has none to wrap (I.157): the conversion
    // truncates the VALUE, undefined in C past the target's ends, and the wrapping tier owes no
    // proof, so `x as% i16` on 40000.0 was `(int16_t)v0`. Nothing about a float says which
    // integer it should wrap to; `as|` and `as?` say what happens at the ends.
    if (e->as.cast_expr.kind == CAST_WRAPPING && !src_is_ptr && !tgt_is_ptr) {
        Type *src_r = resolve_type_alias(src_u), *tgt_r = resolve_type_alias(tgt_u);
        if (src_r && tgt_r && is_float_type(src_r) && is_integer_type(tgt_r)) {
            char sb[128]; type_describe(src_r, sb, sizeof sb);
            fprintf(stderr, "[E012] Error Ln %li, Col %li: `as%%` wraps an integer, and this operand is "
                    "'%s': write `as` (proven to fit), `as|` (clamps; a NaN is 0) or `as?` with `else`.\n",
                    (long)e->line, (long)e->col, sb);
            diagnostic_show_line(e->line, e->col);
            exit(1);
        }
    }
    // ★ A TIER SAYS WHAT HAPPENS AT AN INTEGER TYPE'S ENDS (I.161), and a float has none to
    // name: IEEE 754 defines every conversion to a float (an infinity past its range). `as|`,
    // `as?` and `as%` to f32 or f64 emitted the plain conversion and promised nothing.
    if (e->as.cast_expr.kind != CAST_PROVEN && !src_is_ptr && !tgt_is_ptr) {
        Type *tgt_r = resolve_type_alias(tgt_u);
        if (tgt_r && is_float_type(tgt_r)) {
            char tb[128]; type_describe(tgt_r, tb, sizeof tb);
            const char *tier = e->as.cast_expr.kind == CAST_SATURATING ? "as|"
                             : e->as.cast_expr.kind == CAST_CHECKED ? "as?" : "as%";
            fprintf(stderr, "[E012] Error Ln %li, Col %li: `%s` says what happens at an integer type's "
                    "ends, and '%s' is a float: write `as`.\n", (long)e->line, (long)e->col, tier, tb);
            diagnostic_show_line(e->line, e->col);
            exit(1);
        }
    }
    // type already set at parse time (target_type)
    break;
  }

  case EXPR_ADDR: {
    // &arr[k] — address of an element.
    // Type: TYPE_POINTER(elem_type).
    // Mutability: if arr is mutable (var), the pointer is writable (no const).
    bool _prev_addr_of = sema_addr_of_context;
    sema_addr_of_context = true;
    sema_infer_expr(e->as.addr_expr.expr);
    sema_addr_of_context = _prev_addr_of;
    if (sema_walk_phase) sema_check_packed_field_addr(e->as.addr_expr.expr, e->line, e->col, "`&`");
    Type *inner = e->as.addr_expr.expr ? e->as.addr_expr.expr->type : NULL;
    if (inner) {
        e->type = type_pointer(sema_arena, inner);
        // The address of a place the program may WRITE is a writable `*var T`; of any other
        // place, a read-only `*T`. `&x` was always `*T`, which no check read until `*T` stopped
        // converting to `*var T` silently.
        if (sema_place_writable(e->as.addr_expr.expr)) e->type->pointee_mutable = true;
        // ★ NOT `mode = MODE_MUTABLE`. That was how a writable `&arr[k]` was spelled before
        // `pointee_mutable` (4b08e44), and it outlived it: on a POINTER the mode means "a mutable
        // BORROW" to lowering (ir_lower_borrow_binding_type), so once 1694f66 made `&a[0]` of a
        // `var` slice a `*var u8`, `p = &a[0]` became a reference binding, every read of `p`
        // went one level further, and `*p = 77` was `*(*p) = 77`: C that gcc refuses. A raw
        // pointer value is not a borrow; its writability is `pointee_mutable`, set above.
    }
    break;
  }

  case EXPR_DEREF: {
    // *ptr — dereference a pointer.
    // Safe without unsafe ONLY when ptr is associated with an array via
    // `var p *T in arr` and the in-guard `p in arr` has been pushed.
    // Otherwise requires an unsafe block (same as before).
    sema_infer_expr(e->as.deref_expr.expr);
    Type *ptr_ty = e->as.deref_expr.expr ? e->as.deref_expr.expr->type : NULL;
    if (ptr_ty && ptr_ty->kind == TYPE_POINTER && ptr_ty->element_type) {
        e->type = ptr_ty->element_type;
    }
    // ★ ONLY A POINTER IS DEREFERENCED (I.123). Any other operand left the result untyped and
    // went on: inside `unsafe`, `x = *true` compiled to a `void` C variable that gcc refused, and
    // so did `*[1, 2]`, `*main`, `**5` and `*"ab"` (Secondary Compiler Worker). Outside `unsafe`
    // E060 came first and hid it. `unsafe` licenses memory operations, not a dereference of
    // something that is not an address.
    {   Type *pt = ptr_ty;
        while (pt && pt->kind == TYPE_COMPTIME && pt->element_type) pt = pt->element_type;
        if (pt) pt = resolve_type_alias(pt);
        if (pt && pt->kind != TYPE_POINTER) {
            char tb[128]; type_describe(ptr_ty, tb, sizeof tb);
            fprintf(stderr, "[E012] Error Ln %li, Col %li: `*` dereferences a pointer, and its operand "
                    "is '%s'.\n", (long)e->line, (long)e->col, tb);
            diagnostic_show_line(e->line, e->col);
            exit(1);
        }
    }
    // Safety check only during walk phase (when in-guards are active).
    // During resolve phase, in-guards aren't pushed yet — skip the check.
    if (sema_walk_phase && !sema_in_unsafe_block) {
        bool guarded = false;
        Expr *deref_ptr = e->as.deref_expr.expr;
        // Forward in-guard: `p in arr` or `p in arr` pointer guard
        for (InGuardEntry *ig = sema_in_guards; ig && !guarded; ig = ig->next) {
            if (ig->is_ptr_guard && !ig->is_backward_guard &&
                expr_struct_equal(ig->index, deref_ptr))
                guarded = true;
        }
        // Backward in-guard: `*(p - k)` where k >= 1 and `p > arr` guard exists.
        // Proves p > arr → p >= arr+1 → p-k >= arr (for k=1), so access is safe.
        if (!guarded && deref_ptr && deref_ptr->kind == EXPR_BINARY &&
            deref_ptr->as.binary_expr.op == TOKEN_MINUS) {
            Expr *base   = deref_ptr->as.binary_expr.left;
            Expr *offset = deref_ptr->as.binary_expr.right;
            bool offset_pos = (offset && offset->kind == EXPR_LITERAL &&
                               offset->as.literal_expr.value >= 1);
            if (offset_pos) {
                for (InGuardEntry *ig = sema_in_guards; ig && !guarded; ig = ig->next) {
                    if (ig->is_backward_guard && expr_struct_equal(ig->index, base))
                        guarded = true;
                }
            }
        }
        if (!guarded) {
            fprintf(stderr,
                "[E060] Error Ln %li, Col %li: pointer dereference outside 'unsafe' block. "
                "Dereference it inside 'unsafe { }', or use a slice and an index (`a[i]`), whose bounds "
                "the compiler proves.\n",
                e->line, e->col);
            diagnostic_show_line(e->line, e->col);
            exit(1);
        }
    }
    break;
  }

  case EXPR_TRY: {
    // `try op` — op must be a `T | markers` union. The expression's value is the
    // payload T; on a marker, the enclosing function returns that marker. F3.4.
    Expr *op = e->as.try_expr.operand;
    sema_infer_expr(op);
    Type *pay = op ? union_payload_type(op->type) : NULL;
    if (!pay) {
        fprintf(stderr, "[E066] Error Ln %li, Col %li: `try` requires a `T | markers` union value "
                "(nothing to propagate otherwise).\n", (long)e->line, (long)e->col);
        diagnostic_show_line(e->line, e->col);
        exit(1);
    }
    e->type = pay;
    // The ⊆ propagation law is checked in the walk phase, where the enclosing
    // function's return type (current_return_type) is established. Every marker
    // `op` can produce must be a member of the return union — else fail-closed.
    if (sema_walk_phase) {
        Decl *opU  = find_union_enum(op->type);
        Decl *retU = find_union_enum(current_return_type);
        e->as.try_expr.operand_enum = opU;
        e->as.try_expr.return_enum  = retU;
        // ★ EVERY MARKER, PAYLOAD OR NOT (I.122). The loops skipped any variant with fields, meant
        // for the `__payload` variant, so every PAYLOAD marker was skipped: `try` of a
        // `ParseErr(99)` into `i32 | Other(code i32)` was accepted, and the marker had nowhere to
        // go. Its block returned the zero value, so the error became the success value 0 in
        // silence. The payload variant is skipped by its name, and a payload marker must be in
        // the return union with a payload of the same shape.
        #define TC_IS_PAYLOAD_VARIANT(x) ((x)->name && (x)->name->length == 9 && memcmp((x)->name->name, "__payload", 9) == 0)
        if (opU) for (Variant *v = opU->as.enum_decl.variants; v; v = v->next) {
            if (TC_IS_PAYLOAD_VARIANT(v)) continue;
            bool in_ret = false; Variant *match = NULL;
            if (retU) for (Variant *w = retU->as.enum_decl.variants; w; w = w->next) {
                if (TC_IS_PAYLOAD_VARIANT(w)) continue;
                if (w->name->length == v->name->length &&
                    memcmp(w->name->name, v->name->name, (size_t)v->name->length) == 0) { in_ret = true; match = w; break; }
            }
            if (in_ret) {
                int nv = 0, nw = 0;
                for (DeclList *f = v->fields; f; f = f->next) nv++;
                for (DeclList *f = match->fields; f; f = f->next) nw++;
                if (nv != nw) {
                    fprintf(stderr, "[E065] Error Ln %li, Col %li: `try` may propagate marker `%.*s` with a "
                            "payload of %d field%s, and the enclosing function's return union declares `%.*s` "
                            "with %d. A marker keeps its payload; give both the same fields, or handle it "
                            "with `case`.\n", (long)e->line, (long)e->col,
                            (int)v->name->length, v->name->name, nv, nv == 1 ? "" : "s",
                            (int)v->name->length, v->name->name, nw);
                    diagnostic_show_line(e->line, e->col);
                    exit(1);
                }
            }
            if (!in_ret && !retU) {
                // A function that returns no union has nowhere to send a marker (Handwriting: it read
                // "not in the enclosing function's return union" of a function returning i32).
                char rb[128]; type_describe(current_return_type, rb, sizeof rb);
                fprintf(stderr, "[E065] Error Ln %li, Col %li: `try` may propagate marker `%.*s`, but the "
                        "enclosing function returns '%s', not a union. Return a union that has `%.*s`, or "
                        "handle it with `case`.\n", (long)e->line, (long)e->col,
                        (int)v->name->length, v->name->name, rb, (int)v->name->length, v->name->name);
                diagnostic_show_line(e->line, e->col);
                exit(1);
            }
            if (!in_ret) {
                fprintf(stderr, "[E065] Error Ln %li, Col %li: `try` may propagate marker `%.*s`, "
                        "but it is not in the enclosing function's return union. Add `| %.*s` to the "
                        "return type, or handle it with `case`.\n",
                        (long)e->line, (long)e->col,
                        (int)v->name->length, v->name->name, (int)v->name->length, v->name->name);
                diagnostic_show_line(e->line, e->col);
                exit(1);
            }
        }
        #undef TC_IS_PAYLOAD_VARIANT
    }
    break;
  }

  case EXPR_ELSE: {
    // `op else arm` — op must be a union; on a marker, the value is `arm` (a
    // fallback of type T, or `panic`); otherwise the payload T. F3.4.
    Expr *op = e->as.else_expr.operand;
    if (op) op->checked_ok = true;   // this `else` handles it (if it is a checked op)
    sema_infer_expr(op);
    if (e->as.else_expr.arm) sema_infer_expr(e->as.else_expr.arm);
    // `else return X`: X is a return value — coerce it to the enclosing return type
    // (a bare marker → the return union's constructor), exactly as a `return` does.
    if (e->as.else_expr.arm_is_return && sema_walk_phase && current_return_type && e->as.else_expr.arm) {
        sema_union_coerce(&e->as.else_expr.arm, current_return_type);
        sema_infer_expr(e->as.else_expr.arm);
    }
    // Q1 inline-checked recovery: `a +? b else X` / `x as? T else X`. The operand
    // is a checked op (value type T, never a union); on overflow/out-of-range the
    // value is the arm. Result type is T — no union is materialized.
    if (expr_is_checked_op(op)) {
        e->type = op->type;
        check_else_arm(e, op->type);
        break;
    }
    Type *pay = op ? union_payload_type(op->type) : NULL;
    if (!pay) {
        fprintf(stderr, "[E066] Error Ln %li, Col %li: `else` requires a `T | markers` union value "
                "or a checked op (`+?`/`as?`) on its left (nothing to fall back from otherwise).\n",
                (long)e->line, (long)e->col);
        diagnostic_show_line(e->line, e->col);
        exit(1);
    }
    e->type = pay;
    if (sema_walk_phase) e->as.else_expr.operand_enum = find_union_enum(op->type);
    check_else_arm(e, pay);
    break;
  }

  case EXPR_BUILTIN: {
    BuiltinKind bk = e->as.builtin_expr.builtin_kind;
    if ((bk == BUILTIN_LIKELY || bk == BUILTIN_UNLIKELY) && e->as.builtin_expr.arg) {
        sema_infer_expr(e->as.builtin_expr.arg);
        // Propagate inner type; @likely/@unlikely are transparent wrappers
        e->type = e->as.builtin_expr.arg->type;
    } else if (bk == BUILTIN_ASSUME_ALIGNED && e->as.builtin_expr.arg) {
        sema_infer_expr(e->as.builtin_expr.arg);
        // Return type is same pointer type as the argument
        e->type = e->as.builtin_expr.arg->type;
    } else if ((bk == BUILTIN_CTZ || bk == BUILTIN_CLZ || bk == BUILTIN_POPCOUNT ||
                bk == BUILTIN_MOVEMASK) && e->as.builtin_expr.arg) {
        sema_infer_expr(e->as.builtin_expr.arg);
        // @ctz/@clz/@popcount take an integer; @movemask takes a Vec(N,u8). All
        // yield a u32 (a bit count, or a lane bitmask).
        // ★ @movemask READS BYTES. Its lane test is a byte's top bit, and on `u32x4 == 2` it
        // read the first four BYTES (lane 0 alone): [1,2,3,4] == 2 gave 0, not 0b0010, silently.
        // A u8x64 would need 64 bits in a u32. So: 8-bit lanes, at most 32 of them.
        if (bk == BUILTIN_MOVEMASK) {
            Type *mt = sema_unwrap_type(e->as.builtin_expr.arg->type);
            int mb = 0; bool ms = false;
            if (mt && (mt->kind != TYPE_VECTOR || !parse_iN_uN(mt->element_type, &mb, &ms) ||
                       mb != 8 || mt->array_len > 32)) {
                fprintf(stderr, "[E100] Error Ln %li, Col %li: @movemask takes a vector of at most 32 "
                        "8-bit lanes (u8x16, u8x32): its bits are the lanes' top bits.\n",
                        (long)e->line, (long)e->col);
                diagnostic_show_line(e->line, e->col);
                exit(1);
            }
        }
        static Type *u32_ty = NULL;
        if (!u32_ty) {
            Id *uid = arena_push_aligned(sema_arena, Id);
            uid->name = "u32"; uid->length = 3;
            u32_ty = type_simple(sema_arena, uid);
        }
        e->type = u32_ty;
    } else if (bk == BUILTIN_SIZEOF || bk == BUILTIN_ALIGNOF) {
        // ★ THE TYPE ASKED ABOUT IS THE INSTANCE. `@sizeof(Pair(u64))` kept its type application
        // unresolved, so lowering found the TEMPLATE `Pair` (T erased to `void*`, never reordered)
        // and every instance answered with the template's size: `assert @sizeof(Pair(u8)) == 16`
        // passed. Every other type position resolves applications; this one did not.
        if (e->as.builtin_expr.vec_type)
            e->as.builtin_expr.vec_type = mono_resolve_type_apps_at(e->as.builtin_expr.vec_type, e->line, e->col);
        static Type *usize_ty = NULL;
        if (!usize_ty) {
            Id *uid = arena_push_aligned(sema_arena, Id);
            uid->name = "usize"; uid->length = 5;
            usize_ty = type_simple(sema_arena, uid);
        }
        e->type = usize_ty;
    } else if (bk == BUILTIN_LOAD || bk == BUILTIN_SPLAT || bk == BUILTIN_STORE ||
               bk == BUILTIN_SHUFFLE) {
        if (e->as.builtin_expr.arg)  sema_infer_expr(e->as.builtin_expr.arg);
        if (e->as.builtin_expr.arg2) sema_infer_expr(e->as.builtin_expr.arg2);
        if (e->as.builtin_expr.arg3) sema_infer_expr(e->as.builtin_expr.arg3);
        // @shuffle(t, idx) (DECIDE-Q): `t` a vector of integer lanes, `idx` as many lanes of the
        // UNSIGNED type of the same width — u8x16 for a u8x16 or i8x16 table, u32x4 for u32x4.
        // The result is t's type. An index lane >= N gives 0, so there is no obligation to state.
        if (bk == BUILTIN_SHUFFLE && e->as.builtin_expr.arg && e->as.builtin_expr.arg2) {
            Type *tt = sema_unwrap_type(e->as.builtin_expr.arg->type);
            Type *it = sema_unwrap_type(e->as.builtin_expr.arg2->type);
            if (tt && it) {
                int tb = 0, ib = 0; bool ts = false, is = false; const char *why = NULL;
                if (tt->kind != TYPE_VECTOR || !parse_iN_uN(tt->element_type, &tb, &ts))
                    why = "the table must be a vector of integer lanes";
                else if (it->kind != TYPE_VECTOR || !parse_iN_uN(it->element_type, &ib, &is))
                    why = "the index must be a vector of unsigned integer lanes";
                else if (it->array_len != tt->array_len)
                    why = "the index must have as many lanes as the table";
                else if (is || ib != tb)
                    why = "the index lanes must be the unsigned type of the table's lane width "
                          "(u8 lanes for an 8-bit table, u32 lanes for a 32-bit one)";
                if (why) {
                    fprintf(stderr, "[E100] Error Ln %li, Col %li: @shuffle: %s.\n",
                            (long)e->line, (long)e->col, why);
                    diagnostic_show_line(e->line, e->col);
                    exit(1);
                }
            }
        }
        // @load(V, b, i) / @store(b, i, v): `b` is an array, a slice or a raw pointer — memory
        // whose element order the program defines. A STRUCT was accepted (inside `unsafe`) and
        // emitted `&t[i]`, C that gcc refuses; it would also read the struct's storage order,
        // which is the compiler's (DECIDE-U), not the program's.
        if ((bk == BUILTIN_LOAD || bk == BUILTIN_STORE) && e->as.builtin_expr.arg) {
            // The DECLARED type: sema_unwrap_type strips a pointer, so a raw `*u8` — the one
            // buffer the spec allows inside `unsafe` — read as a `u8` and was refused (506f825).
            Type *bt = e->as.builtin_expr.arg->type;
            while (bt && bt->kind == TYPE_COMPTIME) bt = bt->element_type;
            if (bk == BUILTIN_STORE && bt && bt->kind == TYPE_POINTER && !bt->pointee_mutable) {
                fprintf(stderr, "[E009] Error Ln %li, Col %li: @store: cannot write through a read-only "
                        "pointer `*T`. Declare it `*var T` to write through it.\n", (long)e->line, (long)e->col);
                diagnostic_show_line(e->line, e->col);
                exit(1);
            }
            if (bt && bt->kind != TYPE_ARRAY && bt->kind != TYPE_SLICE && bt->kind != TYPE_POINTER) {
                fprintf(stderr, "[E100] Error Ln %li, Col %li: @%s: the buffer must be an array, a "
                        "slice or a raw pointer. A struct's bytes are not an array: its storage order "
                        "is the compiler's (spec 7, struct types).\n", (long)e->line, (long)e->col,
                        bk == BUILTIN_LOAD ? "load" : "store");
                diagnostic_show_line(e->line, e->col);
                exit(1);
            }
        }
        if (bk == BUILTIN_LOAD || bk == BUILTIN_SPLAT)
            e->type = e->as.builtin_expr.vec_type;                 // result is the vector T
        else if (bk == BUILTIN_SHUFFLE)
            e->type = e->as.builtin_expr.arg ? e->as.builtin_expr.arg->type : NULL; // table's vector
        else
            e->type = NULL;                                        // @store: void statement

        // P2: a @load from a SIZED u8 buffer (a `u8[N]` / `u8[]` array) is
        // bounds-CHECKED — reading [off, off+L) must lie inside it. A @load from
        // a raw `*u8` stays the unchecked unsafe primitive. For a u8 buffer the
        // byte offset IS the element index, so the existing bounds machinery
        // proves it directly (never invent a proof): off ∈ [0,len) and
        // off+L-1 ∈ [0,len)  ⟹  [off, off+L) ⊆ [0,len). Unproven ⟹ hard [VRA].
        if (bk == BUILTIN_LOAD && sema_ranges && sema_walk_phase && !sema_in_unsafe_block) {
            Type *bt = e->as.builtin_expr.arg ? sema_unwrap_type(e->as.builtin_expr.arg->type) : NULL;
            Type *vt = e->as.builtin_expr.vec_type;
            bool u8_buf = bt && bt->kind == TYPE_ARRAY &&
                          bt->element_type && bt->element_type->base_type &&
                          bt->element_type->base_type->length == 2 &&
                          strncmp(bt->element_type->base_type->name, "u8", 2) == 0;
            if (u8_buf && vt && vt->kind == TYPE_VECTOR && e->as.builtin_expr.arg2) {
                long L = (long)vt->array_len;
                Expr *off  = e->as.builtin_expr.arg2;
                Expr *last = expr_binary(sema_arena, TOKEN_PLUS, off,
                                         expr_literal(sema_arena, L - 1));
                // P2b: a wide load is ALSO proven if its LAST byte is in-guarded —
                // `(off + L-1) in buf`. For an UNSIGNED offset (off >= 0 by type),
                // the last-byte guard plus contiguity proves the whole [off, off+L)
                // in bounds with NO runtime check. So a SIMD scan loop
                // `while (i + L-1) in buf { @load(...) ; i += L }` is proven safe,
                // and its `unsafe` goes away.
                extern int type_integer_range(Type *ty, long long *lo, long long *hi);
                long long tlo, thi;
                bool off_unsigned = off->type &&
                                    type_integer_range(off->type, &tlo, &thi) && tlo >= 0;
                (void)off_unsigned;   // bounds are the IR's: see the note at the index case
            }
        }
    }
    break;
  }

  default:
    break;
  }
}

#endif /* SEMA_TYPECHECK_H */
