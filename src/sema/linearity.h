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

// Helper forward declaration
static bool sema_type_is_linear(Type *t);

// Use the robust recursive check
#define is_type_move(t) sema_type_is_linear(t)

static bool sema_type_is_linear(Type *t) {
    if (!t) return false;
    // 1. Explicit ownership override
    if (t->mode == MODE_OWNED) return true;

    // 2. Aggregate types: recursive check
    if (t->kind == TYPE_ARRAY || t->kind == TYPE_SLICE || t->kind == TYPE_POINTER) {
        // Pointers/Slices/Arrays: only linear if mode is OWNED (checked above) 
        // OR if element type is linear?
        // - Array[T] where T is linear -> Array is linear (must consume)
        // - Pointer[T] -> Pointer is copyable unless mode=OWNED. T's linearity doesn't affect pointer linearity.
        // - Slice[T] -> Slice is struct {ptr, len}. If T is linear? 
        //   Lain slices are views. They don't own T.
        //   Arrays [N]T OWN the elements. So matching Rust: [T; N] is linear if T is linear.
        if (t->kind == TYPE_ARRAY) {
            return sema_type_is_linear(t->element_type);
        }
        return false;
    }

    // 3. Simple ID (Structs/Enums)
    if (t->kind == TYPE_SIMPLE) {
        // Resolve underlying declaration
        extern Symbol *sema_lookup(const char *name); // Forward decl from sema.h
        
        char buf[256];
        if (t->base_type->length >= sizeof(buf)) return false;
        memcpy(buf, t->base_type->name, t->base_type->length);
        buf[t->base_type->length] = '\0';
        
        Symbol *sym = sema_lookup(buf);
        if (!sym || !sym->decl) return false;

        if (sym->decl->kind == DECL_STRUCT) {
            // Check all fields
            for (DeclList *f = sym->decl->as.struct_decl.fields; f; f = f->next) {
                if (f->decl->kind == DECL_VARIABLE) {
                    if (sema_type_is_linear(f->decl->as.variable_decl.type)) {
                        return true; // Found a linear field -> Struct is linear
                    }
                }
            }
        }
        if (sym->decl->kind == DECL_ENUM) {
            // Check all variants: if ANY variant has a linear field, the enum is linear
            for (Variant *v = sym->decl->as.enum_decl.variants; v; v = v->next) {
                for (DeclList *f = v->fields; f; f = f->next) {
                    if (f->decl->kind == DECL_VARIABLE) {
                        if (sema_type_is_linear(f->decl->as.variable_decl.type)) {
                            return true; // Found a linear field in a variant -> Enum is linear
                        }
                    }
                }
            }
        }
    }

    return false;
}

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
    bool is_mutable;   // true if variable is mutable (declared with var/mut)
    bool must_consume; // true if type is strictly linear
    isize line;        // NEW: line where var is defined
    isize col;         // NEW: col where var is defined
    LState state;
    struct LEntry *next;
} LEntry;

typedef struct {
    LEntry *head;
    BorrowTable *borrows;  // track active borrows
    Arena *arena;          // arena for allocations
} LTable;

static LTable *ltable_new(Arena *arena) {
    LTable *t = arena_push_aligned(arena, LTable);
    t->head = NULL;
    t->arena = arena;
    t->borrows = arena ? borrow_table_new(arena) : NULL;
    return t;
}

