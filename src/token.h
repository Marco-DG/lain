#ifndef TOKEN_H
#define TOKEN_H

#include "utils/common.h" // isize, strncmp

typedef enum {
    TOKEN_INVALID,
    TOKEN_EOL,
    TOKEN_EOF,
    TOKEN_NEWLINE,
    TOKEN_IDENTIFIER,
    TOKEN_NUMBER,
    TOKEN_CHAR_LITERAL,
    TOKEN_STRING_LITERAL,
    TOKEN_L_PAREN,
    TOKEN_R_PAREN,
    TOKEN_L_BRACKET,
    TOKEN_R_BRACKET,
    TOKEN_L_BRACE,
    TOKEN_R_BRACE,
    TOKEN_DOT,
    TOKEN_DOT_DOT,
    TOKEN_DOT_DOT_EQUAL,
    TOKEN_ELLIPSIS,
    TOKEN_COMMA,
    TOKEN_COLON,
    TOKEN_SEMICOLON,
    TOKEN_TILDE,
    TOKEN_EQUAL,
    TOKEN_EQUAL_EQUAL,
    TOKEN_ANGLE_BRACKET_LEFT,
    TOKEN_ANGLE_BRACKET_LEFT_EQUAL,
    TOKEN_ANGLE_BRACKET_RIGHT,
    TOKEN_ANGLE_BRACKET_RIGHT_EQUAL,
    TOKEN_ASTERISK,
    TOKEN_ASTERISK_EQUAL,
    TOKEN_SLASH,
    TOKEN_SLASH_EQUAL,
    TOKEN_PLUS,
    TOKEN_PLUS_EQUAL,
    TOKEN_MINUS,
    TOKEN_MINUS_EQUAL,
    TOKEN_BANG,
    TOKEN_BANG_EQUAL,
    TOKEN_AMPERSAND,
    TOKEN_AMPERSAND_EQUAL,
    TOKEN_PIPE,
    TOKEN_PIPE_EQUAL,
    TOKEN_CARET,
    TOKEN_CARET_EQUAL,
    TOKEN_PERCENT,
    TOKEN_PERCENT_EQUAL,
    TOKEN_LINE_COMMENT,
    TOKEN_MULTILINE_COMMENT,
    TOKEN_KEYWORD_IF,
    TOKEN_KEYWORD_IN,
    TOKEN_KEYWORD_AS,
    TOKEN_KEYWORD_OR,
    TOKEN_KEYWORD_USE,
    TOKEN_KEYWORD_END,
    TOKEN_KEYWORD_FOR,
    TOKEN_KEYWORD_VAR,
    TOKEN_KEYWORD_MOV,

    TOKEN_KEYWORD_AND,
    TOKEN_KEYWORD_TYPE,
    TOKEN_KEYWORD_FUNC,
    TOKEN_KEYWORD_PROC,
    TOKEN_KEYWORD_EXPR,
    TOKEN_KEYWORD_ELIF,
    TOKEN_KEYWORD_ELSE,
    TOKEN_KEYWORD_CASE,
    TOKEN_KEYWORD_MACRO,
    TOKEN_KEYWORD_IMPORT,
    TOKEN_KEYWORD_EXPORT,
    TOKEN_KEYWORD_EXTERN,
    TOKEN_KEYWORD_RETURN,
    TOKEN_KEYWORD_CONTINUE,
    TOKEN_KEYWORD_BREAK,
    TOKEN_KEYWORD_TRUE,
    TOKEN_KEYWORD_FALSE,
    TOKEN_KEYWORD_COMPTIME,
    TOKEN_KEYWORD_UNSAFE,
    TOKEN_KEYWORD_C_INCLUDE,
    TOKEN_KEYWORD_PRE,
    TOKEN_KEYWORD_POST,
    TOKEN_KEYWORD_WHILE,
} TokenKind;

typedef struct {
    TokenKind   kind;
    const char *start;
    isize       length;
} Token;

