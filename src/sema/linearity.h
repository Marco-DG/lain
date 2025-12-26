#ifndef SEMA_LINEARITY_H
#define SEMA_LINEARITY_H

// Linearity checker for the simple "mov" linear type in your AST.
//
// Usage:
//   Call sema_check_function_linearity(function_decl) after you've run
//   name-resolution and type-inference for that function, and before you
//   clear the local scope (i.e. while sema_locals contains locals).
//
// This implements a pragmatic subset of the Austral-style linearity rules
// sufficient for the examples you described.

#include "../ast.h"
#include "scope.h"
#include "resolve.h"
#include "typecheck.h"
#include "region.h"  // Region-based borrowing
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

// Enable debug prints by defining SEMA_LINEARITY_DEBUG (0 = off, 1 = on)
#ifndef SEMA_LINEARITY_DEBUG
#define SEMA_LINEARITY_DEBUG 0
#endif

#if SEMA_LINEARITY_DEBUG
#define DBG(fmt, ...) fprintf(stderr, "[linearity] " fmt "\n", ##__VA_ARGS__)
#else
#define DBG(fmt, ...) do {} while(0)
#endif

/* ---------- small helpers ---------- */

// Use the type_is_linear helper from ast.h for checking linear types
// This is just an alias for backward compatibility in this file
#define is_type_move(t) type_is_linear(t)

/* ---------- linear table (Id* keyed) ---------- */

typedef enum {
    LSTATE_UNCONSUMED,
    LSTATE_CONSUMED,
    LSTATE_BORROWED_READ,   // reserved for future
    LSTATE_BORROWED_WRITE   // reserved for future
} LState;

typedef struct LEntry {
    Id *id;            // Id pointer for the variable (identifies it)
    int defined_loop_depth; // loop depth where var was declared
    Region *region;    // region where var is defined (for borrow checking)
    LState state;
    struct LEntry *next;
} LEntry;

typedef struct {
    LEntry *head;
    BorrowTable *borrows;  // track active borrows
    Arena *arena;          // arena for allocations
} LTable;

static LTable *ltable_new(Arena *arena) {
    LTable *t = (LTable*)malloc(sizeof *t);
    t->head = NULL;
    t->arena = arena;
    t->borrows = arena ? borrow_table_new(arena) : NULL;
    return t;
}

static void ltable_free(LTable *t) {
    if (!t) return;
    LEntry *e = t->head;
    while (e) {
        LEntry *n = e->next;
        free(e);
        e = n;
    }
    free(t);
}

static LEntry *ltable_find(LTable *t, Id *id) {
    for (LEntry *e = t->head; e; e = e->next) {
        if (e->id->length == id->length &&
            strncmp(e->id->name, id->name, id->length) == 0) {
            return e;
        }
    }
    return NULL;
}

static void ltable_add(LTable *t, Id *id, int loop_depth) {
    if (!id) return;
    if (ltable_find(t, id)) return; // already present — ignore
    LEntry *e = (LEntry*)malloc(sizeof *e);
    e->id = id;
    e->defined_loop_depth = loop_depth;
    e->region = t->borrows ? t->borrows->current_region : NULL;
    e->state = LSTATE_UNCONSUMED;
    e->next = t->head;
    t->head = e;
    DBG("ltable_add: added '%.*s' loop_depth=%d region=%d", 
        (int)id->length, id->name ? id->name : "<null>", loop_depth,
        e->region ? e->region->id : -1);
}

static LTable *ltable_clone(LTable *src) {
    LTable *dst = ltable_new(src->arena);
    // Copy current region state from source
    if (dst->borrows && src->borrows) {
        dst->borrows->current_region = src->borrows->current_region;
    }
    // shallow-copy entries (Id* pointers are fine)
    for (LEntry *s = src->head; s; s = s->next) {
        ltable_add(dst, s->id, s->defined_loop_depth);
        LEntry *d = ltable_find(dst, s->id);
        if (d) {
            d->state = s->state;
            d->region = s->region;
        }
    }
    return dst;
}

