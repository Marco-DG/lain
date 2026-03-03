#ifndef SEMA_USE_ANALYSIS_H
#define SEMA_USE_ANALYSIS_H

/*
 * Use-Analysis Pre-pass for NLL (Non-Lexical Lifetimes)
 *
 * Walks a function body and computes, for each identifier, the index of the
 * last top-level statement where the identifier is referenced.
 *
 * This enables the borrow checker to release persistent borrows at the
 * point of last use instead of at scope exit.
 *
 * Control-flow rules:
 *   - if/else:  last_use = max(then_branch, else_branch)
 *   - for/while: any use inside maps to the loop statement's own index
 *   - match:    last_use = max(all arms)
 */

#include "../ast.h"
#include <string.h>

#ifndef SEMA_USE_ANALYSIS_DEBUG
#define SEMA_USE_ANALYSIS_DEBUG 0
#endif

#if SEMA_USE_ANALYSIS_DEBUG
#define USE_DBG(fmt, ...) fprintf(stderr, "[use_analysis] " fmt "\n", ##__VA_ARGS__)
#else
#define USE_DBG(fmt, ...) do {} while(0)
#endif

/*───────────────────────────────────────────────────────────────────╗
│ UseInfo: tracks last-use statement index for a variable            │
╚───────────────────────────────────────────────────────────────────*/

typedef struct UseInfo {
    Id *id;
    int last_use_stmt_idx;   // index of last top-level stmt where id is used
    struct UseInfo *next;
} UseInfo;

typedef struct UseTable {
    UseInfo *head;
    Arena *arena;
} UseTable;

static UseTable *use_table_new(Arena *arena) {
    UseTable *t = arena_push_aligned(arena, UseTable);
    t->head = NULL;
    t->arena = arena;
    return t;
}

static UseInfo *use_table_find(UseTable *t, Id *id) {
    if (!t || !id) return NULL;
    for (UseInfo *u = t->head; u; u = u->next) {
        if (u->id->length == id->length &&
            strncmp(u->id->name, id->name, id->length) == 0) {
            return u;
        }
    }
    return NULL;
}

static void use_table_update(UseTable *t, Id *id, int stmt_idx) {
    if (!t || !id) return;
    UseInfo *u = use_table_find(t, id);
    if (u) {
        if (stmt_idx > u->last_use_stmt_idx) {
            u->last_use_stmt_idx = stmt_idx;
        }
    } else {
        u = arena_push_aligned(t->arena, UseInfo);
        u->id = id;
        u->last_use_stmt_idx = stmt_idx;
        u->next = t->head;
        t->head = u;
    }
}

static int use_table_get_last_use(UseTable *t, Id *id) {
    UseInfo *u = use_table_find(t, id);
    return u ? u->last_use_stmt_idx : -1;
}

/*───────────────────────────────────────────────────────────────────╗
│ Expression walker: collect identifier uses                         │
╚───────────────────────────────────────────────────────────────────*/

static void use_walk_expr(Expr *e, UseTable *t, int stmt_idx) {
    if (!e) return;
    switch (e->kind) {
    case EXPR_IDENTIFIER:
        if (e->as.identifier_expr.id) {
            use_table_update(t, e->as.identifier_expr.id, stmt_idx);
        }
        break;

    case EXPR_MEMBER:
        use_walk_expr(e->as.member_expr.target, t, stmt_idx);
        break;

    case EXPR_INDEX:
        use_walk_expr(e->as.index_expr.target, t, stmt_idx);
        use_walk_expr(e->as.index_expr.index, t, stmt_idx);
        break;

    case EXPR_CALL:
        use_walk_expr(e->as.call_expr.callee, t, stmt_idx);
        for (ExprList *a = e->as.call_expr.args; a; a = a->next) {
            use_walk_expr(a->expr, t, stmt_idx);
        }
        break;

    case EXPR_BINARY:
        use_walk_expr(e->as.binary_expr.left, t, stmt_idx);
        use_walk_expr(e->as.binary_expr.right, t, stmt_idx);
        break;

    case EXPR_UNARY:
        use_walk_expr(e->as.unary_expr.right, t, stmt_idx);
        break;

    case EXPR_MUT:
        use_walk_expr(e->as.mut_expr.expr, t, stmt_idx);
        break;

    case EXPR_MOVE:
        use_walk_expr(e->as.move_expr.expr, t, stmt_idx);
        break;

    case EXPR_MATCH:
        use_walk_expr(e->as.match_expr.value, t, stmt_idx);
        for (ExprMatchCase *c = e->as.match_expr.cases; c; c = c->next) {
            for (ExprList *p = c->patterns; p; p = p->next) {
                use_walk_expr(p->expr, t, stmt_idx);
            }
            use_walk_expr(c->body, t, stmt_idx);
        }
        break;

    case EXPR_CAST:
        use_walk_expr(e->as.cast_expr.expr, t, stmt_idx);
        break;

    default:
        // Literals etc. — no identifiers
        break;
    }
}

