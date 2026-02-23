# Lain Programming Language Specification

**Version**: 0.1.0 (Draft)

Lain is a **critical-safe** programming language designed for embedded systems. It focuses on memory safety, type safety, and determinism with **zero runtime overhead**.

---

## 1. Core Philosophy

- **Memory Safety**: Guaranteed via linear types and borrow checking. No Garbage Collector.
- **Purity**: Strict distinction between deterministic functions (`func`) and side-effecting procedures (`proc`).
- **Zero Overhead**: No runtime checks for bounds or ownership. All verification is purely static, at compile time.
- **Determinism**: Pure functions (`func`) are guaranteed to terminate. Recursion and unbounded loops are restricted to `proc`.
- **C Interop**: Compiles to portable C99 via `cosmocc`. Native interoperability with C libraries.

---

## 2. Lexical Structure

### 2.1 Keywords

The following identifiers are reserved keywords and cannot be used as variable or function names.

**Core keywords:**
| Keyword | Purpose |
|:--------|:--------|
| `var` | Mutable variable declaration |
| `mov` | Ownership transfer (move semantics) |
| `type` | Type definition (structs, enums, ADTs) |
| `func` | Pure function declaration |
| `proc` | Procedure declaration (side effects allowed) |
| `return` | Return a value from a function/procedure |
| `if` | Conditional branch |
| `elif` | Else-if branch |
| `else` | Default branch in conditional/case |
| `for` | Range-based for loop |
| `while` | While loop |
| `break` | Exit the innermost loop |
| `continue` | Skip to the next iteration |
| `case` | Pattern matching |
| `in` | Range iteration / index bound constraint |
| `and` | Logical AND |
| `or` | Logical OR |
| `true` | Boolean true literal |
| `false` | Boolean false literal |
| `as` | Type cast operator |
| `import` | Module import |
| `extern` | External (C) declaration |
| `unsafe` | Unsafe block |
| `c_include` | Include a C header file |

> [!NOTE]
> **Reserved for future use**: The following keywords are recognized by the lexer but not yet fully implemented:
> `comptime`, `macro`, `expr`, `pre`, `post`, `use`, `end`, `export`.
> The keyword `fun` is accepted as an alias for `func`.

### 2.2 Operators & Punctuation

**Arithmetic operators:**
| Operator | Description |
|:---------|:------------|
| `+` | Addition |
| `-` | Subtraction / Unary negation |
| `*` | Multiplication / Pointer dereference |
| `/` | Division |
| `%` | Modulo |

**Comparison operators:**
| Operator | Description |
|:---------|:------------|
| `==` | Equal |
| `!=` | Not equal |
| `<` | Less than |
| `>` | Greater than |
| `<=` | Less than or equal |
| `>=` | Greater than or equal |

**Logical operators:**
| Operator | Description |
|:---------|:------------|
| `and` | Logical AND (keyword) |
| `or` | Logical OR (keyword) |
| `!` | Logical NOT (prefix) |

**Bitwise operators:**
| Operator | Description |
|:---------|:------------|
| `&` | Bitwise AND |
| `\|` | Bitwise OR |
| `^` | Bitwise XOR |
| `~` | Bitwise NOT (complement) |

**Assignment operators:**
| Operator | Description |
|:---------|:------------|
| `=` | Assignment |
| `+=` | Add and assign |
| `-=` | Subtract and assign |
| `*=` | Multiply and assign |
| `/=` | Divide and assign |
| `%=` | Modulo and assign |
| `&=` | Bitwise AND and assign |
| `\|=` | Bitwise OR and assign |
| `^=` | Bitwise XOR and assign |

> [!NOTE]
> Compound assignments (`+=`, `-=`, etc.) are desugared by the parser into `x = x + expr` form.

**Other punctuation:**
| Symbol | Description |
|:-------|:------------|
| `(` `)` | Grouping, function calls, tuple construction |
| `[` `]` | Array indexing, array/slice type syntax |
| `{` `}` | Blocks, struct/ADT bodies |
| `.` | Field access, module path separator |
| `..` | Range (exclusive end) |
| `..=` | Range (inclusive end) — *reserved* |
| `...` | Variadic parameters (in `extern` declarations) |
| `,` | Separator in lists |
| `:` | Match arm separator, sentinel in slice types |
| `;` | Statement terminator (optional) |

### 2.3 Literals

**Integer literals:**
```lain
42          // Decimal integer
0           // Zero
-1          // Negative (unary minus + literal)
```
> [!WARNING]
> Only decimal integer literals are currently supported. Hex, octal, and binary literals are not yet implemented.

**Character literals:**
```lain
'A'         // Character literal
'\n'        // Escape sequence
```

**String literals:**
```lain
"Hello, World!\n"    // String literal with escape sequence
```
String literals have type `u8[:0]` (null-terminated sentinel slice). They expose two fields:
- `.data` — pointer to the raw bytes (`*u8`)
- `.len` — length of the string (excluding the sentinel)

```lain
var s = "Hello"
libc_printf("Length: %d\n", s.len)   // 5
libc_printf("Content: %s\n", s.data) // Hello
```

### 2.4 Comments

```lain
// Single-line comment
/* Multi-line
   comment */
```

### 2.5 Statement Termination

Semicolons are **optional**. Both styles are valid:

```lain
var x = 10;    // With semicolon
var y = 20     // Without semicolon
```

---

## 3. Type System

### 3.1 Primitive Types

**Integers:**