/* update state strictly */
static void ltable_set_state(LTable *t, Id *id, LState st) {
    LEntry *e = ltable_find(t, id);
    if (!e) return;
    e->state = st;
}

/* mark consumed — checks loop-depth rule */
static void ltable_consume(LTable *t, Id *id, int current_loop_depth) {
    LEntry *e = ltable_find(t, id);
    if (!e) {
        // not tracked: ignore quietly
        DBG("ltable_consume: id '%.*s' not tracked (ignored)", id ? (int)id->length : 0, id ? id->name : "<null>");
        return;
    }
    if (e->state != LSTATE_UNCONSUMED) {
        fprintf(stderr, "sema error: linear variable '%.*s' was already used/consumed.\n",
                (int)e->id->length, e->id->name ? e->id->name : "<unknown>");
        exit(1);
    }
    if (e->defined_loop_depth != current_loop_depth) {
        fprintf(stderr, "sema error: attempting to consume linear variable '%.*s' defined outside a loop from inside a loop.\n",
                (int)e->id->length, e->id->name ? e->id->name : "<unknown>");
        exit(1);
    }
    e->state = LSTATE_CONSUMED;
    DBG("ltable_consume: consumed '%.*s' at loop_depth=%d", (int)e->id->length, e->id->name ? e->id->name : "<unknown>", current_loop_depth);
}

/* check all vars in table are consumed */
static void ltable_ensure_all_consumed(LTable *t) {
    int ok = 1;
    for (LEntry *e = t->head; e; e = e->next) {
        if (e->state != LSTATE_CONSUMED) {
            ok = 0;
            break;
        }
    }
    if (ok) {
        DBG("ltable_ensure_all_consumed: OK");
        return;
    }

    DBG("ltable_ensure_all_consumed: dumping table entries:");
    for (LEntry *e = t->head; e; e = e->next) {
        DBG("  entry '%.*s' state=%d def_loop=%d", (int)e->id->length, e->id->name ? e->id->name : "<unknown>", (int)e->state, e->defined_loop_depth);
    }

    for (LEntry *e = t->head; e; e = e->next) {
        if (e->state != LSTATE_CONSUMED) {
            fprintf(stderr, "sema error: linear variable '%.*s' was not consumed before return.\n",
                    (int)e->id->length, e->id->name ? e->id->name : "<unknown>");
        }
    }
    exit(1);
}

/* verify parent-consistency between two branch-results */
static void ltable_check_branch_consistency(LTable *parent, LTable *a, LTable *b, const char *stmt_name) {
    for (LEntry *p = parent->head; p; p = p->next) {
        LEntry *ea = ltable_find(a, p->id);
        LEntry *eb = ltable_find(b, p->id);
        LState sa = ea ? ea->state : LSTATE_UNCONSUMED;
        LState sb = eb ? eb->state : LSTATE_UNCONSUMED;
        if (sa != sb) {
            fprintf(stderr, "sema error: linear variable '%.*s' is used inconsistently in the branches of %s (one branch: %d, other: %d)\n",
                    (int)p->id->length, p->id->name ? p->id->name : "<unknown>", stmt_name ? stmt_name : "if", (int)sa, (int)sb);
            exit(1);
        }
    }
}

/* merge branch result back into parent */
static void ltable_merge_from_branch(LTable *parent, LTable *branch) {
    for (LEntry *p = parent->head; p; p = p->next) {
        LEntry *b = ltable_find(branch, p->id);
        if (b) p->state = b->state;
    }
}

/* ---------- helpers to find function decl robustly ---------- */

/* Try to find a function Decl by a mangled-or-raw name.
   Accepts either "module_fn" or "fn" and matches Decl->as.function_decl.name (raw name). */