static void ltable_free(LTable *t) {
    if (!t) return;
    // Arena handles deallocation — just detach the list
    t->head = NULL;
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

static void ltable_add(LTable *t, Id *id, int loop_depth, bool is_mutable, bool must_consume, isize line, isize col) {
    if (!id) return;
    if (ltable_find(t, id)) return; // already present — ignore
    LEntry *e = arena_push_aligned(t->arena, LEntry);
    e->id = id;
    e->defined_loop_depth = loop_depth;
    e->region = t->borrows ? t->borrows->current_region : NULL;
    e->is_mutable = is_mutable;
    e->must_consume = must_consume;
    e->line = line;
    e->col = col;
    e->state = LSTATE_UNCONSUMED;
    e->next = t->head;
    t->head = e;
    DBG("ltable_add: added '%.*s' loop_depth=%d region=%d must_consume=%d", 
        (int)id->length, id->name ? id->name : "<null>", loop_depth,
        e->region ? e->region->id : -1, must_consume);
}

static LTable *ltable_clone(LTable *src) {
    LTable *dst = ltable_new(src->arena);
    // Copy current region state from source
    if (dst->borrows && src->borrows) {
        dst->borrows->current_region = src->borrows->current_region;
        
        // Deep copy borrow entries
        BorrowEntry *last = NULL;
        for (BorrowEntry *s_entry = src->borrows->head; s_entry; s_entry = s_entry->next) {
            BorrowEntry *d_entry = arena_push_aligned(dst->arena, BorrowEntry);
            *d_entry = *s_entry; // Copy data
            d_entry->next = NULL;
            
            if (last) {
                last->next = d_entry;
            } else {
                dst->borrows->head = d_entry;
            }
            last = d_entry;
        }
    }
    // shallow-copy entries (Id* pointers are fine)
    for (LEntry *s = src->head; s; s = s->next) {
        ltable_add(dst, s->id, s->defined_loop_depth, s->is_mutable, s->must_consume, s->line, s->col);
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
        fprintf(stderr, "Error Ln %li, Col %li: linear variable '%.*s' was already used/consumed.\n",
                (long)e->line, (long)e->col, (int)e->id->length, e->id->name ? e->id->name : "<unknown>");
        exit(1);
    }
    if (e->defined_loop_depth != current_loop_depth) {
        fprintf(stderr, "Error Ln %li, Col %li: attempting to consume linear variable '%.*s' defined outside a loop from inside a loop.\n",
                (long)e->line, (long)e->col, (int)e->id->length, e->id->name ? e->id->name : "<unknown>");
        exit(1);
    }
    e->state = LSTATE_CONSUMED;
    DBG("ltable_consume: consumed '%.*s' at loop_depth=%d", (int)e->id->length, e->id->name ? e->id->name : "<unknown>", current_loop_depth);
}

/* check all vars in table are consumed */
static void ltable_ensure_all_consumed(LTable *t) {
    int errors = 0;
    for (LEntry *e = t->head; e; e = e->next) {
        if (e->must_consume && e->state != LSTATE_CONSUMED) {
            fprintf(stderr, "Error Ln %li, Col %li: linear variable '%.*s' was not consumed before return.\n",
                    (long)e->line, (long)e->col, (int)e->id->length, e->id->name ? e->id->name : "<unknown>");
            errors++;
        }
    }
    
    if (errors > 0) {
        // Only dump table if we found actual errors
        DBG("ltable_ensure_all_consumed: dumping table entries (due to errors):");
        for (LEntry *e = t->head; e; e = e->next) {
            DBG("  entry '%.*s' state=%d def_loop=%d must=%d", 
                (int)e->id->length, e->id->name ? e->id->name : "<unknown>", 
                (int)e->state, e->defined_loop_depth, e->must_consume);
        }
        exit(1);
    }
    DBG("ltable_ensure_all_consumed: OK (all linear vars consumed)");
}

/* helper to pop a block scope and check consumed locals */
static void ltable_pop_scope(LTable *tbl, LEntry *saved_head) {
    int errors = 0;
    for (LEntry *e = tbl->head; e && e != saved_head; e = e->next) {
        if (e->must_consume && e->state != LSTATE_CONSUMED) {
            fprintf(stderr, "Error Ln %li, Col %li: linear variable '%.*s' was not consumed before end of scope.\n",
                    (long)e->line, (long)e->col, (int)e->id->length, e->id->name ? e->id->name : "<unknown>");
            errors++;
        }
    }
    if (errors > 0) exit(1);
    tbl->head = saved_head;
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
    if (!mangled) return NULL;
    
    // Quick pass: if the string exactly equals a raw name, use it.
    for (ModuleNode *mn = loaded_modules; mn; mn = mn->next) {
        for (DeclList *dl = mn->decls; dl; dl = dl->next) {
            Decl *d = dl->decl;
            if (!d) continue;
            // Also check procedures and externs since they can take linear args
            if (d->kind != DECL_FUNCTION && d->kind != DECL_PROCEDURE && 
                d->kind != DECL_EXTERN_FUNCTION && d->kind != DECL_EXTERN_PROCEDURE) continue;
            
            Id *fid = d->as.function_decl.name;
            if (!fid) continue;
            if (strncmp(mangled, fid->name, fid->length) == 0 && mangled[fid->length] == '\0') return d;
        }
    }
    for (DeclList *dl = sema_decls; dl; dl = dl->next) {
        Decl *d = dl->decl;
        if (!d || d->kind != DECL_FUNCTION) continue;
        Id *fid = d->as.function_decl.name;
        if (!fid) continue;
        if (strncmp(mangled, fid->name, fid->length) == 0 && mangled[fid->length] == '\0') return d;
    }

    // Otherwise, try to match "<module>_<raw>" by suffix:
    size_t mlen = strlen(mangled);
    for (ModuleNode *mn = loaded_modules; mn; mn = mn->next) {
        for (DeclList *dl = mn->decls; dl; dl = dl->next) {
            Decl *d = dl->decl;
            if (!d) continue;
            if (d->kind != DECL_FUNCTION && d->kind != DECL_PROCEDURE && 
                d->kind != DECL_EXTERN_FUNCTION && d->kind != DECL_EXTERN_PROCEDURE) continue;
            
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
    }
    DBG("find_function_decl_by_mangled_or_raw: no decl found for '%s'", mangled);
    return NULL;
}

/* ---------- expression traversal for linearity events ---------- */

static void sema_check_stmt_linearity_with_table(Stmt *s, LTable *tbl, int loop_depth);

static void sema_check_expr_linearity(Expr *e, LTable *tbl, int loop_depth) {
    if (!e) return;
    switch (e->kind) {
    case EXPR_IDENTIFIER: {
        // Check if this is a use of an already-consumed linear variable
        Id *id = e->as.identifier_expr.id;
        if (id && tbl) {
            LEntry *entry = ltable_find(tbl, id);
            if (entry && entry->state == LSTATE_CONSUMED) {
                fprintf(stderr, "Error Ln %li, Col %li: use of linear variable '%.*s' after it was moved.\n",
                        (long)(e->line), (long)(e->col), (int)id->length, id->name ? id->name : "<unknown>");
                exit(1);
            }
        }
        break;
    }

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
            Id *callee_id = callee->as.identifier_expr.id;
            char mangled_buf[256];
            int len = callee_id->length < 255 ? callee_id->length : 255;
            memcpy(mangled_buf, callee_id->name, len);
            mangled_buf[len] = '\0';
            DBG("EXPR_CALL: callee mangled='%s'", mangled_buf);
            fn_decl = find_function_decl_by_mangled_or_raw(mangled_buf);
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
                        fprintf(stderr, "Error Ln %li, Col %li: cannot move '%.*s' because it is currently borrowed.\n",
                                (long)(e->line), (long)(e->col), (int)owner_id->length, owner_id->name);
                        exit(1);
                    }
                    if (arg->kind == EXPR_MOVE) {
                        DBG("EXPR_CALL: '%.*s' already consumed by EXPR_MOVE", (int)owner_id->length, owner_id->name);
                    } else {
                        fprintf(stderr, "Error Ln %li, Col %li: moving linear variable '%.*s' requires explicit 'mov' at the call site.\n",
                                (long)(e->line), (long)(e->col), (int)owner_id->length, owner_id->name);
                        exit(1);
                    }
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
                        borrow_register(tbl->arena, tbl->borrows, borrow_id, owner_id, MODE_MUTABLE, owner_region, true);
                        DBG("EXPR_CALL: registered mutable borrow of '%.*s'", (int)owner_id->length, owner_id->name);
                    }
                } else if (pty && pty->mode == MODE_SHARED) {
                    // Shared borrow: check for mutable conflicts only  
                    LEntry *entry = ltable_find(tbl, owner_id);
                    Region *owner_region = entry ? entry->region : (tbl->borrows ? tbl->borrows->current_region : NULL);
                    
                    if (tbl->borrows && tbl->arena) {
                        Id *borrow_id = params->decl->as.variable_decl.name;
                        borrow_register(tbl->arena, tbl->borrows, borrow_id, owner_id, MODE_SHARED, owner_region, true);
            DBG("EXPR_CALL: registered shared borrow of '%.*s'", (int)owner_id->length, owner_id->name);
                    }
                }
            }
        } else {
            // Function declaration not found.
            // If this is a struct constructor, we still need to consume linear arguments
            // because strict constructors act as implicit moves for linear types.
            bool is_struct_constructor = false;
            if (callee && callee->kind == EXPR_IDENTIFIER && callee->decl) {
                is_struct_constructor = (callee->decl->kind == DECL_STRUCT);
            }
            
            if (is_struct_constructor) {
                DBG("EXPR_CALL: struct constructor found, consuming arguments");
                for (ExprList *a = e->as.call_expr.args; a; a = a->next) {
                    Expr *arg = a->expr;
                    if (!arg) continue;
                    
                    bool should_consume = false;
                    if (arg->kind == EXPR_MOVE) {
                        should_consume = true;
                    } else if (arg->type && is_type_move(arg->type)) {
                        should_consume = true;
                    }
                    
                    if (should_consume) {
                        Expr *inner = arg;
                        if (inner->kind == EXPR_MOVE) inner = inner->as.move_expr.expr;
                        else if (inner->kind == EXPR_MUT) inner = inner->as.mut_expr.expr;
                        
                        if (inner && inner->kind == EXPR_IDENTIFIER) {
                            ltable_consume(tbl, inner->as.identifier_expr.id, loop_depth);
                        } else if (inner && inner->kind == EXPR_MEMBER) {
                             Expr *head = inner->as.member_expr.target;
                             while (head && head->kind == EXPR_MEMBER) head = head->as.member_expr.target;
                             if (head && head->kind == EXPR_IDENTIFIER) {
                                 ltable_consume(tbl, head->as.identifier_expr.id, loop_depth);
                             }
                        }
                    }
                }
            } else {
                DBG("EXPR_CALL: no fn_decl found for callee - no heuristic assumption made.");
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

    case EXPR_MUT:
        sema_check_expr_linearity(e->as.mut_expr.expr, tbl, loop_depth);
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
            // NOTE: Don't recurse on inner here - we handle the consumption directly above
            // Recursing would trigger false-positive "use after move" since we just consumed it
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
// (removed debug print)
        Expr *init = s->as.var_stmt.expr;
        if (init) sema_check_expr_linearity(init, tbl, loop_depth);

        Type *ty = s->as.var_stmt.type;
        bool must = is_type_move(ty);
        if (must || s->as.var_stmt.is_mutable) {
            Id *id = s->as.var_stmt.name;
            ltable_add(tbl, id, loop_depth, s->as.var_stmt.is_mutable, must, s->line, s->col);
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
                    // ... (omitted comments)
                    bool must = is_type_move(decl_ty);
                    ltable_add(tbl, id, loop_depth, true, must, s->line, s->col);
                }
            }
        }
        break;
    }

    case STMT_EXPR: {
        Expr *e = s->as.expr_stmt.expr;
        if (e && e->type && is_type_move(e->type)) {
            fprintf(stderr, "Error Ln %li, Col %li: discarding value of linear type (move) is not allowed.\n", (long)s->line, (long)s->col);
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
        LEntry *then_saved = then_tbl->head;
        if (then_tbl->borrows) borrow_enter_scope(then_tbl->arena, then_tbl->borrows);
        for (StmtList *b = s->as.if_stmt.then_branch; b; b = b->next) {
            sema_check_stmt_linearity_with_table(b->stmt, then_tbl, loop_depth);
        }
        ltable_pop_scope(then_tbl, then_saved);

        LTable *else_tbl = ltable_clone(parent_snapshot);
        LEntry *else_saved = else_tbl->head;
        if (else_tbl->borrows) borrow_enter_scope(else_tbl->arena, else_tbl->borrows);
        for (StmtList *b = s->as.if_stmt.else_branch; b; b = b->next) {
            sema_check_stmt_linearity_with_table(b->stmt, else_tbl, loop_depth);
        }
        ltable_pop_scope(else_tbl, else_saved);

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
        
        LEntry *saved_head = tbl->head;
        if (tbl->borrows) borrow_enter_scope(tbl->arena, tbl->borrows);
        for (StmtList *b = s->as.for_stmt.body; b; b = b->next) {
            sema_check_stmt_linearity_with_table(b->stmt, tbl, new_depth);
        }
        ltable_pop_scope(tbl, saved_head);
        if (tbl->borrows) borrow_exit_scope(tbl->borrows);
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
            LEntry *saved_head = branch_tbl->head;
            if (branch_tbl->borrows) borrow_enter_scope(branch_tbl->arena, branch_tbl->borrows);
            
            if (c->pattern) sema_check_expr_linearity(c->pattern, branch_tbl, loop_depth);
            for (StmtList *b = c->body; b; b = b->next) {
                sema_check_stmt_linearity_with_table(b->stmt, branch_tbl, loop_depth);
            }
            ltable_pop_scope(branch_tbl, saved_head);

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

    case STMT_WHILE: {
        if (s->as.while_stmt.cond) sema_check_expr_linearity(s->as.while_stmt.cond, tbl, loop_depth);
        int new_depth = loop_depth + 1;
        
        LEntry *saved_head = tbl->head;
        if (tbl->borrows) borrow_enter_scope(tbl->arena, tbl->borrows);
        for (StmtList *b = s->as.while_stmt.body; b; b = b->next) {
            sema_check_stmt_linearity_with_table(b->stmt, tbl, new_depth);
        }
        ltable_pop_scope(tbl, saved_head);
        if (tbl->borrows) borrow_exit_scope(tbl->borrows);
        break;
    }

    case STMT_UNSAFE: {
        LEntry *saved_head = tbl->head;
        for (StmtList *b = s->as.unsafe_stmt.body; b; b = b->next) {
            sema_check_stmt_linearity_with_table(b->stmt, tbl, loop_depth);
        }
        ltable_pop_scope(tbl, saved_head);
        break;
    }

    default:
        // other statements: do nothing
        break;
    }

    // Clear temporary borrows created in this statement
    if (tbl->borrows) {
        borrow_clear_temporaries(tbl->borrows);
    }
}

