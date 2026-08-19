#ifndef SEMA_RANGES_H
#define SEMA_RANGES_H

#include "../ast.h"
#include <stdint.h>
#include <stdbool.h>
#include <limits.h>

// Saturating arithmetic to prevent int64_t overflow in range calculations
static inline int64_t sat_add_i64(int64_t a, int64_t b) {
    if (b > 0 && a > INT64_MAX - b) return INT64_MAX;
    if (b < 0 && a < INT64_MIN - b) return INT64_MIN;
    return a + b;
}
static inline int64_t sat_sub_i64(int64_t a, int64_t b) {
    if (b < 0 && a > INT64_MAX + b) return INT64_MAX;
    if (b > 0 && a < INT64_MIN + b) return INT64_MIN;
    return a - b;
}

// F-037: saturating multiply. Prevents silent int64 wrap inside range_mul.
static inline int64_t sat_mul_i64(int64_t a, int64_t b) {
    if (a == 0 || b == 0) return 0;
    // Special-case to avoid INT64_MIN / -1 UB inside the general check.
    if (a == INT64_MIN) return (b > 0) ? INT64_MIN : INT64_MAX;
    if (b == INT64_MIN) return (a > 0) ? INT64_MIN : INT64_MAX;
    bool neg = (a < 0) ^ (b < 0);
    int64_t abs_a = a < 0 ? -a : a;
    int64_t abs_b = b < 0 ? -b : b;
    if (abs_a > INT64_MAX / abs_b) return neg ? INT64_MIN : INT64_MAX;
    return a * b;
}

// F-037: saturating divide. Guards the INT64_MIN / -1 UB edge case.
static inline int64_t sat_div_i64(int64_t a, int64_t b) {
    if (b == 0) return 0; // caller ensures non-zero range before dispatch
    if (a == INT64_MIN && b == -1) return INT64_MAX;
    return a / b;
}

// Simple range: [min, max] inclusive
typedef struct {
    int64_t min;
    int64_t max;
    bool known; // true if we have info, false if unknown (unbounded)
} Range;

static Range range_unknown(void) {
    return (Range){0, 0, false};
}

static Range range_const(int64_t val) {
    return (Range){val, val, true};
}

static Range range_make(int64_t min, int64_t max) {
    return (Range){min, max, true};
}

// Range arithmetic (saturating to prevent int64_t overflow)
static Range range_add(Range a, Range b) {
    if (!a.known || !b.known) return range_unknown();
    return range_make(sat_add_i64(a.min, b.min), sat_add_i64(a.max, b.max));
}

static Range range_sub(Range a, Range b) {
    if (!a.known || !b.known) return range_unknown();
    return range_make(sat_sub_i64(a.min, b.max), sat_sub_i64(a.max, b.min));
}

static Range range_mul(Range a, Range b) {
    if (!a.known || !b.known) return range_unknown();
    // F-037: use saturating multiply on every corner to avoid silent wrap.
    int64_t p1 = sat_mul_i64(a.min, b.min), p2 = sat_mul_i64(a.min, b.max);
    int64_t p3 = sat_mul_i64(a.max, b.min), p4 = sat_mul_i64(a.max, b.max);
    int64_t lo = p1, hi = p1;
    if (p2 < lo) lo = p2;
    if (p2 > hi) hi = p2;
    if (p3 < lo) lo = p3;
    if (p3 > hi) hi = p3;
    if (p4 < lo) lo = p4;
    if (p4 > hi) hi = p4;
    return range_make(lo, hi);
}

static Range range_div(Range a, Range b) {
    if (!a.known || !b.known) return range_unknown();
    // Division by range containing zero is undefined
    if (b.min <= 0 && b.max >= 0) return range_unknown();
    // F-037: sat_div_i64 guards INT64_MIN / -1.
    int64_t p1 = sat_div_i64(a.min, b.min), p2 = sat_div_i64(a.min, b.max);
    int64_t p3 = sat_div_i64(a.max, b.min), p4 = sat_div_i64(a.max, b.max);
    int64_t lo = p1, hi = p1;
    if (p2 < lo) lo = p2;
    if (p2 > hi) hi = p2;
    if (p3 < lo) lo = p3;
    if (p3 > hi) hi = p3;
    if (p4 < lo) lo = p4;
    if (p4 > hi) hi = p4;
    return range_make(lo, hi);
}

static Range range_mod(Range a, Range b) {
    if (!a.known || !b.known) return range_unknown();
    if (b.min <= 0 && b.max >= 0) return range_unknown();
    // Result of a % b is in [0, |b|-1] for non-negative a, broader otherwise
    int64_t abs_b_max = b.max > -b.min ? b.max : -b.min;
    if (a.min >= 0) return range_make(0, abs_b_max - 1);
    return range_make(-(abs_b_max - 1), abs_b_max - 1);
}

// Bitwise AND on non-negative operands: `x & y` clears bits, so it cannot exceed
// either operand — [0, min(max_x, max_y)]. This is what makes masked reads like
// `CTYPE[c] & FLAG` provably small. Negative operands (two's complement) bail.
static Range range_bitand(Range a, Range b) {
    if (!a.known || !b.known || a.min < 0 || b.min < 0) return range_unknown();
    return range_make(0, a.max < b.max ? a.max : b.max);
}
// Bitwise OR on non-negative operands: `x | y` ≥ max(x, y) ≥ max(min_x, min_y),
// and `x | y` ≤ x + y ≤ max_x + max_y. Loose but safe (exact for single values).
static Range range_bitor(Range a, Range b) {
    if (!a.known || !b.known || a.min < 0 || b.min < 0) return range_unknown();
    return range_make(a.min > b.min ? a.min : b.min, sat_add_i64(a.max, b.max));
}

// Map from Variable Id* to Range
typedef struct RangeEntry {
    Id *var;
    Range range;
    struct RangeEntry *next;
} RangeEntry;

// Constraint: v1 - v2 <= max_diff
typedef struct ConstraintEntry {
    Id *v1;
    Id *v2;
    int64_t max_diff;
    struct ConstraintEntry *next;
} ConstraintEntry;

typedef struct {
    RangeEntry *head;
    ConstraintEntry *constraints; // New: List of relational constraints
    Arena *arena;
} RangeTable;

