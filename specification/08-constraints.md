# Chapter 8 — Type Constraints & Static Verification

## 8.1 Overview

Lain uses **equation-style type constraints** to statically verify program
properties at compile time. All verification is performed using **Value Range
Analysis (VRA)** — a decidable, polynomial-time static analysis. No SMT solver
is required.

Constraints provide zero-runtime-overhead guarantees: if the program compiles,
the constraints are satisfied.

## 8.2 Parameter Constraints [Implemented]

Constraints on parameters are written directly after the type:

```lain
func safe_div(a int, b int != 0) int {
    return a / b
}

safe_div(10, 2)       // OK: 2 != 0
safe_div(10, 0)       // ERROR: 0 violates b != 0
```

### 8.2.1 Supported Constraint Operators

| Syntax | Meaning |
|:-------|:--------|
| `b int != 0` | b shall not be zero |
| `b int > 0` | b shall be positive |
| `b int >= 0` | b shall be non-negative |
| `b int < 100` | b shall be less than 100 |
| `b int <= 100` | b shall be at most 100 |
| `b int == 1` | b shall equal 1 |

### 8.2.2 Verification

At each call site, the compiler computes the value range of the argument
expression and checks it against the constraint. If the range cannot be
proven to satisfy the constraint, the call is rejected.

## 8.3 Return Type Constraints [Implemented]

Constraints on the return value are written after the return type:

```lain
func abs(x int) int >= 0 {
    if x < 0 { return 0 - x }
    return x
}
```

> **CONSTRAINT:** All return paths shall satisfy the declared return
> constraints. A return expression whose range does not satisfy the
> constraint produces a diagnostic.

```lain
// ERROR: -1 does not satisfy >= 0
func bad_abs(x int) int >= 0 {
    return -1
}
```

## 8.4 Index Bounds (`in` keyword) [Implemented]

The `in` keyword declares that a value is a valid index for an array:

```lain
func get(arr int[10], i int in arr) int {
    return arr[i]          // always safe — compiler knows i in [0, 9]
}
```

This desugars to the constraint: `i >= 0 and i < arr.len`.

```lain
get(arr, 5)               // OK: 5 in [0, 10)
get(arr, 15)              // ERROR: 15 is not in [0, 10)
```

## 8.5 Relational Constraints [Implemented]

Constraints can reference other parameters:

```lain
func require_lt(a int, b int > a) int {
    return b - a           // always positive
}

func clamp(x int, lo int, hi int >= lo) int >= lo and <= hi {
    if x < lo { return lo }
    if x > hi { return hi }
    return x
}
```

## 8.6 Multiple Constraints [Implemented]

Chain constraints with `and`:

```lain
func bounded(x int >= 0 and <= 100) int {
    return x
}
```

## 8.7 Value Range Analysis (VRA) [Implemented]

The compiler's static verification engine tracks integer value ranges
using interval arithmetic.

### 8.7.1 Interval Representation

Every integer expression carries a range `[min, max]`:

| Source | Range |
|:-------|:------|
| Literal `10` | `[10, 10]` |
| Type `u8` | `[0, 255]` |
| Type `int` | `[-2^31, 2^31-1]` |
| Type `i8` | `[-128, 127]` |

### 8.7.2 Arithmetic Propagation

| Operation | Result Range |
|:----------|:-------------|
| `[a, b] + [c, d]` | `[a+c, b+d]` |
| `[a, b] - [c, d]` | `[a-d, b-c]` |
| `[a, b] * [c, d]` | `[min(ac,ad,bc,bd), max(ac,ad,bc,bd)]` |

### 8.7.3 Control Flow Refinement

Inside conditional branches, the compiler narrows ranges:

```lain
if x < 10 {
    // x: [INT_MIN, 9]
} else {
    // x: [10, INT_MAX]
}
```

After a branch merge, the range is the conservative hull (union) of both
branches.

### 8.7.4 Assignment Tracking

Ranges are updated through assignments:

```lain
var x = 10                // x: [10, 10]
x = x + 1                // x: [11, 11]
if cond { x = 0 }        // x: [0, 11] (hull of both paths)
```

### 8.7.5 Linear Constraint Propagation

The compiler tracks linear relationships between variables:

```lain
var x = y + 1             // compiler knows: x > y
require_gt(x, y)          // OK: x = y + 1 implies x > y
```

## 8.8 Loop Widening [Implemented]

Variables modified inside loops are conservatively widened to their type's
full range:

```lain
var x = 0
for i in 0..10 {
    x = x + 1
}
// x is widened to [INT_MIN, INT_MAX] after the loop
```

This is a known precision limitation. The compiler cannot prove `x == 10`
after the loop.

> Note: Future versions may support loop invariant annotations to improve
> precision.

## 8.9 Static Array Bounds Checking [Implemented]

Array indexing is statically verified using VRA:

```lain
var arr int[10]
for i in 0..10 {
    arr[i] = i             // OK: i in [0, 9], arr.len = 10
}

arr[10] = 0                // ERROR: 10 is not in [0, 10)
arr[-1] = 0                // ERROR: -1 is not in [0, 10)
```

> **CONSTRAINT:** Every array access `arr[i]` shall be statically proven
> to satisfy `0 <= i < arr.len`. Accesses that cannot be proven safe are
> rejected as compile errors. There are no runtime bounds checks.

## 8.10 Division by Zero Detection [Implemented]

> **CONSTRAINT:** Division by a compile-time zero value shall produce
> diagnostic `[E015]`.

```lain
func bad() int {
    return 10 / 0          // ERROR [E015]: division by zero
}
```

For non-constant divisors, the `!= 0` constraint can be used:

```lain
func safe_div(a int, b int != 0) int {
    return a / b           // OK: b is guaranteed non-zero
}
```

## 8.11 Properties of VRA

| Property | Value |
|:---------|:------|
| **Decidable** | Yes — always terminates |
| **Time complexity** | Polynomial |
| **Sound** | Yes — never accepts invalid programs |
| **Complete** | No — may reject valid programs (conservative) |
| **Runtime cost** | Zero — all checks at compile time |

The VRA system is designed to be predictable: programmers can reason about
what the compiler can prove without understanding complex solvers.

---

*This chapter is normative.*
