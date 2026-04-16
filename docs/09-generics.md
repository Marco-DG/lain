# Chapter 9 — Compile-Time Evaluation & Generics

## 9.1 Overview

Lain uses **compile-time function evaluation (CTFE)** instead of traditional
parametric polymorphism syntax (e.g., `<T>`). Generic types and functions are
ordinary `func` declarations that take `comptime` parameters and are evaluated
at compile time. The result is monomorphized code — each unique set of
compile-time arguments produces a distinct specialization.

This design unifies generics, type aliases, and compile-time assertions into
a single mechanism.

## 9.2 The `comptime` Parameter Modifier [Implemented]

### 9.2.1 Syntax

```
comptime_param = "comptime" IDENTIFIER type_expr ;
```

The `comptime` keyword before a parameter declares that the argument must be
known at compile time. The parameter is bound in the function body as a
compile-time constant:

```lain
func identity(comptime T type, x T) T {
    return x
}
```

### 9.2.2 Comptime Parameter Types

| Parameter Type | Meaning |
|:---------------|:--------|
| `comptime T type` | `T` is a type parameter |
| `comptime N int` | `N` is a compile-time integer [Planned] |

Currently, only `type` parameters are implemented. Compile-time integer
parameters are planned (see §9.9).

### 9.2.3 Comptime in Parameter Position

A `comptime` parameter may appear at any position in the parameter list.
Non-comptime parameters may reference comptime type parameters:

```lain
func wrap(comptime T type, value T) T {
    return value
}
```

Here, `value` has type `T`, which is resolved at each call site to the
concrete type argument.

## 9.3 Type as Value [Implemented]

### 9.3.1 The `type` Type

The keyword `type` serves double duty:

1. **As a parameter type**: `comptime T type` — declares `T` as a type parameter.
2. **As a return type**: `func F(...) type` — the function returns a type.

Functions returning `type` are type constructors. They are evaluated entirely
at compile time.

### 9.3.2 Anonymous Type Expressions

Inside a function returning `type`, the `type { ... }` expression constructs
a new anonymous type:

**Anonymous struct:**
```lain
func Pair(comptime T type) type {
    return type {
        first T
        second T
    }
}
```

**Anonymous ADT (enum with variants):**
```lain
func Option(comptime T type) type {
    return type {
        Some { value T }
        None
    }
}
```

The parser determines whether the anonymous type is a struct or an ADT based
on the presence of variant syntax (capitalized identifiers followed by field
blocks or standing alone).

### 9.3.3 Evaluation Rules

> **CONSTRAINT:** A function with return type `type` shall be a `func` (not
> `proc`). It shall be evaluable at compile time — its body may only use:
> - Variable declarations and assignments
> - `if`/`else` with compile-time-evaluable conditions
> - `return` statements
> - Calls to other comptime-evaluable functions
> - `compileError()` intrinsic

## 9.4 Type Aliases [Implemented]

### 9.4.1 Syntax

```
type_alias = "type" IDENTIFIER "=" expr ;
```

A type alias binds a name to the result of evaluating a type expression at
compile time:

```lain
type OptionInt = Option(int)
type Pair = Option(bool)
```

### 9.4.2 Evaluation

When the compiler encounters a type alias:

1. The right-hand side expression is resolved (name resolution).
2. The CTFE engine evaluates the expression.
3. The result determines what the alias becomes:

| Result Kind | Effect |
|:------------|:-------|
| `EXPR_ANON_STRUCT` | A new struct type is registered with the alias name |
| `EXPR_ANON_ENUM` | A new enum/ADT type is registered with the alias name |
| `EXPR_TYPE` | The alias becomes a transparent synonym for the type |

### 9.4.3 Example: Generic Option

```lain
func Option(comptime T type) type {
    return type {
        Some { value T }
        None
    }
}

type OptionInt = Option(int)

proc main() {
    var a OptionInt = OptionInt.Some(42)
    var b OptionInt = OptionInt.None
}
```

After evaluation, `OptionInt` behaves exactly as if the user had written:

