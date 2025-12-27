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

typedef struct {
    RangeEntry *head;
    Arena *arena;
} RangeTable;

static RangeTable *range_table_new(Arena *arena) {
    RangeTable *t = arena_push_aligned(arena, RangeTable);
    t->head = NULL;
    t->arena = arena;
    return t;
}

static void range_set(RangeTable *t, Id *var, Range r) {
    if (!t || !var) return;
    // Update existing
    for (RangeEntry *e = t->head; e; e = e->next) {
        if (e->var->length == var->length &&
            strncmp(e->var->name, var->name, var->length) == 0) {
            e->range = r;
            return;
        }
    }
    // Add new
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
        default: return range_unknown();
    }
}

#endif // SEMA_RANGES_H
