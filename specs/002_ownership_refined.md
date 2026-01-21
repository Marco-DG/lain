# Spec 002: Refined Ownership Syntax

## 1. Motivation
The initial ownership syntax (`mov x = ...`, `var x = ...`, `func(x mut Point)`) was inconsistent, placing keywords in varying positions (sometimes as declarators, sometimes as type modifiers). This caused confusion about when and where to use `mov`, `mut`, and `var`.

This specification proposes a **unified, consistent syntax** that separates **Declaration** (creating a binding) from **Ownership/Mutability** (properties of the type or operation).

## 2. Core Philosophy
1.  **Unified Declaration**: All variable bindings use `var` (mutable/linear) or `let` (immutable). `mov` is **no longer a declarator**.
2.  **Type-Driven Linearity**: Whether a variable is "linear" (must be used once) is determined by its **Type**, not the keyword used to declare it.
3.  **Explicit Operators**: `mov` and `mut` are **operators** used in expressions or type signatures, not variable declarators.

## 3. Proposed Syntax

### 3.1 Variable Declarations
**Old Way:**
```lain
mov r = Resource.new()  // Linear
var x = 10              // Mutable
x = 20                  // Assignment (or immutable decl?)
```

**New Way:**
Use `var` for all new bindings.
```lain
var x = 10              // Mutable int
var r = Resource.new()  // Linear Resource (inferred from return type)
```
*   Lineanty is a property of definitions. `Resource` is defined as a linear struct. Therefore, any variable holding a `Resource` is tracked by the linearity checker.

### 3.2 Move Semantics (The `mov` Operator)
`mov` is strictly an operator used to **transfer ownership** of an existing variable.

**Old Way:**
```lain
mov b = a    // ambiguous: is 'mov' the decl or the op?
```

**New Way:**
```lain
var b = mov a   // Clear: 'var' declares b, 'mov a' consumes a
```
*   `mov a` expression evaluates to the value of `a` and marks `a` as consumed in the current scope.

### 3.3 Function Parameters & Borrows
Ownership is defined in the **Type Signature**.

**Old Way:**
```lain
func foo(p mov Point)   // 'mov' before Type
func bar(p mut Point)   // 'mut' before Type
```

**New Way:**
Modifiers belong to the Type.
```lain
func take(p Point)      // Owned (Move) by default for linear types
func view(p &Point)     // Shared Borrow (ReadOnly) - using '&' or 'ref'
func edit(p mut Point)  // Mutable Borrow
```

### 3.4 Call Site
Call sites must match the signature, making costs explicit.

```lain
var p = Point{1, 2}

take(mov p)    // Explicit move required for owned params
view(&p)       // Explicit shared borrow
edit(mut p)    // Explicit mutable borrow
```

## 4. Comparison Table

| Feature | Old / Ambiguous Syntax | Proposed Refined Syntax | Notes |
| :--- | :--- | :--- | :--- |
| **Linear Decl** | `mov r = Res()` | `var r = Res()` | Linearity inferred from `Res` type |
| **Mutable Decl** | `var i = 0` | `var i = 0` | Standardized on `var` |
| **Move** | `mov b = a` | `var b = mov a` | `mov` is an operator now |
| **Mut Borrow (Param)** | `p mut Point` | `p mut Point` | (Unchanged, maybe clarify `&mut`?) |
| **Shared Borrow (Param)**| `p Point` (ambiguous) | `p &Point` | Explicit reference syntax |
| **Call (Move)** | `take(p)` (sometimes implicit) | `take(mov p)` | Explicit move always required |
| **Call (Mut)** | `update(mut p)` | `update(mut p)` | Clear |

## 5. Implementation Impact
1.  **Parser**: Remove `TOKEN_KEYWORD_MOV` as a valid start for `parse_decl` or `parse_stmt`. It only appears in expressions (`parse_unary_expr` or distinct `parse_move_expr`) or implicitly as part of assignment handling.
2.  **Sema**: The Linearity Checker must look at the **Type** of the `var` to decide if it needs tracking, rather than relying on a `MODE_OWNED` flag set by the parser's `mov` keyword.
3.  **Migration**: Existing tests (`tests/ownership/`) would need to be updated to `var` syntax.
