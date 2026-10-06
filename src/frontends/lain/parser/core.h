#ifndef PARSER_CORE_H
#define PARSER_CORE_H

#include <errno.h>
#include <stdlib.h>
#include "../parser.h"

typedef struct Parser {
    Lexer *lexer;
    Token  token;
    isize  line;
    isize  column;
    isize  line_carry;   // line ends inside the current token, added when the next one is read
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
    // The token just consumed may have spanned lines: a string or character literal can hold a
    // raw line end (Annex A `string-char`). Its own position is where it STARTS, so its line
    // ends count from the next token on. They were never counted, so after a two-line string
    // every position was one line early (I.102).
    parser->line += parser->line_carry;
    parser->line_carry = 0;

    // keep pulling tokens until it's not a comment
    do {
        parser->token = lexer_next(parser->lexer);
        // Block comments may span multiple lines — count their internal newlines
        // so that subsequent tokens get the correct line number.
        if (parser->token.kind == TOKEN_MULTILINE_COMMENT) {
            const char *p   = parser->token.start;
            const char *end = p + parser->token.length;
            for (; p < end; p++) {
                if (*p == '\n') {
                    parser->line++;
                    parser->column = 1;
                }
            }
        }
    } while (parser->token.kind == TOKEN_LINE_COMMENT
          || parser->token.kind == TOKEN_MULTILINE_COMMENT);

    // make a local copy so we can normalize the kind if needed
    Token token = parser->token;

    // update line/column based on the raw token. The column is where the token STARTS, counted
    // from the start of its line. It was `column += token.length`, which counted token lengths
    // and never the whitespace between them, so every column the front end reported was short
    // by the indentation and every space before the token (`    use p.x as px` said Col 4).
    if (token.kind == TOKEN_NEWLINE) {
        parser->line++;
        parser->column = 1;
    } else if (parser->lexer && parser->lexer->text && token.start) {
        const char *p = token.start;
        while (p > parser->lexer->text && p[-1] != '\n') p--;
        parser->column = (isize)(token.start - p) + 1;
        if (token.kind == TOKEN_STRING_LITERAL) parser->column--;   // its start skips the opening quote
        if (token.kind == TOKEN_STRING_LITERAL || token.kind == TOKEN_CHAR_LITERAL)
            for (const char *q = token.start; q < token.start + token.length; q++)
                if (*q == '\n') parser->line_carry++;
    } else {
        parser->column += token.length;
    }

    // A literal or block comment that the text ends inside (lexer.h): the token starts at its
    // opening delimiter, and so does the position reported (I.101).
    if (token.kind == TOKEN_INVALID && token.start
        && (*token.start == '"' || *token.start == '\'' || (token.start[0] == '/' && token.start[1] == '*'))) {
        if (*token.start == '/')
            fprintf(stderr, "[E100] Error Ln %li, Col %li: unterminated block comment: no closing `*/` "
                    "before the end of the file\n", parser->line, parser->column);
        else
            fprintf(stderr, "[E100] Error Ln %li, Col %li: unterminated %s literal: no closing `%c` "
                    "before the end of the file\n", parser->line, parser->column,
                    *token.start == '"' ? "string" : "character", *token.start);
        exit(1);
    }

    // normalize newline and semicolon into a single canonical EOL token
    if (token.kind == TOKEN_NEWLINE || token.kind == TOKEN_SEMICOLON) {
        token.kind = TOKEN_EOL;
    }

    // write the (possibly normalized) token back into parser->token so
    // the rest of the parser sees the canonical kind.
    parser->token = token;

    return token;
}


void _parser_error(Parser* parser, const char *error_message) {
    fprintf(stderr, "[E100] Error Ln %li, Col %li: %s\n", parser->line, parser->column, error_message);
    exit(1);
}

void _parser_expect(Parser* parser, bool expr, const char *error_message) {
    if (expr) {
        fprintf(stderr, "[E100] Error Ln %li, Col %li: %s\n", parser->line, parser->column, error_message);
        exit(1);
    }
}

