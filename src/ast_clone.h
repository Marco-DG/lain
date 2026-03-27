#ifndef AST_CLONE_H
#define AST_CLONE_H

#include "ast.h"
#include <string.h>

/* Forward declarations */
Type *clone_type(Arena *arena, Type *t);
Expr *clone_expr(Arena *arena, Expr *e);
Stmt *clone_stmt(Arena *arena, Stmt *s);
Decl *clone_decl(Arena *arena, Decl *d);

Id *clone_id(Arena *arena, Id *id) {
    if (!id) return NULL;
    Id *new_id = arena_push_aligned(arena, Id);
    new_id->length = id->length;
    // Identifiers are usually static strings from the source, but to be truly safe over transformations, we can just copy the pointer since the source buffer lifespan is the entire compilation.
    new_id->name = id->name;
    return new_id;
}

IdList *clone_id_list(Arena *arena, IdList *list) {
    if (!list) return NULL;
    IdList *head = NULL;
    IdList **tail = &head;
    for (IdList *curr = list; curr; curr = curr->next) {
        IdList *node = arena_push_aligned(arena, IdList);
        node->id = clone_id(arena, curr->id);
        node->next = NULL;
        *tail = node;
        tail = &node->next;
    }
    return head;
}

ExprList *clone_expr_list(Arena *arena, ExprList *list) {
    if (!list) return NULL;
    ExprList *head = NULL;
    ExprList **tail = &head;
    for (ExprList *curr = list; curr; curr = curr->next) {
        ExprList *node = arena_push_aligned(arena, ExprList);
        node->expr = clone_expr(arena, curr->expr);
        node->next = NULL;
        *tail = node;
        tail = &node->next;
    }
    return head;
}

StmtList *clone_stmt_list(Arena *arena, StmtList *list) {
    if (!list) return NULL;
    StmtList *head = NULL;
    StmtList **tail = &head;
    for (StmtList *curr = list; curr; curr = curr->next) {
        StmtList *node = arena_push_aligned(arena, StmtList);
        node->stmt = clone_stmt(arena, curr->stmt);
        node->next = NULL;
        *tail = node;
        tail = &node->next;
    }
    return head;
}

DeclList *clone_decl_list(Arena *arena, DeclList *list) {
    if (!list) return NULL;
    DeclList *head = NULL;
    DeclList **tail = &head;
    for (DeclList *curr = list; curr; curr = curr->next) {
        DeclList *node = arena_push_aligned(arena, DeclList);
        node->decl = clone_decl(arena, curr->decl);
        node->next = NULL;
        *tail = node;
        tail = &node->next;
    }
    return head;
}

Type *clone_type(Arena *arena, Type *t) {
    if (!t) return NULL;
    Type *new_type = arena_push_aligned(arena, Type);
    *new_type = *t; // Shallow copy
    new_type->base_type = clone_id(arena, t->base_type);
    if (t->element_type) new_type->element_type = clone_type(arena, t->element_type);
    
    // t->variant is not cloned here; it's resolved during sema, generic substitution will re-resolve it
    new_type->variant = t->variant;
    return new_type;
}

Variant *clone_variant(Arena *arena, Variant *v) {
    if (!v) return NULL;
    Variant *head = NULL;
    Variant **tail = &head;
    for (Variant *curr = v; curr; curr = curr->next) {
        Variant *node = arena_push_aligned(arena, Variant);
        node->name = clone_id(arena, curr->name);
        node->fields = clone_decl_list(arena, curr->fields);
        node->next = NULL;
        *tail = node;
        tail = &node->next;
    }
    return head;
}

ExprMatchCase *clone_expr_match_case(Arena *arena, ExprMatchCase *cases) {
    if (!cases) return NULL;
    ExprMatchCase *head = NULL;
    ExprMatchCase **tail = &head;
    for (ExprMatchCase *curr = cases; curr; curr = curr->next) {
        ExprMatchCase *node = arena_push_aligned(arena, ExprMatchCase);
        node->patterns = clone_expr_list(arena, curr->patterns);
        node->body = clone_expr(arena, curr->body);
        node->next = NULL;
        *tail = node;
        tail = &node->next;
    }
    return head;
}

StmtMatchCase *clone_stmt_match_case(Arena *arena, StmtMatchCase *cases) {
    if (!cases) return NULL;
    StmtMatchCase *head = NULL;
    StmtMatchCase **tail = &head;
    for (StmtMatchCase *curr = cases; curr; curr = curr->next) {
        StmtMatchCase *node = arena_push_aligned(arena, StmtMatchCase);
        node->patterns = clone_expr_list(arena, curr->patterns);
        node->body = clone_stmt_list(arena, curr->body);
        node->next = NULL;
        *tail = node;
        tail = &node->next;
    }
    return head;
}

