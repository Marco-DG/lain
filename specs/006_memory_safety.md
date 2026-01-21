# Spec 006: Lain Memory Safety & Ownership Model

## 1. Introduction

Lain provides **guaranteed memory safety** and **resource correctness** without a garbage collector. This is achieved through a static analysis system comprised of three pillars:
1.  **Ownership**: Every value has a single owner responsible for its lifecycle.
2.  **Linearity**: Resources must be used exactly once (no leaks, no double-frees).
3.  **Borrow Checking**: Data can be temporarily shared (Aliasing) xor mutated (Mutability), but never both simultaneously.

This document serves as the formal definition of these mechanisms using the **Mut-Only Syntax** (Removal of `var`).

---

## 2. Bindings & Mutability

A **Variable Binding** associates a name (identifier) with a value in memory. The properties of this binding are defined at declaration.

### 2.1 Lexical Definitions
*   **Immutable Binding**: A binding that cannot be reassigned and provides only read-only access to its data.
*   **Mutable Binding**: A binding that can be reassigned and provides read-write access to its data.
*   **`mut` Keyword**: The sole designator for mutability in bindings, parameters, and access patterns.

### 2.2 Declaration Syntax

#### 2.2.1 Immutable Declarations
By default, all bindings are immutable.
```lain
identifier = expression
```
*   **Reassignment**: Forbidden (`id = new_val` is Error).
*   **Modification**: Forbidden (`id.field = 5` is Error).
*   **Move**: Allowed (Ownership transfer is distinct from mutation).

#### 2.2.2 Mutable Declarations
Mutability must be requested explicitly via `mut`.
```lain
mut identifier = expression
```
*   **Reassignment**: Allowed (`id = new_val`).
*   **Modification**: Allowed (`id.field = 5`).

---

## 3. Ownership & Linearity

Lain utilizes a **Strict Linear Type System** for resources and an **Affine System** (Move semantics) for general memory safety.

### 3.1 The Owner
*   At any point in the control flow, exactly **one** binding owns a value.
*   When the owner goes out of scope, the value is dropped (for non-linear types) or must have been consumed (for linear types).

### 3.2 Linear Types ("Must Use")
Types marked as linear (e.g., File handles, Sockets, Heap allocations) have strict lifecycle obligations.
*   **Constraint**: A linear variable must be **Consumed** exactly once on every execution path.
*   **Consumption**: Occurs via a **Move** or by passing to a function that takes ownership.
*   **Error Condition**: If a scope ends and a linear variable is still live (Unconsumed), the compiler reports a "Resource Leak" error.

### 3.3 The `mov` Operator
Ownership transfer is explicit. The `mov` operator extracts the value from a binding, leaving the binding in an **Uninitialized** state.

```lain
dest = mov source
```
*   **Effect**: `source` becomes uninitialized. `dest` becomes the new owner.
*   **Safety**: Subsequent use of `source` is a compile-time "Use After Move" error.

---

## 4. Borrowing (References)

To allow data access without transferring ownership, Lain implements **Borrowing**. A borrow is a scoped reference to a value.

### 4.1 The Borrowing Rules (The "RWLock" Principle)
At any discrete point in the program, a value may have:
1.  **Infinite** Shared (Read-Only) Borrows.
    *   *OR*
2.  **Exactly One** Mutable (Read-Write) Borrow.

This guarantees absence of data races and iterator invalidation bugs.

### 4.2 Shared Borrow (Implicit)
Used for read-only access.
*   **Type Syntax**: `T` (e.g., `Point`, `String`) in argument position implied Borrow.
*   **Semantics**: The callee receives a reference. The caller retains ownership.
*   **Restriction**: Neither caller nor callee can mutate the value while the borrow is active.

```lain
func read(p Point) { ... } // Signature implies: p is &Point (Shared)
```

### 4.3 Mutable Borrow (`mut`)
Used for read-write access.
*   **Type Syntax**: `mut T` (e.g., `mut Point`).
*   **Semantics**: The callee receives an exclusive mutable reference.
*   **Restriction**: No other access (read or write) is allowed to the original owner until this borrow expires.

```lain
func update(p mut Point) { ... }
```

---

## 5. Call Site Semantics

Lain enforces "Explicit Costs" at the call site. The programmer must explicitly opt-in to dangerous actions (mutation or ownership loss).

| Action | Signature | Call Site | Note |
| :--- | :--- | :--- | :--- |
| **Read** | `func f(x T)` | `f(x)` | Safe, cheap, default. |
| **Write** | `func f(x mut T)` | `f(mut x)` | Explicit `mut` warns of side-effects. |
| **Take** | `func f(x mov T)` | `f(mov x)` | Explicit `mov` warns of loss. |

---

## 6. Advanced Mechanics

### 6.1 Reassignment of Linear Variables
Reassigning a linear variable (`mut r = ...`) defines a critical safety hazard: overwriting an active resource causes a leak.

**Rule**: Assignments to populated linear variables are forbidden.
**Resolution**: The user must explicitly consume (drop) the strict linear value before reassignment.

```lain
mut f = File.open("a.txt")
// f = File.open("b.txt") // ERROR: Linear variable 'f' is not consumed. Leak.

close(mov f)          // 1. Consume explicitely
f = File.open("b.txt")    // 2. Reassign (f is now uninitialized, so assignment is safe)
```

### 6.2 Destructuring
Structs can be unpacked. This "peels off" the ownership layer.
*   Immutable Destructuring: `param = struct.field` (Copies if primitive, Error if linear/owned).
*   Linear Destructuring:
    ```lain
    // Consumes 'pkg', extracts 'contents'
    mov { contents } = pkg 
    ```

### 6.3 Control Flow & Initialization Analysis
The compiler performs flow-sensitive analysis:
*   **Branches**: If a variable is moved in `if`, it must be moved in `else` to maintain consistent state, OR the variable is considered "Maybe Moved" (unusable) after the block.
*   **Loops**: A linear variable defined outside a loop cannot be moved inside the loop unless it is re-assigned guaranteed within the same iteration (or it would be a "Use After Move" in the second iteration).

---

## 7. Memory Model Summary

| Type Category | Stack Logic | Passing Semantics |
| :--- | :--- | :--- |
| **Primitives** (int, bool) | Stack Value | Copied (Pass-by-value) |
| **Aggregates** (struct, array) | Stack Value | Borrowed (Pass-by-ref) unless `mov` |
| **Resources** (File, Socket) | Stack Handle to System Resource | Moved (Ownership transfer) |

This model ensures correct resource management without the runtime overhead of reference counting or garbage collection.