static RangeTable *range_table_new(Arena *arena) {
    RangeTable *t = arena_push_aligned(arena, RangeTable);
    t->head = NULL;
    t->constraints = NULL;
    t->arena = arena;
    return t;
}

static void range_set(RangeTable *t, Id *var, Range r) {
    if (!t || !var) return;
    // Always push new entry to support shadowing/scoping
    RangeEntry *e = arena_push_aligned(t->arena, RangeEntry);
    e->var = var;
    e->range = r;
    e->next = t->head;
    t->head = e;
}

static Range range_get(RangeTable *t, Id *var) {
    if (!t || !var) return range_unknown();
    for (RangeEntry *e = t->head; e; e = e->next) {
        if (e->var->length == var->length &&
            strncmp(e->var->name, var->name, var->length) == 0) {
            return e->range;
        }
    }
    return range_unknown();
}

// B.2 forward declare: derive a Range from a callee's return_constraints
// so that call expressions can participate in VRA.
static Range sema_range_from_return_constraints(Decl *callee_decl);

// Build a range from a refinement/field constraint list, where each entry is
// `<var> <relop> LITERAL` (the field-invariant form). Lets a refined struct field
// carry its invariant to use sites (e.g. `a[b.v]` with `v i32 >= 0 and <= 3`).
// P2/S3 (one source of truth): apply ONE interval constraint `ν op val`
// (op ∈ >, >=, <, <=, ==) to a range in place, tightening it. Returns true iff
// `op` is an interval op (so callers can track "did anything refine?"). `!=` is
// deliberately NOT handled here — it is not an interval, and each caller applies
// its own disequality policy (the alias-interval derivation drops it; the
// RangeTable seeding tightens a matching boundary). Shared by
// range_from_refinement_constraints and sema_apply_constraint, which each had
// their own identical copy of this switch.
static bool range_tighten_interval(Range *r, TokenKind op, int64_t val) {
    switch (op) {
        case TOKEN_ANGLE_BRACKET_RIGHT:       if (val + 1 > r->min) r->min = val + 1; return true;
        case TOKEN_ANGLE_BRACKET_RIGHT_EQUAL: if (val     > r->min) r->min = val;     return true;
        case TOKEN_ANGLE_BRACKET_LEFT:        if (val - 1 < r->max) r->max = val - 1; return true;
        case TOKEN_ANGLE_BRACKET_LEFT_EQUAL:  if (val     < r->max) r->max = val;     return true;
        case TOKEN_EQUAL_EQUAL:               r->min = val; r->max = val;             return true;
        default:                              return false;
    }
}

// Mirror a relational operator across its operands (`val op x` ⟺ `x flip(op) val`),
// so a literal-on-the-left constraint reuses the identifier-on-the-left mapping.
static TokenKind relop_flip(TokenKind op) {
    switch (op) {
        case TOKEN_ANGLE_BRACKET_LEFT:        return TOKEN_ANGLE_BRACKET_RIGHT;
        case TOKEN_ANGLE_BRACKET_RIGHT:       return TOKEN_ANGLE_BRACKET_LEFT;
        case TOKEN_ANGLE_BRACKET_LEFT_EQUAL:  return TOKEN_ANGLE_BRACKET_RIGHT_EQUAL;
        case TOKEN_ANGLE_BRACKET_RIGHT_EQUAL: return TOKEN_ANGLE_BRACKET_LEFT_EQUAL;
        default:                              return op;   // ==, != are symmetric
    }
}

static Range range_from_refinement_constraints(ExprList *constraints) {
    if (!constraints) return range_unknown();
    Range r = range_make(INT64_MIN, INT64_MAX);
    bool refined = false;
    for (ExprList *c = constraints; c; c = c->next) {
        if (!c->expr || c->expr->kind != EXPR_BINARY) continue;
        Expr *rhs = c->expr->as.binary_expr.right;
        if (!rhs || rhs->kind != EXPR_LITERAL) continue;
        int64_t k = (int64_t)rhs->as.literal_expr.value;
        // `!=` is dropped here (returns false) — unchanged behavior.
        if (range_tighten_interval(&r, c->expr->as.binary_expr.op, k)) refined = true;
    }
    return refined ? r : range_unknown();
}
// Defined in typecheck.h (needs find_struct_decl): the refinement constraints of
// struct field `field` on a value of `struct_type`, or NULL.
static ExprList *sema_member_field_constraints(Type *struct_type, Id *field);