```lain
type OptionInt {
    Some { value int }
    None
}
```

### 9.4.4 Simple Type Aliases

Type aliases can also alias primitive or existing types:

```lain
type MyInt = int          // MyInt is a synonym for int
```

> **CONSTRAINT:** The right-hand side of a type alias shall evaluate to a type
> at compile time. If evaluation fails or produces a non-type value, diagnostic
> `[E012]` is produced.

## 9.5 Monomorphization [Implemented]

### 9.5.1 Function Monomorphization

When a generic function (one with `comptime` parameters) is called with
concrete type arguments, the compiler generates a **specialized instance**:

```lain
func max(comptime T type, a T, b T) T {
    if a > b { return a }
    return b
}

max(int, 3, 5)       // generates max_int(int a, int b) -> int
max(bool, x, y)      // generates max_bool(bool a, bool b) -> bool
```

### 9.5.2 Monomorphization Process

1. **Detection**: The compiler detects a call to a function with `comptime`
   parameters during name resolution.

2. **Argument resolution**: All arguments are resolved. Comptime arguments
   must be `EXPR_TYPE` (type values).

3. **Name mangling**: A mangled name is constructed by appending the concrete
   type names: `funcname_type1_type2_...`

4. **Deduplication**: If an instance with the same mangled name already exists,
   no new instance is created — the call is redirected to the existing one.

5. **Instantiation**: The function's AST is cloned. Type substitution replaces
   every occurrence of the comptime parameter name with the concrete type
   throughout the cloned AST (parameters, body, return type).

6. **Registration**: The specialized instance is inserted into the global
   symbol table and appended to the declaration list for subsequent type
   checking and code generation.

7. **Callee rewrite**: The call expression's callee is rewritten to point
   to the mangled name.

### 9.5.3 Type Substitution

The substitution engine (`generic_substitute_*`) walks the entire cloned AST
and replaces type references matching the parameter name:

- **Types**: `TYPE_SIMPLE` nodes whose name matches the parameter are replaced
  with the concrete type.
- **Expressions**: All sub-expressions are recursively substituted.
- **Statements**: All statements in the function body are recursively
  substituted.
- **Declarations**: Parameter types, return types, and nested declarations
  are substituted.

The substitution handles:
- Array element types: `T[10]` → `int[10]`
- Slice element types: `T[:]` → `int[:]`
- Pointer element types: `*T` → `*int`
- Nested comptime wrappers

### 9.5.4 Generated C Code

Each monomorphized instance becomes an independent C function:

```c
// max(int, 3, 5) generates:
int main_max_int(int a, int b) {
    if (a > b) { return a; }
    return b;
}
```

## 9.6 The CTFE Engine [Implemented]

### 9.6.1 Architecture

The compile-time function evaluation engine is a tree-walking interpreter
over the AST. It operates during the name resolution phase, before type
checking and code generation.

### 9.6.2 Evaluation Environment

The CTFE engine maintains a linked-list environment mapping parameter names
to their compile-time values:

```
ComptimeEnv = { name: Id, value: Expr, next: ComptimeEnv* }
```

### 9.6.3 Supported Constructs

The CTFE engine evaluates the following AST nodes:

| Node | Behavior |
|:-----|:---------|
| `EXPR_IDENTIFIER` | Environment lookup; falls back to global symbol table |
| `EXPR_BINARY` | Type equality (`==`, `!=`) for `EXPR_TYPE` operands |
| `EXPR_TYPE` | Returned as-is (already a compile-time value) |
| `EXPR_ANON_STRUCT` | Returned as-is |
| `EXPR_ANON_ENUM` | Returned as-is |
| `EXPR_MEMBER` | Target evaluated, member access reconstructed |
| `EXPR_CALL` | Recursive function evaluation |
| `STMT_VAR` | Binds evaluated initializer in environment |
| `STMT_ASSIGN` | Updates environment variable |
| `STMT_IF` | Evaluates condition; takes appropriate branch |
| `STMT_RETURN` | Returns evaluated expression |
| `STMT_EXPR` | Evaluates for side effects |

### 9.6.4 Type Comparison

