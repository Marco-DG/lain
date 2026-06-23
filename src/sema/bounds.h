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
│ Bounds Checking                                                    │
╚───────────────────────────────────────────────────────────────────*/

// Check if an index access is within bounds.
// array_expr is the expression being indexed (used to identify the array by name
// for constraint-based proof when the array has no size_expr annotation).
static void sema_check_bounds(RangeTable *ctx, Expr *index_expr, Type *array_type, Expr *array_expr) {
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
            fprintf(stderr, "[E085] bounds error: slice lower bound may be negative. Range: [%ld, %ld]\n",
                    (long)a.min, (long)a.max);
            exit(1);
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
                    fprintf(stderr, "[E085] bounds error: slice upper bound %ld out of bounds for length %ld\n",
                            (long)b.max, (long)len_range.min);
                    exit(1);
                }
            } else {
                fprintf(stderr, "[E085] bounds error: slice upper bound range unknown, cannot statically verify against length %ld.\n",
                        (long)len_range.min);
                exit(1);
            }
            // a <= len too (a == len gives empty slice, OK)
            if (a.known && a.max > len_range.min) {
                fprintf(stderr, "[E085] bounds error: slice lower bound %ld out of bounds for length %ld\n",
                        (long)a.max, (long)len_range.min);
                exit(1);
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
        fprintf(stderr, "[E085] bounds error: index may be negative. Range: [%ld, %ld]\n",
                (long)idx.min, (long)idx.max);
        exit(1);
    }

    // 4. Interval proof: idx_max < len_min (fast path — works for fixed arrays and
    //    sized slices whose size_expr evaluated to a concrete narrow range)
    if (len_range.known && idx.known) {
        if (idx.max >= len_range.min) {
            // Fall through to constraint proof before erroring
        } else {
            BOUNDS_DBG("OK: Index [%ld, %ld] < Length %ld",
                       (long)idx.min, (long)idx.max, (long)len_range.min);
            return; // safe via interval
        }
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

    // 6. No proof found — emit E085
    if (len_range.known) {
        if (!idx.known) {
            fprintf(stderr, "[E085] bounds error: index range unknown, cannot statically verify against length %ld. "
                    "Use a 'for i in 0..arr.len' loop or an 'if' guard to narrow the index range.\n",
                    (long)len_range.min);
        } else {
            fprintf(stderr, "[E085] bounds error: index %ld out of bounds for length %ld\n",
                    (long)idx.max, (long)len_range.min);
        }
    } else {
        fprintf(stderr, "[E085] bounds error: cannot prove index is within bounds for a dynamic-length array. "
                "Use 'for i in 0..arr.len', a fixed-length array ([N]), or an 'in' guard.\n");
    }
    exit(1);
}

#endif /* SEMA_BOUNDS_H */
