#ifndef PARSER_STMT_H
#define PARSER_STMT_H

#include "../parser.h"

// lists and statements
StmtList* parse_stmt_list(Arena *arena, Parser *parser);
Stmt *    parse_stmt(Arena *arena, Parser *parser);

// specific statement forms
Stmt *parse_var_stmt(Arena *arena, Parser *parser);
Stmt *parse_assign_stmt(Arena *arena, Parser *parser);
Stmt *parse_expr_stmt(Arena *arena, Parser *parser);
Stmt *parse_if_stmt(Arena *arena, Parser *parser);
Stmt *parse_for_stmt(Arena *arena, Parser *parser);
Stmt *parse_continue_stmt(Arena *arena, Parser *parser);
Stmt *parse_match_stmt(Arena *arena, Parser *parser);
Stmt *parse_use_stmt(Arena *arena, Parser *parser);
Stmt *parse_comptime_stmt(Arena *arena, Parser *parser);
Stmt *parse_unsafe_stmt(Arena *arena, Parser *parser);
Stmt *parse_while_stmt(Arena *arena, Parser *parser);

Stmt *parse_decl_stmt(Arena *arena, Parser *parser);

// -----------------------------------------------------------------------------
// NEW: parse a declaration when source looks like:
//    name <type> [= <expr>]
// This mirrors parse_var_stmt but without the leading 'var' token.
// Example it must handle:
//    fixed_len_slice   u8[5]   = "hello"
//    zero_terminated   u8[:0]  = "hello\0"
//    variable_len      u8[]    = "Hi"
// -----------------------------------------------------------------------------
Stmt *parse_decl_stmt(Arena *arena, Parser *parser) {
    // current token must be the identifier of the variable name
    parser_expect(TOKEN_IDENTIFIER, "Expected identifier for declaration");
    Id *var_name = id(arena, parser->token.length, parser->token.start);
    parser_advance(); // consume the name

    // optional type annotation: expect an IDENT (type name) or 'mov' prefix
    Type *type_annotation = NULL;
    if (parser_match(TOKEN_IDENTIFIER) || parser_match(TOKEN_KEYWORD_MOV) || parser_match(TOKEN_KEYWORD_COMPTIME) || parser_match(TOKEN_ASTERISK)) {
        type_annotation = parse_type(arena, parser);
    }

    // optional initializer
    Expr *assigned_expr = NULL;
    if (parser_match(TOKEN_EQUAL)) {
        parser_advance();
        assigned_expr = parse_expr(arena, parser);
    }

    return stmt_var(arena, var_name, type_annotation, assigned_expr);
}

StmtList* parse_stmt_list(Arena* arena, Parser* parser) {
    // Salta eventuali blank lines (newline o semicolon) iniziali
    parser_skip_eol();

    StmtList* list = NULL;
    StmtList** tail = &list;

    // Continua finché non troviamo '}' o EOF
    while (!parser_match(TOKEN_R_BRACE) && !parser_match(TOKEN_EOF)) {
        // Parsifica la singola istruzione
        Stmt* stmt = parse_stmt(arena, parser);

        // Aggiungila alla lista
        *tail = stmt_list(arena, stmt);
        tail  = &(*tail)->next;

        // Check for '}' before expecting EOL
        if (parser_match(TOKEN_R_BRACE)) {
             break;
        }

        // Deve esserci un terminatore (newline o semicolon)
        parser_expect_eol("Expected ';' or newline after statement");
        parser_advance();

        // Salta eventuali blank lines successive
        parser_skip_eol();
    }

    return list;
}


