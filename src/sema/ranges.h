#ifndef SEMA_RANGES_H
#define SEMA_RANGES_H

#include "../ast.h"
#include <stdint.h>
#include <stdbool.h>

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

// Range arithmetic
static Range range_add(Range a, Range b) {
    if (!a.known || !b.known) return range_unknown();
    return range_make(a.min + b.min, a.max + b.max);
}

static Range range_sub(Range a, Range b) {
    if (!a.known || !b.known) return range_unknown();
    return range_make(a.min - b.max, a.max - b.min);
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

static Range sema_eval_range(Expr *e, RangeTable *t) {
    if (!e) return range_unknown();
    switch (e->kind) {
        case EXPR_LITERAL: return range_const(e->as.literal_expr.value);
        case EXPR_IDENTIFIER: return range_get(t, e->as.identifier_expr.id);
        case EXPR_BINARY: {
            Range l = sema_eval_range(e->as.binary_expr.left, t);
            Range r = sema_eval_range(e->as.binary_expr.right, t);
            if (e->as.binary_expr.op == TOKEN_PLUS) return range_add(l, r);
            if (e->as.binary_expr.op == TOKEN_MINUS) return range_sub(l, r);
            return range_unknown();
        }
        case EXPR_UNARY: {
            if (e->as.unary_expr.op == TOKEN_MINUS) {
                Range r = sema_eval_range(e->as.unary_expr.right, t);
                if (r.known) {
                    return range_make(-r.max, -r.min);
                }
            }
            return range_unknown();
        }
        default: return range_unknown();
    }
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
    *found = false;
    return 0;
}

// Apply a boolean constraint to the range table
// e.g. "x > 10" -> update x's min to 11
static void sema_apply_constraint(Expr *cond, RangeTable *t) {
    if (!cond || !t) return;

    if (cond->kind == EXPR_BINARY) {
        TokenKind op = cond->as.binary_expr.op;
        Expr *lhs = cond->as.binary_expr.left;
        Expr *rhs = cond->as.binary_expr.right;

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

            switch (op) {
                case TOKEN_ANGLE_BRACKET_RIGHT: // x > val -> min = val + 1
                    if (val + 1 > r.min) r.min = val + 1;
                    break;
                case TOKEN_ANGLE_BRACKET_RIGHT_EQUAL: // x >= val -> min = val
                    if (val > r.min) r.min = val;
                    break;
                case TOKEN_ANGLE_BRACKET_LEFT: // x < val -> max = val - 1
                    if (val - 1 < r.max) r.max = val - 1;
                    break;
                case TOKEN_ANGLE_BRACKET_LEFT_EQUAL: // x <= val -> max = val
                    if (val < r.max) r.max = val;
                    break;
                case TOKEN_EQUAL_EQUAL: // x == val -> min = val, max = val
                    r.min = val;
                    r.max = val;
                    break;
                case TOKEN_BANG_EQUAL:
                    // != is harder to represent in a single interval if it splits the range.
                    // For now, ignore.
                    break;
                default: break;
            }
            range_set(t, var, r);
        }
        // Handle literal on LHS: 10 < x  <=>  x > 10
        else if (lhs->kind == EXPR_LITERAL && rhs->kind == EXPR_IDENTIFIER) {
            Id *var = rhs->as.identifier_expr.id;
            int64_t val = lhs->as.literal_expr.value;
            Range r = range_get(t, var);
            if (!r.known) r = (Range){INT64_MIN, INT64_MAX, true};

            switch (op) {
                case TOKEN_ANGLE_BRACKET_RIGHT: // val > x  <=>  x < val
                    if (val - 1 < r.max) r.max = val - 1;
                    break;
                case TOKEN_ANGLE_BRACKET_RIGHT_EQUAL: // val >= x <=> x <= val
                    if (val < r.max) r.max = val;
                    break;
                case TOKEN_ANGLE_BRACKET_LEFT: // val < x   <=> x > val
                    if (val + 1 > r.min) r.min = val + 1;
                    break;
                case TOKEN_ANGLE_BRACKET_LEFT_EQUAL: // val <= x  <=> x >= val
                    if (val > r.min) r.min = val;
                    break;
                case TOKEN_EQUAL_EQUAL:
                    r.min = val;
                    r.max = val;
                    break;
                default: break;
            }
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
    }
}

// Apply the negation of a constraint
static void sema_apply_negated_constraint(Expr *cond, RangeTable *t) {
    if (!cond || !t) return;

    if (cond->kind == EXPR_BINARY) {
        TokenKind op = cond->as.binary_expr.op;
        Expr *lhs = cond->as.binary_expr.left;
        Expr *rhs = cond->as.binary_expr.right;

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