static Range sema_eval_range(Expr *e, RangeTable *t) {
    if (!e) return range_unknown();
    switch (e->kind) {
        case EXPR_LITERAL: return range_const(e->as.literal_expr.value);
        case EXPR_IDENTIFIER: {
            Range rg = range_get(t, e->as.identifier_expr.id);
            if (rg.known) return rg;
            // Not in the local range table — a top-level constant contributes its
            // value (a literal init) or, failing that, its declared integer type
            // range, so arithmetic on constants stays provable (no false E086).
            Decl *cd = e->decl;
            if (cd && cd->kind == DECL_VARIABLE && !cd->as.variable_decl.is_mutable &&
                !cd->as.variable_decl.is_parameter && cd->as.variable_decl.init) {
                // A top-level constant (has an initializer, not a parameter): use
                // its literal value, or a NARROW declared type's range. A wide
                // type's full range would make ordinary arithmetic look
                // overflowing, so leave those unknown.
                Expr *ci = cd->as.variable_decl.init;
                if (ci->kind == EXPR_LITERAL) return range_const(ci->as.literal_expr.value);
                extern int type_integer_range(Type *ty, long long *lo, long long *hi);
                long long lo, hi;
                if (cd->as.variable_decl.type &&
                    type_integer_range(cd->as.variable_decl.type, &lo, &hi) &&
                    lo >= -32768 && hi <= 65535)
                    return range_make(lo, hi);
            }
            return rg;
        }
        case EXPR_INDEX: {
            // An element read of a NARROW-element array (`u8[N]` → [0,255]) has a
            // small provable range. For 32-bit+ element types, keep it unknown:
            // a full-width range would make ordinary `a[i]+a[j]` look overflowing
            // (a false E086) — the old, safe "unanalyzable, skip" behavior.
            Type *tt = e->as.index_expr.target ? e->as.index_expr.target->type : NULL;
            while (tt && tt->kind == TYPE_COMPTIME) tt = tt->element_type;
            Type *et = (tt && (tt->kind == TYPE_ARRAY || tt->kind == TYPE_SLICE ||
                               tt->kind == TYPE_VECTOR))   // a SIMD lane has the element's range
                       ? tt->element_type : NULL;
            extern int type_integer_range(Type *ty, long long *lo, long long *hi);
            long long lo, hi;
            if (et && type_integer_range(et, &lo, &hi) &&
                lo >= -32768 && hi <= 65535)   // ≤16-bit: safe to fold, no false overflow
                return range_make(lo, hi);
            return range_unknown();
        }
        case EXPR_BINARY: {
            Range l = sema_eval_range(e->as.binary_expr.left, t);
            Range r = sema_eval_range(e->as.binary_expr.right, t);

            // Q-002: wrapping/saturating ops produce a value bounded by the
            // LHS type's range — wrap reduces modulo 2^N, saturate clamps.
            // Either way the result fits in the LHS type, so we clamp.
            extern int type_integer_range(Type *t, long long *lo, long long *hi);
            TokenKind op = e->as.binary_expr.op;
            bool is_wrap_or_sat = (op == TOKEN_PLUS_PERCENT  || op == TOKEN_MINUS_PERCENT
                                || op == TOKEN_ASTERISK_PERCENT || op == TOKEN_PLUS_PIPE
                                || op == TOKEN_MINUS_PIPE || op == TOKEN_ASTERISK_PIPE);
            if (is_wrap_or_sat && e->as.binary_expr.left && e->as.binary_expr.left->type) {
                long long lo, hi;
                if (type_integer_range(e->as.binary_expr.left->type, &lo, &hi)) {
                    bool is_sat = (op == TOKEN_PLUS_PIPE || op == TOKEN_MINUS_PIPE
                                || op == TOKEN_ASTERISK_PIPE);
                    if (is_sat) {
                        // Saturating clamps to the type range, so compute the raw
                        // arithmetic range and intersect it with [lo, hi]. This
                        // keeps sign/magnitude info that the coarse full-type range
                        // would lose — e.g. `0 -| x` for x < 0 is provably in
                        // [1, type_max], which lets `abs` satisfy an `i32 >= 0`
                        // return refinement even at INT_MIN (clamped to INT_MAX).
                        Range raw;
                        switch (op) {
                            case TOKEN_PLUS_PIPE:     raw = range_add(l, r); break;
                            case TOKEN_MINUS_PIPE:    raw = range_sub(l, r); break;
                            case TOKEN_ASTERISK_PIPE: raw = range_mul(l, r); break;
                            default:                  raw = range_make(lo, hi); break;
                        }
                        int64_t cmin = raw.min < (int64_t)lo ? (int64_t)lo : raw.min;
                        int64_t cmax = raw.max > (int64_t)hi ? (int64_t)hi : raw.max;
                        return range_make(cmin, cmax);
                    }
                    // Wrapping (modulo 2^N): any value in the type is possible.
                    return range_make(lo, hi);
                }
            }

            switch (op) {
                case TOKEN_PLUS:            return range_add(l, r);
                case TOKEN_MINUS:           return range_sub(l, r);
                case TOKEN_ASTERISK:        return range_mul(l, r);
                case TOKEN_SLASH:           return range_div(l, r);
                case TOKEN_PERCENT:         return range_mod(l, r);
                case TOKEN_AMPERSAND:       return range_bitand(l, r);
                case TOKEN_PIPE:            return range_bitor(l, r);
                default:                    return range_unknown();
            }
        }
        case EXPR_UNARY: {
            if (e->as.unary_expr.op == TOKEN_MINUS) {
                Range r = sema_eval_range(e->as.unary_expr.right, t);
                if (r.known) {
                    return range_make(sat_sub_i64(0, r.max), sat_sub_i64(0, r.min));
                }
            }
            return range_unknown();
        }
        case EXPR_CAST: {
            // Q-002: an explicit `as` cast intersects the source range with
            // the target type's range. The user has consented to truncation
            // (if any), so the resulting value is bounded by the target type.
            Range src = sema_eval_range(e->as.cast_expr.expr, t);
            Type *target = e->as.cast_expr.target_type;
            extern int type_integer_range(Type *t, long long *lo, long long *hi);
            long long tlo, thi;
            if (target && type_integer_range(target, &tlo, &thi)) {
                // If source range is unknown, fall back to target range
                if (!src.known) return range_make(tlo, thi);
                long long mn = src.min < tlo ? tlo : src.min;
                long long mx = src.max > thi ? thi : src.max;
                if (mn > mx) {
                    // Empty intersection: the cast wraps/truncates; conservative
                    // fall back to target range.
                    return range_make(tlo, thi);
                }
                return range_make(mn, mx);
            }
            return src;
        }
        case EXPR_CALL: {
            // B.2: interprocedural VRA via return_constraints.
            Decl *callee_decl = e->as.call_expr.callee ? e->as.call_expr.callee->decl : NULL;
            if (callee_decl && (callee_decl->kind == DECL_FUNCTION ||
                                callee_decl->kind == DECL_PROCEDURE)) {
                return sema_range_from_return_constraints(callee_decl);
            }
            return range_unknown();
        }
        case EXPR_BUILTIN: {
            switch (e->as.builtin_expr.builtin_kind) {
                case BUILTIN_CTZ: case BUILTIN_CLZ: case BUILTIN_POPCOUNT:
                    // A bit count / index of a ≤64-bit word is in [0, 64].
                    return range_make(0, 64);
                case BUILTIN_MOVEMASK: {
                    // An N-lane mask occupies N low bits → [0, 2^N − 1].
                    Type *at = e->as.builtin_expr.arg ? e->as.builtin_expr.arg->type : NULL;
                    long n = (at && at->kind == TYPE_VECTOR) ? (long)at->array_len : 32;
                    if (n >= 32) return range_make(0, 4294967295LL);
                    return range_make(0, (1LL << n) - 1);
                }
                case BUILTIN_LIKELY: case BUILTIN_UNLIKELY:
                    // Transparent wrapper — the inner expression's range.
                    return e->as.builtin_expr.arg
                         ? sema_eval_range(e->as.builtin_expr.arg, t) : range_unknown();
                default:
                    return range_unknown();
            }
        }
        case EXPR_MEMBER: {
            // Sized-slice VRA: look up __len_PARAM synthetic entry for foo.len
            Expr *tgt = e->as.member_expr.target;
            Id   *mem = e->as.member_expr.member;
            if (tgt->kind == EXPR_IDENTIFIER &&
                mem->length == 3 && strncmp(mem->name, "len", 3) == 0) {
                Id *obj = tgt->as.identifier_expr.id;
                char key[272];
                int klen = 6 + (int)obj->length;
                if (klen < (int)sizeof(key)) {
                    memcpy(key, "__len_", 6);
                    memcpy(key + 6, obj->name, obj->length);
                    for (RangeEntry *re = t->head; re; re = re->next) {
                        if (re->var->length == klen &&
                            strncmp(re->var->name, key, klen) == 0) {
                            return re->range;
                        }
                    }
                }
            }
            // Refined struct field read: `b.v` where field v has an invariant
            // (`v i32 >= 0 and <= 3`). Seed its declared range so use sites can use
            // it (the invariant was previously only checked at construction).
            if (tgt->type) {
                Type *st = tgt->type;
                while (st && st->kind == TYPE_COMPTIME) st = st->element_type;
                if (st && st->kind == TYPE_SIMPLE && st->base_type) {
                    ExprList *fc = sema_member_field_constraints(st, mem);
                    if (fc) return range_from_refinement_constraints(fc);
                }
            }
            return range_unknown();
        }
        default: return range_unknown();
    }
}