Stmt *parse_stmt(Arena* arena, Parser* parser)
{
    if (parser_match(TOKEN_KEYWORD_RETURN)) {
        parser_advance();                 // consume `return`
        Expr *value = parse_expr(arena, parser);
        return stmt_return(arena, value);
    }
    if (parser_match(TOKEN_KEYWORD_COMPTIME)) {
        // keep consistent with other keywords (we consume keyword in caller)
        parser_advance(); // consume 'comptime'
        return parse_comptime_stmt(arena, parser);
    }
    if (parser_match(TOKEN_KEYWORD_VAR)) {
        parser_advance();
        return parse_var_stmt(arena, parser);
    }
    if (parser_match(TOKEN_KEYWORD_IF)) {
        return parse_if_stmt(arena, parser);
    }
    if (parser_match(TOKEN_KEYWORD_FOR)) {
        parser_advance();                  // consume 'for'
        return parse_for_stmt(arena, parser);
    }
    if (parser_match(TOKEN_KEYWORD_WHILE)) {
        // 'while' keyword consumption handled in parse_while_stmt or here?
        // Other functions consume the keyword before calling. Let's consume it here.
        parser_advance();
        return parse_while_stmt(arena, parser);
    }
    if (parser_match(TOKEN_KEYWORD_CONTINUE)) {
        return parse_continue_stmt(arena, parser);
    }
    if (parser_match(TOKEN_KEYWORD_CASE)) {
        parser_advance();
        return parse_match_stmt(arena, parser);
    }
    if (parser_match(TOKEN_KEYWORD_USE)) {
        parser_advance();
        return parse_use_stmt(arena, parser);
    }
    if (parser_match(TOKEN_KEYWORD_UNSAFE)) {
        parser_advance();
        return parse_unsafe_stmt(arena, parser);
    }

    // ——— handle bare identifier starting lines ———
    // REMOVED: We no longer support bare declarations like `x int`.
    // All declarations must be `var x ...` or implicit `x = ...`.
    /*
    if (parser_match(TOKEN_IDENTIFIER)) {
        Token next = lexer_peek(parser->lexer);

        // If next token is an identifier or 'mov', we consider this a declaration
        if (next.kind == TOKEN_IDENTIFIER || next.kind == TOKEN_KEYWORD_MOV) {
            return parse_decl_stmt(arena, parser);
        }
    }
    */

    // Parse as expression first (LHS of potential assignment)
    Expr *lhs = parse_expr(arena, parser);

    // Check for assignment operator
    TokenKind op = parser->token.kind;
    bool is_assign = false;
    switch (op) {
        case TOKEN_EQUAL:
        case TOKEN_PLUS_EQUAL:
        case TOKEN_MINUS_EQUAL:
        case TOKEN_ASTERISK_EQUAL:
        case TOKEN_SLASH_EQUAL:
        case TOKEN_PERCENT_EQUAL:
        case TOKEN_AMPERSAND_EQUAL:
        case TOKEN_PIPE_EQUAL:
        case TOKEN_CARET_EQUAL:
            is_assign = true;
            break;
        default:
            is_assign = false;
    }

    if (is_assign) {
        parser_advance(); // consume operator
        Expr *rhs = parse_expr(arena, parser);

        // desugar compound-assign
        if (op != TOKEN_EQUAL) {
            TokenKind binop;
            switch (op) {
                case TOKEN_PLUS_EQUAL:      binop = TOKEN_PLUS;      break;
                case TOKEN_MINUS_EQUAL:     binop = TOKEN_MINUS;     break;
                case TOKEN_ASTERISK_EQUAL:  binop = TOKEN_ASTERISK;  break;
                case TOKEN_SLASH_EQUAL:     binop = TOKEN_SLASH;     break;
                case TOKEN_PERCENT_EQUAL:   binop = TOKEN_PERCENT;   break;
                case TOKEN_AMPERSAND_EQUAL: binop = TOKEN_AMPERSAND; break;
                case TOKEN_PIPE_EQUAL:      binop = TOKEN_PIPE;      break;
                case TOKEN_CARET_EQUAL:     binop = TOKEN_CARET;     break;
                default:                    binop = TOKEN_PLUS;      break;
            }
            rhs = expr_binary(arena, binop, lhs, rhs);
        }
        return stmt_assign(arena, lhs, rhs);
    }

    // Otherwise it's an expression statement
    return stmt_expr(arena, lhs);
}

