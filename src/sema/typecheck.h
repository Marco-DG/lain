#ifndef SEMA_TYPECHECK_H
#define SEMA_TYPECHECK_H

#include <limits.h>
#include "../ast.h"
#include "resolve.h"
#include "resolve.h"
#include "ranges.h" // Range analysis
#include "bounds.h"  // Static bounds checking

extern Arena *sema_arena;
extern DeclList *sema_decls;
extern Type *current_return_type;
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

// Q-002 Phase 5 (extended): check if a value's VRA range fits the
// target integer type's natural range. Returns true if check passed
// (or was skipped: unsafe block, unknown range, unbounded range).
// On violation, emits E086 with helpful suggestions and exits.
//
// `context` describes the boundary site for the error message
// (e.g., "assignment to", "return from function", "argument to").
// `target_label` identifies the specific target (variable name,
// parameter name, etc.) for the error.
bool check_value_fits_type(Range r, Type *target_type,
                           isize line, isize col,
                           const char *context, const char *target_label);
bool check_value_fits_type(Range r, Type *target_type,
                           isize line, isize col,
                           const char *context, const char *target_label) {
    if (sema_in_unsafe_block) return true;
    if (!r.known) return true;
    // Skip if range is effectively unbounded — VRA may have widened from
    // an unconstrained value, so a range whose extremum sits within a
    // small window of LLONG_MIN/LLONG_MAX is treated as "no info".
    // The window absorbs arithmetic from type-max constants
    // (e.g. `n - 2` widens to [LLONG_MIN+2, LLONG_MAX-2]).
    const long long UNBOUNDED_WINDOW = 4096;
    if (r.min <= LLONG_MIN + UNBOUNDED_WINDOW ||
        r.max >= LLONG_MAX - UNBOUNDED_WINDOW) return true;
    long long tlo, thi;
    if (!target_type || !type_integer_range(target_type, &tlo, &thi)) return true;
    if (r.min < tlo || r.max > thi) {
        const char *type_name = "?";
        int type_len = 1;
        if (target_type->kind == TYPE_SIMPLE && target_type->base_type) {
            type_name = target_type->base_type->name;
            type_len = (int)target_type->base_type->length;
        }
        fprintf(stderr,
            "[E086] Error Ln %li, Col %li: %s '%s' would overflow target type '%.*s'.\n"
            "       Value range [%lld, %lld] does not fit type range [%lld, %lld].\n"
            "       Options to resolve:\n"
            "         (a) Widen the target type (e.g., u16 instead of u8).\n"
            "         (b) Use a wrapping operator: +%%, -%%, *%% (modular).\n"
            "         (c) Use a saturating operator: +|, -|, *| (clamp).\n"
            "         (d) Tighten input constraints so VRA can prove safety.\n",
            (long)line, (long)col, context,
            target_label ? target_label : "",
            type_len, type_name,
            (long long)r.min, (long long)r.max, tlo, thi);
        diagnostic_show_line(line, col);
        exit(1);
    }
    return true;
}

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

// True ONLY when VRA has proven a concrete bounded range for the source that
// fits `to`. Unlike check_value_fits_type, the fail-open cases (unknown or
// effectively-unbounded range) return FALSE here — "not proven safe", rather
// than "assume safe". This is the positive half needed to close the narrowing
// hole without also silencing genuine refinement narrowing.
static bool range_proves_int_fit(Range r, Type *to) {
    if (!r.known) return false;
    const long long W = 4096;
    if (r.min <= LLONG_MIN + W || r.max >= LLONG_MAX - W) return false;
    long long tlo, thi;
    if (!type_integer_range(to, &tlo, &thi)) return false;
    return r.min >= tlo && r.max <= thi;
}

