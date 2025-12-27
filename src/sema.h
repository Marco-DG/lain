#ifndef SEMA_H
#define SEMA_H

#include "sema/scope.h"
#include "sema/resolve.h"
#include "sema/typecheck.h"
#include "sema/linearity.h"

Type *current_return_type = NULL;
Decl *current_function_decl = NULL; // New: track current function for purity checks
const char *current_module_path = NULL;
DeclList *sema_decls = NULL;
Arena *sema_arena = NULL;
RangeTable *sema_ranges = NULL;

/*─────────────────────────────────────────────────────────────────╗
│ Public entry: call this before emit                             │
╚─────────────────────────────────────────────────────────────────*/
static void sema_resolve_module(DeclList *decls, const char *module_path,
                                Arena *arena) {
    sema_arena = arena;
    sema_decls = decls;
    sema_ranges = range_table_new(arena);

    // 1) Clear old globals + insert top-level decls
    sema_clear_globals();
    sema_build_scope(decls, module_path);

    // 2) For each function: resolve → infer → linearity → clear locals
    for (DeclList *dl = decls; dl; dl = dl->next) {
        Decl *d = dl->decl;
        if (!d || d->kind != DECL_FUNCTION) continue;

        sema_clear_locals();

        // 2.a) Insert parameters into locals
        int param_idx = 0;
        for (DeclList *p = d->as.function_decl.params; p; p = p->next) {
            if (p->decl->kind == DECL_DESTRUCT) {
                // 1) Generate hidden name "_param_N"
                char hidden_name[32];
                snprintf(hidden_name, sizeof(hidden_name), "_param_%d", param_idx);
                
                DeclDestruct *dd = &p->decl->as.destruct_decl;

                // 2) Insert hidden parameter
                sema_insert_local(hidden_name, hidden_name, dd->type, p->decl, false);

                // 3) Resolve struct type to find fields
                Decl *struct_decl = NULL;
                // Simple linear search in sema_decls for the struct
                // (Optimization: could use a hash map, but this is fine for now)
                if (dd->type->kind == TYPE_SIMPLE) {
                    for (DeclList *g = sema_decls; g; g = g->next) {
                        if (g->decl->kind == DECL_STRUCT) {
                            Id *sname = g->decl->as.struct_decl.name;
                            if (sname->length == dd->type->base_type->length &&
                                strncmp(sname->name, dd->type->base_type->name, sname->length) == 0) {
                                struct_decl = g->decl;
                                break;
                            }
                        }
                    }
                }

                if (!struct_decl) {
                    fprintf(stderr, "Error: Could not resolve struct type for destructuring\n");
                    exit(1);
                }

                // 4) For each destructured name, find field type and insert local
                for (IdList *n = dd->names; n; n = n->next) {
                    Type *field_type = NULL;
                    for (DeclList *f = struct_decl->as.struct_decl.fields; f; f = f->next) {
                        Id *fname = f->decl->as.variable_decl.name;
                        if (fname->length == n->id->length &&
                            strncmp(fname->name, n->id->name, fname->length) == 0) {
                            field_type = f->decl->as.variable_decl.type;
                            break;
                        }
                    }

                    if (!field_type) {
                        fprintf(stderr, "Error: Field '%.*s' not found in struct '%.*s'\n", 
                                (int)n->id->length, n->id->name,
                                (int)dd->type->base_type->length, dd->type->base_type->name);
                        exit(1);
                    }

                    // Insert local variable (e.g. "text" -> u8[:0])
                    // Emit will generate "u8[:0] text = _param_N.text;"
                    char raw_field[256];
                    int L = n->id->length < (int)sizeof(raw_field) - 1 ? n->id->length : (int)sizeof(raw_field) - 1;
                    memcpy(raw_field, n->id->name, L);
                    raw_field[L] = '\0';
                    
                    sema_insert_local(raw_field, raw_field, field_type, NULL, false); // Destructured fields don't have a Decl
                }

            } else {
                Id *pid = p->decl->as.variable_decl.name;
                Type *pty = p->decl->as.variable_decl.type;

                char rawp[256];
                int L = pid->length < (int)sizeof(rawp) - 1 ? pid->length
                                                             : (int)sizeof(rawp) - 1;
                memcpy(rawp, pid->name, L);
                rawp[L] = '\0';

                sema_insert_local(rawp, rawp, pty, p->decl, false);
            }
            param_idx++;
        }

        // 2.b) Name resolution
        current_return_type = d->as.function_decl.return_type;
        current_function_decl = d; // Set current function
        current_module_path = module_path;
        for (StmtList *sl = d->as.function_decl.body; sl; sl = sl->next) {
            sema_resolve_stmt(sl->stmt);
        }

        // 2.c) Type inference
        void walk_stmt(Stmt *s) {
            if (!s) return;
            switch (s->kind) {
                case STMT_VAR:
                    sema_infer_expr(s->as.var_stmt.expr);
                    break;
                case STMT_IF: {
                    sema_infer_expr(s->as.if_stmt.cond);
                    for (StmtList *b = s->as.if_stmt.then_branch; b; b = b->next)
                        walk_stmt(b->stmt);
                    for (StmtList *b = s->as.if_stmt.else_branch; b; b = b->next)
                        walk_stmt(b->stmt);
                    break;
                }
                case STMT_FOR:
                    sema_infer_expr(s->as.for_stmt.iterable);
                    for (StmtList *b = s->as.for_stmt.body; b; b = b->next)
                        walk_stmt(b->stmt);
                    break;
                case STMT_ASSIGN:
                    sema_infer_expr(s->as.assign_stmt.expr);
                    break;
                case STMT_EXPR:
                    sema_infer_expr(s->as.expr_stmt.expr);
                    break;
                case STMT_RETURN:
                    sema_infer_expr(s->as.return_stmt.value);
                    break;
                case STMT_MATCH:
                    sema_infer_expr(s->as.match_stmt.value);
                    for (StmtMatchCase *c = s->as.match_stmt.cases; c; c = c->next) {
                        if (c->pattern) sema_infer_expr(c->pattern);
                        for (StmtList *b = c->body; b; b = b->next)
                            walk_stmt(b->stmt);
                    }
                    break;
                default: break;
            }
        }
        for (StmtList *sl = d->as.function_decl.body; sl; sl = sl->next)
            walk_stmt(sl->stmt);

        current_return_type = NULL;
        current_function_decl = NULL;
        current_module_path = NULL;

        // 2.d) Linearity check: run function-level linearity checker
        // NOTE: sema_check_function_linearity must run while sema_locals still
        // exist (so it can trust that implicit locals were created by resolve).
        sema_check_function_linearity(d);

        // 2.e) Clear locals after all passes
        sema_clear_locals();
    }
}

// Optional: destroy/reset global state
static void sema_destroy(void) {
    sema_clear_globals();
}

#endif // SEMA_H