The CTFE engine supports compile-time type comparison:

```lain
if T == int {
    // T is int
}
if T != bool {
    // T is not bool
}
```

Type equality is determined by comparing `TYPE_SIMPLE` base type names.

### 9.6.5 Post-Evaluation Substitution

After the CTFE engine returns a result expression, all comptime type
parameters in the result are substituted with their concrete types. This
ensures that anonymous types returned by the function have all parameter
references replaced:

```lain
func Option(comptime T type) type {
    return type {
        Some { value T }    // T is substituted with the actual type
        None
    }
}
```

## 9.7 Compile-Time Intrinsics [Implemented]

### 9.7.1 `compileError`

The `compileError(msg)` intrinsic aborts compilation with a user-specified
error message. It is used in comptime functions to enforce constraints on
type arguments:

```lain
func AssertInt(comptime T type) type {
    if T != int {
        compileError("Type must be int!")
    }
    return T
}

type MyInt = AssertInt(bool)   // ERROR: "Type must be int!"
```

> **CONSTRAINT:** `compileError` shall only be called within comptime-evaluated
> code. Its argument shall be a string literal.

## 9.8 Interaction with Other Systems [Implemented]

### 9.8.1 Ownership

Generic types interact with ownership naturally through monomorphization.
After type substitution, the ownership system operates on concrete types:

- If `T` is a linear type, `Option(T)` produces a linear ADT (because a
  variant contains a linear field).
- If `T` is copyable, `Option(T)` produces a copyable ADT.

The linearity checker (`sema_type_is_linear()`) operates on post-monomorphization
concrete types, so no special generic handling is needed.

### 9.8.2 Type Checking

Monomorphized instances are appended to the declaration list and undergo
the full semantic analysis pipeline (type checking, borrow checking, VRA)
as if they were hand-written functions.

### 9.8.3 Exhaustiveness

Pattern matching on generic ADT instances (e.g., `case optionValue { ... }`)
uses the concrete variant list from the monomorphized type. Exhaustiveness
checking works identically to non-generic ADTs.

## 9.9 Planned Extensions

### 9.9.1 Compile-Time Integer Parameters [Planned]

```lain
func Array(comptime T type, comptime N int > 0) type {
    return type {
        data T[N]
        len int
    }
}
```

This requires extending the CTFE engine to:
- Evaluate integer arithmetic at compile time
- Support VRA constraints on comptime integer parameters
- Substitute integer values in array size expressions

### 9.9.2 Comptime Constraints [Planned]

Type parameters with constraints:

```lain
func sort(comptime T type, arr T[:]) T[:] {
    // T must support comparison operators
}
```

This would require a trait-like constraint system or compile-time reflection
to verify that a type supports specific operations.

### 9.9.3 Comptime String Parameters [Planned]

```lain
func field_name(comptime name string) type {
    // ...
}
```

Support for compile-time string manipulation is not yet implemented.

## 9.10 Standard Library Generic Types [Implemented]

### 9.10.1 Option

```lain
func Option(comptime T type) type {
    return type {
        Some { value T }
        None
    }
}
```

Represents an optional value. Instantiated as `type OptionInt = Option(int)`.

### 9.10.2 Result

```lain
func Result(comptime T type, comptime E type) type {
    return type {
        Ok { value T }
        Err { error E }
    }
}
```

Represents a computation that may succeed with `Ok` or fail with `Err`.
Instantiated as `type ResultIntStr = Result(int, u8[:0])`.

## 9.11 Properties of Comptime Generics

| Property | Value |
|:---------|:------|
| **Mechanism** | Compile-time function evaluation + monomorphization |
| **Syntax overhead** | Zero — uses existing `func` syntax |
| **Runtime cost** | Zero — all generic dispatch resolved at compile time |
| **Code size** | Each instantiation produces independent code |
| **Type safety** | Full — monomorphized code goes through complete type checking |
| **Ownership safety** | Full — linearity propagates through concrete types |

---

*This chapter is normative for implemented features. Sections marked [Planned]
describe future extensions and are not binding.*
