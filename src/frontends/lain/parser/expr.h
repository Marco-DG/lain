#ifndef PARSER_EXPR_H
#define PARSER_EXPR_H

#include "../parser.h"

// expression entry
Expr *parse_expr(Arena *arena, Parser *parser);

// sub‑levels
Expr *parse_binary_expr(Arena *arena, Parser *parser, int precedence);
Expr *parse_unary_expr(Arena *arena, Parser *parser);
Expr *parse_primary_expr(Arena *arena, Parser *parser);

// `x else panic(...)` detection — defined below parse_unary_expr, used by parse_expr.
static bool expr_is_panic_call(Expr *e);

Expr *parse_expr(Arena* arena, Parser* parser)
{
    isize expr_line = parser->line;
    isize expr_col  = parser->column;
    Expr *result = parse_binary_expr(arena, parser, 0);

    // Postfix `else <arm>` — the recoverable-error handler. Lowest precedence, so
    // `a + b else c` is `(a + b) else c`. A trailing `else` in expression position
    // always belongs to the expression: `if`/`else` statement attachment happens
    // in the statement parser after a brace block, never mid-expression.
    while (parser_match(TOKEN_KEYWORD_ELSE)) {
        parser_advance();
        // `else return X` — on failure, return X from the enclosing function.
        bool is_ret = false;
        if (parser_match(TOKEN_KEYWORD_RETURN)) { is_ret = true; parser_advance(); }
        Expr *arm = parse_binary_expr(arena, parser, 0);
        result = expr_else(arena, result, arm, !is_ret && expr_is_panic_call(arm));
        result->as.else_expr.arm_is_return = is_ret;
    }

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
        // F3.5 tier marker immediately after `as` (before the type). `?T`/`|`-union
        // never start a type here (`?T` is retired; a union `|` only follows a type),
        // so this is unambiguous.
        CastKind ck = CAST_PROVEN;
        if      (parser_match(TOKEN_QUESTION)) { ck = CAST_CHECKED;    parser_advance(); }
        else if (parser_match(TOKEN_PERCENT))  { ck = CAST_WRAPPING;   parser_advance(); }
        else if (parser_match(TOKEN_PIPE))     { ck = CAST_SATURATING; parser_advance(); }
        Type *target = parse_type(arena, parser);
        Expr *operand = left;
        left = expr_cast(arena, left, target);
        left->as.cast_expr.kind = ck;
        // A cast is where its operand is. It had no position, and only the outermost node of an
        // expression gets one, so `x as u8[] + 100` reported its E012 at Ln 0 (M13), while
        // `(x as u8[]) + 100` and `x as u8[]` alone were placed.
        left->line = operand->line; left->col = operand->col;
    }

    while (true) {
        TokenKind op = parser->token.kind;

        // `&&` and `||` are C's spellings. Lain's are `and` and `or`, and the C ones used to
        // lex as TWO tokens — `a == 2 && b == 1` parsed as `(a == 2) & (&b == 1)`, an
        // address-of, and was refused as "incompatible operand types '*u8' and 'i32'" at
        // Ln 0. Say what was meant, where it was written.
        if (op == TOKEN_AMPERSAND_AMPERSAND)
            _parser_expect(parser, true, "`&&` is not an operator in Lain: logical AND is spelled `and`");
        if (op == TOKEN_PIPE_PIPE)
            _parser_expect(parser, true, "`||` is not an operator in Lain: logical OR is spelled `or`");

        int prec = get_precedence(op);
        if (prec < precedence) break;

        // The OPERATOR's position, as C compilers report it. A sub-expression had none: only
        // the whole expression and identifiers were stamped, so every diagnostic about an
        // inner operation said Ln 0.
        isize op_line = parser->line, op_col = parser->column;
        parser_advance();  // consume this operator
        Expr *right = parse_binary_expr(arena, parser, prec + 1);
        // `x in lo..hi` / `x in lo..=hi`: membership in a RANGE (DECIDE-X, I.77). The range binds
        // tighter than the comparison, its bounds as tightly as an additive operand: `x in 0..n + 1`
        // is `x in 0..(n + 1)`. A range is not a value, so it exists only on the right of `in`
        // (and in `for` headers, slices and comprehensions).
        if (op == TOKEN_KEYWORD_IN && (parser_match(TOKEN_DOT_DOT) || parser_match(TOKEN_DOT_DOT_EQUAL))) {
            bool inclusive = parser->token.kind == TOKEN_DOT_DOT_EQUAL;
            isize r_line = right->line, r_col = right->col;
            parser_advance();
            Expr *hi = parse_binary_expr(arena, parser, prec + 1);
            right = expr_range(arena, right, hi, inclusive);
            right->line = r_line; right->col = r_col;
        }
        left = expr_binary(arena, op, left, right);
        left->line = op_line; left->col = op_col;

        // Handle postfix `as` cast after each binary sub-expression
        if (parser_match(TOKEN_KEYWORD_AS)) {
            parser_advance();
            Type *target = parse_type(arena, parser);
            Expr *operand = left;
            left = expr_cast(arena, left, target);
            left->line = operand->line; left->col = operand->col;
        }
    }

    return left;
}


