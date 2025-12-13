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
    return parse_binary_expr(arena, parser, 0);
}

// <expr> <op> <expr>
Expr *parse_binary_expr(Arena *arena, Parser *parser, int precedence) {
    Expr *left = parse_unary_expr(arena, parser);

    while (true) {
        TokenKind op = parser->token.kind;


        int prec = get_precedence(op);
        if (prec < precedence) break;

        parser_advance();  // consume this operator
        Expr *right = parse_binary_expr(arena, parser, prec + 1);
        left = expr_binary(arena, op, left, right);
    }

    return left;
}


// <op> <expr>
Expr *parse_unary_expr(Arena* arena, Parser* parser)
{
    // -, !
    if (parser_match(TOKEN_MINUS) || parser_match(TOKEN_BANG)) {
        TokenKind op = parser->token.kind;
        parser_advance();
        
        Expr *right = parse_unary_expr(arena, parser);
        return expr_unary(arena, op, right);
    }
    
    return parse_primary_expr(arena, parser);
}

// literals, identifiers, and parenthesized expressions
Expr *parse_primary_expr(Arena* arena, Parser* parser)
{
    if (parser_match(TOKEN_NUMBER)) {
        int value = atoi(parser->token.start);
        parser_advance();
        return expr_literal(arena, value);
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

    fprintf(stderr, "Error Ln %li, Col %li: Unexpected token in expression.\n", parser->line, parser->column);
    abort();
    return NULL;
}

#endif // PARSER_EXPR_H
 