TokenKind token_match_keyword(const char* lexeme, isize len) {
    switch (len) {
        case 2:
            if (strncmp(lexeme, "if", 2) == 0)          return TOKEN_KEYWORD_IF;
            if (strncmp(lexeme, "in", 2) == 0)          return TOKEN_KEYWORD_IN;
            if (strncmp(lexeme, "as", 2) == 0)          return TOKEN_KEYWORD_AS;
            if (strncmp(lexeme, "or", 2) == 0)          return TOKEN_KEYWORD_OR;
            break;
        case 3:
            if (strncmp(lexeme, "end", 3) == 0)         return TOKEN_KEYWORD_END;
            if (strncmp(lexeme, "for", 3) == 0)         return TOKEN_KEYWORD_FOR;
            if (strncmp(lexeme, "var", 3) == 0)         return TOKEN_KEYWORD_VAR;
            if (strncmp(lexeme, "mov", 3) == 0)         return TOKEN_KEYWORD_MOV;

            if (strncmp(lexeme, "use", 3) == 0)         return TOKEN_KEYWORD_USE;
            if (strncmp(lexeme, "and", 3) == 0)         return TOKEN_KEYWORD_AND;
            if (strncmp(lexeme, "fun", 3) == 0)         return TOKEN_KEYWORD_FUNC; /* FIXME */
            if (strncmp(lexeme, "pre", 3) == 0)         return TOKEN_KEYWORD_PRE;
            break;
        case 4:
            if (strncmp(lexeme, "type", 4) == 0)        return TOKEN_KEYWORD_TYPE;
            if (strncmp(lexeme, "func", 4) == 0)        return TOKEN_KEYWORD_FUNC;
            if (strncmp(lexeme, "proc", 4) == 0)        return TOKEN_KEYWORD_PROC;
            if (strncmp(lexeme, "expr", 4) == 0)        return TOKEN_KEYWORD_EXPR;
            if (strncmp(lexeme, "elif", 4) == 0)        return TOKEN_KEYWORD_ELIF;
            if (strncmp(lexeme, "else", 4) == 0)        return TOKEN_KEYWORD_ELSE;
            if (strncmp(lexeme, "case", 4) == 0)        return TOKEN_KEYWORD_CASE;
            if (strncmp(lexeme, "post", 4) == 0)        return TOKEN_KEYWORD_POST;
            if (strncmp(lexeme, "true", 4) == 0)        return TOKEN_KEYWORD_TRUE;
            break;
        case 5:
            if (strncmp(lexeme, "break", 5) == 0)       return TOKEN_KEYWORD_BREAK;
            if (strncmp(lexeme, "false", 5) == 0)       return TOKEN_KEYWORD_FALSE;
            if (strncmp(lexeme, "macro", 5) == 0)       return TOKEN_KEYWORD_MACRO;
            if (strncmp(lexeme, "while", 5) == 0)       return TOKEN_KEYWORD_WHILE;
            break;
        case 6:
            if (strncmp(lexeme, "import", 6) == 0)      return TOKEN_KEYWORD_IMPORT;
            if (strncmp(lexeme, "export", 6) == 0)      return TOKEN_KEYWORD_EXPORT;
            if (strncmp(lexeme, "extern", 6) == 0)      return TOKEN_KEYWORD_EXTERN;
            if (strncmp(lexeme, "return", 6) == 0)      return TOKEN_KEYWORD_RETURN;
            if (strncmp(lexeme, "unsafe", 6) == 0)      return TOKEN_KEYWORD_UNSAFE;
            break;
        case 8:
            if (strncmp(lexeme, "continue", 8) == 0)    return TOKEN_KEYWORD_CONTINUE;
            if (strncmp(lexeme, "comptime", 8) == 0)    return TOKEN_KEYWORD_COMPTIME;
            break;
        case 9:
            if (strncmp(lexeme, "c_include", 9) == 0)   return TOKEN_KEYWORD_C_INCLUDE;
            break;
    }
    return TOKEN_IDENTIFIER;
}

