# Specification 012: Memory Safety & Ownership (Final)

**Status**: Implemented
**Version**: 1.0 (Var-Only, Prefix Syntax)

## 1. Overview

Lain guarantees memory safety without garbage collection through a strict **Ownership & Borrowing** system. This system eliminates:
*   Use-After-Free
*   Double-Free
*   Data Races
*   Memory Leaks (for linear resources)

## 2. Mutability Model ("Var-Only")

Lain distinguishes bindings based on mutability. There is no `mut` keyword.

### 2.1 Immutable Binding (Default)
Variables declared without `var` are immutable. They must be initialized and cannot be reassigned.

```lain
x = 10
// x = 20      <-- Compile Error
```

### 2.2 Mutable Binding (`var`)
Variables declared with `var` are mutable binding slots.

```lain
var y = 10
y = 20         <-- OK
```

## 3. Ownership Semantics

### 3.1 Linear Types (`mov`)
A value bound to a `mov` parameter or variable is **Owned**.
*   **Rule**: Owned values must be consumed exactly once.
*   **Consumption**: Occurs via passing to another `mov` parameter, destroying, or returning.
*   **Drop**: Unused linear variables cause a compile-time "must be used" error.

```lain
func take(p mov Point) {
    // p is owned here. 
    // It must be consumed (e.g. valid end of scope or moved again).
}
```

### 3.2 Borrowing (References)
Lain uses a "Read-Write Lock" model for references, enforced at compile time.

*   **Shared Borrow** (`p T`):
    *   Read-Reference.
    *   **Rule**: You can have N shared borrows.
    *   **Restriction**: Cannot coexist with a Mutable Borrow.

*   **Mutable Borrow** (`var p T`):
    *   Read-Write Reference.
    *   **Rule**: You can have exactly 1 mutable borrow.
    *   **Restriction**: Cannot coexist with any other borrow (Shared or Mutable).

## 4. Syntax Reference

### 4.1 Variable Declaration
```lain
val = 10            // Implicit Immutable
var mut_val = 10    // Explicit Mutable
```

### 4.2 Function Parameters (Prefix Syntax)
| Mode | Syntax | Semantics |
| :--- | :--- | :--- |
| **Shared** | `name Type` | Borrow (Read-Only) |
| **Mutable** | `var name Type` | Borrow (Read-Write) |
| **Owned** | `mov name Type` | Transfer Ownership (Move) |

Example:
```lain
func process(
    id int,          // Copied (Primitive)
    ctx var Context, // Mutable Borrow
    res mov Resource // MOVED (Ownership Transfer)
) { ... }
```

### 4.3 Return Types
*   `func foo() T` : Return by value / copy.
*   `func foo() var T` : Return a mutable reference (e.g. accessing a slot in a struct).
*   `func foo() mov T` : Return an owned linear resource.

## 5. The `mov` Operator
Syntax: `mov <lvalue>`

*   Used at call sites to explicitly signal a move.
*   Invalidates the source variable.

```lain
var a = Resource.new()
take(mov a)
// use(a) <-- Compile Error: Use after move
```

## 6. Safety Invariants
1.  **Linearity**: Total number of live instances of a linear resource is invariant across moves. It is never duplicated.
2.  **Exclusive Mutability**: If `var p T` exists and is live, no other access to the underlying data is possible.
