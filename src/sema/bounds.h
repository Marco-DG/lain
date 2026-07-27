#ifndef SEMA_BOUNDS_H
#define SEMA_BOUNDS_H

/*
 * Static Bounds Checking for Lain
 * 
 * Verifies array index accesses at compile time.
 * Relies on Range Analysis (sema/ranges.h).
 */

#include "../ast.h"
#include "ranges.h"
#include "omega.h"
#include <stdio.h>
#include <stdlib.h>

// Debug flag
#ifndef SEMA_BOUNDS_DEBUG
#define SEMA_BOUNDS_DEBUG 0
#endif

#if SEMA_BOUNDS_DEBUG
#define BOUNDS_DBG(fmt, ...) fprintf(stderr, "[bounds] " fmt "\n", ##__VA_ARGS__)
#else
#define BOUNDS_DBG(fmt, ...) do {} while(0)
#endif

/*───────────────────────────────────────────────────────────────────╗
│ Diagnostic Helpers                                                 │
╚───────────────────────────────────────────────────────────────────*/

/* Format a range value: cap INT64 sentinels to readable symbols. */
static void bounds_fmt_val(int64_t v, char *buf, int n) {
    if (v >= (int64_t)4e18)       snprintf(buf, n, "MAX");
    else if (v <= -(int64_t)4e18) snprintf(buf, n, "MIN");
    else                           snprintf(buf, n, "%ld", (long)v);
}

/* Format a range as "[lo, hi]" with readable sentinels. */
static void bounds_fmt_range(Range r, char *buf, int n) {
    if (!r.known) { snprintf(buf, n, "[unknown]"); return; }
    char lo[32], hi[32];
    bounds_fmt_val(r.min, lo, sizeof lo);
    bounds_fmt_val(r.max, hi, sizeof hi);
    snprintf(buf, n, "[%s, %s]", lo, hi);
}

/* Render an expression to a short human-readable string (best-effort). */
static void bounds_expr_str(Expr *e, char *buf, int n) {
    if (!e || n <= 1) { if (n > 0) buf[0] = '\0'; return; }
    switch (e->kind) {
        case EXPR_IDENTIFIER: {
            Id *id = e->as.identifier_expr.id;
            if (id) snprintf(buf, n, "%.*s", (int)id->length, id->name);
            else    snprintf(buf, n, "?");
            break;
        }
        case EXPR_LITERAL:
            snprintf(buf, n, "%lld", (long long)e->as.literal_expr.value);
            break;
        case EXPR_BINARY: {
            char lhs[64], rhs[64];
            bounds_expr_str(e->as.binary_expr.left,  lhs, sizeof lhs);
            bounds_expr_str(e->as.binary_expr.right, rhs, sizeof rhs);
            const char *op = token_kind_to_str(e->as.binary_expr.op);
            snprintf(buf, n, "%s %s %s", lhs, op ? op : "?", rhs);
            break;
        }
        case EXPR_MEMBER: {
            char tgt[64];
            bounds_expr_str(e->as.member_expr.target, tgt, sizeof tgt);
            Id *mem = e->as.member_expr.member;
            if (mem) snprintf(buf, n, "%s.%.*s", tgt, (int)mem->length, mem->name);
            else     snprintf(buf, n, "%s.?", tgt);
            break;
        }
        case EXPR_UNARY: {
            char operand[64];
            bounds_expr_str(e->as.unary_expr.right, operand, sizeof operand);
            const char *op = token_kind_to_str(e->as.unary_expr.op);
            snprintf(buf, n, "%s%s", op ? op : "?", operand);
            break;
        }
        default:
            snprintf(buf, n, "<expr>");
            break;
    }
}

