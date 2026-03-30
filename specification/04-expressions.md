# Chapter 4 — Expressions

## 4.1 Overview

An expression is a syntactic construct that evaluates to a value. Every
expression has a type, determined statically at compile time. Expressions
are evaluated strictly left-to-right (see §4.12).

## 4.2 Literal Expressions [Implemented]

Literal expressions produce constant values:

```lain
42              // int
3.14            // f64
true            // bool
'a'             // u8
"hello"         // u8[:0]
undefined       // special marker (see §3.5)
```

See §1.6 for the lexical rules governing literals.

## 4.3 Identifier Expressions [Implemented]

An identifier expression evaluates to the value of the named variable:

```lain
x               // value of x
my_variable     // value of my_variable
```

> **CONSTRAINT:** The identifier shall be in scope (see §3.7). Using an
> undeclared identifier shall produce a diagnostic.

> **CONSTRAINT:** The identifier shall have been initialized (see §3.5.1).
> Reading an uninitialized variable shall produce a diagnostic.

## 4.4 Arithmetic Expressions [Implemented]

| Operator | Syntax | Description | Operand Types |
|:---------|:-------|:------------|:--------------|
| Addition | `a + b` | Sum | integer, float |
| Subtraction | `a - b` | Difference | integer, float |
| Multiplication | `a * b` | Product | integer, float |
| Division | `a / b` | Quotient | integer (truncated), float |
| Modulo | `a % b` | Remainder | integer only |

> **CONSTRAINT:** Both operands of a binary arithmetic operator shall have
> the same type. Mixed-type arithmetic (e.g., `int + u8`) is a compile error.

> **CONSTRAINT:** Integer division by a literal zero shall be rejected by
> the compiler (diagnostic `[E015]`). Runtime division by zero is undefined
> behavior.

### 4.4.1 Unary Negation

```lain
-x              // arithmetic negation (int or float)
```

## 4.5 Comparison Expressions [Implemented]

| Operator | Syntax | Description |
|:---------|:-------|:------------|
| Equal | `a == b` | True if a equals b |
| Not equal | `a != b` | True if a does not equal b |
| Less than | `a < b` | True if a is less than b |
| Greater than | `a > b` | True if a is greater than b |
| Less or equal | `a <= b` | True if a is less than or equal to b |
| Greater or equal | `a >= b` | True if a is greater than or equal to b |

Comparison operators produce values of type `bool`.

> **CONSTRAINT:** Both operands shall have the same type. Comparing different
> types is a compile error.

> **CONSTRAINT:** `==` and `!=` on structs, arrays, or ADTs are compile errors
> (see §2.13.1).

## 4.6 Logical Expressions [Implemented]

| Operator | Syntax | Description |
|:---------|:-------|:------------|
| Logical AND | `a and b` | Short-circuiting conjunction |
| Logical OR | `a or b` | Short-circuiting disjunction |
| Logical NOT | `!a` | Negation |

`and` and `or` are short-circuiting: `b` is not evaluated if `a` determines
the result.

> **CONSTRAINT:** Operands of `and`, `or`, and `!` shall have type `bool`.

## 4.7 Bitwise Expressions [Implemented]

| Operator | Syntax | Description |
|:---------|:-------|:------------|
| Bitwise AND | `a & b` | |
| Bitwise OR | `a \| b` | |
| Bitwise XOR | `a ^ b` | |
| Bitwise NOT | `~a` | One's complement |
| Left shift | `a << b` | |
| Right shift | `a >> b` | Arithmetic for signed, logical for unsigned |

> **CONSTRAINT:** Operands of bitwise operators shall be integer types.

### 4.7.1 Compound Assignment

All compound assignment operators are syntactic sugar:

```lain
x += 5          // equivalent to: x = x + 5
x -= 3          // equivalent to: x = x - 3
x *= 2          // equivalent to: x = x * 2
x /= 4          // equivalent to: x = x / 4
x %= 3          // equivalent to: x = x % 3
x &= mask       // equivalent to: x = x & mask
x |= flag       // equivalent to: x = x | flag
x ^= bits       // equivalent to: x = x ^ bits
```