| Type | Description | C Equivalent |
|:-----|:------------|:-------------|
| `int` | Signed integer (platform-dependent, typically 32-bit) | `int` |
| `i8` | Signed 8-bit integer | `int8_t` |
| `i16` | Signed 16-bit integer | `int16_t` |
| `i32` | Signed 32-bit integer | `int32_t` |
| `i64` | Signed 64-bit integer | `int64_t` |
| `u8` | Unsigned 8-bit integer | `uint8_t` |
| `u16` | Unsigned 16-bit integer | `uint16_t` |
| `u32` | Unsigned 32-bit integer | `uint32_t` |
| `u64` | Unsigned 64-bit integer | `uint64_t` |
| `isize` | Signed pointer-sized integer | `ptrdiff_t` |
| `usize` | Unsigned pointer-sized integer | `size_t` |

> [!NOTE]
> Fixed-width integer types require `<stdint.h>` in the generated C code (included automatically).
> The type `int` is platform-dependent (typically 32-bit). Prefer `i32` for portable fixed-width semantics.

**Boolean:**

| Type | Description | C Equivalent |
|:-----|:------------|:-------------|
| `bool` | Boolean type with values `true` and `false` | `_Bool` |

`bool` is a distinct type. The literals `true` and `false` are keywords:

```lain
var x bool = true
var y bool = false

if x {
    libc_printf("x is true\n")
}
```

> [!NOTE]
> In the generated C code, `true` maps to `1` and `false` maps to `0`. Boolean values can be used directly in conditions without comparison.

### 3.2 Pointer Types

Pointer types use the prefix `*` syntax:

```lain
*int       // Pointer to int
*u8        // Pointer to u8 (C string compatible)
*void      // Opaque pointer (like C's void*)
```

Raw pointer dereference is only allowed inside `unsafe` blocks (see §12).

### 3.3 Array Types

Arrays are fixed-size, stack-allocated collections. The size is part of the type.

```lain
var arr int[5]        // Array of 5 ints
var bytes u8[256]     // Array of 256 bytes
```

**Indexing:**
```lain
arr[0] = 10
var x = arr[0]
```

Array indices are **statically verified** at compile time. Accessing an out-of-bounds index is a compile error (see §9).

### 3.4 Slice Types

Slices are dynamic views into arrays. They consist of a pointer and a length.

```lain
var s int[] = arr[0..3]   // Slice of elements [0, 1, 2]
```

**Sentinel-terminated slices** are slices that end with a known sentinel value (typically `0`):

```lain
u8[:0]     // Null-terminated byte slice (C string compatible)
```

Sentinel slices expose:
- `.data` — pointer to the underlying data
- `.len` — number of elements (excluding sentinel)

### 3.5 String Literals

String literals have type `u8[:0]` (null-terminated sentinel slice). They are the primary mechanism for working with text.

```lain
var greeting = "Hello, Lain!"       // Type: u8[:0]
var msg u8[:0] = "Explicit type"    // Explicit annotation
```

Strings can be passed to functions expecting `u8[:0]` or to C functions via `.data`:

```lain
func greet(msg u8[:0]) {
    libc_printf("Message: %s\n", msg.data)
}
```

### 3.6 Struct Types

Structs are defined using the `type` keyword. All variants with named fields and no sub-variants are plain structs.

**Definition:**
```lain
type Point {
    x int
    y int
}
```

**Construction** (two forms):

*Positional construction:*
```lain
var p = Point(10, 20)     // Fields assigned by position
```

*Field-by-field assignment:*
```lain
var p Point
p.x = 10
p.y = 20
```

**Field access:**
```lain
var x = p.x
var y = p.y
```

**Copy semantics:**
By default, assigning a struct creates a copy (for non-linear structs):
```lain
var p2 = p          // Copy of p
p2.x = 30           // Does not modify p
```

**Linear fields:**
Struct fields can be annotated with `mov` to indicate ownership:

```lain
type File {
    mov handle *FILE     // Owned handle — makes the struct linear
}
```

A struct containing any `mov` field becomes **linear**: it cannot be copied, only moved (see §5).

### 3.7 Algebraic Data Types (ADTs)

Lain uses a unified syntax for enums, tagged unions, and algebraic data types. All are defined with `type`.

**Simple enum (no data):**
```lain
type Color {
    Red,
    Green,
    Blue
}
```

**ADT with variant data:**
```lain
type Shape {
    Circle { radius int }
    Rectangle { width int, height int }
    Point                               // Unit variant (no data)
}
```

**Construction:**
```lain
var c = Color.Red
var shape = Shape.Circle(10)
var rect = Shape.Rectangle(5, 8)
var p = Shape.Point
```

**Pattern matching:**
See §7.5 for `case` expressions.

### 3.8 Opaque Types (`extern type`)

Opaque types declare a type whose size and layout are unknown to Lain. They can only be used behind pointers.

```lain
extern type FILE      // C's FILE struct
```

**Rules:**
- Can only be used as `*FILE`, never by value.
- Instantiating by value (`var f FILE`) is a compile error.
- Emitted as `typedef struct FILE FILE;` in the generated C code.

This enables safe wrapping of C library handles:

```lain
extern type FILE
extern func fopen(filename *u8, mode *u8) mov *FILE
extern func fclose(stream mov *FILE) int
```

---

## 4. Variables & Mutability

Lain enforces a clear distinction between immutable and mutable bindings.

### 4.1 Immutable Bindings (Default)

Variables declared by simple assignment are **immutable**. They must be initialized and cannot be reassigned.

