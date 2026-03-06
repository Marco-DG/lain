# Appendix C — Comparison with Other Languages

## C.1 Feature Matrix

| Feature | Lain | Rust | C | Zig | Go |
|:--------|:-----|:-----|:--|:----|:---|
| **Memory safety** | Compile-time (ownership) | Compile-time (ownership) | Manual | Runtime + comptime | GC |
| **Null safety** | No null (Option type) | No null (Option type) | Null everywhere | Optional type | Nil + interfaces |
| **Exceptions** | None (Result type) | None (Result type) | None (errno) | Error unions | panic/recover |
| **Generics** | Comptime functions | Trait-based generics | Macros/void* | Comptime functions | Type parameters |
| **Lifetime annotations** | None (implicit NLL) | Required (`'a`) | N/A | N/A | N/A |
| **Purity tracking** | `func` vs `proc` | None | None | None | None |
| **Pattern matching** | `case` with exhaustiveness | `match` with exhaustiveness | `switch` | `switch` | `switch` |
| **UFCS** | Yes | Method syntax | No | No | Method syntax |
| **Backend** | C99 | LLVM | Native | LLVM | Go compiler |
| **Build system** | Single file | Cargo | Make/CMake | Zig build | Go modules |
| **Runtime** | None | Minimal | None | Minimal | GC runtime |
| **Compile speed** | Fast (C backend) | Slow | Fast | Medium | Fast |

## C.2 Lain vs Rust

### Similarities
- Ownership-based memory safety
- Move semantics with explicit transfers
- No null pointers (Option type)
- Pattern matching with exhaustiveness checking
- No exceptions (Result type for errors)
- Zero-cost abstractions

### Differences

| Aspect | Lain | Rust |
|:-------|:-----|:-----|
| Lifetime annotations | Implicit (NLL only) | Explicit (`'a`, `'static`) |
| Borrow syntax | `var x` (call site) | `&mut x` |
| Move syntax | `mov x` (explicit) | Implicit (default) |
| Generics | Comptime functions | Trait-based + monomorphization |
| Purity | `func`/`proc` enforced | Convention only |
| Async | Not supported | `async`/`await` |
| Traits/Interfaces | Not supported | Trait system |
| Macros | Not supported | `macro_rules!` + proc macros |
| Backend | C99 | LLVM |
| Learning curve | Lower | Higher |

### Key Insight

Lain trades Rust's expressiveness (lifetime annotations, traits, async) for
simplicity (no annotations, no trait system, no async). The result is a
language that is easier to learn but less expressive for complex borrowing
patterns.

## C.3 Lain vs C

### Similarities
- Compiles to native code
- No garbage collector
- Explicit memory management
- No runtime overhead
- Systems-level programming

### Differences

| Aspect | Lain | C |
|:-------|:-----|:--|
| Memory safety | Compile-time guarantees | Manual (error-prone) |
| Type safety | Strong, no implicit conversions | Weak, implicit conversions |
| Ownership | Tracked by compiler | Programmer discipline |
| Bounds checking | Static (VRA) | None |
| Division by zero | Static detection | Undefined behavior |
| Pattern matching | Exhaustive `case` | `switch` (fall-through) |
| Null safety | Option type | Null everywhere |
| Strings | Slices with length | Null-terminated arrays |
| Generics | Comptime functions | Macros / `void*` |
| Header files | Module system | `#include` |

### Key Insight

Lain provides C-level performance with compile-time safety guarantees. The
C99 backend means Lain can interoperate seamlessly with existing C codebases
while preventing the most common classes of C bugs.

## C.4 Lain vs Zig

### Similarities
- Comptime-based generics
- No hidden control flow
- No hidden allocations
- C interoperability as a first-class concern
- Explicit error handling

### Differences

| Aspect | Lain | Zig |
|:-------|:-----|:----|
| Memory safety | Compile-time ownership | Runtime safety (debug) |
| Ownership system | Borrow checker | Manual |
| Backend | C99 | LLVM + self-hosted |
| Comptime | Type functions | Full comptime execution |
| Allocator model | C interop (malloc/free) | Custom allocators everywhere |
| Build system | External | Built-in |
| Error handling | Result type + pattern match | Error unions + `try` |

### Key Insight

Both Lain and Zig use comptime for generics. Zig's comptime is more powerful
(full expression evaluation), while Lain's ownership system provides stronger
compile-time safety guarantees.

## C.5 Lain vs Go

### Similarities
- Simple syntax
- Fast compilation
- Strong standard library
- No class hierarchies

### Differences

| Aspect | Lain | Go |
|:-------|:-----|:---|
| Memory management | Ownership (no GC) | Garbage collected |
| Runtime | None | GC + goroutine runtime |
| Generics | Comptime functions | Type parameters |
| Error handling | Result type | Multiple return values |
| Null safety | Option type | Nil (null) |
| Concurrency | Not supported | Goroutines + channels |
| Compilation target | C99 | Go binary |
| Performance | Predictable (no GC pauses) | GC pauses possible |

### Key Insight

Go trades complexity for simplicity through garbage collection. Lain achieves
similar simplicity goals through ownership, but without a runtime — making
Lain suitable for embedded and real-time applications where Go's GC is
problematic.

## C.6 Unique Features

Features that distinguish Lain from all compared languages:

| Feature | Description |
|:--------|:------------|
| `func`/`proc` distinction | Compiler-enforced purity with termination guarantees |
| Caller-site ownership annotations | `mov` and `var` visible at every call site |
| Value Range Analysis | Polynomial-time static verification (no SMT) |
| Static bounds checking | Array accesses verified at compile time |
| `in` keyword for index constraints | `func f(arr int[10], i int in arr)` |
| Return type constraints | `func abs(x int) int >= 0` |
| Non-consuming match | `case &expr { ... }` |

---

*This appendix is informative.*
