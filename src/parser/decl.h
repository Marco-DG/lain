 
#ifndef PARSER_DECL_H
#define PARSER_DECL_H

#include "../parser.h"

// Helper: Check if token is a comparison operator (for equation-style constraints)
static bool is_comparison_op(TokenKind kind) {
    switch (kind) {
        case TOKEN_ANGLE_BRACKET_LEFT:
        case TOKEN_ANGLE_BRACKET_RIGHT:
        case TOKEN_ANGLE_BRACKET_LEFT_EQUAL:
        case TOKEN_ANGLE_BRACKET_RIGHT_EQUAL:
        case TOKEN_EQUAL_EQUAL:
        case TOKEN_BANG_EQUAL:
            return true;
        default:
            return false;
    }
}

// entry‑point for a module
DeclList* parse_module(Arena *arena, Parser *parser);

// top‑level declarations
DeclList* parse_decl_list(Arena *arena, Parser *parser);
Decl *   parse_decl(Arena *arena, Parser *parser);
Decl *   parse_var_decl(Arena *arena, Parser *parser);
Decl *   parse_func_decl(Arena *arena, Parser *parser);
Decl *   parse_proc_decl(Arena *arena, Parser *parser); // New
Decl *   parse_extern_func_decl(Arena *arena, Parser *parser);
Decl *   parse_extern_proc_decl(Arena *arena, Parser *parser); // New
Decl *   parse_type_decl(Arena *arena, Parser *parser);
Decl *   parse_import_decl(Arena *arena, Parser *parser);

// helper for type fields (struct vs enum)
DeclList* parse_type_fields(Arena *arena, Parser *parser, bool *is_enum, Variant **adt_variants);

DeclList *parse_module(Arena* arena, Parser* parser) {
    //parser_skip_whitespace();
    parser_skip_eol();
    
    DeclList*  list = NULL;
    DeclList** list_tail = &list;
    
    while (!parser_match(TOKEN_EOF)) {
        Decl* decl = parse_decl(arena, parser);
        if (!decl) {
            fprintf(stderr, "Error: Unexpected token at top level: %s\n", token_kind_name(parser->token.kind));
            parser_advance(); // consume to avoid infinite loop
            continue;
        }

        *list_tail = decl_list(arena, decl);
        list_tail = &(*list_tail)->next;
        
        parser_skip_eol();
    }

    return list;
}


DeclList* parse_decl_list(Arena* arena, Parser* parser)
{
    DeclList*  list = NULL;
    DeclList** list_tail = &list;
    
    while (!parser_match(TOKEN_R_PAREN)) {
        //parser_advance();
        Decl* decl = parse_decl(arena, parser);

        *list_tail = decl_list(arena, decl);
        list_tail = &(*list_tail)->next;

        if (parser_match(TOKEN_COMMA)) {
            parser_advance();
        } else {
            break;
        }
    }

    return list;
}

Decl *parse_decl(Arena* arena, Parser* parser)
{
    if (parser_match(TOKEN_KEYWORD_IMPORT))
    {
        parser_advance();
        return parse_import_decl(arena, parser);
    }

    if (parser_match(TOKEN_KEYWORD_TYPE))
    {
        parser_advance();
        return parse_type_decl(arena, parser);
    }

    // extern func …
    if (parser_match(TOKEN_KEYWORD_EXTERN)) {
        parser_advance();  // consume 'extern'
        if (parser_match(TOKEN_KEYWORD_FUNC)) {
            parser_advance();  // consume 'func'
            return parse_extern_func_decl(arena, parser);
        }
        if (parser_match(TOKEN_KEYWORD_PROC)) {
            parser_advance();  // consume 'proc'
            return parse_extern_proc_decl(arena, parser);
        }
        parser_expect(TOKEN_KEYWORD_FUNC, "Expected 'func' or 'proc' after 'extern'");
        return NULL;
    }

    if (parser_match(TOKEN_KEYWORD_FUNC)) {
        parser_advance();
        return parse_func_decl(arena, parser);
    }

    if (parser_match(TOKEN_KEYWORD_PROC)) {
        parser_advance();
        return parse_proc_decl(arena, parser);
    }

    if (parser_match(TOKEN_KEYWORD_VAR)) {
        parser_advance();
        return parse_var_decl(arena, parser);
    }

    return NULL;
}