const char* token_kind_name(TokenKind kind) {
    switch (kind) {
        case TOKEN_INVALID:                     return "TOKEN_INVALID";
        case TOKEN_EOL:                         return "TOKEN_EOL";
        case TOKEN_EOF:                         return "TOKEN_EOF";
        case TOKEN_NEWLINE:                     return "TOKEN_NEWLINE";
        case TOKEN_IDENTIFIER:                  return "TOKEN_IDENTIFIER";
        case TOKEN_NUMBER:                      return "TOKEN_NUMBER";
        case TOKEN_STRING_LITERAL:              return "TOKEN_STRING_LITERAL";
        case TOKEN_CHAR_LITERAL:                return "TOKEN_CHAR_LITERAL";
        case TOKEN_L_PAREN:                     return "TOKEN_L_PAREN";
        case TOKEN_R_PAREN:                     return "TOKEN_R_PAREN";
        case TOKEN_L_BRACKET:                   return "TOKEN_L_BRACKET";
        case TOKEN_R_BRACKET:                   return "TOKEN_R_BRACKET";
        case TOKEN_L_BRACE:                     return "TOKEN_L_BRACE";
        case TOKEN_R_BRACE:                     return "TOKEN_R_BRACE";
        case TOKEN_DOT:                         return "TOKEN_DOT";
        case TOKEN_DOT_DOT:                     return "TOKEN_DOT_DOT";
        case TOKEN_DOT_DOT_EQUAL:               return "TOKEN_DOT_DOT_EQUAL";
        case TOKEN_COMMA:                       return "TOKEN_COMMA";
        case TOKEN_COLON:                       return "TOKEN_COLON";
        case TOKEN_SEMICOLON:                   return "TOKEN_SEMICOLON";
        case TOKEN_TILDE:                       return "TOKEN_TILDE";
        case TOKEN_EQUAL:                       return "TOKEN_EQUAL";
        case TOKEN_EQUAL_EQUAL:                 return "TOKEN_EQUAL_EQUAL";
        case TOKEN_ANGLE_BRACKET_LEFT:          return "TOKEN_ANGLE_BRACKET_LEFT";
        case TOKEN_ANGLE_BRACKET_LEFT_EQUAL:    return "TOKEN_ANGLE_BRACKET_LEFT_EQUAL";
        case TOKEN_ANGLE_BRACKET_RIGHT:         return "TOKEN_ANGLE_BRACKET_RIGHT";
        case TOKEN_ANGLE_BRACKET_RIGHT_EQUAL:   return "TOKEN_ANGLE_BRACKET_RIGHT_EQUAL";
        case TOKEN_ASTERISK:                    return "TOKEN_ASTERISK";
        case TOKEN_ASTERISK_EQUAL:              return "TOKEN_ASTERISK_EQUAL";
        case TOKEN_SLASH:                       return "TOKEN_SLASH";
        case TOKEN_SLASH_EQUAL:                 return "TOKEN_SLASH_EQUAL";
        case TOKEN_PLUS:                        return "TOKEN_PLUS";
        case TOKEN_PLUS_EQUAL:                  return "TOKEN_PLUS_EQUAL";
        case TOKEN_MINUS:                       return "TOKEN_MINUS";
        case TOKEN_MINUS_EQUAL:                 return "TOKEN_MINUS_EQUAL";
        case TOKEN_BANG:                        return "TOKEN_BANG";
        case TOKEN_BANG_EQUAL:                  return "TOKEN_BANG_EQUAL";
        case TOKEN_AMPERSAND:                   return "TOKEN_AMPERSAND";
        case TOKEN_AMPERSAND_EQUAL:             return "TOKEN_AMPERSAND_EQUAL";
        case TOKEN_PIPE:                        return "TOKEN_PIPE";
        case TOKEN_PIPE_EQUAL:                  return "TOKEN_PIPE_EQUAL";
        case TOKEN_CARET:                       return "TOKEN_CARET";
        case TOKEN_CARET_EQUAL:                 return "TOKEN_CARET_EQUAL";
        case TOKEN_PERCENT:                     return "TOKEN_PERCENT";
        case TOKEN_PERCENT_EQUAL:               return "TOKEN_PERCENT_EQUAL";
        case TOKEN_LINE_COMMENT:                return "TOKEN_LINE_COMMENT";
        case TOKEN_MULTILINE_COMMENT:           return "TOKEN_MULTILINE_COMMENT";
        case TOKEN_KEYWORD_IF:                  return "TOKEN_KEYWORD_IF";
        case TOKEN_KEYWORD_IN:                  return "TOKEN_KEYWORD_IN";
        case TOKEN_KEYWORD_AS:                  return "TOKEN_KEYWORD_AS";
        case TOKEN_KEYWORD_OR:                  return "TOKEN_KEYWORD_OR";
        case TOKEN_KEYWORD_END:                 return "TOKEN_KEYWORD_END";
        case TOKEN_KEYWORD_FOR:                 return "TOKEN_KEYWORD_FOR";
        case TOKEN_KEYWORD_VAR:                 return "TOKEN_KEYWORD_VAR";
        case TOKEN_KEYWORD_MOV:                 return "TOKEN_KEYWORD_MOV";
        case TOKEN_KEYWORD_USE:                 return "TOKEN_KEYWORD_USE";
        case TOKEN_KEYWORD_AND:                 return "TOKEN_KEYWORD_AND";
        case TOKEN_KEYWORD_TYPE:                return "TOKEN_KEYWORD_TYPE";
        case TOKEN_KEYWORD_FUNC:                return "TOKEN_KEYWORD_FUNC";
        case TOKEN_KEYWORD_PROC:                return "TOKEN_KEYWORD_PROC";
        case TOKEN_KEYWORD_EXPR:                return "TOKEN_KEYWORD_EXPR";
        case TOKEN_KEYWORD_ELIF:                return "TOKEN_KEYWORD_ELIF";
        case TOKEN_KEYWORD_ELSE:                return "TOKEN_KEYWORD_ELSE";
        case TOKEN_KEYWORD_CASE:                return "TOKEN_KEYWORD_CASE";
        case TOKEN_KEYWORD_MACRO:               return "TOKEN_KEYWORD_MACRO";
        case TOKEN_KEYWORD_BREAK:               return "TOKEN_KEYWORD_BREAK";
        case TOKEN_KEYWORD_IMPORT:              return "TOKEN_KEYWORD_IMPORT";
        case TOKEN_KEYWORD_EXPORT:              return "TOKEN_KEYWORD_EXPORT";
        case TOKEN_KEYWORD_EXTERN:              return "TOKEN_KEYWORD_EXTERN";
        case TOKEN_KEYWORD_RETURN:              return "TOKEN_KEYWORD_RETURN";
        case TOKEN_KEYWORD_CONTINUE:            return "TOKEN_KEYWORD_CONTINUE";
        case TOKEN_KEYWORD_COMPTIME:            return "TOKEN_KEYWORD_COMPTIME";
        case TOKEN_KEYWORD_C_INCLUDE:           return "TOKEN_KEYWORD_C_INCLUDE";
        case TOKEN_KEYWORD_TRUE:                return "TOKEN_KEYWORD_TRUE";
        case TOKEN_KEYWORD_FALSE:               return "TOKEN_KEYWORD_FALSE";
        default:                                return 0;
    }
}

