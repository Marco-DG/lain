#ifndef AST_PRINT_H
#define AST_PRINT_H

#include "ast.h"  // Or whatever defines Decl, Stmt, Expr, Type, etc.

void print_ast(DeclList* decl_list, int depth);
void decl_print_ast(Decl *decl, int depth);
void stmt_print_ast(Stmt *stmt, int depth);
void expr_print_ast(Expr *expr, int depth);
void print_type(Type *type);


/* Helper functions for printing AST */

void indent(int depth) {
    for (int i = 0; i < depth; i++) {
        printf("  ");
    }
}

void print_type(Type *type) {
    if (!type) return;

    switch (type->kind) {
        case TYPE_SIMPLE:
            printf("%.*s",
                   (int)type->base_type->length,
                   type->base_type->name);
            break;

        case TYPE_ARRAY:
            // e.g. u8[]
            print_type(type->element_type);
            printf("[]");
            break;

        case TYPE_SLICE:
            // e.g. u8[:"XYZ"] or u8[:0]
            print_type(type->element_type);
            if (type->sentinel_len > 0) {
                if (type->sentinel_is_string) {
                    // string sentinel
                    printf("[:\"%.*s\"]",
                           (int)type->sentinel_len,
                           type->sentinel_str);
                } else {
                    // numeric sentinel
                    printf("[:%.*s]",
                           (int)type->sentinel_len,
                           type->sentinel_str);
                }
            } else {
                // no sentinel at all
                printf("[:]");
            }
            break;

        default:
            printf("<unknown type>");
            break;
    }
}





void print_ast(DeclList* decl_list, int depth) {
    while (decl_list) {
        decl_print_ast(decl_list->decl, depth);
        decl_list = decl_list->next;
    }
}

void expr_print_ast(Expr *expr, int depth) {
    if (!expr) return;

    indent(depth);
    switch (expr->kind) {
        case EXPR_LITERAL:
            printf("Literal: %d\n", expr->as.literal_expr.value);
            break;
        case EXPR_CHAR:
            printf("Char Literal: '%c'\n", expr->as.char_expr.value);
            break;
        case EXPR_STRING:
            printf("String Literal: \"%.*s\"\n", (int)expr->as.string_expr.length,
                   expr->as.string_expr.value);
            break;
        case EXPR_IDENTIFIER:
            printf("Identifier: %.*s\n", 
                   (int)expr->as.identifier_expr.id->length, 
                   expr->as.identifier_expr.id->name);
            break;
        case EXPR_BINARY:
            printf("Binary Expression: %s\n", token_kind_to_str(expr->as.binary_expr.op));
            expr_print_ast(expr->as.binary_expr.left, depth + 1);
            expr_print_ast(expr->as.binary_expr.right, depth + 1);
            break;
        case EXPR_UNARY:
            printf("Unary Expression: %s\n", token_kind_to_str(expr->as.unary_expr.op));
            expr_print_ast(expr->as.unary_expr.right, depth + 1);
            break;
        case EXPR_MEMBER:
            printf("Member Access:\n");
            expr_print_ast(expr->as.member_expr.target, depth + 1);
            indent(depth + 1);
            printf("Field: %.*s\n",
                   (int)expr->as.member_expr.member->length,
                   expr->as.member_expr.member->name);
            break;
        case EXPR_CALL:
            printf("Function Call:\n");
            indent(depth + 1);
            printf("Callee:\n");
            expr_print_ast(expr->as.call_expr.callee, depth + 2);
            indent(depth + 1);
            printf("Arguments:\n");
            {
                ExprList* arg = expr->as.call_expr.args;
                while (arg) {
                    expr_print_ast(arg->expr, depth + 2);
                    arg = arg->next;
                }
            }
            break;
        case EXPR_RANGE:
            printf("Range%s\n", expr->as.range_expr.inclusive
                                 ? " (inclusive)" : "");
            indent(depth+1);
            printf("Start:\n");
            expr_print_ast(expr->as.range_expr.start, depth+2);
            indent(depth+1);
            printf("End:\n");
            expr_print_ast(expr->as.range_expr.end, depth+2);
            break;
            case EXPR_INDEX: {
                Expr *target = expr->as.index_expr.target;
                Expr *idx    = expr->as.index_expr.index;
                if (idx->kind == EXPR_RANGE) {
                    // This is a slice operation
                    printf("Slice:\n");
                    indent(depth+1); printf("Target:\n");
                    expr_print_ast(target, depth+2);
    
                    indent(depth+1); printf("Start:\n");
                    expr_print_ast(idx->as.range_expr.start, depth+2);
    
                    // Only print End if there *is* one (for (…)..… ranges)
                    if (idx->as.range_expr.end) {
                        indent(depth+1); printf("End:\n");
                        expr_print_ast(idx->as.range_expr.end, depth+2);
                    }
                } else {
                    // Plain indexing
                    printf("Index:\n");
                    indent(depth+1); printf("Target:\n");
                    expr_print_ast(target, depth+2);
    
                    indent(depth+1); printf("Index:\n");
                    expr_print_ast(idx, depth+2);
                }
                break;
            }
    
        default:
            printf("/* Unhandled expression type %d */\n", expr->kind);
            break;
    }
}


