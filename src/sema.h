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
bool sema_in_unsafe_block = false;

/*─────────────────────────────────────────────────────────────────╗
│ Public entry: call this before emit                             │
╚─────────────────────────────────────────────────────────────────*/

// Helper to widen variables modified in a loop to unknown
static void sema_widen_loop(StmtList *body, RangeTable *t) {
    for (StmtList *l = body; l; l = l->next) {
        Stmt *s = l->stmt;
        if (!s) continue;
        switch (s->kind) {
            case STMT_ASSIGN:
                if (s->as.assign_stmt.target->kind == EXPR_IDENTIFIER) {
                     range_set(t, s->as.assign_stmt.target->as.identifier_expr.id, range_unknown());
                }
                break;
            case STMT_IF:
                sema_widen_loop(s->as.if_stmt.then_branch, t);
                sema_widen_loop(s->as.if_stmt.else_branch, t);
                break;
            case STMT_FOR:
                sema_widen_loop(s->as.for_stmt.body, t);
                break;
            case STMT_MATCH:
                 for (StmtMatchCase *c = s->as.match_stmt.cases; c; c = c->next) {
                     sema_widen_loop(c->body, t);
                 }
                 break;
            default: break;
        }
    }
}

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
                
                // Handle 'in' constraint: param int in arr
                // Desugars to: param >= 0 and param < arr.len
                if (p->decl->as.variable_decl.in_field && sema_ranges) {
                    Id *arr_id = p->decl->as.variable_decl.in_field;
                    Id *param_id = pid;
                    
                    // Find the array parameter to get its length
                    Type *arr_type = NULL;
                    for (DeclList *arr_p = d->as.function_decl.params; arr_p; arr_p = arr_p->next) {
                        if (arr_p->decl->kind == DECL_VARIABLE) {
                            Id *aname = arr_p->decl->as.variable_decl.name;
                            if (aname->length == arr_id->length &&
                                strncmp(aname->name, arr_id->name, aname->length) == 0) {
                                arr_type = arr_p->decl->as.variable_decl.type;
                                break;
                            }
                        }
                    }
                    
                    if (arr_type) {
                        // Apply range: param >= 0
                        Range r = range_make(0, INT64_MAX);
                        
                        // If array has known length (fixed-size), tighten upper bound
                        if (arr_type->kind == TYPE_ARRAY && arr_type->array_len >= 0) {
                            r = range_make(0, arr_type->array_len - 1);
                        }
                        
                        range_set(sema_ranges, param_id, r);
                    }
                }
                
                // Apply equation-style constraints: b int != 0, x int >= 0 and <= 100
                if (p->decl->as.variable_decl.constraints && sema_ranges) {
                    for (ExprList *c = p->decl->as.variable_decl.constraints; c; c = c->next) {
                        sema_apply_constraint(c->expr, sema_ranges);
                    }
                }
            }
            param_idx++;
        }

        // 2.b) Name resolution
        current_return_type = d->as.function_decl.return_type;
        current_function_decl = d; // Set current function
        current_module_path = module_path;

        // Apply Pre-Contracts to Range Table
        if (sema_ranges) {
            for (ExprList *pre = d->as.function_decl.pre_contracts; pre; pre = pre->next) {
                sema_resolve_expr(pre->expr);
                sema_infer_expr(pre->expr);
                sema_apply_constraint(pre->expr, sema_ranges);
            }
        }

        // Resolve Post-Contracts
        // Inject 'result' variable for resolution
        if (d->as.function_decl.post_contracts) {
             // We inject "result" as a local variable so it can be resolved.
             // It will remain in the scope for the body, which is acceptable.
             // If the user shadows it, the inner "result" will be used in the body,
             // but the contracts are already resolved to this outer "result".
             sema_insert_local("result", "result", d->as.function_decl.return_type, NULL, false);
             
             for (ExprList *post = d->as.function_decl.post_contracts; post; post = post->next) {
                 sema_resolve_expr(post->expr);
                 sema_infer_expr(post->expr);
             }
        }

        for (StmtList *sl = d->as.function_decl.body; sl; sl = sl->next) {
            sema_resolve_stmt(sl->stmt);
        }

        // 2.c) Type inference
        void walk_stmt(Stmt *s) {
            if (!s) return;
            switch (s->kind) {
                case STMT_VAR:
                    sema_infer_expr(s->as.var_stmt.expr);
                    if (sema_ranges && s->as.var_stmt.expr) {
                        Range r = sema_eval_range(s->as.var_stmt.expr, sema_ranges);
                        range_set(sema_ranges, s->as.var_stmt.name, r);
                    }
                    break;
                case STMT_IF: {
                    sema_infer_expr(s->as.if_stmt.cond);
                    
                    // Save state
                    RangeEntry *old_head = sema_ranges->head;
                    ConstraintEntry *old_constraints = sema_ranges->constraints;
                    
                    // Apply condition for THEN branch
                    sema_apply_constraint(s->as.if_stmt.cond, sema_ranges);
                    
                    for (StmtList *b = s->as.if_stmt.then_branch; b; b = b->next)
                        walk_stmt(b->stmt);
                        
                    // Restore state (pop constraints from THEN)
                    sema_ranges->head = old_head;
                    sema_ranges->constraints = old_constraints;
                    
                    // Apply negated condition for ELSE branch
                    sema_apply_negated_constraint(s->as.if_stmt.cond, sema_ranges);
                    
                    for (StmtList *b = s->as.if_stmt.else_branch; b; b = b->next)
                        walk_stmt(b->stmt);
                        
                    // Restore state again
                    sema_ranges->head = old_head;
                    sema_ranges->constraints = old_constraints;
                    break;
                }
                case STMT_FOR:
                    sema_infer_expr(s->as.for_stmt.iterable);
                    // Range Analysis: Loop index
                    if (sema_ranges && s->as.for_stmt.iterable->kind == EXPR_RANGE && s->as.for_stmt.index_name) {
                        Range start = sema_eval_range(s->as.for_stmt.iterable->as.range_expr.start, sema_ranges);
                        Range end = sema_eval_range(s->as.for_stmt.iterable->as.range_expr.end, sema_ranges);
                        if (start.known && end.known) {
                            Range r = range_make(start.min, end.max - 1);
                            range_set(sema_ranges, s->as.for_stmt.index_name, r);
                        }
                    }
                    
                    // Widen modified variables BEFORE body (conservative approximation for loop entry)
                    if (sema_ranges) sema_widen_loop(s->as.for_stmt.body, sema_ranges);
                    
                    for (StmtList *b = s->as.for_stmt.body; b; b = b->next)
                        walk_stmt(b->stmt);
                        
                    // Widen modified variables AFTER body (conservative approximation for loop exit/non-execution)
                    if (sema_ranges) sema_widen_loop(s->as.for_stmt.body, sema_ranges);
                    break;
                case STMT_ASSIGN:
                    sema_infer_expr(s->as.assign_stmt.expr);
                    if (sema_ranges && s->as.assign_stmt.target->kind == EXPR_IDENTIFIER) {
                        Expr *rhs = s->as.assign_stmt.expr;
                        Id *lhs_id = s->as.assign_stmt.target->as.identifier_expr.id;
                        
                        // 1. Update Range
                        Range r = sema_eval_range(rhs, sema_ranges);
                        range_set(sema_ranges, lhs_id, r);
                        
                        // 2. Linear Constraints: x = y + c
                        if (rhs->kind == EXPR_BINARY) {
                            TokenKind op = rhs->as.binary_expr.op;
                            Expr *rl = rhs->as.binary_expr.left;
                            Expr *rr = rhs->as.binary_expr.right;
                            
                            // x = y + c
                            if (op == TOKEN_PLUS && rl->kind == EXPR_IDENTIFIER && rr->kind == EXPR_LITERAL) {
                                // x - y <= c  AND  y - x <= -c
                                int64_t c = rr->as.literal_expr.value;
                                constraint_add(sema_ranges, lhs_id, rl->as.identifier_expr.id, c);
                                constraint_add(sema_ranges, rl->as.identifier_expr.id, lhs_id, -c);
                            }
                            // x = c + y
                            else if (op == TOKEN_PLUS && rl->kind == EXPR_LITERAL && rr->kind == EXPR_IDENTIFIER) {
                                int64_t c = rl->as.literal_expr.value;
                                constraint_add(sema_ranges, lhs_id, rr->as.identifier_expr.id, c);
                                constraint_add(sema_ranges, rr->as.identifier_expr.id, lhs_id, -c);
                            }
                            // x = y - c
                            else if (op == TOKEN_MINUS && rl->kind == EXPR_IDENTIFIER && rr->kind == EXPR_LITERAL) {
                                // x = y - c <=> x - y = -c
                                int64_t c = rr->as.literal_expr.value;
                                constraint_add(sema_ranges, lhs_id, rl->as.identifier_expr.id, -c);
                                constraint_add(sema_ranges, rl->as.identifier_expr.id, lhs_id, c);
                            }
                        }
                        // x = y
                        else if (rhs->kind == EXPR_IDENTIFIER) {
                            // x - y <= 0 AND y - x <= 0
                            constraint_add(sema_ranges, lhs_id, rhs->as.identifier_expr.id, 0);
                            constraint_add(sema_ranges, rhs->as.identifier_expr.id, lhs_id, 0);
                        }
                    }
                    break;
                case STMT_EXPR:
                    sema_infer_expr(s->as.expr_stmt.expr);
                    break;
                case STMT_RETURN:
                    sema_infer_expr(s->as.return_stmt.value);
                    // Check Post-Contracts
                    if (current_function_decl && current_function_decl->as.function_decl.post_contracts) {
                        Range ret_range = sema_eval_range(s->as.return_stmt.value, sema_ranges);
                        
                        for (ExprList *post = current_function_decl->as.function_decl.post_contracts; post; post = post->next) {
                            // Verify the contract
                            int result = sema_check_post_condition(post->expr, ret_range, sema_ranges);
                            
                            if (result == 0) {
                                // Definitely false -> Error
                                fprintf(stderr, "Error: Post-condition violation. Return value cannot satisfy contract.\n");
                                exit(1);
                            }
                            // If result == -1 (unknown), we assume it's okay for now (or warn?)
                            // For strict DbC, we might want to error if we can't prove it.
                            // But given our limited range analysis, that might be too strict.
                            // Let's stick to "error if definitely false".
                        }
                    }
                    
                    // Check equation-style return constraints: func f() int >= 0
                    if (current_function_decl && current_function_decl->as.function_decl.return_constraints) {
                        Range ret_range = sema_eval_range(s->as.return_stmt.value, sema_ranges);
                        
                        for (ExprList *rc = current_function_decl->as.function_decl.return_constraints; rc; rc = rc->next) {
                            int result = sema_check_post_condition(rc->expr, ret_range, sema_ranges);
                            
                            if (result == 0) {
                                fprintf(stderr, "Error: Return constraint violation. Return value does not satisfy type constraint.\n");
                                exit(1);
                            }
                        }
                    }
                    break;
                case STMT_MATCH:
                    sema_infer_expr(s->as.match_stmt.value);
                    for (StmtMatchCase *c = s->as.match_stmt.cases; c; c = c->next) {
                        if (c->pattern) sema_infer_expr(c->pattern);
                        for (StmtList *b = c->body; b; b = b->next)
                            walk_stmt(b->stmt);
                    }
                    break;
                case STMT_UNSAFE: {
                    // (removed debug print)
                    bool old_unsafe = sema_in_unsafe_block;
                    sema_in_unsafe_block = true;
                    for (StmtList *b = s->as.unsafe_stmt.body; b; b = b->next) {
                        walk_stmt(b->stmt);
                    }
                    sema_in_unsafe_block = old_unsafe;
                    // (removed debug print)
                    break;
                }
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
