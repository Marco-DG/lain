#ifndef SEMA_H
#define SEMA_H

#include "sema/scope.h"
#include "sema/resolve.h"
#include "sema/typecheck.h"
#include "sema/linearity.h"

Type *current_return_type = NULL;
const char *current_module_path = NULL;
DeclList *sema_decls = NULL;
Arena *sema_arena = NULL;

/*─────────────────────────────────────────────────────────────────╗
│ Public entry: call this before emit                             │
╚─────────────────────────────────────────────────────────────────*/
static void sema_resolve_module(DeclList *decls, const char *module_path,
                                Arena *arena) {
    sema_arena = arena;
    sema_decls = decls;

    // 1) Clear old globals + insert top-level decls
    sema_clear_globals();
    sema_build_scope(decls, module_path);

    // 2) For each function: resolve → infer → linearity → clear locals
    for (DeclList *dl = decls; dl; dl = dl->next) {
        Decl *d = dl->decl;
        if (!d || d->kind != DECL_FUNCTION) continue;

        sema_clear_locals();

        // 2.a) Insert parameters into locals
        for (DeclList *p = d->as.function_decl.params; p; p = p->next) {
            Id *pid = p->decl->as.variable_decl.name;
            Type *pty = p->decl->as.variable_decl.type;

            char rawp[256];
            int L = pid->length < (int)sizeof(rawp) - 1 ? pid->length
                                                         : (int)sizeof(rawp) - 1;
            memcpy(rawp, pid->name, L);
            rawp[L] = '\0';

            sema_insert_local(rawp, rawp, pty);
        }

        // 2.b) Name resolution
        current_return_type = d->as.function_decl.return_type;
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