void stmt_print_ast(Stmt *stmt, int depth) {
    if (!stmt) return;

    indent(depth);
    switch (stmt->kind) {

    case STMT_USE: {
        // use <path> as <alias>
        printf("Use Directive:\n");
        // Target path
        indent(depth+1);
        printf("Target:\n");
        expr_print_ast(stmt->as.use_stmt.target, depth+2);
        // Alias name
        indent(depth+1);
        printf("Alias: %.*s\n",
            (int)stmt->as.use_stmt.alias_name->length,
            stmt->as.use_stmt.alias_name->name);
        break;
    }

    case STMT_VAR: {
        // 1) header
        indent(depth);
        printf("Variable Declaration:\n");

        // 2) name
        indent(depth + 1);
        printf("Name: %.*s\n",
            (int)stmt->as.var_stmt.name->length,
            stmt->as.var_stmt.name->name);

        // 3) type annotation (if present)
        if (stmt->as.var_stmt.type) {
            indent(depth + 1);
            printf("Type: ");
            print_type(stmt->as.var_stmt.type);
            printf("\n");
        }

        // 4) initializer (if present)
        if (stmt->as.var_stmt.expr) {
            indent(depth + 1);
            printf("Initializer:\n");
            expr_print_ast(stmt->as.var_stmt.expr, depth + 2);
        }
        break;
    }

    case STMT_FOR: {
        // for [i,] value in iterable { ... }
        printf("For Loop:\n");

        // optional index
        if (stmt->as.for_stmt.index_name) {
            indent(depth+1);
            printf("Index: %.*s\n",
                (int)stmt->as.for_stmt.index_name->length,
                stmt->as.for_stmt.index_name->name);
        }

        // value variable
        indent(depth+1);
        printf("Value: %.*s\n",
            (int)stmt->as.for_stmt.value_name->length,
            stmt->as.for_stmt.value_name->name);

        // iterable expression
        indent(depth+1);
        printf("Iterable:\n");
        expr_print_ast(stmt->as.for_stmt.iterable, depth+2);

        // body
        indent(depth+1);
        printf("Body:\n");
        for (StmtList *sl = stmt->as.for_stmt.body; sl; sl = sl->next) {
            stmt_print_ast(sl->stmt, depth+2);
        }
        break;
    }

    case STMT_CONTINUE:
        indent(depth);
        printf("Continue Statement\n");
        break;

    case STMT_BREAK:
        indent(depth);
        printf("Break Statement\n");
        break;

    case STMT_MATCH: {
        // Print the match statement itself
        printf("Match Statement:\n");

        // Scrutinee expression
        indent(depth+1);
        printf("Scrutinee:\n");
        expr_print_ast(stmt->as.match_stmt.value, depth+2);

        // Now each case
        StmtMatchCase *c = stmt->as.match_stmt.cases;
        int case_idx = 0;
        while (c) {
            // Header for this case
            indent(depth+1);
            if (c->pattern) {
                printf("Case %d Pattern:\n", case_idx++);
                expr_print_ast(c->pattern, depth+2);
            } else {
                printf("Case %d Else:\n", case_idx++);
            }

            // Body of this case
            indent(depth+1);
            printf("Body:\n");
            for (StmtList *sl = c->body; sl; sl = sl->next) {
                stmt_print_ast(sl->stmt, depth+2);
            }

            c = c->next;
        }
        break;
    }


    case STMT_ASSIGN:
        printf("Assignment:\n");
        indent(depth + 1);
        printf("LHS:\n");
        expr_print_ast(stmt->as.assign_stmt.target, depth + 2);
        indent(depth + 1);
        printf("RHS:\n");
        expr_print_ast(stmt->as.assign_stmt.expr, depth + 2);
        break;

    case STMT_EXPR:
        printf("Expression Statement:\n");
        expr_print_ast(stmt->as.expr_stmt.expr, depth + 1);
        break;

    case STMT_RETURN:
        printf("Return Statement:\n");
        indent(depth+1);
        printf("Value:\n");
        expr_print_ast(stmt->as.return_stmt.value, depth+2);
        break;

    default:
        printf("/* Unhandled statement type */\n");
        break;
    }
}