// Union of two ranges (widen): the tightest range containing both.
static Range range_join(Range a, Range b) {
    if (!a.known || !b.known) return range_unknown();
    return range_make(a.min < b.min ? a.min : b.min, a.max > b.max ? a.max : b.max);
}

// Recursion guard for body-level return inference (a function returning a call
// to itself would loop). Small in-progress stack; a re-entered callee bails.
#define RET_INFER_MAX 32
static Decl *ret_in_progress[RET_INFER_MAX];
static int   ret_in_progress_n = 0;
static bool ret_infer_reentrant(Decl *d) {
    for (int i = 0; i < ret_in_progress_n; i++) if (ret_in_progress[i] == d) return true;
    return false;
}

// Join the ranges of every `return <expr>` in a statement list, recursing into
// control flow. `*any` records that at least one return was seen; `*bail` is set
// if any return range is unknown (⇒ the whole function's range is unknown).
static void ret_collect(StmtList *body, RangeTable *t, Range *acc, bool *any, bool *bail) {
    for (StmtList *sl = body; sl && !*bail; sl = sl->next) {
        Stmt *s = sl->stmt;
        if (!s) continue;
        switch (s->kind) {
            case STMT_RETURN:
                if (!s->as.return_stmt.value) { *bail = true; return; }
                {
                    Range r = sema_eval_range(s->as.return_stmt.value, t);
                    if (!r.known) { *bail = true; return; }
                    *acc = *any ? range_join(*acc, r) : r;
                    *any = true;
                }
                break;
            case STMT_IF:
                ret_collect(s->as.if_stmt.then_body, t, acc, any, bail);
                ret_collect(s->as.if_stmt.else_branch, t, acc, any, bail);
                break;
            case STMT_MATCH:
                for (StmtMatchCase *c = s->as.match_stmt.cases; c && !*bail; c = c->next)
                    ret_collect(c->body, t, acc, any, bail);
                break;
            case STMT_UNSAFE: ret_collect(s->as.unsafe_stmt.body, t, acc, any, bail); break;
            case STMT_FOR:    ret_collect(s->as.for_stmt.body, t, acc, any, bail);   break;
            case STMT_WHILE:  ret_collect(s->as.while_stmt.body, t, acc, any, bail); break;
            default: break;
        }
    }
}

// Infer a function's return range from its body — the join of its return
// expressions, evaluated with narrow (≤16-bit) params seeded to their type
// range. Tighter than the return type alone (`c & 0x0F` → [0,15], not [0,255]).
static Range sema_range_infer_body(Decl *fn) {
    if (!fn || fn->kind != DECL_FUNCTION || ret_infer_reentrant(fn) ||
        ret_in_progress_n >= RET_INFER_MAX)
        return range_unknown();
    extern int type_integer_range(Type *ty, long long *lo, long long *hi);
    RangeTable *t = range_table_new(sema_arena);
    for (DeclList *p = fn->as.function_decl.params; p; p = p->next) {
        if (!p->decl || p->decl->kind != DECL_VARIABLE) continue;
        Type *pt = p->decl->as.variable_decl.type;
        long long lo, hi;
        if (pt && type_integer_range(pt, &lo, &hi) && lo >= -32768 && hi <= 65535)
            range_set(t, p->decl->as.variable_decl.name, range_make(lo, hi));
    }
    ret_in_progress[ret_in_progress_n++] = fn;
    Range acc = range_unknown(); bool any = false, bail = false;
    ret_collect(fn->as.function_decl.body, t, &acc, &any, &bail);
    ret_in_progress_n--;
    return (any && !bail) ? acc : range_unknown();
}

