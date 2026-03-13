# Plan: Data-Oriented FSM Lexer

## Philosophy

- **Zero-copy tokens**: Token = `{ kind, start, end }` — just offsets into source buffer
- **Table-driven dispatch**: char classification via case expressions (compile to jump tables in C)
- **FSM structure**: explicit state enum, single `while` loop, state transitions
- **No allocations**: lexer is a flat struct, tokens are value types
- **Cache-friendly**: linear scan, no pointer chasing, no backtracking
- **Keyword recognition**: length-switch + first-char dispatch (flattened trie, same as C compiler's own `identify_keyword`)

## Data Layout

```
Lexer {
    src  u8[:0]     // source buffer — never copied, never modified
    pos  int        // current byte offset
    len  int        // cached src.len (avoid repeated field access)
}

Token {
    kind  int       // TokenKind as int — no enum overhead at runtime
    start int       // byte offset into source
    end   int       // byte offset into source (exclusive)
}
```

No row/col tracking in the hot path. Line/column can be computed on demand from the byte offset (a cold-path function for error reporting).

## Character Classification

A single `classify(c int) int` function using a case expression with ranges:

```
CharClass:
  0 = EOF
  1 = ALPHA      ('a'..'z', 'A'..'Z', '_')
  2 = DIGIT      ('0'..'9')
  3 = SPACE      (' ', '\t', '\r', '\n')
  4 = OTHER      (everything else — operators, delimiters, quotes)
```

The case expression compiles to a jump table — effectively a 256-entry lookup.

## FSM States

Not an explicit state enum (overkill for a lexer). Instead: **classify-then-dispatch** at the top of each `scan()` call. The "state" is implicit in which scanning proc is running:

1. `scan()` — top-level: skip whitespace, classify first char, dispatch
2. `scan_ident()` — eat while ALPHA|DIGIT, then keyword lookup
3. `scan_number()` — eat while DIGIT
4. `scan_string()` — eat until closing `"` or EOF
5. `scan_op()` — single/double char operators via lookahead

Each scanner is a tight loop with a single exit — no recursion, no callbacks.

## Keyword Recognition (Flattened Trie)

Switch on lexeme length, then switch on first character, then compare remaining bytes:

```
len 2: "if", "in", "as", "or"
len 3: "var", "for", "mov", "and", "not", "mod"
len 4: "func", "proc", "case", "else", "true", "type", "bool"
len 5: "while", "break", "false", "defer", "comptime"  -- comptime is 8
len 6: "return", "import", "extern", "unsafe", "struct"
...
```

Manual byte comparison (`lexeme[0] == 'v' and lexeme[1] == 'a' and lexeme[2] == 'r'`) — no string comparison function needed.

## Operator Dispatch

Single case on first char, with one-char lookahead for compound operators:

```
'+' → peek '=' ? PlusEq : Plus
'-' → peek '>' ? Arrow : (peek '=' ? MinusEq : Minus)
'=' → peek '=' ? EqEq : Eq
'!' → peek '=' ? BangEq : Bang
'<' → peek '=' ? LessEq : Less
'>' → peek '=' ? GreaterEq : Greater
'.' → peek '.' ? (advance; peek '=' ? DotDotEq : DotDot) : Dot
'&' → Amp
'|' → Pipe
```

## Token Kinds (int constants)

Use plain int constants grouped by category. Adding a new token = add one constant + one case arm.

## main.ln

- Tokenize a sample string in a tight loop
- Print `kind start:end "lexeme"` for each token
- token_name() maps kind int to a string for display

## What this is NOT:
- No Unicode — byte-oriented, ASCII only
- No float literals yet
- No block comments yet
- No escape sequence processing in strings (just find the closing quote)