static Decl *find_function_decl_by_mangled_or_raw(const char *mangled) {
    if (!mangled || !sema_decls) return NULL;

    // Quick pass: if the string exactly equals a raw name, use it.
    for (DeclList *dl = sema_decls; dl; dl = dl->next) {
        Decl *d = dl->decl;
        if (!d || d->kind != DECL_FUNCTION) continue;
        Id *fid = d->as.function_decl.name;
        if (!fid) continue;
        if (strcmp(mangled, fid->name) == 0) return d;
    }

    // Otherwise, try to match "<module>_<raw>" by suffix:
    size_t mlen = strlen(mangled);
    for (DeclList *dl = sema_decls; dl; dl = dl->next) {
        Decl *d = dl->decl;
        if (!d || d->kind != DECL_FUNCTION) continue;
        Id *fid = d->as.function_decl.name;
        if (!fid) continue;
        size_t rlen = (size_t)fid->length;
        if (rlen + 1 <= mlen && mangled[mlen - rlen - 1] == '_') {
            // compare suffix
            if (strncmp(mangled + (mlen - rlen), fid->name, rlen) == 0) {
                DBG("find_function_decl_by_mangled_or_raw: matched mangled='%s' -> raw='%s'", mangled, fid->name);
                return d;
            }
        }
    }
    DBG("find_function_decl_by_mangled_or_raw: no decl found for '%s'", mangled);
    return NULL;
}

/* ---------- expression traversal for linearity events ---------- */

static void sema_check_stmt_linearity_with_table(Stmt *s, LTable *tbl, int loop_depth);

