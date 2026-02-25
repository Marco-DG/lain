#include "lain.h"

typedef enum {
    Lain_1_src_main_TokenKind_Tag_EOF,
    Lain_1_src_main_TokenKind_Tag_Error,
    Lain_1_src_main_TokenKind_Tag_Id,
    Lain_1_src_main_TokenKind_Tag_Num,
    Lain_1_src_main_TokenKind_Tag_Str,
    Lain_1_src_main_TokenKind_Tag_Plus,
    Lain_1_src_main_TokenKind_Tag_Minus,
    Lain_1_src_main_TokenKind_Tag_Star,
    Lain_1_src_main_TokenKind_Tag_Slash,
    Lain_1_src_main_TokenKind_Tag_Eq,
    Lain_1_src_main_TokenKind_Tag_LParen,
    Lain_1_src_main_TokenKind_Tag_RParen,
    Lain_1_src_main_TokenKind_Tag_LBrace,
    Lain_1_src_main_TokenKind_Tag_RBrace,
    Lain_1_src_main_TokenKind_Tag_Comma,
    Lain_1_src_main_TokenKind_Tag_Colon,
    Lain_1_src_main_TokenKind_Tag_Semi,
} Lain_1_src_main_TokenKind_Tag;

typedef struct {
    Lain_1_src_main_TokenKind_Tag tag;
} Lain_1_src_main_TokenKind;

static inline Lain_1_src_main_TokenKind Lain_1_src_main_TokenKind_EOF() {
    return (Lain_1_src_main_TokenKind){ .tag = Lain_1_src_main_TokenKind_Tag_EOF };
}

static inline Lain_1_src_main_TokenKind Lain_1_src_main_TokenKind_Error() {
    return (Lain_1_src_main_TokenKind){ .tag = Lain_1_src_main_TokenKind_Tag_Error };
}

static inline Lain_1_src_main_TokenKind Lain_1_src_main_TokenKind_Id() {
    return (Lain_1_src_main_TokenKind){ .tag = Lain_1_src_main_TokenKind_Tag_Id };
}

static inline Lain_1_src_main_TokenKind Lain_1_src_main_TokenKind_Num() {
    return (Lain_1_src_main_TokenKind){ .tag = Lain_1_src_main_TokenKind_Tag_Num };
}

static inline Lain_1_src_main_TokenKind Lain_1_src_main_TokenKind_Str() {
    return (Lain_1_src_main_TokenKind){ .tag = Lain_1_src_main_TokenKind_Tag_Str };
}

static inline Lain_1_src_main_TokenKind Lain_1_src_main_TokenKind_Plus() {
    return (Lain_1_src_main_TokenKind){ .tag = Lain_1_src_main_TokenKind_Tag_Plus };
}

static inline Lain_1_src_main_TokenKind Lain_1_src_main_TokenKind_Minus() {
    return (Lain_1_src_main_TokenKind){ .tag = Lain_1_src_main_TokenKind_Tag_Minus };
}

static inline Lain_1_src_main_TokenKind Lain_1_src_main_TokenKind_Star() {
    return (Lain_1_src_main_TokenKind){ .tag = Lain_1_src_main_TokenKind_Tag_Star };
}

static inline Lain_1_src_main_TokenKind Lain_1_src_main_TokenKind_Slash() {
    return (Lain_1_src_main_TokenKind){ .tag = Lain_1_src_main_TokenKind_Tag_Slash };
}

static inline Lain_1_src_main_TokenKind Lain_1_src_main_TokenKind_Eq() {
    return (Lain_1_src_main_TokenKind){ .tag = Lain_1_src_main_TokenKind_Tag_Eq };
}

static inline Lain_1_src_main_TokenKind Lain_1_src_main_TokenKind_LParen() {
    return (Lain_1_src_main_TokenKind){ .tag = Lain_1_src_main_TokenKind_Tag_LParen };
}

static inline Lain_1_src_main_TokenKind Lain_1_src_main_TokenKind_RParen() {
    return (Lain_1_src_main_TokenKind){ .tag = Lain_1_src_main_TokenKind_Tag_RParen };
}

