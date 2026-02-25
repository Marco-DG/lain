#ifndef LEXER_H
#define LEXER_H

#include "token.h"
  
#define SWITCH_STATE(_state) state = _state; break
#define RETURN_TOKEN(_kind)  token.kind = _kind; token.length = lexer->current - token.start; return token

typedef struct {
    const char   *text;
    const char   *current;
} Lexer;

typedef enum {
    STATE_START,
    STATE_IDENTIFIER,
    STATE_NUMBER,
    STATE_SINGLE_QUOTE,
    STATE_DOUBLE_QUOTE,
    STATE_LINE_COMMENT,
    STATE_MULTILINE_COMMENT,
    STATE_MULTILINE_COMMENT_END,
    STATE_EQUAL,
    STATE_ANGLE_BRACKET_LEFT,
    STATE_ANGLE_BRACKET_RIGHT,
    STATE_DOT,
    STATE_DOT_DOT,
    STATE_ASTERISK,
    STATE_SLASH,
    STATE_PERCENT,
    STATE_PLUS,
    STATE_MINUS,
    STATE_BANG,
    STATE_AMPERSAND,
    STATE_PIPE,
    STATE_CARET,
} LexerState;

Lexer lexer_new(const char* text) {
    return (Lexer) {
        .text    = text,
        .current = text
    };
}

void lexer_init(Lexer* lexer, const char* text) {
    lexer->text = text;
    lexer->current = text;
}

