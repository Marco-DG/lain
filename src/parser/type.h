#ifndef PARSER_TYPE_H
#define PARSER_TYPE_H

#include "../parser.h"

// Forward declaration: defined in parser/expr.h (included after this file in parser.h)
Expr *parse_expr(Arena *arena, Parser *parser);

static Type *parse_type_core(Arena *arena, Parser *parser);

// SIMD vector sugar `<prim>x<N>` (u8x32, i32x4, f32x8) = Vec(N, prim). Returns a
// TYPE_VECTOR on a match, or NULL if `name` is not the sugar form (then it is an
// ordinary type name). The element must be a scalar primitive iN/uN (N=1..64) or
// f32/f64 — no primitive contains an 'x', so splitting at the first 'x' is safe,
// and a user type like `foox3` fails the primitive check and stays a plain name.
static Type *try_parse_vector_sugar(Arena *arena, const char *name, isize len) {
    isize xp = -1;
    for (isize k = 1; k < len - 1; k++) if (name[k] == 'x') { xp = k; break; }
    if (xp < 0) return NULL;

    // element = name[0 .. xp)
    isize elen = xp;
    bool ok_elem = false;
    if ((name[0] == 'i' || name[0] == 'u') && elen >= 2) {
        bool digits = true; int v = 0;
        for (isize k = 1; k < elen; k++) {
            if (name[k] < '0' || name[k] > '9') { digits = false; break; }
            v = v * 10 + (name[k] - '0');
        }
        if (digits && v >= 1 && v <= 64) ok_elem = true;
    } else if (elen == 3 && (strncmp(name, "f32", 3) == 0 || strncmp(name, "f64", 3) == 0)) {
        ok_elem = true;
    }
    if (!ok_elem) return NULL;

    // lanes = name[xp+1 .. len), all digits, > 0
    long lanes = 0; bool ld = false;
    for (isize k = xp + 1; k < len; k++) {
        if (name[k] < '0' || name[k] > '9') return NULL;
        lanes = lanes * 10 + (name[k] - '0'); ld = true;
    }
    if (!ld || lanes <= 0) return NULL;

    Id *eid = id(arena, elen, name);
    return type_vector(arena, (isize)lanes, type_simple(arena, eid));
}

// A type is a core type optionally followed by union markers: `T | m1 | m2`.
// `|` binds looser than every prefix/suffix, so `*File | none` = `(*File) | none`
// and `T[] | A` = `(T[]) | A`. This is the one construct for optionality
// (`T | none`) and errors (`T | NotFound | Denied`).
Type *parse_type(Arena *arena, Parser *parser) {
  Type *t = parse_type_core(arena, parser);
  if (parser_match(TOKEN_PIPE)) {
    IdList *markers = NULL, **mt = &markers;
    while (parser_match(TOKEN_PIPE)) {
      parser_advance();  // consume '|'
      parser_expect(TOKEN_IDENTIFIER, "Expected a marker name after '|' in a union type");
      Id *m = id(arena, parser->token.length, parser->token.start);
      parser_advance();
      IdList *node = arena_push_aligned(arena, IdList);
      node->id = m; node->next = NULL; node->fields = NULL;
      // F3.4 payload marker: `Marker(field type, ...)` — data-carrying error.
      // Parens (not braces) avoid ambiguity with a following block `{`, and match
      // the construction syntax `Marker(v)`.
      if (parser_match(TOKEN_L_PAREN)) {
        parser_advance();  // consume '('
        DeclList **ft = &node->fields;
        while (!parser_match(TOKEN_R_PAREN) && !parser_match(TOKEN_EOF)) {
          parser_skip_eol();
          if (parser_match(TOKEN_R_PAREN)) break;
          parser_expect(TOKEN_IDENTIFIER, "Expected marker field name");
          Id *fname = id(arena, parser->token.length, parser->token.start);
          parser_advance();
          Type *ftype = parse_type(arena, parser);
          *ft = decl_list(arena, decl_variable(arena, fname, ftype));
          ft = &(*ft)->next;
          if (parser_match(TOKEN_COMMA)) parser_advance();
          else if (!parser_match(TOKEN_R_PAREN)) parser_skip_eol();
        }
        parser_expect(TOKEN_R_PAREN, "Expected ')' after marker fields");
        parser_advance();
      }
      *mt = node; mt = &node->next;
    }
    t = type_union(arena, t, markers);
  }
  return t;
}