// <op> <expr>
static Expr *parse_unary_expr_inner(Arena* arena, Parser* parser);
Expr *parse_unary_expr(Arena* arena, Parser* parser)
{
    // A unary node takes the position of its first token (see the binary loop's note).
    isize u_line = parser->line, u_col = parser->column;
    if (parser_match(TOKEN_AMPERSAND_AMPERSAND))
        _parser_expect(parser, true, "`&&` is not an operator in Lain: logical AND is spelled `and`");
    Expr *e = parse_unary_expr_inner(arena, parser);
    if (e && e->line == 0) { e->line = u_line; e->col = u_col; }
    return e;
}
static Expr *parse_unary_expr_inner(Arena* arena, Parser* parser)
{
    // -, !, ~  (arithmetic/logic unary)
    if (parser_match(TOKEN_MINUS) || parser_match(TOKEN_BANG) || parser_match(TOKEN_TILDE)) {
        TokenKind op = parser->token.kind;
        parser_advance();
        Expr *right = parse_unary_expr(arena, parser);
        return expr_unary(arena, op, right);
    }

    // &expr — address-of (produces a pointer to an array element)
    if (parser_match(TOKEN_AMPERSAND)) {
        parser_advance();
        Expr *right = parse_unary_expr(arena, parser);
        return expr_addr(arena, right);
    }

    // *expr — pointer dereference (safe when ptr in arr proven via in-guard)
    if (parser_match(TOKEN_ASTERISK)) {
        parser_advance();
        Expr *right = parse_unary_expr(arena, parser);
        return expr_deref(arena, right);
    }

    // mov <expr>
    if (parser_match(TOKEN_KEYWORD_MOV)) {
        parser_advance();
        Expr *right = parse_unary_expr(arena, parser);
        return expr_move(arena, right);
    }

    if (parser_match(TOKEN_KEYWORD_VAR)) {
        parser_advance();
        Expr *right = parse_unary_expr(arena, parser);
        return expr_mut(arena, right);
    }

    // try <expr> — propagate a `T | markers` union's markers to the enclosing
    // return. Binds tighter than binary ops so `try f() + 1` is `(try f()) + 1`.
    if (parser_match(TOKEN_KEYWORD_TRY)) {
        parser_advance();
        Expr *right = parse_unary_expr(arena, parser);
        return expr_try(arena, right);
    }

    return parse_primary_expr(arena, parser);
}

