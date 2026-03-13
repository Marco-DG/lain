#ifndef PARSER_EXPR_H
#define PARSER_EXPR_H

#include "../parser.h"

// expression entry
Expr *parse_expr(Arena *arena, Parser *parser);

// sub‑levels
Expr *parse_binary_expr(Arena *arena, Parser *parser, int precedence);
Expr *parse_unary_expr(Arena *arena, Parser *parser);
Expr *parse_primary_expr(Arena *arena, Parser *parser);

Expr *parse_expr(Arena* arena, Parser* parser)
{
    isize expr_line = parser->line;
    isize expr_col  = parser->column;
    Expr *result = parse_binary_expr(arena, parser, 0);
    if (result) {
        result->line = expr_line;
        result->col  = expr_col;
    }
    return result;
}

// <expr> <op> <expr>
Expr *parse_binary_expr(Arena *arena, Parser *parser, int precedence) {
    Expr *left = parse_unary_expr(arena, parser);

    // Handle postfix `as` cast immediately after the initial operand
    if (parser_match(TOKEN_KEYWORD_AS)) {
        parser_advance();
        Type *target = parse_type(arena, parser);
        left = expr_cast(arena, left, target);
    }

    while (true) {
        TokenKind op = parser->token.kind;

        int prec = get_precedence(op);
        if (prec < precedence) break;

        parser_advance();  // consume this operator
        Expr *right = parse_binary_expr(arena, parser, prec + 1);
        left = expr_binary(arena, op, left, right);

        // Handle postfix `as` cast after each binary sub-expression
        if (parser_match(TOKEN_KEYWORD_AS)) {
            parser_advance();
            Type *target = parse_type(arena, parser);
            left = expr_cast(arena, left, target);
        }
    }

    return left;
}


// <op> <expr>
Expr *parse_unary_expr(Arena* arena, Parser* parser)
{
    // -, !, &
    if (parser_match(TOKEN_MINUS) || parser_match(TOKEN_BANG) || parser_match(TOKEN_AMPERSAND) || parser_match(TOKEN_TILDE)) {
        TokenKind op = parser->token.kind;
        parser_advance();
        
        Expr *right = parse_unary_expr(arena, parser);
        return expr_unary(arena, op, right);
    }

    // mov <expr>
    if (parser_match(TOKEN_KEYWORD_MOV)) {
        parser_advance();
        Expr *right = parse_unary_expr(arena, parser);
        return expr_move(arena, right);
    }

    // mut <expr>
    // but what about just `*` (dereference)? 
    
    // dereference *<expr>
    if (parser_match(TOKEN_ASTERISK)) {
        TokenKind op = parser->token.kind;
        parser_advance();
        Expr *right = parse_unary_expr(arena, parser);
        return expr_unary(arena, op, right);
    }

    if (parser_match(TOKEN_KEYWORD_VAR)) {
        parser_advance();
        Expr *right = parse_unary_expr(arena, parser);
        return expr_mut(arena, right);
    }
    
    return parse_primary_expr(arena, parser);
}

