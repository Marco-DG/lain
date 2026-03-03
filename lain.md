# The Lain Programming Language — Complete Specification

**Version**: 1.0.0 (Draft)  
**Date**: 2026-03-03  
**Status**: Normative Specification — Target Design Document

> Lain is a **critical-safe** programming language designed for embedded systems. It achieves memory safety, type safety, and determinism with **zero runtime overhead** — no garbage collector, no reference counting, no runtime checks. This document specifies the complete, target-state language that Lain shall become.

> Sections marked 🟢 are implemented. Sections marked 🔴 are target design. Sections marked 🟡 are long-term goals.

---

## Table of Contents

1. [Philosophy & Design Goals](#1-philosophy--design-goals)
2. [Lexical Structure](#2-lexical-structure)
3. [Primitive Types](#3-primitive-types)
4. [Composite Types](#4-composite-types)
5. [Variables & Mutability](#5-variables--mutability)
6. [Ownership & Borrowing](#6-ownership--borrowing)
7. [Functions & Procedures](#7-functions--procedures)
8. [Control Flow](#8-control-flow)
9. [Expressions & Operators](#9-expressions--operators)
10. [Type Constraints & Static Verification](#10-type-constraints--static-verification)
11. [Module System](#11-module-system)
12. [C Interoperability](#12-c-interoperability)
13. [Unsafe Code](#13-unsafe-code)
14. [Memory Model](#14-memory-model)
15. [Error Handling](#15-error-handling)
16. [Initialization & Zero Values](#16-initialization--zero-values)
17. [Arithmetic & Overflow](#17-arithmetic--overflow)
18. [Generics & Compile-Time Evaluation](#18-generics--compile-time-evaluation)
19. [String & Text Handling](#19-string--text-handling)
20. [Compilation Pipeline](#20-compilation-pipeline)
21. [Diagnostics & Error Reporting](#21-diagnostics--error-reporting)
22. [Standard Library](#22-standard-library)
23. [Design Decisions & Rationale](#23-design-decisions--rationale)
24. [Phased Roadmap](#24-phased-roadmap)

**Appendices:**
- [A: Complete Keyword List](#appendix-a-complete-keyword-list)
- [B: Grammar (Pseudo-BNF)](#appendix-b-grammar-pseudo-bnf)
- [C: Differences from Rust, C, Zig](#appendix-c-differences-from-rust-c-zig)

---

## 1. Philosophy & Design Goals

### 1.1 The Five Pillars

Lain's design is governed by five non-negotiable goals, ordered by priority:

| # | Pillar | Sigla | Description |
|---|--------|-------|-------------|
| 1 | **Assembly-Speed Performance** | PERF | Zero runtime overhead. No GC, no ref-counting, no hidden allocations. The generated code must be as fast as hand-written C. |
| 2 | **Zero-Cost Memory Safety** | SAFE | Guaranteed absence of use-after-free, double-free, data races, buffer overflows, and memory leaks — all enforced at compile time with no runtime cost. |
| 3 | **Static Verification** | STAT | Compile-time proof of program properties: bounds safety via Value Range Analysis, constraint satisfaction, exhaustive pattern matching. No SMT solver — all analyses are polynomial-time. |
| 4 | **Determinism** | DET | Pure functions (`func`) are guaranteed to terminate and produce the same output for the same input. Side effects are confined to `proc`. |
| 5 | **Syntactic Simplicity** | SIMP | Clean, readable syntax with no hidden magic. Explicit annotations at call sites (`var`, `mov`). No implicit conversions, no hidden copies. |

### 1.2 Guiding Principles

- **Soundness over completeness**: The compiler may reject valid programs (false positives) but must **never** accept invalid programs (false negatives).
- **Explicitness at boundaries**: Ownership transfer (`mov`) and mutable access (`var`) are explicit at every call site. The reader of a function call always knows what happens to each argument.
- **No hidden costs**: Every operation's cost is visible in the source code. No implicit copies, no hidden heap allocations, no virtual dispatch.
- **Composition over inheritance**: Lain has no classes, no inheritance, no vtables. Behavior is composed through functions, UFCS, and algebraic data types.
- **C as the escape hatch**: When Lain's safety is too restrictive, `unsafe` blocks and C interop provide a controlled exit. The safety boundary is always explicit.

### 1.3 Target Domains

Lain is designed for:
- **Embedded systems** — bare-metal firmware, RTOS tasks, device drivers
- **Systems programming** — OS components, compilers, interpreters
- **Safety-critical software** — aerospace, medical, automotive (where formal guarantees matter)
- **Performance-critical applications** — game engines, real-time audio, network stacks

### 1.4 Non-Goals

Lain explicitly does **not** aim to:
- Support garbage collection (ever).
- Support runtime reflection or dynamic dispatch.
- Be a general-purpose scripting language.
- Support exceptions or stack unwinding.
- Provide implicit memory management of any kind.

---

## 2. Lexical Structure

### 2.1 Source Encoding 🟢

Source files are UTF-8 encoded. The compiler processes source as a byte stream; multi-byte characters are valid in string literals and comments but not in identifiers.

### 2.2 Comments 🟢

```lain
// Single-line comment (to end of line)
/* Multi-line comment (nesting NOT supported) */
```

### 2.3 Identifiers 🟢

Identifiers begin with a letter or underscore, followed by letters, digits, or underscores:

```
identifier = [a-zA-Z_][a-zA-Z0-9_]*
```

Identifiers are case-sensitive. `foo`, `Foo`, and `FOO` are distinct.

### 2.4 Literals 🟢

| Kind | Examples | Notes |
|------|----------|-------|
| Integer (decimal) | `42`, `0`, `-1` | Default type: `int` |
| Integer (hex) | `0xFF`, `0x1A3B` | Prefix `0x` |
| Boolean | `true`, `false` | Type: `bool` |
| Character | `'a'`, `'\\n'`, `'\\0'` | Type: `u8` |
| String | `"hello"`, `"line\\n"` | Type: `u8[:0]` (null-terminated slice) |
| Float | `3.14`, `1.0`, `-0.5` | Type: `f64` (or `f32` with annotation) |
| Undefined | `undefined` | Explicit uninitialized marker |

**Escape sequences** in strings and characters:

| Escape | Meaning |
|--------|---------|
| `\\n` | Newline |
| `\\t` | Tab |
| `\\r` | Carriage return |
| `\\0` | Null byte |
| `\\\\` | Backslash |
| `\\"` | Double quote |
| `\\'` | Single quote |

### 2.5 Operators & Punctuation 🟢

```
+  -  *  /  %                    // Arithmetic
== != < > <= >=                  // Comparison
and  or  !                       // Logical
&  |  ^  ~                       // Bitwise
=  +=  -=  *=  /=  %=           // Assignment
&=  |=  ^=                       // Bitwise assignment
.  ..  ->                        // Member, range, arrow
( ) [ ] { }                     // Grouping
,  :  ;                          // Separators (; is optional in Lain)
```

---

## 3. Primitive Types

### 3.1 Integer Types 🟢

| Type | Size | Range | C Equivalent |
|------|------|-------|--------------|
| `int` | Platform-dependent (≥ 32-bit) | Signed | `int` |
| `i8` | 8-bit | −128 to 127 | `int8_t` |
| `i16` | 16-bit | −32,768 to 32,767 | `int16_t` |
| `i32` | 32-bit | −2³¹ to 2³¹−1 | `int32_t` |
| `i64` | 64-bit | −2⁶³ to 2⁶³−1 | `int64_t` |
| `u8` | 8-bit | 0 to 255 | `uint8_t` |
| `u16` | 16-bit | 0 to 65,535 | `uint16_t` |
| `u32` | 32-bit | 0 to 2³²−1 | `uint32_t` |
| `u64` | 64-bit | 0 to 2⁶⁴−1 | `uint64_t` |
| `isize` | Pointer-sized signed | Platform-dependent | `ptrdiff_t` |
| `usize` | Pointer-sized unsigned | Platform-dependent | `size_t` |

### 3.2 Floating-Point Types 🟢

| Type | Size | Precision | C Equivalent |
|------|------|-----------|--------------|
| `f32` | 32-bit | ~7 decimal digits | `float` |
| `f64` | 64-bit | ~16 decimal digits | `double` |

### 3.3 Boolean Type 🟢

`bool` — values are `true` and `false`. Stored as a single byte. No implicit conversion to/from integers.

### 3.4 The `void` Type 🟢

Used only in pointer types (`*void`) for C interop. Cannot be used as a variable type.

### 3.5 Implicit Conversions — Forbidden 🟢

Lain **strictly forbids** all implicit type conversions. There is no implicit widening (`u8` → `int`), no implicit truncation, no implicit bool-to-int. Every conversion must use the explicit `as` operator:

```lain
var x i32 = 1000
var y = x as u8           // Explicit truncation
var big = 42 as i64       // Explicit widening
```

---

## 4. Composite Types

### 4.1 Structs 🟢

Structs group named fields into a single value type:

```lain
type Point {
    x int,
    y int
}

type Color {
    r u8, g u8, b u8, a u8
}
```

**Construction**: Positional, with all fields required:
```lain
var p = Point(10, 20)
var c = Color(255, 128, 0, 255)
```

**Field access**: Dot notation:
```lain
var px = p.x              // read
p.x = 42                  // write (only if p is mutable)
```

**Value semantics**: Structs are copied on assignment (unless they contain `mov` fields, which makes them linear):
```lain
var p2 = p                // copies all fields
p2.x = 99                 // does not affect p
```

### 4.2 Linear Struct Fields 🟢

Fields annotated with `mov` make the struct **linear** — it must be consumed exactly once:

```lain
type File {
    mov handle *FILE       // owned resource
}
// File is linear: must be explicitly consumed via close_file(mov f)
```

### 4.3 Destructured Parameters 🟢

Functions can destructure struct arguments:

```lain
func distance(mov {x, y} Point) int {
    return x * x + y * y   // x and y are extracted, Point is consumed
}
```

The `mov {field1, field2, ...} ParamName` syntax extracts the named fields, consuming the struct.

### 4.4 Enums (Simple) 🟢

Simple enumerations without associated data:

```lain
type Color { Red, Green, Blue }
type Direction { North, South, East, West }
```

Values are accessed by name: `Color.Red`, `Direction.North`.

### 4.5 Algebraic Data Types (ADTs) 🟢

ADTs extend enums with associated data per variant:

```lain
type Shape {
    Circle     { radius int }
    Rectangle  { width int, height int }
    Point
}
```

**Construction**:
```lain
var s = Shape.Circle(10)
var r = Shape.Rectangle(5, 3)
var p = Shape.Point
```

**Destructuring** via `case` (pattern matching):
```lain
case s {
    Circle(r):       libc_printf("radius: %d\n", r)
    Rectangle(w, h): libc_printf("%d x %d\n", w, h)
    Point:           libc_printf("point\n")
}
```

### 4.6 Arrays (Fixed-Size) 🟢

Arrays have a compile-time-known size:

```lain
var arr int[5]              // 5 ints, uninitialized (requires = undefined or values)
var arr2 = int[3]{1, 2, 3}  // initialized
```

**Properties**:
- `.len` — compile-time constant, number of elements
- Stack-allocated, contiguous memory
- Bounds-checked statically via VRA (§10)

**Indexing**: `arr[i]` — the compiler statically verifies `0 ≤ i < arr.len`.

### 4.7 Slices 🟢

Slices are dynamically-sized views into arrays:

| Syntax | Description | Type |
|--------|-------------|------|
| `T[]` | Slice of T | Fat pointer: `{data: *T, len: usize}` |
| `T[:0]` | Null-terminated slice | `{data: *T, len: usize}` with sentinel `0` |

```lain
var s = "hello"           // Type: u8[:0] (null-terminated byte slice)
var len = s.len           // 5
var ptr = s.data          // *u8
```

**String literals** are `u8[:0]` — null-terminated byte slices.

### 4.8 Pointers 🟢

Pointers represent memory addresses and come in three modes:

| Syntax | Mode | C Equivalent | Borrow Checker |
|--------|------|-------------|----------------|
| `*T` | Shared (read-only) | `const T*` | Not tracked (raw) |
| `var *T` | Mutable | `T*` | Not tracked (raw) |
| `mov *T` | Owned | `T*` | Tracked as linear |

Dereferencing (`*ptr`) requires an `unsafe` block. Creating a pointer (`&x`) also requires `unsafe`.

### 4.9 Extern Types (Opaque) 🟢

Types defined in C that Lain cannot inspect:

```lain
extern type FILE
extern type sqlite3
```

These can only be used behind pointers (`*FILE`, `mov *FILE`). Their size and layout are unknown to Lain.

---

## 5. Variables & Mutability

### 5.1 Immutable Bindings (Default) 🟢

By default, all bindings are **immutable**:

```lain
var x = 42        // WRONG NAME: 'var' keyword, but x is IMMUTABLE by default
x = 99            // ❌ ERROR: x is immutable
```

> **Design note**: The `var` keyword introduces a binding. Without a `var` annotation on the type/mode, the binding is immutable. This is potentially confusing — see §23 for rationale.

### 5.2 Mutable Bindings 🟢

Adding `var` before the type (or as a standalone keyword in certain contexts) makes the binding mutable:

```lain
var x int = 42    // x is mutable (explicit type with var)
var x = 42        // x is immutable (no var annotation on type)
```

**Mutability in practice**: Mutability is determined by the declaration context:
- `var name = expr` — immutable (value binding)
- `var name Type = expr` — depends on context
- Parameters with `var` mode: `func f(var x int)` — mutable reference

### 5.3 Type Inference 🟢

When the type is omitted, it is inferred from the initializer:

```lain
var x = 42              // inferred: int
var s = "hello"         // inferred: u8[:0]
var b = true            // inferred: bool
var p = Point(1, 2)     // inferred: Point
```

### 5.4 Explicit Type Annotation 🟢

Types can be explicitly specified:

```lain
var x int = 42
var y u8 = 255
var arr int[10]
```

### 5.5 The `undefined` Keyword 🟢

To leave a variable explicitly uninitialized:

```lain
var x int = undefined      // x contains garbage
var p = Point(10, undefined) // p.y contains garbage
```

The compiler enforces **Definite Initialization Analysis**: reading an `undefined` variable before assigning it is a compile error. Immutable bindings cannot use `undefined`.

### 5.6 Shadowing 🟢

Variables can be redeclared in the same scope (shadowing):

```lain
var x = 42
var x = "hello"           // shadows the previous x
// The old x is no longer accessible
```

### 5.7 Block Scoping 🟢

Variables are scoped to the block they are declared in:

```lain
proc example() {
    var x = 10
    if true {
        var y = 20        // y is only visible here
        var x = 30        // shadows outer x within this block
}
```

---

## 6. Ownership & Borrowing

### 6.1 Overview

Lain's ownership system is the foundation of its memory safety. Every value in a Lain program has exactly one **owner** — the variable that holds it. When the owner goes out of scope, the value is destroyed.

The three ownership modes determine how values are passed to functions:

| Mode | Syntax (call site) | Syntax (parameter) | Semantics | C Emission |
|------|--------------------|--------------------|-----------|------------|
| **Shared** | `f(x)` | `f(x T)` | Read-only borrow | `const T*` |
| **Mutable** | `f(var x)` | `f(var x T)` | Read-write borrow | `T*` |
| **Owned** | `f(mov x)` | `f(mov x T)` | Ownership transfer | `T` (by value) |

### 6.2 Shared Borrowing (Default) 🟢

The default mode. The callee receives a read-only reference:

```lain
func length(s u8[:0]) int {
    return s.len              // can read, cannot modify
}

proc main() {
    var msg = "hello"
    var n = length(msg)       // shared borrow — msg still alive
    var m = length(msg)       // multiple shared borrows OK
}
```

### 6.3 Mutable Borrowing 🟢

Exclusive read-write access. While a mutable borrow is active, no other borrow (shared or mutable) of the same variable is allowed:

```lain
func increment(var d Data) {
    d.value = d.value + 1
}

proc main() {
    var data = Data(0)
    increment(var data)       // exclusive mutable borrow
    // data.value is now 1
}
```

### 6.4 Ownership Transfer (`mov`) 🟢

The caller gives up ownership. The source variable is invalidated:

```lain
proc close_file(mov {handle} File) {
    fclose(handle)
}

proc main() {
    var f = open_file("data.txt", "r")
    close_file(mov f)         // f is consumed — dead after this
    // f cannot be used here
}
```

### 6.5 The Borrow Checker — Core Invariant 🟢

At every program point, for every variable `V`:

> **Either** zero or more shared borrows of `V` are active, **or** exactly one mutable borrow of `V` is active — **never both**.

This is the Read-Write Lock invariant. Violations are compile errors.

### 6.6 Non-Lexical Lifetimes (NLL) 🟢

Borrows expire at their **last use**, not at the end of the enclosing scope. This enables patterns like:

```lain
var data = Data(42)
var ref = get_ref(var data)   // mutable borrow of data
consume_ref(var ref)          // last use of ref → borrow released
var x = read_data(data)       // ✅ OK: borrow expired
```

> For the complete borrow checker specification, see the companion document **`borrow_checker.md`** which provides 21 chapters of detailed rules, algorithms, formal typing rules, and phased implementation roadmap.

### 6.7 Linear Types 🟢

Types containing `mov` fields are **linear**: they must be consumed exactly once. Forgetting to consume a linear value, or consuming it twice, is a compile error:

```lain
proc example() {
    var f = open_file("data.txt", "r")
    // ❌ ERROR: linear variable 'f' was not consumed before scope exit
}

proc example2() {
    var f = open_file("data.txt", "r")
    close_file(mov f)
    close_file(mov f)         // ❌ ERROR: use after move
}
```

### 6.8 Branch Consistency 🟢

Linear variables must be consumed consistently across all branches:

```lain
var resource = create_resource()
if condition {
    consume(mov resource)     // consumed in then
} else {
    consume(mov resource)     // must also be consumed in else
}
```

---

## 7. Functions & Procedures

### 7.1 The `func` / `proc` Distinction 🟢

Lain enforces a strict separation between **pure functions** and **side-effecting procedures**:

| | `func` | `proc` |
|---|--------|--------|
| Side effects | ❌ Forbidden | ✅ Allowed |
| `while` loops | ❌ Forbidden | ✅ Allowed |
| Recursion | ❌ Forbidden | ✅ Allowed |
| Calling `proc` | ❌ Forbidden | ✅ Allowed |
| Calling `func` | ✅ Allowed | ✅ Allowed |
| Calling `extern proc` | ❌ Forbidden | ✅ Allowed |
| Calling `extern func` | ✅ Allowed | ✅ Allowed |
| Global state | ❌ No access | ✅ Full access |
| Termination | Guaranteed | Not guaranteed |

**`func`** guarantees mathematical purity: same inputs → same output, always terminates.
**`proc`** is the general-purpose callable unit with no restrictions.

### 7.2 Function Declaration 🟢

```lain
func add(a int, b int) int {
    return a + b
}

proc print_hello() {
    libc_printf("Hello, world!\n")
}
```

### 7.3 Parameter Modes 🟢

Parameters specify their ownership mode:

```lain
func read_only(x Data) int { ... }         // shared borrow
func mutate(var x Data) { ... }            // mutable borrow
func consume(mov x Data) { ... }           // ownership transfer
func destruct(mov {a, b} Pair) int { ... } // destructured ownership
```

### 7.4 Return Types 🟢

```lain
func get_value() int { return 42 }          // return by value
func get_ref(var d Data) var int {          // return mutable reference
    return var d.value
}
func make_resource() mov File {             // return owned value
    return File(fopen("f.txt", "r"))
}
```

Returning `var T` creates a **persistent borrow** — the caller's variable is considered borrowed until the returned reference's last use (see `borrow_checker.md` §9).

### 7.5 Multiple Return Values 🔴

Currently, functions return a single value. Multiple return values are a target feature:

```lain
// 🔴 Target syntax
func divmod(a int, b int != 0) (int, int) {
    return a / b, a % b
}

var q, r = divmod(10, 3)
```

### 7.6 Universal Function Call Syntax (UFCS) 🟢

Any function call `f(x, args...)` can be written as `x.f(args...)`:

```lain
func is_even(n int) bool {
    return n % 2 == 0
}

proc main() {
    var x = 10
    var even = x.is_even()    // Equivalent to: is_even(x)
}
```

This works for all types and enables fluent, chainable APIs without OOP.

### 7.7 Forward Declarations 🟢

Lain uses a multi-pass compiler. Functions can be referenced before they are declared:

```lain
func main() int { return helper() }
func helper() int { return 42 }     // OK: defined after use
```

No forward declarations or header files are needed.

### 7.8 Termination Guarantees 🟢

`func` is guaranteed to terminate because:
1. `while` loops are forbidden → no unbounded iteration.
2. Recursion is forbidden → no stack overflow.
3. Only `for` loops (finite ranges) are allowed.

`proc` has no termination guarantee — it can loop forever, recurse infinitely, etc.

---

## 8. Control Flow

### 8.1 If / Elif / Else 🟢

```lain
if x > 10 {
    // ...
} elif x > 5 {
    // ...
} else {
    // ...
}
```

Conditions do not require parentheses. Bodies must use `{ }`. The condition contributes to **range analysis** — inside `if x < y`, the compiler knows `x < y`.

### 8.2 For Loops (Range-Based) 🟢

```lain
// Single variable
for i in 0..n {
    // i ranges from 0 to n-1 (exclusive end)
}

// Two variable form
for i, val in 0..10 {
    // i = index, val = value
}
```

Range `0..n` is half-open: `[0, n)`. The loop variable `i` is statically known to be in `[0, n-1]`, enabling safe array indexing without runtime checks.

**Only allowed in `func` and `proc`.**

### 8.3 While Loops 🟢

```lain
proc count_up() {
    var i = 0
    while i < 10 {
        libc_printf("%d ", i)
        i = i + 1
    }
}

// Infinite loop pattern
while 1 {
    if done { break }
}
```

**Only allowed in `proc`** (forbidden in `func` for termination guarantees).

### 8.4 Break & Continue 🟢

`break` exits the innermost loop. `continue` skips to the next iteration:

```lain
proc example() {
    var i = 0
    while i < 10 {
        i = i + 1
        if i == 5 { continue }
        if i == 8 { break }
        libc_printf("%d ", i)
    }
    // Output: 1 2 3 4 6 7
}
```

### 8.5 Case (Pattern Matching) 🟢

`case` supports matching on enums, ADTs, characters, integers, and ranges:

```lain
// Enum matching
case color {
    Red:   libc_printf("Red\n")
    Green: libc_printf("Green\n")
    Blue:  libc_printf("Blue\n")
}

// ADT destructuring
case shape {
    Circle(r):        libc_printf("Circle r=%d\n", r)
    Rectangle(w, h):  libc_printf("Rect %dx%d\n", w, h)
    Point:            libc_printf("Point\n")
}

// Multiple patterns and ranges
case character {
    'a'..'z', 'A'..'Z': libc_printf("Alpha\n")
    '0'..'9':           libc_printf("Digit\n")
    '_', '-':           libc_printf("Symbol\n")
    else:               libc_printf("Other\n")
}
```

### 8.6 Case Expressions 🟢

`case` can be used as an expression:

```lain
var size = case width {
    1..10: "Small"
    11..50: "Medium"
    else: "Large"
}
```

All branches must yield the same type.

### 8.7 Exhaustiveness Checking 🟢

The compiler verifies that `case` statements are exhaustive:

| Scrutinee | Requirement |
|-----------|-------------|
| Enum | All variants covered, OR `else` present |
| ADT | All variants covered, OR `else` present |
| Integer | `else` always required (infinite domain) |
| Character | `else` always required |

### 8.8 Defer Statement 🟢

`defer` schedules a block to execute at scope exit (RAII pattern):

```lain
proc process_file() {
    var f = open_file("data.txt", "r")
    defer {
        close_file(mov f)
    }
    // ... use f ...
    // f is automatically closed when scope exits
}
```

**Rules**:
1. Deferred blocks execute in LIFO order (last `defer` first).
2. They execute on all exit paths: end of block, `return`, `break`, `continue`.
3. `defer` blocks cannot contain `return`, `break`, or `continue` that escape the block.

### 8.9 Evaluation Order 🟢

Expressions, function arguments, and operands are evaluated strictly **Left-to-Right**:

```lain
// a() is guaranteed to execute before b()
var result = foo(a(), b())
```

---

## 9. Expressions & Operators

### 9.1 Arithmetic 🟢

```lain
a + b      // Addition
a - b      // Subtraction
a * b      // Multiplication
a / b      // Integer division
a % b      // Modulo (remainder)
```

### 9.2 Comparison 🟢

```lain
a == b     // Equal
a != b     // Not equal
a < b      // Less than
a > b      // Greater than
a <= b     // Less than or equal
a >= b     // Greater than or equal
```

### 9.3 Logical Operators 🟢

Lain uses keyword-based logical operators:

```lain
x > 0 and x < 100    // Logical AND
x == 0 or x == 1     // Logical OR
!condition            // Logical NOT
```

### 9.4 Bitwise Operators 🟢

```lain
a & b      // Bitwise AND
a | b      // Bitwise OR
a ^ b      // Bitwise XOR
~a         // Bitwise NOT (complement)
```

### 9.5 Compound Assignment 🟢

All compound operators are syntactic sugar:

```lain
x += 5     // x = x + 5
x -= 3     // x = x - 3
x *= 2     // x = x * 2
x /= 4     // x = x / 4
x %= 3     // x = x % 3
x &= mask  // x = x & mask
x |= flag  // x = x | flag
x ^= bits  // x = x ^ bits
```

### 9.6 Type Cast (`as`) 🟢

Explicit type conversion:

```lain
var x i32 = 1000
var y = x as u8        // Truncate: 1000 → 232 (wrapping)
var big = 42 as i64    // Widen
```

**Rules**:
- Conversions between integer types are always allowed (truncation may occur).
- Pointer casts (`*int as *void`) require `unsafe`.
- Non-numeric casts (struct to int) are forbidden.

### 9.7 Operator Precedence 🟢

| Precedence | Operators | Associativity |
|:-----------|:----------|:-------------|
| 1 (highest) | `()` `[]` `.` | Left |
| 2 | `!` `~` `-` `*` | Right (unary) |
| 3 | `as` | Left |
| 4 | `*` `/` `%` | Left |
| 5 | `+` `-` | Left |
| 6 | `&` | Left |
| 7 | `^` | Left |
| 8 | `\|` | Left |
| 9 | `<` `>` `<=` `>=` | Left |
| 10 | `==` `!=` | Left |
| 11 | `and` | Left |
| 12 (lowest) | `or` | Left |

### 9.8 Structural Equality 🟢

`==` and `!=` are supported only for:
- Primitive numeric types (integers, `bool`)
- Pointers

Using `==` on structs, arrays, or ADTs is a compile error. Field-by-field comparison must be done manually.

---

## 10. Type Constraints & Static Verification

### 10.1 Parameter Constraints 🟢

Constraints on parameters are written after the type:

```lain
func safe_div(a int, b int != 0) int {
    return a / b
}

safe_div(10, 2)     // ✅ OK: 2 != 0
safe_div(10, 0)     // ❌ ERROR: 0 violates b != 0
```

**Supported constraint operators**:

| Constraint | Meaning |
|:-----------|:--------|
| `b int != 0` | b must not be zero |
| `b int > 0` | b must be positive |
| `b int >= 0` | b must be non-negative |
| `b int < a` | b must be less than a |
| `b int > a` | b must be greater than a |
| `b int == 1` | b must equal 1 |

### 10.2 Return Type Constraints 🟢

```lain
func abs(x int) int >= 0 {
    if x < 0 { return 0 - x }
    return x
}

// ❌ ERROR: -1 does not satisfy >= 0
func bad_abs(x int) int >= 0 {
    return -1
}
```

### 10.3 Index Bounds (`in` keyword) 🟢

The `in` keyword declares compile-time index bounds:

```lain
func get(arr int[10], i int in arr) int {
    return arr[i]      // Always safe — i ∈ [0, 9]
}
```

Desugars to: `i >= 0 and i < arr.len`.

### 10.4 Relational Constraints 🟢

Constraints can reference other parameters:

```lain
func require_lt(a int, b int > a) int {
    return b - a     // Always positive
}

func clamp(x int, lo int, hi int >= lo) int >= lo and <= hi {
    if x < lo { return lo }
    if x > hi { return hi }
    return x
}
```

### 10.5 Multiple Constraints 🟢

Chain with `and`:

```lain
func bounded(x int >= 0 and <= 100) int {
    return x
}
```

### 10.6 Value Range Analysis (VRA) 🟢

The compiler uses **polynomial-time** static analysis to verify constraints:

1. **Interval tracking**: Every integer carries `[min, max]`.
   - `10` → `[10, 10]`, `u8` → `[0, 255]`, `int` → `[-2³¹, 2³¹-1]`

2. **Arithmetic propagation**:
   - `[a, b] + [c, d]` → `[a+c, b+d]`
   - `[a, b] - [c, d]` → `[a-d, b-c]`

3. **Control flow refinement**:
   - Inside `if x < 10`: `x` narrowed to `[min, 9]`
   - In `else`: `x` narrowed to `[10, max]`
   - After merge: conservative hull (union)

4. **Assignment tracking**: Ranges update through assignments.

5. **Linear constraint propagation**: `var x = y + 1` → compiler knows `x > y`.

**No SMT Solver**: All verification is decidable, polynomial-time, fast and predictable.

### 10.7 Loop Widening 🟢

Variables modified inside loops are conservatively widened to the type's full range:

```lain
var x = 0
for i in 0..10 {
    x = x + 1
}
// x is widened to [INT_MIN, INT_MAX] after the loop
```

> This is a known limitation. Future versions may support loop invariant annotations.

---

## 11. Module System

### 11.1 Import 🟢

```lain
import std.c            // Loads std/c.ln
import std.io           // Loads std/io.ln
import std.fs           // Loads std/fs.ln
import std.fs as fs     // Namespace aliasing
```

Without `as`: all public declarations are injected into the current scope.
With `as`: accessed via prefix (`fs.open_file()`).

### 11.2 Module Path Resolution 🟢

Dot-notation maps to filesystem paths: `import tests.stdlib.dummy` → `tests/stdlib/dummy.ln`.

### 11.3 Name Resolution 🟢

Multi-pass compilation: functions, types, and procedures can be referenced before declaration. No forward declarations or header files needed.

### 11.4 Visibility 🟢 (partial)

Currently all top-level declarations are public. The `export` keyword is reserved for future visibility control:

```lain
// 🔴 Future
export func public_api() int { return 42 }
func internal_helper() int { return 0 }  // Not exported
```

---

## 12. C Interoperability

### 12.1 `c_include` Directive 🟢

Directly includes a C header in the generated output:

```lain
c_include "<stdio.h>"
c_include "<stdlib.h>"
c_include "my_header.h"
```

### 12.2 Extern Functions 🟢

Declare C functions with Lain ownership annotations:

```lain
extern func abs(n int) int
extern proc printf(fmt *u8, ...) int
extern proc malloc(size usize) mov *void
extern proc free(ptr mov *void)
extern proc fopen(filename *u8, mode *u8) mov *FILE
extern proc fclose(stream mov *FILE) int
```

Key features:
- `mov` annotations on extern parameters/returns enable ownership tracking across FFI.
- `extern func` = pure (callable from `func`), `extern proc` = impure (only from `proc`).
- Variadic parameters (`...`) are supported in extern declarations only.

### 12.3 Extern Types 🟢

Opaque C types:

```lain
extern type FILE
extern type sqlite3
```

### 12.4 Type Mapping 🟢

| Lain Type | C Type |
|:----------|:-------|
| `int` | `int` |
| `u8` | `unsigned char` |
| `usize` | `size_t` |
| `*u8` (shared) | `const char *` |
| `var *u8` (mutable) | `char *` |
| `*T` (shared) | `const T*` |
| `var *T` | `T*` |
| `mov *T` | `T*` |

### 12.5 Name Mangling 🟢 (partial)

Currently uses `-D` flags for C mapping (e.g., `-Dlibc_printf=printf`). A formalized internal mangling scheme (`lain_module_funcName`) is planned.

---

## 13. Unsafe Code

### 13.1 Unsafe Blocks 🟢

Operations bypassing safety must be in `unsafe`:

```lain
unsafe {
    var val = *ptr      // Pointer dereference: OK
}
// var val = *ptr       // ❌ ERROR: outside unsafe
```

### 13.2 What `unsafe` Allows

| Operation | Safe | Unsafe |
|-----------|------|--------|
| Pointer dereference (`*ptr`) | ❌ | ✅ |
| Address-of (`&x`) | ❌ | ✅ |
| Raw pointer casts | ❌ | ✅ |
| **Linearity tracking** | ✅ enforced | ✅ **still enforced** |
| **Borrow checking** | ✅ enforced | ✅ **still enforced** |
| **Move tracking** | ✅ enforced | ✅ **still enforced** |

**Critical**: Ownership, linearity, and borrowing are **never** suspended by `unsafe`. Only raw pointer operations are unlocked.

### 13.3 Nesting 🟢

Unsafe blocks can be nested and combined with control flow:

```lain
unsafe {
    if condition {
        var val = *p    // OK: inside unsafe
    }
}
```

### 13.4 The Address-Of Operator (`&`) 🟢

Creates a raw pointer. Only allowed inside `unsafe`:

```lain
proc main() int {
    var x = 42
    unsafe {
        var p = &x      // p is *int
        *p = 100        // Mutates x through raw pointer
    }
    return x            // Returns 100
}
```

---

## 14. Memory Model

### 14.1 Stack Allocation 🟢

All local variables, structs, and arrays are **stack-allocated**. There is no implicit heap allocation:

```lain
var x int = 42           // Stack
var arr int[100]         // Stack — 400 bytes on stack
var p Point              // Stack
```

### 14.2 Heap Allocation (via C Interop) 🟢

Heap allocation is explicit, using C's `malloc`/`free`:

```lain
extern proc malloc(size usize) mov *void
extern proc free(ptr mov *void)

proc main() int {
    var ptr = malloc(1024)    // Heap allocation — owned
    // ... use ptr ...
    free(mov ptr)             // Explicit deallocation
    return 0
}
```

Lain's ownership system tracks heap resources via `mov` — preventing leaks and double-frees.

### 14.3 No Garbage Collector 🟢

Lain never performs automatic memory management:
- **Deterministic deallocation**: Resources freed at known, predictable points.
- **Zero runtime overhead**: No GC pauses, no ref-counting, no tracing.
- **Embedded-friendly**: No runtime allocator beyond what the programmer explicitly uses.

### 14.4 Value Semantics vs Reference Semantics

| Type | Semantics | Assignment `b = a` | Pass to function |
|------|-----------|--------------------|-----------------| 
| Primitives (`int`, `bool`, etc.) | Value (copy) | Copies bits | Copies value |
| Structs (no `mov` fields) | Value (copy) | Copies all fields | Pass by `const T*` |
| Structs (with `mov` fields) | Linear | ❌ Copy forbidden | Requires `mov` or borrow |
| Arrays (non-linear elements) | Value (copy) | Copies all elements | Pass by `const T*` |
| Arrays (linear elements) | Linear | ❌ Copy forbidden | Requires `mov` |
| Slices | Fat pointer (copy) | Copies `{data, len}` | Pass by value |
| Raw Pointers | Value (copy) | Copies address | Copies address |

### 14.5 Alignment & Layout 🟢

Lain inherits C99's alignment and padding rules. Structs are laid out in declaration order with platform-specific padding. No `#pragma pack` equivalent exists yet.

---

## 15. Error Handling

### 15.1 No Exceptions 🟢

Lain has no exceptions, no `try`/`catch`, no stack unwinding. Errors are handled through return values.

### 15.2 Current Approach: Return Codes 🟢

```lain
proc try_operation() int {
    // Return 0 for success, -1 for failure
    if failed { return -1 }
    return 0
}
```

### 15.3 The `Option` / `Result` Pattern 🟢

Error handling is formalized through ADTs:

```lain
type OptionInt {
    Some { value int }
    None
}

type ResultFile {
    Ok  { f File }
    Err { code int }
}

// Usage with pattern matching
var result = try_open("file.txt")
case result {
    Ok(f):    write_file(f, "hello")
    Err(code): libc_printf("Error: %d\n", code)
}
```

### 15.4 Generic Option/Result 🔴

When generics are implemented (§18), these become:

```lain
// 🔴 Target
func Option(comptime T type) type {
    return type {
        Some { value T }
        None
    }
}

func Result(comptime T type, comptime E type) type {
    return type {
        Ok  { value T }
        Err { error E }
    }
}
```

### 15.5 Error Propagation Operator 🔴

A `?` operator for concise error propagation is a target feature:

```lain
// 🔴 Target
proc read_config() Result(Config, int) {
    var f = try_open("config.txt")?   // Returns Err early if failed
    var data = read_all(f)?
    return Result.Ok(parse(data))
}
```

---

## 16. Initialization & Zero Values

### 16.1 Mandatory Initialization 🟢

All variables must be explicitly initialized. Uninitialized variables require the `undefined` keyword:

```lain
var x int = 0              // Initialized to 0
var y int = undefined      // Explicitly uninitialized
```

### 16.2 Definite Initialization Analysis 🟢

The compiler flow-sensitively tracks whether `undefined` variables are assigned before use:

```lain
var x int = undefined
if condition {
    x = 42
} else {
    x = 0
}
// ✅ OK: x is definitely initialized on all paths

var y int = undefined
if condition {
    y = 42
}
// ❌ ERROR: y may not be initialized (no else branch)
libc_printf("%d", y)
```

### 16.3 Struct Initialization 🟢

All fields must be provided (or explicitly marked `undefined`):

```lain
var p = Point(10, 20)              // ✅ All fields
var q = Point(10, undefined)       // ✅ Explicit partial init
```

---

## 17. Arithmetic & Overflow

### 17.1 Overflow Semantics 🟢

| Type Category | Overflow Behavior |
|:-------------|:------------------|
| **Signed** (`int`, `i8`–`i64`, `isize`) | Two's Complement wrap-around (compiled with `-fwrapv`) |
| **Unsigned** (`u8`–`u64`, `usize`) | Wrapping (modular arithmetic) |
| **Floating-point** (`f32`, `f64`) | IEEE 754: overflow → ±infinity |

### 17.2 Checked Arithmetic 🔴

Target: built-in checked operations that detect overflow:

```lain
// 🔴 Target
var result = checked_add(a, b)
case result {
    Ok(v):     use(v)
    Overflow:  handle_error()
}
```

---

## 18. Generics & Compile-Time Evaluation

### 18.1 The `comptime` Approach 🟢 (Phase A)

Lain uses compile-time function evaluation instead of traditional generic syntax (`<T>`):

```lain
func Option(comptime T type) type {
    return type {
        Some { value T }
        None
    }
}

type OptionInt = Option(int)
```

### 18.2 Monomorphization 🟢

When a generic function is called, the compiler generates a specialized version:

```lain
func identity(comptime T type, x T) T {
    return x
}

// identity(int, 42) generates identity_int(int x)
// identity(bool, true) generates identity_bool(bool x)
```

### 18.3 Compile-Time Constraints 🔴

Target: `comptime` parameters with constraints:

```lain
// 🔴 Target
func Array(comptime T type, comptime N int > 0) type {
    return type {
        data T[N],
        len int
    }
}
```

### 18.4 Interaction with Ownership 🔴

Generic types propagate linearity naturally:
- If `T` is linear, `Option(T)` is linear.
- If `T` is copyable, `Option(T)` is copyable.

`sema_type_is_linear()` handles this recursively after monomorphization.

---

## 19. String & Text Handling

### 19.1 Byte-Oriented Strings 🟢

Lain strings are **byte arrays**, not character arrays:

```lain
var s = "Hello"      // Type: u8[:0], .len = 5
```

- `.len` returns **byte count** (not Unicode codepoints)
- `.data` returns `*u8` (raw pointer to bytes)

### 19.2 Encoding 🟢

No encoding is enforced. String literals are stored as raw bytes in source encoding (typically UTF-8). Multi-byte characters are multiple `u8` values:

```lain
var s = "café"       // .len = 5 (4 ASCII + 1 two-byte UTF-8)
```

### 19.3 String Operations 🔴

Target: string manipulation via standard library functions (not built-in operators):

```lain
// 🔴 Target stdlib
import std.string

var s = "hello"
var upper = s.to_upper()
var contains = s.contains("ell")
```

---

## 20. Compilation Pipeline

### 20.1 Architecture 🟢

```
 .ln source → [Lain Compiler] → out.c → [C Compiler] → executable
```

Lain compiles to **C99** code, then uses any C99 compiler (GCC, Clang, cosmocc) to produce a native binary.

### 20.2 Building the Compiler 🟢

The compiler is itself written in C99:

```bash
gcc src/main.c -o src/compiler.exe -std=c99 -Wall -Wextra \
    -Wno-unused-function -Wno-unused-parameter
```

### 20.3 Compiling Programs 🟢

```bash
# Step 1: Lain → C
./compiler.exe my_program.ln

# Step 2: C → Executable
gcc out.c -o my_program -Dlibc_printf=printf -Dlibc_puts=puts

# Step 3: Run
./my_program
```

### 20.4 C Compilation Flags 🟢

| Flag | Purpose |
|:-----|:--------|
| `-Dlibc_printf=printf` | Maps Lain's `libc_printf` to C's `printf` |
| `-Dlibc_puts=puts` | Maps Lain's `libc_puts` to C's `puts` |
| `-w` | Suppress C compiler warnings from generated code |
| `-fwrapv` | Guarantee two's complement wrapping for signed overflow |

### 20.5 Compiler Passes 🟢

The compiler performs the following passes in order:

| Pass | Module | Purpose |
|------|--------|---------|
| 1. Lexing | `lexer.h` | Source → Token stream |
| 2. Parsing | `parser.h` | Tokens → AST |
| 3. Name Resolution | `resolve.h` | Resolve identifiers, build scopes |
| 4. Type Checking | `typecheck.h` | Infer types, check constraints, VRA |
| 5. Exhaustiveness | `exhaustiveness.h` | Verify pattern matching completeness |
| 6. NLL Pre-pass | `use_analysis.h` | Compute last-use for each identifier |
| 7. Linearity Check | `linearity.h` | Ownership, borrows, moves, NLL release |
| 8. Code Emission | `emit.h` | AST → C99 source code |

### 20.6 Test Framework 🟢

Tests live under `tests/` and run via `run_tests.sh`:

- **Positive tests** (`*.ln`): Must compile and run successfully.
- **Negative tests** (`*_fail.ln`): Must **fail** compilation.

```bash
./run_tests.sh              # Run all tests
./run_tests.sh tests/core/functions.ln  # Run single test
```

| Directory | Count | Purpose |
|:----------|:------|:--------|
| `tests/core/` | 12 | Functions, loops, math, bitwise, compound, shadowing |
| `tests/types/` | 14 | ADTs, enums, arrays, structs, strings, casts |
| `tests/safety/bounds/` | 14 | Static bounds checking & constraints |
| `tests/safety/ownership/` | 24 | Ownership, borrowing, moves, NLL |
| `tests/safety/purity/` | 3 | Purity enforcement |
| `tests/stdlib/` | 6 | Module system, extern, stdlib |

---

## 21. Diagnostics & Error Reporting

### 21.1 Current Error Format 🟢

```
Error Ln 5, Col 20: cannot access 'data' because it is mutably borrowed by 'ref'.
```

### 21.2 Target Error Format 🔴

Rich, context-aware diagnostics with source spans:

```
error[E0502]: cannot borrow `data` as shared because it is mutably borrowed
  --> main.ln:5:20
   |
3  | var ref = get_ref(var data)
   |                  -------- mutable borrow occurs here
5  | var x = read_data(data)
   |                   ^^^^ shared borrow occurs here
7  | consume_ref(var ref)
   |             ------- mutable borrow used here (last use)
   |
   = help: move the use of `ref` before accessing `data`
```

### 21.3 Error Categories 🟢

| Category | Examples |
|----------|---------|
| **Syntax** | Unexpected token, missing brace |
| **Type** | Type mismatch, unknown type |
| **Ownership** | Use after move, double free, forgot consume |
| **Borrow** | Mutable + shared conflict, dangling reference |
| **Bounds** | Constraint violation, index out of range |
| **Purity** | `func` calling `proc`, `while` in `func` |
| **Exhaustiveness** | Missing case arm |

---

## 22. Standard Library

### 22.1 `std/c.ln` — C Bindings 🟢

```lain
c_include "<stdio.h>"
c_include "<stdlib.h>"

extern type FILE

extern proc printf(fmt *u8, ...) int
extern proc fopen(filename *u8, mode *u8) mov *FILE
extern proc fclose(stream mov *FILE) int
extern proc fputs(s *u8, stream *FILE) int
extern proc fgets(s var *u8, n int, stream *FILE) var *u8
extern proc libc_printf(fmt *u8, ...) int
extern proc libc_puts(s *u8) int
```

### 22.2 `std/io.ln` — Basic I/O 🟢

```lain
import std.c

proc print(s u8[:0]) {
    libc_printf(s.data)
}

proc println(s u8[:0]) {
    libc_puts(s.data)
}
```

### 22.3 `std/fs.ln` — File System 🟢

```lain
import std.c

type File {
    mov handle *FILE       // Linear — must be closed
}

proc open_file(path u8[:0], mode u8[:0]) mov File {
    var raw = fopen(path.data, mode.data)
    return File(raw)
}

proc close_file(mov {handle} File) {
    fclose(handle)
}

proc write_file(f File, s u8[:0]) {
    fputs(s.data, f.handle)
}
```

### 22.4 Target Standard Library 🔴

| Module | Purpose | Priority |
|--------|---------|----------|
| `std.string` | String utilities (split, join, contains, uppercase) | High |
| `std.math` | Math functions (abs, min, max, pow) | High |
| `std.mem` | Arena allocator, pool allocator | High |
| `std.collections` | Dynamic array, hash map (requires generics) | Medium |
| `std.fmt` | Type-safe formatting (printf replacement) | Medium |
| `std.os` | OS abstraction (file system, process, env) | Low |
| `std.net` | Networking (TCP/UDP sockets) | Low |

---

## 23. Design Decisions & Rationale

### 23.1 Why `func` vs `proc`?

**Decision**: Separate pure functions from impure procedures at the type level.

**Rationale**: This gives the compiler strong guarantees about `func`:
- Always terminates → safe for compile-time evaluation.
- No side effects → safe for parallelization, memoization, and formal verification.
- The caller of a `func` knows it cannot mutate global state, allocate memory, or perform I/O.

**Trade-off**: Forces the programmer to think about purity. Some functions that "should" be `func` (e.g., logging wrappers) must be `proc`. This is intentional — purity is a contract.

### 23.2 Why `mov` at Call Sites?

**Decision**: Require `mov` at every call site where ownership is transferred.

**Rationale**: Reading code is more common than writing it. When a reader sees `close(mov f)`, they immediately know `f` is consumed. Without the annotation, they would need to find the function declaration.

**Comparison**: Rust moves implicitly (the reader must check if the type implements `Copy`). Lain's approach prioritizes local readability.

### 23.3 Why `var` for Mutability Instead of `let`/`let mut`?

**Decision**: Use `var x = ...` for all bindings, with mutability determined by type annotation.

**Rationale**: Simplicity. One keyword (`var`) for all bindings. Mutability is an orthogonal concern expressed through the type/mode system, not through the declaration keyword.

**Trade-off**: `var x = 42` being immutable while `var x T = 42` can be mutable is a known source of confusion. This may be revisited.

### 23.4 Why No Lifetime Annotations?

**Decision**: Borrows have inferred lifetimes based on scope and NLL analysis. No `'a` syntax.

**Rationale**: Lain's structured control flow (no goto, no closures capturing references yet) makes most lifetime inference trivial. The NLL pre-pass handles cross-statement analysis. When ambiguity arises (multiple `var` parameters), the compiler rejects the program — forcing the programmer to restructure rather than annotate.

**Trade-off**: Some valid programs are rejected that Rust would accept with lifetime annotations. This is acceptable if it keeps the language simpler for the 95% case.

### 23.5 Why Compile to C99?

**Decision**: Target C99 as the backend instead of LLVM IR, machine code, or WebAssembly.

**Rationale**:
- **Portability**: C compilers exist for every platform (embedded, exotic architectures).
- **Optimization**: GCC/Clang provide world-class optimization for free.
- **Interop**: C interop is trivial — just emit `#include` and call functions directly.
- **Simplicity**: The compiler is ~5000 lines of C. An LLVM backend would triple this.
- **Bootstrapping**: The Lain compiler can be compiled by any C99 compiler.

**Trade-off**: Debugging compiled programs shows C code, not Lain source. Source maps are planned (§24).

### 23.6 Why No Exceptions?

**Decision**: Errors are values, not control flow constructs.

**Rationale**:
- Exceptions have hidden control flow — any function call might throw, making reasoning difficult.
- Exception handling requires stack unwinding machinery — runtime overhead incompatible with embedded.
- ADT-based error handling (`Result`, `Option`) forces callers to handle all failure paths — errors cannot be silently ignored.

### 23.7 Why Explicit `unsafe`?

**Decision**: Raw pointer operations require `unsafe` blocks, but ownership/linearity are **still enforced** inside `unsafe`.

**Rationale**: The `unsafe` boundary is about what the programmer claims responsibility for:
- Raw pointer validity? Programmer's responsibility.
- Ownership tracking? Still the compiler's job.
- This makes `unsafe` blocks smaller and more auditable than in languages where `unsafe` disables *all* checks.

---

## 24. Phased Roadmap

### Phase 1 — Core Language (✅ Complete)

Primitives, structs, enums, ADTs, arrays, slices, functions, procedures, control flow, pattern matching, exhaustiveness, basic ownership, linearity, borrow checking, VRA, modules, C interop, `unsafe`, `defer`.

### Phase 2 — NLL & Safety Hardening (✅ Complete)

Non-lexical lifetimes, use-analysis pre-pass, persistent borrows, cross-statement conflict detection, definite initialization analysis.

### Phase 3 — Soundness Completion (🔴 In Progress)

Re-borrow transitivity, per-field linearity, shared persistent borrows, lifetime inference for multi-`var`-param functions, full owner reassignment checks.

### Phase 4 — Generics & Standard Library (🔴 Next)

Full `comptime` generics, generic `Option`/`Result`, standard library expansion (string, math, memory, collections).

### Phase 5 — Ergonomics & Tooling (🟡 Future)

Block-level NLL, rich diagnostics with source spans, error propagation (`?` operator), two-phase borrows, non-consuming pattern matching, source maps for debugging.

### Phase 6 — Advanced Features (🟡 Long-term)

Closures (with captured borrows), traits/interfaces, concurrency model (`Send`/`Sync`), async/await, self-hosting compiler, comprehensive documentation generator.

---

## Appendix A: Complete Keyword List

| Keyword | Status | Description |
|:--------|:-------|:------------|
| `and` | ✅ | Logical AND |
| `as` | ✅ | Type cast (§9.6) |
| `break` | ✅ | Loop exit |
| `c_include` | ✅ | C header inclusion |
| `case` | ✅ | Pattern matching (§8.5) |
| `comptime` | 🟢 Phase A | Compile-time parameter |
| `continue` | ✅ | Loop skip |
| `defer` | ✅ | Deferred execution (§8.8) |
| `elif` | ✅ | Else-if branch |
| `else` | ✅ | Default branch |
| `export` | 🔮 Reserved | Module visibility |
| `extern` | ✅ | C interop declarations |
| `false` | ✅ | Boolean literal |
| `for` | ✅ | Range loop |
| `func` | ✅ | Pure function |
| `if` | ✅ | Conditional |
| `import` | ✅ | Module import |
| `in` | ✅ | Range iteration / index bounds |
| `mov` | ✅ | Ownership transfer |
| `or` | ✅ | Logical OR |
| `post` | 🔮 Reserved | Postcondition |
| `pre` | 🔮 Reserved | Precondition |
| `proc` | ✅ | Procedure |
| `return` | ✅ | Return value |
| `true` | ✅ | Boolean literal |
| `type` | ✅ | Type definition |
| `undefined` | ✅ | Uninitialized marker |
| `unsafe` | ✅ | Unsafe block |
| `var` | ✅ | Mutable binding / mutable mode |
| `while` | ✅ | While loop (proc only) |

---

## Appendix B: Grammar (Pseudo-BNF)

```
program         = { top_level_decl } ;

top_level_decl  = import_decl | c_include_decl | extern_decl
                | type_decl | var_decl | func_decl | proc_decl ;

import_decl     = "import" module_path [ "as" IDENT ] ;
module_path     = IDENT { "." IDENT } ;

c_include_decl  = "c_include" STRING_LITERAL ;

extern_decl     = "extern" ( "type" IDENT
                           | ("func" | "proc") IDENT "(" param_list ")" [type_expr] ) ;

type_decl       = "type" IDENT "{" type_body "}" ;
type_body       = { field_decl | variant_decl } ;
field_decl      = ["mov"] IDENT type_expr ;
variant_decl    = IDENT [ "{" field_list "}" ] ;

func_decl       = "func" IDENT "(" param_list ")" [type_expr [constraints]] block ;
proc_decl       = "proc" IDENT "(" param_list ")" [type_expr [constraints]] block ;

param_list      = [ param { "," param } ] ;
param           = ["var" | "mov"] IDENT type_expr [constraints] ;

constraints     = constraint { "and" constraint } ;
constraint      = ("!=" | "==" | "<" | ">" | "<=" | ">=") expr
                | "in" IDENT ;

type_expr       = IDENT
                | "*" type_expr
                | type_expr "[" NUMBER "]"
                | type_expr "[" "]"
                | type_expr "[" ":" expr "]" ;

block           = "{" { statement } "}" ;

statement       = var_decl | assignment | return_stmt | if_stmt
                | for_stmt | while_stmt | case_stmt | break_stmt
                | continue_stmt | defer_stmt | unsafe_block | expr_stmt ;

defer_stmt      = "defer" block ;

var_decl        = ["var"] IDENT [type_expr] "=" expr ;
assignment      = lvalue assign_op expr ;
assign_op       = "=" | "+=" | "-=" | "*=" | "/=" | "%="
                | "&=" | "|=" | "^=" ;

return_stmt     = "return" ["mov" | "var"] [expr] ;

if_stmt         = "if" expr block { "elif" expr block } [ "else" block ] ;
for_stmt        = "for" IDENT ["," IDENT] "in" expr ".." expr block ;
while_stmt      = "while" expr block ;

case_stmt       = "case" expr "{" { case_arm } "}" ;
case_arm        = pattern { "," pattern } ":" (expr | block) ;
pattern         = IDENT [ "(" pattern_list ")" ]
                | literal | range | "else" ;

unsafe_block    = "unsafe" block ;

expr            = literal | IDENT | expr binop expr | unop expr
                | expr "." IDENT | expr "[" expr "]"
                | expr "(" arg_list ")"
                | "mov" expr | "var" expr | "(" expr ")"
                | expr "as" type_expr
                | "case" expr "{" { case_arm } "}" ;

binop           = "+" | "-" | "*" | "/" | "%" | "==" | "!="
                | "<" | ">" | "<=" | ">=" | "and" | "or"
                | "&" | "|" | "^" ;

unop            = "-" | "!" | "~" | "*" ;

literal         = NUMBER | FLOAT | CHAR_LITERAL | STRING_LITERAL
                | "true" | "false" | "undefined" ;
```

---

## Appendix C: Differences from Rust, C, Zig

| Feature | C | Rust | Zig | Lain |
|---------|---|------|-----|------|
| Memory safety | ❌ None | ✅ Borrow checker | ⚠️ Optional (runtime) | ✅ Borrow checker (compile-time) |
| GC | ❌ | ❌ | ❌ | ❌ |
| Null pointers | ✅ Pervasive | ❌ `Option` | ✅ Optional ptrs | ❌ Safe refs can't be null |
| Move semantics | ❌ | ✅ Implicit | ❌ | ✅ Explicit `mov` |
| Purity enforcement | ❌ | ❌ | ❌ | ✅ `func` vs `proc` |
| Generics | ❌ (macros) | ✅ Trait-based | ✅ Comptime | ✅ Comptime |
| Exceptions | ❌ | ❌ `Result` | ✅ Error unions | ❌ ADT `Result` |
| Lifetime annotations | N/A | ✅ Required | N/A | ❌ Inferred |
| Bounds checking | ❌ None | ✅ Runtime | ✅ Runtime | ✅ **Compile-time** (VRA) |
| Backend | Native | LLVM | LLVM | **C99** |
| Self-hosting | ✅ | ✅ | ✅ | 🔴 Planned |
| Build system | Make/CMake | Cargo | `zig build` | Shell scripts |
| Call-site annotations | ❌ | ❌ | ❌ | ✅ `var`, `mov` |
| Termination proof | ❌ | ❌ | ❌ | ✅ `func` guaranteed |
| Unsafe boundary | N/A (all unsafe) | `unsafe` blocks | `@extern`/`allowzero` | `unsafe` blocks |

---

## Safety Guarantees Summary

| Safety Concern | Guarantee | Mechanism |
|:---------------|:----------|:----------|
| **Buffer Overflows** | Impossible | Static VRA (§10) — compile-time bounds proof |
| **Use-After-Free** | Impossible | Linear types (`mov`) — exactly-once consumption |
| **Double Free** | Impossible | Linear types — compile-time uniqueness |
| **Data Races** | Impossible | Borrow checker — exclusive mutability (§6) |
| **Null Dereference** | Prevented | Safe references cannot be null; raw ptrs need `unsafe` |
| **Memory Leaks** | Prevented | Linear variables must be consumed |
| **Division by Zero** | Preventable | Constraints (`b int != 0`) |
| **Integer Overflow** | Documented | Two's complement wrapping (`-fwrapv`) |
| **Uninitialized Memory** | Prevented | Definite Initialization Analysis (§16) |

---

*End of Specification*