static inline Lain_1_src_main_TokenKind Lain_1_src_main_TokenKind_LBrace() {
    return (Lain_1_src_main_TokenKind){ .tag = Lain_1_src_main_TokenKind_Tag_LBrace };
}

static inline Lain_1_src_main_TokenKind Lain_1_src_main_TokenKind_RBrace() {
    return (Lain_1_src_main_TokenKind){ .tag = Lain_1_src_main_TokenKind_Tag_RBrace };
}

static inline Lain_1_src_main_TokenKind Lain_1_src_main_TokenKind_Comma() {
    return (Lain_1_src_main_TokenKind){ .tag = Lain_1_src_main_TokenKind_Tag_Comma };
}

static inline Lain_1_src_main_TokenKind Lain_1_src_main_TokenKind_Colon() {
    return (Lain_1_src_main_TokenKind){ .tag = Lain_1_src_main_TokenKind_Tag_Colon };
}

static inline Lain_1_src_main_TokenKind Lain_1_src_main_TokenKind_Semi() {
    return (Lain_1_src_main_TokenKind){ .tag = Lain_1_src_main_TokenKind_Tag_Semi };
}

typedef struct Lain_1_src_main_Lexer {
    Slice_u8_0 src;
    int pos;
    int row;
    int col;
} Lain_1_src_main_Lexer;

static inline Lain_1_src_main_Lexer Lain_1_src_main_Lexer_ctor(Slice_u8_0 src, int pos, int row, int col) {
    return (Lain_1_src_main_Lexer){ .src = src, .pos = pos, .row = row, .col = col };
}

typedef struct Lain_1_src_main_Token {
    Lain_1_src_main_TokenKind kind;
    Slice_u8 lexeme;
} Lain_1_src_main_Token;

static inline Lain_1_src_main_Token Lain_1_src_main_Token_ctor(Lain_1_src_main_TokenKind kind, Slice_u8 lexeme) {
    return (Lain_1_src_main_Token){ .kind = kind, .lexeme = lexeme };
}

Lain_1_src_main_Lexer Lain_1_src_main_init_lexer(Slice_u8_0 s) {
    return Lain_1_src_main_Lexer_ctor(s, 0, 1, 1);
}

_Bool Lain_1_src_main_is_alpha(int c) {
    return ((((c >= 'a') && (c <= 'z')) || ((c >= 'A') && (c <= 'Z'))) || (c == '_'));
}

_Bool Lain_1_src_main_is_digit(int c) {
    return ((c >= '0') && (c <= '9'));
}

_Bool Lain_1_src_main_is_alnum(int c) {
    return (Lain_1_src_main_is_alpha(c) || Lain_1_src_main_is_digit(c));
}

void Lain_1_src_main_skip_space(Lain_1_src_main_Lexer * l) {
    while ((l->pos < l->src.len)) {
        int c = ((int)(l->src.data[l->pos]));
        if (((((c == ' ') || (c == '\t')) || (c == '\r')) || (c == '\n'))) {
            l->pos = (l->pos + 1);
            l->col = (l->col + 1);
            if ((c == '\n')) {
                l->row = (l->row + 1);
                l->col = 1;
            }
        } else {
            break;
        }
    }
}

