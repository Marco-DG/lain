#ifndef SEMANTICS_COMPTIME_H
#define SEMANTICS_COMPTIME_H

#include "../ast.h"
#include "../ast_clone.h"
#include <stdio.h>
#include <stdlib.h>

// The TYPE-LEVEL compile-time evaluator (DECIDE-W v2, section 3): a type alias's right-hand side
// (`type OptInt = Option(i32)`, `type Buf4 = u8[K]`) and a `comptime if` condition (`@os == 1`).
// It computes no value that also exists at run time: every such value is the IR's, computed by
// the interpreter (static_eval.h) or folded by the front end's constant evaluator (sa_eval). Its
// statement interpreter and variable environment (ComptimeEnv, comptime_evaluate_stmt_list) had no
// caller and were deleted with plan I.20; Handwriting's M6 found 40 of its 41 dispatches in the
// corpus to be alias right-hand sides and one a `comptime if @os == 1`.

Type* get_builtin_i32_type(void);

Expr* comptime_evaluate_expr(Arena* arena, Expr* expr);

Expr* comptime_evaluate_expr(Arena* arena, Expr* expr) {
    if (!expr) return NULL;
    
    switch (expr->kind) {
        case EXPR_IDENTIFIER: {
            Id* id = expr->as.identifier_expr.id;
            {
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
                
                // Sized integer builtins (iN / uN, N=1..64) aren't in
                // sema_globals — recognize them as type values directly.
                if (L >= 2 && L <= 3 && (raw[0] == 'i' || raw[0] == 'u')) {
                    bool all_digits = true;
                    int bits = 0;
                    for (int k = 1; k < L; k++) {
                        if (raw[k] < '0' || raw[k] > '9') { all_digits = false; break; }
                        bits = bits * 10 + (raw[k] - '0');
                    }
                    if (all_digits && bits >= 1 && bits <= 64) {
                        Id *type_id = arena_push_aligned(arena, Id);
                        char *nbuf = arena_push_many_aligned(arena, char, L + 1);
                        memcpy(nbuf, raw, L);
                        nbuf[L] = '\0';
                        type_id->name = nbuf;
                        type_id->length = L;
                        Expr* texpr = clone_expr(arena, expr);
                        texpr->kind = EXPR_TYPE;
                        texpr->as.type_expr.type_value = type_simple(arena, type_id);
                        return texpr;
                    }
                }
                if (L == 4 && strncmp(raw, "bool", 4) == 0) {
                    Expr* texpr = clone_expr(arena, expr);
                    texpr->kind = EXPR_TYPE;
                    texpr->as.type_expr.type_value = type_simple(arena, id);
                    return texpr;
                }
                // `int` and `float` documented aliases.
                if ((L == 3 && strncmp(raw, "int", 3) == 0) ||
                    (L == 5 && strncmp(raw, "float", 5) == 0)) {
                    Expr* texpr = clone_expr(arena, expr);
                    texpr->kind = EXPR_TYPE;
                    texpr->as.type_expr.type_value = type_simple(arena, id);
                    return texpr;
                }
                // Same for "comptime_string", "comptime_int"
                
                return expr;
            }
        }
        case EXPR_BINARY: {
            Expr* left = comptime_evaluate_expr(arena, expr->as.binary_expr.left);
            Expr* right = comptime_evaluate_expr(arena, expr->as.binary_expr.right);
            
            // Integer literal comparison (for @os == 1, etc.)
            if (left && right && left->kind == EXPR_LITERAL && right->kind == EXPR_LITERAL) {
                int lv = left->as.literal_expr.value;
                int rv = right->as.literal_expr.value;
                bool res = false;
                switch (expr->as.binary_expr.op) {
                    case TOKEN_EQUAL_EQUAL:              res = (lv == rv); break;
                    case TOKEN_BANG_EQUAL:               res = (lv != rv); break;
                    case TOKEN_ANGLE_BRACKET_LEFT:       res = (lv <  rv); break;
                    case TOKEN_ANGLE_BRACKET_LEFT_EQUAL: res = (lv <= rv); break;
                    case TOKEN_ANGLE_BRACKET_RIGHT:      res = (lv >  rv); break;
                    case TOKEN_ANGLE_BRACKET_RIGHT_EQUAL:res = (lv >= rv); break;
                    default: break;
                }
                Expr* bool_expr = clone_expr(arena, expr);
                bool_expr->kind = EXPR_LITERAL;
                bool_expr->as.literal_expr.value = res ? 1 : 0;
                return bool_expr;
            }

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

        case EXPR_INDEX: {
            // `type A = u8[K]`: an alias of an ARRAY TYPE. The right-hand side parses as an index
            // into a type name, and with a constant index it IS the array type. Every such alias
            // was refused ("Type alias must evaluate to a type", at Ln 0), with a literal length
            // as well as a named constant (Handwriting's M6 probe table).
            Expr *t = comptime_evaluate_expr(arena, expr->as.index_expr.target);
            Expr *ix = expr->as.index_expr.index;
            bool lay = false; __int128 n = 0;
            if (t && t->kind == EXPR_TYPE && t->as.type_expr.type_value && ix
                && sa_is_const(ix, &lay) && !lay && sa_eval(ix, &n) && n > 0 && n <= INT32_MAX) {
                Expr *texpr = clone_expr(arena, expr);
                texpr->kind = EXPR_TYPE;
                texpr->as.type_expr.type_value = type_array(arena, t->as.type_expr.type_value, (isize)n);
                return texpr;
            }
            return clone_expr(arena, expr);
        }
        
        case EXPR_MEMBER: {
            // Since we are parsing things like `OptionInt` from `type OptionInt = Option(int)`
            // We might just need to pass the member expression through un-evaluated for now,
            // or fully evaluate if it's a known struct. For Phase B, returning types is our main goal.
            Expr* target_eval = comptime_evaluate_expr(arena, expr->as.member_expr.target);
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
                
                // ── G-006: CTFE purity, from the ROW rather than the keyword ─────────────
                // Compile-time evaluation genuinely needs purity: there is no console to print
                // to and no clock to read while the compiler runs. What it does NOT need is a
                // keyword — the effect row already says whether a function is pure, and with one
                // introducer there is no `proc` to look for.
                //
                // An `extern` is still allowed through below: with no body there is nothing to
                // execute, and the programmer's declared row is believed (I-016).
                bool callee_impure = false;

                if (callee_decl && (callee_decl->kind == DECL_FUNCTION)
                    && callee_decl->as.function_decl.effects_declared
                    && callee_decl->as.function_decl.effects_bound != 0) callee_impure = true;
                if (callee_impure) {
                    fprintf(stderr, "[E101] Comptime purity error: cannot call '%.*s' from a "
                            "comptime context — it declares effects, and compile-time evaluation "
                            "has no machine to perform them on\n",
                            (int)callee_id->length, callee_id->name);
                    exit(1);
                }
                if (callee_decl && callee_decl->kind == DECL_EXTERN_FUNCTION) {
                    // extern func is trusted by convention (I-016) but still allowed in comptime
                    // since the programmer asserted purity. Note: cannot actually execute it
                    // at comptime (no body). Falls through to clone_expr.
                }

                // NOTE: compile-time evaluation of user `func` calls to a `type`
                // (generic instantiation) was removed. A `func` call in a comptime
                // context now simply falls through unevaluated.
            }

            return clone_expr(arena, expr);
        }

        default:
            return clone_expr(arena, expr);
    }
}

#endif // SEMANTICS_COMPTIME_H
