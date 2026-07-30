#ifndef PARSER_TYPE_H
#define PARSER_TYPE_H

#include "../parser.h"

// Forward declaration: defined in parser/expr.h (included after this file in parser.h)
Expr *parse_expr(Arena *arena, Parser *parser);

// parse any (possibly nested) type e.g. `Foo[][]`, `Bar:[3]`, and `mov Foo`
Type *parse_type(Arena *arena, Parser *parser) {
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
          Type *pt = parse_type(arena, parser);
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
          parser_match(TOKEN_KEYWORD_MOV) || parser_match(TOKEN_KEYWORD_VAR)) {
        ret = parse_type(arena, parser);
      }
      return type_func(arena, params, ret, is_total);
    }

    Type *inner = parse_type(arena, parser);
    // *T[] and *T[:S] collapse: the * is syntactic (these are already reference types).
    // *T[N] (fixed) keeps the TYPE_POINTER wrapper.
    if ((inner->kind == TYPE_ARRAY && inner->array_len < 0) || inner->kind == TYPE_SLICE) {
        return inner;  // collapse *T[] and *T[:S]
    }
    return type_pointer(arena, inner);
  }
  
  if (parser_match(TOKEN_KEYWORD_MOV)) {
    parser_advance();
    Type *inner = parse_type(arena, parser);
    return type_move(arena, inner);
  }

  if (parser_match(TOKEN_KEYWORD_VAR)) {
    parser_advance();
    Type *inner = parse_type(arena, parser);
    return type_mut(arena, inner);
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

  // Combine into one Id based on source range
  isize len = (end.start + end.length) - start.start;
  Id *type_name = id(arena, len, start.start);

  base_type = type_simple(arena, type_name);

  // Generic type-application in type position: `Name(T1, T2, …)` (e.g. Vec(i32)).
  if (parser_match(TOKEN_L_PAREN)) {
    parser_advance(); // '('
    TypeList *targs = NULL, *tt = NULL;
    if (!parser_match(TOKEN_R_PAREN)) {
      for (;;) {
        Type *arg = parse_type(arena, parser);
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