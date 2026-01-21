# Spec 001: Ownership & Linear Types

## 1. Abstract
This specification defines the Ownership and Linear Type system for Lain. This system guarantees memory safety without garbage collection by enforcing that every resource has a single owner and is consumed exactly once (validating proper resource management) or at most once (for memory safety, though Lain prefers "exactly once" for resources). It also defines borrowing rules to allow safe sharing of data.

## 2. Terminology

*   **Linear Type**: A type whose values must be "used" exactly once. In Lain, this applies to types marked with `mov` or specific resource types.
*   **Affine Type**: A type whose values can be used *at most* once. (Lain's "move" semantics are effectively affine checking that prevents use-after-move, but we aim for linear usage for resources like file handles).
*   **Owner**: The variable binding that is currently responsible for the value.
*   **Move**: The transfer of ownership from one variable to another. The original variable becomes invalid (uninitialized).
*   **Borrow**: Creating a temporary reference (view) to a value without taking ownership.

## 3. Variable Declarations

### 3.1 Immutable (Default)
Standard constants/bindings. Cannot be modified.
```lain
x = 10      // Inferred type
var x = 10  // Explicit var, but if no 'mut', is it mutable?
            // PROPOSAL: 'var' implies MUTABLE. 
            // Bare assignment 'x = 10' implies IMMUTABLE binding.
```
*   **Syntax**: `ident = value`
*   **Semantics**: Immutable binding. Copy semantics for primitives (int, bool).

### 3.2 Mutable
Variables that can be reassigned or modified.
```lain
var x int = 10
x = 20
```
*   **Syntax**: `var ident [Type] = value`
*   **Semantics**: Mutable binding.

### 3.3 Linear (Move) Declarations
Variables that hold ownership of a resource or linear object.
*   **Proposal**: Use `mov` keyword for declaration to enforce linearity explicitly.
```lain
mov r = Resource.new() 
// 'r' MUST be moved or correctly disposed of by the end of scope.
```

## 4. Ownership Rules

1.  **Single Owner**: A value has exactly one variable as its owner.
2.  **Move Semantics**: Assigning a linear value to another variable transfers ownership.
    ```lain
    mov a = Resource.new()
    mov b = a  // 'a' is now invalid (uninitialized). 'b' is the owner.
    // use(a) // ERROR: Use after move
    ```
3.  **Consumption**: Linear variables **must** be consumed (moved or passed to a function taking ownership) before they go out of scope.
    *   *Exception*: Primitives (int, u8) have Copy semantics and are not linear unless wrapped.

## 5. Borrowing Rules (References)

To use a value without taking ownership, one can borrow it.

### 5.1 Shared Borrow (`T`)
*   Syntax: `func foo(x Point)` (Pass by shared reference/value - for structs this is effectively a read-only view).
*   Rule: Multiple shared borrows allowed. No mutable borrows allowed while shared borrows exist.

### 5.2 Mutable Borrow (`mut T`)
*   Syntax: `func update(x mut Point)` (Pass by mutable reference).
*   Rule: Only **one** mutable borrow allowed at a time. No other borrows (shared or mutable) allowed.

### 5.3 Function Parameters
```lain
func read(p Point) { ... }      // Read-only access
proc update(p mut Point) { ... }  // Mutable access
func consume(p mov Point) { ... } // Takes ownership
```

## 6. Destructuring
Destructuring a linear struct moves its fields out.
```lain
type Res { ptr u8[:0] }

mov r = Res{...}
// Destructure:
mov { ptr: p } = r
// 'r' is consumed. 'p' is now a linear variable (if u8[:0] is linear).
```

## 7. Control Flow Implication
*   **Branches**: If a linear variable is moved in one branch of an `if`, it must be moved in **all** branches, OR the variable cannot be used after the `if` block.
    ```lain
    mov x = ...
    if cond {
        consume(x)
    } else {
        consume(x)
    }
    // x is consumed in both paths -> OK (x is consumed).
    
    // BAD:
    if cond {
        consume(x)
    }
    // else: x not consumed.
    // After if: x status is inconsistent (consumed vs unconsumed). -> ERROR.
    ```

## 8. Compiler Implementation Strategy
1.  **Resolution**: Track variable definitions.
2.  **Type Check**: identify `mov` types.
3.  **Linearity Check (Flow Sensitive)**:
    *   Walk the AST.
    *   Maintain a table of "Linear Variables" and their states: `Unconsumed`, `Consumed`.
    *   On usage (Move): Transition `Unconsumed` -> `Consumed`.
    *   On usage (Copy/Read): Allowed only if not consumed.
    *   Loop boundaries: Verify no linear variable defined outside is moved inside a loop (unless re-assigned).
    *   Scope Exit: Error if any linear variable is `Unconsumed`.

## 9. Conflict Resolution (Crash Fix)
The current crash in `tests/use_after_move_fail.ln` is likely due to the parser not correctly handling `mov` as a storage class specifier in `StmtVar`, or the linearity checker dereferencing a NULL pointer when looking up a moved variable's state.
**Fix**: Ensure `sema/linearity.h` safely handles lookup failures and reports errors instead of `exit(1)` or crashing.
