# Spec 008: Var-Only Ownership Model

**Status**: Implemented
**Date**: 2026-01-21

## 1. Overview
This specification defines the "Var-Only" strategy for mutability and ownership in Lain. The core principle is that **immutability is the default**, and the keyword **`var`** is used exclusively to opt-in to mutability. This simplifies the language by removing the distinction between "variable declarations" (`var`) and "mutable modifiers" (`mut`).

## 2. Declaration Syntax

### 2.1 Immutable Declarations (Default)
Bindings are immutable by default. They use the assignment syntax without any keyword.

```lain
x = 10           // Infer type: int
p = Point{1, 2}  // Infer type: Point
```

*   **Reassignment**: Illegal (`x = 20` is an error).
*   **Modification**: Illegal (`p.x = 3` is an error).
*   **Borrowing**: Can only be borrowed immutably (`f(p)`).
*   **Moving**: Can be moved/consumed if the type is Linear (`close(mov file_handle)`), ending the scope of the variable.

### 2.2 Mutable Declarations (`var`)
Mutable bindings must be explicitly declared with `var`.

```lain
var x = 10
var p = Point{1, 2}
```

*   **Reassignment**: Allowed (`x = 20`).
*   **Modification**: Allowed (`p.x = 3`).
*   **Borrowing**: Can be borrowed mutably (`f(var p)`) or immutably (`f(p)`).
*   **Moving**: Can be moved (`consume(mov p)`).

## 3. Function Parameters

Function parameters follow the same logic: `var` indicates meaningful mutation (input/output parameters), while the absence of `var` indicates a read-only view. `mov` indicates ownership transfer.

| Syntax | Mode | Semantics | C Equivalent |
| :--- | :--- | :--- | :--- |
| `p T` | **Shared** | Read-only borrow. Non-linear types are copied (primitives) or passed by const pointer (aggregates). Linear types are borrowed. | `const T*` (aggregate) / `T` (primitive) |
| `p var T` | **Mutable** | Read-write borrow. The function can modify `p` and the caller will see changes. | `T*` |
| `p mov T` | **Owned** | Ownership transfer. The callee takes responsibility for destroying the resource. | `T` (value) |

### 3.1 Examples

```lain
func read(p Point) { ... }       // Cannot modify p
func update(p var Point) { ... } // Can modify p
func drop(p mov File) { ... }    // Consumes p, closing the file
```

## 4. Linearity & Resource Safety

Lain enforces linear types (resources that must be used exactly once).

*   **Consumption**: A linear variable must be "consumed" (moved) exactly once on every execution path.
*   **Destructuring**: Linear structs can be destructured in function parameters to implicitly consume them.

```lain
// Implicit consumption via destructuring
func drop_resource({handle} mov Resource) {
    // 'handle' is extracted, 'Resource' is considered consumed.
    close_handle(handle) 
}
```

*   **Use-After-Move**: Using a variable after it has been moved (via `mov`) is a compile-time error.

## 5. Address-of / In-Place Mutation

The `var` keyword is also used as an operator to take a mutable reference (address) of a variable, consistent with its declaration usage.

```lain
var x = 10
increment(var x) // Pass mutable reference to x
```

## 6. Summary of Changes from Legacy

1.  **Removed `mut` keyword**: Replaced entirely by `var`.
2.  **Removed `var` declarations**: Replaced by implicit immutable declarations (`x = val`).
3.  **Unified Syntax**: `var` always means "mutable", whether in a declaration, a parameter, or an argument.