/* ---------- implement parse_comptime_stmt ----------
//
// Grammar we support here (mirrors var/mov statement style):
//   comptime <ident> [ <type> ] [ = <expr> ]
//
// We consume the 'comptime' keyword *before* this function is called
// (consistent with var/mov handling above).
//
----------------------------------------------------*/
Stmt *parse_comptime_stmt(Arena* arena, Parser* parser)
{
    parser_expect(TOKEN_IDENTIFIER, "Expected variable name after 'comptime'");
    Id* var_name = id(arena, parser->token.length, parser->token.start);
    parser_advance(); // consume the name

    // optional type annotation (allow 'mov' or an identifier or nested 'comptime' if parse_type handles it)
    Type *type_annotation = NULL;
    if (parser_match(TOKEN_KEYWORD_MOV) || parser_match(TOKEN_IDENTIFIER) || parser_match(TOKEN_KEYWORD_COMPTIME) || parser_match(TOKEN_ASTERISK)) {
        type_annotation = parse_type(arena, parser);
        // wrap with comptime type so later passes know it's compile-time
        if (type_annotation) {
            type_annotation = type_comptime(arena, type_annotation);
        }
    }

    // optional initializer
    Expr *assigned_expr = NULL;
    if (parser_match(TOKEN_EQUAL)) {
        parser_advance();
        assigned_expr = parse_expr(arena, parser);
    }

    // NOTE: reuse stmt_var node (semantic passes should look for TYPE_COMPTIME
    // on the declared type or special marker). If you prefer a distinct AST node
    // for comptime-decls, change this to build that node instead.
    return stmt_var(arena, var_name, type_annotation, assigned_expr);
}




Stmt *parse_var_stmt(Arena* arena, Parser* parser)
{
    parser_expect(TOKEN_IDENTIFIER, "Expected variable name after 'var'");
    Id* var_name = id(arena, parser->token.length, parser->token.start);
    parser_advance();

    // optional type annotation
    Type *type_annotation = NULL;
    if (parser_match(TOKEN_IDENTIFIER) || parser_match(TOKEN_KEYWORD_MOV) || parser_match(TOKEN_ASTERISK)) {
        type_annotation = parse_type(arena, parser);
    }

    // optional initializer
    Expr *assigned_expr = NULL;
    if (parser_match(TOKEN_EQUAL)) {
        parser_advance();
        assigned_expr = parse_expr(arena, parser);
    }

    // var creates a MUTABLE variable
    Stmt *s = stmt_var(arena, var_name, type_annotation, assigned_expr);
    s->as.var_stmt.is_mutable = true; 
    return s;
}


// <name> (= | += | -= | ... ) <expr>
Stmt *parse_assign_stmt(Arena *arena, Parser *parser) {
    // 1) LHS name
    parser_expect(TOKEN_IDENTIFIER, "Expected an identifier (variable name)");
    Id *var_name = id(arena, parser->token.length, parser->token.start);
    parser_advance();  // consume the identifier

    // Wrap the LHS identifier into an ExprIdentifier for sema
    Expr *lhs_expr = expr_identifier(arena, var_name);

    // 2) operator: =, +=, -=, etc.
    TokenKind op = parser->token.kind;
    switch (op) {
        case TOKEN_EQUAL:
        case TOKEN_PLUS_EQUAL:
        case TOKEN_MINUS_EQUAL:
        case TOKEN_ASTERISK_EQUAL:
        case TOKEN_SLASH_EQUAL:
        case TOKEN_PERCENT_EQUAL:
        case TOKEN_AMPERSAND_EQUAL:
        case TOKEN_PIPE_EQUAL:
        case TOKEN_CARET_EQUAL:
            break;
        default:
            parser_error("Expected assignment operator (=, +=, -=, ...)");
    }
    parser_advance();  // consume the operator

    // 3) RHS expression
    Expr *rhs = parse_expr(arena, parser);

    // 4) desugar compound-assign into a binary op if needed
    if (op != TOKEN_EQUAL) {
        // build `lhs_expr op= rhs` → `lhs_expr = lhs_expr op rhs`
        TokenKind binop;
        switch (op) {
            case TOKEN_PLUS_EQUAL:      binop = TOKEN_PLUS;      break;
            case TOKEN_MINUS_EQUAL:     binop = TOKEN_MINUS;     break;
            case TOKEN_ASTERISK_EQUAL:  binop = TOKEN_ASTERISK;  break;
            case TOKEN_SLASH_EQUAL:     binop = TOKEN_SLASH;     break;
            case TOKEN_PERCENT_EQUAL:   binop = TOKEN_PERCENT;   break;
            case TOKEN_AMPERSAND_EQUAL: binop = TOKEN_AMPERSAND; break;
            case TOKEN_PIPE_EQUAL:      binop = TOKEN_PIPE;      break;
            case TOKEN_CARET_EQUAL:     binop = TOKEN_CARET;     break;
            default:                    binop = TOKEN_PLUS;      break; // never here
        }
        rhs = expr_binary(arena, binop, lhs_expr, rhs);
    }

    // 5) produce the AST node
    return stmt_assign(arena, lhs_expr, rhs);
}