// parse any (possibly nested) type e.g. `Foo[][]`, `Bar:[3]`, and `mov Foo`
static Type *parse_type_core(Arena *arena, Parser *parser) {
  Type *base_type = NULL;



  // 1) prefixes: pointers, etc.

  if (parser_match(TOKEN_ASTERISK)) {
    parser_advance();

    // *func(P..)R / *proc(P..)R — non-capturing function-pointer type.
    // `func` encodes totality (provably terminating); `proc` may diverge.
    if (parser_match(TOKEN_KEYWORD_FUNC) || parser_match(TOKEN_KEYWORD_PROC)) {
      bool is_total = parser_match(TOKEN_KEYWORD_FUNC);
      parser_advance(); // consume func/proc
      parser_expect(TOKEN_L_PAREN, "Expected '(' after 'func'/'proc' in function-pointer type");
      parser_advance(); // consume '('
      TypeList *params = NULL, *tail = NULL;
      if (!parser_match(TOKEN_R_PAREN)) {
        for (;;) {
          Type *pt = parse_type_core(arena, parser);
          TypeList *node = type_list(arena, pt);
          if (!params) params = node; else tail->next = node;
          tail = node;
          if (parser_match(TOKEN_COMMA)) { parser_advance(); continue; }
          break;
        }
      }
      parser_expect(TOKEN_R_PAREN, "Expected ')' after function-pointer parameter types");
      parser_advance(); // consume ')'
      // Optional return type: present iff the next token can start a type.
      Type *ret = NULL;
      if (parser_match(TOKEN_IDENTIFIER) || parser_match(TOKEN_ASTERISK) ||
          parser_match(TOKEN_KEYWORD_MOV) || parser_match(TOKEN_KEYWORD_VAR) ||
          parser_match(TOKEN_QUESTION)) {
        ret = parse_type_core(arena, parser);
      }
      return type_func(arena, params, ret, is_total);
    }

    Type *inner = parse_type_core(arena, parser);
    // `*` is a RAW POINTER, and only that. Arrays and slices are always passed
    // by reference, so they never take a `*`: a fixed array is `T[N]`, a slice
    // `T[]` / `T[n]` / `T[>= n]`, a sentinel string `T[:S]`. The `*` belongs
    // solely to raw pointers: `*T`, `**T`, `*void`, and function pointers
    // (handled above). Reject `*` before any array or slice type.
    if (inner->kind == TYPE_ARRAY || inner->kind == TYPE_SLICE) {
        parser_error("'*' is not allowed before an array or slice type. "
                     "Arrays and slices are always by reference: write 'T[N]', "
                     "'T[]', or 'T[:S]' without '*'. '*' is only for raw pointers "
                     "('*T', '*void', function pointers).");
    }
    return type_pointer(arena, inner);
  }
  
  if (parser_match(TOKEN_KEYWORD_MOV)) {
    parser_advance();
    Type *inner = parse_type_core(arena, parser);
    return type_move(arena, inner);
  }

  if (parser_match(TOKEN_KEYWORD_VAR)) {
    parser_advance();
    Type *inner = parse_type_core(arena, parser);
    return type_mut(arena, inner);
  }

  // `?T` is retired: there is one construct, `T | markers`. Write `T | none`.
  if (parser_match(TOKEN_QUESTION)) {
    parser_error("`?T` is not a type — write `T | none` (a value, or the `none` marker). "
                 "Optionality and errors are the one construct `T | markers`.");
  }

  // The meta-type `type` — the type of a type parameter (`T type`). A parameter
  // of this type is a generic type parameter.
  if (parser_match(TOKEN_KEYWORD_TYPE)) {
    parser_advance();
    return type_meta(arena);
  }

  // 2) parse a simple identifier type (e.g. "Foo", "int", "std.sub.Type")
  parser_expect(TOKEN_IDENTIFIER, "Expected type name");
  Token start = parser->token;
  parser_advance();

  Token end = start;
  while (parser_match(TOKEN_DOT)) {
      parser_advance(); // .
      parser_expect(TOKEN_IDENTIFIER, "Expected identifier after dot");
      end = parser->token;
      parser_advance();
  }

  // A dotted type name is module-qualified (`dep.Widget`) — the real type is the
  // LAST segment, which the glob binds by its bare name (there are no nested
  // types, so a dot here always means module qualification).
  Id *type_name = id(arena, end.length, end.start);

  base_type = type_simple(arena, type_name);

  // Builtin SIMD vector sugar `<prim>x<N>` (u8x32, i32x4, …) = Vec(N, prim).
  // Only a bare, unqualified name (start.start == end.start ⇒ no dotted qualifier).
  if (start.start == end.start) {
    Type *sugar = try_parse_vector_sugar(arena, end.start, end.length);
    if (sugar) return sugar;
  }

  // Builtin SIMD vector `Vec(N, T)` — N lanes of element type T. Recognized here,
  // before generic type-application, because N is a NUMBER (not a type), so the
  // generic-arg path (which parses every argument as a type) cannot express it.
  // Only the bare, unqualified `Vec` is the builtin (start.start == end.start ⇒
  // no dotted qualifier); `mymod.Vec` stays a user type.
  if (start.start == end.start && end.length == 3 && strncmp(end.start, "Vec", 3) == 0
      && parser_match(TOKEN_L_PAREN)) {
    parser_advance(); // '('
    parser_expect(TOKEN_NUMBER, "Vec(N, T): expected a lane count N");
    isize lanes = (isize)parse_numeric_literal(parser->token.start, parser->token.length);
    parser_advance();
    parser_expect(TOKEN_COMMA, "Vec(N, T): expected ',' after the lane count");
    parser_advance();
    Type *elem = parse_type_core(arena, parser);
    parser_expect(TOKEN_R_PAREN, "Vec(N, T): expected ')' after the element type");
    parser_advance();
    return type_vector(arena, lanes, elem);
  }

  // Generic type-application in type position: `Name(T1, T2, …)` (e.g. Vec(i32)).
  if (parser_match(TOKEN_L_PAREN)) {
    parser_advance(); // '('
    TypeList *targs = NULL, *tt = NULL;
    if (!parser_match(TOKEN_R_PAREN)) {
      for (;;) {
        Type *arg = parse_type_core(arena, parser);
        TypeList *node = type_list(arena, arg);
        if (!targs) targs = node; else tt->next = node;
        tt = node;
        if (parser_match(TOKEN_COMMA)) { parser_advance(); continue; }
        break;
      }
    }
    parser_expect(TOKEN_R_PAREN, "Expected ')' after type arguments");
    parser_advance();
    base_type = type_application(arena, type_name, targs);
  }

  // 3) allow array/slice suffixes
  while (parser_match(TOKEN_L_BRACKET)) {
    parser_advance(); // consume '['

    if (parser_match(TOKEN_COLON)) {
      // slice with a compile-time sentinel (string, char, or number)
      parser_advance(); // consume ':'

      if (parser_match(TOKEN_STRING_LITERAL) ||
          parser_match(TOKEN_CHAR_LITERAL) || parser_match(TOKEN_NUMBER)) {
        const char *full = parser->token.start;
        isize full_len = parser->token.length;
        bool is_str = parser->token.kind == TOKEN_STRING_LITERAL;
        bool is_char = parser->token.kind == TOKEN_CHAR_LITERAL;

        // determine sentinel content + length
        const char *sentinel_str;
        isize sentinel_len;
        if (is_str || is_char) {
          // skip opening quote, exclude closing quote
          sentinel_str = full + 1;
          sentinel_len = full_len - 2;
        } else {
          sentinel_str = full;
          sentinel_len = full_len;
        }

        parser_advance(); // consume the literal

        parser_expect(TOKEN_R_BRACKET, "Expected ']' after slice sentinel");
        parser_advance(); // consume ']'

        // build a slice type
        base_type = type_slice(arena, base_type, sentinel_str, sentinel_len,
                               /*is_string_or_char=*/is_str || is_char);
      } else {
        parser_expect(
            TOKEN_STRING_LITERAL,
            "Expected string, char, or number literal after ':' in slice type");
      }

    } else {
      // [N] fixed, [] plain dynamic, or [expr]/[relop expr] sized slice
      isize array_len = -1;
      Expr *size_expr = NULL;
      TokenKind size_relop = TOKEN_EQUAL_EQUAL; // default: equality

      if (parser_match(TOKEN_NUMBER)) {
        // F-004: use numeric-literal parser to support hex/bin/oct/underscore
        array_len = (isize)parse_numeric_literal(parser->token.start,
                                                  parser->token.length);
        parser_advance(); // consume the number
      } else if (!parser_match(TOKEN_R_BRACKET)) {
        // Anything before ']' that is not a number is a size constraint.
        // Optional leading relational operator: i32[>= n], i32[< n]
        TokenKind tok = parser->token.kind;
        if (tok == TOKEN_ANGLE_BRACKET_LEFT         ||
            tok == TOKEN_ANGLE_BRACKET_LEFT_EQUAL   ||
            tok == TOKEN_ANGLE_BRACKET_RIGHT        ||
            tok == TOKEN_ANGLE_BRACKET_RIGHT_EQUAL  ||
            tok == TOKEN_BANG_EQUAL                 ||
            tok == TOKEN_EQUAL_EQUAL) {
          size_relop = tok;
          parser_advance();
        }
        size_expr = parse_expr(arena, parser);
      }

      parser_expect(TOKEN_R_BRACKET, "Expected ']' after '[' in array type");
      parser_advance(); // consume ']'

      if (size_expr) {
        base_type = type_sized_array(arena, base_type, size_expr, size_relop);
      } else {
        base_type = type_array(arena, base_type, array_len);
      }
    }
  }

  return base_type;
}

#endif // PARSER_TYPE_H