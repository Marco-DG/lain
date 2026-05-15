# Self-hosted Lain compiler (in Lain)

Long-term: a complete Lain compiler written in Lain that ingests `.ln`
source and produces C99 source (matching the C-hosted compiler in
`../src/`).

Short-term: progressive build-up, one stage at a time.

## Structure

| File | Purpose | Status |
|------|---------|--------|
| `lexer.ln` | Tokenizer: `*u8` → stream of `Token` | Stage 1 (minimal) |
| `main.ln` | Test driver: runs lexer on a small input and prints tokens | Stage 1 |
| `parser.ln` | Token → AST | not started |
| `sema.ln` | AST → type-checked AST | not started |
| `emit.ln` | type-checked AST → C99 source | not started |

## Building

```
../lain compiler/main.ln -o /tmp/sh.c
gcc -o /tmp/sh /tmp/sh.c -Dlibc_printf=printf -w
/tmp/sh
```

## Stage 1 scope (lexer)

The minimal lexer recognizes:
- Identifiers: `[a-zA-Z_][a-zA-Z_0-9]*`
- Integer literals: `[0-9]+`
- Single-char punctuation: `+ - * / ( ) { } ; , .`
- Whitespace skip: ` \t\n\r`
- EOF terminator

Out of stage 1 (deferred to stage 2):
- String / char literals
- Multi-char punctuation (`==`, `..`, `..=`, etc.)
- Keywords (the lexer emits them as identifiers; the parser can classify)
- Comments
- Numeric literals beyond plain decimal
