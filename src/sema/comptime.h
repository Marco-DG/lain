#ifndef SEMANTICS_COMPTIME_H
#define SEMANTICS_COMPTIME_H

#include "../ast.h"
#include "../ast_clone.h"
#include <stdio.h>
#include <stdlib.h>

/*
    Comptime Interpreter for Phase B
    Executes a subset of the AST to return a compile-time value.
    In Phase B, we focus on functions returning `type` expressions
    to support type aliases like `type OptionInt = Option(int)`.
*/

// A simple structure to hold local variables during comptime execution
typedef struct ComptimeEnv {
    Id* name;
    Expr* value;
    struct ComptimeEnv* next;
} ComptimeEnv;

// Forward declaration from generic.h
void generic_substitute_expr(Expr *e, const char *param_name, Type *actual_type);
Type* get_builtin_int_type(void);

ComptimeEnv* comptime_env_push(Arena* arena, ComptimeEnv* env, Id* name, Expr* value) {
    ComptimeEnv* node = arena_push_aligned(arena, ComptimeEnv);
    node->name = name;
    node->value = value;
    node->next = env;
    return node;
}

Expr* comptime_env_lookup(ComptimeEnv* env, Id* name) {
    for (ComptimeEnv* curr = env; curr; curr = curr->next) {
        if (curr->name->length == name->length && 
            strncmp(curr->name->name, name->name, name->length) == 0) {
            return curr->value;
        }
    }
    return NULL;
}

// Forward declarations of evaluation functions
Expr* comptime_evaluate_expr(Arena* arena, Expr* expr, ComptimeEnv* env);
Expr* comptime_evaluate_stmt_list(Arena* arena, StmtList* stmts, ComptimeEnv* env);
Expr* comptime_evaluate_function(Arena* arena, Decl* func_decl, ExprList* args);