```lain
x = 10           // Immutable binding
// x = 20        // ERROR: Cannot assign to immutable variable
```

### 4.2 Mutable Bindings (`var`)

The `var` keyword creates a mutable binding slot.

```lain
var y = 10       // Mutable binding with initialization
y = 20           // OK: y is mutable
```

### 4.3 Type Annotations

Types can be optionally specified on any declaration:

```lain
var z int = 30            // Explicit type
var arr u8[5]             // Array with explicit type (no initializer)
name = "Lain"             // Inferred as u8[:0]
```

### 4.4 Global Variables

Variables can be declared at module (file) scope:

```lain
var global_counter int    // Mutable global variable
```

> [!WARNING]
> Global mutable state is restricted in Lain's purity model. Pure functions (`func`) **cannot** read or write global variables. Only procedures (`proc`) may access global mutable state.

### 4.5 Binding vs. Assignment Disambiguation

Since `x = 10` can look like both a "new immutable variable" and an "assignment to an existing mutable variable", the compiler uses the following rule:

- If `x` is **not** already in scope → `x = 10` creates a **new immutable binding**.
- If `x` **is** already in scope and was declared `var` → `x = 10` is an **assignment**.
- If `x` **is** already in scope and is immutable → `x = 10` is a **compile error** (cannot assign to immutable).

```lain
x = 10           // New immutable binding (x not in scope before)
var y = 20       // New mutable binding
y = 30           // Assignment (y was declared var)
// x = 40        // ERROR: x is immutable
```

> [!NOTE]
> The `var` keyword is never ambiguous: `var x = ...` is always a new mutable variable declaration.

## 5. Ownership & Borrowing

Lain guarantees memory safety without garbage collection through a strict **Ownership & Borrowing** system based on linear logic.

### 5.1 Ownership Modes

Every parameter, variable and field has one of three ownership modes:

| Mode | Syntax | Semantics | C Emission |
|:-----|:-------|:----------|:-----------|
| **Shared** | `p T` | Immutable borrow. Multiple shared references can coexist. | `const T*` |
| **Mutable** | `var p T` | Exclusive read-write borrow. Only one at a time. | `T*` |
| **Owned** | `mov p T` | Ownership transfer. Value must be consumed exactly once. | `T` (by value) |

### 5.2 Move Semantics (`mov`)

The `mov` operator transfers ownership of a value. After a move, the source variable is **invalidated**.

**Variable-to-variable move:**
```lain
var a Resource
a.id = 1
var b = mov a       // a is moved into b
// a.id             // ERROR: Use after move
```

**Move at function call site:**
```lain
take_ownership(mov resource)
// resource is now invalid
```

**Returning ownership:**
```lain
func wrap(mov r Resource) Container {
    return Container(mov r)    // Transfer ownership into return value
}
```

### 5.3 Borrowing Rules (Read-Write Lock)

Lain enforces a "Read-Write Lock" model at compile time:

1. **Multiple shared borrows** are allowed simultaneously.
2. **Exactly one mutable borrow** is allowed at a time.
3. **Shared and mutable borrows cannot coexist** for the same variable.

```lain
var data Data
data.value = 42

// OK: Multiple shared borrows
x = read(data)
y = read(data)

// OK: Mutable borrow (no shared borrows active)
mutate(var data)

// OK: Shared borrow after mutable borrow ends
z = read(data)
```

**Conflict example (compile error):**
```lain
// ERROR: Same variable borrowed as shared AND mutable in the same call
modify_both(data, var data)
```

### 5.4 Linear Fields in Structs

Struct fields annotated with `mov` make the containing struct **linear** (move-only):

```lain
type Handle {
    mov ptr *int
}

type Wrapper {
    h Handle            // Wrapper is also linear (transitively)
}

var h1 = Handle(ptr)
var h2 = h1             // Moves h1 → h2
// var h3 = h1          // ERROR: h1 was already moved
```

### 5.5 Caller-Site Annotations

When calling a function, the caller must explicitly annotate `var` and `mov` at the call site to signal the intended ownership transfer:

```lain
func read_data(d Data) int { return d.value }            // Shared borrow
func modify_data(var d Data) { d.value = d.value + 1 }   // Mutable borrow
func consume_data(mov d Data) { /* ... */ }               // Ownership transfer

// Call sites:
read_data(data)           // Implicit shared borrow
modify_data(var data)     // Explicit mutable borrow
consume_data(mov data)    // Explicit ownership transfer
```

### 5.6 Destructuring in Parameters

Parameters can be destructured at the function signature level:

```lain
func drop(mov {id} Resource) {
    // 'id' is extracted from Resource, consuming the struct
}
```

This consumes the linear resource by destructuring it into its component fields.

### 5.7 Return Ownership

The `return` statement supports ownership annotations to control how values are returned:

**`return mov` — Transfer ownership:**
```lain
func transfer(mov item Item) Item {
    return mov item       // Transfer ownership to the caller
}
```

**`return var` — Return a mutable reference:**
```lain
func get_ref() var int {
    var x = 10
    return var x          // Return a mutable reference
}
```

**`return` (default) — Return by value (copy):**
```lain
func compute(a int, b int) int {
    return a + b          // Return a copy
}
```

The return type can also be annotated with `mov` or `var` to signal the ownership mode of the return value:
```lain
func open_file(path u8[:0], mode u8[:0]) mov File {
    // Returns an owned File
}
```

---

## 6. Functions & Procedures

Lain enforces a strict boundary between pure computation and side effects.

### 6.1 Pure Functions (`func`)

