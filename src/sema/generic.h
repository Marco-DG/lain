#ifndef SEMA_GENERIC_H
#define SEMA_GENERIC_H

#include "../ast.h"
#include <string.h>

extern Arena *sema_arena;

// Forward declarations of substitution functions
void generic_substitute_type(Type **t, const char *param_name, Type *actual_type);
void generic_substitute_expr(Expr *e, const char *param_name, Type *actual_type);
void generic_substitute_stmt(Stmt *s, const char *param_name, Type *actual_type);
void generic_substitute_decl(Decl *d, const char *param_name, Type *actual_type);

void generic_substitute_type(Type **t_ptr, const char *param_name, Type *actual_type) {
    if (!t_ptr || !*t_ptr) return;
    Type *t = *t_ptr;

    if (t->kind == TYPE_SIMPLE && t->base_type) {
        if ((size_t)t->base_type->length == strlen(param_name) &&
            strncmp(t->base_type->name, param_name, t->base_type->length) == 0) {
            // F-044 fix: preserve the original ownership mode when substituting.
            // Without this, `mov T` specialized with `T = Handle` would lose
            // the `mov` qualifier and the parameter would no longer be linear.
            OwnershipMode original_mode = t->mode;
            if (original_mode == MODE_SHARED || actual_type->mode == original_mode) {
                *t_ptr = actual_type;
            } else {
                // Shallow-clone actual_type preserving the original mode.
                Type *cloned = arena_push_aligned(sema_arena, Type);
                *cloned = *actual_type;
                cloned->mode = original_mode;
                *t_ptr = cloned;
            }
        }
    } else if (t->kind == TYPE_VAR && t->base_type) {
        // 'T type variable — substitute if name matches
        if ((size_t)t->base_type->length == strlen(param_name) &&
            strncmp(t->base_type->name, param_name, t->base_type->length) == 0) {
            OwnershipMode original_mode = t->mode;
            if (original_mode == MODE_SHARED || actual_type->mode == original_mode) {
                *t_ptr = actual_type;
            } else {
                Type *cloned = arena_push_aligned(sema_arena, Type);
                *cloned = *actual_type;
                cloned->mode = original_mode;
                *t_ptr = cloned;
            }
        }
    } else if (t->kind == TYPE_ARRAY || t->kind == TYPE_SLICE || t->kind == TYPE_POINTER || t->kind == TYPE_COMPTIME) {
        generic_substitute_type(&t->element_type, param_name, actual_type);
    }
}

void generic_substitute_expr(Expr *e, const char *param_name, Type *actual_type) {
    if (!e) return;
    generic_substitute_type(&e->type, param_name, actual_type);

    switch (e->kind) {
        case EXPR_BINARY:
            generic_substitute_expr(e->as.binary_expr.left, param_name, actual_type);
            generic_substitute_expr(e->as.binary_expr.right, param_name, actual_type);
            break;
        case EXPR_UNARY:
            generic_substitute_expr(e->as.unary_expr.right, param_name, actual_type);
            break;
        case EXPR_IDENTIFIER:
        case EXPR_LITERAL:
        case EXPR_FLOAT_LITERAL:
        case EXPR_STRING:
        case EXPR_CHAR:
        case EXPR_UNDEFINED:
            break;
        case EXPR_MEMBER:
            generic_substitute_expr(e->as.member_expr.target, param_name, actual_type);
            break;
        case EXPR_CALL:
            generic_substitute_expr(e->as.call_expr.callee, param_name, actual_type);
            for (ExprList *l = e->as.call_expr.args; l; l = l->next) {
                generic_substitute_expr(l->expr, param_name, actual_type);
            }
            break;
        case EXPR_RANGE:
            generic_substitute_expr(e->as.range_expr.start, param_name, actual_type);
            generic_substitute_expr(e->as.range_expr.end, param_name, actual_type);
            break;
        case EXPR_INDEX:
            generic_substitute_expr(e->as.index_expr.target, param_name, actual_type);
            generic_substitute_expr(e->as.index_expr.index, param_name, actual_type);
            break;
        case EXPR_MOVE:
            generic_substitute_expr(e->as.move_expr.expr, param_name, actual_type);
            break;
        case EXPR_MUT:
            generic_substitute_expr(e->as.mut_expr.expr, param_name, actual_type);
            break;
        case EXPR_CAST:
            generic_substitute_expr(e->as.cast_expr.expr, param_name, actual_type);
            generic_substitute_type(&e->as.cast_expr.target_type, param_name, actual_type);
            break;
        case EXPR_MATCH:
            generic_substitute_expr(e->as.match_expr.value, param_name, actual_type);
            for (ExprMatchCase *c = e->as.match_expr.cases; c; c = c->next) {
                for (ExprList *p = c->patterns; p; p = p->next) {
                    generic_substitute_expr(p->expr, param_name, actual_type);
                }
                generic_substitute_expr(c->body, param_name, actual_type);
            }
            break;
        case EXPR_TYPE:
            generic_substitute_type(&e->as.type_expr.type_value, param_name, actual_type);
            break;
        case EXPR_ANON_STRUCT:
            for (DeclList *l = e->as.anon_struct_expr.fields; l; l = l->next) generic_substitute_decl(l->decl, param_name, actual_type);
            break;
        case EXPR_ANON_ENUM:
            for (Variant *v = e->as.anon_enum_expr.variants; v; v = v->next) {
                for (DeclList *l = v->fields; l; l = l->next) generic_substitute_decl(l->decl, param_name, actual_type);
            }
            break;
        case EXPR_ARRAY_LITERAL:
            for (ExprList *l = e->as.array_literal_expr.elements; l; l = l->next) {
                generic_substitute_expr(l->expr, param_name, actual_type);
            }
            break;
        case EXPR_BUILTIN:
            // No type references in builtins
            break;
    }
}

