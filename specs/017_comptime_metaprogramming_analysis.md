# Comptime Metaprogramming: The Generics Killer

## Introduction
You expressed interest in "Comptime Metaprogramming" as a future alternative to standard Generics. This approach, popularized effectively by Zig, replaces complex "Template Syntax" (`<T>`) with simple **Compile Time Function Execution (CTFE)**.

## The Core Concept
In languages like C++ or Rust, "Generics" are a special syntactic feature handled by the compiler's template engine.
In Comptime Metaprogramming, **Types are just values** that exist at compile time.

Instead of defining a generic `struct Option<T>`, you write a **function that returns a Type**.

### Example Vision for Lain

```lain
// A function that takes a Type 'T' and returns a new Type
func Option(comptime T type) type {
    // Generate a struct type programmatically
    return struct {
        has_value bool,
        payload   T
    }
}

// Usage
type OptionInt = Option(int)
var x OptionInt = ...
```

## Why it matches your "Optimization Purist" philosophy

1.  **No New Syntax**: You don't need `<>`, `where`, `impl`, `trait bounds` syntax complexity. You just use `func`, `if`, `return`. It simplifies the language parser and specification.
2.  **Ultimate Control**: You can write `if` statements *inside* the type generation.
    ```lain
    func Vector(comptime Size int) type {
        if Size <= 0 { panic("Size must be > 0") }
        // ...
    }
    ```
3.  **Monomorphization is Explicit**: `Option(int)` calls the function. `Option(float)` calls it again. It's obvious that code is being generated (zero runtime cost, linear compile-time cost).

## Implementation Requirements (High Difficulty)

To support this, the Lain compiler needs a major architectural upgrade: **CTFE (Compile Time Function Execution)**.

1.  **Interpreter**: The compiler must include an interpreter to execute Lain code *during compilation*.
2.  **Type as Value**: The Type System must handle `type` as a first-class value that can be passed to functions.
3.  **Memoization**: The compiler must cache calls: `Option(int)` called twice must return the *same* Type Identity.

## Comparison vs Generics

| Feature | Standard Generics (`<T>`) | Comptime Metaprogramming |
| :--- | :--- | :--- |
| **Syntax** | Special (`class Foo<T>`) | Standard (`func Foo(T)`) |
| **Complexity** | High (Constraints, Variance) | Medium (Interpreter required) |
| **Power** | Constrained by grammar | Turing Complete (Arbitrary logic) |
| **Debuggability** | "Template Soup" errors | Standard compile errors |

## Conclusion
For a "Zero Cost" language that values simplicity, Comptime Metaprogramming is the superior choice over Templates. It unifies the language: "Everything is just code".

**Recommendation**: Keep using Concrete Types (manual `OptionFile`) for now. When the pain of duplication becomes too high, implement a minimal CTFE system to allow `func Option(T) type`.