static void sema_check_expr_linearity(Expr *e, LTable *tbl, int loop_depth) {
    if (!e) return;
    switch (e->kind) {
    case EXPR_IDENTIFIER:
        // bare identifiers are not consuming by themselves
        break;

    case EXPR_MEMBER:
        sema_check_expr_linearity(e->as.member_expr.target, tbl, loop_depth);
        break;

    case EXPR_INDEX:
        sema_check_expr_linearity(e->as.index_expr.target, tbl, loop_depth);
        sema_check_expr_linearity(e->as.index_expr.index, tbl, loop_depth);
        break;

    case EXPR_CALL: {
        // descend callee and args first
        sema_check_expr_linearity(e->as.call_expr.callee, tbl, loop_depth);
        for (ExprList *a = e->as.call_expr.args; a; a = a->next) {
            sema_check_expr_linearity(a->expr, tbl, loop_depth);
        }

        // Attempt to find the function Decl to learn param types.
        Decl *fn_decl = NULL;
        Expr *callee = e->as.call_expr.callee;
        if (callee && callee->kind == EXPR_IDENTIFIER) {
            const char *mangled = callee->as.identifier_expr.id->name;
            DBG("EXPR_CALL: callee mangled='%s'", mangled ? mangled : "<null>");
            // try existing fast-finder if present, else fallback to robust search:
            fn_decl = find_function_decl_by_mangled_or_raw(mangled);
        }

        if (fn_decl) {
            DBG("EXPR_CALL: matched function decl '%.*s'", (int)fn_decl->as.function_decl.name->length, fn_decl->as.function_decl.name->name);
            DeclList *params = fn_decl->as.function_decl.params;
            ExprList *args = e->as.call_expr.args;
            for (; params && args; params = params->next, args = args->next) {
                Type *pty = params->decl->as.variable_decl.type;
                Expr *arg = args->expr;
                if (!arg) continue;
                
                // Get the owner variable ID from the argument
                Id *owner_id = NULL;
                if (arg->kind == EXPR_IDENTIFIER) {
                    owner_id = arg->as.identifier_expr.id;
                } else if (arg->kind == EXPR_MEMBER) {
                    Expr *head = arg->as.member_expr.target;
                    while (head && head->kind == EXPR_MEMBER) head = head->as.member_expr.target;
                    if (head && head->kind == EXPR_IDENTIFIER) {
                        owner_id = head->as.identifier_expr.id;
                    }
                } else if (arg->kind == EXPR_MUT || arg->kind == EXPR_MOVE) {
                    // Unwrap mut/mov expression
                    Expr *inner = (arg->kind == EXPR_MUT) ? arg->as.mut_expr.expr : arg->as.move_expr.expr;
                    if (inner && inner->kind == EXPR_IDENTIFIER) {
                        owner_id = inner->as.identifier_expr.id;
                    }
                }
                
                if (!owner_id) continue;
                
                // Handle based on ownership mode
                if (pty && pty->mode == MODE_OWNED) {
                    // Move: check if borrowed, then consume
                    if (tbl->borrows && borrow_is_borrowed(tbl->borrows, owner_id)) {
                        fprintf(stderr, "borrow error: cannot move '%.*s' because it is currently borrowed\n",
                                (int)owner_id->length, owner_id->name);
                        exit(1);
                    }
                    DBG("EXPR_CALL: consuming '%.*s' (MODE_OWNED)", (int)owner_id->length, owner_id->name);
                    ltable_consume(tbl, owner_id, loop_depth);
                    // Invalidate any previous borrows of this owner
                    if (tbl->borrows) {
                        borrow_invalidate_owner(tbl->borrows, owner_id);
                    }
                } else if (pty && pty->mode == MODE_MUTABLE) {
                    // Mutable borrow: check for conflicts
                    LEntry *entry = ltable_find(tbl, owner_id);
                    Region *owner_region = entry ? entry->region : (tbl->borrows ? tbl->borrows->current_region : NULL);
                    
                    if (tbl->borrows && tbl->arena) {
                        // This will error if conflict detected
                        Id *borrow_id = params->decl->as.variable_decl.name;
                        borrow_register(tbl->arena, tbl->borrows, borrow_id, owner_id, MODE_MUTABLE, owner_region);
                        DBG("EXPR_CALL: registered mutable borrow of '%.*s'", (int)owner_id->length, owner_id->name);
                    }
                } else if (pty && pty->mode == MODE_SHARED) {
                    // Shared borrow: check for mutable conflicts only  
                    LEntry *entry = ltable_find(tbl, owner_id);
                    Region *owner_region = entry ? entry->region : (tbl->borrows ? tbl->borrows->current_region : NULL);
                    
                    if (tbl->borrows && tbl->arena) {
                        Id *borrow_id = params->decl->as.variable_decl.name;
                        borrow_register(tbl->arena, tbl->borrows, borrow_id, owner_id, MODE_SHARED, owner_region);
                        DBG("EXPR_CALL: registered shared borrow of '%.*s'", (int)owner_id->length, owner_id->name);
                    }
                }
            }
        } else {
            // Fallback: if argument expression types are known, consume any arg whose expr->type is move.
            DBG("EXPR_CALL: no fn_decl found for callee; checking arg types");
            for (ExprList *a = e->as.call_expr.args; a; a = a->next) {
                Expr *arg = a->expr;
                if (!arg) continue;
                if (arg->type && is_type_move(arg->type)) {
                    if (arg->kind == EXPR_IDENTIFIER) {
                        Id *idptr = arg->as.identifier_expr.id;
                        DBG("EXPR_CALL fallback: consume IDENT arg '%.*s' (by arg->type)", (int)idptr->length, idptr->name ? idptr->name : "<null>");
                        ltable_consume(tbl, idptr, loop_depth);
                    } else if (arg->kind == EXPR_MEMBER) {
                        Expr *head = arg->as.member_expr.target;
                        while (head && head->kind == EXPR_MEMBER) head = head->as.member_expr.target;
                        if (head && head->kind == EXPR_IDENTIFIER) {
                            DBG("EXPR_CALL fallback: consume MEMBER->IDENT head '%.*s' (by arg->type)", (int)head->as.identifier_expr.id->length, head->as.identifier_expr.id->name ? head->as.identifier_expr.id->name : "<null>");
                            ltable_consume(tbl, head->as.identifier_expr.id, loop_depth);
                        }
                    } else {
                        DBG("EXPR_CALL fallback: arg is move but not IDENT/MEMBER head; ignored for local-table consumption");
                    }
                }
            }
        }
        break;
    }

    case EXPR_UNARY:
        sema_check_expr_linearity(e->as.unary_expr.right, tbl, loop_depth);
        break;

    case EXPR_BINARY:
        sema_check_expr_linearity(e->as.binary_expr.left, tbl, loop_depth);
        sema_check_expr_linearity(e->as.binary_expr.right, tbl, loop_depth);
        break;

    case EXPR_MOVE: {
        Expr *inner = e->as.move_expr.expr;
        if (inner) {
            if (inner->kind == EXPR_IDENTIFIER) {
                Id *idptr = inner->as.identifier_expr.id;
                DBG("EXPR_MOVE: consume IDENT '%.*s'", (int)idptr->length, idptr->name ? idptr->name : "<null>");
                ltable_consume(tbl, idptr, loop_depth);
            } else if (inner->kind == EXPR_MEMBER) {
                Expr *head = inner->as.member_expr.target;
                while (head && head->kind == EXPR_MEMBER) head = head->as.member_expr.target;
                if (head && head->kind == EXPR_IDENTIFIER) {
                    DBG("EXPR_MOVE: consume MEMBER->IDENT head '%.*s'", (int)head->as.identifier_expr.id->length, head->as.identifier_expr.id->name ? head->as.identifier_expr.id->name : "<null>");
                    ltable_consume(tbl, head->as.identifier_expr.id, loop_depth);
                }
            }
            // recurse just in case (though for IDENT/MEMBER it does nothing)
            sema_check_expr_linearity(inner, tbl, loop_depth);
        }
        break;
    }

    default:
        // other expression kinds either contain no linear events or are handled above
        break;
    }
}