Expr* comptime_evaluate_expr(Arena* arena, Expr* expr, ComptimeEnv* env) {
    if (!expr) return NULL;
    
    switch (expr->kind) {
        case EXPR_IDENTIFIER: {
            Id* id = expr->as.identifier_expr.id;
            // Variable lookup
            Expr* val = comptime_env_lookup(env, id);
            if (!val) {
                // If resolving failed, we return the identifier itself, OR look it up globally
                char raw[256];
                int L = id->length < (int)sizeof(raw)-1 ? id->length : (int)sizeof(raw)-1;
                memcpy(raw, id->name, L);
                raw[L] = '\0';
                
                Symbol* sym = sema_lookup(raw);
                if (sym && sym->decl && (sym->decl->kind == DECL_STRUCT || sym->decl->kind == DECL_ENUM || sym->decl->kind == DECL_EXTERN_TYPE || sym->decl->kind == DECL_TYPE_ALIAS)) {
                    Expr* texpr = clone_expr(arena, expr);
                    texpr->kind = EXPR_TYPE;
                    texpr->as.type_expr.type_value = sym->type;
                    return texpr;
                }
                
                // e.g. 'int' comes from type_simple lookup usually, but builtin int might not be a sym?
                // Wait! "int", "bool", "u8" are not in sema_globals! They are primitive types.
                if (L == 3 && strncmp(raw, "int", 3) == 0) {
                    Expr* texpr = clone_expr(arena, expr);
                    texpr->kind = EXPR_TYPE;
                    texpr->as.type_expr.type_value = get_builtin_int_type();
                    return texpr;
                }
                if (L == 4 && strncmp(raw, "bool", 4) == 0) {
                    Expr* texpr = clone_expr(arena, expr);
                    texpr->kind = EXPR_TYPE;
                    texpr->as.type_expr.type_value = type_simple(arena, id);
                    return texpr;
                }
                // Same for "comptime_string", "comptime_int"
                
                return expr;
            }
            return clone_expr(arena, val);
        }
        case EXPR_BINARY: {
            Expr* left = comptime_evaluate_expr(arena, expr->as.binary_expr.left, env);
            Expr* right = comptime_evaluate_expr(arena, expr->as.binary_expr.right, env);
            
            if (left && right && left->kind == EXPR_TYPE && right->kind == EXPR_TYPE) {
                Type *t1 = left->as.type_expr.type_value;
                Type *t2 = right->as.type_expr.type_value;
                bool eq = false;
                if (t1->kind == t2->kind && t1->kind == TYPE_SIMPLE) {
                    if (t1->base_type->length == t2->base_type->length &&
                        strncmp(t1->base_type->name, t2->base_type->name, t1->base_type->length) == 0) {
                        eq = true;
                    }
                }
                
                bool res = false;
                if (expr->as.binary_expr.op == TOKEN_EQUAL_EQUAL) res = eq;
                else if (expr->as.binary_expr.op == TOKEN_BANG_EQUAL) res = !eq;
                
                Expr* bool_expr = clone_expr(arena, expr);
                bool_expr->kind = EXPR_LITERAL;
                bool_expr->as.literal_expr.value = res ? 1 : 0;
                return bool_expr;
            }
            return clone_expr(arena, expr);
        }
        case EXPR_TYPE:
        case EXPR_ANON_STRUCT:
        case EXPR_ANON_ENUM:
            // These are already fully evaluated compile-time values
            return clone_expr(arena, expr);
        
        case EXPR_MEMBER: {
            // Since we are parsing things like `OptionInt` from `type OptionInt = Option(int)`
            // We might just need to pass the member expression through un-evaluated for now,
            // or fully evaluate if it's a known struct. For Phase B, returning types is our main goal.
            Expr* target_eval = comptime_evaluate_expr(arena, expr->as.member_expr.target, env);
            // Reconstruct the member expression with evaluated target
            Expr* res = clone_expr(arena, expr);
            res->as.member_expr.target = target_eval;
            return res;
        }

        case EXPR_CALL: {
            if (expr->as.call_expr.callee->kind == EXPR_IDENTIFIER) {
                Id* callee_id = expr->as.call_expr.callee->as.identifier_expr.id;
                
                // Intrinsic: compileError
                if (strncmp(callee_id->name, "compileError", 12) == 0) {
                    if (expr->as.call_expr.args && expr->as.call_expr.args->expr->kind == EXPR_STRING) {
                        const char* error_msg = expr->as.call_expr.args->expr->as.string_expr.value;
                        fprintf(stderr, "Compile Error: %.*s\n", 
                            (int)expr->as.call_expr.args->expr->as.string_expr.length, error_msg);
                        exit(1);
                    } else {
                        fprintf(stderr, "Compile Error: compileError expects a string literal.\n");
                        exit(1);
                    }
                }
                
                Decl* callee_decl = expr->as.call_expr.callee->decl;
                
                // If it wasn't resolved yet, try looking it up by its name (e.g. for intrinsic checks)
                if (!callee_decl) {
                    char raw[256];
                    int L = callee_id->length;
                    if (L >= (int)sizeof(raw)) L = sizeof(raw) - 1;
                    memcpy(raw, callee_id->name, L);
                    raw[L] = '\0';
                    Symbol* sym = sema_lookup(raw);
                    if (sym) callee_decl = sym->decl;
                }
                
                if (callee_decl && callee_decl->kind == DECL_FUNCTION) {
                    // Evaluate arguments
                    ExprList* eval_args = NULL;
                    ExprList** args_tail = &eval_args;
                    for (ExprList* curr_arg = expr->as.call_expr.args; curr_arg; curr_arg = curr_arg->next) {
                        ExprList* new_arg = arena_push_aligned(arena, ExprList);
                        new_arg->expr = comptime_evaluate_expr(arena, curr_arg->expr, env);
                        new_arg->next = NULL;
                        *args_tail = new_arg;
                        args_tail = &new_arg->next;
                    }
                    
                    // Execute function!
                    Expr* result = comptime_evaluate_function(arena, callee_decl, eval_args);
                    if (result) return result;
                }
            }
            
            return clone_expr(arena, expr);
        }

        default:
            return clone_expr(arena, expr);
    }
}