Expr *clone_expr(Arena *arena, Expr *e) {
    if (!e) return NULL;
    Expr *new_e = arena_push_aligned(arena, Expr);
    *new_e = *e; // Shallow copy metadata (line, col, kind, decl pointer etc)
    new_e->type = clone_type(arena, e->type);

    switch (e->kind) {
        case EXPR_BINARY:
            new_e->as.binary_expr.left  = clone_expr(arena, e->as.binary_expr.left);
            new_e->as.binary_expr.right = clone_expr(arena, e->as.binary_expr.right);
            break;
        case EXPR_UNARY:
            new_e->as.unary_expr.right = clone_expr(arena, e->as.unary_expr.right);
            break;
        case EXPR_IDENTIFIER:
            new_e->as.identifier_expr.id = clone_id(arena, e->as.identifier_expr.id);
            break;
        case EXPR_LITERAL:
        case EXPR_FLOAT_LITERAL:
        case EXPR_STRING:
        case EXPR_CHAR:
        case EXPR_UNDEFINED:
            // Data is primitive or pointer to source, shallow copy is fine
            break;
        case EXPR_MEMBER:
            new_e->as.member_expr.target = clone_expr(arena, e->as.member_expr.target);
            new_e->as.member_expr.member = clone_id(arena, e->as.member_expr.member);
            break;
        case EXPR_CALL:
            new_e->as.call_expr.callee = clone_expr(arena, e->as.call_expr.callee);
            new_e->as.call_expr.args   = clone_expr_list(arena, e->as.call_expr.args);
            break;
        case EXPR_RANGE:
            new_e->as.range_expr.start = clone_expr(arena, e->as.range_expr.start);
            new_e->as.range_expr.end   = clone_expr(arena, e->as.range_expr.end);
            break;
        case EXPR_INDEX:
            new_e->as.index_expr.target = clone_expr(arena, e->as.index_expr.target);
            new_e->as.index_expr.index  = clone_expr(arena, e->as.index_expr.index);
            break;
        case EXPR_MOVE:
            new_e->as.move_expr.expr = clone_expr(arena, e->as.move_expr.expr);
            break;
        case EXPR_MUT:
            new_e->as.mut_expr.expr = clone_expr(arena, e->as.mut_expr.expr);
            break;
        case EXPR_CAST:
            new_e->as.cast_expr.expr = clone_expr(arena, e->as.cast_expr.expr);
            new_e->as.cast_expr.target_type = clone_type(arena, e->as.cast_expr.target_type);
            break;
        case EXPR_MATCH:
            new_e->as.match_expr.value = clone_expr(arena, e->as.match_expr.value);
            new_e->as.match_expr.cases = clone_expr_match_case(arena, e->as.match_expr.cases);
            break;
        case EXPR_TYPE:
            new_e->as.type_expr.type_value = clone_type(arena, e->as.type_expr.type_value);
            break;
        case EXPR_ANON_STRUCT:
            new_e->as.anon_struct_expr.fields = clone_decl_list(arena, e->as.anon_struct_expr.fields);
            break;
        case EXPR_ANON_ENUM:
            new_e->as.anon_enum_expr.variants = clone_variant(arena, e->as.anon_enum_expr.variants);
            break;
        case EXPR_ARRAY_LITERAL:
            new_e->as.array_literal_expr.elements = clone_expr_list(arena, e->as.array_literal_expr.elements);
            break;
        case EXPR_BUILTIN:
            // Shallow copy is fine — builtin_kind is a plain enum value
            break;
    }
    return new_e;
}