// literals, identifiers, and parenthesized expressions
Expr *parse_primary_expr(Arena* arena, Parser* parser)
{
    // EXPR_MATCH (case expression)
    if (parser_match(TOKEN_KEYWORD_CASE)) {
        parser_advance();
        // Check for `case &expr` — non-consuming (borrowed) match
        bool is_borrowed = false;
        if (parser_match(TOKEN_AMPERSAND)) {
            is_borrowed = true;
            parser_advance();
        }
        Expr *value = parse_expr(arena, parser);
        
        parser_expect(TOKEN_L_BRACE, "Expected '{' after case expression");
        parser_advance();
        parser_skip_eol();

        ExprMatchCase *first = NULL, **tail = &first;
        ExprList *current_patterns = NULL, **pat_tail = &current_patterns;
        int pending_count = 0;

        while (!parser_match(TOKEN_R_BRACE) && !parser_match(TOKEN_EOF)) {
            if (parser_match(TOKEN_KEYWORD_ELSE)) {
                parser_advance();
                pending_count++;
            } else {
                do {
                    Expr *pattern = NULL;
                    Expr *left = parse_expr(arena, parser);
                    if (parser_match(TOKEN_DOT_DOT) || parser_match(TOKEN_DOT_DOT_EQUAL)) {
                        bool inclusive = parser->token.kind == TOKEN_DOT_DOT_EQUAL;
                        parser_advance();
                        Expr *right = parse_expr(arena, parser);
                        pattern = expr_range(arena, left, right, inclusive);
                    } else {
                        pattern = left;
                    }
                    *pat_tail = expr_list(arena, pattern);
                    pat_tail = &(*pat_tail)->next;
                    pending_count++;
                    if (parser_match(TOKEN_COMMA)) parser_advance();
                    else break;
                } while (true);
            }
            parser_expect(TOKEN_COLON, "Expected ':' after match pattern");
            parser_advance();
            
            Expr *body = parse_expr(arena, parser);
            
            *tail = expr_match_case(arena, current_patterns, body);
            tail = &(*tail)->next;
            
            current_patterns = NULL;
            pat_tail = &current_patterns;
            pending_count = 0;
            
            if (parser_match(TOKEN_COMMA)) parser_advance();
            parser_skip_eol();
        }
        
        if (pending_count > 0) {
            fprintf(stderr, "Error Ln %li, Col %li: match patterns with no body at end of block\n", parser->line, parser->column);
            exit(1);
        }
        
        parser_expect(TOKEN_R_BRACE, "Expected '}' after case expression block");
        parser_advance();
        
        return expr_match(arena, value, first, is_borrowed);
    }

    // Boolean literals
    if (parser_match(TOKEN_KEYWORD_TRUE)) {
        parser_advance();
        return expr_literal(arena, 1);
    }
    if (parser_match(TOKEN_KEYWORD_FALSE)) {
        parser_advance();
        return expr_literal(arena, 0);
    }
    if (parser_match(TOKEN_KEYWORD_UNDEFINED)) {
        parser_advance();
        return expr_undefined(arena);
    }
    
    // Anonymous types: type { ... }
    if (parser_match(TOKEN_KEYWORD_TYPE)) {
        parser_advance(); // consume 'type'
        parser_expect(TOKEN_L_BRACE, "Expected '{' after 'type' expression");
        parser_advance();
        
        // We'll borrow parse_type_fields logic, we just need to declare it extern or include it
        // Wait, parser/expr.h is included after parser/decl.h normally? Not necessarily.
        // Actually, parse_type_fields is in decl.h. We need to call it.
        bool is_enum;
        Variant* adt_variants;
        DeclList* struct_fields = parse_type_fields(arena, parser, &is_enum, &adt_variants);
        
        parser_expect(TOKEN_R_BRACE, "Expected '}' at end of anonymous type definition");
        parser_advance();

        if (is_enum) {
            return expr_anon_enum(arena, adt_variants);
        } else {
            return expr_anon_struct(arena, struct_fields);
        }
    }

    if (parser_match(TOKEN_NUMBER)) {
        int value = atoi(parser->token.start);
        parser_advance();
        return expr_literal(arena, value);
    }
    else if (parser_match(TOKEN_FLOAT_LITERAL)) {
        double value = strtod(parser->token.start, NULL);
        parser_advance();
        return expr_float_literal(arena, value);
    }   
    else if (parser_match(TOKEN_STRING_LITERAL)) {  // New branch for strings
        const char* str = parser->token.start;
        isize len = parser->token.length;
        parser_advance();
        return expr_string(arena, str, len);
    }
    else if (parser_match(TOKEN_CHAR_LITERAL)) {
        // raw token looks like  'x'  or  '\n'  or  '\x1B'
        const char *s = parser->token.start;
        isize len    = parser->token.length;
        // must be at least 'a' → 3 chars, and start/end with '\''
        if (len < 3 || s[0] != '\'' || s[len-1] != '\'')
            parser_error("malformed character literal");

        unsigned char c;
        if (s[1] != '\\') {
            // simple: 'a'
            c = (unsigned char)s[1];
        } else {
            // escaped: \?
            char esc = s[2];
            switch (esc) {
                case 'n':  c = '\n'; break;
                case 'r':  c = '\r'; break;
                case 't':  c = '\t'; break;
                case '\\': c = '\\'; break;
                case '\'': c = '\''; break;
                case 'x':
                    if (len < 6) parser_error("incomplete \\xHH escape");
                    c = (unsigned char)((from_hex(s[3]) << 4) | from_hex(s[4]));
                    break;
                default:
                    parser_error("unknown escape sequence in char literal");
            }
        }

        parser_advance();
        return expr_char_literal(arena, c);
    }
    else if (parser_match(TOKEN_L_BRACKET)) {
        // Array literal: [expr, expr, ...]
        isize arr_line = parser->line;
        isize arr_col  = parser->column;
        parser_advance();  // consume '['
        ExprList *elements = NULL;
        ExprList **tail = &elements;
        if (!parser_match(TOKEN_R_BRACKET)) {
            do {
                Expr *elem = parse_expr(arena, parser);
                *tail = expr_list(arena, elem);
                tail = &(*tail)->next;
                if (parser_match(TOKEN_COMMA)) parser_advance();
                else break;
            } while (!parser_match(TOKEN_R_BRACKET));
        }
        parser_expect(TOKEN_R_BRACKET, "Expected ']' after array literal");
        parser_advance();
        Expr *arr = expr_array_literal(arena, elements);
        arr->line = arr_line;
        arr->col  = arr_col;
        return arr;
    }
    else if (parser_match(TOKEN_IDENTIFIER)) {
        // 1) get the base identifier
        Id *identifier = id(arena, parser->token.length, parser->token.start);
        parser_advance();
        Expr *expr = expr_identifier(arena, identifier);
    
        // 2) consume any number of `.field` suffixes
        // member‑access: a.b → EXPR_MEMBER
        while (parser_match(TOKEN_DOT)) {
            parser_advance();  // consume '.'

            parser_expect(TOKEN_IDENTIFIER,
                "Expected identifier after '.'");
            Id *field_id = id(arena,
                              parser->token.length,
                              parser->token.start);
            parser_advance();

            expr = expr_member(arena, expr, field_id);
        }
    
        // 3) now handle function calls, if any
        while (parser_match(TOKEN_L_PAREN)) {
            parser_advance(); // Consume '('
            ExprList* args = NULL;
            ExprList** args_tail = &args;
            if (!parser_match(TOKEN_R_PAREN)) {
                // parse comma‑separated args
                do {
                    Expr *arg = parse_expr(arena, parser);
                    *args_tail = expr_list(arena, arg);
                    args_tail = &(*args_tail)->next;
                    if (parser_match(TOKEN_COMMA)) parser_advance();
                    else break;
                } while (true);
            }
            parser_expect(TOKEN_R_PAREN,
                          "Expected ')' after function call arguments");
            parser_advance(); // Consume ')'
    
            expr = expr_call(arena, expr, args);
        }
    
        // 4) then handle any number of indexing/slicing suffixes:  a[expr]
        while (parser_match(TOKEN_L_BRACKET)) {
            parser_advance();  // consume '['

            // check for an open-ended slice: <start> '..' ']'
            Expr *idx_expr = NULL;
            Expr *start = NULL;
            Expr *end   = NULL;

            // first thing inside the brackets
            if (parser_match(TOKEN_DOT_DOT)) {
                // form “[..end]” – empty start
                parser_advance();  // consume '..'
                end = parse_expr(arena, parser);
            } else {
                // parse the start expression
                start = parse_expr(arena, parser);
                if (parser_match(TOKEN_DOT_DOT)) {
                    // slice: start .. [maybe end]
                    parser_advance();  // consume '..'
                    if (!parser_match(TOKEN_R_BRACKET)) {
                        end = parse_expr(arena, parser);
                    }
                } else {
                    // just a plain index: [start]
                    idx_expr = start;
                }
            }

            parser_expect(TOKEN_R_BRACKET,
                          "Expected ']' after index or slice");
            parser_advance();  // consume the ']'

            if (!idx_expr && (start || end)) {
                // build a Range expr (open‐ended if end==NULL)
                idx_expr = expr_range(arena, start, end, /*inclusive=*/false);
            }

            expr = expr_index(arena, expr, idx_expr);
        }

        return expr;
    }
    
    else if (parser_match(TOKEN_L_PAREN)) {
        parser_advance();
        Expr *expr = parse_expr(arena, parser);
        parser_expect(TOKEN_R_PAREN, "Expected closing ')'");
        parser_advance();
        return expr;
    }

    fprintf(stderr, "Error Ln %li, Col %li: Unexpected token in expression: %s (%d)\n", 
            parser->line, parser->column, token_kind_name(parser->token.kind), parser->token.kind);
    abort();
    return NULL;
}

#endif // PARSER_EXPR_H
 