/* Emit a standard E085 header + source snippet + context lines, then exit(1). */
static void bounds_error(
    const char *kind,       /* short one-line description */
    Expr       *index_expr, /* index sub-expression (for location) */
    Expr       *array_expr, /* array being indexed (for name) */
    Range       idx,        /* computed index range */
    Range       len,        /* computed length range */
    const char *hint)       /* optional suggestion, or NULL */
{
    char idx_str[128] = "<expr>";
    char arr_str[64]  = "<array>";
    char idx_range[64];
    char len_range[64];

    if (index_expr) bounds_expr_str(index_expr, idx_str, sizeof idx_str);
    if (array_expr) bounds_expr_str(array_expr, arr_str, sizeof arr_str);
    bounds_fmt_range(idx, idx_range, sizeof idx_range);
    bounds_fmt_range(len, len_range, sizeof len_range);

    isize line = index_expr ? index_expr->line : 0;
    isize col  = index_expr ? index_expr->col  : 0;

    fprintf(stderr, "[E085] bounds error: %s\n", kind);
    if (line > 0) diagnostic_show_line(line, col);
    fprintf(stderr, "       index `%s`: range %s\n", idx_str, idx_range);
    fprintf(stderr, "       array `%s`: length %s\n", arr_str, len_range);
    if (hint) fprintf(stderr, "       hint: %s\n", hint);
    exit(1);
}

/*───────────────────────────────────────────────────────────────────╗
│ Bounds Checking                                                    │
╚───────────────────────────────────────────────────────────────────*/