Lain_1_src_main_Token Lain_1_src_main_next_token(Lain_1_src_main_Lexer * l) {
    Lain_1_src_main_skip_space(l);
    if ((l->pos >= l->src.len)) {
        return Lain_1_src_main_Token_ctor(Lain_1_src_main_TokenKind_EOF(), (Slice_u8){ .data = l->src.data + l->pos, .len = l->pos - l->pos });
    }
    int start = l->pos;
    int c = ((int)(l->src.data[l->pos]));
    l->pos = (l->pos + 1);
    l->col = (l->col + 1);
    Lain_1_src_main_TokenKind kind = Lain_1_src_main_TokenKind_Error();
    int __match0 = c;
    if (__match0 == '+') {
        kind = Lain_1_src_main_TokenKind_Plus();
    }
    else if (__match0 == '-') {
        kind = Lain_1_src_main_TokenKind_Minus();
    }
    else if (__match0 == '*') {
        kind = Lain_1_src_main_TokenKind_Star();
    }
    else if (__match0 == '/') {
        kind = Lain_1_src_main_TokenKind_Slash();
    }
    else if (__match0 == '=') {
        kind = Lain_1_src_main_TokenKind_Eq();
    }
    else if (__match0 == '(') {
        kind = Lain_1_src_main_TokenKind_LParen();
    }
    else if (__match0 == ')') {
        kind = Lain_1_src_main_TokenKind_RParen();
    }
    else if (__match0 == '{') {
        kind = Lain_1_src_main_TokenKind_LBrace();
    }
    else if (__match0 == '}') {
        kind = Lain_1_src_main_TokenKind_RBrace();
    }
    else if (__match0 == ',') {
        kind = Lain_1_src_main_TokenKind_Comma();
    }
    else if (__match0 == ':') {
        kind = Lain_1_src_main_TokenKind_Colon();
    }
    else if (__match0 == ';') {
        kind = Lain_1_src_main_TokenKind_Semi();
    }
    else if (__match0 == '"') {
        while ((l->pos < l->src.len)) {
            int nc = ((int)(l->src.data[l->pos]));
            l->pos = (l->pos + 1);
            l->col = (l->col + 1);
            if ((nc == '"')) {
                break;
            }
        }
        kind = Lain_1_src_main_TokenKind_Str();
    }
    else {
        if (Lain_1_src_main_is_alpha(c)) {
            while (((l->pos < l->src.len) && Lain_1_src_main_is_alnum(((int)(l->src.data[l->pos]))))) {
                l->pos = (l->pos + 1);
                l->col = (l->col + 1);
            }
            kind = Lain_1_src_main_TokenKind_Id();
        } else         if (Lain_1_src_main_is_digit(c)) {
            while (((l->pos < l->src.len) && Lain_1_src_main_is_digit(((int)(l->src.data[l->pos]))))) {
                l->pos = (l->pos + 1);
                l->col = (l->col + 1);
            }
            kind = Lain_1_src_main_TokenKind_Num();
        }
    }
    return Lain_1_src_main_Token_ctor(kind, (Slice_u8){ .data = l->src.data + start, .len = l->pos - start });
}

extern int printf(const char * fmt, ...);
extern FILE * fopen(const char * filename, const char * mode);
extern int fclose(FILE * stream);
extern int fputs(const char * s, FILE * stream);
extern char * fgets(char * s, int n, FILE * stream);
extern int libc_printf(const char * fmt, ...);
extern int libc_puts(const char * s);
int main(void) {
    Slice_u8_0 source = (Slice_u8_0){ .len = 42, .data = (uint8_t[]){ 0x76, 0x61, 0x72, 0x20, 0x63, 0x6F, 0x75, 0x6E, 0x74, 0x20, 0x3D, 0x20, 0x31, 0x30, 0x30, 0x3B, 0x20, 0x69, 0x66, 0x20, 0x63, 0x6F, 0x75, 0x6E, 0x74, 0x20, 0x7B, 0x20, 0x70, 0x72, 0x69, 0x6E, 0x74, 0x28, 0x63, 0x6F, 0x75, 0x6E, 0x74, 0x29, 0x20, 0x7D, 0 } };
    Lain_1_src_main_Lexer lex = Lain_1_src_main_init_lexer(source);
    libc_printf("---- Lexer Bootstrap Test ----\n");
    libc_printf("Source:\n%s\n\n", source.data);
    int done = 0;
    while ((done == 0)) {
        Lain_1_src_main_Token t = Lain_1_src_main_next_token(&(lex));
        libc_printf("Token[%2d] | Lexeme: '%.*s'\n", t.kind, t.lexeme.len, t.lexeme.data);
        Lain_1_src_main_TokenKind __match1 = t.kind;
        if (__match1.tag == Lain_1_src_main_TokenKind_Tag_EOF) {
            done = 1;
        }
        else if (__match1.tag == Lain_1_src_main_TokenKind_Tag_Error) {
            libc_printf("Lexer error encountered.\n");
            done = 1;
        }
        else {
            0;
        }
    }
    return 0;
}

