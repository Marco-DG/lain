#ifndef PARSER_CORE_H
#define PARSER_CORE_H

#include "../parser.h"

typedef struct {
    Lexer *lexer;
    Token  token;
    isize  line;
    isize  column;
} Parser;

// low‑level helpers
Token   _parser_advance(Parser *parser);
void    _parser_expect(Parser *parser, bool expr, const char *msg);
int     get_precedence(TokenKind op);

// helper to parse dotted paths in calls/use
Expr *parse_path_expr(Arena *arena, Parser *parser);

// convenient macros
#define parser_match(k)       (parser->token.kind == k)
#define parser_error(msg)     _parser_error(parser, msg)
#define parser_expect(k,msg)  _parser_expect(parser, !parser_match(k), msg)
#define parser_advance()      _parser_advance(parser)

// Treat the lexer-normalized EOL token (set in _parser_advance) as end-of-line.
// Also accept line/multiline comments here so parser_skip_eol() removes them.
#define parser_is_eol()            (parser_match(TOKEN_EOL))
#define parser_is_eol_or_comment() (parser_match(TOKEN_EOL)         \
                                    || parser_match(TOKEN_LINE_COMMENT)\
                                    || parser_match(TOKEN_MULTILINE_COMMENT))
#define parser_skip_eol()          while (parser_is_eol_or_comment()) parser_advance()
#define parser_expect_eol(msg)     _parser_expect(parser, !parser_is_eol(), msg)


Token _parser_advance(Parser* parser) {
    // keep pulling tokens until it's not a comment
    do {
        parser->token = lexer_next(parser->lexer);
    } while (parser->token.kind == TOKEN_LINE_COMMENT
          || parser->token.kind == TOKEN_MULTILINE_COMMENT);

    // make a local copy so we can normalize the kind if needed
    Token token = parser->token;

    // update line/column based on the raw token
    if (token.kind == TOKEN_NEWLINE) {
        parser->line++;
        parser->column = 1;
    } else {
        parser->column += token.length;
    }

    // normalize newline into a single canonical EOL token
    // Semicolons are FORBIDDEN in Lain — newlines are the only statement terminator
    if (token.kind == TOKEN_NEWLINE) {
        token.kind = TOKEN_EOL;
    } else if (token.kind == TOKEN_SEMICOLON) {
        fprintf(stderr, "Error Ln %li, Col %li: Semicolons are not allowed in Lain. Use newlines to separate statements.\n",
                parser->line, parser->column);
        abort();
    }

    // write the (possibly normalized) token back into parser->token so
    // the rest of the parser sees the canonical kind.
    parser->token = token;

    return token;
}


void _parser_error(Parser* parser, const char *error_message) {
    fprintf(stderr, "Error Ln %li, Col %li: %s\n", parser->line, parser->column, error_message);
    abort();
}

void _parser_expect(Parser* parser, bool expr, const char *error_message) {
    if (expr) {
        fprintf(stderr, "Error Ln %li, Col %li: %s\n", parser->line, parser->column, error_message);
        abort();
    }
}

// Returns operator precedence (higher number = higher precedence)
// Returns operator precedence (higher number = higher precedence)
int get_precedence(TokenKind op) {
    switch (op) {
        // * / %  → precedence 7
        case TOKEN_ASTERISK:
        case TOKEN_SLASH:
        case TOKEN_PERCENT:
            return 7;

        // + -   → precedence 6
        case TOKEN_PLUS:
        case TOKEN_MINUS:
            return 6;

        // <  <=  >  >=   → precedence 5
        case TOKEN_ANGLE_BRACKET_LEFT:
        case TOKEN_ANGLE_BRACKET_LEFT_EQUAL:
        case TOKEN_ANGLE_BRACKET_RIGHT:
        case TOKEN_ANGLE_BRACKET_RIGHT_EQUAL:
            return 5;

        // ==  !=   → precedence 4
        case TOKEN_EQUAL_EQUAL:
        case TOKEN_BANG_EQUAL:
            return 4;

        // &  (bitwise‐and)  → precedence 3
        case TOKEN_AMPERSAND:
            return 3;

        // |  ^  (bitwise‐or, bitwise‐xor)  → precedence 2
        case TOKEN_PIPE:
        case TOKEN_CARET:
            return 2;

        // and  (logical‐and)  → precedence 1
        case TOKEN_KEYWORD_AND:
            return 1;

        // or  (logical‐or)  → precedence 0
        case TOKEN_KEYWORD_OR:
            return 0;

        default:
            return -1;  // everything else (no binary precedence)
    }
}


// call this in parse_use_stmt and elsewhere you accept dotted names
Expr *parse_path_expr(Arena *arena, Parser *parser) {
    // must start with a bare identifier
    parser_expect(TOKEN_IDENTIFIER, "Expected identifier in path");
    Id *base = id(arena, parser->token.length, parser->token.start);
    Expr *expr = expr_identifier(arena, base);
    parser_advance();  // consume the identifier

    // then any number of single-dot member accesses
    while (parser_match(TOKEN_DOT)) {
        parser_advance();   // consume the '.'
        parser_expect(TOKEN_IDENTIFIER,
                      "Expected member name after '.'");
        Id *field = id(arena,
                       parser->token.length,
                       parser->token.start);
        parser_advance();   // consume the field
        expr = expr_member(arena, expr, field);
    }
    return expr;
}

// helper to convert one hex digit '0'–'9','A'–'F','a'–'f' → 0–15
static int from_hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    printf("invalid hex digit in char literal");
    return 0;
}

#endif // PARSER_CORE_H