Functions declared with `func` are **pure, deterministic, and guaranteed to terminate**.

**Restrictions:**
- Cannot modify global state.
- Cannot call procedures (`proc`).
- Cannot recurse (direct recursion is a compile error).
- Can only use `for` loops (over finite ranges). `while` loops are banned.

```lain
func add(a int, b int) int {
    return a + b         // Pure & Total
}
```

**Termination guarantee:**
```lain
// ERROR: Recursion not allowed in pure function
func factorial(n int) int {
    if n <= 1 { return 1 }
    return n * factorial(n - 1)    // Compile error
}
```

### 6.2 Procedures (`proc`)

Procedures declared with `proc` can have side effects, perform I/O, modify global state, and recurse.

```lain
proc log(msg u8[:0]) {
    printf("%s\n", msg)        // Side effect: I/O
}

proc fib(n int) int {
    if n < 2 { return n }
    return fib(n-1) + fib(n-2) // Recursion: OK in proc
}
```

### 6.3 Parameter Modes

Both `func` and `proc` support three parameter modes:

```lain
func process(
    id int,             // Shared borrow (read-only)
    var ctx Context,    // Mutable borrow (read-write)
    mov res Resource    // Owned (consumed by function)
) { /* ... */ }
```

See §5.1 for full semantics.

### 6.4 Return Types & Void Functions

Functions and procedures can return a value or be void:

```lain
func add(a int, b int) int {    // Returns int
    return a + b
}

proc greet(msg u8[:0]) {        // Void (no return type) — must be proc because of I/O
    libc_printf("%s\n", msg.data)
}

proc main() int {               // main must always be 'proc'
    return 0
}
```

> [!IMPORTANT]
> The `main` function **must** be declared as `proc main()`. Since `main` is the program entry point and typically performs I/O, it cannot be a pure `func`. Declaring `main` as `func` is a compile error.

### 6.5 Termination Guarantees

| Feature | `func` | `proc` |
|:--------|:-------|:-------|
| Pure (no side effects) | ✅ Required | ❌ Not required |
| `for` loops | ✅ Allowed | ✅ Allowed |
| `while` loops | ❌ Banned | ✅ Allowed |
| Recursion | ❌ Banned | ✅ Allowed |
| Global state access | ❌ Banned | ✅ Allowed |
| Calling `proc` | ❌ Banned | ✅ Allowed |
| Calling `func` | ✅ Allowed | ✅ Allowed |
| Calling `extern func` | ✅ Allowed | ✅ Allowed |
| Calling `extern proc` | ❌ Banned | ✅ Allowed |

> [!NOTE]
> **`extern func` vs `extern proc`**: External C functions declared as `extern func` are trusted to be pure (no side effects). External C functions declared as `extern proc` may have side effects. A pure `func` can call `extern func` but **not** `extern proc`.
>  
> Example: `extern func abs(n int) int` — pure, callable from `func`.  
> Example: `extern proc printf(fmt *u8, ...) int` — impure, callable only from `proc`.

---

## 7. Control Flow

### 7.1 If / Elif / Else

```lain
if x > 10 {
    // ...
} elif x > 5 {
    // ...
} else {
    // ...
}
```

Conditions do not require parentheses. The body must be enclosed in `{ }`.

The condition contributes to **range analysis**: inside the `if x < y` branch, the compiler knows `x < y` and can propagate this constraint (see §9).

### 7.2 For Loops (Range-Based)

For loops iterate over finite ranges. They are the only loop construct allowed in `func`.

**Single variable form:**
```lain
for i in 0..n {
    // i ranges from 0 to n-1 (exclusive end)
}
```

**Two variable form:**
```lain
for i, val in 0..10 {
    // i = index, val = value (same as i for integer ranges)
}
```

**Range semantics:**
- `0..n` — half-open range: `[0, n)` (0 inclusive, n exclusive)

> [!NOTE]
> The loop variable `i` is statically known to be in `[0, n-1]`, enabling safe array indexing without runtime checks.

### 7.3 While Loops

While loops run until the condition becomes false. They are **only allowed in `proc`** (not in `func`).

```lain
proc count_up() {
    var i = 0
    while i < 10 {
        libc_printf("%d ", i)
        i = i + 1
    }
}
```

**Infinite loop pattern:**
```lain
while 1 {
    // Use 'break' to exit
    if done { break }
}
```

### 7.4 Break & Continue

`break` exits the innermost loop. `continue` skips to the next iteration.

```lain
proc example() {
    var i = 0
    while i < 10 {
        i = i + 1
        if i == 5 { continue }    // Skip 5
        if i == 8 { break }       // Stop at 8
        libc_printf("%d ", i)
    }
    // Output: 1 2 3 4 6 7
}
```

### 7.5 Case (Pattern Matching)

`case` is used for pattern matching on enums, ADTs, and integer values.

**Matching on an enum:**
```lain
case color {
    Red:   libc_printf("Red\n")
    Green: libc_printf("Green\n")
    Blue:  libc_printf("Blue\n")
}
```

**Matching on an ADT with destructuring:**
```lain
case shape {
    Circle(r):        libc_printf("Circle radius: %d\n", r)
    Rectangle(w, h):  libc_printf("Rect: %d x %d\n", w, h)
    Point:            libc_printf("Just a point\n")
}
```

**Matching on integers** (requires `else` for exhaustiveness):
```lain
case x {
    1: return 1
    2: return 2
    3: return 3
    else: return 0    // Required: integers are not exhaustive
}
```

