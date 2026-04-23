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

// Check if an index access is within bounds
static void sema_check_bounds(RangeTable *ctx, Expr *index_expr, Type *array_type) {
    if (!index_expr || !array_type) return;

    // 1. Determine Array Length Range
    Range len_range = range_unknown();
    if (array_type->kind == TYPE_ARRAY && array_type->array_len >= 0) {
        len_range = range_const(array_type->array_len);
    } else if (array_type->kind == TYPE_SLICE && array_type->sentinel_len > 0) {
        // Known fixed-size slice (e.g. string literal)
        len_range = range_const(array_type->sentinel_len);
    } else {
        // Dynamic length or unknown.
        // We can't verify bounds statically against length if length is unknown.
        // However, we MUST check if index is negative in any case.
        // Also, if we have relational constraints (i < arr.len), we could verify.
        // For now, only check non-negative if length unknown.
        // If we want to be strict, we'd error on unknown length static access, 
        // but Lain allows dynamic arrays, so runtime checks would be needed (which we don't have yet).
        // Since "Zero Overhead" is a goal and "No Runtime Checks", 
        // strictly speaking we should REJECT accesses we can't prove safe.
        // But for usability, let's just warn or allow for now?
        // documentation.md says "Static verification".
        // Let's proceed with checking what we can.
    }
    
    // 2. Compute Index Range
    Range idx = sema_eval_range(index_expr, ctx);
    
    // 3. Verify: idx >= 0
    if (idx.known && idx.min < 0) {
        fprintf(stderr, "[VRA] bounds error: index may be negative. Range: [%ld, %ld]\n",
                (long)idx.min, (long)idx.max);
        exit(1);
    }

    // 4. Verify: idx < len
    if (len_range.known) {
        if (idx.known) {
             // We need rigorous proof: idx_max < len_min
             if (idx.max >= len_range.min) {
                 fprintf(stderr, "[VRA] bounds error: index %ld out of bounds for length %ld\n",
                         (long)idx.max, (long)len_range.min);
                 exit(1);
             }
             BOUNDS_DBG("OK: Index [%ld, %ld] < Length %ld", (long)idx.min, (long)idx.max, (long)len_range.min);
        } else {
            // Index unknown -> cannot prove safe. Reject per zero-overhead safety guarantee.
            fprintf(stderr, "[VRA] bounds error: index range unknown, cannot statically verify against length %ld. "
                    "Use a 'for i in 0..arr.len' loop or an 'if' guard to narrow the index range.\n", (long)len_range.min);
            exit(1);
        }
    } else {
        // Array length unknown (dynamic slice) — cannot verify statically.
        // F-040 fix: even with idx known, we cannot prove idx < len when len is
        // unknown. Reject to uphold the zero-cost safety guarantee.
        if (idx.known) {
            fprintf(stderr, "[VRA] bounds error: cannot prove index < length when the array length is unknown. "
                    "Index range [%ld, %ld]. Use a fixed-length array ([N]) or an 'in' guard to prove safety.\n",
                    (long)idx.min, (long)idx.max);
            exit(1);
        } else {
            fprintf(stderr, "[VRA] bounds error: both index and array length unknown, cannot verify bounds statically. "
                    "Use range constraints or 'in' keyword to prove safety.\n");
            exit(1);
        }
    }
}

#endif /* SEMA_BOUNDS_H */
