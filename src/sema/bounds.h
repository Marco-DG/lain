#ifndef SEMA_BOUNDS_H
#define SEMA_BOUNDS_H

/*
 * Static Bounds Checking for Lain
 * 
 * Verifies array index accesses at compile time when possible.
 * Uses compile-time constant evaluation to prove safety.
 */

#include "../ast.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <stdint.h>

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
│ Compile-time constant evaluation                                  │
╚───────────────────────────────────────────────────────────────────*/

// Result of compile-time evaluation
typedef struct {
    bool is_const;      // true if we could evaluate to a constant
    int64_t value;      // the constant value (valid only if is_const)
} ConstEvalResult;

// Try to evaluate an expression as a compile-time integer
static ConstEvalResult sema_eval_comptime_int(Expr *e) {
    ConstEvalResult result = { .is_const = false, .value = 0 };
    
    if (!e) return result;
    
    switch (e->kind) {
    case EXPR_LITERAL: {
        // ExprLiteral has an int value field
        result.is_const = true;
        result.value = (int64_t)e->as.literal_expr.value;
        BOUNDS_DBG("eval EXPR_LITERAL: %lld", (long long)result.value);
        return result;
    }
        
    case EXPR_UNARY: {
        ConstEvalResult inner = sema_eval_comptime_int(e->as.unary_expr.right);
        if (!inner.is_const) return result;
        
        switch (e->as.unary_expr.op) {
        case TOKEN_MINUS:
            result.is_const = true;
            result.value = -inner.value;
            break;
        case TOKEN_BANG:
            result.is_const = true;
            result.value = !inner.value;
            break;
        default:
            break;
        }
        return result;
    }
    
    case EXPR_BINARY: {
        ConstEvalResult left = sema_eval_comptime_int(e->as.binary_expr.left);
        ConstEvalResult right = sema_eval_comptime_int(e->as.binary_expr.right);
        
        if (!left.is_const || !right.is_const) return result;
        
        result.is_const = true;
        switch (e->as.binary_expr.op) {
        case TOKEN_PLUS:
            result.value = left.value + right.value;
            break;
        case TOKEN_MINUS:
            result.value = left.value - right.value;
            break;
        case TOKEN_ASTERISK:
            result.value = left.value * right.value;
            break;
        case TOKEN_SLASH:
            if (right.value == 0) {
                fprintf(stderr, "bounds error: division by zero in compile-time expression\n");
                exit(1);
            }
            result.value = left.value / right.value;
            break;
        case TOKEN_PERCENT:
            if (right.value == 0) {
                fprintf(stderr, "bounds error: modulo by zero in compile-time expression\n");
                exit(1);
            }
            result.value = left.value % right.value;
            break;
        case TOKEN_ANGLE_BRACKET_LEFT:
            result.value = left.value < right.value;
            break;
        case TOKEN_ANGLE_BRACKET_LEFT_EQUAL:
            result.value = left.value <= right.value;
            break;
        case TOKEN_ANGLE_BRACKET_RIGHT:
            result.value = left.value > right.value;
            break;
        case TOKEN_ANGLE_BRACKET_RIGHT_EQUAL:
            result.value = left.value >= right.value;
            break;
        case TOKEN_EQUAL_EQUAL:
            result.value = left.value == right.value;
            break;
        case TOKEN_BANG_EQUAL:
            result.value = left.value != right.value;
            break;
        default:
            result.is_const = false;
            break;
        }
        BOUNDS_DBG("eval EXPR_BINARY: %lld op %lld = %lld", 
                   (long long)left.value, (long long)right.value, (long long)result.value);
        return result;
    }
    
    case EXPR_IDENTIFIER: {
        // Could look up comptime variables here in the future
        // For now, identifiers are not compile-time constants
        BOUNDS_DBG("eval EXPR_IDENTIFIER: not const (id='%.*s')", 
                   (int)e->as.identifier_expr.id->length, e->as.identifier_expr.id->name);
        return result;
    }
    
    default:
        return result;
    }
}

/*───────────────────────────────────────────────────────────────────╗
│ Bounds checking                                                    │
╚───────────────────────────────────────────────────────────────────*/

typedef enum {
    BOUNDS_OK,              // Provably within bounds
    BOUNDS_ERROR,           // Provably out of bounds
    BOUNDS_UNKNOWN,         // Cannot determine at compile time
} BoundsCheckResult;

// Check if an index access is within bounds
// Returns BOUNDS_OK if provably safe, BOUNDS_ERROR if provably unsafe,
// BOUNDS_UNKNOWN if we can't determine at compile time
static BoundsCheckResult sema_check_bounds(Expr *index_expr, Type *array_type) {
    if (!index_expr || !array_type) return BOUNDS_UNKNOWN;
    
    // Get array length (if known)
    isize array_len = -1;
    if (array_type->kind == TYPE_ARRAY && array_type->array_len >= 0) {
        array_len = array_type->array_len;
    } else if (array_type->kind == TYPE_SLICE && array_type->sentinel_len > 0) {
        array_len = (isize)array_type->sentinel_len;
    }
    
    if (array_len < 0) {
        BOUNDS_DBG("check_bounds: array length unknown");
        return BOUNDS_UNKNOWN;  // Can't check if we don't know the length
    }
    
    // Try to evaluate the index
    ConstEvalResult index_val = sema_eval_comptime_int(index_expr);
    
    if (!index_val.is_const) {
        BOUNDS_DBG("check_bounds: index not const, array_len=%ld", (long)array_len);
        return BOUNDS_UNKNOWN;  // Index is not compile-time constant
    }
    
    // Check bounds
    if (index_val.value < 0) {
        BOUNDS_DBG("check_bounds: NEGATIVE index %lld", (long long)index_val.value);
        return BOUNDS_ERROR;
    }
    
    if (index_val.value >= array_len) {
        BOUNDS_DBG("check_bounds: OUT OF BOUNDS index %lld >= len %ld", 
                   (long long)index_val.value, (long)array_len);
        return BOUNDS_ERROR;
    }
    
    BOUNDS_DBG("check_bounds: OK index %lld < len %ld", 
               (long long)index_val.value, (long)array_len);
    return BOUNDS_OK;
}

// Report a bounds error with details
static void sema_report_bounds_error(Expr *index_expr, Type *array_type) {
    isize array_len = -1;
    if (array_type && array_type->kind == TYPE_ARRAY && array_type->array_len >= 0) {
        array_len = array_type->array_len;
    } else if (array_type && array_type->kind == TYPE_SLICE && array_type->sentinel_len > 0) {
        array_len = (isize)array_type->sentinel_len;
    }
    
    ConstEvalResult index_val = sema_eval_comptime_int(index_expr);
    
    if (index_val.is_const && array_len >= 0) {
        fprintf(stderr, "bounds error: index %lld is out of bounds for array of length %ld\n",
                (long long)index_val.value, (long)array_len);
    } else {
        fprintf(stderr, "bounds error: array index out of bounds\n");
    }
}

#endif /* SEMA_BOUNDS_H */