Token lexer_next(Lexer* lexer) {
    Token token;
    token.start = lexer->current;
    
    LexerState state = STATE_START;
    
    while (true) {
        char c = *lexer->current;
        lexer->current++;

        switch (state) {
            case STATE_START:
                switch (c) {
                    case 0:             RETURN_TOKEN(TOKEN_EOF);
                    case ' ':
                    case '\t':          token.start++; break;
                    case '\n':
                    case '\r':          RETURN_TOKEN(TOKEN_NEWLINE);
                    case 'a' ... 'z':
                    case 'A' ... 'Z':
                    case '_':           SWITCH_STATE(STATE_IDENTIFIER);
                    case '0':
                    case '1' ... '9':   SWITCH_STATE(STATE_NUMBER);
                    case '\'':          SWITCH_STATE(STATE_SINGLE_QUOTE);
                    case '"':           SWITCH_STATE(STATE_DOUBLE_QUOTE);
                    case '=':           SWITCH_STATE(STATE_EQUAL);
                    case '%':           SWITCH_STATE(STATE_PERCENT);
                    case '*':           SWITCH_STATE(STATE_ASTERISK);
                    case '/':           SWITCH_STATE(STATE_SLASH);
                    case '+':           SWITCH_STATE(STATE_PLUS);
                    case '-':           SWITCH_STATE(STATE_MINUS);
                    case '!':           SWITCH_STATE(STATE_BANG);
                    case '&':           SWITCH_STATE(STATE_AMPERSAND);
                    case '|':           SWITCH_STATE(STATE_PIPE);
                    case '^':           SWITCH_STATE(STATE_CARET);
                    case '<':           SWITCH_STATE(STATE_ANGLE_BRACKET_LEFT);
                    case '>':           SWITCH_STATE(STATE_ANGLE_BRACKET_RIGHT);
                    case '.':           SWITCH_STATE(STATE_DOT);
                    case '{':           RETURN_TOKEN(TOKEN_L_BRACE);
                    case '}':           RETURN_TOKEN(TOKEN_R_BRACE);
                    case '[':           RETURN_TOKEN(TOKEN_L_BRACKET);
                    case ']':           RETURN_TOKEN(TOKEN_R_BRACKET);
                    case '(':           RETURN_TOKEN(TOKEN_L_PAREN);
                    case ')':           RETURN_TOKEN(TOKEN_R_PAREN);
                    case ':':           RETURN_TOKEN(TOKEN_COLON);
                    case ',':           RETURN_TOKEN(TOKEN_COMMA);
                    case ';':           RETURN_TOKEN(TOKEN_SEMICOLON);
                    case '~':           RETURN_TOKEN(TOKEN_TILDE);
                    default:            RETURN_TOKEN(TOKEN_INVALID);
                }
                break;

            case STATE_IDENTIFIER:
                switch (c) {
                    case 'a' ... 'z':
                    case 'A' ... 'Z':
                    case '0' ... '9':
                    case '_':           break;
                    default:            lexer->current--;
                                        RETURN_TOKEN(token_match_keyword(token.start, lexer->current - token.start));
                }
                break;

            case STATE_NUMBER:
                switch (c) {
                    case '0' ... '9':   break;
                    case '.': {
                        // Check if this is a range operator (..) or a float decimal point
                        if (*lexer->current == '.') {
                            // It's '..' (range), backtrack the dot
                            lexer->current--;
                            RETURN_TOKEN(TOKEN_NUMBER);
                        }
                        // It's a decimal point, scan fractional digits
                        while (*lexer->current >= '0' && *lexer->current <= '9') {
                            lexer->current++;
                        }
                        RETURN_TOKEN(TOKEN_FLOAT_LITERAL);
                    }
                    default:            lexer->current--;
                                        RETURN_TOKEN(TOKEN_NUMBER);
                }
                break;

            case STATE_SINGLE_QUOTE:
                // keep scanning until we hit the *matching* closing '
                if (c == '\\') {
                    // skip over backslash-escape plus its following char
                    if (*lexer->current) lexer->current++;
                }
                else if (c == '\'') {
                    // *including* both quotes in the token
                    token.kind   = TOKEN_CHAR_LITERAL;
                    token.length = lexer->current - token.start;
                    return token;
                }
                // otherwise just loop and consume more
                break;
            

            case STATE_DOUBLE_QUOTE:
                switch (c) {
                    case '"': {
                        // We have reached the closing double quote.
                        // Skip the opening quote and exclude the closing quote.
                        token.start++;  // skip the opening quote
                        token.length = (lexer->current - token.start - 1);
                        token.kind = TOKEN_STRING_LITERAL;
                        return token;
                    }
                    default:            break;
                }
                break;

            case STATE_EQUAL:
                switch (c) {
                    case '=':           RETURN_TOKEN(TOKEN_EQUAL_EQUAL);
                    default:            lexer->current--;
                                        RETURN_TOKEN(TOKEN_EQUAL);
                }
                break;

            case STATE_ANGLE_BRACKET_LEFT:
                switch (c) {
                    case '=':           RETURN_TOKEN(TOKEN_ANGLE_BRACKET_LEFT_EQUAL);
                    default:            lexer->current--;
                                        RETURN_TOKEN(TOKEN_ANGLE_BRACKET_LEFT);
                }
                break;

            case STATE_ANGLE_BRACKET_RIGHT:
                switch (c) {
                    case '=':           RETURN_TOKEN(TOKEN_ANGLE_BRACKET_RIGHT_EQUAL);
                    default:            lexer->current--;
                                        RETURN_TOKEN(TOKEN_ANGLE_BRACKET_RIGHT);
                }
                break;

            case STATE_DOT:
                switch (c) {
                    case '.':       SWITCH_STATE(STATE_DOT_DOT); // got `..`
                    default:        lexer->current--;
                                    RETURN_TOKEN(TOKEN_DOT); // just `.`
                }
                break;

            case STATE_DOT_DOT:
                switch (c) {
                    case '=':       RETURN_TOKEN(TOKEN_DOT_DOT_EQUAL); // `..=`
                    default:        lexer->current--;
                                    RETURN_TOKEN(TOKEN_DOT_DOT);       // `..`
                }
                break;
            

            case STATE_ASTERISK:
                switch (c) {
                    case '=':           RETURN_TOKEN(TOKEN_ASTERISK_EQUAL);
                    default:            lexer->current--;
                                        RETURN_TOKEN(TOKEN_ASTERISK);
                }
                break;

            case STATE_SLASH:
                switch (c) {
                    case '/':
                        // start single‑line comment
                        SWITCH_STATE(STATE_LINE_COMMENT);
            
                    case '*': {
                        // start nested multiline comment; token.start already set
                        int nesting = 1;
                        while (nesting > 0) {
                            char d = *lexer->current;
                            if (d == '/' && lexer->current[1] == '*') {
                                nesting++;
                                lexer->current += 2;
                            } else if (d == '*' && lexer->current[1] == '/') {
                                nesting--;
                                lexer->current += 2;
                            } else if (d == 0) {
                                // EOF before close
                                break;
                            } else {
                                lexer->current++;
                            }
                        }
                        // return the full comment token
                        token.kind   = TOKEN_MULTILINE_COMMENT;
                        token.length = lexer->current - token.start;
                        return token;
                    }
            
                    case '=':
                        RETURN_TOKEN(TOKEN_SLASH_EQUAL);
            
                    default:
                        lexer->current--;
                        RETURN_TOKEN(TOKEN_SLASH);
                }
                break;
            

            case STATE_LINE_COMMENT:
                switch (c) {
                    case '\n':
                    case '\r':
                    case 0:             lexer->current--;
                                        RETURN_TOKEN(TOKEN_LINE_COMMENT);
                    default:            break;
                }
                break;

//            case STATE_MULTILINE_COMMENT:
//                switch (c) {
//                    case '*':           SWITCH_STATE(STATE_MULTILINE_COMMENT_END);
//                    default:            break;
//                }
//                break;
//
//            case STATE_MULTILINE_COMMENT_END:
//                switch (c) {
//                    case '/':           RETURN_TOKEN(TOKEN_MULTILINE_COMMENT);
//                    default:            SWITCH_STATE(STATE_MULTILINE_COMMENT);
//                }
//                break;

            case STATE_PERCENT:
                switch (c) {
                    case '=':           RETURN_TOKEN(TOKEN_PERCENT_EQUAL);
                    default:            lexer->current--;
                                        RETURN_TOKEN(TOKEN_PERCENT);
                }
                break;

            case STATE_PLUS:
                switch (c) {
                    case '=':           RETURN_TOKEN(TOKEN_PLUS_EQUAL);
                    default:            lexer->current--;
                                        RETURN_TOKEN(TOKEN_PLUS);
                }
                break;

            case STATE_MINUS:
                switch (c) {
                    case '=':           RETURN_TOKEN(TOKEN_MINUS_EQUAL);
                    default:            lexer->current--;
                                        RETURN_TOKEN(TOKEN_MINUS);
                }
                break;

            case STATE_BANG:
                switch(c) {
                    case '=':           RETURN_TOKEN(TOKEN_BANG_EQUAL);
                    default:            lexer->current--;
                                        RETURN_TOKEN(TOKEN_BANG);
                }
                break;

            case STATE_AMPERSAND:
                switch (c) {
                    case '=':           RETURN_TOKEN(TOKEN_AMPERSAND_EQUAL);
                    default:            lexer->current--;
                                        RETURN_TOKEN(TOKEN_AMPERSAND);
                }
                break;

            case STATE_PIPE:
                switch (c) {
                    case '=':           RETURN_TOKEN(TOKEN_PIPE_EQUAL);
                    default:            lexer->current--;
                                        RETURN_TOKEN(TOKEN_PIPE);
                }
                break;

            case STATE_CARET:
                switch (c) {
                    case '=':           RETURN_TOKEN(TOKEN_CARET_EQUAL);
                    default:            lexer->current--;
                                        RETURN_TOKEN(TOKEN_CARET);
                }
                break;

            default:                    RETURN_TOKEN(TOKEN_INVALID);
        }
    }
}

Token lexer_peek(Lexer* lexer) {
    Lexer temp = *lexer; // Create a copy of the lexer state
    return lexer_next(&temp); // Return the next token without modifying the original lexer
}

#endif /* LEXER_H */