static Range sema_range_from_return_constraints(Decl *callee_decl) {
    if (!callee_decl) return range_unknown();
    ExprList *rc = callee_decl->as.function_decl.return_constraints;
    // Seed with full int range; refine by each declared constraint on `result`.
    Range r = range_make(INT64_MIN, INT64_MAX);
    bool refined = false;
    for (ExprList *c = rc; c; c = c->next) {
        if (!c->expr || c->expr->kind != EXPR_BINARY) continue;
        Expr *lhs = c->expr->as.binary_expr.left;
        Expr *rhs = c->expr->as.binary_expr.right;
        if (!lhs || lhs->kind != EXPR_IDENTIFIER) continue;
        // Must refer to the magic `result` identifier synthesised by the parser.
        if (lhs->as.identifier_expr.id->length != 6 ||
            strncmp(lhs->as.identifier_expr.id->name, "result", 6) != 0) continue;
        if (!rhs || rhs->kind != EXPR_LITERAL) continue; // only literal RHS for now
        int64_t val = (int64_t)rhs->as.literal_expr.value;
        switch (c->expr->as.binary_expr.op) {
            case TOKEN_ANGLE_BRACKET_RIGHT:        if (val + 1 > r.min) r.min = val + 1; refined = true; break;
            case TOKEN_ANGLE_BRACKET_RIGHT_EQUAL:  if (val > r.min)     r.min = val;     refined = true; break;
            case TOKEN_ANGLE_BRACKET_LEFT:         if (val - 1 < r.max) r.max = val - 1; refined = true; break;
            case TOKEN_ANGLE_BRACKET_LEFT_EQUAL:   if (val < r.max)     r.max = val;     refined = true; break;
            case TOKEN_EQUAL_EQUAL:                r.min = val; r.max = val;             refined = true; break;
            default: break;
        }
    }
    if (refined) return r;
    // No declared refinement. Try body-level inference (the join of the return
    // expressions with narrow params seeded) — tighter than the type. Then fall
    // back to the return TYPE's range, but only for NARROW integer returns
    // (u8/u16/i8/i16/bool): a wide full range would make ordinary caller
    // arithmetic look overflowing (a false E086), so those stay unknown.
    Range body_r = sema_range_infer_body(callee_decl);
    if (body_r.known) return body_r;
    Type *rt = callee_decl->as.function_decl.return_type;
    extern int type_integer_range(Type *ty, long long *lo, long long *hi);
    long long lo, hi;
    if (rt && type_integer_range(rt, &lo, &hi) && lo >= -32768 && hi <= 65535)
        return range_make(lo, hi);
    return range_unknown();
}

// Add or update a constraint: v1 - v2 <= max_diff
static void constraint_add(RangeTable *t, Id *v1, Id *v2, int64_t max_diff) {
    if (!t || !v1 || !v2) return;
    
    // Always push new entry to support shadowing/scoping
    // Note: We might want to check if we are actually tightening the constraint.
    // If we add a looser constraint, it will shadow the tighter one, which is bad.
    // But for IF conditions, we usually refine.
    
    // Check if we already have a tighter constraint visible
    for (ConstraintEntry *c = t->constraints; c; c = c->next) {
        if (c->v1->length == v1->length && strncmp(c->v1->name, v1->name, v1->length) == 0 &&
            c->v2->length == v2->length && strncmp(c->v2->name, v2->name, v2->length) == 0) {
            if (c->max_diff <= max_diff) {
                // Existing constraint is tighter or equal. Don't add looser one.
                return;
            }
            // If existing is looser, we continue to add the new tighter one.
            // We stop searching because we only care about the first match in `get`.
            // Wait, `get` finds the first match. If we add a new one at head, `get` sees it.
            // So we just need to make sure we don't add if the *first* match is already tighter.
            break; 
        }
    }

    ConstraintEntry *c = arena_push_aligned(t->arena, ConstraintEntry);
    c->v1 = v1;
    c->v2 = v2;
    c->max_diff = max_diff;
    c->next = t->constraints;
    t->constraints = c;
}

// Get known max difference: v1 - v2 <= ?
// Includes a one-step bridge: if v1-MID <= d1 and MID-v2 <= d2 in the table,
// returns d1+d2 when no direct entry exists. This lets the prover derive
// i - src <= -2 from (i - out <= -1) + (out - src <= -1) without a
// full transitive closure pass.
static int64_t constraint_get_diff(RangeTable *t, Id *v1, Id *v2, bool *found) {
    if (!t || !v1 || !v2) { *found = false; return 0; }
    // Direct check
    for (ConstraintEntry *c = t->constraints; c; c = c->next) {
        if (c->v1->length == v1->length && strncmp(c->v1->name, v1->name, v1->length) == 0 &&
            c->v2->length == v2->length && strncmp(c->v2->name, v2->name, v2->length) == 0) {
            *found = true;
            return c->max_diff;
        }
    }
    // One-step bridge: v1 - MID <= d1, MID - v2 <= d2 → v1 - v2 <= d1+d2
    int64_t best = INT64_MAX;
    bool bridge = false;
    for (ConstraintEntry *c1 = t->constraints; c1; c1 = c1->next) {
        if (c1->v1->length != v1->length ||
            strncmp(c1->v1->name, v1->name, v1->length) != 0) continue;
        for (ConstraintEntry *c2 = t->constraints; c2; c2 = c2->next) {
            if (c2->v1->length != c1->v2->length ||
                strncmp(c2->v1->name, c1->v2->name, c1->v2->length) != 0) continue;
            if (c2->v2->length != v2->length ||
                strncmp(c2->v2->name, v2->name, v2->length) != 0) continue;
            int64_t total = sat_add_i64(c1->max_diff, c2->max_diff);
            if (!bridge || total < best) { best = total; bridge = true; }
        }
    }
    if (bridge) { *found = true; return best; }
    *found = false;
    return 0;
}

// Apply a boolean constraint to the range table
// e.g. "x > 10" -> update x's min to 11
// VRA: resolve `x.len` (an EXPR_MEMBER) to the synthetic `__len_x` difference
// variable Id, reusing an existing arena-backed Id from the range table (param
// seeding populates `__len_PARAM`). Returns NULL when not a `.len` member or no
// such Id exists yet (conservative — the caller simply adds no constraint).
// Enables `if i < arr.len` / `while i < arr.len` to narrow i against the length.
static Id *range_member_len_id(RangeTable *t, Expr *member) {
    if (!t || !member || member->kind != EXPR_MEMBER ||
        !member->as.member_expr.member ||
        member->as.member_expr.member->length != 3 ||
        strncmp(member->as.member_expr.member->name, "len", 3) != 0 ||
        member->as.member_expr.target->kind != EXPR_IDENTIFIER)
        return NULL;
    Id *obj = member->as.member_expr.target->as.identifier_expr.id;
    char key[272];
    int klen = 6 + (int)obj->length;
    if (klen >= (int)sizeof(key)) return NULL;
    memcpy(key, "__len_", 6);
    memcpy(key + 6, obj->name, obj->length);
    for (RangeEntry *re = t->head; re; re = re->next)
        if (re->var && re->var->length == klen && strncmp(re->var->name, key, klen) == 0)
            return re->var;
    return NULL;
}