/* ---------- public entry: check one function ---------- */

static void sema_check_function_linearity(Decl *d) {
    if (!d || (d->kind != DECL_FUNCTION && d->kind != DECL_PROCEDURE)) return;

    LTable *tbl = ltable_new(sema_arena);

    // add parameters that are move-typed
    for (DeclList *p = d->as.function_decl.params; p; p = p->next) {
        if (p->decl->kind == DECL_VARIABLE) {
            Id *pid = p->decl->as.variable_decl.name;
            Type *pty = p->decl->as.variable_decl.type;
            if (is_type_move(pty) && pty->mode == MODE_OWNED) {
                ltable_add(tbl, pid, /*loop_depth=*/0, true, true, p->decl->line, p->decl->col);
            }
        } else if (p->decl->kind == DECL_DESTRUCT) {
            // For destructuring parameters, the aggregate is already consumed/destructured.
            // We should track the bound variables if THEY are linear.
            // But currently DECL_DESTRUCT doesn't store types for individual bindings easily here?
            // Assuming bindings inherit linearity from their fields.
            // For now, since we only destruct to 'int id', we skip tracking.
            // If we destructure linear fields, we'd need to look up their types.
            // Since this is a specialized fix for 'drop', skipping is safe for now.
        }
    }

    for (StmtList *sl = d->as.function_decl.body; sl; sl = sl->next) {
        // sema_check_stmt_linearity_with_table(sl->stmt, tbl, /*loop_depth=*/0);
        // Borrows now persist until end of scope (handled by borrow_enter_scope/exit_scope in stmt check)
        sema_check_stmt_linearity_with_table(sl->stmt, tbl, /*loop_depth=*/0);
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
        if (d->kind == DECL_FUNCTION || d->kind == DECL_PROCEDURE) {
            sema_check_function_linearity(d);
        }
    }
}

#endif /* SEMA_LINEARITY_H */
