# Chapter 1 — Lexical Structure

## 1.1 Source Encoding [Implemented]

A Lain source file shall be a sequence of bytes encoded in UTF-8. The compiler
processes source as a byte stream. Multi-byte UTF-8 characters are valid within
string literals and comments but shall not appear in identifiers.

The file extension for Lain source files is `.ln`.

## 1.2 Whitespace [Implemented]

Whitespace characters are space (U+0020) and horizontal tab (U+0009).
Whitespace serves to separate tokens and is otherwise ignored.

Newlines (U+000A line feed, U+000D carriage return) are significant: they
serve as implicit statement terminators (see §5.1). Multiple consecutive
newlines are treated as a single statement boundary.

Semicolons (`;`) are recognized as explicit statement terminators but are
optional. Newlines and semicolons are interchangeable as statement separators.

## 1.3 Comments [Implemented]

Lain supports two forms of comments:

### 1.3.1 Line Comments

A line comment begins with `//` and extends to the end of the line:

```lain
// This is a line comment
var x = 42  // This is also a comment
```

### 1.3.2 Block Comments

A block comment begins with `/*` and ends with `*/`. Block comments support
nesting — an inner `/*` opens a new nesting level, requiring a corresponding
`*/` to close:

```lain
/* This is a block comment */

/* This is a
   multi-line comment */

/* Nested /* comments */ are supported */
```

> **CONSTRAINT:** An unterminated block comment (reaching end-of-file before
> the outermost `*/`) shall produce a diagnostic.

Comments are stripped during lexical analysis and do not affect the semantics
of the program.

## 1.4 Identifiers [Implemented]

An identifier is a sequence of ASCII letters, digits, and underscores that
begins with a letter or underscore:

```
identifier = [a-zA-Z_][a-zA-Z0-9_]*
```

Identifiers are case-sensitive. `foo`, `Foo`, and `FOO` are three distinct
identifiers.

> **CONSTRAINT:** An identifier shall not be a keyword (see §1.5).

There is no maximum length for identifiers, but a conforming implementation
shall support identifiers of at least 255 characters.

## 1.5 Keywords [Implemented]

The following identifiers are reserved as keywords and shall not be used
as user-defined identifiers:

### 1.5.1 Implemented Keywords

| Keyword | Purpose |
|:--------|:--------|
| `and` | Logical AND operator |
| `as` | Type cast operator |
| `break` | Exit innermost loop |
| `case` | Pattern matching |
| `c_include` | C header inclusion |
| `comptime` | Compile-time parameter |
| `continue` | Skip to next loop iteration |
| `defer` | Deferred execution |
| `elif` | Else-if branch |
| `else` | Default branch |
| `extern` | C interoperability declarations |
| `false` | Boolean literal |
| `for` | Range-based loop |
| `func` | Pure function declaration |
| `fun` | Alias for `func` |
| `if` | Conditional |
| `import` | Module import |
| `in` | Range iteration / index bounds constraint |
| `mov` | Ownership transfer |
| `or` | Logical OR operator |
| `proc` | Procedure declaration |
| `return` | Return value from function/procedure |
| `true` | Boolean literal |
| `type` | Type definition |
| `undefined` | Explicit uninitialized marker |
| `unsafe` | Unsafe block |
| `var` | Mutable binding / mutable borrow mode |
| `while` | While loop |

### 1.5.2 Reserved Keywords

The following keywords are reserved for future use. They are recognized by the
lexer but have no defined semantics:

| Keyword | Intended Purpose |
|:--------|:-----------------|
| `end` | Block terminator (reserved) |
| `export` | Module visibility control |
| `expr` | Expression context (reserved) |
| `macro` | Macro definitions |
| `post` | Postcondition contracts |
| `pre` | Precondition contracts |
| `use` | Alias binding |

> **CONSTRAINT:** Using a reserved keyword as an identifier shall produce
> a diagnostic.

## 1.6 Literals [Implemented]

### 1.6.1 Integer Literals

Integer literals represent constant integer values:

```
integer_literal = decimal_literal | hex_literal
decimal_literal = digit { digit }
hex_literal     = "0x" hex_digit { hex_digit }
digit           = "0" | "1" | ... | "9"
hex_digit       = digit | "a" | ... | "f" | "A" | ... | "F"
```

Examples:
```lain
42          // decimal
0           // zero
0xFF        // hexadecimal (255)
0x1A3B      // hexadecimal
```