// Canonicalize a pure identifier/member access path (`a`, `l.src`, `x.y.z`) to a
// stable string. Returns bytes written (excluding NUL), or -1 if the expr is not
// a pure path (it contains a call, index, arithmetic, …). This is what gives a
// STRUCT-FIELD slice a stable VRA length identity — a param slice gets
// `__len_PARAM` seeded at entry, but `l.src` is a field, so `l.src.len` and
// `l.src[i]` need a shared key derived from the path to connect.
static int member_path_key(Expr *e, char *buf, int cap) {
    if (!e || cap <= 1) return -1;
    if (e->kind == EXPR_IDENTIFIER) {
        Id *id = e->as.identifier_expr.id;
        if (!id || (int)id->length + 1 > cap) return -1;
        memcpy(buf, id->name, id->length);
        return (int)id->length;
    }
    if (e->kind == EXPR_MEMBER) {
        int n = member_path_key(e->as.member_expr.target, buf, cap);
        if (n < 0) return -1;
        Id *m = e->as.member_expr.member;
        if (!m || n + 1 + (int)m->length + 1 > cap) return -1;
        buf[n++] = '.';
        memcpy(buf + n, m->name, m->length);
        return n + (int)m->length;
    }
    return -1;
}

// Reuse or arena-create a synthetic Id with the given name, so the constraint
// side and the bounds side name the same length key (the constraint table
// compares Ids by name, not pointer).
static Id *member_key_id(RangeTable *t, const char *key, int klen) {
    for (ConstraintEntry *c = t->constraints; c; c = c->next) {
        if (c->v1 && (int)c->v1->length == klen && strncmp(c->v1->name, key, (size_t)klen) == 0) return c->v1;
        if (c->v2 && (int)c->v2->length == klen && strncmp(c->v2->name, key, (size_t)klen) == 0) return c->v2;
    }
    char *stored = arena_push_many(t->arena, char, klen);
    memcpy(stored, key, (size_t)klen);
    Id *id = arena_push_aligned(t->arena, Id);
    id->length = klen; id->name = stored;
    return id;
}

// Member-path length constraints ("__mk_<path>") are only SOUND while the slice the
// path names can't have been mutated. To guarantee that with zero staleness risk,
// they are added ONLY inside a short-circuit `&&`'s right operand — where the
// left `i < l.src.len` was just evaluated and no write can intervene before the
// `l.src[i]` read — and are scoped away immediately after. This flag, set by the
// within-`&&` flow, is the gate: a persisted `i < l.src.len` (a bare while/if
// condition) does NOT create one, so it can never outlive the read and go stale.
static bool sema_mk_scoped = false;

// The length key for a member-path RHS bound (`l.src.len`, `l.len`): "__mk_" + path.
// Returns key length or -1. Shared by the constraint side (`i < <path>`) and the
// bounds side (which builds the SAME string from the indexed slice's length path).
static int member_len_key(Expr *path, char *out, int cap) {
    if (cap < 6) return -1;
    memcpy(out, "__mk_", 5);
    int n = member_path_key(path, out + 5, cap - 5);
    return n < 0 ? -1 : n + 5;
}

static void sema_apply_constraint(Expr *cond, RangeTable *t) {
    if (!cond || !t) return;

    if (cond->kind == EXPR_BINARY) {
        TokenKind op = cond->as.binary_expr.op;
        Expr *lhs = cond->as.binary_expr.left;
        Expr *rhs = cond->as.binary_expr.right;

        // F-038: recurse into `and` chains — both sides refine the range.
        // `or` is left as no-op: the current interval lattice would need a
        // union merge that single-interval ranges cannot express precisely.
        if (op == TOKEN_KEYWORD_AND) {
            sema_apply_constraint(lhs, t);
            sema_apply_constraint(rhs, t);
            return;
        }

        // Normalize: ensure LHS is identifier, RHS is literal
        // TODO: Handle more complex cases (e.g. x < y)
        if (lhs->kind == EXPR_IDENTIFIER && rhs->kind == EXPR_LITERAL) {
            Id *var = lhs->as.identifier_expr.id;
            int64_t val = rhs->as.literal_expr.value;
            Range r = range_get(t, var);
            if (!r.known) {
                // Initialize with unbounded/default if unknown?
                // For now, assume full range if unknown, but we don't have min/max limits defined here.
                // Let's just create a new range if it doesn't exist.
                r = (Range){INT64_MIN, INT64_MAX, true};
            }

            // Interval ops via the shared mapping; `!=` tightens a boundary.
            if (!range_tighten_interval(&r, op, val) && op == TOKEN_BANG_EQUAL) {
                if (val == r.min && val == r.max) { r.min = 1; r.max = 0; } // empty (contradiction)
                else if (val == r.min) r.min = val + 1;                     // exclude lower bound
                else if (val == r.max) r.max = val - 1;                     // exclude upper bound
                // Interior `!=` is conservative (single interval can't express a hole).
            }
            range_set(t, var, r);
        }
        // Handle literal on LHS: 10 < x  <=>  x > 10 (flip the op, reuse the mapping).
        else if (lhs->kind == EXPR_LITERAL && rhs->kind == EXPR_IDENTIFIER) {
            Id *var = rhs->as.identifier_expr.id;
            int64_t val = lhs->as.literal_expr.value;
            Range r = range_get(t, var);
            if (!r.known) r = (Range){INT64_MIN, INT64_MAX, true};
            range_tighten_interval(&r, relop_flip(op), val);
            range_set(t, var, r);
        }
        // Handle Identifier vs Identifier: x < y
        else if (lhs->kind == EXPR_IDENTIFIER && rhs->kind == EXPR_IDENTIFIER) {
            Id *v1 = lhs->as.identifier_expr.id;
            Id *v2 = rhs->as.identifier_expr.id;
            
            switch (op) {
                case TOKEN_ANGLE_BRACKET_LEFT: // x < y  <=> x - y <= -1
                    constraint_add(t, v1, v2, -1);
                    break;
                case TOKEN_ANGLE_BRACKET_LEFT_EQUAL: // x <= y <=> x - y <= 0
                    constraint_add(t, v1, v2, 0);
                    break;
                case TOKEN_ANGLE_BRACKET_RIGHT: // x > y  <=> y - x <= -1
                    constraint_add(t, v2, v1, -1);
                    break;
                case TOKEN_ANGLE_BRACKET_RIGHT_EQUAL: // x >= y <=> y - x <= 0
                    constraint_add(t, v2, v1, 0);
                    break;
                case TOKEN_EQUAL_EQUAL: // x == y <=> x - y <= 0 AND y - x <= 0
                    constraint_add(t, v1, v2, 0);
                    constraint_add(t, v2, v1, 0);
                    break;
                default: break;
            }
        }
        // VRA: Identifier vs member(.len): x < arr.len (narrows i against length)
        else if (lhs->kind == EXPR_IDENTIFIER && rhs->kind == EXPR_MEMBER) {
            Id *v1 = lhs->as.identifier_expr.id;
            Id *v2 = range_member_len_id(t, rhs);
            // Member-PATH bound (`i < l.src.len`, `i < l.len`): no `__len_PARAM`
            // was seeded (it's a struct field, not a param), so key a synthetic
            // length Id off the canonical path. The bounds check rebuilds the SAME
            // key from the indexed slice, so `l.src[i]` proves. (Sound because the
            // within-`&&` flow scopes this to the immediate read; a persisted
            // constraint is invalidated when the index or the path is reassigned.)
            if (!v2 && sema_mk_scoped) {
                char key[256];
                int klen = member_len_key(rhs, key, (int)sizeof key);
                if (klen > 0) v2 = member_key_id(t, key, klen);
            }
            if (v2) {
                switch (op) {
                    case TOKEN_ANGLE_BRACKET_LEFT:        constraint_add(t, v1, v2, -1); break; // x < len
                    case TOKEN_ANGLE_BRACKET_LEFT_EQUAL:  constraint_add(t, v1, v2, 0);  break; // x <= len
                    case TOKEN_ANGLE_BRACKET_RIGHT:       constraint_add(t, v2, v1, -1); break; // x > len
                    case TOKEN_ANGLE_BRACKET_RIGHT_EQUAL: constraint_add(t, v2, v1, 0);  break; // x >= len
                    case TOKEN_EQUAL_EQUAL:
                        constraint_add(t, v1, v2, 0); constraint_add(t, v2, v1, 0); break;
                    default: break;
                }
            }
        }
    }
}

