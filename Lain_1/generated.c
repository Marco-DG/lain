#include "lain.h"

typedef enum {
    src_main_Token_Kind_Tag_EOF,
    src_main_Token_Kind_Tag_Error,
    src_main_Token_Kind_Tag_Identifier,
    src_main_Token_Kind_Tag_Number,
    src_main_Token_Kind_Tag_String,
    src_main_Token_Kind_Tag_Plus,
    src_main_Token_Kind_Tag_Minus,
} src_main_Token_Kind_Tag;

typedef struct {
    src_main_Token_Kind_Tag tag;
} src_main_Token_Kind;

static inline src_main_Token_Kind src_main_Token_Kind_EOF() {
    return (src_main_Token_Kind){ .tag = src_main_Token_Kind_Tag_EOF };
}

static inline src_main_Token_Kind src_main_Token_Kind_Error() {
    return (src_main_Token_Kind){ .tag = src_main_Token_Kind_Tag_Error };
}

static inline src_main_Token_Kind src_main_Token_Kind_Identifier() {
    return (src_main_Token_Kind){ .tag = src_main_Token_Kind_Tag_Identifier };
}

static inline src_main_Token_Kind src_main_Token_Kind_Number() {
    return (src_main_Token_Kind){ .tag = src_main_Token_Kind_Tag_Number };
}

static inline src_main_Token_Kind src_main_Token_Kind_String() {
    return (src_main_Token_Kind){ .tag = src_main_Token_Kind_Tag_String };
}

static inline src_main_Token_Kind src_main_Token_Kind_Plus() {
    return (src_main_Token_Kind){ .tag = src_main_Token_Kind_Tag_Plus };
}

static inline src_main_Token_Kind src_main_Token_Kind_Minus() {
    return (src_main_Token_Kind){ .tag = src_main_Token_Kind_Tag_Minus };
}

typedef struct src_main_Lexer {
    Slice_u8_0 source;
    uint8_t cursor;
    int line;
    int col;
} src_main_Lexer;

static inline src_main_Lexer src_main_Lexer_ctor(Slice_u8_0 source, uint8_t cursor, int line, int col) {
    return (src_main_Lexer){ .source = source, .cursor = cursor, .line = line, .col = col };
}

typedef struct src_main_Token {
    src_main_Token_Kind kind;
    Slice_u8 lexeme;
} src_main_Token;

static inline src_main_Token src_main_Token_ctor(src_main_Token_Kind kind, Slice_u8 lexeme) {
    return (src_main_Token){ .kind = kind, .lexeme = lexeme };
}

src_main_Lexer src_main_new_lexer(Slice_u8_0 source) {
    return src_main_Lexer_ctor(source, 0, 1, 1);
}

int main(void) {
    return 0;
}