// Evaluate a list of statements. Returns the returned expression if a return statement is hit.
Expr* comptime_evaluate_stmt_list(Arena* arena, StmtList* stmts, ComptimeEnv* env) {
    for (StmtList* curr = stmts; curr; curr = curr->next) {
        Stmt* stmt = curr->stmt;
        switch (stmt->kind) {
            case STMT_VAR: {
                Expr* init_val = comptime_evaluate_expr(arena, stmt->as.var_stmt.expr, env);
                env = comptime_env_push(arena, env, stmt->as.var_stmt.name, init_val);
                break;
            }
            case STMT_ASSIGN: {
                // In Phase B, we assume assignment targets are identifiers for simplicity.
                if (stmt->as.assign_stmt.target->kind == EXPR_IDENTIFIER) {
                    Expr* val = comptime_evaluate_expr(arena, stmt->as.assign_stmt.expr, env);
                    // Update existing environment variable (we'd mutate the node value)
                    Id* target_id = stmt->as.assign_stmt.target->as.identifier_expr.id;
                    for (ComptimeEnv* e = env; e; e = e->next) {
                        if (e->name->length == target_id->length && 
                            strncmp(e->name->name, target_id->name, target_id->length) == 0) {
                            e->value = val;
                            break;
                        }
                    }
                }
                break;
            }
            case STMT_IF: {
                Expr* cond = comptime_evaluate_expr(arena, stmt->as.if_stmt.cond, env);
                bool is_true = false;
                if (cond && cond->kind == EXPR_LITERAL) {
                    is_true = cond->as.literal_expr.value != 0;
                }
                
                if (is_true) {
                     Expr* ret = comptime_evaluate_stmt_list(arena, stmt->as.if_stmt.then_branch, env);
                     if (ret) return ret;
                } else if (stmt->as.if_stmt.else_branch) {
                     Expr* ret = comptime_evaluate_stmt_list(arena, stmt->as.if_stmt.else_branch, env);
                     if (ret) return ret;
                }
                break;
            }
            case STMT_RETURN: {
                return comptime_evaluate_expr(arena, stmt->as.return_stmt.value, env);
            }
            case STMT_EXPR: {
                comptime_evaluate_expr(arena, stmt->as.expr_stmt.expr, env);
                break;
            }
            default:
                break; // Ignore other statements in a basic interpreter
        }
    }
    return NULL; // Function fell through without returning
}

// Entry point: evaluates a function declaration statically given argument expressions.
Expr* comptime_evaluate_function(Arena* arena, Decl* func_decl, ExprList* args) {
    if (func_decl->kind != DECL_FUNCTION) return NULL;
    
    ComptimeEnv* env = NULL;
    
    // Bind arguments to parameters
    DeclList* param_curr = func_decl->as.function_decl.params;
    ExprList* arg_curr = args;
    
    while (param_curr && arg_curr) {
        if (param_curr->decl->kind == DECL_VARIABLE) {
            // We assume arguments passed to comptime functions are fully evaluated (e.g. types)
            env = comptime_env_push(arena, env, param_curr->decl->as.variable_decl.name, arg_curr->expr);
        }
        param_curr = param_curr->next;
        arg_curr = arg_curr->next;
    }
    
    Expr* result = comptime_evaluate_stmt_list(arena, func_decl->as.function_decl.body, env);
    
    if (result) {
        // Substitute all comptime type parameters into the resulting AST!
        for (ComptimeEnv* e = env; e; e = e->next) {
            if (e->value && e->value->kind == EXPR_TYPE) {
                char param_str[256];
                snprintf(param_str, 256, "%.*s", (int)e->name->length, e->name->name);
                generic_substitute_expr(result, param_str, e->value->as.type_expr.type_value);
            }
        }
    }
    
    return result;
}

#endif // SEMANTICS_COMPTIME_H