The default type of an integer literal is `int`. To use a different integer
type, an explicit cast is required (see §4.8).

> **CONSTRAINT:** An integer literal shall fit within the range of the `int`
> type (at least 32 bits).

### 1.6.2 Floating-Point Literals

Floating-point literals contain a decimal point:

```
float_literal = digit { digit } "." digit { digit }
```

Examples:
```lain
3.14
1.0
0.5
```

The default type of a floating-point literal is `f64`. A negative floating-point
literal is expressed as a unary minus applied to a positive literal: `-3.14`.

> Note: There is no scientific notation (e.g., `1e10`) in the current specification.

### 1.6.3 Boolean Literals

```lain
true        // Type: bool
false       // Type: bool
```

### 1.6.4 Character Literals

A character literal is a single byte enclosed in single quotes:

```lain
'a'         // Type: u8, value 97
'\n'        // Type: u8, value 10 (newline)
'\0'        // Type: u8, value 0 (null)
```

Character literals support escape sequences (see §1.6.6).

The type of a character literal is `u8`.

### 1.6.5 String Literals

A string literal is a sequence of bytes enclosed in double quotes:

```lain
"hello"         // Type: u8[:0], null-terminated byte slice
"line\n"        // Contains a newline escape
""              // Empty string, .len = 0
```

The type of a string literal is `u8[:0]` — a null-terminated byte slice.
The `.len` property returns the number of bytes excluding the null terminator.
The `.data` property returns a `*u8` pointer to the first byte.

String literals support escape sequences (see §1.6.6).

> Note: Lain strings are byte arrays, not character arrays. No encoding
> is assumed. Multi-byte UTF-8 characters occupy multiple `u8` positions.

### 1.6.6 Escape Sequences

The following escape sequences are recognized in character and string literals:

| Escape | Value | Description |
|:-------|:------|:------------|
| `\n` | 0x0A | Line feed (newline) |
| `\t` | 0x09 | Horizontal tab |
| `\r` | 0x0D | Carriage return |
| `\0` | 0x00 | Null byte |
| `\\` | 0x5C | Backslash |
| `\"` | 0x22 | Double quote |
| `\'` | 0x27 | Single quote |

> **CONSTRAINT:** An unrecognized escape sequence shall produce a diagnostic.

### 1.6.7 The `undefined` Literal

```lain
undefined       // Explicit uninitialized marker
```

`undefined` is a special literal that marks a variable as explicitly
uninitialized. It may only appear in variable initializers and struct
constructor arguments. See §3.5 for rules governing its use.

## 1.7 Operators & Punctuation [Implemented]

### 1.7.1 Arithmetic Operators

| Token | Symbol | Description |
|:------|:-------|:------------|
| `TOKEN_PLUS` | `+` | Addition |
| `TOKEN_MINUS` | `-` | Subtraction / unary negation |
| `TOKEN_ASTERISK` | `*` | Multiplication / pointer dereference |
| `TOKEN_SLASH` | `/` | Division |
| `TOKEN_PERCENT` | `%` | Modulo (remainder) |

### 1.7.2 Comparison Operators

| Token | Symbol | Description |
|:------|:-------|:------------|
| `TOKEN_EQUAL_EQUAL` | `==` | Equal |
| `TOKEN_BANG_EQUAL` | `!=` | Not equal |
| `TOKEN_ANGLE_BRACKET_LEFT` | `<` | Less than |
| `TOKEN_ANGLE_BRACKET_RIGHT` | `>` | Greater than |
| `TOKEN_ANGLE_BRACKET_LEFT_EQUAL` | `<=` | Less than or equal |
| `TOKEN_ANGLE_BRACKET_RIGHT_EQUAL` | `>=` | Greater than or equal |

### 1.7.3 Logical Operators

Logical operators use keyword syntax:

| Keyword | Description |
|:--------|:------------|
| `and` | Logical AND (short-circuiting) |
| `or` | Logical OR (short-circuiting) |
| `!` | Logical NOT (prefix unary) |

### 1.7.4 Bitwise Operators

| Token | Symbol | Description |
|:------|:-------|:------------|
| `TOKEN_AMPERSAND` | `&` | Bitwise AND / address-of (in unsafe) |
| `TOKEN_PIPE` | `\|` | Bitwise OR |
| `TOKEN_CARET` | `^` | Bitwise XOR |
| `TOKEN_TILDE` | `~` | Bitwise NOT (complement) |
| `TOKEN_SHIFT_LEFT` | `<<` | Left shift |
| `TOKEN_SHIFT_RIGHT` | `>>` | Right shift |