// helper for type fields (struct vs enum/ADT)
DeclList* parse_type_fields(Arena *arena, Parser *parser, bool *is_enum, Variant **adt_variants) {
    DeclList* struct_fields = NULL;
    DeclList** struct_tail = &struct_fields;
    *adt_variants = NULL;
    Variant** variant_tail = adt_variants;
    *is_enum = false; // Default to struct, switch to enum if we see variants

    /* Skip any leading blank lines before the first field/value */
    parser_skip_eol();

    /* Loop until we hit '}' or EOF */
    while (!parser_match(TOKEN_R_BRACE) && !parser_match(TOKEN_EOF)) {
        /* Skip blank lines before each entry */
        parser_skip_eol();
        if (parser_match(TOKEN_R_BRACE)) break;

        /* Accept optional 'comptime' prefix for a field */
        bool field_is_comptime = false;
        if (parser_match(TOKEN_KEYWORD_COMPTIME)) {
            field_is_comptime = true;
            parser_advance(); // consume 'comptime'
        }

        /* Must start with an identifier (field name or enum value) */
        parser_expect(TOKEN_IDENTIFIER, "Expected field name or enum value");
        Id *name = id(arena, parser->token.length, parser->token.start);
        parser_advance();

        // Lookahead to distinguish:
        // 1. Name Type -> Struct Field
        // 2. Name { ... } -> ADT Variant
        // 3. Name -> Enum Variant (if followed by separator or '}')
        
        bool is_struct_field = false;

        if (parser_match(TOKEN_L_BRACE)) {
            // Case 2: ADT Variant with fields
            // is_adt_variant = true;
        } else if (parser_match(TOKEN_IDENTIFIER) || parser_match(TOKEN_KEYWORD_MOV) || parser_match(TOKEN_KEYWORD_MUT) || parser_match(TOKEN_KEYWORD_COMPTIME) || parser_match(TOKEN_L_BRACKET) || parser_match(TOKEN_ASTERISK)) {
            // Case 1: Struct Field (followed by Type start tokens)
            is_struct_field = true;
        } else {
            // Case 3: Simple Enum Variant
            // is_adt_variant = true;
        }

        if (is_struct_field) {
            if (*is_enum) {
                parser_error("Cannot mix struct fields and enum variants in the same type");
            }
            
            /* Struct field: parse its type */
            Type *field_type = parse_type(arena, parser);
            if (field_is_comptime && field_type) {
                field_type = type_comptime(arena, field_type);
            }

            /* Create the Decl for this field */
            Decl *var_decl = decl_variable(arena, name, field_type);

            /* --- NEW: optional `in <identifier>` annotation --- */
            if (parser_match(TOKEN_KEYWORD_IN)) {
                parser_advance(); // consume 'in'
                parser_expect(TOKEN_IDENTIFIER, "Expected identifier after 'in'");
                Id *container_name = id(arena, parser->token.length, parser->token.start);
                parser_advance(); // consume the identifier
                var_decl->as.variable_decl.in_field = container_name;
            }

            /* Append to struct_fields list */
            *struct_tail = decl_list(arena, var_decl);
            struct_tail = &(*struct_tail)->next;
        } else {
            // ADT/Enum Variant
            *is_enum = true;
            if (struct_fields != NULL) {
                 parser_error("Cannot mix struct fields and enum variants in the same type");
            }
            
            DeclList *variant_fields = NULL;
            
            if (parser_match(TOKEN_L_BRACE)) {
                parser_advance(); // consume '{'
                
                // Parse variant fields: Name Type, ...
                DeclList** vfields_tail = &variant_fields;
                
                while (!parser_match(TOKEN_R_BRACE) && !parser_match(TOKEN_EOF)) {
                    parser_skip_eol();
                    if (parser_match(TOKEN_R_BRACE)) break;
                    
                    parser_expect(TOKEN_IDENTIFIER, "Expected variant field name");
                    Id *fname = id(arena, parser->token.length, parser->token.start);
                    parser_advance();
                    
                    Type *ftype = parse_type(arena, parser);
                    Decl *fdecl = decl_variable(arena, fname, ftype);
                    
                    *vfields_tail = decl_list(arena, fdecl);
                    vfields_tail = &(*vfields_tail)->next;
                    
                    if (parser_match(TOKEN_COMMA)) {
                        parser_advance();
                    } else if (parser_match(TOKEN_R_BRACE)) {
                        break;
                    } else {
                        // Optional newline separator?
                        if (parser_match(TOKEN_EOL)) {
                             parser_skip_eol();
                        } else {
                             parser_expect(TOKEN_COMMA, "Expected ',' after variant field");
                        }
                    }
                }
                
                parser_expect(TOKEN_R_BRACE, "Expected '}' after variant fields");
                parser_advance();
            }
            
            Variant *v = variant(arena, name, variant_fields);
            *variant_tail = v;
            variant_tail = &(*variant_tail)->next;
        }

        /* --- Separator handling (robust ordering) --- */

        /* 1) explicit comma -> eat it and continue (then skip any newlines) */
        if (parser_match(TOKEN_COMMA)) {
            parser_advance();
            parser_skip_eol();
            continue;
        }

        /* 2) explicit semicolon -> eat it, skip EOLs, allow immediate '}' after that */
        if (parser_match(TOKEN_SEMICOLON)) {
            parser_advance();
            parser_skip_eol();
            if (parser_match(TOKEN_R_BRACE) || parser_match(TOKEN_EOF)) break;
            continue;
        }

        /* 3) explicit newline / comment separators -> consume them and continue */
        if (parser_match(TOKEN_EOL) ||
            parser_match(TOKEN_LINE_COMMENT) ||
            parser_match(TOKEN_MULTILINE_COMMENT))
        {
            parser_skip_eol();
            if (parser_match(TOKEN_R_BRACE) || parser_match(TOKEN_EOF)) break;
            continue;
        }

        /* 4) some token remains that is not a valid separator */
        parser_error("Expected ',', newline, or ';' after field or enum value");
        return struct_fields;
    }

    return struct_fields;
}


