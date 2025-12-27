# Lain Programming Language Guide

Lain is a **critical-safe** programming language designed for embedded systems. It focuses on memory safety, type safety, and determinism with **zero runtime overhead**.

---

## 1. Core Philosophy

- **Memory Safety**: Guaranteed via linear types and borrow checking (no Garbage Collector).
- **Purity**: Strict distinction between deterministic functions and side-effecting procedures.
- **Zero Overhead**: No runtime checks for bounds or ownership; all verification is static.
- **C Interop**: Compiles to portable C99.

---

## 2. Ownership & Type System

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
    mov res Resource
    res.data = 42
    var val = take(mov res) // ownership moved to 'take'
    // res.data = 10 // ERROR: use after move
    return 0
}
```

---

## 3. Functions vs. Procedures

Lain enforces a strict boundary between pure logic and side effects.

- **`func`**: Pure and deterministic. Cannot modify global state or call procedures.
- **`proc`**: Can have side effects, modify state, and perform I/O.

```lain
func add(a int, b int) int {
    return a + b // Pure
}

proc log(msg u8[:0]) {
    printf("%s\n", msg) // Side effect
}
```

---

## 4. Data Structures

### Structs
```lain
type Point {
    x int
    y int
}

var p = Point{ x: 10, y: 20 }
```

### Enums (ADTs)
Lain supports simple enums and is expanding towards full algebraic data types.
```lain
type Color {
    Red
    Green
    Blue
}

var c = Color.Red
```

### Arrays & Slices
- **Arrays**: Fixed-size, stored on stack or within structs.
- **Slices**: Dynamic views into arrays.

```lain
var arr int[5] = [1, 2, 3, 4, 5]
var s int[] = arr[1..3] // Slice of elements 1 and 2
```

---

## 5. Control Flow

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

## 6. Safety Features

### Static Bounds Checking
Lain verifies array accesses at compile time when the index is a constant expression.
```lain
var arr int[3]
var x = arr[0] // OK
var y = arr[5] // COMPILE ERROR: index out of bounds
```

### Linear Type Enforcement
Variables marked with `mov` or parameters of type `mov T` must be used exactly once. The compiler tracks the "consumed" state to prevent leaks or double-frees.

### Borrow Checking
The compiler ensures that:
1. You cannot move an object while it is borrowed.
2. You cannot have a mutable borrow while shared borrows exist.

---

## 7. C Interoperability

Use `extern` to link with C functions.

```lain
extern func printf(fmt u8[:0], ...) int

proc main() {
    printf("Hello, Lain!\n")
}
```

---

## 8. Compilation

Lain compiles to C code.
```bash
./compiler my_program.ln
gcc out.c -o my_program
./my_program
```