// Returns operator precedence (higher number = higher precedence)
// Spec 08's table, highest first (the Rust and Zig order, not C's):
//   * / %   + -   << >>   &   ^   |   < > <= >= in   == !=   and   or
// The bitwise operators bind TIGHTER than the comparisons, so `a & 1 == 0` is `(a & 1) == 0`. The
// parser had C's order, & ^ | below the comparisons, against the spec, LANGUAGE §7.7 and this
// comment's own heading: `a & 1 == 0` was `a & (1 == 0)` and refused as "`&` on a `bool` operand".
// Every unparenthesised mix of the two was refused that way (a bitwise operator refuses a bool),
// so moving them changed the meaning of no program that compiled.
int get_precedence(TokenKind op) {
    switch (op) {
        // * / %  → precedence 10
        case TOKEN_ASTERISK:
        case TOKEN_SLASH:
        case TOKEN_PERCENT:
        case TOKEN_ASTERISK_PERCENT:   // *%  wrapping mul (Q-002)
        case TOKEN_ASTERISK_PIPE:      // *|  saturating mul (Q-002)
        case TOKEN_ASTERISK_QUESTION:  // *?  checked mul (F3.5)
        case TOKEN_SLASH_PERCENT:      // /%  wrapping divide (DECIDE-M)
        case TOKEN_SLASH_PIPE:         // /|  saturating divide (DECIDE-M)
            return 10;

        // + -   → precedence 9
        case TOKEN_PLUS:
        case TOKEN_MINUS:
        case TOKEN_PLUS_PERCENT:       // +%  wrapping add (Q-002)
        case TOKEN_MINUS_PERCENT:      // -%  wrapping sub (Q-002)
        case TOKEN_PLUS_PIPE:          // +|  saturating add (Q-002)
        case TOKEN_MINUS_PIPE:         // -|  saturating sub (Q-002)
        case TOKEN_PLUS_QUESTION:      // +?  checked add (F3.5)
        case TOKEN_MINUS_QUESTION:     // -?  checked sub (F3.5)
            return 9;

        // << >>  → precedence 8 (bitwise shift)
        case TOKEN_SHIFT_LEFT:
        case TOKEN_SHIFT_LEFT_PERCENT:     // <<%  wrapping left shift
        case TOKEN_SHIFT_RIGHT:
            return 8;

        // &  (bitwise‐and)  → precedence 7
        case TOKEN_AMPERSAND:
            return 7;

        // ^  (bitwise‐xor)  → precedence 6
        case TOKEN_CARET:
            return 6;

        // |  (bitwise‐or)   → precedence 5
        case TOKEN_PIPE:
            return 5;

        // <  <=  >  >=  in  → precedence 4
        case TOKEN_ANGLE_BRACKET_LEFT:
        case TOKEN_ANGLE_BRACKET_LEFT_EQUAL:
        case TOKEN_ANGLE_BRACKET_RIGHT:
        case TOKEN_ANGLE_BRACKET_RIGHT_EQUAL:
        case TOKEN_KEYWORD_IN:
            return 4;

        // ==  !=   → precedence 3
        case TOKEN_EQUAL_EQUAL:
        case TOKEN_BANG_EQUAL:
            return 3;

        // and  (logical‐and)  → precedence 2
        case TOKEN_KEYWORD_AND:
            return 2;

        // or  (logical‐or)  → precedence 1
        case TOKEN_KEYWORD_OR:
            return 1;

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
    // Not a hex digit: the caller refuses the escape. This printed "invalid hex digit in char
    // literal" to STDOUT, with no code and no newline, and returned 0, so `'\xZ1'` compiled to 1.
    return -1;
}

// F-004 helper: strip underscores from a numeric lexeme then convert.
// Accepts 0x/0X (hex), 0b/0B (binary), 0o/0O (octal), and plain decimal.
//
// Overflow is a hard error rather than a silent saturation: strtoll clamps an
// out-of-range value to LLONG_MAX (which then fits i64, so the downstream E086
// boundary check never fires) — so `99999999999999999999` used to wrap in
// silently. Decimal literals must fit the i64 value representation. Non-decimal
// bases are bit-pattern notations, so they may span the full unsigned 64-bit
// range (e.g. 0xFFFFFFFFFFFFFFFF == -1 as the stored int64_t), matching C.
static long long parse_numeric_literal_at(isize line, isize col, const char *start, long length) {
    char buf[80];
    long i = 0, j = 0;
    bool truncated = false;
    for (; i < length; i++) {
        if (start[i] == '_') continue;
        if (j >= (long)sizeof(buf) - 1) { truncated = true; break; }
        buf[j++] = start[i];
    }
    buf[j] = '\0';

    // Non-decimal bases are BIT-PATTERN notations: the 64 bits written, as a two's-complement i64
    // (a full u64 literal representation is a separate follow-up). They were read with strtoll,
    // which does not keep the bits — it CLAMPS: `0xcbf29ce484222325` (FNV-1a's offset basis)
    // compiled as 0x7FFFFFFFFFFFFFFF, a silently wrong constant, and so did every mask and
    // pattern with the top bit set. Read unsigned, the bits are exact: the top bit set makes the
    // i64 negative, so `var h u64 = 0xcbf2...` is refused (E086) rather than wrong, and
    // `0xcbf29ce484222325 as% u64` is the u64 constant. More than 64 bits is refused outright.
    // Decimal literals must fit the i64 value model; strtoll otherwise clamps to
    // LLONG_MAX (which fits i64, so the E086 boundary check never fires) — this
    // is what let `99999999999999999999` wrap in silently.
    {
        int base = 0, skip = 0;
        if (j >= 2 && buf[0] == '0' && (buf[1] == 'b' || buf[1] == 'B')) { base = 2;  skip = 2; }
        if (j >= 2 && buf[0] == '0' && (buf[1] == 'o' || buf[1] == 'O')) { base = 8;  skip = 2; }
        if (j >= 2 && buf[0] == '0' && (buf[1] == 'x' || buf[1] == 'X')) { base = 16; skip = 2; }
        if (base) {
            errno = 0;
            unsigned long long u = strtoull(buf + skip, NULL, base);
            if (truncated || errno == ERANGE) {
                fprintf(stderr, "[E086] Error Ln %li, Col %li: integer literal '%.*s' has more "
                        "than 64 bits.\n", (long)line, (long)col, (int)length, start);
                exit(1);
            }
            long long bits; memcpy(&bits, &u, sizeof bits);      // the pattern, two's complement
            return bits;
        }
    }
    // A decimal literal has no leading zero (Annex A: `nonzero-digit {digit} | "0"`). `010` was
    // read as 10, where C reads 8: a program ported from C, or a reader who knows C, got another
    // number with nothing said. Octal is spelled `0o10`. A float (`010.5`) is not read here.
    if (j > 1 && buf[0] == '0') {
        long k = 0; while (k < j - 1 && buf[k] == '0') k++;
        fprintf(stderr, "[E100] Error Ln %li, Col %li: a decimal literal has no leading zero: `%.*s` "
                "would be octal in C. Write `%s`, or `0o%s` for octal.\n", (long)line, (long)col,
                (int)length, start, buf + k, buf + k);
        exit(1);
    }
    errno = 0;
    long long value = strtoll(buf, NULL, 10);
    if (truncated || errno == ERANGE) {
        fprintf(stderr, "[E086] Error Ln %li, Col %li: integer literal '%.*s' is too large to "
                "fit in a signed 64-bit integer.\n", (long)line, (long)col, (int)length, start);
        exit(1);
    }
    return value;
}
// Every caller reads the parser's current token, so the position is the parser's.
#define parse_numeric_literal(s, l) parse_numeric_literal_at(parser->line, parser->column, (s), (l))

#endif // PARSER_CORE_H