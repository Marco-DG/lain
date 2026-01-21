#ifndef PARSER_TYPE_H
#define PARSER_TYPE_H

#include "../parser.h"

// parse any (possibly nested) type e.g. `Foo[][]`, `Bar:[3]`, and `mov Foo`
Type *parse_type(Arena *arena, Parser *parser) {
  Type *base_type = NULL;



  // 1) prefixes: pointers, etc.
  if (parser_match(TOKEN_ASTERISK)) {
    parser_advance();
    Type *inner = parse_type(arena, parser);
    return type_pointer(arena, inner);
  }
  
  if (parser_match(TOKEN_KEYWORD_MOV)) {
    parser_advance();
    Type *inner = parse_type(arena, parser);
    return type_move(arena, inner);
  }

  // 2) parse a simple identifier type (e.g. "Foo", "int")
  parser_expect(TOKEN_IDENTIFIER, "Expected type name");
  Id *type_name = id(arena, parser->token.length, parser->token.start);
  parser_advance();

  base_type = type_simple(arena, type_name);

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
      // Either a numeric fixed length like [5] or plain [] dynamic array
      isize array_len = -1; // -1 means dynamic / runtime-length ([])

      if (parser_match(TOKEN_NUMBER)) {
        // parse the integer literal token content (decimal)
        array_len = (isize)atoi(parser->token.start);
        parser_advance(); // consume the number
      }

      parser_expect(TOKEN_R_BRACKET, "Expected ']' after '[' in array type");
      parser_advance(); // consume ']'

      base_type = type_array(arena, base_type, array_len);
    }
  }

  return base_type;
}

#endif // PARSER_TYPE_H