> **CONSTRAINT:** The left-hand side of a compound assignment shall be a
> mutable variable (see §3.2.1).

## 4.8 Type Cast (`as`) [Implemented]

The `as` operator performs explicit type conversion:

```lain
var x i32 = 1000
var y = x as u8           // truncation: 1000 -> 232
var big = 42 as i64       // widening
```

### 4.8.1 Permitted Casts

| From | To | Semantics |
|:-----|:---|:----------|
| Integer | Integer | Truncation or zero/sign-extension |
| Integer | Float | Conversion to nearest representable value |
| Float | Integer | Truncation toward zero |
| Float | Float | Precision change |
| Pointer | Pointer | Reinterpretation (requires `unsafe`) |
| Integer | Pointer | Reinterpretation (requires `unsafe`) |
| Pointer | Integer | Reinterpretation (requires `unsafe`) |

> **CONSTRAINT:** Non-numeric casts (e.g., struct to int) are compile errors.

> **CONSTRAINT:** Pointer casts require `unsafe` blocks (see §12).

## 4.9 Member Access [Implemented]

Dot notation accesses struct fields, enum variants, and nested type members:

```lain
p.x                       // struct field access
Color.Red                 // enum variant
Token.Kind.Number         // nested type variant
s.len                     // slice property
```

> **CONSTRAINT:** The member name shall exist on the type. Accessing a
> nonexistent field shall produce diagnostic `[E012]`.

## 4.10 Index Expression [Implemented]

Square bracket notation accesses array and slice elements:

```lain
arr[i]                    // read element at index i
arr[i] = 42               // write element (if arr is mutable)
```

> **CONSTRAINT:** The index shall be statically proven to be within bounds
> via Value Range Analysis (see §8) or an in-guard (see §4.21). Accesses
> that cannot be statically proven safe are rejected.

## 4.11 Function Call [Implemented]

A function call evaluates the callee and arguments, then transfers control:

```lain
add(1, 2)                 // direct call
obj.method(arg)           // UFCS call (see §6.6)
```

### 4.11.1 Argument Passing

Arguments are passed with explicit ownership annotations at the call site:

```lain
read_data(data)           // shared borrow (default)
modify(var data)          // mutable borrow (explicit var)
consume(mov data)         // ownership transfer (explicit mov)
```

> **CONSTRAINT:** The mode annotation at the call site shall match the
> parameter mode of the callee. Passing without `var` to a `var` parameter,
> or without `mov` to a `mov` parameter, is a compile error.

### 4.11.2 Argument Count

> **CONSTRAINT:** The number of arguments shall match the number of parameters,
> unless the function is variadic (`extern` with `...`).

## 4.12 Move Expression (`mov`) [Implemented]

The `mov` operator transfers ownership of a value:

```lain
mov x                     // transfer ownership of x
```

After a move, the source variable is **invalidated** — any subsequent use
is a compile error (see §7.3).

```lain
consume(mov resource)
// resource is now invalid — any use is [E001]
```

## 4.13 Mutable Borrow Expression (`var`) [Implemented]

The `var` operator at expression level signals a mutable borrow:

```lain
modify(var data)          // mutable borrow of data
```

The borrowed variable remains valid but cannot be used in conflicting ways
while the mutable borrow is active (see §7.4).

## 4.14 Range Expression [Implemented]

Range expressions define a half-open interval:

```lain
0..10                     // range [0, 10) — exclusive end
```

Range expressions are used in:
- `for` loops: `for i in 0..n { ... }`
- Pattern matching: `case x { 1..10: ... }`

In pattern matching contexts, ranges are **inclusive** on both ends:
`1..10` matches values 1 through 10.

> Note: The `..=` operator for explicit inclusive ranges is lexed but
> not yet fully specified. [Planned]

## 4.15 Case Expression (Expression-Form Match) [Implemented]

A `case` block can be used as an expression:

