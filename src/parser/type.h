#ifndef PARSER_TYPE_H
#define PARSER_TYPE_H

#include "../parser.h"

// Forward declaration: defined in parser/expr.h (included after this file in parser.h)
Expr *parse_expr(Arena *arena, Parser *parser);

// parse any (possibly nested) type e.g. `Foo[][]`, `Bar:[3]`, and `mov Foo`
Type *parse_type(Arena *arena, Parser *parser) {
  Type *base_type = NULL;



  // 1) prefixes: pointers, type variables, etc.

  // Type variable: 'T — token includes the leading ' (skip it for the name)
  if (parser_match(TOKEN_TYPEVAR)) {
      Id *var_name = id(arena, parser->token.length - 1, parser->token.start + 1);
      parser_advance();
      return type_var(arena, var_name);
  }

  if (parser_match(TOKEN_ASTERISK)) {
    parser_advance();
    Type *inner = parse_type(arena, parser);
    // *T[] and *T[:S] collapse: the * is syntactic (these are already reference types).
    // *T[N] (pointer to fixed array) keeps the TYPE_POINTER wrapper.
    if ((inner->kind == TYPE_ARRAY && inner->array_len < 0) || inner->kind == TYPE_SLICE) {
        return inner;
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

  // 2) parse a simple identifier type (e.g. "Foo", "int")
  // 2) parse a simple identifier type (e.g. "Foo", "int", "std.sub.Type") or "type"
  if (parser_match(TOKEN_KEYWORD_TYPE)) {
      parser_advance();
      base_type = type_meta_type(arena);
  } else {
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