**Case arms** can be:
- A single expression: `Red: return 1`
- A block: `Red: { libc_printf("red\n"); return 1 }`

### 7.6 Exhaustiveness Checking

`case` statements must be **exhaustive**. The compiler verifies:

1. **Enums**: All variants must be covered, OR an `else` arm must be present.
2. **ADTs**: All variants must be covered, OR an `else` arm must be present.
3. **Integers**: An `else` arm is always required (integers are not finite).

```lain
// OK: All 3 variants covered, no 'else' needed
type Status { Ready, Running, Done }
case s {
    Ready:   /* ... */
    Running: /* ... */
    Done:    /* ... */
}

// ERROR: Missing 'Done' variant and no 'else'
case s {
    Ready:   /* ... */
    Running: /* ... */
    // Compile error: non-exhaustive case
}
```

---

## 8. Expressions & Operators

### 8.1 Arithmetic

```lain
a + b      // Addition
a - b      // Subtraction
a * b      // Multiplication
a / b      // Integer division
a % b      // Modulo (remainder)
```

### 8.2 Comparison

```lain
a == b     // Equal
a != b     // Not equal
a < b      // Less than
a > b      // Greater than
a <= b     // Less than or equal
a >= b     // Greater than or equal
```

### 8.3 Logical Operators

Lain uses keyword-based logical operators:

```lain
x > 0 and x < 100    // Logical AND
x == 0 or x == 1     // Logical OR
!condition            // Logical NOT
```

### 8.4 Bitwise Operators

```lain
a & b      // Bitwise AND
a | b      // Bitwise OR
a ^ b      // Bitwise XOR
~a         // Bitwise NOT (complement)
```

### 8.5 Compound Assignment

All compound assignment operators are syntactic sugar:

```lain
x += 5     // Equivalent to: x = x + 5
x -= 3     // Equivalent to: x = x - 3
x *= 2     // Equivalent to: x = x * 2
x /= 4     // Equivalent to: x = x / 4
x %= 3     // Equivalent to: x = x % 3
x &= mask  // Equivalent to: x = x & mask
x |= flag  // Equivalent to: x = x | flag
x ^= bits  // Equivalent to: x = x ^ bits
```

### 8.6 Type Cast (`as`)

The `as` operator performs explicit type conversions between numeric types:

```lain
var x i32 = 1000
var y = x as u8           // Truncate i32 to u8 (wrapping)
var big = 42 as i64       // Widen int to i64
var small = 300 as u8     // Truncate to u8 (300 → 44)
```

**Rules:**
- Conversions between integer types are always allowed (truncation may occur).
- Pointer casts (`*int as *void`) require an `unsafe` block.
- Non-numeric casts (e.g., struct to int) are not allowed.

### 8.7 Operator Precedence

Operators are evaluated according to the following precedence table (highest to lowest):

| Precedence | Operators | Associativity | Description |
|:-----------|:----------|:-------------|:------------|
| 1 (highest) | `()` `[]` `.` | Left | Grouping, indexing, field access |
| 2 | `!` `~` `-` `*` | Right | Unary NOT, complement, negation, deref |
| 3 | `as` | Left | Type cast |
| 4 | `*` `/` `%` | Left | Multiplicative |
| 5 | `+` `-` | Left | Additive |
| 6 | `&` | Left | Bitwise AND |
| 7 | `^` | Left | Bitwise XOR |
| 8 | `\|` | Left | Bitwise OR |
| 9 | `<` `>` `<=` `>=` | Left | Comparison |
| 10 | `==` `!=` | Left | Equality |
| 11 | `and` | Left | Logical AND |
| 12 (lowest) | `or` | Left | Logical OR |

> [!NOTE]
> Use parentheses to override precedence when intent is unclear: `(a + b) * c`.

---

## 9. Type Constraints

Lain uses **equation-style type constraints** to statically verify function requirements and guarantees. All checks are performed at compile time with **zero runtime overhead**.

### 9.1 Parameter Constraints

Constraints on parameters are written directly after the type:

```lain
func safe_div(a int, b int != 0) int {
    return a / b
}
```

The compiler verifies at each call site that `b` cannot be zero:
```lain
safe_div(10, 2)     // OK: 2 != 0
safe_div(10, 0)     // ERROR: 0 violates b != 0
```

**Supported constraint operators:**
| Constraint | Meaning |
|:-----------|:--------|
| `b int != 0` | b must not be zero |
| `b int > 0` | b must be positive |
| `b int >= 0` | b must be non-negative |
| `b int < a` | b must be less than a |
| `b int > a` | b must be greater than a |
| `b int == 1` | b must equal 1 |

### 9.2 Return Type Constraints

Constraints on the return value are written after the return type:

```lain
func abs(x int) int >= 0 {
    if x < 0 { return 0 - x }
    return x
}
```

The compiler verifies that **all** return paths satisfy `result >= 0`:
```lain
// ERROR: -1 does not satisfy >= 0
func bad_abs(x int) int >= 0 {
    return -1
}
```

### 9.3 Index Bounds (`in` keyword)

The `in` keyword declares that a value is a valid index into an array:

```lain
func get(arr int[10], i int in arr) int {
    return arr[i]      // Always safe — compiler knows i ∈ [0, 9]
}
```

This desugars to the constraint: `i >= 0 and i < arr.len`.

The caller must prove the constraint:
```lain
get(arr, 5)      // OK: 5 ∈ [0, 10)
get(arr, 15)     // ERROR: 15 is not in [0, 10)
```

### 9.4 Relational Constraints Between Parameters

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