/* ---------- statement traversal ---------- */

static void sema_check_stmt_linearity_with_table(Stmt *s, LTable *tbl, int loop_depth) {
    if (!s) return;
    switch (s->kind) {

    case STMT_VAR: {
        Expr *init = s->as.var_stmt.expr;
        if (init) sema_check_expr_linearity(init, tbl, loop_depth);

        Type *ty = s->as.var_stmt.type;
        if (is_type_move(ty)) {
            Id *id = s->as.var_stmt.name;
            ltable_add(tbl, id, loop_depth);
        }
        break;
    }

    case STMT_ASSIGN: {
        Expr *lhs = s->as.assign_stmt.target;
        Expr *rhs = s->as.assign_stmt.expr;

        if (rhs) sema_check_expr_linearity(rhs, tbl, loop_depth);

        if (s->as.assign_stmt.is_const) {
            if (lhs && lhs->kind == EXPR_IDENTIFIER) {
                Id *id = lhs->as.identifier_expr.id;
                Type *decl_ty = rhs ? rhs->type : NULL;
                if (is_type_move(decl_ty)) {
                    ltable_add(tbl, id, loop_depth);
                }
            }
        }
        break;
    }

    case STMT_EXPR: {
        Expr *e = s->as.expr_stmt.expr;
        if (e && e->type && is_type_move(e->type)) {
            fprintf(stderr, "sema error: discarding value of linear type (move) is not allowed.\n");
            exit(1);
        }
        if (e) sema_check_expr_linearity(e, tbl, loop_depth);
        break;
    }

    case STMT_IF: {
        Expr *cond = s->as.if_stmt.cond;
        if (cond) sema_check_expr_linearity(cond, tbl, loop_depth);

        LTable *parent_snapshot = ltable_clone(tbl);

        LTable *then_tbl = ltable_clone(parent_snapshot);
        for (StmtList *b = s->as.if_stmt.then_branch; b; b = b->next) {
            sema_check_stmt_linearity_with_table(b->stmt, then_tbl, loop_depth);
        }

        LTable *else_tbl = ltable_clone(parent_snapshot);
        for (StmtList *b = s->as.if_stmt.else_branch; b; b = b->next) {
            sema_check_stmt_linearity_with_table(b->stmt, else_tbl, loop_depth);
        }

        ltable_check_branch_consistency(parent_snapshot, then_tbl, else_tbl, "if");
        ltable_merge_from_branch(tbl, then_tbl);

        ltable_free(parent_snapshot);
        ltable_free(then_tbl);
        ltable_free(else_tbl);
        break;
    }

    case STMT_FOR: {
        if (s->as.for_stmt.iterable) sema_check_expr_linearity(s->as.for_stmt.iterable, tbl, loop_depth);
        int new_depth = loop_depth + 1;
        for (StmtList *b = s->as.for_stmt.body; b; b = b->next) {
            sema_check_stmt_linearity_with_table(b->stmt, tbl, new_depth);
        }
        break;
    }

    case STMT_RETURN: {
        Expr *val = s->as.return_stmt.value;
        if (val) sema_check_expr_linearity(val, tbl, loop_depth);
        ltable_ensure_all_consumed(tbl);
        break;
    }

    case STMT_MATCH: {
        if (s->as.match_stmt.value) sema_check_expr_linearity(s->as.match_stmt.value, tbl, loop_depth);
        LTable *parent_snapshot = ltable_clone(tbl);
        LTable *first_branch = NULL;
        for (StmtMatchCase *c = s->as.match_stmt.cases; c; c = c->next) {
            LTable *branch_tbl = ltable_clone(parent_snapshot);
            if (c->pattern) sema_check_expr_linearity(c->pattern, branch_tbl, loop_depth);
            for (StmtList *b = c->body; b; b = b->next) {
                sema_check_stmt_linearity_with_table(b->stmt, branch_tbl, loop_depth);
            }
            if (!first_branch) first_branch = ltable_clone(branch_tbl);
            else ltable_check_branch_consistency(parent_snapshot, first_branch, branch_tbl, "match");
            ltable_free(branch_tbl);
        }
        if (first_branch) {
            ltable_merge_from_branch(tbl, first_branch);
            ltable_free(first_branch);
        }
        ltable_free(parent_snapshot);
        break;
    }

    default:
        // other statements: do nothing
        break;
    }
}