void generic_substitute_stmt(Stmt *s, const char *param_name, Type *actual_type) {
    if (!s) return;
    switch (s->kind) {
        case STMT_VAR:
            generic_substitute_type(&s->as.var_stmt.type, param_name, actual_type);
            generic_substitute_expr(s->as.var_stmt.expr, param_name, actual_type);
            break;
        case STMT_ASSIGN:
            generic_substitute_expr(s->as.assign_stmt.target, param_name, actual_type);
            generic_substitute_expr(s->as.assign_stmt.expr, param_name, actual_type);
            break;
        case STMT_EXPR:
            generic_substitute_expr(s->as.expr_stmt.expr, param_name, actual_type);
            break;
        case STMT_IF:
            generic_substitute_expr(s->as.if_stmt.cond, param_name, actual_type);
            for (StmtList *l = s->as.if_stmt.then_body; l; l = l->next) generic_substitute_stmt(l->stmt, param_name, actual_type);
            for (StmtList *l = s->as.if_stmt.else_branch; l; l = l->next) generic_substitute_stmt(l->stmt, param_name, actual_type);
            break;
        case STMT_FOR:
            generic_substitute_expr(s->as.for_stmt.iterable, param_name, actual_type);
            for (StmtList *l = s->as.for_stmt.body; l; l = l->next) generic_substitute_stmt(l->stmt, param_name, actual_type);
            break;
        case STMT_WHILE:
            generic_substitute_expr(s->as.while_stmt.cond, param_name, actual_type);
            for (StmtList *l = s->as.while_stmt.body; l; l = l->next) generic_substitute_stmt(l->stmt, param_name, actual_type);
            break;
        case STMT_MATCH:
            generic_substitute_expr(s->as.match_stmt.value, param_name, actual_type);
            for (StmtMatchCase *c = s->as.match_stmt.cases; c; c = c->next) {
                for (ExprList *p = c->patterns; p; p = p->next) generic_substitute_expr(p->expr, param_name, actual_type);
                for (StmtList *l = c->body; l; l = l->next) generic_substitute_stmt(l->stmt, param_name, actual_type);
            }
            break;
        case STMT_USE:
            generic_substitute_expr(s->as.use_stmt.target, param_name, actual_type);
            break;
        case STMT_RETURN:
            generic_substitute_expr(s->as.return_stmt.value, param_name, actual_type);
            break;
        case STMT_UNSAFE:
            for (StmtList *l = s->as.unsafe_stmt.body; l; l = l->next) generic_substitute_stmt(l->stmt, param_name, actual_type);
            break;
        case STMT_DEFER:
            generic_substitute_stmt(s->as.defer_stmt.stmt, param_name, actual_type);
            break;
        case STMT_COMPTIME_IF:
            generic_substitute_expr(s->as.comptime_if_stmt.cond, param_name, actual_type);
            for (StmtList *l = s->as.comptime_if_stmt.then_body; l; l = l->next) generic_substitute_stmt(l->stmt, param_name, actual_type);
            for (StmtList *l = s->as.comptime_if_stmt.else_branch; l; l = l->next) generic_substitute_stmt(l->stmt, param_name, actual_type);
            break;
        case STMT_CONTINUE:
        case STMT_BREAK:
        case STMT_MATCH_CASE:
            break;
    }
}