// Check if an index access is within bounds.
// array_expr is the expression being indexed (used to identify the array by name
// for constraint-based proof when the array has no size_expr annotation).
static void sema_check_bounds(RangeTable *ctx, Expr *index_expr, Type *array_type, Expr *array_expr, bool is_addr_of) {
    if (!index_expr || !array_type) return;

    // Sprint 4 / Q-003.B: range index `arr[a..b]` bounds check.
    // Verify: a >= 0 AND b <= arr.len (semi-open interval [a, b)).
    if (index_expr->kind == EXPR_RANGE) {
        Expr *lo = index_expr->as.range_expr.start;
        Expr *hi = index_expr->as.range_expr.end;
        Range a = lo ? sema_eval_range(lo, ctx) : range_const(0);
        Range b = hi ? sema_eval_range(hi, ctx) : range_unknown();

        // a >= 0
        if (a.known && a.min < 0) {
            bounds_error("slice lower bound may be negative",
                         lo, array_expr, a, range_unknown(),
                         "ensure start index >= 0");
        }
        // Determine array len
        Range len_range = range_unknown();
        if (array_type->kind == TYPE_ARRAY && array_type->array_len >= 0) {
            len_range = range_const(array_type->array_len);
        } else if (array_type->kind == TYPE_SLICE && array_type->sentinel_len > 0) {
            len_range = range_const(array_type->sentinel_len);
        }
        // b <= len
        if (len_range.known) {
            if (!hi) {
                // Open upper: arr[a..] — fine: implicitly b = arr.len.
            } else if (b.known) {
                if (b.max > len_range.min) {
                    bounds_error("slice upper bound out of range",
                                 hi, array_expr, b, len_range, NULL);
                }
            } else {
                bounds_error("slice upper bound cannot be statically verified",
                             hi, array_expr, b, len_range,
                             "use a literal or constrained upper bound");
            }
            // a <= len too (a == len gives empty slice, OK)
            if (a.known && a.max > len_range.min) {
                bounds_error("slice lower bound out of range",
                             lo, array_expr, a, len_range, NULL);
            }
        } else {
            // Dynamic array length — cannot verify statically.
            // The result is still a slice (a..b) but we don't verify the upper bound.
            // Lower bound (a >= 0) was checked above. Accept; runtime check would be needed
            // for full safety on dynamic slices.
        }
        return;
    }

    // 1. Determine Array Length Range
    Range len_range = range_unknown();
    if (array_type->kind == TYPE_ARRAY && array_type->array_len >= 0) {
        len_range = range_const(array_type->array_len);
    } else if (array_type->kind == TYPE_SLICE && array_type->sentinel_len > 0) {
        // Known fixed-size slice (e.g. string literal)
        len_range = range_const(array_type->sentinel_len);
    } else if (array_type->kind == TYPE_ARRAY && array_type->array_len == -1 &&
               array_type->size_expr) {
        // Sized slice: evaluate the size expression via VRA.
        // For relop constraints (i32[>= k], i32[> k]), len_range is a lower bound,
        // not an equality: len >= base (or len > base).
        Range base = sema_eval_range(array_type->size_expr, ctx);
        if (array_type->size_relop == TOKEN_ANGLE_BRACKET_RIGHT_EQUAL ||
            array_type->size_relop == TOKEN_ANGLE_BRACKET_RIGHT) {
            if (base.known) {
                int64_t delta = (array_type->size_relop == TOKEN_ANGLE_BRACKET_RIGHT) ? 1 : 0;
                len_range = range_make(base.min + delta, INT64_MAX);
            }
        } else {
            len_range = base;
        }
    }

    // 2. Compute Index Range
    Range idx = sema_eval_range(index_expr, ctx);

    // 3. Verify: idx >= 0
    if (idx.known && idx.min < 0) {
        // Interval is pessimistic. Try Omega to prove expr >= 0 given VRA constraints.
        if (!omega_prove_nonneg(ctx, index_expr)) {
            // Build a hint: if the index is "param - 1" and param has constraint >= 0,
            // suggest tightening to >= 1.
            char hint_buf[256] = "";
            if (index_expr->kind == EXPR_BINARY &&
                index_expr->as.binary_expr.op == TOKEN_MINUS &&
                index_expr->as.binary_expr.left->kind == EXPR_IDENTIFIER &&
                index_expr->as.binary_expr.right->kind == EXPR_LITERAL &&
                index_expr->as.binary_expr.right->as.literal_expr.value == 1) {
                Id *pid = index_expr->as.binary_expr.left->as.identifier_expr.id;
                if (pid) snprintf(hint_buf, sizeof hint_buf,
                    "parameter `%.*s` has constraint >= 0; change to `>= 1`, "
                    "or guard with `if %.*s == 0 { ... }`",
                    (int)pid->length, pid->name, (int)pid->length, pid->name);
            }
            bounds_error("index may be negative",
                         index_expr, array_expr, idx, len_range,
                         hint_buf[0] ? hint_buf : NULL);
        }
        // Omega proved non-negativity; mark idx as known-safe for subsequent checks.
        idx.min = 0;
    }

    // 4. Interval proof: idx_max < len_min (or <= len_min for address-of).
    //    Fast path — works for fixed arrays and sized slices with narrow ranges.
    if (len_range.known && idx.known) {
        // For plain dereference: idx must be strictly < len.
        // For address-of (&arr[i]): idx == len is valid (one-past-end pointer).
        bool interval_safe = is_addr_of ? (idx.max <= len_range.min)
                                        : (idx.max <  len_range.min);
        if (interval_safe) {
            BOUNDS_DBG("OK: Index [%ld, %ld] %s Length %ld",
                       (long)idx.min, (long)idx.max,
                       is_addr_of ? "<=" : "<", (long)len_range.min);
            return; // safe via interval
        }
        // else fall through to constraint proof
    } else if (len_range.known && !idx.known) {
        // len known but idx unknown: fall through to constraint proof
    }

    // 5. Constraint-based proof for dynamic slices (sized or plain).
    //    Looks for "idx - len_key <= -1" in the constraint table where len_key is
    //    the synthetic __len_PARAM entry that the for-loop injector adds.
    //    This proves idx < arr.len even when the actual length is unknown at compile time.
    if (array_type->kind == TYPE_ARRAY && array_type->array_len == -1 &&
        index_expr->kind == EXPR_IDENTIFIER) {
        Id *idx_id = index_expr->as.identifier_expr.id;

        // Determine the effective length key to look up in the constraint table.
        //   size_expr == EXPR_MEMBER(ref.len) → key = __len_ref
        //   size_expr == EXPR_IDENTIFIER(n)   → key = n (scalar param)
        //   size_expr == NULL (plain i32[])   → key = __len_ARRAY (from array_expr)
        bool proved = false;
        if (array_type->size_expr &&
            array_type->size_expr->kind == EXPR_MEMBER &&
            array_type->size_expr->as.member_expr.target->kind == EXPR_IDENTIFIER &&
            array_type->size_expr->as.member_expr.member->length == 3 &&
            strncmp(array_type->size_expr->as.member_expr.member->name, "len", 3) == 0) {
            // a i32[ref.len]: check i - __len_ref <= -1
            Id *ref_id = array_type->size_expr->as.member_expr.target->as.identifier_expr.id;
            char key[272];
            int klen = 6 + (int)ref_id->length;
            if (klen < (int)sizeof(key)) {
                memcpy(key, "__len_", 6);
                memcpy(key + 6, ref_id->name, ref_id->length);
                for (ConstraintEntry *ce = ctx->constraints; ce; ce = ce->next) {
                    if (ce->v1->length == idx_id->length &&
                        strncmp(ce->v1->name, idx_id->name, idx_id->length) == 0 &&
                        ce->v2->length == klen &&
                        strncmp(ce->v2->name, key, klen) == 0 &&
                        ce->max_diff <= -1) {
                        BOUNDS_DBG("OK: %.*s < %.*s.len via constraint",
                                   (int)idx_id->length, idx_id->name,
                                   (int)ref_id->length, ref_id->name);
                        proved = true;
                        break;
                    }
                }
            }
        } else if (array_type->size_expr &&
                   array_type->size_expr->kind == EXPR_IDENTIFIER) {
            // a i32[n] or a i32[>= n]: check i - n <= -1
            // For >= n: i < n AND n <= a.len (by annotation) → i < a.len
            Id *n_id = array_type->size_expr->as.identifier_expr.id;
            bool found = false;
            int64_t diff = constraint_get_diff(ctx, idx_id, n_id, &found);
            if (found && diff <= -1) {
                BOUNDS_DBG("OK: %.*s < n via constraint",
                           (int)idx_id->length, idx_id->name);
                proved = true;
            }
        } else if (array_type->size_expr &&
                   array_type->size_expr->kind == EXPR_BINARY) {
            // Arithmetic size_expr: i32[a.len + b.len] or i32[src.len - k].
            TokenKind bop = array_type->size_expr->as.binary_expr.op;
            Expr *blhs = array_type->size_expr->as.binary_expr.left;
            Expr *brhs = array_type->size_expr->as.binary_expr.right;
            // Helper macro: given an EXPR_MEMBER(x.len) node, find __len_x Id in VRA
            // and check idx - __len_x <= threshold via constraint_get_diff (with bridge).
            #define TRY_MEMBER_LEN(MEM_EXPR, THRESHOLD) do { \
                if (!proved && (MEM_EXPR)->kind == EXPR_MEMBER && \
                    (MEM_EXPR)->as.member_expr.member->length == 3 && \
                    strncmp((MEM_EXPR)->as.member_expr.member->name, "len", 3) == 0 && \
                    (MEM_EXPR)->as.member_expr.target->kind == EXPR_IDENTIFIER) { \
                    Id *_ref = (MEM_EXPR)->as.member_expr.target->as.identifier_expr.id; \
                    char _k[272]; int _kl = 6 + (int)_ref->length; \
                    if (_kl < (int)sizeof(_k)) { \
                        memcpy(_k, "__len_", 6); memcpy(_k+6, _ref->name, _ref->length); \
                        Id *_lid = NULL; \
                        for (RangeEntry *_re = ctx->head; _re; _re = _re->next) { \
                            if (_re->var->length == _kl && \
                                strncmp(_re->var->name, _k, _kl) == 0) \
                            { _lid = _re->var; break; } \
                        } \
                        if (_lid) { \
                            bool _f = false; \
                            int64_t _d = constraint_get_diff(ctx, idx_id, _lid, &_f); \
                            if (_f && _d <= (THRESHOLD)) proved = true; \
                        } \
                    } \
                } \
            } while(0)
            if (bop == TOKEN_PLUS) {
                // out i32[a.len + b.len]: idx < a.len OR idx < b.len → idx < out.len
                TRY_MEMBER_LEN(blhs, -1);
                TRY_MEMBER_LEN(brhs, -1);
            } else if (bop == TOKEN_MINUS && brhs->kind == EXPR_LITERAL) {
                // out i32[src.len - k]: need idx - __len_src <= -(k+1)
                // (bridge: i-__len_out<=-1, __len_out-__len_src<=-k → i-__len_src<=-(k+1))
                int64_t k = brhs->as.literal_expr.value;
                TRY_MEMBER_LEN(blhs, -(k + 1));
            }
            #undef TRY_MEMBER_LEN
        } else {
            // Plain i32[]: check i - __len_ARRAY <= -1 (from for loop over arr.len)
            if (array_expr && array_expr->kind == EXPR_IDENTIFIER) {
                Id *arr_id = array_expr->as.identifier_expr.id;
                char key[272];
                int klen = 6 + (int)arr_id->length;
                if (klen < (int)sizeof(key)) {
                    memcpy(key, "__len_", 6);
                    memcpy(key + 6, arr_id->name, arr_id->length);
                    // Use constraint_get_diff (includes one-step bridge) so that
                    // transitive chains like i-__len_out<=-1, __len_out-__len_src<=-1
                    // prove i < src.len without needing a direct entry.
                    Id *arr_len_id = NULL;
                    for (RangeEntry *re = ctx->head; re; re = re->next) {
                        if (re->var->length == klen &&
                            strncmp(re->var->name, key, klen) == 0)
                        { arr_len_id = re->var; break; }
                    }
                    if (arr_len_id) {
                        bool gd_found = false;
                        int64_t gd = constraint_get_diff(ctx, idx_id, arr_len_id, &gd_found);
                        if (gd_found && gd <= -1) {
                            BOUNDS_DBG("OK: %.*s < %.*s.len via constraint (bridge)",
                                       (int)idx_id->length, idx_id->name,
                                       (int)arr_id->length, arr_id->name);
                            proved = true;
                        }
                    }
                }
            }
        }
        if (proved) return;
    }

    // 5.4. Division/modulo monotonicity proofs.
    //
    // Division:  arr[i / d]  with literal d >= 1.
    //   Mathematical fact: if i >= 0 and i < n, then i/d <= i < n.
    //   Sufficient conditions: (a) i >= 0  (b) i < arr.len.
    //   (b) is checked via three paths: interval (L1), sized-slice FM (L4),
    //   and constraint table (for plain i32[] with for-loop injected constraints).
    //
    // Modulo:  arr[i % arr.len]  (modulus = the same array's .len).
    //   i % arr.len ∈ [0, arr.len-1] when i >= 0 and arr.len > 0.
    //   For modulo with a literal divisor, L1 already handles it via range_mod.
    if (index_expr->kind == EXPR_BINARY) {
        TokenKind idx_op = index_expr->as.binary_expr.op;
        Expr *idx_lhs   = index_expr->as.binary_expr.left;
        Expr *idx_rhs   = index_expr->as.binary_expr.right;

        if (idx_op == TOKEN_SLASH &&
            idx_rhs && idx_rhs->kind == EXPR_LITERAL &&
            idx_rhs->as.literal_expr.value >= 1) {
            /* --- division monotonicity: i/d < arr.len iff i >= 0 and i < arr.len --- */
            bool num_nn = false;
            Range nr = sema_eval_range(idx_lhs, ctx);
            if (nr.known && nr.min >= 0)             num_nn = true;
            else if (omega_prove_nonneg(ctx, idx_lhs)) num_nn = true;

            if (num_nn) {
                /* (a) interval path: numerator.max < fixed arr.len */
                if (len_range.known && nr.known && nr.max < len_range.min) return;

                /* (b) sized-slice FM path: prove num < size_expr (equality constraints) */
                if (array_type->kind == TYPE_ARRAY && array_type->array_len == -1 &&
                    array_type->size_expr && array_type->size_relop == TOKEN_EQUAL_EQUAL &&
                    omega_prove_lt(ctx, idx_lhs, array_type->size_expr)) return;

                /* (c) constraint path: numerator - __len_arr ≤ -1 (for-loop / in-guard) */
                if (idx_lhs->kind == EXPR_IDENTIFIER && array_expr &&
                    array_expr->kind == EXPR_IDENTIFIER) {
                    Id *num_id = idx_lhs->as.identifier_expr.id;
                    Id *arr_id = array_expr->as.identifier_expr.id;
                    char _k[272]; int _kl = 6 + (int)arr_id->length;
                    if (_kl < (int)sizeof(_k)) {
                        memcpy(_k, "__len_", 6);
                        memcpy(_k + 6, arr_id->name, arr_id->length);
                        Id *aln_id = NULL;
                        for (RangeEntry *re = ctx->head; re; re = re->next) {
                            if ((int)re->var->length == _kl &&
                                strncmp(re->var->name, _k, _kl) == 0)
                            { aln_id = re->var; break; }
                        }
                        if (aln_id) {
                            bool gf = false;
                            int64_t gd = constraint_get_diff(ctx, num_id, aln_id, &gf);
                            if (gf && gd <= -1) return; /* num < arr.len → num/d < arr.len */
                        }
                    }
                }
            }
        }

        if (idx_op == TOKEN_PERCENT && array_expr &&
            array_expr->kind == EXPR_IDENTIFIER) {
            /* --- modulo: arr[i % arr.len] — safe when i >= 0 and arr.len > 0 --- */
            Id *arr_id = array_expr->as.identifier_expr.id;
            if (idx_rhs && idx_rhs->kind == EXPR_MEMBER &&
                idx_rhs->as.member_expr.target &&
                idx_rhs->as.member_expr.target->kind == EXPR_IDENTIFIER &&
                idx_rhs->as.member_expr.member &&
                idx_rhs->as.member_expr.member->length == 3 &&
                memcmp(idx_rhs->as.member_expr.member->name, "len", 3) == 0) {
                Id *ref_id = idx_rhs->as.member_expr.target->as.identifier_expr.id;
                if (ref_id && ref_id->length == arr_id->length &&
                    memcmp(ref_id->name, arr_id->name, arr_id->length) == 0) {
                    /* numerator must be non-negative (i % n is negative if i < 0 in C) */
                    bool lhs_nn = false;
                    Range lr = sema_eval_range(idx_lhs, ctx);
                    if (lr.known && lr.min >= 0)               lhs_nn = true;
                    else if (omega_prove_nonneg(ctx, idx_lhs)) lhs_nn = true;
                    /* arr.len must be > 0 so modulo domain is valid */
                    if (lhs_nn && len_range.known && len_range.min > 0) return;
                }
            }
        }
    }

    // 5.5. Omega Test fallback: linear arithmetic for arithmetic index
    //      expressions (e.g. j + a.len) against sized-slice bounds.
    //      Handles all patterns that reduce to difference constraints after
    //      FM variable elimination (concat, reverse, interleave, etc.).
    //      For address-of, also try index <= size (one-past-end valid).
    if (array_type->kind == TYPE_ARRAY && array_type->array_len == -1 &&
        array_type->size_expr) {
        if (omega_prove_lt(ctx, index_expr, array_type->size_expr)) return;
        if (is_addr_of && omega_prove_le(ctx, index_expr, array_type->size_expr)) return;
    }

    // 5.6. VRA#3: plain dynamic array `T[]` (no size_expr) — synthesize `arr.len`
    //      as the bound so the Omega fallback proves arithmetic indices
    //      (reverse `a[a.len-i-1]`, two-pointer, interleave) on plain slices too,
    //      not only sized `T[n]`. omega_decompose maps `.len` → `__len_arr`.
    if (array_type->kind == TYPE_ARRAY && array_type->array_len == -1 &&
        !array_type->size_expr && ctx && array_expr && array_expr->kind == EXPR_IDENTIFIER) {
        Id *len_name = id(ctx->arena, 3, "len");
        Expr *synth_len = expr_member(ctx->arena, array_expr, len_name);
        if (omega_prove_lt(ctx, index_expr, synth_len)) return;
        if (is_addr_of && omega_prove_le(ctx, index_expr, synth_len)) return;
    }

    // 6. No proof found — emit E085
    if (len_range.known) {
        if (!idx.known) {
            bounds_error("cannot statically verify index is within bounds",
                         index_expr, array_expr, idx, len_range,
                         "use `for i in 0..arr.len`, a parameter constraint, or an `in` guard");
        } else {
            bounds_error("index out of bounds",
                         index_expr, array_expr, idx, len_range, NULL);
        }
    } else {
        bounds_error("cannot prove index is within bounds for dynamic-length array",
                     index_expr, array_expr, idx, len_range,
                     "use `for i in 0..arr.len`, a fixed-length type `[N]`, or a `p in arr` guard");
    }
}

#endif /* SEMA_BOUNDS_H */
