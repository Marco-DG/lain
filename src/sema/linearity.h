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
#include "use_analysis.h"  // NLL last-use pre-pass
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

// Phase 5: per-field linearity tracking
typedef struct FieldState {
    Id *field_name;      // name of the struct field
    bool is_consumed;    // true if this specific field was consumed via mov
    struct FieldState *next;
} FieldState;

typedef struct LEntry {
    Id *id;            // Id pointer for the variable (identifies it)
    int defined_loop_depth; // loop depth where var was declared
    Region *region;    // region where var is defined (for borrow checking)
    bool is_mutable;   // true if variable is mutable (declared with var/mut)
    bool must_consume; // true if type is strictly linear
    bool is_initialized; // true if the variable has been definitely initialized
    bool is_defer_consumed; // Phase 4: true if consumed inside a defer block
    Type *var_type;    // Phase 5: the type of the variable (for field resolution)
    FieldState *field_states; // Phase 5: per-field consumption (NULL = whole-var tracking)
    isize line;        // line where var is defined
    isize col;         // col where var is defined
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

static void ltable_add(LTable *t, Id *id, int loop_depth, bool is_mutable, bool must_consume, bool is_initialized, isize line, isize col) {
    if (!id) return;
    if (ltable_find(t, id)) return; // already present — ignore
    LEntry *e = arena_push_aligned(t->arena, LEntry);
    e->id = id;
    e->defined_loop_depth = loop_depth;
    e->region = t->borrows ? t->borrows->current_region : NULL;
    e->is_mutable = is_mutable;
    e->must_consume = must_consume;
    e->is_initialized = is_initialized;
    e->is_defer_consumed = false;
    e->var_type = NULL;
    e->field_states = NULL;
    e->line = line;
    e->col = col;
    e->state = LSTATE_UNCONSUMED;
    e->next = t->head;
    t->head = e;
    DBG("ltable_add: added '%.*s' loop_depth=%d region=%d must_consume=%d", 
        (int)id->length, id->name ? id->name : "<null>", loop_depth,
        e->region ? e->region->id : -1, must_consume);
}

// Phase 5: Initialize field-level tracking for a struct variable with linear fields
static void ltable_init_field_states(LEntry *e, Type *ty, Arena *arena) {
    if (!e || !ty || !arena) return;
    if (ty->kind != TYPE_SIMPLE || !ty->base_type) return;
    e->var_type = ty;
    
    extern Symbol *sema_lookup(const char *name);
    char buf[256];
    if (ty->base_type->length >= sizeof(buf)) return;
    memcpy(buf, ty->base_type->name, ty->base_type->length);
    buf[ty->base_type->length] = '\0';
    
    Symbol *sym = sema_lookup(buf);
    if (!sym || !sym->decl || sym->decl->kind != DECL_STRUCT) return;
    
    // Count linear fields; only enable field tracking if there are any
    int linear_count = 0;
    for (DeclList *f = sym->decl->as.struct_decl.fields; f; f = f->next) {
        if (f->decl->kind == DECL_VARIABLE && sema_type_is_linear(f->decl->as.variable_decl.type))
            linear_count++;
    }
    if (linear_count == 0) return;
    
    // Create FieldState entries for each linear field
    for (DeclList *f = sym->decl->as.struct_decl.fields; f; f = f->next) {
        if (f->decl->kind != DECL_VARIABLE) continue;
        if (!sema_type_is_linear(f->decl->as.variable_decl.type)) continue;
        FieldState *fs = arena_push_aligned(arena, FieldState);
        fs->field_name = f->decl->as.variable_decl.name;
        fs->is_consumed = false;
        fs->next = e->field_states;
        e->field_states = fs;
    }
    DBG("ltable_init_field_states: '%.*s' has %d linear fields",
        (int)e->id->length, e->id->name, linear_count);
}

// Phase 5: Try to consume a specific field of a variable. Returns true if handled.
static bool ltable_consume_field(LTable *t, Id *var_id, Id *field_id, int current_loop_depth) {
    LEntry *e = ltable_find(t, var_id);
    if (!e || !e->field_states) return false; // no field tracking → fall through to whole-var
    
    // Find the field
    for (FieldState *fs = e->field_states; fs; fs = fs->next) {
        if (fs->field_name->length == field_id->length &&
            strncmp(fs->field_name->name, field_id->name, field_id->length) == 0) {
            if (fs->is_consumed) {
                fprintf(stderr, "Error Ln %li, Col %li: field '%.*s' of '%.*s' was already consumed.\n",
                        (long)e->line, (long)e->col,
                        (int)field_id->length, field_id->name,
                        (int)var_id->length, var_id->name);
                exit(1);
            }
            if (e->defined_loop_depth != current_loop_depth) {
                fprintf(stderr, "Error Ln %li, Col %li: attempting to consume field '%.*s' of '%.*s' inside a loop.\n",
                        (long)e->line, (long)e->col,
                        (int)field_id->length, field_id->name,
                        (int)var_id->length, var_id->name);
                exit(1);
            }
            fs->is_consumed = true;
            DBG("ltable_consume_field: consumed '%.*s.%.*s'",
                (int)var_id->length, var_id->name,
                (int)field_id->length, field_id->name);
            
            // Check if ALL linear fields are now consumed → auto-complete whole var
            bool all_consumed = true;
            for (FieldState *check = e->field_states; check; check = check->next) {
                if (!check->is_consumed) { all_consumed = false; break; }
            }
            if (all_consumed) {
                e->state = LSTATE_CONSUMED;
                DBG("ltable_consume_field: all fields of '%.*s' consumed → whole var consumed",
                    (int)var_id->length, var_id->name);
            }
            return true;
        }
    }
    // Field not tracked (non-linear field) → fall through
    return false;
}

// Phase 5: Check if a variable is partially consumed (some fields consumed, not all)
static bool ltable_is_partially_consumed(LEntry *e) {
    if (!e || !e->field_states) return false;
    bool has_consumed = false, has_unconsumed = false;
    for (FieldState *fs = e->field_states; fs; fs = fs->next) {
        if (fs->is_consumed) has_consumed = true;
        else has_unconsumed = true;
    }
    return has_consumed && has_unconsumed;
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
        ltable_add(dst, s->id, s->defined_loop_depth, s->is_mutable, s->must_consume, s->is_initialized, s->line, s->col);
        LEntry *d = ltable_find(dst, s->id);
        if (d) {
            d->state = s->state;
            d->region = s->region;
            d->is_initialized = s->is_initialized;
            d->is_defer_consumed = s->is_defer_consumed;
            d->var_type = s->var_type;
            
            // Deep copy field_states
            if (s->field_states) {
                FieldState *last_fs = NULL;
                d->field_states = NULL;
                for (FieldState *sf = s->field_states; sf; sf = sf->next) {
                    FieldState *df = arena_push_aligned(dst->arena, FieldState);
                    df->field_name = sf->field_name;
                    df->is_consumed = sf->is_consumed;
                    df->next = NULL;
                    if (last_fs) last_fs->next = df;
                    else d->field_states = df;
                    last_fs = df;
                }
            }
        }
    }
    return dst;
}