### 1.7.5 Assignment Operators

| Token | Symbol | Description |
|:------|:-------|:------------|
| `TOKEN_EQUAL` | `=` | Assignment / binding |
| `TOKEN_PLUS_EQUAL` | `+=` | Add and assign |
| `TOKEN_MINUS_EQUAL` | `-=` | Subtract and assign |
| `TOKEN_ASTERISK_EQUAL` | `*=` | Multiply and assign |
| `TOKEN_SLASH_EQUAL` | `/=` | Divide and assign |
| `TOKEN_PERCENT_EQUAL` | `%=` | Modulo and assign |
| `TOKEN_AMPERSAND_EQUAL` | `&=` | Bitwise AND and assign |
| `TOKEN_PIPE_EQUAL` | `\|=` | Bitwise OR and assign |
| `TOKEN_CARET_EQUAL` | `^=` | Bitwise XOR and assign |

Compound assignment `x op= y` is syntactic sugar for `x = x op y` (see §4.7).

### 1.7.6 Range Operators

| Token | Symbol | Description |
|:------|:-------|:------------|
| `TOKEN_DOT_DOT` | `..` | Exclusive range (half-open: `[start, end)`) |
| `TOKEN_DOT_DOT_EQUAL` | `..=` | Inclusive range `[start, end]` [Planned] |

### 1.7.7 Other Operators

| Token | Symbol | Description |
|:------|:-------|:------------|
| `TOKEN_DOT` | `.` | Member access, module path separator |
| `TOKEN_ELLIPSIS` | `...` | Variadic parameters (extern only) |

### 1.7.8 Punctuation

| Token | Symbol | Description |
|:------|:-------|:------------|
| `TOKEN_L_PAREN` | `(` | Function call, grouping |
| `TOKEN_R_PAREN` | `)` | |
| `TOKEN_L_BRACKET` | `[` | Array indexing, type syntax |
| `TOKEN_R_BRACKET` | `]` | |
| `TOKEN_L_BRACE` | `{` | Block, struct/enum body |
| `TOKEN_R_BRACE` | `}` | |
| `TOKEN_COMMA` | `,` | Separator in lists |
| `TOKEN_COLON` | `:` | Case arm separator, type annotations |
| `TOKEN_SEMICOLON` | `;` | Optional statement terminator |

## 1.8 Token Disambiguation [Implemented]

Several tokens have context-dependent meaning:

| Symbol | Context 1 | Context 2 |
|:-------|:----------|:----------|
| `*` | Multiplication (binary) | Pointer dereference (unary prefix) |
| `-` | Subtraction (binary) | Negation (unary prefix) |
| `&` | Bitwise AND (binary) | Address-of (unary prefix, unsafe only) |
| `&` | Bitwise AND | Non-consuming match prefix (`case &expr`) |
| `=` | Assignment | Binding declaration |
| `.` | Member access | Module path separator |
| `..` | Range operator | Pattern range in `case` |

The parser resolves these ambiguities through context. The lexer produces
the same token regardless of context.

## 1.9 Lexical Ordering [Implemented]

When a token can match multiple rules, the lexer applies **maximal munch**:
the longest possible match is always taken. For example, `..=` is lexed as
a single `TOKEN_DOT_DOT_EQUAL`, not as `..` followed by `=`.

Keywords take priority over identifiers: the string `func` is always
`TOKEN_KEYWORD_FUNC`, never `TOKEN_IDENTIFIER`.

## 1.10 Integer Overflow Semantics [Implemented]

Integer arithmetic in Lain follows these rules:

| Type Category | Overflow Behavior |
|:-------------|:------------------|
| Signed integers (`int`, `i8`-`i64`, `isize`) | Two's complement wrapping |
| Unsigned integers (`u8`-`u64`, `usize`) | Modular arithmetic (wrapping) |
| Floating-point (`f32`, `f64`) | IEEE 754: overflow produces +-infinity |

The C backend shall be compiled with `-fwrapv` to guarantee two's complement
wrapping for signed integer overflow. This is a normative requirement — signed
overflow in Lain is **defined behavior** (wrapping), not undefined behavior.

---

*This chapter is normative. All lexical rules stated herein apply to every
conforming Lain implementation.*
