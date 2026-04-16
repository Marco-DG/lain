# Appendix B — Design Rationale

This appendix explains the reasoning behind Lain's key design decisions.

## B.1 Why `func` / `proc`?

**Decision:** Separate pure functions (`func`) from side-effecting procedures
(`proc`) at the type level.

**Rationale:**
- **Termination guarantees**: `func` is guaranteed to terminate (no `while`,
  no recursion). This enables the compiler to reason statically about
  function completion.
- **Purity tracking**: The type system propagates purity constraints — a
  `func` cannot call a `proc`, ensuring that pure code stays pure.
- **Optimization potential**: The compiler can safely reorder, memoize, or
  eliminate calls to `func` because they have no side effects.
- **Documentation**: The declaration keyword immediately tells the reader
  whether a callable has side effects, without reading its body.

**Alternatives considered:**
- Single keyword with purity annotation (`func(pure)`) — rejected as
  less ergonomic and less visible.
- Haskell-style IO monad — rejected as too complex for a systems language.

## B.2 Why `mov` at Call Sites?

**Decision:** Require explicit `mov` annotation at call sites when
transferring ownership.

**Rationale:**
- **Readability**: `close_file(mov f)` makes it immediately clear that `f`
  is being consumed. Without it, the reader must consult `close_file`'s
  signature.
- **Intentionality**: Accidental ownership transfer is a common source of
  bugs. Requiring `mov` forces the programmer to acknowledge the transfer.
- **Symmetry**: The same annotation appears in declarations (`mov x T`) and
  at call sites (`mov x`), creating a consistent language.

**Alternatives considered:**
- Rust-style implicit moves — rejected because they're invisible at the
  call site.
- C++-style `std::move` — rejected as a library function, not a language
  construct.

## B.3 Why `var` for Mutability?

**Decision:** Use `var` to declare mutable bindings and annotate mutable
borrows.

**Rationale:**
- **Familiarity**: `var` is widely understood from JavaScript, Swift, Kotlin,
  and other languages.
- **Dual role**: `var` means "mutable" everywhere — in declarations
  (`var x = 42`), parameters (`f(var x T)`), and call sites (`f(var x)`).
- **Default immutability**: Omitting `var` means immutable/shared, encouraging
  safe-by-default programming.

## B.4 Why No Lifetime Annotations?

