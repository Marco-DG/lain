 
#ifndef PARSER_DECL_H
#define PARSER_DECL_H

#include "../parser.h"

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
DeclList* parse_type_fields(Arena *arena, Parser *parser, bool *is_enum, IdList **enum_values);

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

    return NULL;
}


DeclList *parse_type_fields(Arena* arena, Parser* parser, bool *is_enum, IdList **enum_values) {
    DeclList* struct_fields = NULL;
    DeclList** struct_tail = &struct_fields;
    *enum_values = NULL;
    IdList** enum_tail = enum_values;
    *is_enum = true;

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
        Id *field_name = id(arena, parser->token.length, parser->token.start);
        parser_advance();

        if (parser_match(TOKEN_IDENTIFIER) || parser_match(TOKEN_KEYWORD_MOV) || parser_match(TOKEN_KEYWORD_COMPTIME)) {
            /* Struct field: parse its type */
            *is_enum = false;
            Type *field_type = parse_type(arena, parser);
            if (field_is_comptime && field_type) {
                field_type = type_comptime(arena, field_type);
            }

            /* Create the Decl for this field */
            Decl *var_decl = decl_variable(arena, field_name, field_type);

            /* --- NEW: optional `in <identifier>` annotation --- */
            if (parser_match(TOKEN_KEYWORD_IN)) {
                parser_advance(); // consume 'in'
                parser_expect(TOKEN_IDENTIFIER, "Expected identifier after 'in'");
                Id *container_name = id(arena, parser->token.length, parser->token.start);
                parser_advance(); // consume the identifier
                /* Attach the container identifier to the variable decl.
                   This requires `Decl->as.variable_decl.in_field` (see instructions). */
                var_decl->as.variable_decl.in_field = container_name;
            }

            /* Append to struct_fields list */
            *struct_tail = decl_list(arena, var_decl);
            struct_tail = &(*struct_tail)->next;
        } else {
            /* Enum value: we don't accept 'comptime' here */
            if (field_is_comptime) {
                parser_error("Enum value cannot be marked 'comptime'");
            }
            *enum_tail = id_list(arena, field_name);
            enum_tail = &(*enum_tail)->next;
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
    IdList* enum_values;
    DeclList* struct_fields = parse_type_fields(arena, parser, &is_enum, &enum_values);

    parser_expect(TOKEN_R_BRACE, "Expected '}' at end of type definition");
    parser_advance();

    if (is_enum) {
        return decl_enum(arena, name, enum_values);
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
            bool is_move = false;
            bool is_comptime = false;
            if (parser_match(TOKEN_KEYWORD_MOV)) {
                parser_advance(); // consume 'mov'
                is_move = true;
            } else if (parser_match(TOKEN_KEYWORD_COMPTIME)) {
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
                // Normal parameter: name Type
                parser_expect(TOKEN_IDENTIFIER, "Expected parameter name");
                Id *pname = id(arena, parser->token.length, parser->token.start);
                parser_advance();

                // parameter type
                Type *ptype = parse_type(arena, parser);

                // wrap in TYPE_MOVE safely
                if (is_move) {
                    ptype = type_move(arena, ptype);
                }
                // wrap in TYPE_COMPTIME safely
                if (is_comptime) {
                    ptype = type_comptime(arena, ptype);
                }

                Decl *pdecl = decl_variable(arena, pname, ptype);

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
    Type *ret_type = parse_type(arena, parser);
    if (ret_is_comptime && ret_type) {
        ret_type = type_comptime(arena, ret_type);
    }

    // function body
    parser_expect(TOKEN_L_BRACE, "Expected '{' after signature");
    parser_advance();

    StmtList *body = parse_stmt_list(arena, parser);

    parser_expect(TOKEN_R_BRACE, "Expected '}' at end of body");
    parser_advance();

    if (is_proc) {
        return decl_procedure(arena, func_name, params, ret_type, body, false);
    } else {
        return decl_function(arena, func_name, params, ret_type, body, false);
    }
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
    Type *ret_type = parse_type(arena, parser);
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