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
| **Shared** | `p T` | Immutable borrow. Multiple shared references can exist. |
| **Mutable** | `var p T` | Mutable borrow. Only one exclusive mutable reference allowed. |
| **Owned** | `mov p T` | Ownership transfer. The value must be consumed exactly once. |

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

### 4.1 Parameter Modes
Functions and procedures support three parameter modes (see [Ownership](#3-ownership--type-system)):
- **Shared** (default): `p T`. Read-only access.
- **Mutable**: `var p T`. Exclusive read-write access.
- **Owned**: `mov p T`. Transfer of ownership (value is consumed).

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

## 7. Safety Guarantees

Lain eliminates entire classes of bugs at compile time without runtime overhead.

| Safety Concern | Lain's Guarantee | Mechanism |
| :--- | :--- | :--- |
| **Buffer Overflows** | **Impossible** | **Static Range Analysis** verifies every array access at compile time. No runtime checks are needed. |
| **Use-After-Free** | **Impossible** | **Linear Types** (`mov`) ensure resources are consumed exactly once. Accessing a moved variable is a compile error. |
| **Double Free** | **Impossible** | Since ownership is linear and must be consumed exactly once, resources cannot be destroyed twice. |
| **Data Races** | **Impossible** | The **Borrow Checker** enforces Exclusive Mutability. You cannot write to data while others are reading it. |
| **Null Dereference** | **Impossible** | Lain has no `null` value. References are valid by construction. |
| **Memory Leaks** | **Prevented** | Linear variables *must* be consumed. Forgetting to use or destroy a resource is a compile error. |

---

## 8. C Interoperability

Use `extern` to link with C functions.

```lain
extern func printf(fmt *char, ...) int

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

---

## 11. Unsafe Code

While Lain prioritizes safety, low-level systems programming sometimes requires bypassing these checks (e.g., interacting with hardware, C libraries, or manual memory management). Lain provides the `unsafe` keyword for this purpose.

### 11.1 Unsafe Blocks

Operations that are potentially unsafe must be enclosed in an `unsafe` block.

```lain
unsafe {
    // Potentially unsafe operations here
}
```

### 11.2 Raw Pointers

Lain supports raw pointers (e.g., `*int`, `*void`) primarily for C interoperability. 
**Dereferencing a raw pointer is only allowed inside an `unsafe` block.**

```lain
func main() {
    var p *int = 0
    
    // var x = *p // Error: Dereference of raw pointer outside 'unsafe' block
    
    unsafe {
        // var y = *p // OK (compiles, though unsafe at runtime if p is invalid)
    }
}
```

The compiler does not enforce memory safety or linear types for raw pointers; the programmer assumes full responsibility for their validity.
