# Appendix D — Evolution Roadmap

## D.1 Feature Maturity Levels

| Level | Meaning |
|:------|:--------|
| **Stable** | Fully implemented, tested, and specified. Unlikely to change. |
| **Implemented** | Working in the compiler but may evolve. |
| **Planned** | Designed and specified but not yet implemented. |
| **Future** | Under consideration; design not finalized. |

## D.2 Current Status (Stable / Implemented)

### Core Language
- Lexer and parser (all token types, all statement forms)
- Type system (primitives, structs, enums, ADTs, arrays, slices, pointers)
- Variable declarations with type inference
- Control flow (`if`/`elif`/`else`, `for`, `while`, `break`, `continue`)
- Pattern matching (`case`) with exhaustiveness checking
- `defer` statement
- `unsafe` blocks

### Functions
- `func` / `proc` distinction with purity enforcement
- Parameter modes (shared, mutable, owned, destructured)
- Return types with ownership annotations
- UFCS (Universal Function Call Syntax)
- Forward declarations (multi-pass resolution)

### Ownership & Borrowing
- Three ownership modes (shared, mutable, owned)
- Move semantics with use-after-move detection
- Borrow checker (read-write lock invariant)
- Non-Lexical Lifetimes (NLL)
- Linear types with scope-exit checking
- Branch consistency for linear values
- Per-field borrowing
- Non-consuming match (`case &expr`)

### Static Verification
- Value Range Analysis (VRA)
- Parameter and return type constraints
- Static array bounds checking
- Division by zero detection
- Relational constraints between parameters

### Generics
- `comptime` type parameters
- Anonymous type expressions (`type { ... }`)
- Type aliases with CTFE evaluation
- Monomorphization
- `compileError` intrinsic

### Modules & Interop
- Module system (`import`) with deduplication
- C interop (`c_include`, `extern func/proc/type`)
- Standard library (std.c, std.io, std.fs, std.math, std.option, std.result)

### Diagnostics
- 15 error codes ([E001]–[E015])
- Source-line error display with caret

## D.3 Phase 1 — Near-Term (Planned)

### Two-Phase Borrows
Enable `x.method(x.field)` without false-positive borrow errors.
- **Impact**: High — eliminates the most common borrow checker false positive.
- **Complexity**: Medium (~50 lines).
- **Reference**: §7.5

### Compile-Time Integer Parameters
```lain
func Array(comptime T type, comptime N int > 0) type { ... }
```
- **Impact**: Medium — enables fixed-size generic containers.
- **Complexity**: Medium — extend CTFE engine with integer evaluation.
- **Reference**: §9.9.1

### `char` Type for C Interop
Resolve the `u8` vs `char` mismatch that blocks `std.string`.
- **Impact**: High — unblocks string interop with C libraries.
- **Complexity**: Low-Medium — add `char` primitive mapping to C's `char`.
- **Reference**: §11.8.3

## D.4 Phase 2 — Medium-Term (Planned)

### `export` Visibility
```lain
export func public_api() int { ... }
```
- **Impact**: Medium — enables proper encapsulation.
- **Reference**: §10.5.2

### Error Propagation (`?` Operator)
```lain
var value = try_operation()?   // propagate Err upward
```
- **Impact**: High — dramatically reduces error handling boilerplate.
- **Reference**: §14.8

### Inclusive Range (`..=`)
```lain
for i in 0..=10 { ... }       // 0 through 10 inclusive
```
- **Impact**: Low — convenience feature.
- **Reference**: §4.13

### Pre/Post Contracts
```lain
func sqrt(x int) int
    pre x >= 0
    post result >= 0
{
    // ...
}
```
- **Impact**: Medium — formal specification of function contracts.

## D.5 Phase 3 — Long-Term (Future)

### Trait / Interface System
A mechanism for ad-hoc polymorphism:
```lain
// Future — design not finalized
trait Comparable {
    func compare(self, other Self) int
}
```
- **Impact**: Very high — enables generic algorithms.
- **Complexity**: Very high — requires vtable or monomorphization strategy.

### Macro System
Compile-time code generation:
```lain
// Future — design not finalized
macro derive_debug(T type) { ... }
```

### Async / Concurrency
Structured concurrency without a runtime:
- Stackless coroutines (compile to state machines)
- Channel-based communication
- **Design constraint**: Must work without heap allocation.

### Dynamic Dispatch
Optional virtual dispatch for polymorphic containers:
```lain
// Future — design not finalized
var shapes []&Shape            // slice of trait references
```

### Package Manager
A `lain.toml`-based package manager for dependency management:
```toml
[package]
name = "myproject"
version = "0.1.0"

[dependencies]
json = "1.0"
```

## D.6 Non-Goals

The following are explicitly out of scope for Lain:

| Feature | Reason |
|:--------|:-------|
| Garbage collection | Incompatible with determinism and embedded targets |
| Exceptions | Hidden control flow, requires stack unwinding |
| Class hierarchies | Unnecessary complexity; UFCS + traits suffice |
| Implicit conversions | Source of bugs; explicitness is a core principle |
| Operator overloading | Hides computation cost |
| Multiple inheritance | Diamond problem; complexity |
| Reflection | Runtime cost; incompatible with zero-overhead goal |
| JIT compilation | Complexity; not needed for systems programming |

## D.7 Versioning Policy

The language specification follows semantic versioning:

- **Major version** (1.0, 2.0): Breaking changes to stable features.
- **Minor version** (1.1, 1.2): New features that don't break existing code.
- **Patch version** (1.0.1): Bug fixes and clarifications.

The current specification describes version **0.x** — the language is in
active development and all features may change.

---

*This appendix is informative and subject to change.*