Stmt *parse_expr_stmt(Arena* arena, Parser* parser) {
    Expr *expr = parse_expr(arena, parser);

    return stmt_expr(arena, expr);
}

/// parse_if_stmt: parses
///    if <expr> { <stmts> }
///    [ else if <expr> { <stmts> } ]*
///    [ else { <stmts> } ]
Stmt *parse_if_stmt(Arena *arena, Parser *parser) {
    // we know parser->token.kind == TOKEN_KEYWORD_IF
    parser_advance();  // consume 'if'

    // 1) condition
    Expr *cond = parse_expr(arena, parser);

    // 2) then‐block
    parser_expect(TOKEN_L_BRACE, "Expected '{' after if");
    parser_advance();
    StmtList *then_branch = parse_stmt_list(arena, parser);
    parser_expect(TOKEN_R_BRACE, "Expected '}' after if‐then block");
    parser_advance();

    // 3) optional else / else‐if
    StmtList *else_branch = NULL;
    if (parser_match(TOKEN_KEYWORD_ELSE)) {
        parser_advance();  // consume 'else'
        if (parser_match(TOKEN_KEYWORD_IF)) {
            // parse an `else if` as a nested if-stmt
            Stmt *nested = parse_if_stmt(arena, parser);
            else_branch   = stmt_list(arena, nested);
        } else {
            // plain else‐block
            parser_expect(TOKEN_L_BRACE, "Expected '{' after else");
            parser_advance();
            else_branch = parse_stmt_list(arena, parser);
            parser_expect(TOKEN_R_BRACE, "Expected '}' after else block");
            parser_advance();
        }
    }

    return stmt_if(arena, cond, then_branch, else_branch);
}


// for <ident> in <expr> { <stmts> }
Stmt *parse_for_stmt(Arena* arena, Parser* parser) {
    // 1) read first identifier
    parser_expect(TOKEN_IDENTIFIER,
                  "Expected loop variable after 'for'");
    Id *first = id(arena,
                   parser->token.length,
                   parser->token.start);
    parser_advance();

    // 2) maybe a comma + second identifier
    Id *index_name = NULL;
    Id *value_name = first;
    if (parser_match(TOKEN_COMMA)) {
        parser_advance();  // consume ','
        parser_expect(TOKEN_IDENTIFIER,
                      "Expected second loop variable");
        value_name = id(arena,
                        parser->token.length,
                        parser->token.start);
        parser_advance();
        index_name = first;
    }

    // 3) the 'in'
    parser_expect(TOKEN_KEYWORD_IN,
                  "Expected 'in' after loop variables");
    parser_advance();

    // 4) the iterable expression
    Expr *iterable = parse_expr(arena, parser);
    
    // Handle range syntax: start..end or start..=end
    if (parser_match(TOKEN_DOT_DOT)) {
        parser_advance(); // consume '..'
        Expr *end = parse_expr(arena, parser);
        iterable = expr_range(arena, iterable, end, false);
    } else if (parser_match(TOKEN_DOT_DOT_EQUAL)) {
        parser_advance(); // consume '..='
        Expr *end = parse_expr(arena, parser);
        iterable = expr_range(arena, iterable, end, true);
    }

    // 5) the body
    parser_expect(TOKEN_L_BRACE,
                  "Expected '{' to start for‐body");
    parser_advance();
    StmtList *body = parse_stmt_list(arena, parser);
    parser_expect(TOKEN_R_BRACE,
                  "Expected '}' after for‐body");
    parser_advance();

    // 6) build the new node
    return stmt_for(arena,
                    index_name,
                    value_name,
                    iterable,
                    body);
}

