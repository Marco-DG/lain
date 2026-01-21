# Spec 005: The "Mut-Only" Strategy

## 1. Core Idea
You suggested removing `var` because checking for both `var` (capability) and `mut` (action) adds mental overhead. 
This specification proposes a radical simplification: **`mut` is the single keyword for all things mutable.**

We remove `var` entirely from the language.

## 2. Declaration Syntax

We strictly distinguish between **Immutable Bindings** (default) and **Mutable Variable Bindings** (explicit `mut`).

### 2.1 Immutable Binding (Default)
`identifier = value`
*   No keyword.
*   Cannot reassign.
*   Cannot pass as `mut`.
*   *Note*: If the type is Linear (Resource), you **can** still Move/Consume it (ending its life), but you cannot mutate it in place or swap it.

```lain
x = 10
// x = 20    <-- ERROR: Immutable
// mut_fn(x) <-- ERROR: Immutable

r = File.open("data.txt")
close(mov r) // OK: Consuming is allowed (lifecycle end).
```

### 2.2 Mutable Binding (`mut`)
`mut identifier = value`
*   Replaces `var`.
*   Declares that "this name binds to a slot that can change".

```lain
mut x = 10
x = 20       // OK
mut_fn(mut x) // OK
```

## 3. Function Signatures (Unified)

Now `mut` behaves consistently across declarations and parameters. It always means "Mutable".

| Kind | Syntax | Meaning |
| :--- | :--- | :--- |
| **Shared (Read)** | `p Point` | I just read it. (Borrow) |
| **Mutable (Write)** | `p mut Point` | I will change it. (Mut Borrow) |
| **Owned (Move)** | `p mov Point` | I own it now. (Move) |

## 4. Usage Interactions

The rules become extremely simple: **You can only use `mut` on things declared as `mut`.**

### Scenario A: Primitives
```lain
func main() {
    x = 10      // Immutable
    mut y = 20  // Mutable

    // x = 11      <-- ERROR
    y = 21      // OK

    // inc(mut x)  <-- ERROR: 'x' is not 'mut'
    inc(mut y)  // OK
}
```

### Scenario B: Linear Resources
```lain
func main() {
    // Immutable binding of a Resource
    f1 = File.open("a.txt")
    // f1 = ... <-- ERROR: Cannot reassign.
    
    // Can I write to it?
    // write(f1, "data")     <-- depends on 'write' signature.
    // If 'write' needs 'mut File', then NO.
    // If 'write' just needs 'File' (shared handle), then YES.
    
    // Usually, writing changes internal state, so 'write' demands 'mut File'.
    // Therefore, to use a file, you usually need 'mut'.
    
    mut f2 = File.open("b.txt")
    write(mut f2, "data") // OK.
    
    close(mov f2) // OK: Move works on both mut and immut (it consumes binding).
}
```

## 5. Ambiguity Check

Does `mut x = ...` conflict with anything?

*   **Expression**: `mut x` exists as an operator (taking address/creating mutable reference).
*   **Statement**: A statement starting with `mut` could be:
    1.  `mut x = 10` (Declaration)
    2.  `mut x` (Expression statement: "take mutable address of x and discard it") -> This is a no-op / useless statement.

**Resolution**: At the statement level, if we see `mut identifier` followed by `=`, it is parsed as a Declaration. This is unambiguous.

## 6. The "Move" Operator

`mov` remains purely an operator for transfer.

```lain
mut a = Resource.new()
mut b = mov a   // 'mov a' expression consumes a. 'mut b' declares new mutable b.
```

## 7. Final Summary Table

| Concept | Syntax | Meaning |
| :--- | :--- | :--- |
| **Immutable Decl** | `x = val` | Stable value. |
| **Mutable Decl** | `mut x = val` | Changeable value. |
| **Assignment** | `x = val` | Update (requires `mut x`). |
| **Shared Param** | `p T` | Look. |
| **Mut Param** | `p mut T` | Touch. |
| **Owned Param** | `p mov T` | Take. |
| **Calls** | `f(x)`, `f(mut x)`, `f(mov x)` | Pass. |

## 8. Why this wins
1.  **Reduced Vocabulary**: `var` is gone.
2.  **Visual Grep**: If you want to find where state changes, search for `mut`. It catches declarations (`mut x =`), parameters (`p mut T`), and arguments (`f(mut x)`).
3.  **Consistency**: Mutability is always signaled by `mut`.

This seems to be the optimal design for the constraints you listed.