void decl_print_ast(Decl *decl, int depth) {
    if (!decl) return;

    indent(depth);
    switch (decl->kind) {
        case DECL_IMPORT:
            Id *mod = decl->as.import_decl.module_name;
            printf("Import: %.*s\n", (int)mod->length, mod->name);
            break;

        case DECL_VARIABLE:
            printf("Variable Declaration: %.*s : ", 
                (int)decl->as.variable_decl.name->length, 
                decl->as.variable_decl.name->name);
            print_type(decl->as.variable_decl.type);
            printf("\n");
            break;

        case DECL_EXTERN_FUNCTION:
        case DECL_FUNCTION:
            printf("Function Declaration: %.*s\n", 
                   (int)decl->as.function_decl.name->length, 
                   decl->as.function_decl.name->name);
            
            DeclList *param_list = decl->as.function_decl.params;
            if (param_list) {
                indent(depth + 1);
                printf("Parameters:\n");
                while (param_list) {
                    indent(depth + 2);
                    printf("%.*s : ", 
                        (int)param_list->decl->as.variable_decl.name->length,
                        param_list->decl->as.variable_decl.name->name);
                    print_type(param_list->decl->as.variable_decl.type);
                    printf("\n");

                    param_list = param_list->next;
                }
            } else {
                indent(depth + 1);
                printf("Parameters: (none)\n");
            }

            if (decl->as.function_decl.pre_contracts) {
                indent(depth + 1);
                printf("Pre-Contracts:\n");
                for (ExprList *c = decl->as.function_decl.pre_contracts; c; c = c->next) {
                    expr_print_ast(c->expr, depth + 2);
                }
            }

            if (decl->as.function_decl.post_contracts) {
                indent(depth + 1);
                printf("Post-Contracts:\n");
                for (ExprList *c = decl->as.function_decl.post_contracts; c; c = c->next) {
                    expr_print_ast(c->expr, depth + 2);
                }
            }

            indent(depth + 1);
            printf("Body:\n");
            StmtList *stmt_list = decl->as.function_decl.body;
            while (stmt_list) {
                stmt_print_ast(stmt_list->stmt, depth + 2);
                stmt_list = stmt_list->next;
            }
            break;
        
        case DECL_STRUCT:
            printf("Struct Declaration: %.*s\n", 
                   (int)decl->as.struct_decl.name->length, 
                   decl->as.struct_decl.name->name);
            
            DeclList *field = decl->as.struct_decl.fields;
            if (!field) {
                indent(depth + 1);
                printf("// Empty struct\n");
            } else {
                while (field) {
                    if (field->decl) {
                        indent(depth + 1);
                        printf("Field: %.*s : ", 
                            (int)field->decl->as.variable_decl.name->length, 
                            field->decl->as.variable_decl.name->name);
                        print_type(field->decl->as.variable_decl.type);
                        printf("\n");

                    } else {
                        indent(depth + 1);
                        printf("/* Warning: NULL struct field */\n");
                    }
                    field = field->next;
                }
            }
            break;
        
        case DECL_ENUM:
            printf("Enum Declaration: %.*s\n",
                   (int)decl->as.enum_decl.type_name->length,
                   decl->as.enum_decl.type_name->name);

            Variant* value = decl->as.enum_decl.variants;
            if (!value) {
                indent(depth + 1);
                printf("// Empty enum\n");
            } else {
                while (value) {
                    if (value->name) {
                        indent(depth + 1);
                        printf("Enum Value: %.*s\n",
                               (int)value->name->length,
                               value->name->name);
                        if (value->fields) {
                            indent(depth + 2);
                            printf("Fields:\n");
                            for (DeclList *f = value->fields; f; f = f->next) {
                                indent(depth + 3);
                                printf("%.*s : ", 
                                    (int)f->decl->as.variable_decl.name->length,
                                    f->decl->as.variable_decl.name->name);
                                print_type(f->decl->as.variable_decl.type);
                                printf("\n");
                            }
                        }
                    } else {
                        indent(depth + 1);
                        printf("/* Warning: NULL enum value */\n");
                    }
                    value = value->next;
                }
            }
            break;
        
        default:
            printf("/* Unhandled declaration type */\n");
            break;
    }
}

#endif // AST_PRINT_H