// while <expr> { <body> }
Stmt *parse_while_stmt(Arena *arena, Parser *parser) {
    // 'while' keyword already consumed
    
    // 1) condition
    Expr *cond = parse_expr(arena, parser);
    
    // 2) body
    parser_expect(TOKEN_L_BRACE, "Expected '{' to start while-body");
    parser_advance();
    StmtList *body = parse_stmt_list(arena, parser);
    parser_expect(TOKEN_R_BRACE, "Expected '}' after while-body");
    parser_advance();
    
    return stmt_while(arena, cond, body);
}

// parse a standalone `continue` statement
Stmt *parse_continue_stmt(Arena *arena, Parser *parser) {
    // we know parser->token.kind == TOKEN_KEYWORD_CONTINUE
    parser_advance();  
    return stmt_continue(arena);
}


Stmt *parse_match_stmt(Arena *arena, Parser *parser) {
    // 'match' keyword already consumed
    Expr *value = parse_expr(arena, parser);

    // expect and consume '{'
    parser_expect(TOKEN_L_BRACE, "Expected '{' after match expression");
    parser_advance();
    // skip any blank lines/comments
    parser_skip_eol();

    StmtMatchCase *first = NULL, **tail = &first;
    Expr *pending_patterns[16]; // for fall-through buffering
    int pending_count = 0;

    while (!parser_match(TOKEN_R_BRACE) && !parser_match(TOKEN_EOF)) {
        // —— parse the pattern header ——
        Expr *pattern = NULL;
        if (parser_match(TOKEN_KEYWORD_ELSE)) {
            parser_advance();
        } else {
            // parse a sub‑expression, then see if it's followed by '..' for a range
            Expr *left = parse_expr(arena, parser);
            if (parser_match(TOKEN_DOT_DOT) || parser_match(TOKEN_DOT_DOT_EQUAL)) {
                bool inclusive = parser->token.kind == TOKEN_DOT_DOT_EQUAL;
                parser_advance(); // consume '..' or '..='
            
                // RHS of the range must be a literal or identifier in patterns
                Expr *right = parse_expr(arena, parser);
                pattern = expr_range(arena, left, right, inclusive);
            } else {
                pattern = left;
            }
        }
        parser_expect(TOKEN_COLON, "Expected ':' after match pattern");
        parser_advance();
        parser_skip_eol();

        // —— parse the body of this case ——
        StmtList *body = NULL, **body_tail = &body;
        while (true) {
            // break if we hit the end of the match block
            if (parser_match(TOKEN_R_BRACE) || parser_match(TOKEN_EOF))
                break;

            // peek ahead to see if the next tokens form a *new* case header:
            //    1) single‐value:    X :
            //    2) range‐value:     X .. Y :
            
            // peek ahead to see if the next tokens form a *new* case header:
            bool stop_for_header = false;
            TokenKind cur = parser->token.kind;

            // allow numeric literals, char/string, identifiers or `else`
            if (cur == TOKEN_NUMBER
            || cur == TOKEN_CHAR_LITERAL
            || cur == TOKEN_STRING_LITERAL
            || cur == TOKEN_IDENTIFIER
            || cur == TOKEN_KEYWORD_ELSE)
            {
                // fork the lexer so we don't consume anything
                Lexer fork = *parser->lexer;

                // first look at the very next token
                Token t1 = lexer_next(&fork);

                // Handle qualified names: Enum.Variant :
                while (t1.kind == TOKEN_DOT) {
                     // Check if next is ident
                     Token t2 = lexer_next(&fork);
                     if (t2.kind == TOKEN_IDENTIFIER) {
                         t1 = lexer_next(&fork);
                     } else {
                         break;
                     }
                }

                if (t1.kind == TOKEN_COLON) {
                    // single‐value header: X :
                    stop_for_header = true;
                }
                else if (t1.kind == TOKEN_DOT_DOT
                    || t1.kind == TOKEN_DOT_DOT_EQUAL)
                {
                    // maybe a range: X .. Y :
                    Token t2 = lexer_next(&fork);
                    if (t2.kind == TOKEN_CHAR_LITERAL
                    || t2.kind == TOKEN_STRING_LITERAL
                    || t2.kind == TOKEN_IDENTIFIER
                    || t2.kind == TOKEN_NUMBER)
                    {
                        Token t3 = lexer_next(&fork);
                        if (t3.kind == TOKEN_COLON) {
                            stop_for_header = true;
                        }
                    }
                }
                else if (t1.kind == TOKEN_L_PAREN) {
                    // Constructor pattern: Variant(...) :
                    // We need to skip balanced parens
                    int depth = 1;
                    while (depth > 0) {
                        Token t = lexer_next(&fork);
                        if (t.kind == TOKEN_EOF) break;
                        if (t.kind == TOKEN_L_PAREN) depth++;
                        else if (t.kind == TOKEN_R_PAREN) depth--;
                    }
                    
                    if (depth == 0) {
                        Token t_after = lexer_next(&fork);
                        if (t_after.kind == TOKEN_COLON) {
                            stop_for_header = true;
                        }
                    }
                }
            }

            if (stop_for_header) {
                break;
            }


            // otherwise it's a real statement inside this case
            Stmt *st = parse_stmt(arena, parser);
            *body_tail = stmt_list(arena, st);
            body_tail  = &(*body_tail)->next;

            parser_expect_eol("Expected end‐of‐line after statement in match case");
            parser_advance();
            parser_skip_eol();
        }

        // buffer this pattern
        pending_patterns[pending_count++] = pattern;

        // if we actually saw any statements, flush all buffered patterns with that body
        if (body) {
            for (int i = 0; i < pending_count; i++) {
                *tail = stmt_match_case(arena, pending_patterns[i], body);
                tail = &(*tail)->next;
            }
            pending_count = 0;
        }

        // skip blank lines before the next pattern
        parser_skip_eol();
    }

    if (pending_count > 0) {
        // trailing patterns with no body → error
        fprintf(stderr, "Error: match patterns with no body at end of block\n");
        exit(1);
    }

    parser_expect(TOKEN_R_BRACE, "Expected '}' after match block");
    parser_advance();

    return stmt_match(arena, value, first);
}



Stmt *parse_use_stmt(Arena* arena, Parser* parser) {
    // 1) target path — MUST call parse_path_expr, not parse_expr
    Expr *target = parse_path_expr(arena, parser);

    // 2) then the `as` keyword
    parser_expect(TOKEN_KEYWORD_AS, "Expected 'as' after use target");
    parser_advance();  // consume 'as'

    // 3) then the alias identifier
    parser_expect(TOKEN_IDENTIFIER,
                  "Expected alias name after 'as'");
    Id *alias = id(arena,
                  parser->token.length,
                  parser->token.start);
    parser_advance();  // consume the alias

    return stmt_use(arena, target, alias);
}

// unsafe { stmts... }
Stmt *parse_unsafe_stmt(Arena *arena, Parser *parser) {
    // 'unsafe' keyword already consumed
    parser_expect(TOKEN_L_BRACE, "Expected '{' after unsafe");
    parser_advance(); // consume '{'
    
    StmtList *body = parse_stmt_list(arena, parser);
    
    parser_expect(TOKEN_R_BRACE, "Expected '}' after unsafe block");
    parser_advance(); // consume '}'
    
    return stmt_unsafe(arena, body);
}


#endif // PARSER_STMT_H 