/*───────────────────────────────────────────────────────────────────╗
│ Statement walker: assign stmt_idx to each top-level statement      │
│ For nested blocks (if/for/while/match), uses inside map to the     │
│ enclosing top-level statement's index.                             │
╚───────────────────────────────────────────────────────────────────*/

static void use_walk_stmt(Stmt *s, UseTable *t, int stmt_idx);

static void use_walk_stmt_list(StmtList *list, UseTable *t, int stmt_idx) {
    for (StmtList *sl = list; sl; sl = sl->next) {
        use_walk_stmt(sl->stmt, t, stmt_idx);
    }
}

static void use_walk_stmt(Stmt *s, UseTable *t, int stmt_idx) {
    if (!s) return;
    switch (s->kind) {
    case STMT_VAR:
        if (s->as.var_stmt.expr) {
            use_walk_expr(s->as.var_stmt.expr, t, stmt_idx);
        }
        // Note: the declared variable name is NOT a "use" — it's a definition
        break;

    case STMT_ASSIGN:
        if (s->as.assign_stmt.target) {
            use_walk_expr(s->as.assign_stmt.target, t, stmt_idx);
        }
        if (s->as.assign_stmt.expr) {
            use_walk_expr(s->as.assign_stmt.expr, t, stmt_idx);
        }
        break;

    case STMT_EXPR:
        use_walk_expr(s->as.expr_stmt.expr, t, stmt_idx);
        break;

    case STMT_RETURN:
        if (s->as.return_stmt.value) {
            use_walk_expr(s->as.return_stmt.value, t, stmt_idx);
        }
        break;

    case STMT_IF:
        use_walk_expr(s->as.if_stmt.cond, t, stmt_idx);
        use_walk_stmt_list(s->as.if_stmt.then_branch, t, stmt_idx);
        use_walk_stmt_list(s->as.if_stmt.else_branch, t, stmt_idx);
        break;

    case STMT_FOR:
        if (s->as.for_stmt.iterable) {
            use_walk_expr(s->as.for_stmt.iterable, t, stmt_idx);
        }
        use_walk_stmt_list(s->as.for_stmt.body, t, stmt_idx);
        break;

    case STMT_WHILE:
        if (s->as.while_stmt.cond) {
            use_walk_expr(s->as.while_stmt.cond, t, stmt_idx);
        }
        use_walk_stmt_list(s->as.while_stmt.body, t, stmt_idx);
        break;

    case STMT_MATCH:
        if (s->as.match_stmt.value) {
            use_walk_expr(s->as.match_stmt.value, t, stmt_idx);
        }
        for (StmtMatchCase *c = s->as.match_stmt.cases; c; c = c->next) {
            for (ExprList *p = c->patterns; p; p = p->next) {
                use_walk_expr(p->expr, t, stmt_idx);
            }
            use_walk_stmt_list(c->body, t, stmt_idx);
        }
        break;

    case STMT_UNSAFE:
        use_walk_stmt_list(s->as.unsafe_stmt.body, t, stmt_idx);
        break;

    case STMT_DEFER:
        use_walk_stmt(s->as.defer_stmt.stmt, t, stmt_idx);
        break;

    default:
        break;
    }
}

/*───────────────────────────────────────────────────────────────────╗
│ Public API: compute last-use for all identifiers in a function     │
╚───────────────────────────────────────────────────────────────────*/

static UseTable *use_compute_last_uses(StmtList *body, Arena *arena) {
    UseTable *t = use_table_new(arena);
    int stmt_idx = 0;
    for (StmtList *sl = body; sl; sl = sl->next, stmt_idx++) {
        use_walk_stmt(sl->stmt, t, stmt_idx);
    }
    
    USE_DBG("use_compute_last_uses: computed last uses for %d statements", stmt_idx);
    for (UseInfo *u = t->head; u; u = u->next) {
        USE_DBG("  '%.*s' last_use_stmt_idx=%d", 
                (int)u->id->length, u->id->name, u->last_use_stmt_idx);
    }
    
    return t;
}

#endif /* SEMA_USE_ANALYSIS_H */