/* ---------- public entry: check one function ---------- */

static void sema_check_function_linearity(Decl *d) {
    if (!d || d->kind != DECL_FUNCTION) return;

    LTable *tbl = ltable_new(sema_arena);

    // add parameters that are move-typed
    for (DeclList *p = d->as.function_decl.params; p; p = p->next) {
        Id *pid = p->decl->as.variable_decl.name;
        Type *pty = p->decl->as.variable_decl.type;
        if (is_type_move(pty)) {
            ltable_add(tbl, pid, /*loop_depth=*/0);
        }
    }

    for (StmtList *sl = d->as.function_decl.body; sl; sl = sl->next) {
        sema_check_stmt_linearity_with_table(sl->stmt, tbl, /*loop_depth=*/0);
        // Clear borrows after each statement (NLL-like: borrows don't persist across statements)
        if (tbl->borrows) {
            borrow_clear_all(tbl->borrows);
        }
    }

    // final check (if function falls off end)
    ltable_ensure_all_consumed(tbl);

    ltable_free(tbl);
}

/* ---------- module-level entry: run linearity check over all functions ---------- */

static void sema_check_module_linearity(DeclList *decls) {
    if (!decls) return;
    for (DeclList *dl = decls; dl; dl = dl->next) {
        Decl *d = dl->decl;
        if (!d) continue;
        if (d->kind == DECL_FUNCTION) {
            sema_check_function_linearity(d);
        }
    }
}

#endif /* SEMA_LINEARITY_H */