```lain
var size = case width {
    1..10: "Small"
    11..50: "Medium"
    else: "Large"
}
```

All branches shall yield the same type. See §5.7 for full pattern matching rules.

## 4.16 Address-Of (`&`) [Implemented]

The unary `&` operator creates a raw pointer to a variable:

```lain
unsafe {
    var p = &x            // p is of type *int
}
```

> **CONSTRAINT:** The `&` operator shall only be used inside `unsafe`
> blocks (see §12).

## 4.17 Dereference (`*`) [Implemented]

The unary `*` operator dereferences a pointer:

```lain
unsafe {
    var val = *ptr        // read value pointed to by ptr
    *ptr = 42             // write through pointer
}
```

> **CONSTRAINT:** Pointer dereference shall only be used inside `unsafe`
> blocks (see §12).

## 4.18 Operator Precedence [Implemented]

Operators are listed from highest to lowest precedence:

| Precedence | Operators | Associativity |
|:-----------|:----------|:-------------|
| 1 (highest) | `()` `[]` `.` | Left |
| 2 | `!` `~` `-` `*` (unary) | Right |
| 3 | `as` | Left |
| 4 | `*` `/` `%` | Left |
| 5 | `+` `-` | Left |
| 6 | `<<` `>>` | Left |
| 7 | `&` | Left |
| 8 | `^` | Left |
| 9 | `\|` | Left |
| 10 | `<` `>` `<=` `>=` `in` | Left |
| 11 | `==` `!=` | Left |
| 12 | `and` | Left |
| 13 (lowest) | `or` | Left |

Parentheses `()` may be used to override precedence.

## 4.19 Bounds-Proving Expression (`in`) [Implemented]

The `in` keyword can be used as a binary operator between an index expression
and a container (array or slice):

```lain
idx in arr                // true iff 0 <= idx < arr.len
l.pos in l.src            // works with member expressions
```

The result type is `bool`. The expression compiles to `(idx >= 0 && idx < arr.len)`.

### 4.19.1 In-Guard Semantics

When `idx in arr` appears as the condition (or part of an `and`-chain condition)
of an `if` or `while` statement, it creates an **in-guard**: the compiler
permits `arr[idx]` inside the guarded body without further bounds verification.

```lain
if pos in data {
    return data[pos] as int   // safe: in-guarded
}

while i in arr and arr[i] != 0 decreasing arr.len - i {
    i += 1                    // safe: in-guarded
}
```

> **CONSTRAINT:** The in-guard matches structurally: `idx in arr` guards
> exactly `arr[idx]`. Accesses with arithmetic offsets (e.g., `arr[idx + 1]`)
> are **not** guarded and require separate verification or `unsafe`.

### 4.19.2 And-Chain Propagation

In an `and` expression, in-guards from the left operand are active when
evaluating the right operand. This enables idiomatic bounds-then-access
patterns:

```lain
while l.pos in l.src and (l.src[l.pos] as int) != '"' decreasing l.src.len - l.pos {
    // l.src[l.pos] is safe in both the condition RHS and the body
}
```

## 4.20 Evaluation Order [Implemented]


Expressions, function arguments, and binary operands are evaluated strictly
**left-to-right**:

```lain
// a() is guaranteed to execute before b()
var result = foo(a(), b())
```

This guarantee applies to:
- Binary operator operands: left before right
- Function arguments: first argument before second, etc.
- Compound expressions: sub-expressions in textual order

## 4.21 Lvalue vs Rvalue

An **lvalue** is an expression that refers to a storage location. An **rvalue**
is an expression that produces a value.

Lvalues include:
- Identifiers (`x`)
- Member access (`p.x`)
- Array index (`arr[i]`)
- Pointer dereference (`*ptr`, in unsafe)

Rvalues include all other expressions (literals, function calls, arithmetic, etc.).

The left-hand side of an assignment shall be an lvalue. The left-hand side of
a compound assignment shall be a mutable lvalue.

---

*This chapter is normative.*