// Apply the negation of a constraint
static void sema_apply_negated_constraint(Expr *cond, RangeTable *t) {
    if (!cond || !t) return;

    if (cond->kind == EXPR_BINARY) {
        TokenKind op = cond->as.binary_expr.op;
        Expr *lhs = cond->as.binary_expr.left;
        Expr *rhs = cond->as.binary_expr.right;

        // De Morgan on the negation of a compound guard. The common early-return
        // idiom `if i < 0 or i >= a.len { return }` leaves `!(i<0 or i>=len)` in
        // force afterward, i.e. `i >= 0 and i < len` — both facts must be applied
        // so `a[i]` is provable. Without this, `or` was a no-op and the equivalent
        // split-if form was needed. `!(A and B)` is a disjunction (no single
        // narrowing), so nothing is added.
        if (op == TOKEN_KEYWORD_OR) {
            sema_apply_negated_constraint(lhs, t);
            sema_apply_negated_constraint(rhs, t);
            return;
        }
        if (op == TOKEN_KEYWORD_AND) return;

        // Handle Identifier vs Identifier: !(x < y) <=> x >= y
        if (lhs->kind == EXPR_IDENTIFIER && rhs->kind == EXPR_IDENTIFIER) {
            Id *v1 = lhs->as.identifier_expr.id;
            Id *v2 = rhs->as.identifier_expr.id;
            
            switch (op) {
                case TOKEN_ANGLE_BRACKET_LEFT: // !(x < y) <=> x >= y <=> y - x <= 0
                    constraint_add(t, v2, v1, 0);
                    break;
                case TOKEN_ANGLE_BRACKET_LEFT_EQUAL: // !(x <= y) <=> x > y <=> y - x <= -1
                    constraint_add(t, v2, v1, -1);
                    break;
                case TOKEN_ANGLE_BRACKET_RIGHT: // !(x > y) <=> x <= y <=> x - y <= 0
                    constraint_add(t, v1, v2, 0);
                    break;
                case TOKEN_ANGLE_BRACKET_RIGHT_EQUAL: // !(x >= y) <=> x < y <=> x - y <= -1
                    constraint_add(t, v1, v2, -1);
                    break;
                // Equality negation is hard for ranges/DBM (disjunction)
                default: break;
            }
        }
        // VRA: negated Identifier vs member(.len): e.g. `if i >= arr.len { return }`
        else if (lhs->kind == EXPR_IDENTIFIER && rhs->kind == EXPR_MEMBER) {
            Id *v1 = lhs->as.identifier_expr.id;
            Id *v2 = range_member_len_id(t, rhs);
            if (v2) {
                switch (op) {
                    case TOKEN_ANGLE_BRACKET_LEFT:        constraint_add(t, v2, v1, 0);  break; // !(x<len) => x>=len
                    case TOKEN_ANGLE_BRACKET_LEFT_EQUAL:  constraint_add(t, v2, v1, -1); break; // !(x<=len) => x>len
                    case TOKEN_ANGLE_BRACKET_RIGHT:       constraint_add(t, v1, v2, 0);  break; // !(x>len) => x<=len
                    case TOKEN_ANGLE_BRACKET_RIGHT_EQUAL: constraint_add(t, v1, v2, -1); break; // !(x>=len) => x<len
                    default: break;
                }
            }
        }
        // Handle Identifier vs Literal: !(x < 10) <=> x >= 10
        else if (lhs->kind == EXPR_IDENTIFIER && rhs->kind == EXPR_LITERAL) {
            Id *var = lhs->as.identifier_expr.id;
            int64_t val = rhs->as.literal_expr.value;
            Range r = range_get(t, var);
            if (!r.known) r = (Range){INT64_MIN, INT64_MAX, true};

            switch (op) {
                case TOKEN_ANGLE_BRACKET_RIGHT: // !(x > val) <=> x <= val
                    if (val < r.max) r.max = val;
                    break;
                case TOKEN_ANGLE_BRACKET_RIGHT_EQUAL: // !(x >= val) <=> x < val
                    if (val - 1 < r.max) r.max = val - 1;
                    break;
                case TOKEN_ANGLE_BRACKET_LEFT: // !(x < val) <=> x >= val
                    if (val > r.min) r.min = val;
                    break;
                case TOKEN_ANGLE_BRACKET_LEFT_EQUAL: // !(x <= val) <=> x > val
                    if (val + 1 > r.min) r.min = val + 1;
                    break;
                default: break;
            }
            range_set(t, var, r);
        }
    }
}