// Is `e` a call to the `panic` builtin? Used to tag `x else panic(...)`, whose
// arm aborts and therefore need not yield a value of the payload type.
static bool expr_is_panic_call(Expr *e) {
    if (!e || e->kind != EXPR_CALL) return false;
    Expr *c = e->as.call_expr.callee;
    return c && c->kind == EXPR_IDENTIFIER &&
           c->as.identifier_expr.id->length == 5 &&
           strncmp(c->as.identifier_expr.id->name, "panic", 5) == 0;
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
            fprintf(stderr, "[E100] Error Ln %li, Col %li: a `case` pattern with no body ends the "
                    "block; give it an arm.\n", parser->line, parser->column);
            exit(1);
        }
        
        parser_expect(TOKEN_R_BRACE, "Expected '}' after case expression block");
        parser_advance();
        
        return expr_match(arena, value, first, is_borrowed);
    }

    // Boolean literals — values 1 and 0, but typed `bool` (see the EXPR_LITERAL typing)
    if (parser_match(TOKEN_KEYWORD_TRUE) || parser_match(TOKEN_KEYWORD_FALSE)) {
        bool v = parser_match(TOKEN_KEYWORD_TRUE);
        parser_advance();
        Expr *b = expr_literal(arena, v ? 1 : 0);
        b->as.literal_expr.is_bool = true;
        return b;
    }
    if (parser_match(TOKEN_KEYWORD_NIL)) {
        parser_error("`nil` is retired — write `none` (the marker) for the absent case of `T | none`.");
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
        long long value = parse_numeric_literal(parser->token.start, parser->token.length);
        parser_advance();
        return expr_literal(arena, value);
    }
    else if (parser_match(TOKEN_FLOAT_LITERAL)) {
        // ★ The separators go before strtod reads the digits. strtod stops at the first `_`,
        // so `1_000.5` became 1.0 and `var x f64 = 1_000.5` compiled with x == 1 — silently.
        char fb[128]; int fl = 0;
        for (isize k = 0; k < parser->token.length && fl < (int)sizeof fb - 1; k++)
            if (parser->token.start[k] != '_') fb[fl++] = parser->token.start[k];
        fb[fl] = 0;
        double value = strtod(fb, NULL);
        parser_advance();
        return expr_float_literal(arena, value);
    }   
    else if (parser_match(TOKEN_STRING_LITERAL)) {  // New branch for strings
        const char* str = parser->token.start;
        isize len = parser->token.length;
        // Validate escape sequences (spec §5.9.6): a backslash not followed by
        // one of the recognized escape characters is ill-formed. The raw lexeme
        // includes the surrounding quotes, so scan the interior [1, len-1).
        for (isize i = 1; i + 1 < len; i++) {
            if (str[i] == '\\') {
                char e = str[i + 1];
                if (e != 'n' && e != 't' && e != 'r' && e != '0' &&
                    e != '\\' && e != '"' && e != '\'' && e != 'x') {
                    parser_error("unknown escape sequence in string literal");
                }
                i++;  // consume the escaped character
            }
        }
        // DECODE the escapes into the bytes the string actually denotes. They were stored
        // RAW, so `"ok\n"` was nine bytes with a literal backslash in it: `.len` was wrong
        // for every string containing an escape, and the only reason the output looked right
        // is that the old emitter pastes the raw text into a C literal and lets the C
        // compiler decode it — which the new backend, correctly escaping what it is given,
        // does not do. The IR should hold the real bytes.
        { char *dec = arena_push_many(arena, char, len + 1); isize dn = 0;
          for (isize i = 0; i < len; i++) {
              if (str[i] != '\\' || i + 1 >= len) { dec[dn++] = str[i]; continue; }
              char e = str[++i];
              switch (e) {
                  case 'n':  dec[dn++] = '\n'; break;
                  case 't':  dec[dn++] = '\t'; break;
                  case 'r':  dec[dn++] = '\r'; break;
                  case '0':  dec[dn++] = '\0'; break;
                  case '\\': dec[dn++] = '\\'; break;
                  case '"':  dec[dn++] = '"';  break;
                  case '\'': dec[dn++] = '\''; break;
                  case 'x': {                    // \xHH
                      int v = 0, k = 0;
                      while (k < 2 && i + 1 < len) {
                          char h = str[i+1];
                          int d = (h>='0'&&h<='9') ? h-'0'
                                : (h>='a'&&h<='f') ? h-'a'+10
                                : (h>='A'&&h<='F') ? h-'A'+10 : -1;
                          if (d < 0) break;
                          v = v*16 + d; i++; k++;
                      }
                      dec[dn++] = (char)v; break;
                  }
                  default: dec[dn++] = '\\'; dec[dn++] = e; break;   // validated above
              }
          }
          dec[dn] = 0;
          parser_advance();
          return expr_string(arena, dec, dn);
        }
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
                case '0':  c = 0;    break;   // Annex A escape-sequence; strings had it,
                case '"':  c = '"';  break;   // character literals did not
                case 'x':
                    if (len < 6) parser_error("incomplete \\xHH escape");
                    c = (unsigned char)((from_hex(s[3]) << 4) | from_hex(s[4]));
                    break;
                default:
                    parser_error("unknown escape sequence in char literal");
            }
        }
        // ★ EXACTLY ONE character or one escape (spec 05; I.83, Documentation). Only the empty
        // literal was refused: `'ab'` compiled and was 97, the first character, and a
        // multi-byte UTF-8 character (`'é'`) was its first BYTE. The compiler changed what the
        // source said and printed nothing. The literal's length is fixed by its first character:
        // 3 for a plain one, 4 for `\n`, 6 for `\xHH`.
        {
            isize want = (s[1] != '\\') ? 3 : (s[2] == 'x' ? 6 : 4);
            if (len != want) {
                fprintf(stderr, "[E100] Error Ln %li, Col %li: a character literal holds exactly one "
                        "character or one escape; %.*s holds more, and would have been read as its "
                        "first. For several characters write a string, \"...\".\n",
                        (long)parser->line, (long)parser->column, (int)len, s);
                exit(1);
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
        parser_skip_eol();          // allow a multi-line array literal
        ExprList *elements = NULL;
        ExprList **tail = &elements;
        if (!parser_match(TOKEN_R_BRACKET)) {
            Expr *first = parse_expr(arena, parser);
            // Array comprehension: [ body for idx in start..end ]
            if (parser_match(TOKEN_KEYWORD_FOR)) {
                parser_advance();  // consume 'for'
                parser_expect(TOKEN_IDENTIFIER, "Expected loop variable after 'for' in comprehension");
                Id *idx = id(arena, parser->token.length, parser->token.start);
                parser_advance();
                parser_expect(TOKEN_KEYWORD_IN, "Expected 'in' after comprehension loop variable");
                parser_advance();
                Expr *start = parse_expr(arena, parser);
                Expr *range = start;
                if (parser_match(TOKEN_DOT_DOT)) {
                    parser_advance();
                    range = expr_range(arena, start, parse_expr(arena, parser), false);
                } else if (parser_match(TOKEN_DOT_DOT_EQUAL)) {
                    parser_advance();
                    range = expr_range(arena, start, parse_expr(arena, parser), true);
                } else {
                    parser_error("Expected a range `start..end` in array comprehension");
                }
                parser_expect(TOKEN_R_BRACKET, "Expected ']' after comprehension");
                parser_advance();
                Expr *comp = expr_array_comprehension(arena, first, idx, range);
                comp->line = arr_line;
                comp->col  = arr_col;
                return comp;
            }
            // Otherwise: array literal; `first` is the first element.
            *tail = expr_list(arena, first);
            tail = &(*tail)->next;
            parser_skip_eol();
            if (parser_match(TOKEN_COMMA)) {
                parser_advance();
                parser_skip_eol();
                while (!parser_match(TOKEN_R_BRACKET)) {
                    Expr *elem = parse_expr(arena, parser);
                    *tail = expr_list(arena, elem);
                    tail = &(*tail)->next;
                    parser_skip_eol();
                    if (parser_match(TOKEN_COMMA)) { parser_advance(); parser_skip_eol(); }
                    else break;
                }
            }
        }
        parser_skip_eol();
        parser_expect(TOKEN_R_BRACKET, "Expected ']' after array literal");
        parser_advance();
        Expr *arr = expr_array_literal(arena, elements);
        arr->line = arr_line;
        arr->col  = arr_col;
        return arr;
    }
    else if (parser_match(TOKEN_IDENTIFIER)) {
        // 1) get the base identifier
        isize id_line = parser->line, id_col = parser->column;
        Id *identifier = id(arena, parser->token.length, parser->token.start);
        parser_advance();
        Expr *expr = expr_identifier(arena, identifier);
        // Record the source location so diagnostics (e.g. E106 undeclared) can
        // point at the identifier; without this, operands of larger expressions
        // were left at line 0 and slipped past the real-location check.
        expr->line = id_line;
        expr->col  = id_col;
    
        // Single postfix loop: handles .field, (call), [index] in any order.
        // Supports chained expressions like p.data[i].val or f(x)[0].name.
        while (true) {
            if (parser_match(TOKEN_DOT)) {
                parser_advance();  // consume '.'
                parser_expect(TOKEN_IDENTIFIER, "Expected identifier after '.'");
                Id *field_id = id(arena, parser->token.length, parser->token.start);
                parser_advance();
                expr = expr_member(arena, expr, field_id);
                // Each link of a postfix chain takes the chain's first token, as a unary node does.
                // Only the outermost link got one (parse_unary_expr), so an error about an inner
                // one, `std.math` in `std.math.max(1, 2)`, said Ln 0.
                expr->line = id_line; expr->col = id_col;

            } else if (parser_match(TOKEN_L_PAREN)) {
                parser_advance(); // consume '('
                ExprList* args = NULL;
                ExprList** args_tail = &args;
                if (!parser_match(TOKEN_R_PAREN)) {
                    do {
                        Expr *arg = parse_expr(arena, parser);
                        *args_tail = expr_list(arena, arg);
                        args_tail = &(*args_tail)->next;
                        if (parser_match(TOKEN_COMMA)) parser_advance();
                        else break;
                    } while (true);
                }
                parser_expect(TOKEN_R_PAREN, "Expected ')' after function call arguments");
                parser_advance(); // consume ')'
                Expr *callee = expr;
                expr = expr_call(arena, expr, args);
                // A call inside a chain (`G(Quad).Has(x)`) had no position: its diagnostics
                // said "Ln 0, Col 0". It is where its callee is.
                if (callee) { expr->line = callee->line; expr->col = callee->col; }

            } else if (parser_match(TOKEN_L_BRACKET)) {
                parser_advance();  // consume '['
                Expr *idx_expr = NULL;
                Expr *start = NULL;
                Expr *end   = NULL;
                if (parser_match(TOKEN_DOT_DOT)) {
                    // "[..end]" – empty start
                    parser_advance();  // consume '..'
                    end = parse_expr(arena, parser);
                } else {
                    start = parse_expr(arena, parser);
                    if (parser_match(TOKEN_DOT_DOT)) {
                        // slice: start .. [maybe end]
                        parser_advance();  // consume '..'
                        if (!parser_match(TOKEN_R_BRACKET)) {
                            end = parse_expr(arena, parser);
                        }
                    } else {
                        // plain index
                        idx_expr = start;
                    }
                }
                parser_expect(TOKEN_R_BRACKET, "Expected ']' after index or slice");
                parser_advance();  // consume ']'
                if (!idx_expr && (start || end)) {
                    idx_expr = expr_range(arena, start, end, /*inclusive=*/false);
                }
                expr = expr_index(arena, expr, idx_expr);
                expr->line = id_line; expr->col = id_col;

            } else {
                break;
            }
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
    else if (parser_match(TOKEN_AT)) {
        isize at_line = parser->line, at_col = parser->column;   // where the builtin is named
        parser_advance(); // consume '@'
        parser_expect(TOKEN_IDENTIFIER, "Expected builtin name after '@'");
        const char *name = parser->token.start;
        isize len = parser->token.length;
        parser_advance(); // consume identifier

        if (len == 2 && strncmp(name, "os", 2) == 0) {
            return expr_builtin(arena, BUILTIN_OS);
        } else if (len == 4 && strncmp(name, "arch", 4) == 0) {
            return expr_builtin(arena, BUILTIN_ARCH);
        } else if ((len == 6 && strncmp(name, "likely", 6) == 0) ||
                   (len == 8 && strncmp(name, "unlikely", 8) == 0)) {
            BuiltinKind bk = (len == 6) ? BUILTIN_LIKELY : BUILTIN_UNLIKELY;
            parser_expect(TOKEN_L_PAREN, "Expected '(' after '@likely'/'@unlikely'");
            parser_advance(); // consume '('
            Expr *arg = parse_expr(arena, parser);
            parser_expect(TOKEN_R_PAREN, "Expected ')' after @likely/@unlikely argument");
            parser_advance(); // consume ')'
            return expr_builtin_arg(arena, bk, arg);
        } else if (len == 14 && strncmp(name, "assume_aligned", 14) == 0) {
            // @assume_aligned(ptr, N) → __builtin_assume_aligned(ptr, N)
            parser_expect(TOKEN_L_PAREN, "Expected '(' after '@assume_aligned'");
            parser_advance(); // consume '('
            Expr *ptr_arg = parse_expr(arena, parser);
            parser_expect(TOKEN_COMMA, "Expected ',' after pointer argument in '@assume_aligned'");
            parser_advance(); // consume ','
            parser_expect(TOKEN_NUMBER, "Expected alignment integer literal in '@assume_aligned'");
            isize align_val = (isize)parse_numeric_literal(parser->token.start, parser->token.length);
            parser_advance(); // consume N
            parser_expect(TOKEN_R_PAREN, "Expected ')' after '@assume_aligned' arguments");
            parser_advance(); // consume ')'
            return expr_builtin_assume_aligned(arena, ptr_arg, align_val);
        } else if (len == 4 && strncmp(name, "load", 4) == 0) {
            // @load(T, ptr, off) → read sizeof(T) bytes at ptr+off into a vector T.
            parser_expect(TOKEN_L_PAREN, "Expected '(' after '@load'");
            parser_advance(); // consume '('
            Type *vt = parse_type(arena, parser);
            parser_expect(TOKEN_COMMA, "Expected ',' after the vector type in '@load'");
            parser_advance(); // consume ','
            Expr *ptr = parse_expr(arena, parser);
            parser_expect(TOKEN_COMMA, "Expected ',' after the pointer in '@load'");
            parser_advance(); // consume ','
            Expr *off = parse_expr(arena, parser);
            parser_expect(TOKEN_R_PAREN, "Expected ')' after '@load' arguments");
            parser_advance(); // consume ')'
            Expr *e = expr_builtin_arg(arena, BUILTIN_LOAD, ptr);
            e->as.builtin_expr.vec_type = vt;
            e->as.builtin_expr.arg2 = off;
            return e;
        } else if (len == 5 && strncmp(name, "splat", 5) == 0) {
            // @splat(T, x) → a vector T with every lane = x.
            parser_expect(TOKEN_L_PAREN, "Expected '(' after '@splat'");
            parser_advance();
            Type *vt = parse_type(arena, parser);
            parser_expect(TOKEN_COMMA, "Expected ',' after the vector type in '@splat'");
            parser_advance();
            Expr *x = parse_expr(arena, parser);
            parser_expect(TOKEN_R_PAREN, "Expected ')' after '@splat' arguments");
            parser_advance();
            Expr *e = expr_builtin_arg(arena, BUILTIN_SPLAT, x);
            e->as.builtin_expr.vec_type = vt;
            return e;
        } else if (len == 5 && strncmp(name, "store", 5) == 0) {
            // @store(ptr, off, v) → write vector v to ptr+off.
            parser_expect(TOKEN_L_PAREN, "Expected '(' after '@store'");
            parser_advance();
            Expr *ptr = parse_expr(arena, parser);
            parser_expect(TOKEN_COMMA, "Expected ',' after the pointer in '@store'");
            parser_advance();
            Expr *off = parse_expr(arena, parser);
            parser_expect(TOKEN_COMMA, "Expected ',' after the offset in '@store'");
            parser_advance();
            Expr *v = parse_expr(arena, parser);
            parser_expect(TOKEN_R_PAREN, "Expected ')' after '@store' arguments");
            parser_advance();
            Expr *e = expr_builtin_arg(arena, BUILTIN_STORE, ptr);
            e->as.builtin_expr.arg2 = off;
            e->as.builtin_expr.arg3 = v;
            return e;
        } else if (len == 7 && strncmp(name, "shuffle", 7) == 0) {
            // @shuffle(tbl, idx) → per-lane table lookup (pshufb).
            parser_expect(TOKEN_L_PAREN, "Expected '(' after '@shuffle'");
            parser_advance();
            Expr *tbl = parse_expr(arena, parser);
            parser_expect(TOKEN_COMMA, "Expected ',' after the table in '@shuffle'");
            parser_advance();
            Expr *idx = parse_expr(arena, parser);
            parser_expect(TOKEN_R_PAREN, "Expected ')' after '@shuffle' arguments");
            parser_advance();
            Expr *e = expr_builtin_arg(arena, BUILTIN_SHUFFLE, tbl);
            e->as.builtin_expr.arg2 = idx;
            return e;
        } else if ((len == 3 && strncmp(name, "ctz", 3) == 0) ||
                   (len == 3 && strncmp(name, "clz", 3) == 0) ||
                   (len == 8 && strncmp(name, "popcount", 8) == 0) ||
                   (len == 8 && strncmp(name, "movemask", 8) == 0)) {
            // Single-arg SIMD / bit-scan intrinsics: @ctz @clz @popcount @movemask.
            BuiltinKind bk = (len == 3 && name[1] == 't') ? BUILTIN_CTZ
                           : (len == 3 && name[1] == 'l') ? BUILTIN_CLZ
                           : (name[0] == 'p')             ? BUILTIN_POPCOUNT
                                                          : BUILTIN_MOVEMASK;
            parser_expect(TOKEN_L_PAREN, "Expected '(' after a SIMD/bit builtin");
            parser_advance(); // consume '('
            Expr *arg = parse_expr(arena, parser);
            parser_expect(TOKEN_R_PAREN, "Expected ')' after the builtin argument");
            parser_advance(); // consume ')'
            return expr_builtin_arg(arena, bk, arg);
        } else if ((len == 6 && strncmp(name, "sizeof", 6) == 0) ||
                   (len == 7 && strncmp(name, "alignof", 7) == 0)) {
            // @sizeof(T) / @alignof(T) — a TYPE's size and alignment, as the C backend lays it
            // out. A layout claim written as a comment (`// 8 bytes`) drifts silently; one
            // written as `@sizeof(Token)` in a test is checked every build.
            bool is_size = (len == 6);
            parser_expect(TOKEN_L_PAREN, is_size ? "Expected '(' after '@sizeof'" : "Expected '(' after '@alignof'");
            parser_advance();
            Type *t = parse_type(arena, parser);
            parser_expect(TOKEN_R_PAREN, "Expected ')' after the type");
            parser_advance();
            Expr *e = expr_builtin(arena, is_size ? BUILTIN_SIZEOF : BUILTIN_ALIGNOF);
            e->as.builtin_expr.vec_type = t;
            return e;
        } else {
            fprintf(stderr, "[E106] Error Ln %li, Col %li: unknown builtin '@%.*s'.\n",
                    at_line, at_col, (int)len, name);
            exit(1);
        }
    }

    {
        const char *tname = token_kind_name(parser->token.kind);
        fprintf(stderr, "[E100] Error Ln %li, Col %li: Unexpected token in expression: %s (%d)\n",
                parser->line, parser->column,
                tname ? tname : "UNKNOWN_TOKEN",
                parser->token.kind);
    }
    exit(1);
    return NULL;
}

#endif // PARSER_EXPR_H
 
