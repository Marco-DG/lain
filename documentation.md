# Lain Programming Language Guide

Lain is a **critical-safe** programming language designed for embedded systems. It focuses on memory safety, type safety, and determinism with **zero runtime overhead**.

---

## 1. Core Philosophy

- **Memory Safety**: Guaranteed via linear types and borrow checking (no Garbage Collector).
- **Purity**: Strict distinction between deterministic functions and side-effecting procedures.
- **Zero Overhead**: No runtime checks for bounds or ownership; all verification is static.
- **C Interop**: Compiles to portable C99.

---

## 2. Variables & Mutability

Lain enforces a clear distinction between immutable and mutable variables.

### Immutable Declarations (Implicit)
By default, variables are immutable. You declare them by simply assigning a value.
```lain
x = 10         // 'x' is immutable
// x = 20      // ERROR: Cannot assign to immutable variable
```

### Mutable Declarations (Explicit)
To create a mutable variable, use the `var` keyword.
```lain
var y = 10     // 'y' is mutable
y = 20         // OK
```

### Type Annotations
You can optionally specify the type.
```lain
var z int = 30
```

---

## 3. Ownership & Type System

Lain uses an ownership model inspired by linear logic to manage memory safely.

### Ownership Modes

| Mode | Syntax | Description |
| :--- | :--- | :--- |
| **Shared** | `T` | Immutable borrow. Multiple shared references can exist. |
| **Mutable** | `mut T` | Mutable borrow. Only one exclusive mutable reference allowed. |
| **Owned** | `mov T` | Ownership transfer. The value must be consumed exactly once. |

### Example: Ownership Transfer
```lain
type Resource { data int }

func take(mov r Resource) int {
    return r.data // 'r' is consumed here
}

func main() int {
    // Mutable declaration
    var res Resource
    res.data = 42
    
    // Move ownership to 'take'
    var val = take(mov res) 
    
    // res.data = 10 // ERROR: use after move
    return 0
}
```

---

## 4. Functions vs. Procedures

Lain enforces a strict boundary between pure logic and side effects.

- **`func`**: Pure, deterministic, and **guaranteed to terminate**.
    - Cannot modify global state.
    - Cannot call procedures.
    - **Cannot recurse** (direct recursion is banned).
    - Can only use `for` loops (over finite ranges).
- **`proc`**: Can have side effects, modify state, perform I/O, and recurse.

```lain
func add(a int, b int) int {
    return a + b // Pure & Total
}

proc log(msg u8[:0]) {
    printf("%s\n", msg) // Side effect
}
```

---

## 5. Data Structures

### Structs
```lain
type Point {
    x int
    y int
}

func main() int {
    var p Point
    p.x = 10
    p.y = 20
    return 0
}
```

### Algebraic Data Types (ADTs)
Lain supports full Algebraic Data Types, allowing you to define types that can be one of several variants, each potentially holding different data.

#### Definition
Use the `type` keyword. Variants can be simple (like enums) or hold fields (like structs).

```lain
type Shape {
    Circle {
        rad int
    }
    Rectangle {
        w int
        h int
    }
    Point // Simple variant (unit)
}
```

#### Construction
Construct variants using the `Type.Variant` syntax.

```lain
var c = Shape.Circle(10)
var r = Shape.Rectangle(5, 8)
var p = Shape.Point
```

#### Pattern Matching
Use `match` to handle different variants and destructure their data.

```lain
match s {
    Circle(r): {
        printf("Circle radius: %d\n", r)
    }
    Rectangle(w, h): {
        printf("Rect: %d x %d\n", w, h)
    }
    Point: {
        printf("Just a point\n")
    }
}
```
Note: `match` must be exhaustive. You must cover all variants or provide an `else` case.

### Arrays & Slices
- **Arrays**: Fixed-size, stored on stack or within structs.
- **Slices**: Dynamic views into arrays.

```lain
var arr int[5]
arr[0] = 1
arr[1] = 2

var s int[] = arr[0..2] // Slice of elements 0 and 1
```

---

## 6. Control Flow

### If / Else
```lain
if x > 10 {
    // ...
} else {
    // ...
}
```

### For Loops
Supports range-based iteration.
```lain
for i, val in 0..10 {
    printf("%d: %d\n", i, val)
}
```

### Match (Pattern Matching)
Match statements must be **exhaustive**.
```lain
match color {
    Red:   return 1
    Green: return 2
    Blue:  return 3
    else:  return 0 // Required if not all cases covered
}
```

---

## 7. Safety Features

### Static Bounds Checking (Zero Overhead)
Lain verifies array accesses at compile time using **Static Range Analysis**. It tracks the possible range of values for every integer variable to ensure indices are always within bounds. If the compiler cannot prove safety, it rejects the code. There are **no runtime checks**.

```lain
var arr int[3]
for i in 0..3 {
    var x = arr[i] // OK: i is known to be in [0, 2]
}
// var y = arr[5] // COMPILE ERROR: index out of bounds
```

### Linear Type Enforcement
Variables marked with `mov` or parameters of type `mov T` must be used exactly once. The compiler tracks the "consumed" state to prevent leaks or double-frees.

### Borrow Checking (with NLL-lite)
Lain implements a strict borrow checker with **Non-Lexical Lifetimes (NLL-lite)** for temporary borrows.
1. **Lexical Scopes**: Explicit borrows last until the end of their scope.
2. **Temporary Borrows**: Implicit borrows (e.g., function arguments) expire immediately after the statement, allowing more flexible patterns without sacrificing safety.
3. **Aliasing Rules**: You cannot have a mutable borrow while other borrows (shared or mutable) exist.
4. **No Use-After-Move**: You cannot move an object while it is borrowed.

---

## 8. C Interoperability

Use `extern` to link with C functions.

```lain
extern func printf(fmt u8[:0], ...) int

proc main() {
    printf("Hello, Lain!\n")
}
```

---

## 9. Compilation

Lain compiles to C code.
```bash
./compiler my_program.ln
gcc out.c -o my_program
./my_program
```

---

## 10. Type Constraints

Lain uses **equation-style type constraints** to statically verify function requirements and guarantees. All checks are performed at compile time with **zero runtime overhead**.

### 10.1 Parameter Constraints

Constraints on parameters are written directly after the type:
```lain
func safe_div(a int, b int != 0) int {
    return a / b
}
```
The compiler verifies at each call site that `b` cannot be zero.

### 10.2 Return Type Constraints

Constraints on the return value are written after the return type:
```lain
func abs(x int) int >= 0 {
    if x < 0 { return 0 - x }
    return x
}
```
The compiler verifies that all return statements satisfy `result >= 0`.

### 10.3 Index Bounds (`in` keyword)

The `in` keyword declares that a value is a valid index into an array:
```lain
func get(arr int[10], i int in arr) int {
    return arr[i]  // Always safe!
}
```
This desugars to `i >= 0 and i < arr.len` and is verified at call sites.

### 10.4 Multiple Constraints

Chain constraints with `and`:
```lain
func clamp(x int, lo int, hi int >= lo) int >= lo and <= hi {
    if x < lo { return lo }
    if x > hi { return hi }
    return x
}
```

### 10.5 Static Verification

The compiler uses decidable static analysis (Range Analysis + Difference Bound Matrix) to verify constraints:
- **Linear Comparisons**: `x > 0`, `x >= y`, `x != 0`
- **Control Flow**: Constraints are propagated through `if`/`else` branches
- **No SMT Solver**: All verification is polynomial-time, fast and predictable

**Note on Loops**: Variables modified within loops are conservatively widened to unknown. For complex loop contracts, explicit verification may be needed in future versions.