Stmt *clone_stmt(Arena *arena, Stmt *s) {
    if (!s) return NULL;
    Stmt *new_s = arena_push_aligned(arena, Stmt);
    *new_s = *s;

    switch (s->kind) {
        case STMT_VAR:
            new_s->as.var_stmt.name = clone_id(arena, s->as.var_stmt.name);
            new_s->as.var_stmt.type = clone_type(arena, s->as.var_stmt.type);
            new_s->as.var_stmt.expr = clone_expr(arena, s->as.var_stmt.expr);
            break;
        case STMT_ASSIGN:
            new_s->as.assign_stmt.target = clone_expr(arena, s->as.assign_stmt.target);
            new_s->as.assign_stmt.expr   = clone_expr(arena, s->as.assign_stmt.expr);
            break;
        case STMT_EXPR:
            new_s->as.expr_stmt.expr = clone_expr(arena, s->as.expr_stmt.expr);
            break;
        case STMT_IF:
            new_s->as.if_stmt.cond        = clone_expr(arena, s->as.if_stmt.cond);
            new_s->as.if_stmt.then_branch = clone_stmt_list(arena, s->as.if_stmt.then_branch);
            new_s->as.if_stmt.else_branch = clone_stmt_list(arena, s->as.if_stmt.else_branch);
            break;
        case STMT_FOR:
            new_s->as.for_stmt.index_name = clone_id(arena, s->as.for_stmt.index_name);
            new_s->as.for_stmt.value_name = clone_id(arena, s->as.for_stmt.value_name);
            new_s->as.for_stmt.iterable   = clone_expr(arena, s->as.for_stmt.iterable);
            new_s->as.for_stmt.body       = clone_stmt_list(arena, s->as.for_stmt.body);
            break;
        case STMT_WHILE:
            new_s->as.while_stmt.cond = clone_expr(arena, s->as.while_stmt.cond);
            new_s->as.while_stmt.body = clone_stmt_list(arena, s->as.while_stmt.body);
            break;
        case STMT_MATCH_CASE:
            // Should not be encountered directly, but matched via STMT_MATCH
            break;
        case STMT_MATCH:
            new_s->as.match_stmt.value = clone_expr(arena, s->as.match_stmt.value);
            new_s->as.match_stmt.cases = clone_stmt_match_case(arena, s->as.match_stmt.cases);
            break;
        case STMT_USE:
            new_s->as.use_stmt.target     = clone_expr(arena, s->as.use_stmt.target);
            new_s->as.use_stmt.alias_name = clone_id(arena, s->as.use_stmt.alias_name);
            break;
        case STMT_RETURN:
            new_s->as.return_stmt.value = clone_expr(arena, s->as.return_stmt.value);
            break;
        case STMT_UNSAFE:
            new_s->as.unsafe_stmt.body = clone_stmt_list(arena, s->as.unsafe_stmt.body);
            break;
        case STMT_DEFER:
            new_s->as.defer_stmt.stmt = clone_stmt(arena, s->as.defer_stmt.stmt);
            break;
        case STMT_COMPTIME_IF:
            new_s->as.comptime_if_stmt.cond = clone_expr(arena, s->as.comptime_if_stmt.cond);
            new_s->as.comptime_if_stmt.then_branch = clone_stmt_list(arena, s->as.comptime_if_stmt.then_branch);
            new_s->as.comptime_if_stmt.else_branch = clone_stmt_list(arena, s->as.comptime_if_stmt.else_branch);
            break;
        case STMT_CONTINUE:
        case STMT_BREAK:
            break;
    }
    return new_s;
}

Decl *clone_decl(Arena *arena, Decl *d) {
    if (!d) return NULL;
    Decl *new_d = arena_push_aligned(arena, Decl);
    *new_d = *d;

    switch (d->kind) {
        case DECL_VARIABLE:
            new_d->as.variable_decl.name = clone_id(arena, d->as.variable_decl.name);
            new_d->as.variable_decl.type = clone_type(arena, d->as.variable_decl.type);
            new_d->as.variable_decl.in_field = clone_id(arena, d->as.variable_decl.in_field);
            new_d->as.variable_decl.constraints = clone_expr_list(arena, d->as.variable_decl.constraints);
            break;
        case DECL_FUNCTION:
        case DECL_PROCEDURE:
        case DECL_EXTERN_FUNCTION:
        case DECL_EXTERN_PROCEDURE:
            new_d->as.function_decl.name = clone_id(arena, d->as.function_decl.name);
            new_d->as.function_decl.params = clone_decl_list(arena, d->as.function_decl.params);
            new_d->as.function_decl.return_type = clone_type(arena, d->as.function_decl.return_type);
            new_d->as.function_decl.body = clone_stmt_list(arena, d->as.function_decl.body);
            new_d->as.function_decl.pre_contracts = clone_expr_list(arena, d->as.function_decl.pre_contracts);
            new_d->as.function_decl.post_contracts = clone_expr_list(arena, d->as.function_decl.post_contracts);
            new_d->as.function_decl.return_constraints = clone_expr_list(arena, d->as.function_decl.return_constraints);
            break;
        case DECL_STRUCT:
            new_d->as.struct_decl.name = clone_id(arena, d->as.struct_decl.name);
            new_d->as.struct_decl.fields = clone_decl_list(arena, d->as.struct_decl.fields);
            break;
        case DECL_ENUM:
            new_d->as.enum_decl.type_name = clone_id(arena, d->as.enum_decl.type_name);
            new_d->as.enum_decl.variants = clone_variant(arena, d->as.enum_decl.variants);
            break;
        case DECL_IMPORT:
        case DECL_EVAL_IMPORT:
            new_d->as.import_decl.module_name = clone_id(arena, d->as.import_decl.module_name);
            break;
        case DECL_C_INCLUDE:
            // Constant string pointer clone semantics
            break;
        case DECL_DESTRUCT:
            new_d->as.destruct_decl.names = clone_id_list(arena, d->as.destruct_decl.names);
            new_d->as.destruct_decl.type = clone_type(arena, d->as.destruct_decl.type);
            break;
        case DECL_EXTERN_TYPE:
            new_d->as.extern_type_decl.name = clone_id(arena, d->as.extern_type_decl.name);
            break;
        case DECL_TYPE_ALIAS:
            new_d->as.type_alias_decl.name = clone_id(arena, d->as.type_alias_decl.name);
            new_d->as.type_alias_decl.expr = clone_expr(arena, d->as.type_alias_decl.expr);
            break;
    }
    return new_d;
}

#endif // AST_CLONE_H