Decl* parse_type_decl(Arena* arena, Parser* parser) {
    parser_expect(TOKEN_IDENTIFIER, "Expected type name");
    Id* name = id(arena, parser->token.length, parser->token.start);
    parser_advance();

    // allow the '{' to be on the next line
    parser_skip_eol();

    parser_expect(TOKEN_L_BRACE, "Expected '{' after type name");
    parser_advance();

    bool is_enum;
    Variant* adt_variants;
    DeclList* struct_fields = parse_type_fields(arena, parser, &is_enum, &adt_variants);

    parser_expect(TOKEN_R_BRACE, "Expected '}' at end of type definition");
    parser_advance();

    if (is_enum) {
        return decl_enum(arena, name, adt_variants);
    } else {
        return decl_struct(arena, name, struct_fields);
    }
}

// var <name> <type>
Decl *parse_var_decl(Arena* arena, Parser* parser)
{
    parser_expect(TOKEN_IDENTIFIER, "Expected variable name");
    Id *var_name = id(arena, parser->token.length, parser->token.start);
    parser_advance();

    Type *var_type = parse_type(arena, parser);
    return decl_variable(arena, var_name, var_type);
}

// func <name>(<params>) <return_type> { <body> }
Decl *parse_func_proc_decl_impl(Arena* arena, Parser* parser, bool is_proc) {
    // function name
    parser_expect(TOKEN_IDENTIFIER, "Expected function/procedure name");
    Id *func_name = id(arena, parser->token.length, parser->token.start);
    parser_advance();

    // parameter list
    parser_expect(TOKEN_L_PAREN, "Expected '(' after name");
    parser_advance();

    DeclList *params = NULL;
    DeclList **tail = &params;

    if (!parser_match(TOKEN_R_PAREN)) {
        do {
            bool is_comptime = false;
            if (parser_match(TOKEN_KEYWORD_COMPTIME)) {
                parser_advance(); // consume 'comptime'
                is_comptime = true;
            }

            // parameter name
            // Check for destructuring: {a, b} Type
            if (parser_match(TOKEN_L_BRACE)) {
                parser_advance(); // consume '{'
                
                IdList* names = NULL;
                IdList** names_tail = &names;

                // Parse identifiers inside {}
                do {
                    parser_expect(TOKEN_IDENTIFIER, "Expected field name in destructuring");
                    Id* field_name = id(arena, parser->token.length, parser->token.start);
                    parser_advance();

                    *names_tail = id_list(arena, field_name);
                    names_tail = &(*names_tail)->next;

                    if (parser_match(TOKEN_COMMA)) {
                        parser_advance();
                    } else {
                        break;
                    }
                } while (true);

                parser_expect(TOKEN_R_BRACE, "Expected '}' after destructuring list");
                parser_advance();

                // parameter type
                Type *ptype = parse_type(arena, parser);
                
                // Create DECL_DESTRUCT
                Decl *pdecl = decl_destruct(arena, names, ptype);
                
                *tail = decl_list(arena, pdecl);
                tail  = &(*tail)->next;

            } else {
                // Normal parameter: [mov|mut] name Type
                // Check for ownership mode prefix
                OwnershipMode param_mode = MODE_SHARED;  // default
                if (parser_match(TOKEN_KEYWORD_MOV)) {
                    parser_advance();
                    param_mode = MODE_OWNED;
                } else if (parser_match(TOKEN_KEYWORD_MUT)) {
                    parser_advance();
                    param_mode = MODE_MUTABLE;
                }

                parser_expect(TOKEN_IDENTIFIER, "Expected parameter name");
                Id *pname = id(arena, parser->token.length, parser->token.start);
                parser_advance();

                // parameter type
                Type *ptype = parse_type(arena, parser);
                
                // Apply ownership mode to the type
                if (ptype) {
                    ptype->mode = param_mode;
                }

                // wrap in TYPE_COMPTIME safely
                if (is_comptime) {
                    ptype = type_comptime(arena, ptype);
                }

                Decl *pdecl = decl_variable(arena, pname, ptype);
                pdecl->as.variable_decl.is_parameter = true;

                // Check for 'in' keyword: param int in arr
                if (parser_match(TOKEN_KEYWORD_IN)) {
                    parser_advance();
                    parser_expect(TOKEN_IDENTIFIER, "Expected array name after 'in'");
                    pdecl->as.variable_decl.in_field = id(arena, parser->token.length, parser->token.start);
                    parser_advance();
                }
                
                // Parse equation-style constraints: param int != 0, param int >= 0 and <= 100
                if (is_comparison_op(parser->token.kind)) {
                    ExprList *constraints = NULL;
                    ExprList **ctail = &constraints;
                    
                    // Create expression for the parameter name (LHS of constraint)
                    Expr *param_expr = expr_identifier(arena, pname);
                    
                    do {
                        TokenKind op = parser->token.kind;
                        parser_advance();  // consume operator
                        
                        // Parse the RHS (literal or identifier)
                        Expr *rhs = NULL;
                        if (parser_match(TOKEN_NUMBER)) {
                            int value = atoi(parser->token.start);
                            parser_advance();
                            rhs = expr_literal(arena, value);
                        } else if (parser_match(TOKEN_IDENTIFIER)) {
                            Id *rhs_id = id(arena, parser->token.length, parser->token.start);
                            parser_advance();
                            rhs = expr_identifier(arena, rhs_id);
                        } else {
                            parser_error("Expected number or identifier after comparison operator");
                        }
                        
                        // Create binary constraint expression
                        Expr *constraint = expr_binary(arena, op, param_expr, rhs);
                        *ctail = expr_list(arena, constraint);
                        ctail = &(*ctail)->next;
                        
                        // Check for 'and' to chain more constraints
                        if (parser_match(TOKEN_KEYWORD_AND)) {
                            parser_advance();
                            if (!is_comparison_op(parser->token.kind)) {
                                parser_error("Expected comparison operator after 'and'");
                            }
                        } else {
                            break;
                        }
                    } while (is_comparison_op(parser->token.kind));
                    
                    pdecl->as.variable_decl.constraints = constraints;
                }

                *tail = decl_list(arena, pdecl);
                tail  = &(*tail)->next;
            }

            if (parser_match(TOKEN_COMMA)) {
                parser_advance();
            } else {
                break;
            }
        } while (true);
    }

    parser_expect(TOKEN_R_PAREN, "Expected ')' after parameters");
    parser_advance();

    // --- return type ---
    bool ret_is_comptime = false;
    if (parser_match(TOKEN_KEYWORD_COMPTIME)) {
        parser_advance();
        ret_is_comptime = true;
    }
    
    Type *ret_type = NULL;
    if (ret_is_comptime || parser_match(TOKEN_IDENTIFIER) || parser_match(TOKEN_KEYWORD_MOV)) {
        ret_type = parse_type(arena, parser);
    }

    if (ret_is_comptime && ret_type) {
        ret_type = type_comptime(arena, ret_type);
    }
    
    // --- return type constraints (equation-style): int >= 0, int >= lo and <= hi ---
    ExprList *return_constraints = NULL;
    if (ret_type && is_comparison_op(parser->token.kind)) {
        ExprList **rc_tail = &return_constraints;
        
        // Create 'result' identifier for LHS of constraint
        Id *result_id = id(arena, 6, "result");
        Expr *result_expr = expr_identifier(arena, result_id);
        
        do {
            TokenKind op = parser->token.kind;
            parser_advance();  // consume operator
            
            // Parse the RHS (literal or identifier)
            Expr *rhs = NULL;
            if (parser_match(TOKEN_NUMBER)) {
                int value = atoi(parser->token.start);
                parser_advance();
                rhs = expr_literal(arena, value);
            } else if (parser_match(TOKEN_IDENTIFIER)) {
                Id *rhs_id = id(arena, parser->token.length, parser->token.start);
                parser_advance();
                rhs = expr_identifier(arena, rhs_id);
            } else {
                parser_error("Expected number or identifier after comparison operator in return constraint");
            }
            
            // Create binary constraint expression: result op rhs
            Expr *constraint = expr_binary(arena, op, result_expr, rhs);
            *rc_tail = expr_list(arena, constraint);
            rc_tail = &(*rc_tail)->next;
            
            // Check for 'and' to chain more constraints
            if (parser_match(TOKEN_KEYWORD_AND)) {
                parser_advance();
                if (!is_comparison_op(parser->token.kind)) {
                    parser_error("Expected comparison operator after 'and' in return constraint");
                }
            } else {
                break;
            }
        } while (is_comparison_op(parser->token.kind));
    }

    // --- contracts (pre/post) ---
    ExprList *pre_contracts = NULL;
    ExprList **pre_tail = &pre_contracts;
    ExprList *post_contracts = NULL;
    ExprList **post_tail = &post_contracts;

    while (true) {
        parser_skip_eol(); // Skip newlines before checking for contracts
        if (parser_match(TOKEN_KEYWORD_PRE)) {
            parser_advance();
            Expr *e = parse_expr(arena, parser);
            *pre_tail = expr_list(arena, e);
            pre_tail = &(*pre_tail)->next;
            parser_skip_eol();
        } else if (parser_match(TOKEN_KEYWORD_POST)) {
            parser_advance();
            Expr *e = parse_expr(arena, parser);
            *post_tail = expr_list(arena, e);
            post_tail = &(*post_tail)->next;
            parser_skip_eol();
        } else {
            break;
        }
    }

    // function body
    parser_expect(TOKEN_L_BRACE, "Expected '{' after signature");
    parser_advance();

    StmtList *body = parse_stmt_list(arena, parser);

    parser_expect(TOKEN_R_BRACE, "Expected '}' at end of body");
    parser_advance();

    Decl *d;
    if (is_proc) {
        d = decl_procedure(arena, func_name, params, ret_type, body, false);
    } else {
        d = decl_function(arena, func_name, params, ret_type, body, false);
    }
    d->as.function_decl.pre_contracts = pre_contracts;
    d->as.function_decl.post_contracts = post_contracts;
    d->as.function_decl.return_constraints = return_constraints;
    return d;
}

