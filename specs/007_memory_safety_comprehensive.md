# Spec 007: Lain Memory Safety System (Comprehensive Reference)

## 1. Introduction & Core Philosophy
Lain aims to provide **zero-cost memory safety** without garbage collection. The system is built on three foundational pillars enforced at compile-time:
1.  **Ownership**: Every value has a single owner.
2.  **Linearity**: Resources must be consumed exactly once (no leaks).
3.  **Borrowing**: References follow the "Shared-XOR-Mutable" rule (Reader-Writer Lock).

This document details the mechanics of these systems, their interaction with control flow, and the precise rules for implementation.

---

## 2. Syntax & Bindings (The "Mut-Only" Model)

Lain simplifies variable declarations by removing `var`. The presence of `mut` is the sole indicator of mutability/capability.

### 2.1 Bindings
A binding is a named handle to a value.

*   **Immutable Binding** (`identifier = expression`)
    *   **Capabilities**: Read-Only Access, Move (Membership Transfer).
    *   **Restrictions**: No Reassignment, No Mutation on fields.
    *   **Memory**: Typically stack-allocated.

*   **Mutable Binding** (`mut identifier = expression`)
    *   **Capabilities**: Read-Write Access, Move, Reassignment.
    *   **Restrictions**: Reassignment of Linear types requires prior consumption.

### 2.2 Global Variables
*   Globals follow the same syntax but have restricted capabilities.
*   **Immutable Globals**: Safe, visible everywhere.
*   **Mutable Globals**: **Unsafe**. Accessing them requires `unsafe` blocks (future feature) or is restricted to the main thread's root scope, as they introduce data races in multi-threaded contexts. *For v0.1: Mutable globals are effectively strict static-lifetime variables.*

---

## 3. Ownership & Move Semantics

### 3.1 The Move Operator (`mov`)
`mov` is a unary operator that performs a **Semantic Move**.
*   **Syntax**: `mov <lvalue>`
*   **Semantics**:
    1.  Read the bits of `<lvalue>`.
    2.  Mark `<lvalue>` as **Uninitialized/Moved** in the compiler's flow-analysis table.
    3.  Return the bits as an **RVALUE**.

### 3.2 Implicit vs Explicit Moves
*   **Lain is Explicit**: Moves never happen implicitly on assignment.
    *   `y = x` (Error if `x` is linear/owned).
    *   `y = mov x` (OK).
*   **Return Statements**: `return x` is an implicit move if the return type implies ownership. (Pragmatic exception).

### 3.3 Partial Moves (Destructuring)
Moving specific fields out of a struct is allowed via destructuring.
```lain
type Pkg { id int, data Resource }
pkg = Pkg{...}

// Destructure Moves
mov { data } = pkg 
// 'pkg' is now PARTIALLY MOVED. 
// 'pkg.id' is still valid (if int is Copy).
// 'pkg' as a whole is Invalid.
```
*Rule Invariant*: You cannot allow a partial move out of a struct that you borrowed. You must Own the struct to destruct it.

---

## 4. Borrowing Rules (The Reference System)

Lain enforces safety via distinct Reference types.

### 4.1 Shared References (Immutable View)
*   **Declaration**: `p T` (in args) or implicit `&x`.
*   **Semantics**: "Many readers".
*   **Restrictions**:
    *   Cannot mutate pointee.
    *   Cannot `mov` from pointee (would steal ownership).

### 4.2 Mutable References (Exclusive View)
*   **Declaration**: `p mut T` (in args).
*   **Semantics**: "One writer".
*   **Restrictions**:
    *   Must be the ONLY active reference to that data in the scope.
    *   Cannot `mov` from pointee (would leave the owner with a hole). *Exception: `swap` or `replace` primitives.*

### 4.3 The "Loan" Analysis
The compiler tracks "Loans".
1.  Expression `f(x)` creates a Loan on `x`.
2.  Expression `f(mut x)` creates a Mutable Loan on `x`.
3.  **Invariant**: While a Mutable Loan is active, no other usages of `x` (read or write) are valid.

---

## 5. Linearity & Control Flow Analysis

The **Linearity Checker** is a flow-sensitive analysis pass running after Type Checking.

### 5.1 State Tracking
For every Linear Variable (resource), the compiler tracks a state:
*   **Valid**: Alive and owning.
*   **Consumed**: Moved away.
*   **Uninitialized**: Declared but not set (or moved away).

### 5.2 Branching (Path Sensitivity)
When control flow splits (`if/else`, `match`), the state is tracked per-branch using a "Meet" operation (Intersection).