### 9.5 Multiple Constraints

Chain constraints with `and`:

```lain
func bounded(x int >= 0 and <= 100) int {
    return x
}
```

### 9.6 Static Verification Mechanism

The compiler uses **Value Range Analysis (VRA)** — a decidable, polynomial-time static analysis:

1. **Interval tracking**: Every integer variable carries a range `[min, max]`.
   - Literals: `10` → `[10, 10]`
   - Types: `u8` → `[0, 255]`, `int` → `[-2³¹, 2³¹-1]`

2. **Arithmetic propagation**:
   - `[a, b] + [c, d]` → `[a+c, b+d]`
   - `[a, b] - [c, d]` → `[a-d, b-c]`

3. **Control flow refinement**:
   - Inside `if x < 10`: `x` is narrowed to `[min, 9]`
   - Inside `else`: `x` is narrowed to `[10, max]`
   - After merge: conservative hull (union of both branches)

4. **Assignment tracking**: Ranges are updated through assignments:
   ```lain
   var x = 10      // x: [10, 10]
   x = 0           // x: [0, 0]
   require_pos(x)  // ERROR: [0, 0] does not satisfy > 0
   ```

5. **Linear constraint propagation**:
   ```lain
   var x = y + 1   // x = y + 1 ⟹ x > y
   require_gt(x, y) // OK: compiler knows x > y
   ```

**No SMT Solver**: All verification is polynomial-time — fast and predictable.

### 9.7 Loop Widening

Variables modified within loops are **conservatively widened** to their type's full range:

```lain
func main() int {
    var x = 0
    for i in 0..10 {
        x = x + 1
    }
    // x is widened to [INT_MIN, INT_MAX] after the loop
    // Cannot prove x == 10 statically
    return require_one(x)  // ERROR: Cannot verify x == 1
}
```

> [!WARNING]
> This is a known limitation of the current analysis. Loop variables lose precision, making some post-loop constraints unverifiable. Future versions may support loop invariant annotations.

---

## 10. Module System

### 10.1 Import

The `import` keyword loads another Lain module. Module paths use dot-notation corresponding to the filesystem hierarchy:

```lain
import std.c            // Loads std/c.ln
import std.io           // Loads std/io.ln
import std.fs           // Loads std/fs.ln
import tests.stdlib.dummy   // Loads tests/stdlib/dummy.ln
```

All public declarations from the imported module become available in the current scope.

### 10.2 Standard Library

Lain ships with a minimal standard library:

**`std/c.ln`** — Core C bindings:
```lain
c_include "<stdio.h>"
c_include "<stdlib.h>"

extern type FILE

extern func printf(fmt *u8, ...) int
extern func fopen(filename *u8, mode *u8) mov *FILE
extern func fclose(stream mov *FILE) int
extern func fputs(s *u8, stream *FILE) int
extern func fgets(s var *u8, n int, stream *FILE) var *u8
extern func libc_printf(fmt *u8, ...) int
```

**`std/io.ln`** — Basic I/O:
```lain
import std.c

func print(s *u8) {
    libc_printf("%s", s)
}

func println(s *u8) {
    libc_puts(s)
}
```

**`std/fs.ln`** — File system with ownership:
```lain
import std.c

type File {
    mov handle *FILE       // Owned file handle
}

func open_file(path u8[:0], mode u8[:0]) mov File {
    var raw = fopen(path.data, mode.data)
    return File(raw)
}

proc close_file(mov f File) {
    if f.handle != 0 {
        fclose(f.handle)
    }
}

proc write_file(var f File, s u8[:0]) {
    fputs(s.data, f.handle)
}
```

> [!NOTE]
> The `File` type is a safe wrapper around C's `FILE*`. Because `handle` is declared `mov`, the `File` struct is **linear**: it must be explicitly consumed via `close_file(mov f)`. Forgetting to close a file is a compile error.

---

## 11. C Interoperability

Lain compiles to C99 and provides first-class mechanisms for interfacing with C code.

### 11.1 `c_include` Directive

Directly includes a C header file in the generated output:

```lain
c_include "<stdio.h>"
c_include "<stdlib.h>"
c_include "my_header.h"
```

This translates to `#include <stdio.h>` / `#include "my_header.h"` in the generated C code.

### 11.2 Extern Functions

The `extern` keyword declares functions defined in C:

```lain
extern func abs(n int) int
extern func malloc(size usize) mov *void
extern func free(ptr mov *void)
extern func exit(status int)
```

**Key features:**
- Ownership annotations (`mov`) can be applied to extern parameters and return types.
- `malloc` returns `mov *void` (the caller owns the allocation).
- `free` takes `mov *void` (it consumes the pointer).

### 11.3 Extern Types (Opaque)

See §3.8. Opaque types allow wrapping C handles safely:

```lain
extern type FILE
extern func fopen(filename *u8, mode *u8) mov *FILE
```

### 11.4 Variadic Parameters

C-style variadic parameters are supported in extern declarations:

```lain
extern func printf(fmt *u8, ...) int
```

The `...` is only valid in `extern` function declarations.

### 11.5 Type Mapping

The compiler maps Lain types to C types as follows:

| Lain Type | C Type | Context |
|:----------|:-------|:--------|
| `int` | `int` | — |
| `u8` | `unsigned char` | — |
| `usize` | `size_t` | — |
| `*u8` (shared) | `const char *` | In extern parameters |
| `var *u8` (mutable) | `char *` | In extern parameters |
| `*T` (shared) | `const T*` | Default for shared references |
| `var *T` | `T*` | Mutable references |
| `mov *T` | `T*` | Owned pointer |