/* update state strictly */
static void __attribute__((unused)) ltable_set_state(LTable *t, Id *id, LState st) {
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
        fprintf(stderr, "  --> declared at Ln %li, Col %li\n", (long)e->line, (long)e->col);
        exit(1);
    }
    if (e->defined_loop_depth != current_loop_depth) {
        fprintf(stderr, "Error Ln %li, Col %li: attempting to consume linear variable '%.*s' defined outside a loop from inside a loop.\n",
                (long)e->line, (long)e->col, (int)e->id->length, e->id->name ? e->id->name : "<unknown>");
        fprintf(stderr, "  --> declared at Ln %li, Col %li (loop depth %d, current %d)\n",
                (long)e->line, (long)e->col, e->defined_loop_depth, current_loop_depth);
        exit(1);
    }
    // Phase 5: whole-var consume should also mark all fields consumed
    if (e->field_states) {
        for (FieldState *fs = e->field_states; fs; fs = fs->next) {
            fs->is_consumed = true;
        }
    }
    e->state = LSTATE_CONSUMED;
    DBG("ltable_consume: consumed '%.*s' at loop_depth=%d", (int)e->id->length, e->id->name ? e->id->name : "<unknown>", current_loop_depth);
}

/* check all vars in table are consumed */
static void ltable_ensure_all_consumed(LTable *t) {
    int errors = 0;
    for (LEntry *e = t->head; e; e = e->next) {
        // Phase 4: defer-consumed counts as consumed
        if (e->must_consume && e->state != LSTATE_CONSUMED && !e->is_defer_consumed) {
            // Phase 5: check field-level consumption
            if (e->field_states) {
                for (FieldState *fs = e->field_states; fs; fs = fs->next) {
                    if (!fs->is_consumed) {
                        fprintf(stderr, "Error Ln %li, Col %li: linear field '%.*s' of '%.*s' was not consumed before return.\n",
                                (long)e->line, (long)e->col,
                                (int)fs->field_name->length, fs->field_name->name,
                                (int)e->id->length, e->id->name ? e->id->name : "<unknown>");
                        fprintf(stderr, "  --> '%.*s' declared at Ln %li, Col %li\n",
                                (int)e->id->length, e->id->name ? e->id->name : "<unknown>",
                                (long)e->line, (long)e->col);
                        fprintf(stderr, "  help: consume with `mov %.*s.%.*s` or add `defer { drop(mov %.*s) }`\n",
                                (int)e->id->length, e->id->name,
                                (int)fs->field_name->length, fs->field_name->name,
                                (int)e->id->length, e->id->name);
                        errors++;
                    }
                }
            } else {
                fprintf(stderr, "Error Ln %li, Col %li: linear variable '%.*s' was not consumed before return.\n",
                        (long)e->line, (long)e->col, (int)e->id->length, e->id->name ? e->id->name : "<unknown>");
                fprintf(stderr, "  --> declared at Ln %li, Col %li\n", (long)e->line, (long)e->col);
                fprintf(stderr, "  help: consume with `mov %.*s` or add `defer { drop(mov %.*s) }`\n",
                        (int)e->id->length, e->id->name, (int)e->id->length, e->id->name);
                errors++;
            }
        }
    }
    
    if (errors > 0) {
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
        // Phase 4: defer-consumed counts as consumed
        if (e->must_consume && e->state != LSTATE_CONSUMED && !e->is_defer_consumed) {
            // Phase 5: check field-level consumption
            if (e->field_states) {
                for (FieldState *fs = e->field_states; fs; fs = fs->next) {
                    if (!fs->is_consumed) {
                        fprintf(stderr, "Error Ln %li, Col %li: linear field '%.*s' of '%.*s' was not consumed before end of scope.\n",
                                (long)e->line, (long)e->col,
                                (int)fs->field_name->length, fs->field_name->name,
                                (int)e->id->length, e->id->name ? e->id->name : "<unknown>");
                        errors++;
                    }
                }
            } else {
                fprintf(stderr, "Error Ln %li, Col %li: linear variable '%.*s' was not consumed before end of scope.\n",
                        (long)e->line, (long)e->col, (int)e->id->length, e->id->name ? e->id->name : "<unknown>");
                errors++;
            }
        }
        // Release persistent borrows held by variables leaving scope (NLL)
        if (tbl->borrows && e->id) {
            borrow_release_by_binding(tbl->borrows, e->id);
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
        // Phase 5: check field-level consistency
        if (ea && eb && ea->field_states && eb->field_states) {
            for (FieldState *fa = ea->field_states, *fb = eb->field_states; fa && fb; fa = fa->next, fb = fb->next) {
                if (fa->is_consumed != fb->is_consumed) {
                    fprintf(stderr, "sema error: linear field '%.*s' of '%.*s' is consumed inconsistently in the branches of %s\n",
                            (int)fa->field_name->length, fa->field_name->name,
                            (int)p->id->length, p->id->name ? p->id->name : "<unknown>",
                            stmt_name ? stmt_name : "if");
                    exit(1);
                }
            }
        }
    }
}

/* merge branch result back into parent */
static void ltable_merge_from_branch(LTable *parent, LTable *branch) {
    for (LEntry *p = parent->head; p; p = p->next) {
        LEntry *b = ltable_find(branch, p->id);
        if (b) {
            p->state = b->state;
            p->is_defer_consumed = b->is_defer_consumed;
            // Phase 5: merge field states
            if (b->field_states && p->field_states) {
                for (FieldState *pf = p->field_states, *bf = b->field_states; pf && bf; pf = pf->next, bf = bf->next) {
                    pf->is_consumed = bf->is_consumed;
                }
            }
        }
    }
}

/* intersect initialized state from two branches (for IF) */
static void ltable_intersect_initialization(LTable *parent, LTable *a, LTable *b) {
    for (LEntry *p = parent->head; p; p = p->next) {
        LEntry *ea = ltable_find(a, p->id);
        LEntry *eb = ltable_find(b, p->id);
        bool init_a = ea ? ea->is_initialized : false;
        bool init_b = eb ? eb->is_initialized : false;
        p->is_initialized = p->is_initialized || (init_a && init_b);
    }
}

/* apply initialization from a branch back to parent */
static void ltable_apply_initialization(LTable *parent, LTable *branch) {
    for (LEntry *p = parent->head; p; p = p->next) {
        LEntry *b = ltable_find(branch, p->id);
        if (b) p->is_initialized = p->is_initialized || b->is_initialized;
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

static void sema_check_stmt_linearity_with_table(Stmt *s, LTable *tbl, int loop_depth, UseTable *use_tbl);

static void sema_check_expr_linearity(Expr *e, LTable *tbl, int loop_depth) {
    if (!e) return;
    switch (e->kind) {
    case EXPR_IDENTIFIER: {
        // Check if this is a use of an already-consumed linear variable
        Id *id = e->as.identifier_expr.id;
        
        if (id && tbl) {
            LEntry *entry = ltable_find(tbl, id);
            if (entry) {
                if (entry->state == LSTATE_CONSUMED) {
                    fprintf(stderr, "Error Ln %li, Col %li: use of linear variable '%.*s' after it was moved.\n",
                            (long)(e->line), (long)(e->col), (int)id->length, id->name ? id->name : "<unknown>");
                    exit(1);
                }
                if (!entry->is_initialized) {
                    fprintf(stderr, "Error Ln %li, Col %li: use of uninitialized variable '%.*s'.\n",
                            (long)(e->line), (long)(e->col), (int)id->length, id->name ? id->name : "<unknown>");
                    exit(1);
                }
            }
            // NLL: Check if this variable is persistently borrowed (read access)
            if (tbl->borrows) {
                if (borrow_check_owner_access(tbl->borrows, id, MODE_SHARED, e->line, e->col)) {
                    exit(1);
                }
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
            if (callee && callee->decl) {
                is_struct_constructor = (callee->decl->kind == DECL_STRUCT || callee->decl->kind == DECL_ENUM);
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

    case EXPR_MATCH: {
        if (e->as.match_expr.value) {
            sema_check_expr_linearity(e->as.match_expr.value, tbl, loop_depth);
            if (tbl->borrows) borrow_clear_temporaries(tbl->borrows);
        }
        LTable *parent_snapshot = ltable_clone(tbl);
        LTable *first_branch = NULL;
        for (ExprMatchCase *c = e->as.match_expr.cases; c; c = c->next) {
            LTable *branch_tbl = ltable_clone(parent_snapshot);
            
            for (ExprList *p = c->patterns; p; p = p->next) {
                sema_check_expr_linearity(p->expr, branch_tbl, loop_depth);
            }
            sema_check_expr_linearity(c->body, branch_tbl, loop_depth);
            
            if (!first_branch) {
                first_branch = ltable_clone(branch_tbl);
            } else {
                ltable_check_branch_consistency(parent_snapshot, first_branch, branch_tbl, "match expr");
                ltable_intersect_initialization(first_branch, first_branch, branch_tbl);
            }
            ltable_free(branch_tbl);
        }
        if (first_branch) {
            ltable_merge_from_branch(tbl, first_branch);
            ltable_apply_initialization(tbl, first_branch);
            ltable_free(first_branch);
        }
        ltable_free(parent_snapshot);
        break;
    }

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
                // Phase 5: check if trying to whole-move a partially consumed var
                LEntry *entry = ltable_find(tbl, idptr);
                if (entry && ltable_is_partially_consumed(entry)) {
                    fprintf(stderr, "Error Ln %li, Col %li: cannot move '%.*s' because some fields have already been consumed.\n",
                            (long)e->line, (long)e->col, (int)idptr->length, idptr->name);
                    exit(1);
                }
                DBG("EXPR_MOVE: consume IDENT '%.*s'", (int)idptr->length, idptr->name ? idptr->name : "<null>");
                ltable_consume(tbl, idptr, loop_depth);
            } else if (inner->kind == EXPR_MEMBER) {
                // Phase 5/9.2: try field-level consumption — supports multi-level paths
                // Walk up the member chain to find the root identifier and leaf field
                Id *leaf_field = inner->as.member_expr.member;
                
                // Walk to find root identifier
                Expr *root = inner->as.member_expr.target;
                while (root && root->kind == EXPR_MEMBER) {
                    root = root->as.member_expr.target;
                }
                
                if (root && root->kind == EXPR_IDENTIFIER && leaf_field) {
                    Id *var_id = root->as.identifier_expr.id;
                    if (ltable_consume_field(tbl, var_id, leaf_field, loop_depth)) {
                        DBG("EXPR_MOVE: consumed field '%.*s...%.*s'",
                            (int)var_id->length, var_id->name,
                            (int)leaf_field->length, leaf_field->name);
                        break; // handled at field level
                    }
                }
                // Fall through: consume whole variable (no field tracking or non-linear field)
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

static void sema_check_stmt_linearity_with_table(Stmt *s, LTable *tbl, int loop_depth, UseTable *use_tbl) {
    if (!s) return;
    switch (s->kind) {

    case STMT_VAR: {
        Expr *init = s->as.var_stmt.expr;
        if (init) sema_check_expr_linearity(init, tbl, loop_depth);

        Type *ty = s->as.var_stmt.type;
        bool must = is_type_move(ty);
        if (must || s->as.var_stmt.is_mutable) {
            Id *id = s->as.var_stmt.name;
            bool is_init = (init != NULL && init->kind != EXPR_UNDEFINED);
            ltable_add(tbl, id, loop_depth, s->as.var_stmt.is_mutable, must, is_init, s->line, s->col);
            // Phase 5: init field-level tracking if struct with linear fields
            if (must && ty) {
                LEntry *entry = ltable_find(tbl, id);
                if (entry) ltable_init_field_states(entry, ty, tbl->arena);
            }
        }
        
        // NLL Phase 2: Register persistent borrow when init is a call
        // returning a reference (MODE_MUTABLE return type).
        // Pattern: var ref = func(var data)  where func returns var T
        // Clear temporary borrows first — the call already registered them,
        // and we're upgrading the relevant one to a persistent borrow.
        if (tbl->borrows) borrow_clear_temporaries(tbl->borrows);
        if (init && init->kind == EXPR_CALL && tbl->borrows && tbl->arena) {
            Id *binding_id = s->as.var_stmt.name;
            Expr *callee = init->as.call_expr.callee;
            Decl *fn_decl = NULL;
            
            if (callee && callee->kind == EXPR_IDENTIFIER) {
                char mangled_buf[256];
                Id *callee_id = callee->as.identifier_expr.id;
                int len = callee_id->length < 255 ? callee_id->length : 255;
                memcpy(mangled_buf, callee_id->name, len);
                mangled_buf[len] = '\0';
                fn_decl = find_function_decl_by_mangled_or_raw(mangled_buf);
            }
            
            // Check if function returns a reference (var T = MODE_MUTABLE on return type)
            if (fn_decl) {
                Type *ret_type = fn_decl->as.function_decl.return_type;
                if (ret_type && ret_type->mode == MODE_MUTABLE) {
                    // Find the borrowed owner: look for var-parameter args
                    DeclList *params = fn_decl->as.function_decl.params;
                    ExprList *args = init->as.call_expr.args;
                    for (; params && args; params = params->next, args = args->next) {
                        Type *pty = params->decl->as.variable_decl.type;
                        Expr *arg = args->expr;
                        if (!arg || !pty) continue;
                        
                        // Look for mutable (var) parameters — the borrow source
                        if (pty->mode == MODE_MUTABLE) {
                            Id *owner_id = NULL;
                            Expr *inner = arg;
                            if (inner->kind == EXPR_MUT) inner = inner->as.mut_expr.expr;
                            if (inner && inner->kind == EXPR_IDENTIFIER) {
                                owner_id = inner->as.identifier_expr.id;
                            } else if (inner && inner->kind == EXPR_MEMBER) {
                                Expr *head = inner->as.member_expr.target;
                                while (head && head->kind == EXPR_MEMBER) head = head->as.member_expr.target;
                                if (head && head->kind == EXPR_IDENTIFIER) owner_id = head->as.identifier_expr.id;
                            }
                            
                            if (owner_id) {
                                LEntry *owner_entry = ltable_find(tbl, owner_id);
                                Region *owner_region = owner_entry ? owner_entry->region 
                                    : tbl->borrows->current_region;
                                int lu = use_tbl ? use_table_get_last_use(use_tbl, binding_id) : -1;
                                borrow_register_persistent(tbl->arena, tbl->borrows, 
                                    binding_id, owner_id, MODE_MUTABLE, owner_region, lu, NULL);
                                DBG("STMT_VAR NLL: registered persistent mutable borrow of '%.*s' by '%.*s' (last_use=%d)",
                                    (int)owner_id->length, owner_id->name,
                                    (int)binding_id->length, binding_id->name, lu);
                            }
                        }
                    }
                }
            }
        }
        
        // Phase 4 Feature 1: Direct var-expression persistent borrow.
        // Pattern: var ref = var data.field  (EXPR_MUT initializer)
        // This creates a persistent borrow of the owner without going through a function call.
        if (init && init->kind == EXPR_MUT && tbl->borrows && tbl->arena) {
            Id *binding_id = s->as.var_stmt.name;
            Expr *inner = init->as.mut_expr.expr;
            Id *owner_id = NULL;
            
            // Extract owner from inner expression
            if (inner && inner->kind == EXPR_IDENTIFIER) {
                owner_id = inner->as.identifier_expr.id;
            } else if (inner && inner->kind == EXPR_MEMBER) {
                Expr *head = inner->as.member_expr.target;
                while (head && head->kind == EXPR_MEMBER) head = head->as.member_expr.target;
                if (head && head->kind == EXPR_IDENTIFIER) owner_id = head->as.identifier_expr.id;
            }
            
            if (owner_id) {
                LEntry *owner_entry = ltable_find(tbl, owner_id);
                Region *owner_region = owner_entry ? owner_entry->region
                    : tbl->borrows->current_region;
                int lu = use_tbl ? use_table_get_last_use(use_tbl, binding_id) : -1;
                borrow_register_persistent(tbl->arena, tbl->borrows,
                    binding_id, owner_id, MODE_MUTABLE, owner_region, lu, NULL);
                DBG("STMT_VAR Phase 4: registered persistent mutable borrow of '%.*s' by '%.*s' via direct var-expr (last_use=%d)",
                    (int)owner_id->length, owner_id->name,
                    (int)binding_id->length, binding_id->name, lu);
            }
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
                    bool must = is_type_move(decl_ty);
                    ltable_add(tbl, id, loop_depth, true, must, true, s->line, s->col);
                }
            }
        } else {
            // Phase 3: Check if writing to a persistently-borrowed owner
            // This covers both member/index paths AND direct identifier assignment
            Id *base_id = NULL;
            if (lhs && lhs->kind == EXPR_IDENTIFIER) {
                // Direct assignment: data = new_data
                base_id = lhs->as.identifier_expr.id;
            } else {
                // Member/Index path: data.value = 99 or data[i] = 99
                Expr *base = lhs;
                while (base && (base->kind == EXPR_INDEX || base->kind == EXPR_MEMBER)) {
                    if (base->kind == EXPR_INDEX) base = base->as.index_expr.target;
                    else if (base->kind == EXPR_MEMBER) base = base->as.member_expr.target;
                }
                if (base && base->kind == EXPR_IDENTIFIER) {
                    base_id = base->as.identifier_expr.id;
                }
            }
            if (base_id) {
                // Check for write conflict with persistent borrows
                if (tbl->borrows) {
                    if (borrow_check_owner_access(tbl->borrows, base_id, MODE_MUTABLE, s->line, s->col)) {
                        exit(1);
                    }
                }
                
                LEntry *entry = ltable_find(tbl, base_id);
                if (entry) {
                    entry->is_initialized = true;
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
        if (cond) {
            sema_check_expr_linearity(cond, tbl, loop_depth);
            if (tbl->borrows) borrow_clear_temporaries(tbl->borrows);
        }

        LTable *parent_snapshot = ltable_clone(tbl);

        LTable *then_tbl = ltable_clone(parent_snapshot);
        LEntry *then_saved = then_tbl->head;
        if (then_tbl->borrows) borrow_enter_scope(then_tbl->arena, then_tbl->borrows);
        for (StmtList *b = s->as.if_stmt.then_branch; b; b = b->next) {
            sema_check_stmt_linearity_with_table(b->stmt, then_tbl, loop_depth, use_tbl);
        }
        ltable_pop_scope(then_tbl, then_saved);

        LTable *else_tbl = ltable_clone(parent_snapshot);
        LEntry *else_saved = else_tbl->head;
        if (else_tbl->borrows) borrow_enter_scope(else_tbl->arena, else_tbl->borrows);
        for (StmtList *b = s->as.if_stmt.else_branch; b; b = b->next) {
            sema_check_stmt_linearity_with_table(b->stmt, else_tbl, loop_depth, use_tbl);
        }
        ltable_pop_scope(else_tbl, else_saved);

        ltable_check_branch_consistency(parent_snapshot, then_tbl, else_tbl, "if");
        ltable_merge_from_branch(tbl, then_tbl);
        ltable_intersect_initialization(tbl, then_tbl, else_tbl);

        ltable_free(parent_snapshot);
        ltable_free(then_tbl);
        ltable_free(else_tbl);
        break;
    }

    case STMT_FOR: {
        if (s->as.for_stmt.iterable) {
            sema_check_expr_linearity(s->as.for_stmt.iterable, tbl, loop_depth);
            if (tbl->borrows) borrow_clear_temporaries(tbl->borrows);
        }
        int new_depth = loop_depth + 1;
        
        LTable *pre_loop = ltable_clone(tbl);
        
        LEntry *saved_head = tbl->head;
        if (tbl->borrows) borrow_enter_scope(tbl->arena, tbl->borrows);
        for (StmtList *b = s->as.for_stmt.body; b; b = b->next) {
            sema_check_stmt_linearity_with_table(b->stmt, tbl, new_depth, use_tbl);
        }
        ltable_pop_scope(tbl, saved_head);
        if (tbl->borrows) borrow_exit_scope(tbl->borrows);
        
        // Loop might run zero times, so restore initialization state from before the loop
        for (LEntry *p = tbl->head; p; p = p->next) {
            LEntry *pre = ltable_find(pre_loop, p->id);
            if (pre) p->is_initialized = pre->is_initialized;
        }
        ltable_free(pre_loop);
        
        break;
    }

    case STMT_RETURN: {
        Expr *val = s->as.return_stmt.value;
        if (val) sema_check_expr_linearity(val, tbl, loop_depth);
        
        // Check for dangling `return var local` — returning a mutable
        // reference to a local variable is always a dangling pointer.
        // Recursively unwrap EXPR_MEMBER and EXPR_INDEX to find the root identifier.
        if (val && val->kind == EXPR_MUT) {
            Expr *inner = val->as.mut_expr.expr;
            // Recursively walk through member/index to find root
            Expr *root = inner;
            while (root) {
                if (root->kind == EXPR_MEMBER) root = root->as.member_expr.target;
                else if (root->kind == EXPR_INDEX) root = root->as.index_expr.target;
                else break;
            }
            if (root && root->kind == EXPR_IDENTIFIER) {
                Decl *d = root->decl;
                if (d && d->kind == DECL_VARIABLE && !d->as.variable_decl.is_parameter) {
                    fprintf(stderr, "Error Ln %li, Col %li: cannot return mutable reference to local variable '%.*s'. "
                            "Local variables are deallocated when the function returns.\n",
                            (long)s->line, (long)s->col,
                            (int)root->as.identifier_expr.id->length,
                            root->as.identifier_expr.id->name);
                    exit(1);
                }
            }
        }
        
        ltable_ensure_all_consumed(tbl);
        break;
    }

    case STMT_MATCH: {
        if (s->as.match_stmt.value) {
            sema_check_expr_linearity(s->as.match_stmt.value, tbl, loop_depth);
            if (tbl->borrows) borrow_clear_temporaries(tbl->borrows);
        }
        LTable *parent_snapshot = ltable_clone(tbl);
        LTable *first_branch = NULL;
        for (StmtMatchCase *c = s->as.match_stmt.cases; c; c = c->next) {
            LTable *branch_tbl = ltable_clone(parent_snapshot);
            LEntry *saved_head = branch_tbl->head;
            if (branch_tbl->borrows) borrow_enter_scope(branch_tbl->arena, branch_tbl->borrows);
            
            for (ExprList *p = c->patterns; p; p = p->next) {
                sema_check_expr_linearity(p->expr, branch_tbl, loop_depth);
            }
            for (StmtList *b = c->body; b; b = b->next) {
                sema_check_stmt_linearity_with_table(b->stmt, branch_tbl, loop_depth, use_tbl);
            }
            ltable_pop_scope(branch_tbl, saved_head);

            if (!first_branch) {
                first_branch = ltable_clone(branch_tbl);
            } else {
                ltable_check_branch_consistency(parent_snapshot, first_branch, branch_tbl, "match");
                ltable_intersect_initialization(first_branch, first_branch, branch_tbl);
            }
            ltable_free(branch_tbl);
        }
        if (first_branch) {
            ltable_merge_from_branch(tbl, first_branch);
            ltable_apply_initialization(tbl, first_branch);
            ltable_free(first_branch);
        }
        ltable_free(parent_snapshot);
        break;
    }

    case STMT_WHILE: {
        if (s->as.while_stmt.cond) {
            sema_check_expr_linearity(s->as.while_stmt.cond, tbl, loop_depth);
            if (tbl->borrows) borrow_clear_temporaries(tbl->borrows);
        }
        int new_depth = loop_depth + 1;
        
        LTable *pre_loop = ltable_clone(tbl);
        
        LEntry *saved_head = tbl->head;
        if (tbl->borrows) borrow_enter_scope(tbl->arena, tbl->borrows);
        for (StmtList *b = s->as.while_stmt.body; b; b = b->next) {
            sema_check_stmt_linearity_with_table(b->stmt, tbl, new_depth, use_tbl);
        }
        ltable_pop_scope(tbl, saved_head);
        if (tbl->borrows) borrow_exit_scope(tbl->borrows);
        
        // Loop might run zero times, so restore initialization state from before the loop
        for (LEntry *p = tbl->head; p; p = p->next) {
            LEntry *pre = ltable_find(pre_loop, p->id);
            if (pre) p->is_initialized = pre->is_initialized;
        }
        ltable_free(pre_loop);
        
        break;
    }

    case STMT_UNSAFE: {
        LEntry *saved_head = tbl->head;
        for (StmtList *b = s->as.unsafe_stmt.body; b; b = b->next) {
            sema_check_stmt_linearity_with_table(b->stmt, tbl, loop_depth, use_tbl);
        }
        ltable_pop_scope(tbl, saved_head);
        break;
    }

    case STMT_DEFER: {
        // Phase 4 Feature 3: Defer consumption tracking.
        // Walk the defer body to detect mov of linear variables.
        // Instead of actually consuming them now (which would cause false
        // "use after move" errors for uses after the defer declaration),
        // mark them as is_defer_consumed.
        // 
        // Strategy: save states, run the walker (which will consume vars),
        // then detect which vars changed state and mark them as defer-consumed
        // while restoring the original state.
        
        // Save state of all tracked variables before walking defer body
        typedef struct DeferSave { Id *id; LState state; bool must_consume; struct DeferSave *next; } DeferSave;
        DeferSave *saves = NULL;
        for (LEntry *e = tbl->head; e; e = e->next) {
            DeferSave *sv = arena_push_aligned(tbl->arena, DeferSave);
            sv->id = e->id;
            sv->state = e->state;
            sv->must_consume = e->must_consume;
            sv->next = saves;
            saves = sv;
        }
        
        // Walk the defer body — this will consume variables via EXPR_MOVE
        sema_check_stmt_linearity_with_table(s->as.defer_stmt.stmt, tbl, loop_depth, use_tbl);
        
        // Detect state changes: any variable that was UNCONSUMED before and
        // is now CONSUMED was consumed inside the defer → mark as defer-consumed
        // and restore to UNCONSUMED so it can still be used after defer declaration.
        for (DeferSave *sv = saves; sv; sv = sv->next) {
            LEntry *e = ltable_find(tbl, sv->id);
            if (e && sv->state == LSTATE_UNCONSUMED && e->state == LSTATE_CONSUMED) {
                e->is_defer_consumed = true;
                e->state = LSTATE_UNCONSUMED; // Restore: var is still usable
                DBG("STMT_DEFER Phase 4: marked '%.*s' as defer-consumed (restored to UNCONSUMED)",
                    (int)e->id->length, e->id->name);
            }
        }
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

    // NLL: Compute last-use statement indices for all identifiers in the function body
    UseTable *use_tbl = use_compute_last_uses(d->as.function_decl.body, sema_arena);

    // add parameters that are move-typed
    for (DeclList *p = d->as.function_decl.params; p; p = p->next) {
        if (p->decl->kind == DECL_VARIABLE) {
            Id *pid = p->decl->as.variable_decl.name;
            Type *pty = p->decl->as.variable_decl.type;
            if (is_type_move(pty) && pty->mode == MODE_OWNED) {
                // parameters are assumed definitely initialized
                ltable_add(tbl, pid, /*loop_depth=*/0, true, true, true, p->decl->line, p->decl->col);
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

    int stmt_counter = 0;
    for (StmtList *sl = d->as.function_decl.body; sl; sl = sl->next) {
        sema_check_stmt_linearity_with_table(sl->stmt, tbl, /*loop_depth=*/0, use_tbl);
        
        // NLL: After each top-level statement, release persistent borrows
        // whose binding has no more uses beyond this statement index.
        if (tbl->borrows) {
            borrow_release_expired(tbl->borrows, stmt_counter);
        }
        stmt_counter++;
    }

    // final check (if function falls off end)
    ltable_ensure_all_consumed(tbl);

    ltable_free(tbl);
}

/* ---------- module-level entry: run linearity check over all functions ---------- */

static void __attribute__((unused)) sema_check_module_linearity(DeclList *decls) {
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