// P2/S3: reject an implicit LOSSY integer conversion at a boundary. A fixed
// width narrowing or signedness change is permitted only when it is either
// statically safe (from <: to) or VRA-proven to fit; otherwise it needs an
// explicit `as` cast (or a wrapping/saturating operator). This closes the
// hole where signed / u64 sources — whose bare-parameter VRA range is
// effectively unbounded — slipped past check_value_fits_type silently
// (e.g. `func f(a i32) i8 { return a }` truncated with no diagnostic).
// usize/isize (platform-dependent width) are left to other checks.
static void reject_lossy_int_conversion(Type *from, Type *to, Range r,
                                        isize line, isize col,
                                        const char *ctx, const char *label) {
    if (sema_in_unsafe_block) return;
    if (from == to) return;                              // interned identity: same type
    // F3.5 Path-F: a widened arithmetic result narrowing back to an operand-width
    // type is judged by the window policy (check_value_fits_type, already called
    // at this boundary) — which still catches a KNOWN overflow — not the
    // fail-closed lossy check. Keeps `x = x + 1` ergonomic; a genuine truncation
    // of a non-widened value (`i32 -> i8`) is unaffected.
    if (from && from->arith_widened) return;
    if (!is_integer_type(from) || !is_integer_type(to)) return;
    long long flo, fhi, tlo, thi;
    if (!type_integer_range(from, &flo, &fhi)) {
        // usize/isize source has no fixed width, but narrowing it to a fixed-width
        // target can still lose data (a length can exceed i32). Judge it with a
        // conservative platform range so the narrowing is not silently accepted;
        // usize -> i64/u64 stays a safe widening, usize -> i32 needs proof or a cast.
        bool from_unsigned = from->base_type && from->base_type->length >= 1 &&
                             from->base_type->name[0] == 'u';
        flo = from_unsigned ? 0 : LLONG_MIN;
        fhi = LLONG_MAX;
    }
    if (!type_integer_range(to,   &tlo, &thi)) return;   // usize/isize target: don't judge
    if (flo >= tlo && fhi <= thi) return;                // statically safe widening
    if (flo >= tlo && fhi <= thi) return;                // statically safe widening
    if (range_proves_int_fit(r, to)) return;             // VRA proved the narrowing safe
    const char *fn = (from->base_type) ? from->base_type->name : "?";
    int fl = (from->base_type) ? (int)from->base_type->length : 1;
    const char *tn = (to->base_type) ? to->base_type->name : "?";
    int tl = (to->base_type) ? (int)to->base_type->length : 1;
    fprintf(stderr,
        "[E086] Error Ln %li, Col %li: %s '%s' implicitly converts '%.*s' to '%.*s', "
        "which may lose information.\n"
        "       Source type range [%lld, %lld] does not fit target range [%lld, %lld].\n"
        "       Options to resolve:\n"
        "         (a) Use an explicit cast: 'value as %.*s' (truncates).\n"
        "         (b) Use a wrapping (+%%) or saturating (+|) operator.\n"
        "         (c) Constrain the source so VRA can prove it fits.\n",
        (long)line, (long)col, ctx, label ? label : "",
        fl, fn, tl, tn, flo, fhi, tlo, thi, tl, tn);
    diagnostic_show_line(line, col);
    exit(1);
}

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
static Type *path_f_result_type(Type *a, Type *b, TokenKind op) {
    int wa, wb; bool sa, sb;
    if (!parse_iN_uN(a, &wa, &sa) || !parse_iN_uN(b, &wb, &sb))
        return wider_integer_type(a, b);
    bool rsigned = (op == TOKEN_MINUS) || sa || sb;
    int w;
    if (op == TOKEN_ASTERISK) {
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

static bool is_pointer_bearing(Type *t) {
    if (!t) return false;
    while (t && t->kind == TYPE_COMPTIME) t = t->element_type;
    if (!t) return false;
    // Direct pointer, slice (T[]), null-terminated slice (T[:0])
    if (t->kind == TYPE_POINTER) return true;
    if (t->kind == TYPE_SLICE)   return true;
    if (t->kind == TYPE_ARRAY) {
        // Dynamic-length array (slice) is pointer-bearing.
        // Fixed-size array (length >= 0) is NOT (data is inline).
        if (t->array_len == -1) return true;
        return false;
    }
    // TYPE_SIMPLE: check the underlying decl's fields recursively.
    if (t->kind == TYPE_SIMPLE && t->base_type) {
        char buf[256];
        if ((size_t)t->base_type->length >= sizeof(buf)) return false;
        memcpy(buf, t->base_type->name, t->base_type->length);
        buf[t->base_type->length] = '\0';
        extern Symbol *sema_lookup(const char *name);
        Symbol *sym = sema_lookup(buf);
        if (!sym || !sym->decl) return false;
        if (sym->decl->kind == DECL_STRUCT) {
            for (DeclList *f = sym->decl->as.struct_decl.fields; f; f = f->next) {
                if (f->decl && f->decl->kind == DECL_VARIABLE) {
                    if (is_pointer_bearing(f->decl->as.variable_decl.type)) return true;
                }
            }
        }
        if (sym->decl->kind == DECL_ENUM) {
            for (Variant *v = sym->decl->as.enum_decl.variants; v; v = v->next) {
                for (DeclList *f = v->fields; f; f = f->next) {
                    if (f->decl && f->decl->kind == DECL_VARIABLE) {
                        if (is_pointer_bearing(f->decl->as.variable_decl.type)) return true;
                    }
                }
            }
        }
    }
    return false;
}

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
            if (a->sentinel_is_string != b->sentinel_is_string) return false;
            if (a->sentinel_len != b->sentinel_len) return false;
            if ((a->sentinel_str == NULL) != (b->sentinel_str == NULL)) return false;
            if (a->sentinel_str && b->sentinel_str &&
                memcmp(a->sentinel_str, b->sentinel_str, (size_t)a->sentinel_len) != 0) return false;
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
            if (a->func_is_total != b->func_is_total) return false;
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
            snprintf(buf, cap, "%s[]", inner);
            break;
        case TYPE_ARRAY:
            type_describe(t->element_type, inner, sizeof inner);
            if (t->array_len >= 0) snprintf(buf, cap, "%s[%lld]", inner, (long long)t->array_len);
            else snprintf(buf, cap, "%s[]", inner);
            break;
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
// This is the single reader `type_subsumes` (and, later, VRA) consult in place of
// the name-keyed range table.
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

// Subsumption `sub <: sup`: a value of `sub` is safely usable where `sup` is
// expected. True iff they share a core (mode/refinement stripped, aliases peeled)
// AND sub's interval ⊆ sup's. The single relation that (K3) will decide every
// boundary — replacing the scattered fits-checks. Sound: only accepts when the
// interval containment is proven.
static bool type_subsumes(Type *sub, Type *sup) {
    if (!sub || !sup) return false;
    if (!core_identical(resolve_type_alias(sub), resolve_type_alias(sup))) return false;
    Refinement a = type_interval(sub);
    Refinement b = type_interval(sup);
    if (!b.known) return true;                        // sup is ⊤ → anything of this core fits
    if (!a.known) return false;                       // sub unbounded, sup bounded → not proven
    return a.lo >= b.lo && a.hi <= b.hi;              // [a] ⊆ [b]
}

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
    if (sema_in_unsafe_block) return;
    if (!from || !to) return;
    Type *f = from, *t = to;
    while (f && f->kind == TYPE_COMPTIME) f = f->element_type;
    while (t && t->kind == TYPE_COMPTIME) t = t->element_type;
    if (!f || !t) return;
    f = resolve_type_alias(f);
    t = resolve_type_alias(t);
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
        // Neither is a pointer. Reject nominal (struct/enum) confusion — a
        // distinct struct/enum, or a struct-vs-scalar. Pure-scalar mismatches
        // (bool<->int, which share a representation) are deferred to the full
        // subsumption relation.
        if (!is_nominal_aggregate(f) && !is_nominal_aggregate(t)) return;
        ok = types_compatible(f, t);   // same struct/enum name (mode-agnostic) is fine
    }
    if (ok) return;
    char fb[128], tb[128];
    type_describe(f, fb, sizeof fb);
    type_describe(t, tb, sizeof tb);
    fprintf(stderr,
        "[E012] Error Ln %li, Col %li: %s '%s' has incompatible type: cannot implicitly "
        "convert '%s' to '%s'.\n",
        (long)line, (long)col, ctx, label ? label : "", fb, tb);
    diagnostic_show_line(line, col);
    exit(1);
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

// P2/S3 (unification): enforce a refinement-type-alias's refinement on a value
// whose VRA range is r flowing into a slot of type `to`. This is the boundary
// half of the subsumption relation `r ⊑ refine(to)`. The INTERVAL component is
// now read from the type itself (`type_refine_interval`, the S2 `refine` field)
// rather than re-derived from a private op→bound switch — one source of truth,
// shared with `type_integer_range`. Only the DISEQUALITY residual (`!= k`), which
// an interval cannot represent, is still read from the alias's constraint list.
// No-op for non-alias types, unknown range, or inside unsafe. Riding inside
// check_conversion, this makes refinement aliases sound at EVERY boundary.
static void check_type_alias_constraints(Type *to, Range r, isize line, isize col,
                                         const char *ctx, const char *label) {
    if (sema_in_unsafe_block || !r.known) return;
    if (!to || to->kind != TYPE_SIMPLE || !to->base_type) return;
    if ((size_t)to->base_type->length >= 256) return;

    // Interval refinement — the single relation, read from the type's `refine`.
    Refinement rf = type_refine_interval(to);
    bool interval_ok = !rf.known || (r.min >= rf.lo && r.max <= rf.hi);

    // Disequality residual (`x != k`): not expressible as an interval, so read it
    // from the alias constraints directly. `r` satisfies `!= k` iff it excludes k.
    bool diseq_ok = true;
    ExprList *cs = alias_constraints_for(to);
    for (ExprList *c = cs; c; c = c->next) {
        if (!c->expr || c->expr->kind != EXPR_BINARY) continue;
        if (c->expr->as.binary_expr.op != TOKEN_BANG_EQUAL) continue;
        Expr *rhs = c->expr->as.binary_expr.right;
        if (!rhs || rhs->kind != EXPR_LITERAL) continue;
        long long k = rhs->as.literal_expr.value;
        if (!(r.min > k || r.max < k)) { diseq_ok = false; break; }
    }

    if (!interval_ok || !diseq_ok) {
        char tnam[256];
        memcpy(tnam, to->base_type->name, to->base_type->length);
        tnam[to->base_type->length] = '\0';
        fprintf(stderr, "[E086] Error Ln %li, Col %li: %s '%s' violates refinement "
            "constraint of type alias '%s': value range [%lld, %lld] does not satisfy it.\n",
            (long)line, (long)col, ctx, label ? label : "", tnam,
            (long long)r.min, (long long)r.max);
        diagnostic_show_line(line, col);
        exit(1);
    }
}

// P2/S4: evaluate a dependent size expression (e.g. `a.len + b.len`) to a Range
// by resolving each `param.len` to the actual argument's length at a call site.
// Handles member(.len), literals, and +/-/* of those. Returns range_unknown()
// if any referenced length is not statically known. Used to verify dependent
// size contracts at the CALL boundary (closes the OOB where the caller passes a
// wrong-length array — the callee's internal proofs trust the declared size).
static Range eval_callsite_size_range(Expr *se, DeclList *params, ExprList *args) {
    if (!se) return range_unknown();
    if (se->kind == EXPR_LITERAL) return range_const(se->as.literal_expr.value);
    if (se->kind == EXPR_MEMBER && se->as.member_expr.member &&
        se->as.member_expr.member->length == 3 &&
        strncmp(se->as.member_expr.member->name, "len", 3) == 0 &&
        se->as.member_expr.target->kind == EXPR_IDENTIFIER) {
        Id *rid = se->as.member_expr.target->as.identifier_expr.id;
        int rpi = 0; Expr *ref_arg = NULL;
        for (DeclList *rp = params; rp; rp = rp->next, rpi++) {
            if (rp->decl->kind != DECL_VARIABLE) continue;
            Id *rpn = rp->decl->as.variable_decl.name;
            if (rpn && rpn->length == rid->length &&
                strncmp(rpn->name, rid->name, rpn->length) == 0) {
                int ri = 0;
                for (ExprList *ra = args; ra; ra = ra->next)
                    if (ri++ == rpi) { ref_arg = ra->expr; break; }
                break;
            }
        }
        if (!ref_arg) return range_unknown();
        if (ref_arg->type && ref_arg->type->kind == TYPE_ARRAY && ref_arg->type->array_len >= 0)
            return range_const(ref_arg->type->array_len);
        if (ref_arg->kind == EXPR_IDENTIFIER && sema_ranges) {
            Id *aid = ref_arg->as.identifier_expr.id;
            char lk[272]; int lklen = 6 + (int)aid->length;
            if (lklen < (int)sizeof(lk)) {
                memcpy(lk, "__len_", 6); memcpy(lk + 6, aid->name, aid->length);
                for (RangeEntry *re = sema_ranges->head; re; re = re->next)
                    if (re->var->length == lklen && strncmp(re->var->name, lk, lklen) == 0)
                        return re->range;
            }
        }
        return range_unknown();
    }
    if (se->kind == EXPR_BINARY) {
        Range l = eval_callsite_size_range(se->as.binary_expr.left, params, args);
        Range r = eval_callsite_size_range(se->as.binary_expr.right, params, args);
        if (!l.known || !r.known) return range_unknown();
        switch (se->as.binary_expr.op) {
            case TOKEN_PLUS:     return (Range){ l.min + r.min, l.max + r.max, true };
            case TOKEN_MINUS:    return (Range){ l.min - r.max, l.max - r.min, true };
            case TOKEN_ASTERISK: return (Range){ l.min * r.min, l.max * r.max, true };
            default: return range_unknown();
        }
    }
    return range_unknown();
}

// Companion to eval_callsite_size_range: for a callee `param.len` at the call
// site, return the ACTUAL array argument's `__len_` difference variable Id (or
// NULL). Mirrors the identifier branch above but yields the synthetic Id so a
// precondition can be discharged relationally (difference constraints), not just
// by interval — this is what lets a forwarded/guarded `i < a.len` prove without a
// concrete numeric length.
static Id *callsite_len_id(Expr *se, DeclList *params, ExprList *args) {
    if (!se || se->kind != EXPR_MEMBER || !se->as.member_expr.member ||
        se->as.member_expr.member->length != 3 ||
        strncmp(se->as.member_expr.member->name, "len", 3) != 0 ||
        !se->as.member_expr.target || se->as.member_expr.target->kind != EXPR_IDENTIFIER ||
        !sema_ranges)
        return NULL;
    Id *rid = se->as.member_expr.target->as.identifier_expr.id;
    int rpi = 0; Expr *ref_arg = NULL;
    for (DeclList *rp = params; rp; rp = rp->next, rpi++) {
        if (rp->decl->kind != DECL_VARIABLE) continue;
        Id *rpn = rp->decl->as.variable_decl.name;
        if (rpn && rpn->length == rid->length &&
            strncmp(rpn->name, rid->name, rpn->length) == 0) {
            int ri = 0;
            for (ExprList *ra = args; ra; ra = ra->next)
                if (ri++ == rpi) { ref_arg = ra->expr; break; }
            break;
        }
    }
    if (!ref_arg || ref_arg->kind != EXPR_IDENTIFIER) return NULL;
    Id *aid = ref_arg->as.identifier_expr.id;
    char lk[272]; int lklen = 6 + (int)aid->length;
    if (lklen >= (int)sizeof(lk)) return NULL;
    memcpy(lk, "__len_", 6); memcpy(lk + 6, aid->name, aid->length);
    for (RangeEntry *re = sema_ranges->head; re; re = re->next)
        if (re->var && re->var->length == lklen && strncmp(re->var->name, lk, lklen) == 0)
            return re->var;
    return NULL;
}

// Fail-CLOSED precondition proof for a relational parameter refinement
// `arg OP param.len` (the bounds-carrying case of `i usize < a.len`). Returns
// true only when the constraint is PROVEN at the call site — otherwise the
// caller rejects, because an unproven `i < a.len` becomes an unchecked
// out-of-bounds `a[i]` inside the callee. Proof is either:
//   (1) interval — the argument's VRA range vs the actual array's length range
//       (covers fixed arrays / concrete indices), or
//   (2) relational — a difference constraint `arg - __len_actual` (covers
//       symbolic slice lengths, forwarded param refinements, and `if`/loop
//       guards, which all seed the difference constraint).
static bool callsite_len_precond_proven(Expr *lhs_arg, Expr *rhs_len, TokenKind op,
                                        DeclList *params, ExprList *args) {
    Range lr   = lhs_arg ? sema_eval_range(lhs_arg, sema_ranges) : range_unknown();
    Range lenr = eval_callsite_size_range(rhs_len, params, args);
    if (lr.known && lenr.known) {
        switch (op) {
            case TOKEN_ANGLE_BRACKET_LEFT:        if (lr.max <  lenr.min) return true; break;
            case TOKEN_ANGLE_BRACKET_LEFT_EQUAL:  if (lr.max <= lenr.min) return true; break;
            case TOKEN_ANGLE_BRACKET_RIGHT:       if (lr.min >  lenr.max) return true; break;
            case TOKEN_ANGLE_BRACKET_RIGHT_EQUAL: if (lr.min >= lenr.max) return true; break;
            default: break;
        }
    }
    if (lhs_arg && lhs_arg->kind == EXPR_IDENTIFIER && sema_ranges) {
        Id *argid = lhs_arg->as.identifier_expr.id;
        Id *lenid = callsite_len_id(rhs_len, params, args);
        if (argid && lenid) {
            bool found = false; int64_t diff = 0;
            switch (op) {
                case TOKEN_ANGLE_BRACKET_LEFT:        // arg < len  ⇐  arg - len <= -1
                    diff = constraint_get_diff(sema_ranges, argid, lenid, &found);
                    if (found && diff <= -1) return true; break;
                case TOKEN_ANGLE_BRACKET_LEFT_EQUAL:  // arg <= len ⇐  arg - len <= 0
                    diff = constraint_get_diff(sema_ranges, argid, lenid, &found);
                    if (found && diff <= 0) return true; break;
                case TOKEN_ANGLE_BRACKET_RIGHT:       // arg > len  ⇐  len - arg <= -1
                    diff = constraint_get_diff(sema_ranges, lenid, argid, &found);
                    if (found && diff <= -1) return true; break;
                case TOKEN_ANGLE_BRACKET_RIGHT_EQUAL: // arg >= len ⇐  len - arg <= 0
                    diff = constraint_get_diff(sema_ranges, lenid, argid, &found);
                    if (found && diff <= 0) return true; break;
                default: break;
            }
        }
    }
    return false;
}

// Argument-refinement reflexivity (fail-closed precondition support): a callee
// precondition `arg OP <literal>` is discharged when the argument is an
// IMMUTABLE binding — a non-`var` parameter/local, so its declared refinement
// cannot have been invalidated by reassignment — whose OWN declared constraints
// include the same `OP <same literal>`. This lets an already-refined value
// forward into a same-refinement callee (e.g. `n != 0` passed to a `!= 0`
// parameter) without a redundant, unprovable-by-interval check — refinement
// subtyping in miniature (the S3 relation, specialized to a literal bound so the
// constraint denotes the same thing in caller and callee scope). Sound and
// conservative: exact op + exact literal only; anything else falls through to
// the normal prove-or-reject.
static bool arg_refinement_discharges(Expr *arg, TokenKind need_op, Expr *need_rhs) {
    if (!arg || arg->kind != EXPR_IDENTIFIER || !arg->decl) return false;
    if (arg->decl->kind != DECL_VARIABLE) return false;
    if (arg->decl->as.variable_decl.is_mutable) return false;   // `var` may be reassigned
    if (!need_rhs || need_rhs->kind != EXPR_LITERAL) return false;
    int64_t want = need_rhs->as.literal_expr.value;
    for (ExprList *c = arg->decl->as.variable_decl.constraints; c; c = c->next) {
        if (!c->expr || c->expr->kind != EXPR_BINARY) continue;
        if (c->expr->as.binary_expr.op != need_op) continue;
        Expr *r = c->expr->as.binary_expr.right;
        if (r && r->kind == EXPR_LITERAL && r->as.literal_expr.value == want)
            return true;
    }
    return false;
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
// source expr where available (enables the null-literal idiom, 0 -> *T); NULL
// is fine (that sub-check simply won't fire).
static void check_conversion(Type *from, Type *to, Range r, Expr *src_expr,
                             isize line, isize col,
                             const char *ctx, const char *label) {
    // `T | markers` coercion: a union proven present (narrowed by `if r`) is used
    // as its payload T; using it unnarrowed where a non-union is expected is
    // rejected (it may be a marker). Construction (value/marker -> union) is done
    // earlier by sema_union_coerce at each boundary.
    {
        Type *tu = to;   while (tu && tu->kind == TYPE_COMPTIME) tu = tu->element_type;
        Type *fu = from; while (fu && fu->kind == TYPE_COMPTIME) fu = fu->element_type;
        Type *fpay = union_payload_type(fu);
        if (fpay && !union_payload_type(tu)) {
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
    check_value_fits_type(r, to, line, col, ctx, label);
    reject_float_int_mismatch(from, to, line, col, ctx, label);
    reject_lossy_int_conversion(from, to, r, line, col, ctx, label);
    reject_incompatible_conversion(from, to, src_expr, line, col, ctx, label);
    check_type_alias_constraints(to, r, line, col, ctx, label);

    // K2 dual-run (measurement only, behind LAIN_KEYSTONE_DUALRUN — zero behavior
    // change). Reaching here means the r-based checks ACCEPTED. On a same-core
    // conversion, does the on-type subsumption relation agree? A "gap" is a case
    // the name-keyed range accepts but subsumption cannot yet prove — i.e. where
    // the interval still lives in the range table, not on the value's type. The
    // count of gaps is the size of the remaining per-value on-type migration (K5).
    if (getenv("LAIN_KEYSTONE_DUALRUN") &&
        core_identical(resolve_type_alias(from), resolve_type_alias(to)) &&
        !type_subsumes(from, to)) {
        char fb[96], tb[96];
        type_describe(from, fb, sizeof fb); type_describe(to, tb, sizeof tb);
        fprintf(stderr, "[keystone-gap] same-core accept but !subsumes: %s -> %s "
                "(r=[%lld,%lld]) — interval not yet on the type\n",
                fb, tb, (long long)r.min, (long long)r.max);
    }
}

// P2/S3: reject a type-confused comparison (==, !=, <, <=, >, >=). Comparing a
// pointer to a non-pointer (except the null idiom `p == 0`), two pointers with
// different pointee types, or a float to an integer are confusions that emit
// broken/mis-evaluated C. Integer signedness is intentionally NOT policed here
// — i32-vs-usize comparisons (`i < xs.len`) are idiomatic; struct/enum '==' is
// handled separately. `unsafe` bypasses the check.
static void check_comparison_operands(Type *lt, Type *rt, Expr *le, Expr *re,
                                      const char *op, isize line, isize col) {
    if (sema_in_unsafe_block) return;
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
    fprintf(stderr, "sema error: internal: lookup_struct_field_type called "
                    "with NULL struct_name\n");
    exit(1);
  }

  DeclStruct *sd = find_struct_decl(struct_name);
  if (!sd) {
    fprintf(stderr, "sema error: unknown struct ‘%.*s’\n",
            (int)struct_name->length, struct_name->name);
    exit(1);
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
static Type *sema_unwrap_type(Type *t) {
    while (t) {
        // With the new OwnershipMode system, we only unwrap pointer/comptime
        // The mode is just a field on the type, not a wrapper
        if (t->kind == TYPE_POINTER) t = t->element_type;
        else if (t->kind == TYPE_COMPTIME) t = t->element_type;
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

// Synthesize the function-pointer type of a function/procedure decl.
static Type *fnptr_type_of_decl(Decl *d) {
    if (!d) return NULL;
    bool is_func = (d->kind == DECL_FUNCTION || d->kind == DECL_EXTERN_FUNCTION);
    bool is_proc = (d->kind == DECL_PROCEDURE || d->kind == DECL_EXTERN_PROCEDURE);
    if (!is_func && !is_proc) return NULL;
    TypeList *pts = NULL, *tail = NULL;
    for (DeclList *p = d->as.function_decl.params; p; p = p->next) {
        Type *pt = (p->decl && p->decl->kind == DECL_VARIABLE)
                   ? p->decl->as.variable_decl.type : NULL;
        TypeList *node = type_list(sema_arena, pt);
        if (!pts) pts = node; else tail->next = node;
        tail = node;
    }
    return type_func(sema_arena, pts, d->as.function_decl.return_type, is_func);
}

// Is a function-pointer value of type `from` assignable to target `to`?
// Equal arity, structurally-equal parameter/return types, and totality
// subtyping: a `*func` target (total) accepts only a total source; `*proc`
// accepts either.
static bool fnptr_types_assignable(Type *to, Type *from) {
    if (!to || !from || to->kind != TYPE_FUNC || from->kind != TYPE_FUNC) return false;
    if (to->func_is_total && !from->func_is_total) return false;
    if ((to->element_type == NULL) != (from->element_type == NULL)) return false;
    if (to->element_type && !types_equal_exact(to->element_type, from->element_type)) return false;
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
                "type — arity, parameter/return types, or totality (`func` vs `proc`) differ.\n",
                (long)line, (long)col);
        diagnostic_show_line(line, col); exit(1);
    }
}

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
    break;

// ...

  case EXPR_MEMBER: {
    sema_infer_expr(e->as.member_expr.target);
    Type *t = e->as.member_expr.target->type;
    
    // Case 1: Accessing ADT Variant Constructor (e.g. Shape.Circle)
    // ONLY valid if the target resolves to the Enum declaration itself!
    if (e->as.member_expr.target->kind == EXPR_IDENTIFIER || e->as.member_expr.target->kind == EXPR_TYPE) {
        if (e->as.member_expr.target->decl && e->as.member_expr.target->decl->kind == DECL_ENUM) {
            DeclEnum *adt = &e->as.member_expr.target->decl->as.enum_decl;
            Variant *v = lookup_adt_variant(adt, e->as.member_expr.member);
            if (!v) {
                fprintf(stderr, "sema error Ln %li, Col %li: ADT has no variant '%.*s'\n",
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
        // The target of a member access has no value type. The common trigger is
        // module-qualified access (`io.println(...)`) — imports are a flat
        // namespace, so `io` is not a value. Emit a diagnostic instead of the old
        // assertion abort (a compiler crash on user input).
        Expr *tgt = e->as.member_expr.target;
        int tl = (tgt && tgt->kind == EXPR_IDENTIFIER && tgt->as.identifier_expr.id)
                 ? (int)tgt->as.identifier_expr.id->length : 0;
        const char *tn = tl ? tgt->as.identifier_expr.id->name : "";
        int ml = (int)e->as.member_expr.member->length;
        const char *mn = e->as.member_expr.member->name;
        fprintf(stderr, "[E102] Error Ln %li, Col %li: cannot access member '%.*s' of "
            "'%.*s' — '%.*s' is not a value. If it is an imported module, imports share "
            "a flat namespace: call '%.*s' directly (unqualified).\n",
            e->line, e->col, ml, mn, tl, tn, tl, tn, ml, mn);
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
        fprintf(stderr, "sema error Ln %li, Col %li: variant '%.*s' has no field '%.*s'\n",
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
        // .len → integer
        e->type = get_builtin_i32_type();
        break;
      }
      if (mem && mem->length == 4 && strncmp(mem->name, "data", 4) == 0) {
        // .data → pointer to element_type
        e->type = type_pointer(sema_arena, t->element_type);
        break;
      }
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
        if (sym && sym->decl && (sym->decl->kind == DECL_FUNCTION || sym->decl->kind == DECL_PROCEDURE || sym->decl->kind == DECL_EXTERN_FUNCTION || sym->decl->kind == DECL_EXTERN_PROCEDURE)) {
            // It might be a UFCS method call (e.g., `l.consume()`).
            // We leave `e->type = NULL`. The parent `EXPR_CALL` will detect this
            // and rewrite the AST to `consume(l)`.
        } else {
            fprintf(stderr, "sema error: struct '%.*s' has no field '%.*s'\n",
                (int)t->base_type->length, t->base_type->name, 
                (int)e->as.member_expr.member->length, e->as.member_expr.member->name);
            exit(1);
        }
    }
    break;
  }

// ...

  case EXPR_CALL: {
    // ensure callee resolved & infer args
    sema_infer_expr(e->as.call_expr.callee); // Changed from resolve to infer to handle Shape.Circle

    // Call THROUGH a function pointer: the callee is a VALUE of TYPE_FUNC (a
    // variable/param/field), not a direct function reference. Check args
    // against the pointer's parameter types (same discipline as a direct call)
    // and take the result type from the pointer's return type.
    {
      Decl *cd = e->as.call_expr.callee->decl;
      bool callee_is_direct_fn = cd && (cd->kind == DECL_FUNCTION || cd->kind == DECL_PROCEDURE ||
                                        cd->kind == DECL_EXTERN_FUNCTION || cd->kind == DECL_EXTERN_PROCEDURE);
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
                     fprintf(stderr, "sema error: wrong number of arguments for variant constructor '%.*s'\n",
                             (int)v->name->length, v->name->name);
                     exit(1);
                 }
                 
                 e->type = e->as.call_expr.callee->type; // The ADT type
                 break;
             }
        }
    }
    
    // Check for UFCS: `a.method(b)` -> `method(a, b)`
    // If the callee is EXPR_MEMBER and it failed to find a field (type is NULL),
    // we assume it's a UFCS call.
    if (e->as.call_expr.callee->kind == EXPR_MEMBER && !e->as.call_expr.callee->type) {
        Expr *target = e->as.call_expr.callee->as.member_expr.target;
        Id *method_name = e->as.call_expr.callee->as.member_expr.member;
        
        char mbuf[256];
        int mlen = method_name->length < 255 ? method_name->length : 255;
        memcpy(mbuf, method_name->name, mlen);
        mbuf[mlen] = '\0';
        
        Symbol *sym = sema_lookup(mbuf);
        if (sym && sym->decl && (sym->decl->kind == DECL_FUNCTION || sym->decl->kind == DECL_PROCEDURE || sym->decl->kind == DECL_EXTERN_FUNCTION || sym->decl->kind == DECL_EXTERN_PROCEDURE)) {
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
        } else {
            fprintf(stderr, "sema error: struct field or UFCS method '%.*s' not found on type '%.*s'\n",
                    (int)method_name->length, method_name->name,
                    (int)target->type->base_type->length, target->type->base_type->name);
            exit(1);
        }
    }
    
    // Normal function call logic...
    sema_resolve_expr(e->as.call_expr.callee);
    
    // Purity check: func cannot call proc
    if (current_function_decl && current_function_decl->kind == DECL_FUNCTION) {
        Expr *callee = e->as.call_expr.callee;
        if (callee->decl) {
            if (callee->decl->kind == DECL_PROCEDURE || callee->decl->kind == DECL_EXTERN_PROCEDURE) {
                fprintf(stderr, "sema error: pure function '%.*s' cannot call procedure\n",
                        (int)current_function_decl->as.function_decl.name->length, current_function_decl->as.function_decl.name->name);
                exit(1);
            }
            
            // Termination Analysis: Ban recursion in func
            if (callee->decl == current_function_decl) {
                fprintf(stderr, "[E011] Error: recursion is not allowed in pure function '%.*s' (a `func` must be total; recursion cannot guarantee termination).\n",
                        (int)current_function_decl->as.function_decl.name->length, current_function_decl->as.function_decl.name->name);
                exit(1);
            }
        }
    }

    for (ExprList *a = e->as.call_expr.args; a; a = a->next) {
      sema_infer_expr(a->expr);
    }
    
    // Argument count check — skip extern functions (may be variadic like printf)
    {
        Decl *cd = e->as.call_expr.callee->decl;
        if (cd && (cd->kind == DECL_FUNCTION || cd->kind == DECL_PROCEDURE)) {
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
    if (callee_decl && (callee_decl->kind == DECL_FUNCTION || 
                        callee_decl->kind == DECL_PROCEDURE ||
                        callee_decl->kind == DECL_EXTERN_FUNCTION || 
                        callee_decl->kind == DECL_EXTERN_PROCEDURE)) {
        
        int param_idx = 0;
        DeclList *params = callee_decl->as.function_decl.params;
        
        for (DeclList *p = params; p; p = p->next) {
            
            // Check 'in' field constraint
            if (p->decl->kind == DECL_VARIABLE && p->decl->as.variable_decl.in_field) {
                Id *arr_name = p->decl->as.variable_decl.in_field;
                
                // Find the argument for this parameter (index)
                Expr *idx_arg = NULL;
                int a_idx = 0;
                for (ExprList *a = e->as.call_expr.args; a; a = a->next) {
                    if (a_idx == param_idx) { idx_arg = a->expr; break; }
                    a_idx++;
                }
                
                // Find the array argument by name
                int arr_param_idx = 0;
                Type *arr_type = NULL;
                Expr *arr_arg = NULL;
                for (DeclList *arr_p = params; arr_p; arr_p = arr_p->next) {
                    if (arr_p->decl->kind == DECL_VARIABLE) {
                        Id *an = arr_p->decl->as.variable_decl.name;
                        if (an->length == arr_name->length &&
                            strncmp(an->name, arr_name->name, an->length) == 0) {
                            arr_type = arr_p->decl->as.variable_decl.type;
                            // Get corresponding arg for array
                            int aa_idx = 0;
                            for (ExprList *aa = e->as.call_expr.args; aa; aa = aa->next) {
                                if (aa_idx == arr_param_idx) { arr_arg = aa->expr; break; }
                                aa_idx++;
                            }
                            break;
                        }
                    }
                    arr_param_idx++;
                }
                
                (void)arr_type;
                if (idx_arg && arr_arg && sema_walk_phase) {
                    // P2/S3 (fail-CLOSED): the `in <arr>` invariant is 0 <= idx <
                    // arr.len. idx >= 0 holds for a usize; prove idx < arr.len at the
                    // CALL site — interval for a fixed array, or difference-constraint
                    // for a symbolic slice / guarded index — else reject. The old
                    // check only fired for a known-constant length AND a known index
                    // range, so an unbounded/symbolic forward was an unchecked
                    // out-of-bounds read inside the callee (ASan-confirmed). Reuses the
                    // same prover as the `i < a.len` refinement. Walk-phase-gated so it
                    // cannot false-positive before VRA is populated.
                    Range idx_range = sema_eval_range(idx_arg, sema_ranges);
                    if (idx_range.known && idx_range.min < 0) {
                        fprintf(stderr, "[E085] Error Ln %li, Col %li: index may be negative: range [%ld, %ld].\n",
                                (long)idx_arg->line, (long)idx_arg->col,
                                (long)idx_range.min, (long)idx_range.max);
                        diagnostic_show_line(idx_arg->line, idx_arg->col);
                        exit(1);
                    }
                    // Synthetic `arr_name.len` member for the shared prover.
                    Id *lm = arena_push_aligned(sema_arena, Id);
                    lm->name = "len"; lm->length = 3;
                    Expr *aid = arena_push_aligned(sema_arena, Expr);
                    aid->kind = EXPR_IDENTIFIER; aid->as.identifier_expr.id = arr_name;
                    Expr *lenE = arena_push_aligned(sema_arena, Expr);
                    lenE->kind = EXPR_MEMBER;
                    lenE->as.member_expr.target = aid;
                    lenE->as.member_expr.member = lm;
                    if (!callsite_len_precond_proven(idx_arg, lenE,
                                TOKEN_ANGLE_BRACKET_LEFT, params, e->as.call_expr.args)) {
                        fprintf(stderr, "[E085] Error Ln %li, Col %li: index argument cannot be proven "
                            "within the bounds of '%.*s' (the `in %.*s` invariant). Constrain the index "
                            "(a literal, a bounded local, an `if`/loop guard, or a matching parameter "
                            "refinement) so VRA can discharge it.\n",
                            (long)idx_arg->line, (long)idx_arg->col,
                            (int)arr_name->length, arr_name->name,
                            (int)arr_name->length, arr_name->name);
                        diagnostic_show_line(idx_arg->line, idx_arg->col);
                        exit(1);
                    }
                }
            }


            if (p->decl->kind == DECL_VARIABLE && p->decl->as.variable_decl.constraints) {
                // ... (inner logic) ...
                // Find the argument for this parameter (LHS of constraint)
                Expr *lhs_arg = NULL;
                int a_idx = 0;
                for (ExprList *a = e->as.call_expr.args; a; a = a->next) {
                    if (a_idx == param_idx) { lhs_arg = a->expr; break; }
                    a_idx++;
                }

                if (lhs_arg) {
                    for (ExprList *c = p->decl->as.variable_decl.constraints; c; c = c->next) {
                        if (c->expr->kind == EXPR_BINARY) {
                            Expr *rhs_expr = c->expr->as.binary_expr.right;
                            Expr *rhs_arg = NULL;

                            if (rhs_expr->kind == EXPR_IDENTIFIER) {
                                int rhs_p_idx = 0;
                                for (DeclList *rp = params; rp; rp = rp->next) {
                                    Id *rp_name = rp->decl->as.variable_decl.name;
                                    if (rp_name->length == rhs_expr->as.identifier_expr.id->length && 
                                        strncmp(rp_name->name, rhs_expr->as.identifier_expr.id->name, rp_name->length) == 0) {
                                        int ra_idx = 0;
                                        for (ExprList *ra = e->as.call_expr.args; ra; ra = ra->next) {
                                            if (ra_idx == rhs_p_idx) { rhs_arg = ra->expr; break; }
                                            ra_idx++;
                                        }
                                        break;
                                    }
                                    rhs_p_idx++;
                                }
                                if (!rhs_arg) rhs_arg = rhs_expr;
                            } else if (rhs_expr->kind == EXPR_MEMBER &&
                                       rhs_expr->as.member_expr.member &&
                                       rhs_expr->as.member_expr.member->length == 3 &&
                                       strncmp(rhs_expr->as.member_expr.member->name, "len", 3) == 0) {
                                // G8 / P2-S3 (fail-CLOSED): `param.len` RHS — the passed
                                // value must be PROVEN to satisfy `arg OP arg_array.len`
                                // at the CALL site, else the callee's `a[i]` is an
                                // unchecked out-of-bounds read (confirmed OOB via ASan on
                                // an unbounded index). Proof is interval OR relational
                                // (see callsite_len_precond_proven). Gate on the walk
                                // phase: during resolve/infer the VRA table isn't built,
                                // so a fail-closed check there would false-positive; the
                                // walk phase is the final pass before emit, so an unproven
                                // precondition there is genuinely unproven.
                                if (sema_walk_phase &&
                                    !callsite_len_precond_proven(lhs_arg, rhs_expr,
                                            c->expr->as.binary_expr.op,
                                            params, e->as.call_expr.args)) {
                                    isize el = lhs_arg ? lhs_arg->line : e->line;
                                    isize ec = lhs_arg ? lhs_arg->col  : e->col;
                                    fprintf(stderr, "[E012] Error Ln %li, Col %li: Constraint violation. "
                                        "Argument cannot be proven to satisfy the '%s' refinement "
                                        "'%.*s'. Constrain the argument (a literal, a bounded local, "
                                        "an `if`/loop guard, or a matching parameter refinement) so "
                                        "VRA can discharge it.\n",
                                        (long)el, (long)ec,
                                        token_kind_to_str(c->expr->as.binary_expr.op),
                                        (int)rhs_expr->as.member_expr.target->as.identifier_expr.id->length,
                                        rhs_expr->as.member_expr.target->as.identifier_expr.id->name);
                                    diagnostic_show_line(el, ec);
                                    exit(1);
                                }
                                continue;  // handled directly; skip the sema_check_condition path
                            } else {
                                rhs_arg = rhs_expr;
                            }

                            // F-025: initialize line/col so any diagnostic
                            // path inside sema_check_condition reports a
                            // location tied to the offending argument.
                            Expr temp = {0};
                            temp.kind = EXPR_BINARY;
                            temp.as.binary_expr.op = c->expr->as.binary_expr.op;
                            temp.as.binary_expr.left = lhs_arg;
                            temp.as.binary_expr.right = rhs_arg;
                            temp.line = lhs_arg ? lhs_arg->line : e->line;
                            temp.col  = lhs_arg ? lhs_arg->col  : e->col;

                            int result = sema_check_condition(&temp, sema_ranges);
                            if (result == 0) {
                                fprintf(stderr, "[E012] Error Ln %li, Col %li: Constraint violation. Argument does not satisfy '%s' constraint.\n",
                                        (long)temp.line, (long)temp.col,
                                        token_kind_to_str(c->expr->as.binary_expr.op));
                                diagnostic_show_line(temp.line, temp.col);
                                exit(1);
                            } else if (result == -1 && sema_walk_phase &&
                                       !arg_refinement_discharges(lhs_arg,
                                               c->expr->as.binary_expr.op, rhs_arg)) {
                                // P2/S3 (fail-CLOSED): an UNPROVEN non-`.len` precondition
                                // is a rejection, not a pass — otherwise the callee runs
                                // check-free on an assumption the caller never discharged
                                // (e.g. a forwarded `b != 0` divisor -> division by zero,
                                // which the callee emits as a raw `a / b`). Gate on the
                                // walk phase so we don't false-positive before VRA is
                                // populated. The argument's own immutable refinement can
                                // still discharge it (arg_refinement_discharges); range
                                // and difference-constraint proofs are handled by
                                // sema_check_condition above (result == 1).
                                fprintf(stderr, "[E012] Error Ln %li, Col %li: Constraint violation. "
                                        "Argument cannot be proven to satisfy the '%s' refinement. "
                                        "Constrain the argument (a literal, a bounded local, an "
                                        "`if`/loop guard, or a matching parameter refinement) so VRA "
                                        "can discharge it.\n",
                                        (long)temp.line, (long)temp.col,
                                        token_kind_to_str(c->expr->as.binary_expr.op));
                                diagnostic_show_line(temp.line, temp.col);
                                exit(1);
                            }
                        }
                    }
                }
            }

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
                }
            }
            // E087: verify sized-slice length constraints at call site.
            // Conservative: only fires when a violation is statically provable.
            if (sema_walk_phase && sema_ranges && p->decl->kind == DECL_VARIABLE) {
                Type *e87_ptype = p->decl->as.variable_decl.type;
                if (e87_ptype && e87_ptype->kind == TYPE_ARRAY &&
                    e87_ptype->array_len == -1 && e87_ptype->size_expr) {
                    Expr *e87_parg = NULL;
                    { int e87_ai = 0;
                      for (ExprList *e87_a = e->as.call_expr.args; e87_a; e87_a = e87_a->next) {
                          if (e87_ai == param_idx) { e87_parg = e87_a->expr; break; }
                          e87_ai++;
                      }
                    }
                    if (e87_parg) {
                        Id *e87_pname = p->decl->as.variable_decl.name;
                        // Determine arg's concrete length as a Range
                        Range e87_alen = range_unknown();
                        if (e87_parg->type && e87_parg->type->kind == TYPE_ARRAY &&
                            e87_parg->type->array_len >= 0)
                            e87_alen = range_const(e87_parg->type->array_len);
                        if (!e87_alen.known && e87_parg->kind == EXPR_IDENTIFIER) {
                            Id *aid = e87_parg->as.identifier_expr.id;
                            char lk[272]; int lklen = 6 + (int)aid->length;
                            if (lklen < (int)sizeof(lk)) {
                                memcpy(lk, "__len_", 6);
                                memcpy(lk + 6, aid->name, aid->length);
                                for (RangeEntry *re = sema_ranges->head; re; re = re->next) {
                                    if (re->var->length == lklen &&
                                        strncmp(re->var->name, lk, lklen) == 0)
                                    { e87_alen = re->range; break; }
                                }
                            }
                        }
                        bool e87_fail = false;
                        char e87_msg[320]; e87_msg[0] = '\0';
                        if (e87_ptype->size_relop == TOKEN_ANGLE_BRACKET_RIGHT ||
                            e87_ptype->size_relop == TOKEN_ANGLE_BRACKET_RIGHT_EQUAL) {
                            // i32[> k] / i32[>= k] with literal k
                            if (e87_ptype->size_expr->kind == EXPR_LITERAL) {
                                int64_t k = e87_ptype->size_expr->as.literal_expr.value;
                                int64_t req = k + (e87_ptype->size_relop == TOKEN_ANGLE_BRACKET_RIGHT ? 1 : 0);
                                if (e87_alen.known && e87_alen.max < req) {
                                    snprintf(e87_msg, sizeof(e87_msg),
                                        "argument for '%.*s' has length at most %ld"
                                        " but constraint requires length %s %ld",
                                        (int)e87_pname->length, e87_pname->name,
                                        (long)e87_alen.max,
                                        e87_ptype->size_relop == TOKEN_ANGLE_BRACKET_RIGHT ? ">" : ">=",
                                        (long)k);
                                    e87_fail = true;
                                }
                            }
                        } else if (e87_ptype->size_relop == TOKEN_EQUAL_EQUAL) {
                            if (e87_ptype->size_expr->kind == EXPR_LITERAL) {
                                // i32[k]: arg.len must equal k
                                int64_t k = e87_ptype->size_expr->as.literal_expr.value;
                                if (e87_alen.known &&
                                    (e87_alen.min > k || e87_alen.max < k)) {
                                    snprintf(e87_msg, sizeof(e87_msg),
                                        "argument for '%.*s' has length [%ld,%ld]"
                                        " but constraint requires length == %ld",
                                        (int)e87_pname->length, e87_pname->name,
                                        (long)e87_alen.min, (long)e87_alen.max,
                                        (long)k);
                                    e87_fail = true;
                                }
                            } else if (e87_ptype->size_expr->kind == EXPR_MEMBER &&
                                       e87_ptype->size_expr->as.member_expr.member->length == 3 &&
                                       strncmp(e87_ptype->size_expr->as.member_expr.member->name, "len", 3) == 0 &&
                                       e87_ptype->size_expr->as.member_expr.target->kind == EXPR_IDENTIFIER) {
                                // i32[ref.len]: find ref param and compare concrete lengths
                                Id *ref_pid = e87_ptype->size_expr->as.member_expr.target->as.identifier_expr.id;
                                Expr *ref_arg = NULL;
                                int rpi = 0;
                                for (DeclList *rp = params; rp; rp = rp->next) {
                                    if (rp->decl->kind == DECL_VARIABLE) {
                                        Id *rpn = rp->decl->as.variable_decl.name;
                                        if (rpn->length == ref_pid->length &&
                                            strncmp(rpn->name, ref_pid->name, rpn->length) == 0) {
                                            int ri = 0;
                                            for (ExprList *ra = e->as.call_expr.args; ra; ra = ra->next) {
                                                if (ri++ == rpi) { ref_arg = ra->expr; break; }
                                            }
                                            break;
                                        }
                                    }
                                    rpi++;
                                }
                                if (ref_arg) {
                                    Range ref_len = range_unknown();
                                    if (ref_arg->type && ref_arg->type->kind == TYPE_ARRAY &&
                                        ref_arg->type->array_len >= 0)
                                        ref_len = range_const(ref_arg->type->array_len);
                                    if (!ref_len.known && ref_arg->kind == EXPR_IDENTIFIER) {
                                        Id *rid = ref_arg->as.identifier_expr.id;
                                        char rk[272]; int rklen = 6 + (int)rid->length;
                                        if (rklen < (int)sizeof(rk)) {
                                            memcpy(rk, "__len_", 6);
                                            memcpy(rk + 6, rid->name, rid->length);
                                            for (RangeEntry *re = sema_ranges->head; re; re = re->next) {
                                                if (re->var->length == rklen &&
                                                    strncmp(re->var->name, rk, rklen) == 0)
                                                { ref_len = re->range; break; }
                                            }
                                        }
                                    }
                                    // Fire only when both sides are concrete point values that differ
                                    if (e87_alen.known && ref_len.known &&
                                        e87_alen.min == e87_alen.max &&
                                        ref_len.min == ref_len.max &&
                                        e87_alen.min != ref_len.min) {
                                        Id *disp = (ref_arg->kind == EXPR_IDENTIFIER)
                                                   ? ref_arg->as.identifier_expr.id : ref_pid;
                                        snprintf(e87_msg, sizeof(e87_msg),
                                            "argument for '%.*s' has length %ld"
                                            " but constraint requires == %.*s.len (%ld)",
                                            (int)e87_pname->length, e87_pname->name,
                                            (long)e87_alen.min,
                                            (int)disp->length, disp->name,
                                            (long)ref_len.min);
                                        e87_fail = true;
                                    }
                                }
                            } else {
                                // General dependent size (e.g. `a.len + b.len`): evaluate
                                // it against the actual argument lengths and require the
                                // passed length to match. Closes the OOB where a wrong-
                                // length array is passed for `out i32[a.len + b.len]`.
                                Range req = eval_callsite_size_range(e87_ptype->size_expr,
                                                                     params, e->as.call_expr.args);
                                if (e87_alen.known && req.known &&
                                    e87_alen.min == e87_alen.max && req.min == req.max &&
                                    e87_alen.min != req.min) {
                                    snprintf(e87_msg, sizeof(e87_msg),
                                        "argument for '%.*s' has length %ld but the dependent size requires length %ld",
                                        (int)e87_pname->length, e87_pname->name,
                                        (long)e87_alen.min, (long)req.min);
                                    e87_fail = true;
                                }
                            }
                        }
                        if (e87_fail) {
                            fprintf(stderr, "[E087] Error Ln %li, Col %li: %s.\n",
                                    (long)e87_parg->line, (long)e87_parg->col, e87_msg);
                            diagnostic_show_line(e87_parg->line, e87_parg->col);
                            exit(1);
                        }
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
                    check_value_fits_type(r, field_ty, a->expr->line, a->expr->col,
                        "struct field initialization", lbuf);
                } else if (field_ty && arg_ty &&
                    !types_compatible(arg_ty, field_ty)) {
                    Id *fname = f->decl->as.variable_decl.name;
                    fprintf(stderr,
                            "[E012] Error Ln %li, Col %li: struct '%.*s' field '%.*s' type mismatch at argument %d.\n",
                            (long)e->line, (long)e->col,
                            (int)callee_decl->as.struct_decl.name->length,
                            callee_decl->as.struct_decl.name->name,
                            (int)fname->length, fname->name,
                            field_count + 1);
                    diagnostic_show_line(e->line, e->col);
                    exit(1);
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
                    check_value_fits_type(r, field_ty, a->expr->line, a->expr->col,
                        "struct field initialization", buf);
                }
                // G5: enforce field refinement constraints at construction
                // (`type Config { pct i32 >= 0 and <= 100 }`), so the invariant
                // holds for every constructed value.
                if (f->decl->as.variable_decl.constraints && sema_ranges &&
                    !sema_in_unsafe_block) {
                    Range r = (a->expr->kind == EXPR_LITERAL)
                        ? (Range){ a->expr->as.literal_expr.value, a->expr->as.literal_expr.value, true }
                        : sema_eval_range(a->expr, sema_ranges);
                    if (r.known) {
                        for (ExprList *c = f->decl->as.variable_decl.constraints; c; c = c->next) {
                            if (!c->expr || c->expr->kind != EXPR_BINARY) continue;
                            Expr *rhs = c->expr->as.binary_expr.right;
                            if (!rhs || rhs->kind != EXPR_LITERAL) continue;
                            long long k = rhs->as.literal_expr.value;
                            bool fits = true;
                            switch (c->expr->as.binary_expr.op) {
                                case TOKEN_ANGLE_BRACKET_LEFT_EQUAL:  fits = (r.max <= k); break;
                                case TOKEN_ANGLE_BRACKET_LEFT:        fits = (r.max <  k); break;
                                case TOKEN_ANGLE_BRACKET_RIGHT_EQUAL: fits = (r.min >= k); break;
                                case TOKEN_ANGLE_BRACKET_RIGHT:       fits = (r.min >  k); break;
                                case TOKEN_EQUAL_EQUAL:               fits = (r.min == k && r.max == k); break;
                                case TOKEN_BANG_EQUAL:                fits = (r.min > k || r.max < k); break;
                                default: break;
                            }
                            if (!fits) {
                                Id *fnm = f->decl->as.variable_decl.name;
                                fprintf(stderr, "[E086] Error Ln %li, Col %li: struct '%.*s' field '%.*s' "
                                    "value range [%lld, %lld] violates its refinement constraint.\n",
                                    (long)e->line, (long)e->col,
                                    (int)callee_decl->as.struct_decl.name->length,
                                    callee_decl->as.struct_decl.name->name,
                                    (int)(fnm ? fnm->length : 0), fnm ? fnm->name : "",
                                    (long long)r.min, (long long)r.max);
                                diagnostic_show_line(e->line, e->col);
                                exit(1);
                            }
                        }
                    }
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
        e->type = e->as.call_expr.callee->type;
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

    // 'in' operator: result is bool, skip other checks
    if (e->as.binary_expr.op == TOKEN_KEYWORD_IN) {
        e->type = get_builtin_i32_type();
        break;
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
            if (ltv && ltv->kind == TYPE_VECTOR) { e->type = ltv; break; }
            if (rtv && rtv->kind == TYPE_VECTOR) { e->type = rtv; break; }
        }
    }

    // Division/modulo by a definitely-zero divisor (a literal 0, or a value VRA
    // proves is exactly 0) is undefined behavior in ANY context — reject in func
    // AND proc (the func-only totality check below is a stricter superset).
    {
        TokenKind dop = e->as.binary_expr.op;
        if (dop == TOKEN_SLASH || dop == TOKEN_PERCENT) {
            Expr *rhs = e->as.binary_expr.right;
            bool is_zero = rhs && rhs->kind == EXPR_LITERAL && rhs->as.literal_expr.value == 0;
            if (!is_zero && sema_ranges && rhs) {
                Range dr = sema_eval_range(rhs, sema_ranges);
                if (dr.known && dr.min == 0 && dr.max == 0) is_zero = true;
            }
            if (is_zero) {
                fprintf(stderr, "[E015] Error Ln %li, Col %li: division or modulo by zero.\n",
                        (long)e->line, (long)e->col);
                diagnostic_show_line(e->line, e->col);
                exit(1);
            }
        }
    }

    // Shift by a constant amount that is negative or >= the bit width of the
    // left operand is undefined behavior (gcc: "shift count >= width of type").
    {
        TokenKind sop = e->as.binary_expr.op;
        if (sop == TOKEN_SHIFT_LEFT || sop == TOKEN_SHIFT_RIGHT) {
            Expr *lhs = e->as.binary_expr.left;
            Expr *rhs = e->as.binary_expr.right;
            int bits; bool sgn;
            if (rhs && rhs->kind == EXPR_LITERAL && lhs && lhs->type &&
                parse_iN_uN(lhs->type, &bits, &sgn)) {
                long long n = rhs->as.literal_expr.value;
                if (n < 0 || n >= bits) {
                    fprintf(stderr, "[E086] Error Ln %li, Col %li: shift amount %lld is out of "
                        "range for a %d-bit operand (valid range 0..%d).\n",
                        (long)e->line, (long)e->col, n, bits, bits - 1);
                    diagnostic_show_line(e->line, e->col);
                    exit(1);
                }
            }
        }
    }

    // Division/modulo by zero check in pure func (must be total).
    // Parameter constraints (e.g., `b int != 0`) guarantee safety.
    if (current_function_decl && current_function_decl->kind == DECL_FUNCTION) {
        TokenKind op = e->as.binary_expr.op;
        if (op == TOKEN_SLASH || op == TOKEN_PERCENT) {
            Range rhs_range = sema_eval_range(e->as.binary_expr.right, sema_ranges);
            
            // Check if divisor is provably non-zero
            bool proven_nonzero = false;
            
            // Case 1: VRA proves range excludes zero
            if (rhs_range.known && (rhs_range.min > 0 || rhs_range.max < 0)) {
                proven_nonzero = true;
            }
            
            // Case 2: Divisor is a parameter with `!= 0` constraint
            if (!proven_nonzero && e->as.binary_expr.right->kind == EXPR_IDENTIFIER) {
                Decl *rhs_decl = e->as.binary_expr.right->decl;
                if (rhs_decl && rhs_decl->kind == DECL_VARIABLE && rhs_decl->as.variable_decl.constraints) {
                    for (ExprList *c = rhs_decl->as.variable_decl.constraints; c; c = c->next) {
                        if (c->expr->kind == EXPR_BINARY && 
                            c->expr->as.binary_expr.op == TOKEN_BANG_EQUAL &&
                            c->expr->as.binary_expr.right->kind == EXPR_LITERAL &&
                            c->expr->as.binary_expr.right->as.literal_expr.value == 0) {
                            proven_nonzero = true;
                            break;
                        }
                    }
                }
            }
            
            if (!proven_nonzero) {
                if (!rhs_range.known) {
                    fprintf(stderr, "[E015] Error Ln %li, Col %li: potential division by zero in pure function '%.*s'. "
                            "The divisor's range is unknown. Use a constraint (e.g., `b int != 0`) to prove safety.\n",
                            (long)e->line, (long)e->col,
                            (int)current_function_decl->as.function_decl.name->length,
                            current_function_decl->as.function_decl.name->name);
                } else {
                    fprintf(stderr, "[E015] Error Ln %li, Col %li: potential division by zero in pure function '%.*s'. "
                            "Divisor range [%ld, %ld] includes zero. Use a constraint (e.g., `b int != 0`) to prove safety.\n",
                            (long)e->line, (long)e->col,
                            (int)current_function_decl->as.function_decl.name->length,
                            current_function_decl->as.function_decl.name->name,
                            (long)rhs_range.min, (long)rhs_range.max);
                }
                diagnostic_show_line(e->line, e->col);
                exit(1);
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
                    fprintf(stderr, "[E012] Error Ln %li, Col %li: operator '%s' is not "
                        "defined on %s types%s. Implement a method instead.\n",
                        (long)e->line, (long)e->col, token_kind_to_str(bop), what,
                        eqop ? " (compare a slice against a string literal, or write a helper)" : "");
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
        if (is_cmp || bop == TOKEN_KEYWORD_AND || bop == TOKEN_KEYWORD_OR) {
            e->type = get_builtin_i32_type();
        } else {
            Type *lt = e->as.binary_expr.left->type;
            Type *rt = e->as.binary_expr.right->type;
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
                                || aop == TOKEN_ASTERISK_QUESTION);
            if (is_wrap_or_sat && lt && is_integer_type(lt)) {
                e->type = lt;
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
            } else if (lt && rt && is_integer_type(lt) && is_integer_type(rt)) {
                // F3.5 Path-F: +,-,* widen to a type that holds the result (no
                // op-level overflow); other ops (/, %, &, |, ^, <<, >>) keep the
                // max-operand rule (their result already fits the wider operand).
                if (aop == TOKEN_PLUS || aop == TOKEN_MINUS || aop == TOKEN_ASTERISK)
                    e->type = path_f_result_type(lt, rt, aop);
                else
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
            if (aop2 == TOKEN_ASTERISK) {
                Range lrg = sema_eval_range(e->as.binary_expr.left, sema_ranges);
                Range rrg = sema_eval_range(e->as.binary_expr.right, sema_ranges);
                long long tlo, thi;
                if (lrg.known && rrg.known && type_integer_range(check_ty, &tlo, &thi)) {
                    __int128 c1 = (__int128)lrg.min * (__int128)rrg.min;
                    __int128 c2 = (__int128)lrg.min * (__int128)rrg.max;
                    __int128 c3 = (__int128)lrg.max * (__int128)rrg.min;
                    __int128 c4 = (__int128)lrg.max * (__int128)rrg.max;
                    __int128 pmin = c1, pmax = c1;
                    if (c2 < pmin) pmin = c2;
                    if (c2 > pmax) pmax = c2;
                    if (c3 < pmin) pmin = c3;
                    if (c3 > pmax) pmax = c3;
                    if (c4 < pmin) pmin = c4;
                    if (c4 > pmax) pmax = c4;
                    if (pmin < (__int128)tlo || pmax > (__int128)thi) {
                        fprintf(stderr, "[E086] Error Ln %li, Col %li: multiplication would "
                            "overflow target type — the product range exceeds the type's range. "
                            "Use a wrapping (*%%) or saturating (*|) operator, widen the type, or "
                            "constrain the operands.\n", dl, dc);
                        diagnostic_show_line(dl, dc);
                        exit(1);
                    }
                }
            }
            check_value_fits_type(rr, check_ty, dl, dc, "arithmetic result of", "");
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
             fprintf(stderr, "sema error: internal: deref operand untyped\n");
             exit(1);
        }
        
        Type *t = e->as.unary_expr.right->type;
        while (t && t->kind == TYPE_COMPTIME) {
            t = t->element_type;
        }
        
        if (t->kind == TYPE_POINTER) {
            e->type = t->element_type;
            if (!sema_in_unsafe_block) {
                fprintf(stderr, "sema error: Dereference of raw pointer outside 'unsafe' block.\n");
                exit(1);
            }
        } else {
             // Non-pointer deref??
             // Likely a parsing error or a reference deref if supported.
             // For now, assume it results in element type if we can determine it, 
             // or just int if unknown.
             e->type = get_builtin_i32_type();
        }
    } else {
        e->type = get_builtin_i32_type();
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
                check_value_fits_type(rr, ot, dl, dc, "negation result of", "");
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

    // FIXED-LENGTH: no sentinel data, just record the length
    slice_ty->sentinel_str = NULL;
    slice_ty->sentinel_len = (isize)L;
    slice_ty->sentinel_is_string = false;

    e->type = slice_ty;
    break;
  }

  case EXPR_ARRAY_LITERAL: {
    ExprList *elems = e->as.array_literal_expr.elements;
    if (!elems) {
        fprintf(stderr, "sema error Ln %li, Col %li: empty array literal\n", e->line, e->col);
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
        fprintf(stderr, "sema error Ln %li, Col %li: array comprehension requires a range `start..end`\n", e->line, e->col);
        diagnostic_show_line(e->line, e->col); exit(1);
    }
    Expr *lo = range->as.range_expr.start, *hi = range->as.range_expr.end;
    if (!lo || lo->kind != EXPR_LITERAL || !hi || hi->kind != EXPR_LITERAL) {
        fprintf(stderr, "sema error Ln %li, Col %li: array comprehension range bounds must be integer literals\n", e->line, e->col);
        diagnostic_show_line(e->line, e->col); exit(1);
    }
    isize n = (isize)hi->as.literal_expr.value - (isize)lo->as.literal_expr.value;
    if (range->as.range_expr.inclusive) n += 1;
    if (n <= 0) {
        fprintf(stderr, "sema error Ln %li, Col %li: array comprehension length must be positive\n", e->line, e->col);
        diagnostic_show_line(e->line, e->col); exit(1);
    }
    e->type = type_array(sema_arena, body->type, n);
    break;
  }

  case EXPR_LITERAL:
    e->type = get_builtin_i32_type();
    break;

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
            if (!sema_in_unsafe_block &&
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
            bool guarded = sema_is_in_guarded(e->as.index_expr.index, e->as.index_expr.target);
            if (guarded) {
                /* bounds proven by 'in' guard — skip check */
            } else {
                sema_check_bounds(sema_ranges, e->as.index_expr.index, t, e->as.index_expr.target, _is_addr_of);
            }
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
                fprintf(stderr, "[E085] bounds error Ln %li, Col %li: vector lane %lld is out of "
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
        fprintf(stderr, "sema error: indexing non-array/slice type\n");
        // exit(1); // Optional: be strict
    }
    break;
  }


  case EXPR_MATCH: {
    sema_infer_expr(e->as.match_expr.value);
    Type *inferred_type = NULL;
    for (ExprMatchCase *c = e->as.match_expr.cases; c; c = c->next) {
        sema_push_scope();
        for (ExprList *p = c->patterns; p; p = p->next) {
            sema_infer_expr(p->expr);
        }
        sema_infer_expr(c->body);
        sema_pop_scope();
        
        if (!inferred_type && c->body->type) {
            inferred_type = c->body->type;
        }
    }
    
    if (!sema_check_expr_match_exhaustive(e)) {
        fprintf(stderr, "[E014] Error Ln %li, Col %li: non-exhaustive match expression\n", e->line, e->col);
        diagnostic_show_line(e->line, e->col);
        exit(1);
    }
    
    // If inferred type is a fixed-length string literal type (TYPE_SLICE with
    // sentinel_str==NULL, sentinel_len>0), unify to u8[:0] so case expressions
    // with string arms of different lengths share a common result type.
    if (inferred_type && inferred_type->kind == TYPE_SLICE &&
        !inferred_type->sentinel_is_string &&
        inferred_type->sentinel_str == NULL &&
        inferred_type->sentinel_len > 0) {
        Type *slice_ty = arena_push_aligned(sema_arena, Type);
        slice_ty->kind       = TYPE_SLICE;
        slice_ty->mode       = MODE_SHARED;
        slice_ty->element_type = inferred_type->element_type;
        slice_ty->sentinel_str       = "0";
        slice_ty->sentinel_len       = 1;
        slice_ty->sentinel_is_string = false;
        inferred_type = slice_ty;
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
    Type *inner = e->as.addr_expr.expr ? e->as.addr_expr.expr->type : NULL;
    if (inner) {
        e->type = type_pointer(sema_arena, inner);
        // Propagate mutability from the indexed array.
        Expr *addr_inner = e->as.addr_expr.expr;
        if (addr_inner->kind == EXPR_INDEX && addr_inner->as.index_expr.target) {
            Type *arr_ty = addr_inner->as.index_expr.target->type;
            if (arr_ty && (arr_ty->mode == MODE_MUTABLE || arr_ty->mode == MODE_OWNED)) {
                e->type->mode = MODE_MUTABLE; // mutable borrow: int32_t *, not linear
            }
        }
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
                "Use 'unsafe { }' or declare the pointer with 'var p *T in arr' and guard with 'p in arr'.\n",
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
        if (opU) for (Variant *v = opU->as.enum_decl.variants; v; v = v->next) {
            if (v->fields) continue;                 // skip the `__payload` variant
            bool in_ret = false;
            if (retU) for (Variant *w = retU->as.enum_decl.variants; w; w = w->next) {
                if (w->fields) continue;
                if (w->name->length == v->name->length &&
                    memcmp(w->name->name, v->name->name, (size_t)v->name->length) == 0) { in_ret = true; break; }
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
        static Type *u32_ty = NULL;
        if (!u32_ty) {
            Id *uid = arena_push_aligned(sema_arena, Id);
            uid->name = "u32"; uid->length = 3;
            u32_ty = type_simple(sema_arena, uid);
        }
        e->type = u32_ty;
    } else if (bk == BUILTIN_LOAD || bk == BUILTIN_SPLAT || bk == BUILTIN_STORE ||
               bk == BUILTIN_SHUFFLE) {
        if (e->as.builtin_expr.arg)  sema_infer_expr(e->as.builtin_expr.arg);
        if (e->as.builtin_expr.arg2) sema_infer_expr(e->as.builtin_expr.arg2);
        if (e->as.builtin_expr.arg3) sema_infer_expr(e->as.builtin_expr.arg3);
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
                if (off_unsigned && sema_is_in_guarded(last, e->as.builtin_expr.arg)) {
                    /* proven via the last-byte in-guard — no check, no error */
                } else {
                    sema_check_bounds(sema_ranges, off,  bt, e->as.builtin_expr.arg, false);
                    sema_check_bounds(sema_ranges, last, bt, e->as.builtin_expr.arg, false);
                }
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