**Rule**: Determining state after a branch requires consistency.
*   If `x` is Consumed in `Branch A` but Valid in `Branch B` -> `State(After) = MaybeConsumed`.
*   **Usage of `MaybeConsumed`**: Error. You cannot use a variable that *might* be gone.

**Implication for Linear Types**:
To satisfy "Must Use" obligations:
*   A linear var must be consumed in **ALL** branches OR **NONE**.
*   If consumed in only one branch, the compiler forces a "Leak Error" in the other branch (unless returned/escaped).

```lain
mut r = Resource.new()
if cond {
    close(mov r)
} else {
    // Error: 'r' must be consumed here too to match the other branch, 
    // OR 'r' must be alive after the if.
    // Since 'r' is dead in the 'then' block, it is "MaybeConsumed" afterwards, rendering it unusable.
    // The strict linear checker demands exact consumption.
}
```

### 5.3 Loops (Cyclic Analysis)
Loops introduce re-entry.
**Rule**: The state of all variables at the *end* of the loop body must be compatible with the state expected at the *start* of the loop body.

**Scenario: Move in Loop**
```lain
r = Resource.new()
while true {
    close(mov r) 
}
```
*   Iteration 1: `r` is Valid -> `mov` -> `r` is Consumed.
*   Back edge: Loop restarts. `r` is Consumed?
*   Start of Iteration 2: `close(mov r)` -> **Error: Use After Move**.

*Fix*: Reassign inside the loop.
```lain
mut r = Resource.new()
while true {
    close(mov r)
    r = Resource.new() // 'r' becomes Valid again before back-edge. OK.
}
```

---

## 6. Interaction with Functions (ABI)

The ownership system maps cleanly to low-level ABI, enabling efficient execution.

### 6.1 Parameter Passing
| Lain Signature | Logical Meaning | Physical ABI (C Backend) |
| :--- | :--- | :--- |
| `f(x int)` | Copy | Passed by Value (`int`) |
| `f(x Point)` | Shared Borrow | Passed by Const Pointer (`const Point*`) |
| `f(x mut Point)`| Mut Borrow | Passed by Pointer (`Point*`) |
| `f(x mov Point)`| Ownership Transfer | Passed by Value (`Point`) OR Pointer if large (Backend optimization) |

*Note*: For large structs, "Move" might be implemented physically as a `memcpy` or passing a pointer, but logically the source is dead.

### 6.2 Return Values
*   `func f() T`: Returns an owned value (R-Value).
*   Typically return-by-value. (Subject to RVO - Return Value Optimization in backend).

---

## 7. Reassignment Rules (Detailed)

Reassigning a variable (`x = new_val`) is a two-step safety check:

1.  **Capability Check**: Is `x` declared `mut`? If no -> Error.
2.  **Liveness Check** (for Linear Types): Is `x` currently holding a value?
    *   **Case No (Uninit/Consumed)**: Safe to assign. `x` becomes Valid.
    *   **Case Yes (Valid)**: **Error**. You are overwriting (leaking) a live resource.
  
**Correct Pattern for Reassignment**:
```lain
mut r = Res.new()
// Use r...
close(mov r) // State -> Consumed
r = Res.new() // State -> Valid. OK.
```

---

## 8. Type System Integration

### 8.1 "Resource" Types
How does the compiler know a type is "Linear"?
*   **Annotation**: Types defined with a destructor (future feature) or marked intrinsic.
*   **Structural**: A struct containing a Linear Field is automatically Linear.

### 8.2 Copy Types
Primitives (`int`, `u8`, `bool`) are `Copy`.
*   `mov x` on a Copy type is a `memcpy`. The original `x` typically remains valid (Copy semantics) unless explicitly opted out.
*   *Lain Decision*: For simplicity, `mov` on primitive invalidates the variable name logic-wise in the checker, but physically it's just a copy. To allow reuse of `x` after use, pass by copy (implicit).

---

## 9. Implementation Checklist

To implement this spec, the following steps are required:

1.  **AST Refactor**: remove `is_mutable` from `DeclVariable`? No, keep it but source it from `mut` keyword presence.
2.  **Parser Update**: 
    *   Remove `TOKEN_KEYWORD_VAR`.
    *   Add `mut` parsing in `parse_decl`. (`mut ident = ...`)
    *   Update Param parsing to `ident [mut|mov] Type`.
3.  **Sema Update**:
    *   **Mutability Check**: Enforce `mut` requirement for assignments.
    *   **Linearity Check**: Implement the Branch/Loop state intersection logic described in Section 5.
    *   **Borrow Check**: Ensure `mut` args bind only to `mut` variables.