Decl *parse_func_decl(Arena* arena, Parser* parser) {
    return parse_func_proc_decl_impl(arena, parser, false);
}

Decl *parse_proc_decl(Arena* arena, Parser* parser) {
    return parse_func_proc_decl_impl(arena, parser, true);
}



// extern func <name>(<params>) <return> ;
Decl *parse_extern_func_proc_decl_impl(Arena *arena, Parser *parser, bool is_proc) {
    // name
    parser_expect(TOKEN_IDENTIFIER, "Expected function/procedure name");
    Id *func_name = id(arena, parser->token.length, parser->token.start);
    parser_advance();

    // parameters
    parser_expect(TOKEN_L_PAREN, "Expected '(' after name");
    parser_advance();

    DeclList *params = NULL;
    DeclList **tail = &params;

    if (!parser_match(TOKEN_R_PAREN)) {
        do {
            // Check for varargs "..."
            if (parser_match(TOKEN_DOT_DOT) && lexer_peek(parser->lexer).kind == TOKEN_DOT) {
                 // consume ".." then "."
                 parser_advance(); 
                 parser_advance();
                 break;
            }

            // parameter name
            parser_expect(TOKEN_IDENTIFIER, "Expected parameter name");
            Id *pname = id(arena, parser->token.length, parser->token.start);
            parser_advance();

            // parameter type
            Type *ptype = parse_type(arena, parser);

            Decl *pdecl = decl_variable(arena, pname, ptype);
            pdecl->as.variable_decl.is_parameter = true;
            *tail = decl_list(arena, pdecl);
            tail = &(*tail)->next;

            if (parser_match(TOKEN_COMMA)) {
                parser_advance();
            } else {
                break;
            }
        } while (true);
    }
    
    parser_expect(TOKEN_R_PAREN, "Expected ')' after parameters");
    parser_advance();

    // return type: accept optional 'comptime' prefix
    bool ret_is_comptime = false;
    if (parser_match(TOKEN_KEYWORD_COMPTIME)) {
        parser_advance();
        ret_is_comptime = true;
    }
    
    Type *ret_type = NULL;
    if (ret_is_comptime || parser_match(TOKEN_IDENTIFIER) || parser_match(TOKEN_KEYWORD_MOV)) {
        ret_type = parse_type(arena, parser);
    }

    if (ret_is_comptime && ret_type) {
        ret_type = type_comptime(arena, ret_type);
    }

    // require end-of-decl (newline or semicolon)
    parser_expect_eol("Expected ';' or newline after extern decl");
    parser_advance();

    // NULL body signals extern
    if (is_proc) {
        return decl_procedure(arena, func_name, params, ret_type, /*body=*/NULL, true);
    } else {
        return decl_function(arena, func_name, params, ret_type, /*body=*/NULL, true);
    }
}

Decl *parse_extern_func_decl(Arena *arena, Parser *parser) {
    return parse_extern_func_proc_decl_impl(arena, parser, false);
}

Decl *parse_extern_proc_decl(Arena *arena, Parser *parser) {
    return parse_extern_func_proc_decl_impl(arena, parser, true);
}


// parser.h (below your other parse_* declarations)
Decl *parse_import_decl(Arena* arena, Parser* parser) {
    // Start with first identifier
    parser_expect(TOKEN_IDENTIFIER, "Expected module name after import");
    Token start = parser->token;
    parser_advance();

    // Loop to collect dotted path
    Token end = start;
    while (parser_match(TOKEN_DOT)) {
        parser_advance();  // consume dot
        parser_expect(TOKEN_IDENTIFIER, "Expected identifier after '.'");
        end = parser->token;
        parser_advance();
    }

    // Splice tokens together to form "foo.bar" from start to end
    size_t len = (end.start + end.length) - start.start;
    Id* mod = id(arena, len, start.start);

    return decl_import(arena, mod);
}

#endif // PARSER_DECL_H