const char* token_kind_to_str(TokenKind kind) {
    switch (kind) {
        case TOKEN_INVALID:                     return "TOKEN_INVALID";
        case TOKEN_EOL:                         return "TOKEN_EOL";
        case TOKEN_EOF:                         return "TOKEN_EOF";
        case TOKEN_NEWLINE:                     return "TOKEN_NEWLINE";
        case TOKEN_IDENTIFIER:                  return "TOKEN_IDENTIFIER";
        case TOKEN_NUMBER:                      return "TOKEN_NUMBER";
        case TOKEN_CHAR_LITERAL:                return "TOKEN_CHAR_LITERAL";
        case TOKEN_STRING_LITERAL:              return "TOKEN_STRING_LITERAL";
        case TOKEN_L_PAREN:                     return "(";
        case TOKEN_R_PAREN:                     return ")";
        case TOKEN_L_BRACKET:                   return "[";
        case TOKEN_R_BRACKET:                   return "]";
        case TOKEN_L_BRACE:                     return "{";
        case TOKEN_R_BRACE:                     return "}";
        case TOKEN_DOT:                         return ".";
        case TOKEN_DOT_DOT:                     return "..";
        case TOKEN_DOT_DOT_EQUAL:               return "..=";
        case TOKEN_ELLIPSIS:                    return "...";
        case TOKEN_COMMA:                       return ",";
        case TOKEN_COLON:                       return ":";
        case TOKEN_SEMICOLON:                   return ";";
        case TOKEN_TILDE:                       return "~";
        case TOKEN_EQUAL:                       return "=";
        case TOKEN_EQUAL_EQUAL:                 return "==";
        case TOKEN_ANGLE_BRACKET_LEFT:          return "<";
        case TOKEN_ANGLE_BRACKET_LEFT_EQUAL:    return "<=";
        case TOKEN_ANGLE_BRACKET_RIGHT:         return ">";
        case TOKEN_ANGLE_BRACKET_RIGHT_EQUAL:   return ">=";
        case TOKEN_ASTERISK:                    return "*";
        case TOKEN_ASTERISK_EQUAL:              return "*=";
        case TOKEN_SLASH:                       return "/";
        case TOKEN_SLASH_EQUAL:                 return "/=";
        case TOKEN_PLUS:                        return "+";
        case TOKEN_PLUS_EQUAL:                  return "+=";
        case TOKEN_MINUS:                       return "-";
        case TOKEN_MINUS_EQUAL:                 return "-=";
        case TOKEN_BANG:                        return "!";
        case TOKEN_BANG_EQUAL:                  return "!=";
        case TOKEN_AMPERSAND:                   return "&";
        case TOKEN_AMPERSAND_EQUAL:             return "&=";
        case TOKEN_PIPE:                        return "|";
        case TOKEN_PIPE_EQUAL:                  return "|=";
        case TOKEN_CARET:                       return "^";
        case TOKEN_CARET_EQUAL:                 return "^=";
        case TOKEN_PERCENT:                     return "%";
        case TOKEN_PERCENT_EQUAL:               return "%=";
        case TOKEN_LINE_COMMENT:                return "TOKEN_LINE_COMMENT";
        case TOKEN_MULTILINE_COMMENT:           return "TOKEN_MULTILINE_COMMENT";
        case TOKEN_KEYWORD_IF:                  return "if";
        case TOKEN_KEYWORD_IN:                  return "in";
        case TOKEN_KEYWORD_AS:                  return "as";
        case TOKEN_KEYWORD_OR:                  return "or";
        case TOKEN_KEYWORD_END:                 return "end";
        case TOKEN_KEYWORD_FOR:                 return "for";
        case TOKEN_KEYWORD_VAR:                 return "var";
        case TOKEN_KEYWORD_MOV:                 return "mov";

        case TOKEN_KEYWORD_USE:                 return "use";
        case TOKEN_KEYWORD_AND:                 return "and";
        case TOKEN_KEYWORD_TYPE:                return "type";
        case TOKEN_KEYWORD_FUNC:                return "func";
        case TOKEN_KEYWORD_PROC:                return "proc";
        case TOKEN_KEYWORD_EXPR:                return "expr";
        case TOKEN_KEYWORD_ELIF:                return "elif";
        case TOKEN_KEYWORD_ELSE:                return "else";
        case TOKEN_KEYWORD_CASE:                return "case";
        case TOKEN_KEYWORD_MACRO:               return "macro";
        case TOKEN_KEYWORD_BREAK:               return "break";
        case TOKEN_KEYWORD_IMPORT:              return "import";
        case TOKEN_KEYWORD_EXPORT:              return "export";
        case TOKEN_KEYWORD_EXTERN:              return "extern";
        case TOKEN_KEYWORD_RETURN:              return "return";
        case TOKEN_KEYWORD_CONTINUE:            return "continue";
        case TOKEN_KEYWORD_COMPTIME:            return "comptime";
        case TOKEN_KEYWORD_UNSAFE:              return "unsafe";
        case TOKEN_KEYWORD_PRE:                 return "pre";
        case TOKEN_KEYWORD_POST:                return "post";
        case TOKEN_KEYWORD_C_INCLUDE:           return "c_include";
        case TOKEN_KEYWORD_TRUE:                return "true";
        case TOKEN_KEYWORD_FALSE:               return "false";
        default:                                return 0;
    }
}


#endif /* TOKEN_H */