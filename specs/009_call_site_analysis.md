# Design Note 009: Analysis of Implicit Call-Site Modifiers

**Topic**: Removing `var` and `mov` annotations at function call sites.
**Current**: `f(x)`, `f(var x)`, `f(mov x)`
**Proposed**: `f(x)` (Semantics inferred from function signature)

## 1. The Proposal
The idea is to rely on the function signature to determine if an argument is borrowed, mutated, or moved, rather than requiring the caller to explicitly annotate it.

```lain
// Definition
func take_ownership(p mov Point) { ... }
func mutate(p var Point) { ... }
func read(p Point) { ... }

// Current Call Site
take_ownership(mov p)
mutate(var p)
read(p)

// Proposed Call Site (Implicit)
take_ownership(p)
mutate(p)
read(p)
```

## 2. Pros (Advantages)

### 2.1 Conciseness & Aesthetics
*   **Reduced Noise**: Code is less cluttered. `process(var context, mov data)` becomes `process(context, data)`.
*   **Fluidity**: Writing code is faster as there's less "ritual" for every mutable call.

### 2.2 Familiarity
*   **Mainstream Alignment**: This matches the behavior of Java, Python, C#, and JavaScript, where objects are passed by reference implicitly.
*   **C++ Alignment**: In C++, `foo(x)` could be passing by value or by reference (`foo(int& x)`). The call site looks the same.

### 2.3 Refactoring Ease
*   Changing a parameter from `Point` (by value) to `const Point&` (by reference) doesn't require updating every call site.

## 3. Cons (Disadvantages)

### 3.1 Loss of "Visual Grep" (Critical)
*   **Hidden Side Effects**: With implicit syntax, `update(config)` looks harmless. You cannot tell if `config` is modified just by reading the local code. You *must* check the definition of `update`.
*   **Hidden Consumption**: `process(resource)` looks safe, but if `process` takes ownership (`mov`), `resource` is gone. A subsequent line `use(resource)` would fail compilation, but the *reason* (the move happening at `process`) is not visible locally.

### 3.2 Refactoring Risks
*   **Silent Semantic Change**: If you change `func foo(x T)` to `func foo(x var T)`, logically you are changing a read operation to a write operation.
    *   **Explicit**: All callers fail compilation (`foo(x)` vs `foo(var x)` mismatch). You are forced to audit every call site to ensure mutation is intended.
    *   **Implicit**: Code continues to compile, but now `foo` mutates its argument silently. This can introduce subtle bugs.

### 3.3 Philosophical Inconsistency
*   Lain's design (especially the "Var-Only" shift) prioritizes **explicit mutability**.
*   We require `var x = ...` to signal "this declaration changes".
*   It is inconsistent to then hide "this function call changes x" behind implicit syntax.

### 3.4 Ambiguity Resolution (Technical)
*   **Overloading**: If we support function overloading, `f(x)` becomes ambiguous if `f(T)` and `f(var T)` both exist. Explicit explicit modifiers resolve this (`f(x)` vs `f(var x)`).

## 4. Alternate Middle Ground: "Rust Style"
Rust requires explicit `&mut`, but moves are implicit.

*   `f(&mut x)`: Mutation is explicit.
*   `f(x)`: Move is implicit (for non-Copy types).

**Analysis**: Rust treats "Move" as the default for linear types. Lain currently treats "Shared Borrow" as the default. Making Move implicit in Lain would be confusing because `f(x)` usually means "Borrow".

## 5. Recommendation

**Keep Explicit Modifiers.**

The primary value proposition of Lain appears to be safety and clarity ("Local Reasoning"). Removing call-site modifiers sacrifices Local Reasoning for minor aesthetic gains.

*   **Explicit is better than implicit** (Zen of Python).
*   **Mutation should be visible**.
*   **Ownership transfer (Move) should be visible**.

If concise syntax is desired, we could relax `mov` validation to be implicit *only for literals or rvalues* (e.g. `f(Point{1,2})` vs `f(mov Point{1,2})`), but for variables (`f(x)`), explicit markers prevent bugs.
