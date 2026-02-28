#ifndef SEMA_GENERIC_H
#define SEMA_GENERIC_H

#include "../ast.h"
#include <string.h>

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
            // Replace this type with actual_type!
            // But keep the mode (ownership) if the parametric type has it?
            // Usually, `mov T` means T is the actual type and mode=MODE_OWNED.
            OwnershipMode original_mode = t->mode;
            *t_ptr = actual_type; // Since we cloned the AST, we could just replace the pointer, BUT other places might share actual_type. We should probably clone actual_type if we change its mode? Let's just point to it for now (Phase A).
            // Actually, if we had `mov T`, `t->mode` is `MODE_OWNED`. If `actual_type` is just `int`, we need an owned `int`.
            // Because types are shared pointers, modifying actual_type directly is BAD.
            // But we don't have an arena here. For Phase A, let's assume `T` has MODE_SHARED if it's just `T`.
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
            for (StmtList *l = s->as.if_stmt.then_branch; l; l = l->next) generic_substitute_stmt(l->stmt, param_name, actual_type);
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
        case DECL_C_INCLUDE:
        case DECL_EXTERN_TYPE:
            break;
    }
}

#endif // SEMA_GENERIC_H