**Decision:** Borrows have implicit lifetimes determined by non-lexical
lifetime (NLL) analysis. No explicit lifetime annotations (like Rust's `'a`)
are required.

**Rationale:**
- **Simplicity**: Lifetime annotations are one of Rust's steepest learning
  curves. Lain targets programmers who want safety without that complexity.
- **Sufficient expressiveness**: NLL + the borrow checker's scope-based
  analysis handles the vast majority of real-world borrow patterns.
- **Predictability**: Borrows expire at last use — a simple, intuitive rule.

**Trade-offs:**
- Some valid programs that Rust can express with explicit lifetimes are
  rejected by Lain's more conservative analysis.
- Higher-order borrowing patterns (returning borrows of borrows) are limited.

## B.5 Why C99 Backend?

**Decision:** Compile to C99 source code rather than LLVM IR, machine code,
or bytecode.

**Rationale:**
- **Portability**: C99 compilers exist for every platform, from x86 to
  microcontrollers. Lain inherits this portability for free.
- **Toolchain reuse**: Users get GCC/Clang's optimization passes, debuggers
  (GDB/LLDB), profilers, and sanitizers without building custom tooling.
- **Interoperability**: C interop is trivial because the output *is* C.
  No FFI marshaling, no ABI compatibility concerns.
- **Simplicity**: A C emitter is orders of magnitude simpler than an LLVM
  backend or a native code generator.
- **Auditability**: The generated C code is human-readable, allowing users
  to verify what their Lain code actually does.

**Trade-offs:**
- Compilation is two-phase (Lain → C → binary), adding latency.
- Some optimizations are impossible without direct control over the IR.
- The C type system constrains what Lain can express (e.g., no guaranteed
  tail calls).

## B.6 Why No Exceptions?

**Decision:** Lain has no exception mechanism. All error handling uses
return values.

**Rationale:**
- **Zero overhead**: No unwinding tables, no catch tables, no RTTI overhead.
- **Determinism**: Error handling follows the same control flow as normal
  code. There are no invisible jumps.
- **Embedded compatibility**: Stack unwinding is unavailable or unreliable
  on many embedded platforms.
- **Explicit error paths**: Every error is visible in the function signature
  (via `Result` return types), preventing silent error propagation.
- **Compatibility with `defer`**: The `defer` mechanism provides
  deterministic cleanup without exceptions.

## B.7 Why Explicit `unsafe`?

**Decision:** Pointer dereference and direct ADT field access require an
explicit `unsafe` block.

**Rationale:**
- **Auditability**: Safety-critical codebases can grep for `unsafe {` to
  find all potentially dangerous operations.
- **Minimal scope**: Unlike C (where everything is unsafe) or some languages
  (where `unsafe` unlocks many capabilities), Lain's `unsafe` is narrow —
  it only enables pointer dereference and direct ADT access.
- **Social contract**: The `unsafe` keyword communicates to code reviewers
  that extra scrutiny is needed.

## B.8 Why Value Range Analysis Instead of SMT?

**Decision:** Use interval-based Value Range Analysis (VRA) for static
verification instead of an SMT solver.

**Rationale:**
- **Decidability**: VRA always terminates in polynomial time. SMT solving
  is NP-hard in the general case and may time out.
- **Predictability**: Programmers can reason about what the compiler will
  accept by thinking about integer ranges. SMT solvers are opaque.
- **Simplicity**: The VRA implementation is ~600 lines. An SMT integration
  would be orders of magnitude more complex.
- **Zero dependencies**: No external solver dependency.

**Trade-offs:**
- VRA is incomplete — it may reject valid programs that an SMT solver could
  prove correct.
- Non-linear constraints (multiplication of two unknowns) produce
  conservative (wide) ranges.

## B.9 Why Comptime Generics Instead of `<T>` Syntax?

**Decision:** Use compile-time function evaluation with `comptime` parameters
instead of traditional generic syntax.

**Rationale:**
- **Unification**: Generics, type aliases, and compile-time assertions use
  the same mechanism. No separate generic syntax to learn.
- **Expressiveness**: Comptime functions can contain arbitrary logic
  (conditionals, type comparison, `compileError`), enabling type-level
  programming that traditional generics cannot express.
- **Simplicity**: No angle brackets, no trait bounds syntax, no where
  clauses. Just functions that take types and return types.
- **Precedent**: Inspired by Zig's comptime approach, which has proven
  ergonomic in practice.

## B.10 Why No Implicit Conversions?

**Decision:** No implicit type conversions between numeric types.

**Rationale:**
- **Precision preservation**: Implicit widening can silently change the
  semantics of arithmetic operations.
- **Bug prevention**: Implicit narrowing is a common source of truncation
  bugs in C.
- **Explicitness**: `x as i64` makes type conversions visible and auditable.
- **VRA compatibility**: Explicit casts allow the compiler to track value
  ranges precisely through type boundaries.

## B.11 Why UFCS?

**Decision:** Support Universal Function Call Syntax (`x.f()` ≡ `f(x)`).

**Rationale:**
- **No classes needed**: UFCS provides method-like syntax without class
  hierarchies, virtual dispatch, or inheritance.
- **Chainability**: Enables fluent APIs: `data.filter(pred).map(fn).reduce(init, op)`.
- **Discoverability**: IDE autocompletion works naturally — type a dot
  after a value to see available operations.
- **Simplicity**: Just syntactic sugar. No new concepts, no method tables,
  no `this` pointer. The first parameter is the receiver.

---

*This appendix is informative.*
