# Appendix A — Keyword & Type Reference

## A.1 Keywords

### A.1.1 Implemented Keywords

| Keyword | Category | Description | Reference |
|:--------|:---------|:------------|:----------|
| `and` | Operator | Logical AND / constraint combinator | §4.5, §8.6 |
| `as` | Operator | Type cast | §4.8 |
| `break` | Control flow | Exit innermost loop | §5.6.3 |
| `c_include` | Interop | Include C header | §11.2 |
| `case` | Control flow | Pattern matching | §5.7 |
| `comptime` | Generic | Compile-time parameter modifier | §9.2 |
| `continue` | Control flow | Skip to next loop iteration | §5.6.4 |
| `decreasing` | Control flow | Termination measure for bounded `while` in `func` | §5.6.2.1 |
| `defer` | Control flow | Schedule cleanup at scope exit | §5.8 |
| `elif` | Control flow | Else-if branch | §5.5 |
| `else` | Control flow | Default branch | §5.5 |
| `export` | Module | Visibility modifier | §10.5.2 (reserved) |
| `expr` | Reserved | Expression context marker | Reserved |
| `extern` | Interop | C function/type declaration | §11.3, §11.4 |
| `false` | Literal | Boolean false | §2.5.5 |
| `for` | Control flow | Range-based loop | §5.6.1 |
| `func` | Declaration | Pure function | §6.1.1 |
| `fun` | Declaration | Alias for `func` | §6.2.1 |
| `if` | Control flow | Conditional branch | §5.5 |
| `import` | Module | Module import | §10.2 |
| `in` | Constraint / Operator | Array index constraint / loop variable / bounds-proving condition | §4.19, §5.6.1, §8.4 |
| `macro` | Reserved | Macro definition | Reserved |
| `mov` | Ownership | Ownership transfer | §7.2 |
| `or` | Operator | Logical OR | §4.5 |
| `post` | Contract | Postcondition (reserved) | Reserved |
| `pre` | Contract | Precondition (reserved) | Reserved |
| `proc` | Declaration | Procedure (with side effects) | §6.1.2 |
| `return` | Control flow | Return from function | §5.9 |
| `true` | Literal | Boolean true | §2.5.5 |
| `type` | Declaration | Type definition / type-as-value | §2.7, §9.3 |
| `undefined` | Literal | Uninitialized value marker | §3.5 |
| `unsafe` | Safety | Unsafe code block | §12.2 |
| `use` | Module | Name aliasing (reserved) | Reserved |
| `var` | Declaration/Ownership | Mutable binding / mutable borrow | §3.2, §7.1 |
| `while` | Control flow | Conditional loop (`proc`); bounded `while cond decreasing measure` also in `func` | §5.6.2 |

### A.1.2 Reserved Keywords

These keywords are recognized by the lexer but have no current implementation:

| Keyword | Intended Purpose |
|:--------|:-----------------|
| `end` | Block terminator (legacy) |
| `export` | Visibility control |
| `expr` | Expression-level constructs |
| `macro` | Macro system |
| `post` | Postcondition contracts |
| `pre` | Precondition contracts |
| `use` | Name aliasing |

## A.2 Primitive Types

### A.2.1 Integer Types

| Type | Size | Signed | Range | C Equivalent |
|:-----|:-----|:-------|:------|:-------------|
| `i8` | 1 byte | Yes | −128 to 127 | `int8_t` |
| `i16` | 2 bytes | Yes | −32,768 to 32,767 | `int16_t` |
| `i32` | 4 bytes | Yes | −2³¹ to 2³¹−1 | `int32_t` |
| `i64` | 8 bytes | Yes | −2⁶³ to 2⁶³−1 | `int64_t` |
| `int` | 4 bytes | Yes | −2³¹ to 2³¹−1 | `int32_t` |
| `u8` | 1 byte | No | 0 to 255 | `uint8_t` |
| `u16` | 2 bytes | No | 0 to 65,535 | `uint16_t` |
| `u32` | 4 bytes | No | 0 to 2³²−1 | `uint32_t` |
| `u64` | 8 bytes | No | 0 to 2⁶⁴−1 | `uint64_t` |
| `isize` | pointer | Yes | Platform-dependent | `intptr_t` |
| `usize` | pointer | No | Platform-dependent | `uintptr_t` |

### A.2.2 Other Primitive Types

| Type | Size | Description | C Equivalent |
|:-----|:-----|:------------|:-------------|
| `bool` | 1 byte | Boolean (true/false) | `bool` |
| `f32` | 4 bytes | 32-bit float | `float` |
| `f64` | 8 bytes | 64-bit float | `double` |
| `void` | 0 bytes | No value | `void` |

## A.3 Type Constructors

| Syntax | Kind | Description | Reference |
|:-------|:-----|:------------|:----------|
| `T` | Simple | Named type | §2.3 |
| `*T` | Pointer | Raw pointer to T | §2.6.1 |
| `T[N]` | Array | Fixed-size array of N elements | §2.5.1 |
| `T[:]` | Slice | Dynamic-size slice | §2.5.2 |
| `T[:0]` | Slice | Null-terminated slice | §2.5.3 |
| `comptime T` | Comptime | Compile-time parameter type | §9.2 |
| `var T` | Mutable | Mutable reference type | §7.1 |
| `mov T` | Owned | Ownership transfer type | §7.1 |

## A.4 Operator Precedence

Listed from highest to lowest precedence:

| Level | Operators | Associativity | Description |
|:------|:----------|:--------------|:------------|
| 13 | `.` `()` `[]` | Left | Member, call, index |
| 12 | `*` (unary) `&` `-` (unary) `!` | Right | Deref, address, negate, not |
| 11 | `as` | Left | Type cast |
| 10 | `*` `/` `%` | Left | Multiplicative |
| 9 | `+` `-` | Left | Additive |
| 8 | `<<` `>>` | Left | Bit shift |
| 7 | `&` | Left | Bitwise AND |
| 6 | `^` | Left | Bitwise XOR |
| 5 | `\|` | Left | Bitwise OR |
| 4 | `==` `!=` `<` `>` `<=` `>=` `in` | Left | Comparison / bounds |
| 3 | `and` | Left | Logical AND |
| 2 | `or` | Left | Logical OR |
| 1 | `=` `+=` `-=` `*=` `/=` `%=` | Right | Assignment |

---

*This appendix is normative.*