// Helper to compare two ranges
static int sema_compare_ranges(Range l, Range r, TokenKind op) {
    if (!l.known || !r.known) return -1;

    switch (op) {
        case TOKEN_ANGLE_BRACKET_RIGHT: // L > R
            if (l.min > r.max) return 1; // definitely true
            if (l.max <= r.min) return 0; // definitely false
            return -1;
        case TOKEN_ANGLE_BRACKET_RIGHT_EQUAL: // L >= R
            if (l.min >= r.max) return 1;
            if (l.max < r.min) return 0;
            return -1;
        case TOKEN_ANGLE_BRACKET_LEFT: // L < R
            if (l.max < r.min) return 1;
            if (l.min >= r.max) return 0;
            return -1;
        case TOKEN_ANGLE_BRACKET_LEFT_EQUAL: // L <= R
            if (l.max <= r.min) return 1;
            if (l.min > r.max) return 0;
            return -1;
        case TOKEN_EQUAL_EQUAL: // L == R
            if (l.min == l.max && r.min == r.max && l.min == r.min) return 1;
            if (l.max < r.min || l.min > r.max) return 0;
            return -1;
        case TOKEN_BANG_EQUAL: // L != R
            if (l.max < r.min || l.min > r.max) return 1;
            if (l.min == l.max && r.min == r.max && l.min == r.min) return 0;
            return -1;
        default: return -1;
    }
}

// Check if a condition is statically true given the current ranges
// Returns: 1 (true), 0 (false), -1 (unknown)
static int sema_check_condition(Expr *cond, RangeTable *t) {
    if (!cond || !t) return -1;

    if (cond->kind == EXPR_BINARY) {
        // Check relational constraints first
        if (cond->as.binary_expr.left->kind == EXPR_IDENTIFIER &&
            cond->as.binary_expr.right->kind == EXPR_IDENTIFIER) {
            
            Id *v1 = cond->as.binary_expr.left->as.identifier_expr.id;
            Id *v2 = cond->as.binary_expr.right->as.identifier_expr.id;
            TokenKind op = cond->as.binary_expr.op;
            
            bool found = false;
            int64_t diff = 0;
            
            switch (op) {
                case TOKEN_ANGLE_BRACKET_LEFT: // x < y
                    // True if x - y <= -1
                    diff = constraint_get_diff(t, v1, v2, &found);
                    if (found && diff <= -1) return 1;
                    // False if x >= y <=> y - x <= 0
                    diff = constraint_get_diff(t, v2, v1, &found);
                    if (found && diff <= 0) return 0;
                    break;
                    
                case TOKEN_ANGLE_BRACKET_LEFT_EQUAL: // x <= y
                    // True if x - y <= 0
                    diff = constraint_get_diff(t, v1, v2, &found);
                    if (found && diff <= 0) return 1;
                    // False if x > y <=> y - x <= -1
                    diff = constraint_get_diff(t, v2, v1, &found);
                    if (found && diff <= -1) return 0;
                    break;
                    
                case TOKEN_ANGLE_BRACKET_RIGHT: // x > y
                    // True if y - x <= -1
                    diff = constraint_get_diff(t, v2, v1, &found);
                    if (found && diff <= -1) return 1;
                    // False if x <= y <=> x - y <= 0
                    diff = constraint_get_diff(t, v1, v2, &found);
                    if (found && diff <= 0) return 0;
                    break;
                    
                case TOKEN_ANGLE_BRACKET_RIGHT_EQUAL: // x >= y
                    // True if y - x <= 0
                    diff = constraint_get_diff(t, v2, v1, &found);
                    if (found && diff <= 0) return 1;
                    // False if x < y <=> x - y <= -1
                    diff = constraint_get_diff(t, v1, v2, &found);
                    if (found && diff <= -1) return 0;
                    break;
                    
                default: break;
            }
        }

        Range l = sema_eval_range(cond->as.binary_expr.left, t);
        Range r = sema_eval_range(cond->as.binary_expr.right, t);
        return sema_compare_ranges(l, r, cond->as.binary_expr.op);
    }
    return -1;
}

// Check post-condition with result range substitution
static int sema_check_post_condition(Expr *cond, Range result_range, RangeTable *t) {
    if (!cond || !t) return -1;

    if (cond->kind == EXPR_BINARY) {
        Range l, r;
        
        // Evaluate Left
        if (cond->as.binary_expr.left->kind == EXPR_IDENTIFIER &&
            strncmp(cond->as.binary_expr.left->as.identifier_expr.id->name, "result", 6) == 0 &&
            cond->as.binary_expr.left->as.identifier_expr.id->length == 6) {
            l = result_range;
        } else {
            l = sema_eval_range(cond->as.binary_expr.left, t);
        }

        // Evaluate Right
        if (cond->as.binary_expr.right->kind == EXPR_IDENTIFIER &&
            strncmp(cond->as.binary_expr.right->as.identifier_expr.id->name, "result", 6) == 0 &&
            cond->as.binary_expr.right->as.identifier_expr.id->length == 6) {
            r = result_range;
        } else {
            r = sema_eval_range(cond->as.binary_expr.right, t);
        }

        return sema_compare_ranges(l, r, cond->as.binary_expr.op);
    }
    return -1;
}

#endif // SEMA_RANGES_H