void generic_substitute_decl(Decl *d, const char *param_name, Type *actual_type) {
    if (!d) return;
    switch (d->kind) {
        case DECL_VARIABLE:
            generic_substitute_type(&d->as.variable_decl.type, param_name, actual_type);
            for (ExprList *l = d->as.variable_decl.constraints; l; l = l->next) generic_substitute_expr(l->expr, param_name, actual_type);
            break;
        case DECL_FUNCTION:
        case DECL_PROCEDURE:
        case DECL_EXTERN_FUNCTION:
        case DECL_EXTERN_PROCEDURE:
            for (DeclList *l = d->as.function_decl.params; l; l = l->next) generic_substitute_decl(l->decl, param_name, actual_type);
            generic_substitute_type(&d->as.function_decl.return_type, param_name, actual_type);
            for (StmtList *l = d->as.function_decl.body; l; l = l->next) generic_substitute_stmt(l->stmt, param_name, actual_type);
            for (ExprList *l = d->as.function_decl.pre_contracts; l; l = l->next) generic_substitute_expr(l->expr, param_name, actual_type);
            for (ExprList *l = d->as.function_decl.post_contracts; l; l = l->next) generic_substitute_expr(l->expr, param_name, actual_type);
            for (ExprList *l = d->as.function_decl.return_constraints; l; l = l->next) generic_substitute_expr(l->expr, param_name, actual_type);
            break;
        case DECL_STRUCT:
            for (DeclList *l = d->as.struct_decl.fields; l; l = l->next) generic_substitute_decl(l->decl, param_name, actual_type);
            break;
        case DECL_ENUM:
            for (Variant *v = d->as.enum_decl.variants; v; v = v->next) {
                for (DeclList *l = v->fields; l; l = l->next) generic_substitute_decl(l->decl, param_name, actual_type);
            }
            break;
        case DECL_DESTRUCT:
            generic_substitute_type(&d->as.destruct_decl.type, param_name, actual_type);
            break;
        case DECL_IMPORT:
        case DECL_EVAL_IMPORT:
        case DECL_C_INCLUDE:
        case DECL_EXTERN_TYPE:
            break;
        case DECL_TYPE_ALIAS:
            generic_substitute_expr(d->as.type_alias_decl.expr, param_name, actual_type);
            break;
    }
}

// Substitute a size variable 'N with a concrete integer value in all TYPE_ARRAY
// size_expr positions. Used for monomorphizing functions with *T['N] params.
void generic_substitute_size_in_type(Type **t_ptr, const char *var_name, isize size_val) {
    if (!t_ptr || !*t_ptr) return;
    Type *t = *t_ptr;

    if (t->kind == TYPE_ARRAY && t->array_len < 0 &&
        t->size_relop == TOKEN_TYPEVAR && t->size_expr &&
        t->size_expr->kind == EXPR_IDENTIFIER) {
        Id *n = t->size_expr->as.identifier_expr.id;
        if ((size_t)n->length == strlen(var_name) &&
            strncmp(n->name, var_name, n->length) == 0) {
            Type *newt = arena_push_aligned(sema_arena, Type);
            *newt = *t;
            newt->array_len = size_val;
            newt->size_expr = NULL;
            newt->size_relop = 0;
            *t_ptr = newt;
            return;
        }
    }
    // Recurse into nested types
    if (t->kind == TYPE_ARRAY || t->kind == TYPE_POINTER ||
        t->kind == TYPE_SLICE || t->kind == TYPE_COMPTIME) {
        generic_substitute_size_in_type(&t->element_type, var_name, size_val);
    }
}

static void generic_substitute_size_in_decl(Decl *d, const char *var_name, isize size_val);

static void generic_substitute_size_in_stmt(Stmt *s, const char *var_name, isize size_val) {
    if (!s) return;
    switch (s->kind) {
        case STMT_VAR:
            generic_substitute_size_in_type(&s->as.var_stmt.type, var_name, size_val);
            break;
        case STMT_RETURN:
            break; // expr types handled by infer pass
        default: break;
    }
    // Walk sub-stmts
    StmtList *body = NULL;
    switch (s->kind) {
        case STMT_IF:
            for (StmtList *b = s->as.if_stmt.then_body; b; b = b->next)
                generic_substitute_size_in_stmt(b->stmt, var_name, size_val);
            for (StmtList *b = s->as.if_stmt.else_branch; b; b = b->next)
                generic_substitute_size_in_stmt(b->stmt, var_name, size_val);
            break;
        case STMT_WHILE:
            for (StmtList *b = s->as.while_stmt.body; b; b = b->next)
                generic_substitute_size_in_stmt(b->stmt, var_name, size_val);
            break;
        default: (void)body; break;
    }
}

static void generic_substitute_size_in_decl(Decl *d, const char *var_name, isize size_val) {
    if (!d) return;
    if (d->kind == DECL_FUNCTION || d->kind == DECL_PROCEDURE) {
        for (DeclList *p = d->as.function_decl.params; p; p = p->next) {
            if (p->decl && p->decl->kind == DECL_VARIABLE)
                generic_substitute_size_in_type(&p->decl->as.variable_decl.type,
                                                var_name, size_val);
        }
        generic_substitute_size_in_type(&d->as.function_decl.return_type,
                                        var_name, size_val);
        for (StmtList *b = d->as.function_decl.body; b; b = b->next)
            generic_substitute_size_in_stmt(b->stmt, var_name, size_val);
    }
}

#endif // SEMA_GENERIC_H