> [!WARNING]
> Currently, certain C functions (`fopen`, `fputs`, `fclose`, etc.) have hardcoded type mapping overrides in the emitter to handle `FILE*` correctly. A more general annotation system is planned for the future.

---

## 12. Unsafe Code

While Lain prioritizes safety, low-level systems programming sometimes requires bypassing safety checks.

### 12.1 Unsafe Blocks

Operations that bypass safety checks must be enclosed in an `unsafe` block:

```lain
unsafe {
    var val = *ptr      // Pointer dereference: OK inside unsafe
}

// var val = *ptr       // ERROR: Dereference outside unsafe block
```

### 12.2 Raw Pointers

Raw pointers (`*int`, `*void`) bypass Lain's ownership system. **Dereferencing** a raw pointer is only allowed inside `unsafe` blocks.

```lain
func main() {
    var p *int = 0

    // var x = *p       // ERROR: Dereference outside unsafe

    unsafe {
        var y = *p      // OK (compiles, though dangerous at runtime)
    }
}
```

### 12.3 Nesting Rules

Unsafe blocks can be nested and combined with control flow:

```lain
unsafe {
    unsafe {
        var val = *p    // OK: nested unsafe
    }
    var val2 = *p       // OK: still inside outer unsafe
}

if condition {
    unsafe {
        var val3 = *p   // OK: unsafe inside control flow
    }
}

unsafe {
    if condition {
        var val4 = *p   // OK: control flow inside unsafe
    }
}
```

> [!IMPORTANT]
> The compiler does **not** enforce memory safety or linear types for raw pointers inside unsafe blocks. The programmer assumes full responsibility for pointer validity, aliasing, and lifetime correctness.

---

## 13. Safety Guarantees

Lain eliminates entire classes of bugs at compile time without runtime overhead.

| Safety Concern | Lain's Guarantee | Mechanism |
|:---------------|:-----------------|:----------|
| **Buffer Overflows** | **Impossible** | **Static Range Analysis** (§9) verifies every array access at compile time. No runtime bounds checks. |
| **Use-After-Free** | **Impossible** | **Linear Types** (`mov`) ensure resources are consumed exactly once. Accessing a moved variable is a compile error. |
| **Double Free** | **Impossible** | Ownership is linear — a resource must be consumed exactly once, preventing double destruction. |
| **Data Races** | **Impossible** | The **Borrow Checker** enforces exclusive mutability. Simultaneous shared + mutable borrows are rejected. |
| **Null Dereference** | **Prevented** | References are valid by construction. Raw pointer dereference requires `unsafe`. |
| **Memory Leaks** | **Prevented** | Linear variables (`mov`) **must** be consumed. Forgetting to use or destroy a resource is a compile error. |
| **Division by Zero** | **Prevented** | Type constraints (`b int != 0`) can enforce non-zero divisors at compile time. |
| **Integer Overflow** | *Not yet addressed* | Future versions may add overflow detection. |

---

## 14. Compilation Pipeline

### 14.1 Lain → C Code Generation

Lain compiles to C99 code, which is then compiled with a C compiler.

```
 .ln source → [Lain Compiler] → out.c → [C Compiler] → executable
```

### 14.2 Building the Compiler

The Lain compiler is itself written in C99:

```bash
gcc src/main.c -o src/compiler.exe -std=c99 -Wall -Wextra \
    -Wno-unused-function -Wno-unused-parameter
```

### 14.3 Compiling Programs

```bash
# Step 1: Lain → C
./compiler.exe my_program.ln

# Step 2: C → Executable
gcc out.c -o my_program
# or with cosmocc for portable binaries:
./cosmocc/bin/cosmocc out.c -o my_program.exe -w

# Step 3: Run
./my_program
```

### 14.4 Test Framework

Tests are organized under `tests/` and run via `run_tests.sh`:

- **Positive tests** (`*.ln`): Must compile and run successfully.
- **Negative tests** (`*_fail.ln`): Must **fail** compilation (testing error detection).

```bash
# Run all tests
./run_tests.sh

# Run a single test
./run_tests.sh tests/core/functions.ln

# Negative tests are auto-detected by the _fail suffix
./run_tests.sh tests/safety/bounds/bounds_fail.ln
```

**Test categories:**
| Directory | Tests | Purpose |
|:----------|:------|:--------|
| `tests/core/` | 8 | Basic language features (functions, loops, math) |
| `tests/types/` | 10 | Type system (ADTs, enums, arrays, structs, strings, bool, casts, integers) |
| `tests/safety/bounds/` | 14 | Static bounds checking & type constraints |
| `tests/safety/ownership/` | 11 | Ownership, borrowing, move semantics |
| `tests/safety/purity/` | 2 | Purity enforcement |
| `tests/safety/` (root) | 4 | Unsafe blocks, linear struct fields |
| `tests/stdlib/` | 6 | Module system, extern, stdlib |

### 14.5 C Compilation Flags

When compiling the generated `out.c`, certain C preprocessor flags are needed to bridge naming differences between Lain's standard library and the underlying C library:

```bash
# Standard compilation with cosmocc:
./cosmocc/bin/cosmocc out.c -o program.exe -w \
    -Dlibc_printf=printf \
    -Dlibc_puts=puts
```

**Required flags:**
| Flag | Purpose |
|:-----|:--------|
| `-Dlibc_printf=printf` | Maps Lain's `libc_printf` to C's `printf` |
| `-Dlibc_puts=puts` | Maps Lain's `libc_puts` to C's `puts` |
| `-w` | Suppress C compiler warnings from generated code |

