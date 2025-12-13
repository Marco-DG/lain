#ifndef LEXER_EXPERIMENTAL_H
#define LEXER_EXPERIMENTAL_H

// Exploit:
//  All keywords are less than 8 bytes long - so each keyword fits into a single 64-bit register.
//  That means that keyword comparisons can be done with a single cmp instruction.
enum Keywords {
    Var,
    Func
};

// Exploit:
//  Statically generate a perfect hash function. I'm doing this with GNU's gperf.


// Exploit:
//  SIMD parallel lookup function

void lex(char* text, long length) {
    
}

#endif /* LEXER_EXPERIMENTAL_H */