> [!IMPORTANT]
> The `libc_` prefix convention exists to avoid name collisions between Lain's extern declarations and C's standard library during compilation. The `-D` flags perform the final mapping. Without these flags, the linker will report undefined symbol errors.

---

## Appendix A: Complete Keyword List

| Keyword | Status | First Defined |
|:--------|:-------|:--------------|
| `and` | ✅ Implemented | Logical AND operator |
| `as` | ✅ Implemented | Type cast operator (§8.6) |
| `break` | ✅ Implemented | Loop exit |
| `c_include` | ✅ Implemented | C header inclusion |
| `case` | ✅ Implemented | Pattern matching (§7.5) |
| `comptime` | 🔮 Reserved | Compile-time execution |
| `continue` | ✅ Implemented | Loop iteration skip |
| `elif` | ✅ Implemented | Else-if branch |
| `else` | ✅ Implemented | Default branch |
| `end` | 🔮 Reserved | — |
| `export` | 🔮 Reserved | Module export |
| `expr` | 🔮 Reserved | — |
| `extern` | ✅ Implemented | C interop declarations |
| `false` | ✅ Implemented | Boolean false literal |
| `for` | ✅ Implemented | Range-based loop |
| `func` | ✅ Implemented | Pure function |
| `fun` | ⚠️ Alias | Alias for `func` |
| `if` | ✅ Implemented | Conditional |
| `import` | ✅ Implemented | Module import |
| `in` | ✅ Implemented | Range iteration / index bounds |
| `macro` | 🔮 Reserved | — |
| `mov` | ✅ Implemented | Ownership transfer |
| `or` | ✅ Implemented | Logical OR operator |
| `post` | 🔮 Reserved | Postcondition |
| `pre` | 🔮 Reserved | Precondition |
| `proc` | ✅ Implemented | Procedure (side effects) |
| `return` | ✅ Implemented | Return value |
| `true` | ✅ Implemented | Boolean true literal |
| `type` | ✅ Implemented | Type definition |
| `unsafe` | ✅ Implemented | Unsafe block |
| `use` | 🔮 Reserved | — |
| `var` | ✅ Implemented | Mutable binding |
| `while` | ✅ Implemented | While loop (only in `proc`) |

---

## Appendix B: Reserved Keywords & Future Plans

### `comptime` — Compile-Time Function Execution

A planned alternative to traditional generics (`<T>`). Instead of template syntax, types would be first-class values manipulated at compile time:

```lain
// Future vision
func Option(comptime T type) type {
    return struct {
        has_value bool,
        payload   T
    }
}

type OptionInt = Option(int)
```

**Requirement**: The compiler would need a built-in interpreter (CTFE engine).

### `pre` / `post` — Contract Annotations

Reserved for explicit precondition and postcondition contract blocks. Currently, constraints are expressed inline on parameter and return types (§9).

### `..=` — Inclusive Range

Reserved for inclusive range syntax. Currently only `..` (exclusive end) is supported.

---

## Appendix C: Grammar Summary (Pseudo-BNF)

```
program         = { top_level_decl } ;

top_level_decl  = import_decl | c_include_decl | extern_decl
                | type_decl | var_decl | func_decl | proc_decl ;

import_decl     = "import" module_path ;
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

type_expr       = IDENT                    (* named type *)
                | "*" type_expr            (* pointer *)
                | type_expr "[" NUMBER "]" (* array *)
                | type_expr "[" "]"        (* slice *)
                | type_expr "[" ":" expr "]" (* sentinel slice *) ;

block           = "{" { statement } "}" ;

statement       = var_decl | assignment | return_stmt | if_stmt
                | for_stmt | while_stmt | case_stmt | break_stmt
                | continue_stmt | unsafe_block | expr_stmt ;

var_decl        = ["var"] IDENT [type_expr] "=" expr ;
assignment      = lvalue assign_op expr ;
assign_op       = "=" | "+=" | "-=" | "*=" | "/=" | "%="
                | "&=" | "|=" | "^=" ;

return_stmt     = "return" ["mov" | "var"] [expr] ;
break_stmt      = "break" ;
continue_stmt   = "continue" ;

if_stmt         = "if" expr block { "elif" expr block } [ "else" block ] ;
for_stmt        = "for" IDENT ["," IDENT] "in" expr ".." expr block ;
while_stmt      = "while" expr block ;           (* only valid inside proc *)

case_stmt       = "case" expr "{" { case_arm } "}" ;
case_arm        = pattern ":" (expr | block) ;
pattern         = IDENT [ "(" pattern_list ")" ]
                | "else" ;

unsafe_block    = "unsafe" block ;

expr            = literal | IDENT | expr binop expr | unop expr
                | expr "." IDENT | expr "[" expr "]"
                | expr "(" arg_list ")"
                | IDENT "." IDENT [ "(" arg_list ")" ]
                | "mov" expr | "(" expr ")" ;

binop           = "+" | "-" | "*" | "/" | "%" | "==" | "!="
                | "<" | ">" | "<=" | ">=" | "and" | "or"
                | "&" | "|" | "^" ;

unop            = "-" | "!" | "~" | "*" ;

literal         = NUMBER | CHAR_LITERAL | STRING_LITERAL ;
```

> [!WARNING]
> This grammar is a simplified approximation. The actual parser may accept or reject certain constructs not captured here. The grammar is intended as a reference, not a formal specification.
